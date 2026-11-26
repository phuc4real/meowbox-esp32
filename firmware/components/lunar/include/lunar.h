/*
 * lunar.h - Vietnamese lunar calendar (Âm lịch) for MeowBox.
 *
 * Algorithm: Hồ Ngọc Đức, "Âm lịch Việt Nam" (astronomical new moon and
 * solar term computation, time zone UTC+7). Pure C, no heap allocation,
 * re-entrant. Valid range: solar years 1900..2199.
 *
 * All strings are UTF-8.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUNAR_TIMEZONE   7      /* Vietnam, UTC+7 */
#define LUNAR_MIN_YEAR   1900
#define LUNAR_MAX_YEAR   2199

typedef struct {
    int     day;    /* 1..30 */
    int     month;  /* 1..12 */
    int     year;   /* lunar year (Tết of this year starts it) */
    bool    leap;   /* true if this is a leap (nhuận) month */
    int32_t jd;     /* Julian day number of the solar date */
} lunar_date_t;

typedef enum {
    LUNAR_FMT_VI = 0,   /* "Mùng 3 tháng Chạp", "15 tháng Tám", "2 tháng Sáu nhuận" */
    LUNAR_FMT_VI_NUM,   /* "Mùng 3 tháng 12",   "15 tháng 8",   "2 tháng 6 nhuận"   */
    LUNAR_FMT_EN,       /* "Lunar 3/12",        "Lunar 15/8",   "Lunar 2/6 (leap)"  */
} lunar_fmt_t;

/* ---- Julian day helpers ------------------------------------------------ */
int32_t lunar_jd_from_date(int y, int m, int d);
void    lunar_jd_to_date(int32_t jd, int *y, int *m, int *d);

/* ---- Conversion -------------------------------------------------------- */

/* Solar (Gregorian) -> lunar. Out-of-range input still computes (the
 * algorithm is valid well beyond 1900..2199), but is not tested there. */
lunar_date_t lunar_from_solar(int y, int m, int d);

/* Lunar -> solar. Returns false if the date does not exist (e.g. leap=true
 * but that month is not leap in that year). Day 30 in a 29-day month is not
 * rejected (it maps to day 1 of the following month). */
bool solar_from_lunar(int lunar_day, int lunar_month, int lunar_year, bool leap,
                      int *y, int *m, int *d);

/* Leap month number (1..12) of lunar year, or 0 if that year has none. */
int lunar_leap_month(int lunar_year);

/* ---- Can Chi (sexagenary cycle), Vietnamese names --------------------- */
/* Each returns buf (UTF-8, e.g. "Ất Tỵ"); buf of 24 bytes is always enough. */
const char *lunar_can_chi_year(int lunar_year, char *buf, size_t len);
const char *lunar_can_chi_month(int lunar_month, int lunar_year, char *buf, size_t len);
const char *lunar_can_chi_day(int32_t jd, char *buf, size_t len);

/* ---- Formatting -------------------------------------------------------- */
/* Vietnamese month name: 1 -> "Giêng" ... 11 -> "Mười Một", 12 -> "Chạp". */
const char *lunar_month_name_vi(int month);

/* Formats ld into buf (UTF-8). 40 bytes is always enough. Returns buf. */
const char *lunar_format(const lunar_date_t *ld, lunar_fmt_t fmt, char *buf, size_t len);

/* ---- Holidays ---------------------------------------------------------- */
/* Returns the holiday name for the solar date (y,m,d), or NULL.
 * ld may be NULL (then computed). Lunar holidays never match leap months.
 * Covers: Tết Nguyên Đán (1-3/1), Giao thừa (last day of month 12),
 * Rằm tháng Giêng, Giỗ Tổ Hùng Vương, Phật Đản, Tết Đoan Ngọ, Vu Lan,
 * Tết Trung Thu, Ông Công Ông Táo, and solar 1/1, 30/4, 1/5, 2/9. */
const char *lunar_holiday(int y, int m, int d, const lunar_date_t *ld, bool english);

#ifdef __cplusplus
}
#endif
