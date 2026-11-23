/*
 * mb_fonts.h - MeowBox fonts for LVGL 9 (4 bpp, uncompressed).
 *
 * Text fonts cover Basic Latin, Latin-1, all Vietnamese letters
 * (U+0102/3, U+0110/1, U+0128/9, U+0168/9, U+01A0/1, U+01AF/B0,
 * U+1EA0..U+1EF9) and ‘ ’ “ ” – — … • €.
 * *_14 / *_20 fonts (and mb_font_14 / mb_font_20) also contain the LV_SYMBOL_* icons.
 * Clock fonts contain only "0123456789: -°".
 * Glyphs a theme font lacks (U+00B5 in Chakra Petch / Cormorant) are merged
 * from Be Vietnam Pro by firmware/tools/fonts/gen_theme_fonts.py.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default set (Be Vietnam Pro) */
LV_FONT_DECLARE(mb_font_14)        /* Be Vietnam Pro Regular 14 px + symbols */
LV_FONT_DECLARE(mb_font_20)        /* Be Vietnam Pro Regular 20 px + symbols */
LV_FONT_DECLARE(mb_font_20_bold)   /* Be Vietnam Pro Bold 20 px */
LV_FONT_DECLARE(mb_font_clock_64)  /* Be Vietnam Pro Bold 64 px, clock digits */

/* Theme: swiss */
LV_FONT_DECLARE(mb_sw_14)          /* Archivo Regular 14 px + symbols */
LV_FONT_DECLARE(mb_sw_20)          /* Archivo Regular 20 px + symbols */
LV_FONT_DECLARE(mb_sw_20b)         /* Archivo Bold 20 px */
LV_FONT_DECLARE(mb_sw_clock)       /* Archivo ExtraBold wdth 72, 96 px, clock digits */

/* Theme: cyberpunk */
LV_FONT_DECLARE(mb_cy_14)          /* Chakra Petch Regular 14 px + symbols */
LV_FONT_DECLARE(mb_cy_20)          /* Chakra Petch Medium 20 px + symbols */
LV_FONT_DECLARE(mb_cy_20b)         /* Chakra Petch Bold 20 px */
LV_FONT_DECLARE(mb_cy_clock)       /* VT323 Regular 92 px, clock digits */

/* Theme: zen */
LV_FONT_DECLARE(mb_zen_14)         /* Be Vietnam Pro Light 14 px + symbols */
LV_FONT_DECLARE(mb_zen_20)         /* Cormorant Garamond Medium 20 px + symbols + U+732B (Noto Serif JP SemiBold) */
LV_FONT_DECLARE(mb_zen_20b)        /* Cormorant Garamond SemiBold Italic 22 px */
LV_FONT_DECLARE(mb_zen_clock)      /* Cormorant Garamond Light 56 px, clock digits */

/* Theme: brutal */
LV_FONT_DECLARE(mb_br_14)          /* IBM Plex Mono Regular 14 px + symbols */
LV_FONT_DECLARE(mb_br_20)          /* Tinos Regular 20 px + symbols */
LV_FONT_DECLARE(mb_br_20b)         /* Tinos Bold 20 px */
LV_FONT_DECLARE(mb_br_clock)       /* Tinos Bold 84 px, clock digits */

#ifdef __cplusplus
}
#endif
