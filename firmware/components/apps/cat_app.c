/* Meow Coding (cat tile): the cat codes along with the AI coding agents running on the PC. The PC agent sends
 * {"t":"coding","s":[...]} over /ws/pc (pc_app.c -> cat_coding_update); this tile shows the most important session
 * (waiting for you > working > error > done > idle) with the cat at a laptop, plus tokens, context and an estimated
 * cost, and lists the others. Name/fur/clap/auto-show/sounds live in settings group "cat". */
#include "apps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "board.h"
#include "net.h"
#include "settings.h"
#include "audio_sfx.h"
#include "cat_sprites.h"
#include "utf8.h"

static const char *TAG = "cat";
#define OLD_CAT_FILE "/data/cat.json" /* virtual pet save (before Meow Coding), deleted at boot */
#define SCALE 3
#define MAX_SESS     4
#define FLOOR_Y      126              /* the cat stands on this line */
#define STALE_US     (75 * 1000000LL) /* the agent repeats the list every 30 s; older than this = gone */

/* ---------------- sessions (written by the web server task, read by the UI) ---------------- */
typedef enum { ST_IDLE, ST_WORK, ST_WAIT, ST_DONE, ST_ERR } sess_st_t;

typedef struct {
    char id[12], ag[12], name[100], dir[48], br[48], act[100], model[24];
    sess_st_t st;
    int for_s, ctx; /* seconds in this state when received; context use in %, -1 unknown */
    double tok, usd;
    int64_t at_us; /* when received */
} sess_t;

static sess_t *s_sess; /* MAX_SESS, in PSRAM */
static int s_nsess;
static int64_t s_sess_us; /* last coding message */
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_gen; /* bumped on every update */
static volatile bool s_ev_wait, s_ev_done;
static char s_ev_id[12]; /* session that just started waiting */
static int s_lim[2] = {-1, -1};      /* plan usage in %: 5-hour and weekly window (-1 unknown, API key) */
static double s_lim_reset[2];        /* when they reset, epoch seconds */

static sess_st_t parse_st(const char *s)
{
    if (!s) return ST_IDLE;
    return !strcmp(s, "work")   ? ST_WORK
           : !strcmp(s, "wait") ? ST_WAIT
           : !strcmp(s, "done") ? ST_DONE
           : !strcmp(s, "err")  ? ST_ERR
                                : ST_IDLE;
}

static const char *st_key(sess_st_t s)
{
    static const char *k[] = {"idle", "work", "wait", "done", "err"};
    return k[s];
}

static void jstr(cJSON *o, const char *k, char *dst, size_t n)
{
    utf8_copy(dst, n, cJSON_GetStringValue(cJSON_GetObjectItem(o, k)));
}

static double jnum(cJSON *o, const char *k, double def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

void cat_coding_update(cJSON *msg)
{
    cJSON *arr = cJSON_GetObjectItem(msg, "s");
    if (!s_sess || !cJSON_IsArray(arr)) return;
    sess_t tmp[MAX_SESS];
    int n = 0;
    int64_t now = esp_timer_get_time();
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (n >= MAX_SESS) break;
        sess_t *s = &tmp[n++];
        memset(s, 0, sizeof(*s));
        jstr(o, "id", s->id, sizeof(s->id));
        jstr(o, "ag", s->ag, sizeof(s->ag));
        jstr(o, "name", s->name, sizeof(s->name));
        jstr(o, "dir", s->dir, sizeof(s->dir));
        jstr(o, "br", s->br, sizeof(s->br));
        jstr(o, "act", s->act, sizeof(s->act));
        jstr(o, "model", s->model, sizeof(s->model));
        s->st = parse_st(cJSON_GetStringValue(cJSON_GetObjectItem(o, "st")));
        s->for_s = (int)jnum(o, "for", 0);
        s->ctx = (int)jnum(o, "ctx", -1);
        s->tok = jnum(o, "tok", 0);
        s->usd = jnum(o, "usd", 0);
        s->at_us = now;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool fresh = now - s_sess_us < STALE_US;
    for (int i = 0; i < n; i++) {
        sess_st_t before = ST_IDLE;
        bool known = false;
        for (int k = 0; fresh && k < s_nsess; k++)
            if (!strcmp(s_sess[k].id, tmp[i].id)) {
                before = s_sess[k].st;
                known = true;
            }
        if (tmp[i].st == ST_WAIT && (!known || before != ST_WAIT)) {
            s_ev_wait = true;
            snprintf(s_ev_id, sizeof(s_ev_id), "%.11s", tmp[i].id);
        }
        if (tmp[i].st == ST_DONE && known && before == ST_WORK) s_ev_done = true;
    }
    memcpy(s_sess, tmp, n * sizeof(sess_t));
    s_nsess = n;
    cJSON *lim = cJSON_GetObjectItem(msg, "lim");
    s_lim[0] = (int)jnum(lim, "h5", -1);
    s_lim[1] = (int)jnum(lim, "d7", -1);
    s_lim_reset[0] = jnum(lim, "h5r", 0);
    s_lim_reset[1] = jnum(lim, "d7r", 0);
    s_sess_us = now;
    s_gen++;
    xSemaphoreGive(s_lock);
}

/* copy of the current list (empty when the PC is gone or stopped sending) */
static int sess_copy(sess_t *out)
{
    if (!s_sess) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = pc_online() && esp_timer_get_time() - s_sess_us < STALE_US ? s_nsess : 0;
    memcpy(out, s_sess, n * sizeof(sess_t));
    xSemaphoreGive(s_lock);
    return n;
}

static int sess_secs(const sess_t *s) { return s->for_s + (int)((esp_timer_get_time() - s->at_us) / 1000000); }

/* ---------------- sprites ---------------- */
typedef struct {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    uint32_t *buf;
    int w, h;
} spr_t;

static void spr_alloc(spr_t *s, int w, int h)
{
    if (s->buf) return;
    s->w = w;
    s->h = h;
    s->buf = heap_caps_calloc(w * h, 4, MALLOC_CAP_SPIRAM);
    s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s->dsc.header.w = w;
    s->dsc.header.h = h;
    s->dsc.header.stride = w * 4;
    s->dsc.data = (const uint8_t *)s->buf;
    s->dsc.data_size = w * h * 4;
}

/* (re)create the LVGL image for a sprite on a new parent (after a theme rebuild) */
static void spr_place(spr_t *s, lv_obj_t *parent, int scale)
{
    s->img = lv_image_create(parent);
    lv_image_set_src(s->img, &s->dsc);
    lv_image_set_scale(s->img, 256 * scale);
    lv_image_set_antialias(s->img, false);
    lv_obj_set_size(s->img, s->w * scale, s->h * scale);
    lv_image_set_inner_align(s->img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(s->img, LV_OBJ_FLAG_CLICKABLE);
}

static void spr_set(spr_t *s, const uint8_t *idx, const uint16_t *pal, bool flip)
{
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            int sx = flip ? s->w - 1 - x : x;
            uint8_t b = idx[(y * s->w + sx) / 2];
            uint8_t i = (sx & 1) ? (b & 0x0F) : (b >> 4);
            uint32_t argb = 0;
            if (i) {
                uint16_t c = pal[i];
                uint32_t r = ((c >> 11) & 0x1F) * 255 / 31, g = ((c >> 5) & 0x3F) * 255 / 63, bl = (c & 0x1F) * 255 / 31;
                argb = 0xFF000000u | (r << 16) | (g << 8) | bl;
            }
            s->buf[y * s->w + x] = argb;
        }
    }
    if (s->img) {
        lv_image_set_src(s->img, NULL);
        lv_image_set_src(s->img, &s->dsc);
    }
}

static bool s_force_redraw;

static const uint16_t *day_palette(void)
{
    const char *tp = ui_theme()->cat_palette;
    const uint16_t *p = (tp && strcmp(tp, "fur") != 0) ? cat_theme_palette(tp) : NULL;
    if (p) return p;
    char fur[12];
    settings_get_str("cat", "fur", fur, sizeof(fur), "tabby");
    return cat_palette(cat_fur_from_name(fur));
}

/* Night mode dims the whole UI and drops blue light; the cat follows with a darker, warmer copy of its palette
 * (every fur and theme palette, no extra generated tables). */
static const uint16_t *cur_palette(void)
{
    static uint16_t night[16];
    static const uint16_t *night_of;
    const uint16_t *p = day_palette();
    if (!ui_is_night()) return p;
    if (night_of != p) {
        for (int i = 0; i < 16; i++) {
            uint16_t c = p[i];
            int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            r = r * 62 / 100; /* about 60 % brightness, blue cut harder than red */
            g = g * 55 / 100;
            b = b * 40 / 100;
            night[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
        night_of = p;
        s_force_redraw = true; /* same pointer as before (night[]) but new colours */
    }
    return night;
}

/* ---------------- UI ---------------- */
static lv_obj_t *s_count, *s_state, *s_time, *s_act, *s_name, *s_where;
static lv_obj_t *s_ctx, *s_tok, *s_lim5, *s_ctxbar;
static int s_act_lines; /* 2 with a session, 3 for the hint without one */
static spr_t s_cat_spr, s_prop, s_mini;
static cat_pose_t s_pose = CAT_POSE_SIT;
static volatile bool s_clap; /* set by the mic task, handled in cat_tick */
static uint32_t s_startle_until;
static int s_jump;             /* px the cat is lifted while startled */
static char s_focus[12];       /* session picked by a tap or a new "waiting" (else the first one) */
static uint32_t s_focus_until; /* the pick holds for 2 min */
static uint32_t s_shown_gen = UINT32_MAX;

void cat_hear_clap(void)
{
    if (settings_get_bool("cat", "clap", true)) s_clap = true;
}

static uint32_t now_ms(void) { return lv_tick_get(); }

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (o && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Setting text, a colour or the hidden flag invalidates even when nothing changes (the 0.2.2 cat tile spent ~15 %
 * of a core redrawing like that): everything below only touches LVGL on a real change. */
static void set_txt(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static char s_clean_last[8][104]; /* text last asked for, per label slot (user data), to skip the glyph filter */

static void set_txt_clean(lv_obj_t *l, const char *t)
{
    if (!l) return;
    uintptr_t slot = (uintptr_t)lv_obj_get_user_data(l);
    const size_t nslots = sizeof(s_clean_last) / sizeof(s_clean_last[0]);
    if (slot && slot < nslots && !strcmp(s_clean_last[slot], t)) return;
    if (slot && slot < nslots) snprintf(s_clean_last[slot], sizeof(s_clean_last[0]), "%s", t);
    ui_set_text_clean(l, t);
}

static void set_color(lv_obj_t *l, ui_color_t c)
{
    if (!l) return;
    lv_color_t want = ui_lc(c);
    if (!lv_color_eq(lv_obj_get_style_text_color(l, 0), want)) lv_obj_set_style_text_color(l, want, 0);
}

static void fmt_dur(int s, char *out, size_t n)
{
    if (s < 0) s = 0;
    if (s < 3600) snprintf(out, n, "%d:%02d", s / 60, s % 60);
    else snprintf(out, n, "%dh%02d", s / 3600, s / 60 % 60);
}

static void fmt_tok(double t, char *out, size_t n)
{
    if (t >= 1e9) snprintf(out, n, "%.1fB", t / 1e9);
    else if (t >= 1e6) snprintf(out, n, "%.1fM", t / 1e6);
    else if (t >= 1e3) snprintf(out, n, "%.1fK", t / 1e3);
    else snprintf(out, n, "%d", (int)t);
}

static const char *st_text(sess_st_t s)
{
    switch (s) {
    case ST_WORK: return ui_tr("Đang cào phím", "Pawing keys");
    case ST_WAIT: return ui_tr("Sen ơi!", "Hooman!");
    case ST_DONE: return ui_tr("Xong! Pate?", "Done! Treats?");
    case ST_ERR: return ui_tr("Toang rồi", "Hairball!");
    default: return ui_tr("Liếm lông", "Grooming");
    }
}

static ui_color_t st_color(sess_st_t s)
{
    return s == ST_WAIT || s == ST_ERR ? UI_C_ALERT : s == ST_WORK ? UI_C_ACCENT : s == ST_DONE ? UI_C_OK : UI_C_DIM;
}

/* The agent sends the kind of action ("read", "bash"...); the cat says it in a few words. */
static const char *act_text(const char *kind, sess_st_t st)
{
    static const struct { const char *k, *vi, *en; } A[] = {
        {"read", "Đang đọc file", "Reading a file"},     {"image", "Đọc ảnh nè!", "Ooh, a picture!"},
        {"edit", "Đang sửa code", "Fixing code"},         {"write", "Viết file mới", "Writing a new file"},
        {"bash", "Đang chạy bash", "Running bash"},       {"search", "Đánh hơi tìm code", "Sniffing for code"},
        {"web", "Đang lướt web", "Surfing the web"},      {"agent", "Gọi đồng bọn", "Calling the gang"},
        {"plan", "Đang lên kế hoạch", "Plotting a plan"}, {"ask", "Có câu hỏi cho sen", "Has a question"},
        {"skill", "Lục túi đồ nghề", "Digging in its kit"}, {"compact", "Dọn ổ trí nhớ", "Tidying its memory"},
        {"tool", "Đang vọc công cụ", "Fiddling with a tool"},
    };
    if (!kind[0]) return st == ST_WORK ? ui_tr("Đang ngẫm sự đời…", "Purr-cessing…") : "";
    for (size_t i = 0; i < sizeof(A) / sizeof(A[0]); i++)
        if (!strcmp(kind, A[i].k)) return ui_tr(A[i].vi, A[i].en);
    return ui_tr("Đang vọc công cụ", "Fiddling with a tool"); /* a kind from a newer agent */
}

static const char *agent_name(const char *ag) { return !strcmp(ag, "claude") ? "Claude Code" : ag[0] ? ag : "Agent"; }

static int pick_focus(const sess_t *ss, int n)
{
    if (s_focus[0] && now_ms() < s_focus_until)
        for (int i = 0; i < n; i++)
            if (!strcmp(ss[i].id, s_focus)) return i;
    s_focus[0] = 0;
    return 0;
}

const char *cat_mood_text(void)
{
    sess_t ss[MAX_SESS];
    int n = sess_copy(ss);
    sess_st_t st = n ? ss[0].st : ST_IDLE; /* the list comes sorted: most urgent first */
    bool en = ui_lang_en();
    if (!n) return en ? "napping" : "ngủ khò";
    switch (st) {
    case ST_WORK: return en ? "pawing keys" : "cào phím";
    case ST_WAIT: return en ? "calling you" : "gọi sen";
    case ST_DONE: return en ? "wants treats" : "đòi pate";
    case ST_ERR: return en ? "hairball" : "toang";
    default: return en ? "grooming" : "liếm lông";
    }
}

/* the pose for a state; t = ms clock for the animation */
static cat_pose_t pose_for(int n, const sess_t *f, uint32_t t, int type_ms, bool *prop_on, cat_prop_t *prop)
{
    *prop_on = false;
    if (!n || f->st == ST_IDLE) {
        if (!n) {
            *prop = CAT_PROP_ZZZ;
            *prop_on = (t / 1200) % 2;
            return (t / 1200) % 2 ? CAT_POSE_SLEEP1 : CAT_POSE_SLEEP2;
        }
        return (t % 4000) < 200 ? CAT_POSE_BLINK : CAT_POSE_SIT;
    }
    switch (f->st) {
    case ST_WORK:
        if (!f->act[0] || (t / 6000) % 4 == 3) { /* thinking (no tool running), and a short pause now and then */
            *prop = CAT_PROP_DOTS;
            *prop_on = true;
            return CAT_POSE_THINK;
        }
        return (t / type_ms) % 2 ? CAT_POSE_CODE1 : CAT_POSE_CODE2;
    case ST_WAIT:
        *prop = CAT_PROP_QUESTION;
        *prop_on = (t / 500) % 3 != 2;
        return CAT_POSE_WAIT;
    case ST_DONE:
        if (sess_secs(f) < 30) {
            *prop = CAT_PROP_SPARKLE;
            *prop_on = (t / 400) % 2;
            return CAT_POSE_HAPPY;
        }
        return (t % 4000) < 200 ? CAT_POSE_BLINK : CAT_POSE_SIT;
    case ST_ERR: return CAT_POSE_SAD;
    default: return CAT_POSE_SIT;
    }
}

static void show_prop(cat_prop_t p, bool on)
{
    if (!s_prop.img) return;
    static int last = -1;
    if (on && (last != (int)p || s_force_redraw)) {
        last = p;
        spr_set(&s_prop, cat_prop(p), cur_palette(), false);
    }
    set_hidden(s_prop.img, !on);
}

static void apply_pose(void)
{
    static cat_pose_t last_pose = CAT_POSE_COUNT;
    static const uint16_t *last_pal;
    const uint16_t *pal = cur_palette();
    if (!s_force_redraw && s_pose == last_pose && pal == last_pal) return;
    s_force_redraw = false;
    last_pose = s_pose;
    last_pal = pal;
    spr_set(&s_cat_spr, cat_sprite(CAT_AGE_ADULT, s_pose), pal, false);
}

static void update_mini(void)
{
    if (!s_mini.img) return;
    static cat_pose_t last = CAT_POSE_COUNT;
    static const uint16_t *last_pal;
    sess_t ss[MAX_SESS];
    int n = sess_copy(ss);
    bool prop_on;
    cat_prop_t prop;
    cat_pose_t p = pose_for(n, n ? &ss[0] : NULL, now_ms(), 600, &prop_on, &prop);
    if (p == CAT_POSE_THINK) p = CAT_POSE_CODE1; /* no bubble on the clock: keep it at the keyboard */
    const uint16_t *pal = cur_palette();
    if (p == last && pal == last_pal && !s_force_redraw) return;
    last = p;
    last_pal = pal;
    spr_set(&s_mini, cat_sprite(CAT_AGE_ADULT, p), pal, false);
}

void cat_mini_attach(lv_obj_t *parent, int x, int y, int scale)
{
    spr_alloc(&s_mini, CAT_SPRITE_W, CAT_SPRITE_H);
    spr_place(&s_mini, parent, scale);
    lv_obj_set_pos(s_mini.img, x, y);
    s_force_redraw = true;
    update_mini();
}

static void tap_cat_cb(lv_event_t *e) { audio_sfx_play(SFX_MEOW); }

/* tap the details: next session */
static void tap_info_cb(lv_event_t *e)
{
    sess_t ss[MAX_SESS];
    int n = sess_copy(ss);
    if (n < 2) return;
    int i = pick_focus(ss, n);
    snprintf(s_focus, sizeof(s_focus), "%s", ss[(i + 1) % n].id);
    s_focus_until = now_ms() + 120000;
    s_shown_gen = UINT32_MAX;
}

static void act_lines(int lines)
{
    if (lines == s_act_lines) return;
    s_act_lines = lines;
    lv_obj_set_height(s_act, lines * lv_font_get_line_height(ui_f(UI_F_SMALL)));
}

/* One session in focus, laid out for the usual case of a single Claude Code window: a big state next to the cat,
 * what it is doing, the session title across the screen, then context, tokens and cost. With more sessions the
 * header shows "1/2" and a tap goes to the next one. */
static void update_text(const sess_t *ss, int n, int fi)
{
    char buf[160], d[16];
    bool on = pc_online();
    set_hidden(s_time, !n);
    set_hidden(s_where, !n);
    if (!n) {
        char nm[72]; /* the web allows 16 bytes */
        settings_get_str("cat", "name", nm, sizeof(nm), "Mướp");
        set_txt(s_count, "");
        set_txt_clean(s_state, nm);
        set_color(s_state, UI_C_TEXT);
        act_lines(3);
        set_color(s_act, UI_C_DIM);
        set_txt_clean(s_act, on ? ui_tr("Rảnh quá nên ngủ. Bật \"Meow Coding\" ở khay MeowBox Agent rồi mở Claude Code.",
                                        "Bored, so napping. Turn on \"Meow Coding\" in the MeowBox Agent tray, then start Claude Code.")
                                : ui_tr("Chưa thấy máy tính đâu. Mở MeowBox Agent để mèo code cùng sen.",
                                        "No computer around. Open MeowBox Agent and the cat codes along."));
        set_txt_clean(s_name, on ? ui_tr("Chưa ai giao việc", "Nobody to code for") : ui_tr("Máy tính đâu rồi?", "Where's the computer?"));
        set_hidden(s_ctx, true);
        set_hidden(s_tok, true);
        set_hidden(s_lim5, true);
        set_hidden(s_ctxbar, true);
        return;
    }
    const sess_t *f = &ss[fi];
    if (n > 1) snprintf(buf, sizeof(buf), "%d/%d", fi + 1, n);
    else snprintf(buf, sizeof(buf), "%s", f->model[0] ? f->model : agent_name(f->ag));
    set_txt(s_count, buf);
    set_txt_clean(s_state, st_text(f->st));
    set_color(s_state, st_color(f->st));
    fmt_dur(sess_secs(f), d, sizeof(d));
    set_txt(s_time, d);
    act_lines(2);
    set_color(s_act, UI_C_TEXT);
    set_txt_clean(s_act, act_text(f->act, f->st));
    set_txt_clean(s_name, f->name[0] ? f->name : f->dir[0] ? f->dir : f->id);
    snprintf(buf, sizeof(buf), f->br[0] ? "%s · %s" : "%s", f->dir, f->br);
    set_txt_clean(s_where, buf);

    bool ctx = f->ctx >= 0;
    set_hidden(s_ctx, !ctx);
    set_hidden(s_ctxbar, !ctx);
    set_hidden(s_tok, f->tok <= 0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int h5 = s_lim[0];
    xSemaphoreGive(s_lock);
    set_hidden(s_lim5, h5 < 0);
    if (ctx) {
        int v = f->ctx > 100 ? 100 : f->ctx;
        snprintf(buf, sizeof(buf), "Context %d%%", v);
        set_txt(s_ctx, buf);
        if (lv_bar_get_value(s_ctxbar) != v) lv_bar_set_value(s_ctxbar, v, LV_ANIM_OFF);
        lv_color_t c = ui_lc(v >= 80 ? UI_C_ALERT : v >= 60 ? UI_C_WARN : UI_C_ACCENT);
        if (!lv_color_eq(lv_obj_get_style_bg_color(s_ctxbar, LV_PART_INDICATOR), c))
            lv_obj_set_style_bg_color(s_ctxbar, c, LV_PART_INDICATOR);
    }
    fmt_tok(f->tok, d, sizeof(d));
    snprintf(buf, sizeof(buf), "%s tok", d);
    set_txt(s_tok, buf);
    if (h5 >= 0) { /* the plan's 5-hour window, shared by every session */
        snprintf(buf, sizeof(buf), "5h %d%%", h5);
        set_txt(s_lim5, buf);
        set_color(s_lim5, h5 >= 90 ? UI_C_ALERT : h5 >= 70 ? UI_C_WARN : UI_C_DIM);
    }
}

static void cat_tick(lv_timer_t *t)
{
    uint32_t ms = now_ms();
    if (s_ev_wait) { /* an agent needs you: show the tile (and wake the screen), chime */
        s_ev_wait = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_focus, sizeof(s_focus), "%s", s_ev_id);
        xSemaphoreGive(s_lock);
        s_focus_until = ms + 120000;
        s_shown_gen = UINT32_MAX;
        if (settings_get_bool("cat", "show", true)) {
            ui_wake();
            if (ui_active_tile() != UI_TILE_CAT) ui_show_tile(UI_TILE_CAT, true);
        }
        if (settings_get_bool("cat", "snd", true)) audio_sfx_play(SFX_CHIME);
    }
    if (s_ev_done) { /* a turn finished: a meow (not at night) */
        s_ev_done = false;
        if (settings_get_bool("cat", "snd", true) && !ui_is_night()) audio_sfx_play(SFX_MEOW);
    }
    if (ui_active_tile() == UI_TILE_CLOCK) update_mini();
    if (ui_active_tile() != UI_TILE_CAT || !s_cat_spr.img) {
        s_clap = false; /* only the cat on screen reacts, not later when the tile is opened */
        return;
    }

    sess_t ss[MAX_SESS];
    int n = sess_copy(ss);
    int fi = n ? pick_focus(ss, n) : 0;
    /* text: on a new message, and once a second for the timer */
    static uint32_t last_txt;
    if (s_gen != s_shown_gen || ms - last_txt >= 1000) {
        s_shown_gen = s_gen;
        last_txt = ms;
        update_text(ss, n, fi);
    }

    if (s_clap) {
        s_clap = false;
        s_startle_until = ms + 1600; /* fur up and a hop */
        if (!ui_is_night()) audio_sfx_play(SFX_MEOW);
    }
    bool prop_on;
    cat_prop_t prop = CAT_PROP_ZZZ;
    if (ms < s_startle_until) {
        s_pose = CAT_POSE_ANGRY;
        prop_on = false;
        uint32_t e = 1600 - (s_startle_until - ms); /* 0..1600 ms: one hop in the first 400 ms */
        s_jump = e < 400 ? (int)(e < 200 ? e / 10 : (400 - e) / 10) : 0;
    } else {
        s_pose = pose_for(n, n ? &ss[fi] : NULL, ms, 400, &prop_on, &prop);
        s_jump = 0;
    }
    show_prop(prop, prop_on);
    static int shown_jump = -1;
    if (s_jump != shown_jump) {
        lv_obj_set_y(s_cat_spr.img, FLOOR_Y - CAT_SPRITE_H * SCALE - s_jump);
        shown_jump = s_jump;
    }
    apply_pose();
}

static lv_obj_t *small(lv_obj_t *p, ui_color_t c, int w)
{
    lv_obj_t *l = ui_label(p, UI_F_SMALL, c, "");
    lv_obj_set_width(l, w);
    ui_dot(l);
    return l;
}

static void cat_build(lv_obj_t *t)
{
    /* not s_mini: it belongs to the clock tile, built (and attached via cat_mini_attach) before this one */
    s_cat_spr.img = s_prop.img = NULL;
    s_shown_gen = UINT32_MAX;
    s_act_lines = 0;
    memset(s_clean_last, 0, sizeof(s_clean_last)); /* new labels */
    spr_alloc(&s_cat_spr, CAT_SPRITE_W, CAT_SPRITE_H);
    spr_alloc(&s_prop, CAT_PROP_W, CAT_PROP_H);
    s_count = ui_header(t, "Meow Coding", "");
    const int cx = UI_SAFE_X + 2, bw = UI_W - 2 * UI_SAFE_X - 4;
    const int lh_s = lv_font_get_line_height(ui_f(UI_F_SMALL)), lh_b = lv_font_get_line_height(ui_f(UI_F_BODY));

    /* left: the cat at its laptop on the floor rule; tap = meow */
    const int cw = CAT_SPRITE_W * SCALE + 4;
    ui_rule(t, cx, FLOOR_Y, cw);
    spr_place(&s_cat_spr, t, SCALE);
    lv_obj_set_pos(s_cat_spr.img, cx + 2, FLOOR_Y - CAT_SPRITE_H * SCALE);
    spr_place(&s_prop, t, 2);
    lv_obj_set_pos(s_prop.img, cx + CAT_SPRITE_W * SCALE - 26, 24);
    lv_obj_add_flag(s_prop.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *area = lv_obj_create(t);
    lv_obj_remove_style_all(area);
    lv_obj_set_pos(area, cx, 30);
    lv_obj_set_size(area, cw, FLOOR_Y - 30);
    lv_obj_add_flag(area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(area, tap_cat_cb, LV_EVENT_SHORT_CLICKED, NULL);

    /* right of the cat: state (big), time in it, what it is doing */
    const int ix = cx + cw + 10, iw = UI_W - UI_SAFE_X - 2 - ix;
    lv_obj_t *col = lv_obj_create(t);
    lv_obj_remove_style_all(col);
    lv_obj_set_pos(col, ix, 36);
    lv_obj_set_size(col, iw, FLOOR_Y - 36);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_state = ui_label(col, UI_F_TITLE, UI_C_ACCENT, "");
    lv_obj_set_width(s_state, iw);
    ui_dot(s_state);
    lv_obj_set_user_data(s_state, (void *)1);
    s_time = small(col, UI_C_DIM, iw);
    s_act = ui_label(col, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_width(s_act, iw);
    lv_label_set_long_mode(s_act, LV_LABEL_LONG_DOT); /* wraps, "..." after the last line */
    lv_obj_set_user_data(s_act, (void *)2);
    act_lines(2);

    /* across the screen: session title, folder and branch */
    int y = FLOOR_Y + 4;
    s_name = ui_label(t, UI_F_BODY, UI_C_TEXT, "");
    lv_obj_set_width(s_name, bw);
    ui_dot(s_name);
    lv_obj_set_pos(s_name, cx, y);
    lv_obj_set_user_data(s_name, (void *)3);
    y += lh_b;
    s_where = small(t, UI_C_DIM, bw);
    lv_obj_set_pos(s_where, cx, y);
    lv_obj_set_user_data(s_where, (void *)4);

    /* usage: context bar, then context / tokens / estimated cost */
    y += lh_s + 4;
    s_ctxbar = ui_bar(t, bw, 4, UI_C_ACCENT);
    lv_obj_set_pos(s_ctxbar, cx, y);
    y += 6;
    s_ctx = ui_label(t, UI_F_SMALL, UI_C_TEXT, "");
    lv_obj_set_pos(s_ctx, cx, y);
    s_tok = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_align(s_tok, LV_ALIGN_TOP_MID, 12, y);
    s_lim5 = ui_label(t, UI_F_SMALL, UI_C_DIM, "");
    lv_obj_align(s_lim5, LV_ALIGN_TOP_RIGHT, -(UI_SAFE_X + 2), y);

    /* tap next to the cat or below it: the next session (when there are several) */
    lv_obj_t *hit = lv_obj_create(t);
    lv_obj_remove_style_all(hit);
    lv_obj_set_pos(hit, ix, 30);
    lv_obj_set_size(hit, UI_W - ix, y + lh_s - 30);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, tap_info_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *hit2 = lv_obj_create(t);
    lv_obj_remove_style_all(hit2);
    lv_obj_set_pos(hit2, 0, FLOOR_Y + 2);
    lv_obj_set_size(hit2, ix, y + lh_s - FLOOR_Y - 2);
    lv_obj_add_flag(hit2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hit2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit2, tap_info_cb, LV_EVENT_SHORT_CLICKED, NULL);
    s_force_redraw = true;
    apply_pose();
}

/* ---------------- web ---------------- */
static void cat_cfg_hook(cJSON *doc)
{
    sess_t ss[MAX_SESS];
    int n = sess_copy(ss);
    cJSON *st = cJSON_AddObjectToObject(doc, "st");
    cJSON_AddBoolToObject(st, "pc", pc_online());
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int lim[2] = {n || pc_online() ? s_lim[0] : -1, n || pc_online() ? s_lim[1] : -1};
    double rst[2] = {s_lim_reset[0], s_lim_reset[1]};
    xSemaphoreGive(s_lock);
    if (lim[0] >= 0 || lim[1] >= 0) {
        cJSON *l = cJSON_AddObjectToObject(st, "lim");
        if (lim[0] >= 0) cJSON_AddNumberToObject(l, "h5", lim[0]);
        if (rst[0] > 0) cJSON_AddNumberToObject(l, "h5r", rst[0]);
        if (lim[1] >= 0) cJSON_AddNumberToObject(l, "d7", lim[1]);
        if (rst[1] > 0) cJSON_AddNumberToObject(l, "d7r", rst[1]);
    }
    cJSON *a = cJSON_AddArrayToObject(st, "s");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", ss[i].id);
        cJSON_AddStringToObject(o, "ag", ss[i].ag);
        cJSON_AddStringToObject(o, "name", ss[i].name);
        cJSON_AddStringToObject(o, "dir", ss[i].dir);
        cJSON_AddStringToObject(o, "br", ss[i].br);
        cJSON_AddStringToObject(o, "st", st_key(ss[i].st));
        cJSON_AddStringToObject(o, "act", ss[i].act);
        cJSON_AddNumberToObject(o, "for", sess_secs(&ss[i]));
        cJSON_AddNumberToObject(o, "tok", ss[i].tok);
        cJSON_AddNumberToObject(o, "usd", ss[i].usd);
        if (ss[i].ctx >= 0) cJSON_AddNumberToObject(o, "ctx", ss[i].ctx);
        cJSON_AddStringToObject(o, "model", ss[i].model);
        cJSON_AddItemToArray(a, o);
    }
}

static void on_cat_settings(const char *group, void *arg)
{
    if (strcmp(group, "cat") == 0) s_force_redraw = true; /* fur colour changed */
}

/* ---------------- init ---------------- */
void cat_app_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_sess = heap_caps_calloc(MAX_SESS, sizeof(sess_t), MALLOC_CAP_SPIRAM);
    if (remove(OLD_CAT_FILE) == 0) ESP_LOGI(TAG, "removed the old virtual pet save");
    ui_register_builder(UI_TILE_CAT, cat_build);
    lv_timer_create(cat_tick, 200, NULL);
    settings_on_change(on_cat_settings, NULL);
    web_set_config_hook("cat", cat_cfg_hook);
}
