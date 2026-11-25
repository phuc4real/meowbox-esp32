#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "settings.h"
#include "misc/lv_text_private.h" /* lv_text_encoded_next */

static const char *TAG = "ui";

static lv_obj_t *s_tv;
static lv_obj_t *s_tiles[UI_TILE_COUNT];
static ui_builder_t s_builders[UI_TILE_COUNT];
static lv_obj_t *s_ov;
static lv_timer_t *s_ov_timer;
static ui_overlay_cb_t s_ov_cb;
static bool s_ov_sticky; /* the current card refuses to be replaced (ui_overlay_set_sticky) */
static void *s_ov_arg;
static volatile bool s_wake_req, s_rebuild_req;
static volatile int64_t s_force_until;
static volatile bool s_asleep; /* screen sleep: backlight and panel off, LVGL drawing paused */
static bool s_built_night;     /* palette the tiles were last built with (set by ui_build_all, boot included) */
static lv_timer_t *s_ui_tmr;
static lv_obj_t *s_blocker; /* while asleep: eats the touch that wakes the screen */
static bool s_bl_hold;      /* just woke: backlight waits for the first fresh frame */
static ui_sleep_hook_t s_sleep_hook;
static bool s_lang_en, s_night, s_fx = true;
static int s_theme = 0;

/* ---------------- theme state ---------------- */
static int find_theme(const char *id)
{
    for (int i = 0; i < g_theme_count; i++)
        if (strcmp(g_themes[i].id, id) == 0) return i;
    for (int i = 0; i < g_theme_count; i++)
        if (strcmp(g_themes[i].id, g_theme_default) == 0) return i;
    return 0;
}

const theme_def_t *ui_theme(void) { return &g_themes[s_theme]; }
const theme_colors_t *ui_colors(void) { return s_night ? &g_themes[s_theme].night : &g_themes[s_theme].day; }
bool ui_fx(void) { return s_fx; }
bool ui_lang_en(void) { return s_lang_en; }
const char *ui_tr(const char *vi, const char *en) { return s_lang_en ? en : vi; }
bool ui_is_night(void) { return s_night; }
void ui_request_rebuild(void) { s_rebuild_req = true; }

uint32_t ui_c(ui_color_t c)
{
    const theme_colors_t *p = ui_colors();
    switch (c) {
    case UI_C_TEXT: return p->text;
    case UI_C_DIM: return p->dim;
    case UI_C_ACCENT: return p->accent;
    case UI_C_ACCENT2: return p->accent2;
    case UI_C_OK: return p->ok;
    case UI_C_WARN: return p->warn;
    case UI_C_ALERT: return p->alert;
    case UI_C_ON_ACCENT: return p->on_accent;
    case UI_C_LINE: return p->line;
    case UI_C_BG: return p->bg;
    case UI_C_SURFACE: return p->surface;
    }
    return p->text;
}

lv_color_t ui_lc(ui_color_t c) { return lv_color_hex(ui_c(c)); }

const lv_font_t *ui_f(ui_font_t f)
{
    const theme_def_t *t = ui_theme();
    switch (f) {
    case UI_F_SMALL: return t->f_small;
    case UI_F_BODY: return t->f_body;
    case UI_F_TITLE: return t->f_title;
    case UI_F_CLOCK: return t->f_clock;
    }
    return t->f_small;
}

/* ---------------- themed widgets ---------------- */
lv_obj_t *ui_label(lv_obj_t *parent, ui_font_t f, ui_color_t c, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, ui_f(f), 0);
    lv_obj_set_style_text_color(l, ui_lc(c), 0);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *txt, bool primary, int w, int h)
{
    const theme_def_t *t = ui_theme();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(b, UI_F_SMALL, UI_C_TEXT, txt);
    lv_obj_center(l);
    ui_color_t fg = UI_C_TEXT;
    switch (t->btn) {
    case THEME_BTN_FILL: /* Swiss: black block / outlined */
        lv_obj_set_style_radius(b, t->radius, 0);
        if (primary) {
            lv_obj_set_style_bg_color(b, ui_lc(UI_C_TEXT), 0);
            fg = UI_C_BG;
        } else {
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, ui_lc(UI_C_LINE), 0);
        }
        break;
    case THEME_BTN_CUT: /* Cyberpunk: flat block with an accent underline */
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_bg_color(b, ui_lc(primary ? UI_C_ACCENT2 : UI_C_SURFACE), 0);
        if (!primary) {
            lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, ui_lc(UI_C_ACCENT), 0);
        }
        fg = primary ? UI_C_ON_ACCENT : UI_C_ACCENT;
        break;
    case THEME_BTN_TEXT: /* Zen: pill outline for the main action, underlined text otherwise */
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        if (primary) {
            lv_obj_set_style_radius(b, h / 2, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, ui_lc(UI_C_TEXT), 0);
        } else {
            lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
        }
        break;
    case THEME_BTN_RAW: /* Brutalism: heavy border, hard shadow */
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, t->border, 0);
        lv_obj_set_style_border_color(b, ui_lc(UI_C_LINE), 0);
        lv_obj_set_style_bg_color(b, ui_lc(primary ? UI_C_ALERT : UI_C_SURFACE), 0);
        if (ui_fx() && !ui_is_night()) {
            lv_obj_set_style_shadow_width(b, 1, 0);
            lv_obj_set_style_shadow_spread(b, 0, 0);
            lv_obj_set_style_shadow_offset_x(b, 3, 0);
            lv_obj_set_style_shadow_offset_y(b, 3, 0);
            lv_obj_set_style_shadow_color(b, ui_lc(UI_C_LINE), 0);
            lv_obj_set_style_shadow_opa(b, LV_OPA_COVER, 0);
        }
        fg = primary ? UI_C_ON_ACCENT : UI_C_TEXT; /* night turns the alert fill yellow: grey text would vanish */
        break;
    }
    lv_obj_set_style_text_color(l, ui_lc(fg), 0);
    /* pressed feedback without animation */
    lv_obj_set_style_bg_opa(b, primary || t->btn == THEME_BTN_CUT || t->btn == THEME_BTN_RAW ? LV_OPA_70 : LV_OPA_20,
                            LV_STATE_PRESSED);
    if (t->btn == THEME_BTN_TEXT || (t->btn == THEME_BTN_FILL && !primary))
        lv_obj_set_style_bg_color(b, ui_lc(UI_C_TEXT), LV_STATE_PRESSED);
    return b;
}

lv_obj_t *ui_button_label(lv_obj_t *btn) { return lv_obj_get_child(btn, 0); }

/* Marquee: wait 2 s, scroll one full loop, then rest 8 s. A label that scrolls nonstop redraws ~30 times/s:
 * on the clock tile that cost 17-30 % of a CPU core (docs/perf/results-0.2.0.md). */
static lv_anim_t s_marquee;

static void one_line(lv_obj_t *l, lv_label_long_mode_t m)
{
    if (!s_marquee.repeat_delay) {
        lv_anim_init(&s_marquee);
        lv_anim_set_delay(&s_marquee, 2000);
        lv_anim_set_repeat_count(&s_marquee, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&s_marquee, 8000);
    }
    lv_obj_set_style_anim(l, &s_marquee, 0);
    lv_label_set_long_mode(l, m);
    const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
    lv_obj_set_height(l, lv_font_get_line_height(f));
}

/* Text conventions: what is playing now (radio / PC track title) may scroll; everything else is laid out to fit and,
 * if it still doesn't, is cut with "…" (a still label costs nothing to keep on screen). */
void ui_dot(lv_obj_t *label)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, 0)));
}
void ui_scroll(lv_obj_t *label) { one_line(label, LV_LABEL_LONG_SCROLL_CIRCULAR); }

void ui_set_text_clean(lv_obj_t *label, const char *txt)
{
    /* Text from outside (track titles, stream metadata) can hold emoji or symbols the theme fonts lack, which LVGL
     * draws as boxes: drop those code points, and the spaces they leave at the start. */
    const lv_font_t *f = lv_obj_get_style_text_font(label, 0);
    char out[160];
    size_t o = 0;
    uint32_t i = 0;
    while (txt && txt[i] && o + 5 < sizeof(out)) {
        uint32_t start = i;
        uint32_t cp = lv_text_encoded_next(txt, &i);
        if (cp == ' ' && o == 0) continue;
        lv_font_glyph_dsc_t g;
        if (cp < 0x20 || !lv_font_get_glyph_dsc(f, &g, cp, 0) || g.resolved_font == NULL) continue;
        memcpy(out + o, txt + start, i - start);
        o += i - start;
    }
    out[o] = 0;
    if (strcmp(lv_label_get_text(label), out) != 0) lv_label_set_text(label, out);
}

static void seg_draw(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(bar, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = ui_lc(UI_C_BG);
    d.width = 2;
    for (int x = a.x1 + 5; x < a.x2; x += 6) {
        d.p1.x = x; d.p1.y = a.y1;
        d.p2.x = x; d.p2.y = a.y2 + 1;
        lv_draw_line(layer, &d);
    }
}

lv_obj_t *ui_bar(lv_obj_t *parent, int w, int h, ui_color_t indicator)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 100);
    int r = ui_theme()->btn == THEME_BTN_TEXT ? h / 2 : 0;
    lv_obj_set_style_radius(b, r, 0);
    lv_obj_set_style_radius(b, r, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, ui_lc(UI_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_lc(indicator), LV_PART_INDICATOR);
    if (ui_theme()->btn == THEME_BTN_RAW) {
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, ui_lc(UI_C_LINE), 0);
    }
    if (ui_theme()->fx_segments && ui_fx()) lv_obj_add_event_cb(b, seg_draw, LV_EVENT_DRAW_POST, NULL);
    return b;
}

lv_obj_t *ui_rule(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, ui_theme()->btn == THEME_BTN_RAW ? 2 : 1);
    lv_obj_set_style_bg_color(r, ui_lc(UI_C_LINE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

void ui_hud_corners(lv_obj_t *parent, int x, int y, int w, int h)
{
    if (!ui_theme()->fx_hud || !ui_fx()) return;
    const int L = 6;
    static const struct { int sx, sy; lv_border_side_t side; } c[4] = {
        {0, 0, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_TOP}, {1, 0, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_TOP},
        {0, 1, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM}, {1, 1, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM}};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, L, L);
        lv_obj_set_pos(o, x + c[i].sx * (w - L), y + c[i].sy * (h - L));
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_side(o, c[i].side, 0);
        lv_obj_set_style_border_color(o, ui_lc(i == 3 ? UI_C_ACCENT2 : UI_C_ACCENT), 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ---------------- visible area ---------------- */
static int corner_cut(int y0, int y1)
{
    int d = y0 < UI_H - 1 - y1 ? y0 : UI_H - 1 - y1; /* rows to the nearer edge */
    if (d < 0) d = 0;
    if (d >= UI_CORNER_R) return 0;
    float dy = (float)(UI_CORNER_R - d);
    return (int)ceilf(UI_CORNER_R - sqrtf((float)(UI_CORNER_R * UI_CORNER_R) - dy * dy));
}

int ui_inset_l(int y0, int y1)
{
    int c = corner_cut(y0, y1);
    if (c < UI_HIDE_L) c = UI_HIDE_L;
    c += 2;
    return c > UI_SAFE_X + 8 ? c : UI_SAFE_X + 8;
}

int ui_inset_r(int y0, int y1)
{
    int c = corner_cut(y0, y1) + 2;
    return c > UI_SAFE_X + 8 ? c : UI_SAFE_X + 8;
}

lv_obj_t *ui_header(lv_obj_t *tile, const char *left, const char *right)
{
    const theme_def_t *t = ui_theme();
    lv_obj_t *l, *r;
    char buf[64];
    int lh = lv_font_get_line_height(ui_f(UI_F_SMALL));
    if (t->btn == THEME_BTN_RAW) { /* Brutalism: black path bar; text low in the bar, clear of the rounded corners */
        const int bar_h = 26, ty = bar_h - 3 - lh;
        lv_obj_t *bar = lv_obj_create(tile);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, UI_W, bar_h);
        lv_obj_set_style_bg_color(bar, ui_lc(UI_C_LINE), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        snprintf(buf, sizeof(buf), "~/%s", left);
        l = ui_label(bar, UI_F_SMALL, UI_C_BG, buf);
        lv_obj_set_pos(l, ui_inset_l(ty, ty + lh), ty);
        r = ui_label(bar, UI_F_SMALL, UI_C_BG, right);
        lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -ui_inset_r(ty, ty + lh), ty);
        return r;
    }
    if (t->btn == THEME_BTN_CUT) snprintf(buf, sizeof(buf), "// %s", left);
    else snprintf(buf, sizeof(buf), "%s", left);
    ui_color_t lc = t->btn == THEME_BTN_CUT ? UI_C_ACCENT2 : t->btn == THEME_BTN_FILL ? UI_C_TEXT : UI_C_DIM;
    l = ui_label(tile, UI_F_SMALL, lc, buf);
    lv_obj_set_pos(l, ui_inset_l(UI_HEAD_Y, UI_HEAD_Y + lh), UI_HEAD_Y);
    r = ui_label(tile, UI_F_SMALL, UI_C_DIM, right);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -ui_inset_r(UI_HEAD_Y, UI_HEAD_Y + lh), UI_HEAD_Y);
    return r;
}

/* ---------------- tile background effects ---------------- */
static void tile_bg_draw(lv_event_t *e)
{
    const theme_def_t *t = ui_theme();
    if (!ui_fx() || ui_is_night()) return;
    lv_obj_t *tile = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(tile, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = 1;
    if (t->fx_scanline) {
        d.color = lv_color_white();
        d.opa = 10;
        for (int y = a.y1; y <= a.y2; y += 3) {
            d.p1.x = a.x1; d.p1.y = y; d.p2.x = a.x2; d.p2.y = y;
            lv_draw_line(layer, &d);
        }
    }
    if (t->fx_grid) {
        d.color = lv_color_hex(0xD0D0D0);
        d.opa = LV_OPA_COVER;
        for (int y = a.y1; y <= a.y2; y += 16) {
            d.p1.x = a.x1; d.p1.y = y; d.p2.x = a.x2; d.p2.y = y;
            lv_draw_line(layer, &d);
        }
        for (int x = a.x1; x <= a.x2; x += 16) {
            d.p1.x = x; d.p1.y = a.y1; d.p2.x = x; d.p2.y = a.y2;
            lv_draw_line(layer, &d);
        }
    }
}

/* ---------------- page indicator ---------------- */
static void page_indicator(lv_obj_t *tile, int idx)
{
    const theme_def_t *t = ui_theme();
    if (t->btn == THEME_BTN_RAW) {
        char buf[32] = "";
        for (int i = 0; i < UI_TILE_COUNT; i++) {
            char p[6];
            snprintf(p, sizeof(p), i == idx ? "[%d] " : "%d ", i + 1);
            strcat(buf, p);
        }
        lv_obj_t *l = ui_label(tile, UI_F_SMALL, UI_C_TEXT, buf);
        lv_obj_set_style_bg_color(l, ui_lc(UI_C_BG), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, UI_FOOT_Y - lv_font_get_line_height(ui_f(UI_F_SMALL)) / 2);
        return;
    }
    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 80, 6);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, t->btn == THEME_BTN_TEXT ? 5 : 3, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, UI_FOOT_Y - 3); /* centred: the bottom corners cut the sides */
    for (int i = 0; i < UI_TILE_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(row);
        lv_obj_remove_style_all(d);
        bool on = i == idx;
        if (t->btn == THEME_BTN_TEXT) {
            lv_obj_set_size(d, on ? 5 : 3, on ? 5 : 3);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        } else {
            lv_obj_set_size(d, on ? (t->btn == THEME_BTN_CUT ? 16 : 14) : (t->btn == THEME_BTN_CUT ? 8 : 4), 2);
        }
        lv_obj_set_style_bg_color(d, on ? ui_lc(t->btn == THEME_BTN_CUT ? UI_C_ACCENT : UI_C_TEXT) : ui_lc(UI_C_LINE), 0);
        lv_obj_set_style_bg_opa(d, on ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

/* ---------------- tiles ---------------- */
lv_obj_t *ui_tile(ui_tile_t t) { return s_tiles[t]; }

void ui_show_tile(ui_tile_t t, bool anim) { lv_tileview_set_tile(s_tv, s_tiles[t], anim ? LV_ANIM_ON : LV_ANIM_OFF); }

ui_tile_t ui_active_tile(void)
{
    lv_obj_t *a = lv_tileview_get_tile_active(s_tv);
    for (int i = 0; i < UI_TILE_COUNT; i++)
        if (s_tiles[i] == a) return (ui_tile_t)i;
    return UI_TILE_CLOCK;
}

void ui_register_builder(ui_tile_t t, ui_builder_t fn) { s_builders[t] = fn; }

static void read_display_settings(void)
{
    char buf[16];
    settings_get_str("display", "theme", buf, sizeof(buf), g_theme_default);
    s_theme = find_theme(buf);
    settings_get_str("display", "fx", buf, sizeof(buf), "full");
    s_fx = strcmp(buf, "eco") != 0;
    settings_get_str("display", "lang", buf, sizeof(buf), "vi");
    s_lang_en = strcmp(buf, "en") == 0;
}

void ui_build_all(void)
{
    read_display_settings();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, ui_lc(UI_C_BG), 0);
    lv_obj_set_style_text_font(scr, ui_f(UI_F_SMALL), 0);
    lv_obj_set_style_text_color(scr, ui_lc(UI_C_TEXT), 0);
    for (int i = 0; i < UI_TILE_COUNT; i++) {
        lv_obj_t *t = s_tiles[i];
        lv_obj_clean(t);
        lv_obj_set_style_bg_color(t, ui_lc(UI_C_BG), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        if (s_builders[i]) s_builders[i](t);
        page_indicator(t, i);
    }
    s_built_night = s_night;
    ESP_LOGI(TAG, "built theme %s (%s%s)", ui_theme()->id, s_night ? "night" : "day", s_fx ? "" : ", eco");
}

/* ---------------- overlay ---------------- */
static void ov_close(bool tapped)
{
    if (!s_ov) return;
    if (s_ov_timer) {
        lv_timer_delete(s_ov_timer);
        s_ov_timer = NULL;
    }
    lv_obj_delete(s_ov);
    s_ov = NULL;
    s_ov_sticky = false;
    ui_overlay_cb_t cb = s_ov_cb;
    s_ov_cb = NULL;
    if (cb) cb(tapped, s_ov_arg);
}

static void ov_clicked(lv_event_t *e) { ov_close(true); }
static void ov_timeout(lv_timer_t *t) { s_ov_timer = NULL; ov_close(false); }
void ui_overlay_hide(void) { ov_close(false); }
bool ui_overlay_visible(void) { return s_ov != NULL; }
void ui_overlay_set_sticky(void) { s_ov_sticky = s_ov != NULL; }
void ui_overlay_hide_if(lv_obj_t *ov)
{
    if (ov && ov == s_ov) ov_close(false);
}

lv_obj_t *ui_overlay_show(const char *icon, const char *title, const char *text, ui_color_t accent, int duration_s,
                          ui_overlay_cb_t on_dismiss, void *arg)
{
    const theme_def_t *t = ui_theme();
    if (s_ov && s_ov_sticky) return NULL; /* a ringing alarm keeps its card (and its tap-to-snooze) */
    if (s_ov) {
        s_ov_cb = NULL;
        ov_close(false);
    }
    s_ov_cb = on_dismiss;
    s_ov_arg = arg;
    s_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ov);
    lv_obj_set_size(s_ov, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_ov, ui_lc(UI_C_BG), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_80, 0);
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ov, ov_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_ov);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, UI_W - 2 * UI_SAFE_X - 12, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, ui_lc(UI_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, t->btn == THEME_BTN_TEXT ? 12 : 0, 0);
    lv_obj_set_style_border_color(card, ui_lc(accent), 0);
    if (t->btn == THEME_BTN_RAW) lv_obj_set_style_border_width(card, 3, 0);
    else {
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(card, 4, 0);
    }
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    char head[96];
    snprintf(head, sizeof(head), "%s%s%s", icon ? icon : "", icon ? "  " : "", title && *title ? title : "");
    if (head[0]) {
        lv_obj_t *h = ui_label(card, UI_F_BODY, accent, head);
        lv_obj_set_width(h, lv_pct(100));
        ui_dot(h);
    }
    if (text && *text) {
        lv_obj_t *b = ui_label(card, UI_F_BODY, UI_C_TEXT, text);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_style_max_height(b, 130, 0);
    }
    if (duration_s > 0) {
        s_ov_timer = lv_timer_create(ov_timeout, duration_s * 1000, NULL);
        lv_timer_set_repeat_count(s_ov_timer, 1);
    }
    ui_wake();
    return s_ov;
}

/* ---------------- backlight, night, theme changes ---------------- */
void ui_wake(void) { s_wake_req = true; }
void ui_force_bright(int seconds)
{
    s_force_until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s_wake_req = true;
}

static bool compute_night(void)
{
    char mode[8];
    settings_get_str("display", "night_mode", mode, sizeof(mode), "auto");
    if (!strcmp(mode, "on")) return true;
    if (!strcmp(mode, "off")) return false;
    if (!settings_get_bool("display", "night", true)) return false;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return false;
    int from = settings_get_int("display", "n_from", 22), to = settings_get_int("display", "n_to", 7);
    int h = tm.tm_hour;
    return from <= to ? (h >= from && h < to) : (h >= from || h < to);
}

/* ---------------- screen sleep ---------------- */
bool ui_asleep(void) { return s_asleep; }
void ui_set_sleep_hook(ui_sleep_hook_t hook) { s_sleep_hook = hook; }

static void blocker_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && !s_asleep && s_blocker) {
        lv_obj_delete_async(s_blocker);
        s_blocker = NULL;
    }
}

static void apply_brightness(void);
static void backlight_on(lv_timer_t *t)
{
    s_bl_hold = false;
    apply_brightness();
}

static void set_asleep(bool asleep)
{
    lv_timer_t *refr = lv_display_get_refr_timer(board_display());
    s_asleep = asleep;
    if (asleep) {
        board_set_brightness(0);
        if (refr) lv_timer_pause(refr);
        board_lcd_on(false);
        if (!s_blocker) {
            s_blocker = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(s_blocker);
            lv_obj_set_size(s_blocker, LV_PCT(100), LV_PCT(100));
            lv_obj_add_flag(s_blocker, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_blocker, blocker_event, LV_EVENT_ALL, NULL);
        }
        lv_timer_set_period(s_ui_tmr, 100); /* react to a button or touch quickly */
        ESP_LOGI(TAG, "screen sleep");
    } else {
        board_lcd_on(true);
        if (refr) lv_timer_resume(refr);
        lv_obj_invalidate(lv_screen_active());
        if (s_blocker && !lv_obj_has_state(s_blocker, LV_STATE_PRESSED)) { /* else it goes on release */
            lv_obj_delete(s_blocker);
            s_blocker = NULL;
        }
        lv_timer_set_period(s_ui_tmr, 500);
        /* backlight once the first fresh frame is out, not over the old picture */
        s_bl_hold = true;
        lv_timer_t *t = lv_timer_create(backlight_on, 60, NULL);
        lv_timer_set_repeat_count(t, 1);
        ESP_LOGI(TAG, "screen wake");
    }
}

static void on_settings(const char *group, void *arg)
{
    if (strcmp(group, "display") == 0) s_rebuild_req = true; /* applied in the UI timer (LVGL context) */
}

static void ui_timer(lv_timer_t *tmr)
{
    if (s_wake_req) {
        s_wake_req = false;
        lv_display_trigger_activity(NULL);
    }
    bool night = compute_night();
    if (s_rebuild_req || night != s_night) {
        if (!s_ov) { /* never rebuild under an alarm/notification card */
            s_rebuild_req = false;
            s_night = night;
            int old = s_theme;
            bool old_fx = s_fx, old_en = s_lang_en;
            read_display_settings();
            if (old != s_theme || old_fx != s_fx || old_en != s_lang_en || s_built_night != s_night) {
                int64_t t0 = esp_timer_get_time();
                ui_build_all();
                ESP_LOGI(TAG, "rebuild took %lld ms", (esp_timer_get_time() - t0) / 1000);
            }
        }
    }
    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    int sleep_s = settings_get_int("display", "sleep_s", 300);
    bool want = sleep_s > 0 && idle_ms > (uint32_t)sleep_s * 1000 && !s_ov && esp_timer_get_time() >= s_force_until;
    if (want != s_asleep) set_asleep(want);
    if (s_sleep_hook) s_sleep_hook(s_asleep);
    apply_brightness();
}

static void apply_brightness(void)
{
    if (s_asleep || s_bl_hold) return;
    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    int idle_s = settings_get_int("display", "idle_s", 60);
    int bri = settings_get_int("display", "bri", 80);
    if (s_night) bri = settings_get_int("display", "n_bri", 5);
    else if (idle_s > 0 && idle_ms > (uint32_t)idle_s * 1000) bri = settings_get_int("display", "idle_bri", 20);
    if (s_night && idle_ms < 10000) bri = settings_get_int("display", "idle_bri", 20);
    if (esp_timer_get_time() < s_force_until) bri = settings_get_int("display", "bri", 80);
    if (bri < 1) bri = 1;
    if (bri != board_get_brightness()) board_set_brightness(bri);
}

/* ---------------- redraw stats (temporary, docs/perf: why taskLVGL uses 17-30 % CPU on some clock layouts) ---------------- */
static struct { uint32_t n, px; lv_area_t big, last[4]; int li; } s_inv;

static void inv_cb(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    if (!a) return;
    uint32_t px = lv_area_get_size(a);
    s_inv.n++;
    s_inv.px += px;
    if (px > lv_area_get_size(&s_inv.big)) s_inv.big = *a;
    s_inv.last[s_inv.li++ & 3] = *a;
}

static void inv_report(lv_timer_t *t)
{
    if (s_inv.n) {
        ESP_LOGI(TAG, "redraw 10s: %lu areas, %lu px, tile %d, big %d,%d-%d,%d, last %d,%d-%d,%d %d,%d-%d,%d",
                 (unsigned long)s_inv.n, (unsigned long)s_inv.px, (int)ui_active_tile(), s_inv.big.x1, s_inv.big.y1,
                 s_inv.big.x2, s_inv.big.y2, s_inv.last[0].x1, s_inv.last[0].y1, s_inv.last[0].x2, s_inv.last[0].y2,
                 s_inv.last[1].x1, s_inv.last[1].y1, s_inv.last[1].x2, s_inv.last[1].y2);
    }
    memset(&s_inv, 0, sizeof(s_inv));
}

/* ---------------- init ---------------- */
void ui_init(void)
{
    read_display_settings();
    s_night = compute_night();
    lv_obj_t *scr = lv_screen_active();
    s_tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < UI_TILE_COUNT; i++) {
        lv_dir_t dir = i == 0 ? LV_DIR_RIGHT : i == UI_TILE_COUNT - 1 ? LV_DIR_LEFT : LV_DIR_HOR;
        s_tiles[i] = lv_tileview_add_tile(s_tv, i, 0, dir);
        lv_obj_remove_flag(s_tiles[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_tiles[i], tile_bg_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    }
    settings_on_change(on_settings, NULL);
    s_ui_tmr = lv_timer_create(ui_timer, 500, NULL);
    lv_display_add_event_cb(lv_display_get_default(), inv_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_timer_create(inv_report, 10000, NULL);
    ESP_LOGI(TAG, "ui ready, theme %s", ui_theme()->id);
}
