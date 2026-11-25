/* Virtual cat (P6). Simulation runs in real time (also while the speaker is off, caught up from the RTC/SNTP
 * clock at boot). State lives in /data/cat.json; name/fur/difficulty in settings group "cat". */
#include "apps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "board.h"
#include "net.h"
#include "settings.h"
#include "audio_sfx.h"
#include "cat_sprites.h"
#include "utf8.h"

static const char *TAG = "cat";
#define CAT_FILE "/data/cat.json"
#define SCALE 3

/* ---------------- simulation ---------------- */
typedef struct {
    double full, fun, clean, energy, health; /* 0..100 */
    time_t born, updated;
    int care_days;
    double day_acc;   /* hours accumulated in the current "care day" */
    double day_good;  /* hours of that day with good care */
    int poops;
    double poop_timer; /* hours until next poop */
    bool sleeping, lights_off, away;
} cat_t;

static cat_t s_cat;
static bool s_loaded;
static bool s_dirty;

static double diff_mult(void)
{
    char d[12];
    settings_get_str("cat", "diff", d, sizeof(d), "normal");
    return !strcmp(d, "easy") ? 0.6 : !strcmp(d, "hard") ? 1.5 : 1.0;
}

static double clampd(double v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

static cat_age_t stage(void)
{
    int d = s_cat.care_days;
    return d < 3 ? CAT_AGE_KITTEN : d < 10 ? CAT_AGE_JUNIOR : d < 40 ? CAT_AGE_ADULT : CAT_AGE_SENIOR;
}

static const char *stage_name(cat_age_t a, bool en)
{
    static const char *vi[] = {"mèo con", "mèo nhỡ", "mèo lớn", "mèo già"};
    static const char *e[] = {"kitten", "junior", "adult", "senior"};
    return en ? e[a] : vi[a];
}

static const char *stage_key(cat_age_t a)
{
    static const char *k[] = {"kitten", "junior", "adult", "senior"};
    return k[a];
}

typedef enum { MOOD_HAPPY, MOOD_SAD, MOOD_HUNGRY, MOOD_SICK, MOOD_SLEEP, MOOD_AWAY } mood_t;

static mood_t mood(void)
{
    if (s_cat.away) return MOOD_AWAY;
    if (s_cat.sleeping) return MOOD_SLEEP;
    if (s_cat.health < 40) return MOOD_SICK;
    if (s_cat.full < 25) return MOOD_HUNGRY;
    if (s_cat.fun < 25 || s_cat.clean < 25) return MOOD_SAD;
    return MOOD_HAPPY;
}

static const char *mood_key(mood_t m)
{
    static const char *k[] = {"happy", "sad", "hungry", "sick", "sleep", "away"};
    return k[m];
}

static void new_cat(void)
{
    memset(&s_cat, 0, sizeof(s_cat));
    s_cat.full = 80;
    s_cat.fun = 80;
    s_cat.clean = 100;
    s_cat.energy = 80;
    s_cat.health = 100;
    s_cat.born = s_cat.updated = net_time_valid() ? time(NULL) : 0;
    s_cat.poop_timer = 4;
    s_dirty = true;
}

/* Advance the simulation by `h` hours (h <= 1). */
static void step(double h)
{
    if (s_cat.away) return;
    double k = diff_mult();
    bool night = ui_is_night();
    /* sleep transitions */
    if (!s_cat.sleeping && (s_cat.energy < 12 || (night && s_cat.lights_off) || (night && s_cat.energy < 60))) s_cat.sleeping = true;
    if (s_cat.sleeping && s_cat.energy >= 95 && !night && !s_cat.lights_off) s_cat.sleeping = false;

    double sleepf = s_cat.sleeping ? 0.4 : 1.0; /* slower needs while sleeping */
    s_cat.full = clampd(s_cat.full - 6 * k * h * sleepf);
    s_cat.fun = clampd(s_cat.fun - 5 * k * h * sleepf);
    s_cat.clean = clampd(s_cat.clean - (3 + 4 * s_cat.poops) * k * h);
    s_cat.energy = clampd(s_cat.energy + (s_cat.sleeping ? 14 : -4 * k) * h);

    if (!s_cat.sleeping) {
        s_cat.poop_timer -= h;
        if (s_cat.poop_timer <= 0) {
            if (s_cat.poops < 3) s_cat.poops++;
            s_cat.poop_timer = 3 + (esp_random() % 300) / 100.0;
        }
    }
    bool bad = s_cat.full < 15 || s_cat.clean < 15 || s_cat.fun < 10;
    bool good = s_cat.full > 50 && s_cat.clean > 50 && s_cat.fun > 40;
    if (bad) s_cat.health = clampd(s_cat.health - 4 * k * h);
    else if (good) s_cat.health = clampd(s_cat.health + 2 * h);
    if (s_cat.health <= 0) {
        s_cat.away = true;
        ESP_LOGW(TAG, "the cat ran away");
    }
    /* care days: a day counts if it was good care for >= 60% of the time */
    s_cat.day_acc += h;
    if (good) s_cat.day_good += h;
    if (s_cat.day_acc >= 24) {
        if (s_cat.day_good >= 24 * 0.6) s_cat.care_days++;
        s_cat.day_acc = s_cat.day_good = 0;
    }
    s_dirty = true;
}

static void catch_up(void)
{
    if (!net_time_valid()) return;
    time_t now = time(NULL);
    if (!s_cat.updated) {
        s_cat.updated = now;
        if (!s_cat.born) s_cat.born = now;
        return;
    }
    double hours = difftime(now, s_cat.updated) / 3600.0;
    if (hours <= 0) {
        s_cat.updated = now;
        return;
    }
    if (hours > 72) hours = 72; /* forgiving: at most 3 days of neglect while switched off */
    while (hours > 0) {
        double h = hours > 1 ? 1 : hours;
        step(h);
        hours -= h;
    }
    s_cat.updated = now;
}

/* ---------------- persistence ---------------- */
static void save(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "full", s_cat.full);
    cJSON_AddNumberToObject(j, "fun", s_cat.fun);
    cJSON_AddNumberToObject(j, "clean", s_cat.clean);
    cJSON_AddNumberToObject(j, "energy", s_cat.energy);
    cJSON_AddNumberToObject(j, "health", s_cat.health);
    cJSON_AddNumberToObject(j, "born", (double)s_cat.born);
    cJSON_AddNumberToObject(j, "updated", (double)s_cat.updated);
    cJSON_AddNumberToObject(j, "care", s_cat.care_days);
    cJSON_AddNumberToObject(j, "dacc", s_cat.day_acc);
    cJSON_AddNumberToObject(j, "dgood", s_cat.day_good);
    cJSON_AddNumberToObject(j, "poops", s_cat.poops);
    cJSON_AddNumberToObject(j, "ptimer", s_cat.poop_timer);
    cJSON_AddBoolToObject(j, "sleep", s_cat.sleeping);
    cJSON_AddBoolToObject(j, "lights", s_cat.lights_off);
    cJSON_AddBoolToObject(j, "away", s_cat.away);
    char *t = cJSON_PrintUnformatted(j);
    settings_write_file(CAT_FILE, t, strlen(t));
    cJSON_free(t);
    cJSON_Delete(j);
    s_dirty = false;
}

static double jnum(cJSON *j, const char *k, double d)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(v) ? v->valuedouble : d;
}

static void load(void)
{
    char *t = settings_read_file(CAT_FILE, NULL);
    cJSON *j = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!j) {
        new_cat();
        return;
    }
    s_cat.full = jnum(j, "full", 80);
    s_cat.fun = jnum(j, "fun", 80);
    s_cat.clean = jnum(j, "clean", 100);
    s_cat.energy = jnum(j, "energy", 80);
    s_cat.health = jnum(j, "health", 100);
    s_cat.born = (time_t)jnum(j, "born", 0);
    s_cat.updated = (time_t)jnum(j, "updated", 0);
    s_cat.care_days = (int)jnum(j, "care", 0);
    s_cat.day_acc = jnum(j, "dacc", 0);
    s_cat.day_good = jnum(j, "dgood", 0);
    s_cat.poops = (int)jnum(j, "poops", 0);
    s_cat.poop_timer = jnum(j, "ptimer", 4);
    s_cat.sleeping = cJSON_IsTrue(cJSON_GetObjectItem(j, "sleep"));
    s_cat.lights_off = cJSON_IsTrue(cJSON_GetObjectItem(j, "lights"));
    s_cat.away = cJSON_IsTrue(cJSON_GetObjectItem(j, "away"));
    cJSON_Delete(j);
}

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
static lv_obj_t *s_title, *s_bar_lbl[4], *s_bars[4], *s_msg, *s_area, *s_stars, *s_dark;
static spr_t s_cat_spr, s_prop, s_poop[3], s_mini;
static cat_pose_t s_pose = CAT_POSE_SIT;
static uint32_t s_anim_until_ms;
static int s_action; /* 1 eat, 2 play, 3 bath, 4 pet */
static int s_cat_x = 0, s_walk_dir = 0;
static uint32_t s_pet_strokes;

static uint32_t now_ms(void) { return lv_tick_get(); }

/* The cat tile runs a 200 ms tick. Setting a style, size or the hidden flag invalidates even when nothing changes,
 * which redrew the play area with the scaled cat ~5 times a second while it sat still (~15 % of a core, 0.2.2
 * measurement). Everything below only touches LVGL when the value really changed; cat_build() resets the cache. */
static struct {
    int bar[4], bar_col[4], lbl_alert[4];
    int dark, scale, x;
} s_shown;

static void shown_reset(void)
{
    memset(&s_shown, 0xFF, sizeof(s_shown)); /* -1: nothing shown yet */
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (o && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != hidden) {
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void say(const char *vi, const char *en)
{
    const char *t = ui_tr(vi, en);
    if (s_msg && strcmp(lv_label_get_text(s_msg), t)) lv_label_set_text(s_msg, t);
}

static int cat_scale(void) { return stage() == CAT_AGE_KITTEN ? 4 : 3; }

const char *cat_mood_text(void)
{
    static const char *vi[] = {"vui vẻ", "buồn", "đói", "ốm", "đang ngủ", "đã bỏ đi"};
    static const char *en[] = {"happy", "sad", "hungry", "sick", "sleeping", "gone"};
    mood_t m = mood();
    return ui_lang_en() ? en[m] : vi[m];
}

static void apply_pose(void)
{
    static cat_pose_t last_pose = CAT_POSE_COUNT;
    static cat_age_t last_age = CAT_AGE_COUNT;
    static const uint16_t *last_pal;
    static bool last_flip;
    const uint16_t *pal = cur_palette();
    cat_age_t a = stage();
    bool flip = s_walk_dir < 0;
    if (!s_force_redraw && s_pose == last_pose && a == last_age && pal == last_pal && flip == last_flip) return;
    s_force_redraw = false;
    last_pose = s_pose;
    last_age = a;
    last_pal = pal;
    last_flip = flip;
    spr_set(&s_cat_spr, cat_sprite(a, s_pose), pal, flip);
    for (int i = 0; i < 3; i++) spr_set(&s_poop[i], cat_prop(CAT_PROP_POOP), pal, false);
}

static void update_mini(void)
{
    if (!s_mini.img) return;
    static cat_pose_t last = CAT_POSE_COUNT;
    static const uint16_t *last_pal;
    mood_t m = mood();
    cat_pose_t p = m == MOOD_SLEEP ? ((now_ms() / 1500) % 2 ? CAT_POSE_SLEEP1 : CAT_POSE_SLEEP2)
                 : m == MOOD_HUNGRY || m == MOOD_SAD ? CAT_POSE_SAD
                 : m == MOOD_SICK ? CAT_POSE_SICK
                 : (now_ms() % 5000) < 250 ? CAT_POSE_BLINK : CAT_POSE_SIT;
    const uint16_t *pal = cur_palette();
    if (p == last && pal == last_pal && !s_force_redraw) return;
    last = p;
    last_pal = pal;
    spr_set(&s_mini, cat_sprite(stage(), p), pal, false);
    if (m == MOOD_AWAY) lv_obj_add_flag(s_mini.img, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_mini.img, LV_OBJ_FLAG_HIDDEN);
}

void cat_mini_attach(lv_obj_t *parent, int x, int y, int scale)
{
    spr_alloc(&s_mini, CAT_SPRITE_W, CAT_SPRITE_H);
    spr_place(&s_mini, parent, scale);
    lv_obj_set_pos(s_mini.img, x, y);
    s_force_redraw = true;
    update_mini();
}

static void show_prop(cat_prop_t p, bool on)
{
    if (!s_prop.img) return;
    if (!on) {
        set_hidden(s_prop.img, true);
        return;
    }
    static int last = -1;
    if (last != (int)p || lv_obj_has_flag(s_prop.img, LV_OBJ_FLAG_HIDDEN)) {
        last = p;
        spr_set(&s_prop, cat_prop(p), cur_palette(), false);
    }
    set_hidden(s_prop.img, false);
}

static void start_action(int a)
{
    if (s_cat.away) {
        say("Mèo đã bỏ đi. Vào web để nhận mèo mới.", "The cat left. Adopt a new one on the web page.");
        return;
    }
    if (s_cat.sleeping && a != 5) {
        say("Suỵt, mèo đang ngủ.", "Shh, the cat is asleep.");
        return;
    }
    s_action = a;
    s_walk_dir = 0;
    switch (a) {
    case 1:
        if (s_cat.full > 92) {
            say("No rồi, không ăn nữa đâu!", "Too full to eat!");
            s_action = 0;
            return;
        }
        s_cat.full = clampd(s_cat.full + 35);
        s_cat.health = clampd(s_cat.health + 2);
        s_anim_until_ms = now_ms() + 3000;
        say("Măm măm…", "Nom nom…");
        audio_sfx_play(SFX_MEOW);
        break;
    case 2:
        if (s_cat.energy < 15) {
            say("Mệt quá, muốn ngủ…", "Too tired to play…");
            s_action = 0;
            return;
        }
        s_cat.fun = clampd(s_cat.fun + 25);
        s_cat.energy = clampd(s_cat.energy - 10);
        s_anim_until_ms = now_ms() + 3500;
        say("Vờn len nè!", "Yarn time!");
        break;
    case 3:
        s_cat.clean = 100;
        s_cat.poops = 0;
        s_cat.fun = clampd(s_cat.fun - 5);
        s_anim_until_ms = now_ms() + 3000;
        say("Tắm sạch thơm tho", "All clean!");
        break;
    case 4:
        s_cat.fun = clampd(s_cat.fun + 3);
        s_anim_until_ms = now_ms() + 1500;
        if (++s_pet_strokes % 4 == 1) audio_sfx_play(SFX_PURR);
        say("Rừ rừ…", "Purr…");
        break;
    }
    s_dirty = true;
}

static void lights_label(lv_obj_t *btn)
{
    lv_label_set_text(ui_button_label(btn), s_cat.lights_off ? ui_tr("Bật đèn", "Lights on") : ui_tr("Tắt đèn", "Lights off"));
}

static void act_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (a == 5) {
        if (s_cat.away) return;
        s_cat.lights_off = !s_cat.lights_off;
        s_cat.sleeping = s_cat.lights_off;
        say(s_cat.lights_off ? "Chúc ngủ ngon." : "Dậy thôi!", s_cat.lights_off ? "Good night." : "Wake up!");
        lights_label(lv_event_get_target(e));
        s_dirty = true;
        return;
    }
    start_action(a);
}

static void pet_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t v;
    lv_indev_get_vect(in, &v);
    if (abs(v.x) + abs(v.y) > 2 && !s_cat.sleeping && !s_cat.away && now_ms() > s_anim_until_ms) start_action(4);
}

static void tap_cat_cb(lv_event_t *e)
{
    if (s_cat.sleeping || s_cat.away) return;
    audio_sfx_play(SFX_MEOW);
    say("Meo?", "Meow?");
}

static void update_bars(void)
{
    double v[4] = {s_cat.full, s_cat.fun, s_cat.clean, s_cat.energy};
    for (int i = 0; i < 4; i++) {
        if (!s_bars[i]) continue;
        int val = (int)v[i];
        if (val != s_shown.bar[i]) {
            lv_bar_set_value(s_bars[i], val, LV_ANIM_OFF);
            s_shown.bar[i] = val;
        }
        ui_color_t c = v[i] < 20 ? UI_C_ALERT : v[i] < 45 ? UI_C_WARN : UI_C_OK;
        if (ui_theme()->btn == THEME_BTN_FILL) c = v[i] < 20 ? UI_C_ALERT : UI_C_TEXT;
        if ((int)c != s_shown.bar_col[i]) {
            lv_obj_set_style_bg_color(s_bars[i], ui_lc(c), LV_PART_INDICATOR);
            s_shown.bar_col[i] = c;
        }
        int alert = v[i] < 20;
        if (alert != s_shown.lbl_alert[i]) {
            lv_obj_set_style_text_color(s_bar_lbl[i], ui_lc(alert ? UI_C_ALERT : UI_C_TEXT), 0);
            s_shown.lbl_alert[i] = alert;
        }
    }
}

static void cat_tick(lv_timer_t *t)
{
    static uint32_t last_sim, last_save, last_nag;
    uint32_t ms = now_ms();
    if (!s_loaded && net_time_valid()) {
        catch_up();
        s_loaded = true;
    }
    if (ms - last_sim >= 60000) {
        last_sim = ms;
        if (net_time_valid()) {
            time_t now = time(NULL);
            double h = s_cat.updated ? difftime(now, s_cat.updated) / 3600.0 : 1.0 / 60;
            if (h > 0 && h < 2) step(h);
            s_cat.updated = now;
        }
    }
    if (s_dirty && ms - last_save >= 300000) {
        last_save = ms;
        save();
    }
    mood_t m = mood();
    if ((m == MOOD_HUNGRY || m == MOOD_SICK) && !ui_is_night() && ms - last_nag > 1800000) {
        last_nag = ms;
        audio_sfx_play(SFX_MEOW);
    }
    if (ui_active_tile() == UI_TILE_CLOCK) update_mini();
    if (ui_active_tile() != UI_TILE_CAT || !s_cat_spr.img) return;

    char name[72]; /* the web allows 16 characters: up to 64 bytes of Vietnamese */
    settings_get_str("cat", "name", name, sizeof(name), "Mướp");
    char title[112];
    snprintf(title, sizeof(title), "%s, %s", name, stage_name(stage(), ui_lang_en()));
    utf8_trim(title);
    if (strcmp(lv_label_get_text(s_title), title)) lv_label_set_text(s_title, title);
    update_bars();

    /* lights off: the play area turns dark and shows a few stars */
    if ((int)s_cat.lights_off != s_shown.dark) {
        lv_obj_set_style_bg_opa(s_dark, s_cat.lights_off ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        s_shown.dark = s_cat.lights_off;
    }
    set_hidden(s_stars, !s_cat.lights_off);
    for (int i = 0; i < 3; i++) set_hidden(s_poop[i].img, !(i < s_cat.poops && !s_cat.away));

    uint32_t phase = ms / 400;
    bool acting = s_action && ms < s_anim_until_ms;
    if (!acting && s_action) {
        s_action = 0;
        show_prop(0, false);
        save();
    }
    if (s_cat.away) {
        set_hidden(s_cat_spr.img, true);
        show_prop(CAT_PROP_PAW, true);
        say("Mèo đã bỏ đi rồi…", "The cat has left…");
        return;
    }
    set_hidden(s_cat_spr.img, false);
    if (acting) {
        switch (s_action) {
        case 1: s_pose = phase % 2 ? CAT_POSE_EAT1 : CAT_POSE_EAT2; show_prop(CAT_PROP_FISH, (phase / 2) % 2); break;
        case 2: s_pose = phase % 2 ? CAT_POSE_PLAY1 : CAT_POSE_PLAY2; show_prop(CAT_PROP_YARN, true); break;
        case 3: s_pose = CAT_POSE_BLINK; show_prop(CAT_PROP_BUBBLES, true); break;
        case 4: s_pose = CAT_POSE_HAPPY; show_prop(CAT_PROP_HEART, true); break;
        }
    } else if (s_cat.sleeping) {
        s_pose = (ms / 1200) % 2 ? CAT_POSE_SLEEP1 : CAT_POSE_SLEEP2;
        show_prop(CAT_PROP_ZZZ, (ms / 1200) % 2);
        s_walk_dir = 0;
    } else {
        static uint32_t next_change;
        static int idle_mode; /* 0 sit, 1 walk, 2 groom */
        if (ms > next_change) {
            int r = esp_random() % 10;
            idle_mode = r < 5 ? 0 : r < 8 ? 1 : 2;
            s_walk_dir = idle_mode == 1 ? (s_cat_x > 16 ? -1 : s_cat_x < -16 ? 1 : (esp_random() & 1 ? 1 : -1)) : 0;
            next_change = ms + 2500 + esp_random() % 4000;
        }
        if (m == MOOD_SICK) { s_pose = CAT_POSE_SICK; s_walk_dir = 0; }
        else if (m == MOOD_SAD || m == MOOD_HUNGRY) { s_pose = CAT_POSE_SAD; s_walk_dir = 0; }
        else if (idle_mode == 1 && s_walk_dir) {
            s_pose = phase % 2 ? CAT_POSE_WALK1 : CAT_POSE_WALK2;
            s_cat_x += s_walk_dir * 2;
            if (s_cat_x > 24 || s_cat_x < -24) s_walk_dir = -s_walk_dir;
        } else if (idle_mode == 2) s_pose = CAT_POSE_GROOM;
        else s_pose = (ms % 4000) < 200 ? CAT_POSE_BLINK : CAT_POSE_SIT;
        show_prop(0, false);
        if (m == MOOD_HUNGRY) say("Đói quá, cho ăn đi!", "Hungry, feed me!");
        else if (m == MOOD_SICK) say("Mèo bị ốm, chăm sóc kỹ nhé.", "The cat is sick, take care.");
        else if (s_cat.poops) say("Có gì đó cần dọn…", "Something needs cleaning…");
    }
    int sc = cat_scale();
    if (sc != s_shown.scale || s_cat_x != s_shown.x) {
        lv_image_set_scale(s_cat_spr.img, 256 * sc);
        lv_obj_set_size(s_cat_spr.img, CAT_SPRITE_W * sc, CAT_SPRITE_H * sc);
        lv_obj_set_pos(s_cat_spr.img, 86 - CAT_SPRITE_W * sc / 2 + s_cat_x, 132 - CAT_SPRITE_H * sc);
        s_shown.scale = sc;
        s_shown.x = s_cat_x;
    }
    apply_pose();
}

static void cat_build(lv_obj_t *t)
{
    /* not s_mini: it belongs to the clock tile, built (and attached via cat_mini_attach) before this one */
    s_cat_spr.img = s_prop.img = NULL;
    for (int i = 0; i < 3; i++) s_poop[i].img = NULL;
    shown_reset();
    spr_alloc(&s_cat_spr, CAT_SPRITE_W, CAT_SPRITE_H);
    spr_alloc(&s_prop, CAT_PROP_W, CAT_PROP_H);
    for (int i = 0; i < 3; i++) spr_alloc(&s_poop[i], CAT_PROP_W, CAT_PROP_H);

    if (ui_theme()->btn == THEME_BTN_RAW) {
        s_title = ui_header(t, ui_tr("mèo", "cat"), "");
    } else {
        s_title = ui_label(t, UI_F_SMALL, ui_theme()->btn == THEME_BTN_CUT ? UI_C_ACCENT2 : UI_C_TEXT, "");
        int lh = lv_font_get_line_height(ui_f(UI_F_SMALL)), x = ui_inset_l(UI_HEAD_Y, UI_HEAD_Y + lh);
        lv_obj_set_pos(s_title, x, UI_HEAD_Y);
        lv_obj_set_width(s_title, UI_W - x - ui_inset_r(UI_HEAD_Y, UI_HEAD_Y + lh)); /* a long cat name gets "…" */
        ui_dot(s_title);
    }

    /* dark backdrop for "lights off", drawn behind the cat */
    s_dark = lv_obj_create(t);
    lv_obj_remove_style_all(s_dark);
    lv_obj_set_pos(s_dark, UI_SAFE_X + 4, 36);
    lv_obj_set_size(s_dark, 152, 100);
    lv_obj_set_style_bg_color(s_dark, lv_color_hex(0x0A0C16), 0);
    lv_obj_set_style_bg_opa(s_dark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_dark, ui_theme()->btn == THEME_BTN_TEXT ? 12 : 0, 0);
    lv_obj_remove_flag(s_dark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_stars = ui_label(s_dark, UI_F_SMALL, UI_C_WARN, ". *        .\n     .      .     *");
    lv_obj_set_style_text_color(s_stars, lv_color_hex(0xF5E9A0), 0);
    lv_obj_set_pos(s_stars, 10, 8);
    lv_obj_add_flag(s_stars, LV_OBJ_FLAG_HIDDEN);

    /* play area (touch): left 2/3, floor rule at y=136 */
    s_area = lv_obj_create(t);
    lv_obj_remove_style_all(s_area);
    lv_obj_set_pos(s_area, UI_SAFE_X + 4, 36);
    lv_obj_set_size(s_area, 152, 100);
    lv_obj_add_flag(s_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_area, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_area, pet_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_area, tap_cat_cb, LV_EVENT_SHORT_CLICKED, NULL);
    ui_rule(t, UI_SAFE_X + 4, 136, 152);

    for (int i = 0; i < 3; i++) {
        spr_place(&s_poop[i], t, 2);
        lv_obj_set_pos(s_poop[i].img, UI_SAFE_X + 6 + i * 50, 104);
        lv_obj_add_flag(s_poop[i].img, LV_OBJ_FLAG_HIDDEN);
    }
    spr_place(&s_cat_spr, t, cat_scale());
    spr_place(&s_prop, t, 2);
    lv_obj_set_pos(s_prop.img, 128, 44);
    lv_obj_add_flag(s_prop.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_area);
    lv_obj_set_style_bg_opa(s_area, LV_OPA_TRANSP, 0);

    /* stats: label left, value right, bar under */
    static const char *lbl_vi[4] = {"No", "Vui", "Sạch", "Năng lượng"};
    static const char *lbl_en[4] = {"Full", "Fun", "Clean", "Energy"};
    for (int i = 0; i < 4; i++) {
        s_bar_lbl[i] = ui_label(t, UI_F_SMALL, UI_C_TEXT, ui_tr(lbl_vi[i], lbl_en[i]));
        lv_obj_set_pos(s_bar_lbl[i], 182, 28 + i * 28);
        s_bars[i] = ui_bar(t, 92, 4, UI_C_OK);
        lv_obj_set_pos(s_bars[i], 182, 50 + i * 28);
    }

    s_msg = ui_label(t, UI_F_SMALL, UI_C_ACCENT2, "");
    lv_obj_set_width(s_msg, UI_W - 2 * UI_SAFE_X - 16);
    ui_dot(s_msg);
    lv_obj_set_pos(s_msg, UI_SAFE_X + 8, 142);

    lv_obj_t *row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, UI_W - 2 * UI_SAFE_X - 12, 40);
    lv_obj_set_pos(row, UI_SAFE_X + 6, 166);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    static const char *acts_vi[] = {"Ăn", "Chơi", "Tắm"};
    static const char *acts_en[] = {"Feed", "Play", "Bath"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = ui_button(row, ui_tr(acts_vi[i], acts_en[i]), false, 56, 40);
        lv_obj_add_event_cb(b, act_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
    }
    lv_obj_t *lb = ui_button(row, "", true, 84, 40);
    lights_label(lb);
    lv_obj_add_event_cb(lb, act_cb, LV_EVENT_CLICKED, (void *)(intptr_t)5);
    s_force_redraw = true;
    apply_pose();
}

/* ---------------- web ---------------- */
static void cat_cfg_hook(cJSON *doc)
{
    cJSON *st = cJSON_AddObjectToObject(doc, "stats");
    double age = s_cat.born && net_time_valid() ? difftime(time(NULL), s_cat.born) / 86400.0 : 0;
    cJSON_AddNumberToObject(st, "age", (int)age);
    cJSON_AddStringToObject(st, "stage", stage_key(stage()));
    cJSON_AddNumberToObject(st, "full", (int)s_cat.full);
    cJSON_AddNumberToObject(st, "fun", (int)s_cat.fun);
    cJSON_AddNumberToObject(st, "clean", (int)s_cat.clean);
    cJSON_AddNumberToObject(st, "energy", (int)s_cat.energy);
    cJSON_AddNumberToObject(st, "health", (int)s_cat.health);
    cJSON_AddStringToObject(st, "mood", mood_key(mood()));
    cJSON_AddBoolToObject(st, "away", s_cat.away);
}

static esp_err_t h_reset(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return ESP_OK;
    cJSON_Delete(j);
    if (board_lvgl_lock(500)) {
        new_cat();
        save();
        s_cat_x = 0;
        s_force_redraw = true;
        board_lvgl_unlock();
    }
    cJSON *doc = settings_get("cat");
    cat_cfg_hook(doc);
    return web_send_json(req, doc);
}

static void on_cat_settings(const char *group, void *arg)
{
    if (strcmp(group, "cat") == 0) s_force_redraw = true; /* fur colour changed */
}

/* ---------------- init ---------------- */
void cat_app_init(void)
{
    load();
    ui_register_builder(UI_TILE_CAT, cat_build);
    lv_timer_create(cat_tick, 200, NULL);
    settings_on_change(on_cat_settings, NULL);
    web_set_config_hook("cat", cat_cfg_hook);
    web_register("/api/cat/reset", HTTP_POST, h_reset, WEB_AUTH_SESSION);
}
