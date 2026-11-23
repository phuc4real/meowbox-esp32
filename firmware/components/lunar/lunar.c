/*
 * lunar.c - Vietnamese lunar calendar, Hồ Ngọc Đức's algorithm
 * (https://www.informatik.uni-leipzig.de/~duc/amlich/), time zone UTC+7.
 *
 * Kept deliberately 1:1 with tests/lunar_ref.py so the two can be compared
 * day by day. Uses double precision (soft-float on ESP32-S3; one conversion
 * costs a few dozen sin() calls, well under 1 ms).
 */
#include "lunar.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846

/* ------------------------------------------------------------------------ */
/* Julian day                                                               */
/* ------------------------------------------------------------------------ */

static int32_t floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

int32_t lunar_jd_from_date(int yy, int mm, int dd)
{
    int32_t a = floordiv(14 - mm, 12);
    int32_t y = yy + 4800 - a;
    int32_t m = mm + 12 * a - 3;
    int32_t jd = dd + floordiv(153 * m + 2, 5) + 365 * y + floordiv(y, 4)
                 - floordiv(y, 100) + floordiv(y, 400) - 32045;
    if (jd < 2299161) {
        jd = dd + floordiv(153 * m + 2, 5) + 365 * y + floordiv(y, 4) - 32083;
    }
    return jd;
}

void lunar_jd_to_date(int32_t jd, int *yy, int *mm, int *dd)
{
    int32_t a, b, c;
    if (jd > 2299160) { /* after 5/10/1582, Gregorian */
        a = jd + 32044;
        b = floordiv(4 * a + 3, 146097);
        c = a - floordiv(b * 146097, 4);
    } else {
        b = 0;
        c = jd + 32082;
    }
    int32_t d = floordiv(4 * c + 3, 1461);
    int32_t e = c - floordiv(1461 * d, 4);
    int32_t m = floordiv(5 * e + 2, 153);
    if (dd) *dd = (int)(e - floordiv(153 * m + 2, 5) + 1);
    if (mm) *mm = (int)(m + 3 - 12 * floordiv(m, 10));
    if (yy) *yy = (int)(b * 100 + d - 4800 + floordiv(m, 10));
}

/* ------------------------------------------------------------------------ */
/* Astronomy                                                                */
/* ------------------------------------------------------------------------ */

/* Julian day (fractional, UT) of the k-th new moon after 1900-01-01. */
static double new_moon(int32_t k)
{
    const double T = k / 1236.85;
    const double T2 = T * T;
    const double T3 = T2 * T;
    const double dr = PI / 180;
    double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
    Jd1 = Jd1 + 0.00033 * sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
    const double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
    const double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
    const double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
    double C1 = (0.1734 - 0.000393 * T) * sin(M * dr) + 0.0021 * sin(2 * dr * M);
    C1 = C1 - 0.4068 * sin(Mpr * dr) + 0.0161 * sin(dr * 2 * Mpr);
    C1 = C1 - 0.0004 * sin(dr * 3 * Mpr);
    C1 = C1 + 0.0104 * sin(dr * 2 * F) - 0.0051 * sin(dr * (M + Mpr));
    C1 = C1 - 0.0074 * sin(dr * (M - Mpr)) + 0.0004 * sin(dr * (2 * F + M));
    C1 = C1 - 0.0004 * sin(dr * (2 * F - M)) - 0.0006 * sin(dr * (2 * F + Mpr));
    C1 = C1 + 0.0010 * sin(dr * (2 * F - Mpr)) + 0.0005 * sin(dr * (2 * Mpr + M));
    double deltat;
    if (T < -11) {
        deltat = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3;
    } else {
        deltat = -0.000278 + 0.000265 * T + 0.000262 * T2;
    }
    return Jd1 + C1 - deltat;
}

/* Sun's apparent longitude in radians [0, 2PI) at fractional JD jdn (UT). */
static double sun_longitude(double jdn)
{
    const double T = (jdn - 2451545.0) / 36525;
    const double T2 = T * T;
    const double dr = PI / 180;
    const double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
    const double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
    double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * sin(dr * M);
    DL = DL + (0.019993 - 0.000101 * T) * sin(dr * 2 * M) + 0.000290 * sin(dr * 3 * M);
    double L = (L0 + DL) * dr;
    L = L - PI * 2 * floor(L / (PI * 2));
    return L;
}

static int32_t get_new_moon_day(int32_t k)
{
    return (int32_t)floor(new_moon(k) + 0.5 + LUNAR_TIMEZONE / 24.0);
}

/* Sun longitude sector 0..11 (30° each) at local midnight starting day_number. */
static int get_sun_longitude(int32_t day_number)
{
    return (int)floor(sun_longitude(day_number - 0.5 - LUNAR_TIMEZONE / 24.0) / PI * 6);
}

/* JD of the start of lunar month 11 (the one containing the winter solstice) of solar year yy. */
static int32_t get_lunar_month11(int yy)
{
    const int32_t off = lunar_jd_from_date(yy, 12, 31) - 2415021;
    const int32_t k = (int32_t)floor(off / 29.530588853);
    int32_t nm = get_new_moon_day(k);
    if (get_sun_longitude(nm) >= 9) {
        nm = get_new_moon_day(k - 1);
    }
    return nm;
}

/* Offset (in months, counted from month 11 starting at a11) of the leap month. */
static int get_leap_month_offset(int32_t a11)
{
    const int32_t k = (int32_t)floor((a11 - 2415021.076998695) / 29.530588853 + 0.5);
    int last;
    int i = 1;
    int arc = get_sun_longitude(get_new_moon_day(k + i));
    do {
        last = arc;
        i++;
        arc = get_sun_longitude(get_new_moon_day(k + i));
    } while (arc != last && i < 14);
    return i - 1;
}

/* ------------------------------------------------------------------------ */
/* Conversion                                                               */
/* ------------------------------------------------------------------------ */

static lunar_date_t lunar_from_jd(int32_t day_number)
{
    int yy;
    lunar_jd_to_date(day_number, &yy, NULL, NULL);

    lunar_date_t r;
    r.jd = day_number;

    const int32_t k = (int32_t)floor((day_number - 2415021.076998695) / 29.530588853);
    int32_t month_start = get_new_moon_day(k + 1);
    if (month_start > day_number) {
        month_start = get_new_moon_day(k);
    }
    /* Not in the original: the mean-lunation estimate of k is occasionally
     * one too high (e.g. 2054-05-07, 2062-04-09 would give day 0). */
    if (month_start > day_number) {
        month_start = get_new_moon_day(k - 1);
    }
    int32_t a11 = get_lunar_month11(yy);
    int32_t b11 = a11;
    int lunar_year;
    if (a11 >= month_start) {
        lunar_year = yy;
        a11 = get_lunar_month11(yy - 1);
    } else {
        lunar_year = yy + 1;
        b11 = get_lunar_month11(yy + 1);
    }
    const int lunar_day = (int)(day_number - month_start + 1);
    const int diff = (int)floordiv(month_start - a11, 29);
    bool lunar_leap = false;
    int lunar_month = diff + 11;
    if (b11 - a11 > 365) {
        const int leap_month_diff = get_leap_month_offset(a11);
        if (diff >= leap_month_diff) {
            lunar_month = diff + 10;
            if (diff == leap_month_diff) {
                lunar_leap = true;
            }
        }
    }
    if (lunar_month > 12) {
        lunar_month = lunar_month - 12;
    }
    if (lunar_month >= 11 && diff < 4) {
        lunar_year -= 1;
    }
    r.day = lunar_day;
    r.month = lunar_month;
    r.year = lunar_year;
    r.leap = lunar_leap;
    return r;
}

lunar_date_t lunar_from_solar(int y, int m, int d)
{
    return lunar_from_jd(lunar_jd_from_date(y, m, d));
}

/* JD of (lunar_day, lunar_month, lunar_year, leap) or -1 if invalid. */
static int32_t jd_from_lunar(int lunar_day, int lunar_month, int lunar_year, bool lunar_leap)
{
    if (lunar_month < 1 || lunar_month > 12 || lunar_day < 1 || lunar_day > 30) return -1;
    int32_t a11, b11;
    if (lunar_month < 11) {
        a11 = get_lunar_month11(lunar_year - 1);
        b11 = get_lunar_month11(lunar_year);
    } else {
        a11 = get_lunar_month11(lunar_year);
        b11 = get_lunar_month11(lunar_year + 1);
    }
    int off = lunar_month - 11;
    if (off < 0) {
        off += 12;
    }
    if (b11 - a11 > 365) {
        const int leap_off = get_leap_month_offset(a11);
        int leap_month = leap_off - 2;
        if (leap_month < 0) {
            leap_month += 12;
        }
        if (lunar_leap && lunar_month != leap_month) {
            return -1;
        } else if (lunar_leap || off >= leap_off) {
            off += 1;
        }
    } else if (lunar_leap) {
        return -1;
    }
    const int32_t k = (int32_t)floor(0.5 + (a11 - 2415021.076998695) / 29.530588853);
    const int32_t month_start = get_new_moon_day(k + off);
    return month_start + lunar_day - 1;
}

bool solar_from_lunar(int lunar_day, int lunar_month, int lunar_year, bool leap,
                      int *y, int *m, int *d)
{
    const int32_t jd = jd_from_lunar(lunar_day, lunar_month, lunar_year, leap);
    if (jd < 0) {
        if (y) *y = 0;
        if (m) *m = 0;
        if (d) *d = 0;
        return false;
    }
    lunar_jd_to_date(jd, y, m, d);
    return true;
}

int lunar_leap_month(int lunar_year)
{
    const int32_t tet = jd_from_lunar(1, 1, lunar_year, false);
    if (tet < 0) return 0;
    /* Sample the middle of each of up to 13 months of this lunar year. */
    for (int i = 0; i < 13; i++) {
        const lunar_date_t ld = lunar_from_jd(tet + 15 + (i * 2953) / 100);
        if (ld.year != lunar_year) break;
        if (ld.leap) return ld.month;
    }
    return 0;
}

int lunar_month_days(int lunar_month, int lunar_year, bool leap)
{
    const int32_t start = jd_from_lunar(1, lunar_month, lunar_year, leap);
    if (start < 0) return 0;
    const lunar_date_t ld = lunar_from_jd(start + 29);
    return (ld.day == 1) ? 29 : 30;
}

/* ------------------------------------------------------------------------ */
/* Can Chi                                                                  */
/* ------------------------------------------------------------------------ */

static const char *const CAN[10] = {
    "Giáp", "Ất", "Bính", "Đinh", "Mậu", "Kỷ", "Canh", "Tân", "Nhâm", "Quý",
};
static const char *const CHI[12] = {
    "Tý", "Sửu", "Dần", "Mão", "Thìn", "Tỵ", "Ngọ", "Mùi", "Thân", "Dậu", "Tuất", "Hợi",
};
static const char *const ZODIAC_EN[12] = {
    "Rat", "Ox", "Tiger", "Cat", "Dragon", "Snake", "Horse", "Goat", "Monkey", "Rooster", "Dog", "Pig",
};

static int pmod(int32_t a, int b)
{
    int r = (int)(a % b);
    return r < 0 ? r + b : r;
}

static const char *can_chi(int can, int chi, char *buf, size_t len)
{
    if (!buf || len == 0) return "";
    snprintf(buf, len, "%s %s", CAN[can], CHI[chi]);
    return buf;
}

const char *lunar_can_chi_year(int lunar_year, char *buf, size_t len)
{
    return can_chi(pmod(lunar_year + 6, 10), pmod(lunar_year + 8, 12), buf, len);
}

/* Month 1 is always Dần; leap months take the can-chi of the regular month. */
const char *lunar_can_chi_month(int lunar_month, int lunar_year, char *buf, size_t len)
{
    return can_chi(pmod((int32_t)lunar_year * 12 + lunar_month + 3, 10),
                   pmod(lunar_month + 1, 12), buf, len);
}

const char *lunar_can_chi_day(int32_t jd, char *buf, size_t len)
{
    return can_chi(pmod(jd + 9, 10), pmod(jd + 1, 12), buf, len);
}

const char *lunar_zodiac(int lunar_year, bool english)
{
    const int chi = pmod(lunar_year + 8, 12);
    return english ? ZODIAC_EN[chi] : CHI[chi];
}

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

static const char *const MONTH_VI[12] = {
    "Giêng", "Hai", "Ba", "Tư", "Năm", "Sáu", "Bảy", "Tám", "Chín", "Mười", "Mười Một", "Chạp",
};

const char *lunar_month_name_vi(int month)
{
    if (month < 1 || month > 12) return "";
    return MONTH_VI[month - 1];
}

const char *lunar_format(const lunar_date_t *ld, lunar_fmt_t fmt, char *buf, size_t len)
{
    if (!buf || len == 0) return "";
    buf[0] = '\0';
    if (!ld) return buf;

    if (fmt == LUNAR_FMT_EN) {
        snprintf(buf, len, "Lunar %d/%d%s", ld->day, ld->month, ld->leap ? " (leap)" : "");
        return buf;
    }

    char day[24];
    if (ld->day <= 10) {
        snprintf(day, sizeof day, "Mùng %d", ld->day);
    } else {
        snprintf(day, sizeof day, "%d", ld->day);
    }
    if (fmt == LUNAR_FMT_VI_NUM) {
        snprintf(buf, len, "%s tháng %d%s", day, ld->month, ld->leap ? " nhuận" : "");
    } else {
        snprintf(buf, len, "%s tháng %s%s", day, lunar_month_name_vi(ld->month),
                 ld->leap ? " nhuận" : "");
    }
    return buf;
}

/* ------------------------------------------------------------------------ */
/* Holidays                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t day, month;
    const char *vi;
    const char *en;
} holiday_t;

static const holiday_t LUNAR_HOLIDAYS[] = {
    { 1,  1, "Tết Nguyên Đán",       "Lunar New Year" },
    { 2,  1, "Mùng 2 Tết",           "Lunar New Year (2nd day)" },
    { 3,  1, "Mùng 3 Tết",           "Lunar New Year (3rd day)" },
    {15,  1, "Rằm tháng Giêng",      "Lantern Festival" },
    {10,  3, "Giỗ Tổ Hùng Vương",    "Hung Kings' Festival" },
    {15,  4, "Lễ Phật Đản",          "Buddha's Birthday" },
    { 5,  5, "Tết Đoan Ngọ",         "Double Fifth Festival" },
    {15,  7, "Lễ Vu Lan",            "Vu Lan Festival" },
    {15,  8, "Tết Trung Thu",        "Mid-Autumn Festival" },
    {23, 12, "Ông Công Ông Táo",     "Kitchen Gods' Day" },
};

static const holiday_t SOLAR_HOLIDAYS[] = {
    { 1,  1, "Tết Dương lịch",       "New Year's Day" },
    {30,  4, "Ngày Thống nhất",      "Reunification Day" },
    { 1,  5, "Quốc tế Lao động",     "Labour Day" },
    { 2,  9, "Quốc khánh",           "National Day" },
};

const char *lunar_holiday(int y, int m, int d, const lunar_date_t *ld, bool english)
{
    lunar_date_t tmp;
    if (!ld) {
        tmp = lunar_from_solar(y, m, d);
        ld = &tmp;
    }
    if (!ld->leap) {
        for (size_t i = 0; i < sizeof LUNAR_HOLIDAYS / sizeof LUNAR_HOLIDAYS[0]; i++) {
            if (LUNAR_HOLIDAYS[i].day == ld->day && LUNAR_HOLIDAYS[i].month == ld->month) {
                return english ? LUNAR_HOLIDAYS[i].en : LUNAR_HOLIDAYS[i].vi;
            }
        }
        if (ld->month == 12 && ld->day >= 29) {
            const lunar_date_t next = lunar_from_jd(ld->jd + 1);
            if (next.day == 1 && next.month == 1 && !next.leap) {
                return english ? "Lunar New Year's Eve" : "Giao thừa";
            }
        }
    }
    for (size_t i = 0; i < sizeof SOLAR_HOLIDAYS / sizeof SOLAR_HOLIDAYS[0]; i++) {
        if (SOLAR_HOLIDAYS[i].day == d && SOLAR_HOLIDAYS[i].month == m) {
            return english ? SOLAR_HOLIDAYS[i].en : SOLAR_HOLIDAYS[i].vi;
        }
    }
    return NULL;
}
