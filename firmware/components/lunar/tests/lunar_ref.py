#!/usr/bin/env python3
"""Reference port of lunar.c (Ho Ngoc Duc's algorithm, UTC+7), line for line.

Usage:
  python lunar_ref.py check          # validate against known Vietnamese dates
  python lunar_ref.py dump > ref.txt # same output format as tests/host_dump.c
  python lunar_ref.py compare c.txt  # compare a host_dump.c output with this port
"""
import math
import sys

TZ = 7
PI = math.pi
MIN_YEAR, MAX_YEAR = 1900, 2199


def jd_from_date(yy, mm, dd):
    a = (14 - mm) // 12
    y = yy + 4800 - a
    m = mm + 12 * a - 3
    jd = dd + (153 * m + 2) // 5 + 365 * y + y // 4 - y // 100 + y // 400 - 32045
    if jd < 2299161:
        jd = dd + (153 * m + 2) // 5 + 365 * y + y // 4 - 32083
    return jd


def jd_to_date(jd):
    if jd > 2299160:
        a = jd + 32044
        b = (4 * a + 3) // 146097
        c = a - (b * 146097) // 4
    else:
        b = 0
        c = jd + 32082
    d = (4 * c + 3) // 1461
    e = c - (1461 * d) // 4
    m = (5 * e + 2) // 153
    day = e - (153 * m + 2) // 5 + 1
    month = m + 3 - 12 * (m // 10)
    year = b * 100 + d - 4800 + m // 10
    return year, month, day


def new_moon(k):
    T = k / 1236.85
    T2 = T * T
    T3 = T2 * T
    dr = PI / 180
    Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3
    Jd1 = Jd1 + 0.00033 * math.sin((166.56 + 132.87 * T - 0.009173 * T2) * dr)
    M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3
    Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3
    F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3
    C1 = (0.1734 - 0.000393 * T) * math.sin(M * dr) + 0.0021 * math.sin(2 * dr * M)
    C1 = C1 - 0.4068 * math.sin(Mpr * dr) + 0.0161 * math.sin(dr * 2 * Mpr)
    C1 = C1 - 0.0004 * math.sin(dr * 3 * Mpr)
    C1 = C1 + 0.0104 * math.sin(dr * 2 * F) - 0.0051 * math.sin(dr * (M + Mpr))
    C1 = C1 - 0.0074 * math.sin(dr * (M - Mpr)) + 0.0004 * math.sin(dr * (2 * F + M))
    C1 = C1 - 0.0004 * math.sin(dr * (2 * F - M)) - 0.0006 * math.sin(dr * (2 * F + Mpr))
    C1 = C1 + 0.0010 * math.sin(dr * (2 * F - Mpr)) + 0.0005 * math.sin(dr * (2 * Mpr + M))
    if T < -11:
        deltat = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3
    else:
        deltat = -0.000278 + 0.000265 * T + 0.000262 * T2
    return Jd1 + C1 - deltat


def sun_longitude(jdn):
    T = (jdn - 2451545.0) / 36525
    T2 = T * T
    dr = PI / 180
    M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2
    L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2
    DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * math.sin(dr * M)
    DL = DL + (0.019993 - 0.000101 * T) * math.sin(dr * 2 * M) + 0.000290 * math.sin(dr * 3 * M)
    L = (L0 + DL) * dr
    L = L - PI * 2 * math.floor(L / (PI * 2))
    return L


def get_new_moon_day(k):
    return math.floor(new_moon(k) + 0.5 + TZ / 24.0)


def get_sun_longitude(day_number):
    return math.floor(sun_longitude(day_number - 0.5 - TZ / 24.0) / PI * 6)


def get_lunar_month11(yy):
    off = jd_from_date(yy, 12, 31) - 2415021
    k = math.floor(off / 29.530588853)
    nm = get_new_moon_day(k)
    if get_sun_longitude(nm) >= 9:
        nm = get_new_moon_day(k - 1)
    return nm


def get_leap_month_offset(a11):
    k = math.floor((a11 - 2415021.076998695) / 29.530588853 + 0.5)
    i = 1
    arc = get_sun_longitude(get_new_moon_day(k + i))
    while True:
        last = arc
        i += 1
        arc = get_sun_longitude(get_new_moon_day(k + i))
        if not (arc != last and i < 14):
            break
    return i - 1


def lunar_from_jd(day_number):
    yy, _, _ = jd_to_date(day_number)
    k = math.floor((day_number - 2415021.076998695) / 29.530588853)
    month_start = get_new_moon_day(k + 1)
    if month_start > day_number:
        month_start = get_new_moon_day(k)
    if month_start > day_number:  # not in the original (2054-05-07, 2062-04-09)
        month_start = get_new_moon_day(k - 1)
    a11 = get_lunar_month11(yy)
    b11 = a11
    if a11 >= month_start:
        lunar_year = yy
        a11 = get_lunar_month11(yy - 1)
    else:
        lunar_year = yy + 1
        b11 = get_lunar_month11(yy + 1)
    lunar_day = day_number - month_start + 1
    diff = (month_start - a11) // 29
    leap = False
    lunar_month = diff + 11
    if b11 - a11 > 365:
        leap_diff = get_leap_month_offset(a11)
        if diff >= leap_diff:
            lunar_month = diff + 10
            if diff == leap_diff:
                leap = True
    if lunar_month > 12:
        lunar_month -= 12
    if lunar_month >= 11 and diff < 4:
        lunar_year -= 1
    return lunar_day, lunar_month, lunar_year, leap


def lunar_from_solar(y, m, d):
    return lunar_from_jd(jd_from_date(y, m, d))


def jd_from_lunar(ld, lm, ly, leap):
    if not (1 <= lm <= 12 and 1 <= ld <= 30):
        return -1
    if lm < 11:
        a11 = get_lunar_month11(ly - 1)
        b11 = get_lunar_month11(ly)
    else:
        a11 = get_lunar_month11(ly)
        b11 = get_lunar_month11(ly + 1)
    off = lm - 11
    if off < 0:
        off += 12
    if b11 - a11 > 365:
        leap_off = get_leap_month_offset(a11)
        leap_month = leap_off - 2
        if leap_month < 0:
            leap_month += 12
        if leap and lm != leap_month:
            return -1
        elif leap or off >= leap_off:
            off += 1
    elif leap:
        return -1
    k = math.floor(0.5 + (a11 - 2415021.076998695) / 29.530588853)
    return get_new_moon_day(k + off) + ld - 1


def solar_from_lunar(ld, lm, ly, leap):
    jd = jd_from_lunar(ld, lm, ly, leap)
    return None if jd < 0 else jd_to_date(jd)


def leap_month(ly):
    tet = jd_from_lunar(1, 1, ly, False)
    for i in range(13):
        d, m, y, lp = lunar_from_jd(tet + 15 + (i * 2953) // 100)
        if y != ly:
            break
        if lp:
            return m
    return 0


CAN = ["Giáp", "Ất", "Bính", "Đinh", "Mậu", "Kỷ", "Canh", "Tân", "Nhâm", "Quý"]
CHI = ["Tý", "Sửu", "Dần", "Mão", "Thìn", "Tỵ", "Ngọ", "Mùi", "Thân", "Dậu", "Tuất", "Hợi"]


def can_chi_year(y):
    return f"{CAN[(y + 6) % 10]} {CHI[(y + 8) % 12]}"


def can_chi_month(m, y):
    return f"{CAN[(y * 12 + m + 3) % 10]} {CHI[(m + 1) % 12]}"


def can_chi_day(jd):
    return f"{CAN[(jd + 9) % 10]} {CHI[(jd + 1) % 12]}"


# --------------------------------------------------------------------------
# Known data (Vietnamese calendar, UTC+7), from published Vietnamese calendars.
# --------------------------------------------------------------------------
KNOWN_TET = {
    1968: (1968, 1, 29),  # Tết Mậu Thân; China: 1968-01-30
    1985: (1985, 1, 21),  # Vietnam differs from China (1985-02-20)
    2000: (2000, 2, 5), 2001: (2001, 1, 24), 2002: (2002, 2, 12), 2003: (2003, 2, 1),
    2004: (2004, 1, 22), 2005: (2005, 2, 9), 2006: (2006, 1, 29), 2007: (2007, 2, 17),
    2008: (2008, 2, 7), 2009: (2009, 1, 26), 2010: (2010, 2, 14), 2011: (2011, 2, 3),
    2012: (2012, 1, 23), 2013: (2013, 2, 10), 2014: (2014, 1, 31), 2015: (2015, 2, 19),
    2016: (2016, 2, 8), 2017: (2017, 1, 28), 2018: (2018, 2, 16), 2019: (2019, 2, 5),
    2020: (2020, 1, 25), 2021: (2021, 2, 12), 2022: (2022, 2, 1), 2023: (2023, 1, 22),
    2024: (2024, 2, 10), 2025: (2025, 1, 29), 2026: (2026, 2, 17), 2027: (2027, 2, 6),
    2028: (2028, 1, 26), 2029: (2029, 2, 13),
    2030: (2030, 2, 2),   # Vietnam: new moon 23:07 UTC+7 on Feb 2; China: Feb 3
    2033: (2033, 1, 31), 2034: (2034, 2, 19),
}
KNOWN_LEAP = {
    1979: 6, 1982: 4,
    1985: 2,   # Vietnam 2 nhuận (China: 1984 leap 10)
    1987: 7,   # Vietnam 7 nhuận (China: 6); 1987-08-07 = 13/7 on VN calendars
    1990: 5, 1993: 3, 1995: 8, 1998: 5,
    2001: 4, 2004: 2, 2006: 7, 2009: 5, 2012: 4, 2014: 9, 2017: 6, 2020: 4,
    2023: 2, 2025: 6, 2028: 5, 2031: 3, 2033: 11,
}
NO_LEAP = [1984, 1986, 1988, 2000, 2002, 2003, 2005, 2007, 2008, 2010, 2011, 2013, 2015, 2016, 2018,
           2019, 2021, 2022, 2024, 2026, 2027, 2029, 2030]
# (solar date) -> (lunar day, month, leap)
KNOWN_DAYS = {
    (2025, 2, 12): (15, 1, False),   # Rằm tháng Giêng Ất Tỵ
    (2025, 4, 7): (10, 3, False),    # Giỗ Tổ 2025
    (2025, 5, 12): (15, 4, False),   # Phật Đản 2025
    (2025, 5, 31): (5, 5, False),    # Đoan Ngọ 2025
    (2025, 7, 25): (1, 6, True),     # 1/6 nhuận Ất Tỵ
    (2025, 9, 6): (15, 7, False),    # Vu Lan 2025
    (2025, 10, 6): (15, 8, False),   # Trung Thu 2025
    (2025, 1, 22): (23, 12, False),  # Ông Táo, end of Giáp Thìn
    (2024, 4, 18): (10, 3, False),   # Giỗ Tổ 2024
    (2024, 9, 17): (15, 8, False),   # Trung Thu 2024
    (2026, 4, 26): (10, 3, False),   # Giỗ Tổ 2026
    (2026, 9, 25): (15, 8, False),   # Trung Thu 2026
    (2023, 9, 29): (15, 8, False),   # Trung Thu 2023
    (2023, 3, 22): (1, 2, True),     # 1/2 nhuận Quý Mão
    (1987, 7, 1): (6, 6, False),     # VN calendar sites (xemlicham, baolamdong)
    (1987, 7, 7): (12, 6, False),
    (1987, 8, 7): (13, 7, False),
    (2054, 5, 7): (30, 3, False),    # regression: original algorithm gave day 0
    (2062, 4, 9): (30, 2, False),
}
KNOWN_CANCHI_YEAR = {1968: "Mậu Thân", 1985: "Ất Sửu", 2000: "Canh Thìn", 2020: "Canh Tý",
                     2024: "Giáp Thìn", 2025: "Ất Tỵ", 2026: "Bính Ngọ"}
KNOWN_CANCHI_DAY = {(2000, 1, 1): "Mậu Ngọ"}


def check():
    ok = bad = 0

    def expect(cond, msg):
        nonlocal ok, bad
        if cond:
            ok += 1
        else:
            bad += 1
            print("FAIL:", msg)

    for ly, sd in KNOWN_TET.items():
        got = solar_from_lunar(1, 1, ly, False)
        expect(got == sd, f"Tet {ly}: got {got}, want {sd}")
        expect(lunar_from_solar(*sd) == (1, 1, ly, False), f"lunar_from_solar{sd}")
    for ly, lm in KNOWN_LEAP.items():
        expect(leap_month(ly) == lm, f"leap {ly}: got {leap_month(ly)}, want {lm}")
    for ly in NO_LEAP:
        expect(leap_month(ly) == 0, f"no leap {ly}: got {leap_month(ly)}")
    for sd, (d, m, lp) in KNOWN_DAYS.items():
        got = lunar_from_solar(*sd)
        expect(got[:2] == (d, m) and got[3] == lp, f"{sd}: got {got}, want {(d, m, lp)}")
    for y, s in KNOWN_CANCHI_YEAR.items():
        expect(can_chi_year(y) == s, f"canchi year {y}: {can_chi_year(y)} != {s}")
    for sd, s in KNOWN_CANCHI_DAY.items():
        expect(can_chi_day(jd_from_date(*sd)) == s, f"canchi day {sd}")
    expect(can_chi_month(1, 2025) == "Mậu Dần", f"canchi month 1/2025 {can_chi_month(1, 2025)}")
    # Structural checks over the whole range
    prev = None
    for jd in range(jd_from_date(MIN_YEAR, 1, 1), jd_from_date(MAX_YEAR, 12, 31) + 1):
        cur = lunar_from_jd(jd)
        if prev is not None:
            if cur[0] == 1:
                if not (prev[0] in (29, 30)):
                    expect(False, f"month length {prev} before jd {jd}")
            elif cur[0] != prev[0] + 1:
                expect(False, f"day sequence jd {jd}")
        prev = cur
    for ly in range(MIN_YEAR, MAX_YEAR):
        n = (jd_from_lunar(1, 1, ly + 1, False) - jd_from_lunar(1, 1, ly, False))
        has_leap = leap_month(ly) != 0
        expect((383 <= n <= 385) if has_leap else (353 <= n <= 355), f"year length {ly}: {n} leap={has_leap}")
    print(f"check: {ok} passed, {bad} failed")
    return bad == 0


def dump(out=sys.stdout):
    w = out.write
    for jd in range(jd_from_date(MIN_YEAR, 1, 1), jd_from_date(MAX_YEAR, 12, 31) + 1):
        y, m, d = jd_to_date(jd)
        ld, lm, ly, lp = lunar_from_solar(y, m, d)
        w(f"{y:04d}-{m:02d}-{d:02d} {jd} {ld} {lm} {ly} {1 if lp else 0}\n")
    for ly in range(MIN_YEAR, MAX_YEAR):
        w(f"LEAP {ly} {leap_month(ly)}\n")


def compare(path):
    import io
    buf = io.StringIO()
    dump(buf)
    ref = buf.getvalue().splitlines()
    with open(path, encoding="ascii") as f:
        got = f.read().splitlines()
    diffs = [(a, b) for a, b in zip(ref, got) if a != b]
    print(f"compare: python {len(ref)} lines, C {len(got)} lines, {len(diffs)} differing")
    for a, b in diffs[:20]:
        print("  py:", a, "| c:", b)
    return len(ref) == len(got) and not diffs


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        sys.exit(0 if check() else 1)
    elif cmd == "dump":
        dump()
    elif cmd == "compare":
        sys.exit(0 if compare(sys.argv[2]) else 1)
