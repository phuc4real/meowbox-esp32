/*
 * MeowBox UI shell: horizontal tileview (Clock, Radio, PC, Cat, System), themed widgets, overlay cards,
 * page indicator, backlight (idle dim + night mode) and runtime theme switching.
 *
 * Apps register a builder per tile. On a theme change or a day/night switch every tile is cleaned and
 * rebuilt by its builder, so builders must (re)create all widgets and only keep widget pointers in
 * statics. Timers/tasks/web handlers are created once in the app's init, never in the builder.
 * All functions must be called with the LVGL lock held unless noted.
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "mb_fonts.h"
#include "theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Visible area (measured 2026-11-25, docs/pinmap.md "Visible area"): the glass shows the whole 296x240 panel except
 * ~3 px on the left and four rounded corners of radius ~60 px. UI_SAFE_X/Y are the usual content margins; anything near
 * the top or bottom edge must also clear the corners: use ui_inset_l()/ui_inset_r(). tools/ui_shots.py draws the
 * hidden area over screenshots. */
#define UI_W 296
#define UI_H 240
#define UI_SAFE_X 14
#define UI_SAFE_Y 10
#define UI_CORNER_R 60
#define UI_HIDE_L   3
#define UI_HEAD_Y 12   /* top of the header row */
#define UI_FOOT_Y 222  /* centre line of the page indicator */

typedef enum { UI_TILE_CLOCK, UI_TILE_RADIO, UI_TILE_PC, UI_TILE_CAT, UI_TILE_SYSTEM, UI_TILE_COUNT } ui_tile_t;

typedef enum { UI_F_SMALL, UI_F_BODY, UI_F_TITLE, UI_F_CLOCK } ui_font_t;
typedef enum {
    UI_C_TEXT, UI_C_DIM, UI_C_ACCENT, UI_C_ACCENT2, UI_C_OK, UI_C_WARN, UI_C_ALERT,
    UI_C_ON_ACCENT, UI_C_LINE, UI_C_BG, UI_C_SURFACE,
} ui_color_t;

typedef void (*ui_builder_t)(lv_obj_t *tile);

void ui_init(void);
void ui_register_builder(ui_tile_t t, ui_builder_t fn);
void ui_build_all(void);                 /* first build, after all builders are registered */

lv_obj_t *ui_tile(ui_tile_t t);
void ui_show_tile(ui_tile_t t, bool anim); /* also shows a tile left out by ui_tile_set_skip, until it is left */
/* Leaves a tile out of the swipe order and the page indicator (true) or puts it back. A tile on screen stays until
 * the user swipes away from it. */
void ui_tile_set_skip(ui_tile_t t, bool skip);
ui_tile_t ui_active_tile(void);
bool ui_lang_en(void);
const char *ui_tr(const char *vi, const char *en);

/* Theme */
const theme_def_t *ui_theme(void);
const theme_colors_t *ui_colors(void);   /* day or night palette of the current theme */
uint32_t ui_c(ui_color_t c);
lv_color_t ui_lc(ui_color_t c);
const lv_font_t *ui_f(ui_font_t f);
bool ui_fx(void);                        /* effects enabled (display.fx != "eco") */
/* Thread-safe: request a theme change (applied by the UI task). */
void ui_request_rebuild(void);

/* Themed widgets */
lv_obj_t *ui_label(lv_obj_t *parent, ui_font_t f, ui_color_t c, const char *txt);
/* Smallest left x / right margin (from UI_W) so a box spanning rows y0..y1 stays clear of the rounded corners and the
 * hidden left strip, with 2 px to spare. Never less than UI_SAFE_X + 8, the usual text margin. */
int ui_inset_l(int y0, int y1);
int ui_inset_r(int y0, int y1);
lv_obj_t *ui_button(lv_obj_t *parent, const char *txt, bool primary, int w, int h);
lv_obj_t *ui_button_label(lv_obj_t *btn);  /* the label inside a ui_button */
/* Single-line label at its width. ui_dot: text that doesn't fit is cut with "…" (the default). ui_scroll: it scrolls
 * (marquee: one loop, then an 8 s pause); only for what is playing now, since a scrolling label keeps redrawing. */
void ui_dot(lv_obj_t *label);
void ui_scroll(lv_obj_t *label);
/* lv_label_set_text() for text from outside: drops code points the label's font can't draw (emoji, symbols). */
void ui_set_text_clean(lv_obj_t *label, const char *txt);
lv_obj_t *ui_bar(lv_obj_t *parent, int w, int h, ui_color_t indicator);
lv_obj_t *ui_rule(lv_obj_t *parent, int x, int y, int w);
lv_obj_t *ui_header(lv_obj_t *tile, const char *left, const char *right); /* returns right label */
void ui_hud_corners(lv_obj_t *parent, int x, int y, int w, int h);

/* Overlay card on top of everything. icon may be NULL. duration_s 0 = until dismissed. */
typedef void (*ui_overlay_cb_t)(bool tapped, void *arg);
/* Returns the card's overlay object, or NULL if a sticky card is showing (then nothing is shown). */
lv_obj_t *ui_overlay_show(const char *icon, const char *title, const char *text, ui_color_t accent, int duration_s,
                          ui_overlay_cb_t on_dismiss, void *arg);
void ui_overlay_hide(void);
/* The card on screen stays until it is tapped, times out or is hidden: later ui_overlay_show() calls are refused. */
void ui_overlay_set_sticky(void);
/* Hide the card only if it is still this one (from ui_overlay_show). */
void ui_overlay_hide_if(lv_obj_t *ov);
bool ui_overlay_visible(void);

/* Backlight: call on any user activity (buttons). Thread-safe. */
void ui_wake(void);
void ui_force_bright(int seconds);
bool ui_is_night(void);

/* Screen sleep: after display.sleep_s seconds without input (0 = never) and with no overlay card, the backlight and
 * panel go off and LVGL stops drawing; Wi-Fi, audio and app timers keep running. Input, ui_wake() or a card wakes it
 * (within 100 ms); the touch that wakes it does not reach the UI. */
bool ui_asleep(void); /* thread-safe */
/* Called from the LVGL task every UI tick (100-500 ms) with the current state, e.g. to save power while asleep. */
typedef void (*ui_sleep_hook_t)(bool asleep);
void ui_set_sleep_hook(ui_sleep_hook_t hook);

#ifdef __cplusplus
}
#endif
