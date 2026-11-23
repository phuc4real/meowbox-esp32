/*
 * MeowBox themes. Definitions are generated from firmware/themes/<id>.json by tools/gen_themes.py
 * into components/ui/themes_gen.c. Apps never hard-code colours: they use ui_theme() / ui_col().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bg, surface, text, dim, line, accent, accent2, on_accent, ok, warn, alert;
} theme_colors_t;

typedef enum { THEME_BTN_FILL, THEME_BTN_CUT, THEME_BTN_TEXT, THEME_BTN_RAW } theme_btn_t;
typedef enum { THEME_CLOCK_LEFT, THEME_CLOCK_CENTER, THEME_CLOCK_ENSO, THEME_CLOCK_BLOCK } theme_clock_t;

typedef struct {
    const char *id, *name_vi, *name_en, *desc_vi, *desc_en;
    theme_colors_t day, night;
    const lv_font_t *f_small, *f_body, *f_title, *f_clock;
    uint8_t radius, border;
    int8_t shadow_x, shadow_y;
    theme_btn_t btn;
    theme_clock_t clock;
    bool fx_aberration, fx_scanline, fx_grid, fx_hud, fx_enso, fx_tilt, fx_segments;
    const char *cat_palette; /* "fur" = the user's fur colour, otherwise a cat_sprites palette name */
    const char *web_fonts;   /* comma-separated woff2 files under /fonts/ used by the web UI */
} theme_def_t;

extern const theme_def_t g_themes[];
extern const int g_theme_count;
extern const char *const g_theme_default;

#ifdef __cplusplus
}
#endif
