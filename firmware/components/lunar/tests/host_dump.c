/* Host test driver: dumps lunar_from_solar() for every day in 1900..2199 and
 * checks solar_from_lunar() round-trips. Build with any host C compiler, e.g.
 *   python -m ziglang cc -O2 -Iinclude lunar.c tests/host_dump.c -o host_dump.exe
 * Output lines: "Y-M-D jd lday lmonth lyear leap"; compared with lunar_ref.py. */
#include <stdio.h>
#include "lunar.h"

int main(void)
{
    int32_t jd0 = lunar_jd_from_date(LUNAR_MIN_YEAR, 1, 1);
    int32_t jd1 = lunar_jd_from_date(LUNAR_MAX_YEAR, 12, 31);
    long bad_rt = 0, n = 0;
    for (int32_t jd = jd0; jd <= jd1; jd++) {
        int y, m, d;
        lunar_jd_to_date(jd, &y, &m, &d);
        lunar_date_t l = lunar_from_solar(y, m, d);
        printf("%04d-%02d-%02d %ld %d %d %d %d\n", y, m, d, (long)l.jd, l.day, l.month, l.year, l.leap ? 1 : 0);
        int y2, m2, d2;
        if (!solar_from_lunar(l.day, l.month, l.year, l.leap, &y2, &m2, &d2) || y2 != y || m2 != m || d2 != d) {
            fprintf(stderr, "ROUNDTRIP FAIL %d-%d-%d\n", y, m, d);
            bad_rt++;
        }
        n++;
    }
    /* Leap month table and month lengths */
    for (int ly = LUNAR_MIN_YEAR; ly < LUNAR_MAX_YEAR; ly++) {
        printf("LEAP %d %d\n", ly, lunar_leap_month(ly));
    }
    char a[24], b[24], c[24], f[48];
    lunar_date_t t = lunar_from_solar(2025, 1, 29);
    fprintf(stderr, "2025-01-29: %s | year %s month %s day %s | %s | %s\n",
            lunar_format(&t, LUNAR_FMT_VI, f, sizeof f), lunar_can_chi_year(t.year, a, sizeof a),
            lunar_can_chi_month(t.month, t.year, b, sizeof b), lunar_can_chi_day(t.jd, c, sizeof c),
            lunar_holiday(2025, 1, 29, &t, false), lunar_holiday(2025, 1, 29, NULL, true));
    fprintf(stderr, "days=%ld roundtrip_fail=%ld\n", n, bad_rt);
    return bad_rt != 0;
}
