// Vietnamese lunar calendar core (Hồ Ngọc Đức algorithm, time zone UTC+7).
// Pure C++, no Windows or third-party dependency.
#pragma once
#include <cmath>

namespace vnl {

constexpr double kPi = 3.14159265358979323846;
// Vietnam calendar: UTC+8 before 1968, UTC+7 since 1968.
inline double TimeZoneFor(int solarYear) { return solarYear < 1968 ? 8.0 : 7.0; }

inline int IntFloor(double d) { return static_cast<int>(std::floor(d)); }

inline int Jdn(int d, int m, int y) {
    int a = (14 - m) / 12;
    int yy = y + 4800 - a;
    int mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

inline void JdnToDate(int jdn, int& y, int& m, int& d) {
    int a = jdn + 32044;
    int b = (4 * a + 3) / 146097;
    int c = a - 146097 * b / 4;
    int dd = (4 * c + 3) / 1461;
    int e = c - 1461 * dd / 4;
    int mm = (5 * e + 2) / 153;
    d = e - (153 * mm + 2) / 5 + 1;
    m = mm + 3 - 12 * (mm / 10);
    y = 100 * b + dd - 4800 + mm / 10;
}

// 0 = Sunday ... 6 = Saturday
inline int DayOfWeekSun0(int jdn) { return (jdn + 1) % 7; }

inline double NewMoon(int k) {
    double T = k / 1236.85, T2 = T * T, T3 = T2 * T, dr = kPi / 180.0;
    double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
    Jd1 += 0.00033 * std::sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
    double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
    double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
    double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
    double C1 = (0.1734 - 0.000393 * T) * std::sin(M * dr) + 0.0021 * std::sin(2 * dr * M);
    C1 = C1 - 0.4068 * std::sin(Mpr * dr) + 0.0161 * std::sin(dr * 2 * Mpr);
    C1 = C1 - 0.0004 * std::sin(dr * 3 * Mpr);
    C1 = C1 + 0.0104 * std::sin(dr * 2 * F) - 0.0051 * std::sin(dr * (M + Mpr));
    C1 = C1 - 0.0074 * std::sin(dr * (M - Mpr)) + 0.0004 * std::sin(dr * (2 * F + M));
    C1 = C1 - 0.0004 * std::sin(dr * (2 * F - M)) - 0.0006 * std::sin(dr * (2 * F + Mpr));
    C1 = C1 + 0.0010 * std::sin(dr * (2 * F - Mpr)) + 0.0005 * std::sin(dr * (2 * Mpr + M));
    double dt;
    if (T < -11) dt = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3;
    else dt = -0.000278 + 0.000265 * T + 0.000262 * T2;
    return Jd1 + C1 - dt;
}

inline int NewMoonDay(int k, double tz) { return IntFloor(NewMoon(k) + 0.5 + tz / 24.0); }

inline double SunLongitude(double jdn) {
    double T = (jdn - 2451545.0) / 36525.0, T2 = T * T, dr = kPi / 180.0;
    double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
    double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
    double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * std::sin(dr * M);
    DL += (0.019993 - 0.000101 * T) * std::sin(dr * 2 * M) + 0.000290 * std::sin(dr * 3 * M);
    double L = (L0 + DL) * dr;
    L -= kPi * 2 * IntFloor(L / (kPi * 2));
    return L;
}

inline int SunSector(int dayNumber, double tz) {
    return IntFloor(SunLongitude(dayNumber - 0.5 - tz / 24.0) / kPi * 6.0);
}

inline int LunarMonth11(int yy, double tz) {
    int off = Jdn(31, 12, yy) - 2415021;
    int k = IntFloor(off / 29.530588853);
    int nm = NewMoonDay(k, tz);
    if (SunSector(nm, tz) >= 9) nm = NewMoonDay(k - 1, tz);
    return nm;
}

inline int LeapMonthOffset(int a11, double tz) {
    int k = IntFloor((a11 - 2415021.076998695) / 29.530588853 + 0.5);
    int last = 0, i = 1;
    int arc = SunSector(NewMoonDay(k + i, tz), tz);
    do {
        last = arc;
        i++;
        arc = SunSector(NewMoonDay(k + i, tz), tz);
    } while (arc != last && i < 14);
    return i - 1;
}

struct Lunar {
    int day = 1, month = 1, year = 2000;
    bool leap = false;
};

inline Lunar ToLunar(int sYear, int sMonth, int sDay) {
    Lunar r;
    const double tz = TimeZoneFor(sYear);
    int dayNumber = Jdn(sDay, sMonth, sYear);
    // The mean-lunation estimate of k can be off by one for some dates (e.g. 2054-05-07):
    // correct it against the real new-moon days so the day number can never be 0 or 31.
    int k = IntFloor((dayNumber - 2415021.076998695) / 29.530588853);
    while (NewMoonDay(k + 1, tz) <= dayNumber) k++;
    int monthStart = NewMoonDay(k, tz);
    while (monthStart > dayNumber) {
        k--;
        monthStart = NewMoonDay(k, tz);
    }
    int a11 = LunarMonth11(sYear, tz);
    int b11 = a11;
    if (a11 >= monthStart) {
        r.year = sYear;
        a11 = LunarMonth11(sYear - 1, tz);
    } else {
        r.year = sYear + 1;
        b11 = LunarMonth11(sYear + 1, tz);
    }
    r.day = dayNumber - monthStart + 1;
    int diff = IntFloor((monthStart - a11) / 29.0);
    r.leap = false;
    r.month = diff + 11;
    if (b11 - a11 > 365) {
        int leapDiff = LeapMonthOffset(a11, tz);
        if (diff >= leapDiff) {
            r.month = diff + 10;
            if (diff == leapDiff) r.leap = true;
        }
    }
    if (r.month > 12) r.month -= 12;
    if (r.month >= 11 && diff < 4) r.year -= 1;
    return r;
}

inline const wchar_t* Can(int i) {
    static const wchar_t* t[] = {L"Giáp", L"Ất", L"Bính", L"Đinh", L"Mậu", L"Kỷ", L"Canh", L"Tân", L"Nhâm", L"Quý"};
    return t[((i % 10) + 10) % 10];
}
inline const wchar_t* Chi(int i) {
    static const wchar_t* t[] = {L"Tý", L"Sửu", L"Dần", L"Mão", L"Thìn", L"Tỵ", L"Ngọ", L"Mùi", L"Thân", L"Dậu", L"Tuất", L"Hợi"};
    return t[((i % 12) + 12) % 12];
}

// Can Chi indexes
inline int YearCan(int lunarYear) { return ((lunarYear + 6) % 10 + 10) % 10; }
inline int YearChi(int lunarYear) { return ((lunarYear + 8) % 12 + 12) % 12; }
inline int MonthCan(int lunarYear, int lunarMonth) { return (lunarYear * 12 + lunarMonth + 3) % 10; }
inline int MonthChi(int lunarMonth) { return (lunarMonth + 1) % 12; }
inline int DayCan(int jdn) { return (jdn + 9) % 10; }
inline int DayChi(int jdn) { return (jdn + 1) % 12; }

// Lunar festivals. redSolar: the solar day number is red as well (public holidays).
struct Festival {
    int day, month;
    bool redSolar;
    const wchar_t* name;
};

inline const Festival* FindLunarFestival(int lunarDay, int lunarMonth, bool leap) {
    static const Festival t[] = {
        {1, 1, true, L"Mùng 1 Tết Nguyên Đán"},
        {2, 1, true, L"Mùng 2 Tết Nguyên Đán"},
        {3, 1, true, L"Mùng 3 Tết Nguyên Đán"},
        {4, 1, true, L"Mùng 4 Tết Nguyên Đán"},
        {7, 1, false, L"Lễ Khai hạ (hạ nêu)"},
        {9, 1, false, L"Vía Ngọc Hoàng"},
        {10, 1, false, L"Vía Thần Tài"},
        {15, 1, false, L"Tết Nguyên Tiêu (Rằm tháng Giêng)"},
        {3, 3, false, L"Tết Hàn Thực"},
        {10, 3, true, L"Giỗ Tổ Hùng Vương"},
        {15, 4, false, L"Lễ Phật Đản"},
        {5, 5, false, L"Tết Đoan Ngọ"},
        {7, 7, false, L"Tết Thất Tịch"},
        {15, 7, false, L"Lễ Vu Lan / Tết Trung Nguyên"},
        {15, 8, false, L"Tết Trung Thu"},
        {9, 9, false, L"Tết Trùng Cửu"},
        {10, 10, false, L"Tết Thường Tân (Tết cơm mới)"},
        {15, 10, false, L"Tết Hạ Nguyên"},
        {23, 12, false, L"Ông Công Ông Táo"},
    };
    if (leap) return nullptr;
    for (const auto& f : t) {
        if (f.day == lunarDay && f.month == lunarMonth) return &f;
    }
    return nullptr;
}

// Solar days: name only (shown in the info line, not coloured red).
inline const wchar_t* FindSolarHoliday(int d, int m) {
    struct S { int d, m; const wchar_t* n; };
    static const S t[] = {
        {1, 1, L"Tết Dương lịch"}, {14, 2, L"Lễ Tình nhân (Valentine)"}, {8, 3, L"Quốc tế Phụ nữ"},
        {26, 3, L"Thành lập Đoàn TNCS Hồ Chí Minh"}, {30, 4, L"Ngày Giải phóng miền Nam"},
        {1, 5, L"Quốc tế Lao động"}, {1, 6, L"Quốc tế Thiếu nhi"}, {2, 9, L"Quốc khánh Việt Nam"},
        {20, 10, L"Ngày Phụ nữ Việt Nam"}, {20, 11, L"Ngày Nhà giáo Việt Nam"},
        {24, 11, L"Ngày Văn hóa Việt Nam"}, {22, 12, L"Thành lập Quân đội Nhân dân Việt Nam"},
        {24, 12, L"Đêm Giáng Sinh"}, {25, 12, L"Lễ Giáng Sinh (Noel)"},
    };
    for (const auto& s : t) {
        if (s.d == d && s.m == m) return s.n;
    }
    return nullptr;
}

}  // namespace vnl
