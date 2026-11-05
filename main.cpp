// VN_LunarCalendar - Vietnamese lunar calendar popup for the Windows taskbar clock (Win 10 / Win 11).
// One small exe: pure Win32 + GDI, no DLL injection, no mouse hook, no third-party code, no C++ runtime library.
// Idle cost: a hidden window + a foreground event hook used only to replace the Windows clock flyout.
// It reads only system settings: date/time format, first day of week, light/dark theme, accent colour.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <cwchar>
#include <strsafe.h>
#include "vn_lunar.h"

#ifndef LOCALE_RETURN_NUMBER
#define LOCALE_RETURN_NUMBER 0x20000000
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef LOCALE_SSHORTESTDAYNAME1
#define LOCALE_SSHORTESTDAYNAME1 0x00000031
#endif

namespace {

const wchar_t* kClassName = L"VN_LunarCalendarWnd";
const wchar_t* kMutexName = L"Local\\VN_LunarCalendar_SingleInstance";
const wchar_t* kRunKeyName = L"VN_LunarCalendar";
const wchar_t* kSettingsKey = L"Software\\VN_LunarCalendar";

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_CLOCK_CLICK = WM_APP + 2;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 3;
constexpr int ID_MENU_SETTINGS = 1006;
constexpr int ID_CFG_AUTORUN = 2001, ID_CFG_REPLACE = 2002, ID_CFG_TRAY = 2003, ID_CFG_SAVE = 2004, ID_CFG_STATUS = 2005;
constexpr UINT_PTR TIMER_CLOCK = 1;
constexpr UINT_PTR TIMER_MIDNIGHT = 2;
constexpr int ID_MENU_OPEN = 1001;
constexpr int ID_MENU_REPLACE = 1002;
constexpr int ID_MENU_AUTORUN = 1003;
constexpr int ID_MENU_ABOUT = 1005;
constexpr int ID_MENU_EXIT = 1004;
constexpr int ID_ICON = 1;   // resource.rc

// ---- layout in DIP (96 dpi); fixed comfortable width ----
constexpr double BASE_W = 356, MARGIN = 14;
constexpr double TIME_Y = 8, TIME_H = 58;
constexpr double DATE_Y = 68, LINE_H = 18, CANCHI_Y = 87;
constexpr double DIV_Y = 114;
constexpr double MONTH_Y = 122, MONTH_H = 32;
constexpr double WD_Y = 156, WD_H = 20;
constexpr double GRID_Y = 178, CELL_H = 46;
constexpr double INFO_Y = 458, INFO_H = 58;
constexpr double FOOT_Y = 520, FOOT_H = 28;
constexpr double H = 552;

// ---- state ----
HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HWND g_settings = nullptr;
HFONT g_cfgFont = nullptr;
HFONT g_cfgTitleFont = nullptr;
bool g_cfgAutorun = false, g_cfgReplace = false, g_cfgTray = false;
bool g_trayEnabled = true;
HWINEVENTHOOK g_eventHook = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreated = 0;
HICON g_trayIcon = nullptr;
bool g_replaceFlyout = true;
bool g_replacingFlyout = false;
DWORD g_shownTick = 0;
DWORD g_focusLossTick = 0;
RECT g_pendingClock{};

double g_scale = 1.0;
double g_calendarW = BASE_W;
int g_viewYear = 2000, g_viewMonth = 1;
int g_todayJdn = 0;
int g_hover = -1;       // 0..41 cell, 100 prev, 101 next, 102 footer
int g_selected = -1;
bool g_tracking = false;
bool g_dark = true;
COLORREF g_accent = RGB(0, 120, 215);
int g_firstDaySun0 = 1;  // 1 = Monday (system locale)

// created only while the calendar is visible
HFONT g_fTime, g_fDate, g_fMonth, g_fDay, g_fLunar, g_fSmall;
HDC g_memDC = nullptr;
HBITMAP g_memBmp = nullptr, g_memOld = nullptr;
int g_bufW = 0, g_bufH = 0;

struct Cell {
    int jdn, y, m, d;
    bool inMonth;
    vnl::Lunar lunar;
    const vnl::Festival* fest;
    const wchar_t* solarName;
};
Cell g_cells[42];

// small fixed buffer string (keeps the exe free of the C++ standard library)
struct Str {
    wchar_t s[200];
    int n;
    Str() : n(0) { s[0] = 0; }
    Str& A(const wchar_t* t) {
        while (t && *t && n < 199) s[n++] = *t++;
        s[n] = 0;
        return *this;
    }
    Str& N(int v) {
        wchar_t tmp[16];
        int len = 0;
        unsigned u = v < 0 ? static_cast<unsigned>(-v) : static_cast<unsigned>(v);
        do { tmp[len++] = static_cast<wchar_t>(L'0' + u % 10); u /= 10; } while (u);
        if (v < 0 && n < 199) s[n++] = L'-';
        while (len && n < 199) s[n++] = tmp[--len];
        s[n] = 0;
        return *this;
    }
};

int S(double v) { return static_cast<int>(v * g_scale + 0.5); }
int MaxI(int a, int b) { return a > b ? a : b; }

// ---------------------------------------------------------------- system settings

void LoadSystemTheme() {
    DWORD light = 0, size = sizeof(light);
    g_dark = true;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS) {
        g_dark = (light == 0);
    }
    DWORD accent = 0;
    size = sizeof(accent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &accent, &size) == ERROR_SUCCESS) {
        g_accent = RGB(accent & 0xFF, (accent >> 8) & 0xFF, (accent >> 16) & 0xFF);  // 0xAABBGGRR
    } else {
        g_accent = RGB(0, 120, 215);
    }
}

void LoadLocaleInfo() {
    DWORD v = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<LPWSTR>(&v), sizeof(v) / sizeof(WCHAR)) > 0) {
        g_firstDaySun0 = static_cast<int>((v + 1) % 7);   // Windows: 0 = Monday
    }
}

void FormatDate(const SYSTEMTIME& st, DWORD flags, const wchar_t* pattern, wchar_t* buf, int n) {
    buf[0] = 0;
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &st, pattern, buf, n, nullptr);
}

SYSTEMTIME MakeDate(int y, int m, int d) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    return st;
}

void WeekdayLetter(int col, wchar_t* buf, int n) {
    int sun0 = (g_firstDaySun0 + col) % 7;
    int winDay = (sun0 + 6) % 7;   // 0 = Monday
    buf[0] = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTESTDAYNAME1 + winDay, buf, n) <= 0 || !buf[0]) {
        static const wchar_t* fallback[] = {L"H", L"B", L"T", L"N", L"S", L"B", L"C"};
        wcsncpy(buf, fallback[winDay], n - 1);
        buf[n - 1] = 0;
    }
}

typedef UINT(WINAPI* PfnWindowDpi)(HWND);
UINT DpiOfWindow(HWND hwnd) {
    static PfnWindowDpi fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) fn = reinterpret_cast<PfnWindowDpi>(GetProcAddress(u, "GetDpiForWindow"));
    }
    return fn ? fn(hwnd) : 96;
}

typedef HRESULT(WINAPI* PfnMonitorDpi)(HMONITOR, int, UINT*, UINT*);


UINT DpiOfRect(const RECT* r) {
    UINT dx = 96, dy = 96;
    HMONITOR mon = MonitorFromRect(r, MONITOR_DEFAULTTONEAREST);
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    if (sh) {
        PfnMonitorDpi fn = reinterpret_cast<PfnMonitorDpi>(GetProcAddress(sh, "GetDpiForMonitor"));
        if (fn) fn(mon, 0, &dx, &dy);
        FreeLibrary(sh);
    }
    return dx ? dx : 96;
}

// ---------------------------------------------------------------- calendar model

void RebuildCells() {
    int first = vnl::Jdn(1, g_viewMonth, g_viewYear);
    int col = (vnl::DayOfWeekSun0(first) - g_firstDaySun0 + 7) % 7;
    int start = first - col;
    for (int i = 0; i < 42; i++) {
        Cell& c = g_cells[i];
        c.jdn = start + i;
        vnl::JdnToDate(c.jdn, c.y, c.m, c.d);
        c.inMonth = (c.m == g_viewMonth && c.y == g_viewYear);
        c.lunar = vnl::ToLunar(c.y, c.m, c.d);
        c.fest = vnl::FindLunarFestival(c.lunar.day, c.lunar.month, c.lunar.leap);
        c.solarName = vnl::FindSolarHoliday(c.d, c.m);
    }
}

void GoToToday() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    g_viewYear = st.wYear;
    g_viewMonth = st.wMonth;
    g_selected = -1;
    g_todayJdn = vnl::Jdn(st.wDay, st.wMonth, st.wYear);
    RebuildCells();
}

void ChangeMonth(int delta) {
    int m = g_viewMonth + delta, y = g_viewYear;
    while (m < 1) { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    if (y < 1900 || y > 2100) return;
    g_viewYear = y;
    g_viewMonth = m;
    g_selected = -1;
    RebuildCells();
}

void LunarLabel(const vnl::Lunar& l, Str& out) {
    out.N(l.day).A(L"/").N(l.month);
    if (l.leap) out.A(L"n");
}

void CanChiYear(int lunarYear, Str& out) {
    out.A(vnl::Can(vnl::YearCan(lunarYear))).A(L" ").A(vnl::Chi(vnl::YearChi(lunarYear)));
}

void CanChiLine(int jdn, const vnl::Lunar& l, Str& out) {
    out.A(L"Ngày ").A(vnl::Can(vnl::DayCan(jdn))).A(L" ").A(vnl::Chi(vnl::DayChi(jdn)));
    out.A(L" tháng ").A(vnl::Can(vnl::MonthCan(l.year, l.month))).A(L" ").A(vnl::Chi(vnl::MonthChi(l.month)));
    if (l.leap) out.A(L" (nhuận)");
    out.A(L" năm ");
    CanChiYear(l.year, out);
}

// ---------------------------------------------------------------- geometry

RECT RectDip(double x, double y, double w, double h) {
    RECT r;
    r.left = S(x);
    r.top = S(y);
    r.right = S(x + w);
    r.bottom = S(y + h);
    return r;
}

RECT CellRect(int idx) {
    int row = idx / 7, col = idx % 7;
    double cw = (g_calendarW - 2 * MARGIN) / 7.0;
    RECT r;
    r.left = S(MARGIN + col * cw);
    r.right = S(MARGIN + (col + 1) * cw);
    r.top = S(GRID_Y + row * CELL_H);
    r.bottom = S(GRID_Y + (row + 1) * CELL_H);
    return r;
}

RECT PrevRect() { return RectDip(g_calendarW - MARGIN - 72, MONTH_Y, 36, MONTH_H); }
RECT NextRect() { return RectDip(g_calendarW - MARGIN - 36, MONTH_Y, 36, MONTH_H); }
RECT FootRect() { return RectDip(0, FOOT_Y, g_calendarW, FOOT_H); }

int HitTest(int x, int y) {
    POINT p{x, y};
    RECT r = PrevRect();
    if (PtInRect(&r, p)) return 100;
    r = NextRect();
    if (PtInRect(&r, p)) return 101;
    r = FootRect();
    if (PtInRect(&r, p)) return 102;
    for (int i = 0; i < 42; i++) {
        r = CellRect(i);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

// ---------------------------------------------------------------- painting (only while visible)

struct Theme {
    COLORREF bg, text, dim, lunar, lunarDim, divider, hover, red, link, border;
};

Theme GetTheme() {
    Theme t;
    if (g_dark) {
        t.bg = RGB(32, 32, 32); t.text = RGB(255, 255, 255); t.dim = RGB(122, 122, 122);
        t.lunar = RGB(196, 196, 196); t.lunarDim = RGB(105, 105, 105); t.divider = RGB(72, 72, 72);
        t.hover = RGB(62, 62, 62); t.red = RGB(255, 106, 106); t.link = RGB(118, 185, 237);
        t.border = RGB(70, 70, 70);
    } else {
        t.bg = RGB(243, 243, 243); t.text = RGB(0, 0, 0); t.dim = RGB(150, 150, 150);
        t.lunar = RGB(85, 85, 85); t.lunarDim = RGB(170, 170, 170); t.divider = RGB(205, 205, 205);
        t.hover = RGB(224, 224, 224); t.red = RGB(205, 32, 32); t.link = RGB(0, 90, 158);
        t.border = RGB(190, 190, 190);
    }
    return t;
}

void DrawStr(HDC dc, HFONT font, COLORREF color, const wchar_t* text, RECT r, UINT format) {
    if (!text || !text[0]) return;
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, format | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void FrameRectColor(HDC dc, const RECT& r, COLORREF c, int width) {
    HBRUSH b = CreateSolidBrush(c);
    for (int i = 0; i < width; i++) {
        RECT q = {r.left + i, r.top + i, r.right - i, r.bottom - i};
        FrameRect(dc, &q, b);
    }
    DeleteObject(b);
}

void DrawChevron(HDC dc, const RECT& r, bool up, COLORREF color) {
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    int a = S(5);
    HPEN pen = CreatePen(PS_SOLID, MaxI(1, S(1.5)), color);
    HGDIOBJ old = SelectObject(dc, pen);
    int dy = up ? 1 : -1;
    MoveToEx(dc, cx - a, cy + dy * a / 2, nullptr);
    LineTo(dc, cx, cy - dy * a / 2);
    LineTo(dc, cx + a + 1, cy + dy * a / 2 + dy);
    SelectObject(dc, old);
    DeleteObject(pen);
}

HFONT MakeFont(int heightDip, int weight, const wchar_t* face) {
    return CreateFontW(-S(heightDip), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void ReleaseResources();

bool AcquireResources(int w, int h) {
    ReleaseResources();
    g_fTime = MakeFont(46, FW_LIGHT, L"Segoe UI Light");
    g_fDate = MakeFont(13, FW_NORMAL, L"Segoe UI");
    g_fMonth = MakeFont(15, FW_NORMAL, L"Segoe UI");
    g_fDay = MakeFont(16, FW_NORMAL, L"Segoe UI");
    g_fLunar = MakeFont(10, FW_NORMAL, L"Segoe UI");
    g_fSmall = MakeFont(12, FW_NORMAL, L"Segoe UI");
    if (!g_fTime || !g_fDate || !g_fMonth || !g_fDay || !g_fLunar || !g_fSmall) {
        ReleaseResources();
        return false;
    }
    HDC screen = GetDC(nullptr);
    if (!screen) { ReleaseResources(); return false; }
    g_memDC = CreateCompatibleDC(screen);
    if (g_memDC) g_memBmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);
    if (!g_memDC || !g_memBmp) { ReleaseResources(); return false; }
    HGDIOBJ old = SelectObject(g_memDC, g_memBmp);
    if (!old || old == HGDI_ERROR) { ReleaseResources(); return false; }
    g_memOld = static_cast<HBITMAP>(old);
    g_bufW = w;
    g_bufH = h;
    return true;
}

void ReleaseResources() {
    HFONT* all[] = {&g_fTime, &g_fDate, &g_fMonth, &g_fDay, &g_fLunar, &g_fSmall};
    for (HFONT* f : all) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    if (g_memDC) {
        if (g_memOld) SelectObject(g_memDC, g_memOld);
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
    if (g_memBmp) {
        DeleteObject(g_memBmp);
        g_memBmp = nullptr;
    }
    g_memOld = nullptr;
    g_bufW = g_bufH = 0;
}

void Render() {
    if (!g_memDC) return;
    HDC dc = g_memDC;
    Theme t = GetTheme();
    RECT all = {0, 0, g_bufW, g_bufH};
    FillRectColor(dc, all, t.bg);
    SetBkMode(dc, TRANSPARENT);

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t buf[128];

    buf[0] = 0;
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, nullptr, buf, 128);
    DrawStr(dc, g_fTime, t.text, buf, RectDip(MARGIN + 2, TIME_Y, g_calendarW - 2 * MARGIN, TIME_H), DT_LEFT | DT_VCENTER);
    FormatDate(now, DATE_LONGDATE, nullptr, buf, 128);
    DrawStr(dc, g_fDate, t.link, buf, RectDip(MARGIN + 2, DATE_Y, g_calendarW - 2 * MARGIN, LINE_H), DT_LEFT | DT_VCENTER);
    Str canChi;
    CanChiLine(g_todayJdn, vnl::ToLunar(now.wYear, now.wMonth, now.wDay), canChi);
    DrawStr(dc, g_fDate, t.link, canChi.s, RectDip(MARGIN + 2, CANCHI_Y, g_calendarW - 2 * MARGIN, LINE_H),
            DT_LEFT | DT_VCENTER);
    FillRectColor(dc, RectDip(0, DIV_Y, g_calendarW, 1), t.divider);

    SYSTEMTIME first = MakeDate(g_viewYear, g_viewMonth, 1);
    FormatDate(first, 0, L"MMMM yyyy", buf, 128);
    DrawStr(dc, g_fMonth, t.text, buf, RectDip(MARGIN + 2, MONTH_Y, g_calendarW - 2 * MARGIN - 80, MONTH_H),
            DT_LEFT | DT_VCENTER);
    RECT pr = PrevRect(), nr = NextRect();
    if (g_hover == 100) FillRectColor(dc, pr, t.hover);
    if (g_hover == 101) FillRectColor(dc, nr, t.hover);
    DrawChevron(dc, pr, true, t.text);
    DrawChevron(dc, nr, false, t.text);

    double cw = (g_calendarW - 2 * MARGIN) / 7.0;
    for (int c = 0; c < 7; c++) {
        WeekdayLetter(c, buf, 16);
        DrawStr(dc, g_fSmall, t.text, buf, RectDip(MARGIN + c * cw, WD_Y, cw, WD_H), DT_CENTER | DT_VCENTER);
    }

    for (int i = 0; i < 42; i++) {
        const Cell& c = g_cells[i];
        RECT r = CellRect(i);
        bool today = (c.jdn == g_todayJdn);
        RECT inner = {r.left + S(1), r.top + S(1), r.right - S(1), r.bottom - S(1)};
        if (today) FillRectColor(dc, inner, g_accent);
        else if (i == g_hover) FillRectColor(dc, inner, t.hover);
        if (i == g_selected) FrameRectColor(dc, inner, g_accent, MaxI(1, S(2)));

        COLORREF dayColor = c.inMonth ? t.text : t.dim;
        COLORREF lunarColor = c.inMonth ? t.lunar : t.lunarDim;
        if (c.fest) {
            lunarColor = t.red;
            if (c.fest->redSolar) dayColor = t.red;
        }
        if (today) dayColor = lunarColor = RGB(255, 255, 255);
        RECT dayRect = {r.left, r.top + S(4), r.right, r.top + S(25)};
        RECT lunarRect = {r.left, r.top + S(26), r.right, r.bottom - S(2)};
        Str num, lab;
        num.N(c.d);
        LunarLabel(c.lunar, lab);
        DrawStr(dc, g_fDay, dayColor, num.s, dayRect, DT_CENTER | DT_VCENTER);
        DrawStr(dc, g_fLunar, lunarColor, lab.s, lunarRect, DT_CENTER | DT_VCENTER);
    }

    // info lines for hovered / selected / today
    int infoIdx = -1;
    if (g_hover >= 0 && g_hover < 42) infoIdx = g_hover;
    else if (g_selected >= 0) infoIdx = g_selected;
    else {
        for (int i = 0; i < 42; i++) {
            if (g_cells[i].jdn == g_todayJdn) { infoIdx = i; break; }
        }
    }
    if (infoIdx >= 0) {
        const Cell& c = g_cells[infoIdx];
        SYSTEMTIME st = MakeDate(c.y, c.m, c.d);
        Str l1, l2;
        FormatDate(st, 0, L"dddd, dd/MM/yyyy", buf, 128);
        l1.A(buf).A(L"  \u2022  ");
        LunarLabel(c.lunar, l1);
        l1.A(L" ");
        CanChiYear(c.lunar.year, l1);
        CanChiLine(c.jdn, c.lunar, l2);
        const wchar_t* l3 = nullptr;
        COLORREF l3Color = t.red;
        if (c.fest) l3 = c.fest->name;
        else if (c.solarName) { l3 = c.solarName; l3Color = t.link; }
        else if (c.lunar.day == 1) { l3 = L"Mùng Một (Sóc)"; l3Color = t.lunar; }
        else if (c.lunar.day == 15) { l3 = L"Ngày Rằm (Vọng)"; l3Color = t.lunar; }
        double lh = INFO_H / 3.0;
        FillRectColor(dc, RectDip(MARGIN, INFO_Y - 2, g_calendarW - 2 * MARGIN, 1), t.divider);
        UINT f = DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS;
        DrawStr(dc, g_fSmall, t.text, l1.s, RectDip(MARGIN + 2, INFO_Y, g_calendarW - 2 * MARGIN, lh), f);
        DrawStr(dc, g_fSmall, t.lunar, l2.s, RectDip(MARGIN + 2, INFO_Y + lh, g_calendarW - 2 * MARGIN, lh), f);
        DrawStr(dc, g_fSmall, l3Color, l3, RectDip(MARGIN + 2, INFO_Y + 2 * lh, g_calendarW - 2 * MARGIN, lh), f);
    }

    RECT foot = FootRect();
    if (g_hover == 102) FillRectColor(dc, foot, t.hover);
    RECT footText = {foot.left + S(MARGIN + 2), foot.top, foot.right - S(MARGIN), foot.bottom};
    DrawStr(dc, g_fSmall, t.link, L"Cài đặt ngày và giờ", footText, DT_LEFT | DT_VCENTER);
    FrameRectColor(dc, all, t.border, 1);
}

void Redraw() {
    if (!g_hwnd || !g_memDC) return;
    Render();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- tray icon

void UpdateTrayIcon(bool add) {
    if (!g_trayEnabled) return;
    if (!g_hwnd) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    vnl::Lunar l = vnl::ToLunar(st.wYear, st.wMonth, st.wDay);
    Str tip;
    tip.A(L"Âm lịch ");
    LunarLabel(l, tip);
    tip.A(L" ");
    CanChiYear(l.year, tip);
    if (!g_trayIcon) {
        int sz = GetSystemMetrics(SM_CXSMICON);
        g_trayIcon = static_cast<HICON>(
            LoadImageW(g_inst, MAKEINTRESOURCEW(ID_ICON), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR));
    }
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_trayIcon;
    wcsncpy(g_nid.szTip, tip.s, 127);
    g_nid.szTip[127] = 0;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

void ArmMidnightTimer() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    UINT ms = ((23 - st.wHour) * 60 + (59 - st.wMinute)) * 60 * 1000 + (60 - st.wSecond) * 1000 -
              st.wMilliseconds + 500;
    SetTimer(g_hwnd, TIMER_MIDNIGHT, ms, nullptr);
}

void TrimMemory() {
    // Windows tự quản lý working set; tài nguyên GDI đã được giải phóng tại ReleaseResources().
}

// ---------------------------------------------------------------- settings in the registry

bool ReadDword(const wchar_t* name, DWORD def) {
    DWORD v = def, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

void WriteDword(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

bool IsAutorun() {
    DWORD type = 0, bytes = 0;
    LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", kRunKeyName,
                          RRF_RT_REG_SZ, &type, nullptr, &bytes);
    return r == ERROR_SUCCESS && bytes > sizeof(wchar_t);
}

bool SetAutorun(bool on) {
    HKEY key = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (r != ERROR_SUCCESS) return false;
    if (on) {
        wchar_t exe[32768]{};
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
        if (!n || n >= sizeof(exe) / sizeof(exe[0])) { RegCloseKey(key); return false; }
        wchar_t value[32780]{};
        if (FAILED(StringCchPrintfW(value, sizeof(value) / sizeof(value[0]), L"\"%s\" --auto", exe))) {
            RegCloseKey(key); return false;
        }
        r = RegSetValueExW(key, kRunKeyName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                           static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, kRunKeyName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS && (on ? IsAutorun() : !IsAutorun());
}

// ---------------------------------------------------------------- show / hide

void PlaceWindow(const RECT* anchor, int w, int h, int& x, int& y, HMONITOR& monOut) {
    RECT a;
    if (anchor) {
        a = *anchor;
    } else {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray || !GetWindowRect(tray, &a)) {
            POINT pt;
            GetCursorPos(&pt);
            a = {pt.x, pt.y, pt.x, pt.y};
        }
    }
    HMONITOR mon = MonitorFromRect(&a, MONITOR_DEFAULTTONEAREST);
    monOut = mon;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    const RECT& mr = mi.rcMonitor;
    int edge = 0;   // 0 bottom, 1 top, 2 left, 3 right
    if (wa.top > mr.top) edge = 1;
    else if (wa.left > mr.left) edge = 2;
    else if (wa.right < mr.right) edge = 3;
    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (edge == 1) { x = clampi(a.right - w, wa.left, wa.right - w); y = wa.top; }
    else if (edge == 2) { x = wa.left; y = clampi(a.bottom - h, wa.top, wa.bottom - h); }
    else if (edge == 3) { x = wa.right - w; y = clampi(a.bottom - h, wa.top, wa.bottom - h); }
    else { x = clampi(a.right - w, wa.left, wa.right - w); y = wa.bottom - h; }
    if (y < wa.top) y = wa.top;
}

void HideCalendar(bool byFocusLoss) {
    if (!g_hwnd || !IsWindowVisible(g_hwnd)) return;
    KillTimer(g_hwnd, TIMER_CLOCK);
    ShowWindow(g_hwnd, SW_HIDE);
    ReleaseResources();
    g_hover = -1;
    g_tracking = false;
    if (byFocusLoss) g_focusLossTick = GetTickCount();
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW);
    TrimMemory();
}

void SendEscape() {
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
}

void ShowCalendar(const RECT* anchor, bool fromFlyout = false, const RECT* nativeRect = nullptr) {
    if (IsWindowVisible(g_hwnd)) return;
    (void)fromFlyout;
    (void)nativeRect;
    GoToToday();
    g_hover = -1;
    RECT probe;
    if (nativeRect) probe = *nativeRect;
    else if (anchor) probe = *anchor;
    else {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray || !GetWindowRect(tray, &probe)) { POINT pt; GetCursorPos(&pt); probe = {pt.x, pt.y, pt.x, pt.y}; }
    }
    g_scale = DpiOfRect(&probe) / 96.0;
    g_calendarW = BASE_W;
    int w = S(g_calendarW), h = S(H), x = 0, y = 0;
    HMONITOR mon = nullptr;
    PlaceWindow(anchor, w, h, x, y, mon);
    if (!AcquireResources(w, h)) return;
    Render();
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = false;
    if (fgThread && fgThread != me) attached = AttachThreadInput(me, fgThread, TRUE) != FALSE;
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
    g_shownTick = GetTickCount();
    SetTimer(g_hwnd, TIMER_CLOCK, 1000, nullptr);
}

void ToggleCalendar(const RECT* anchor) {
    if (IsWindowVisible(g_hwnd)) HideCalendar(false);
    else ShowCalendar(anchor);
}

// ---------------------------------------------------------------- detect the real Windows taskbar clock

struct ClockSearch {
    POINT pt;
    RECT rect;
    bool found;
};

BOOL CALLBACK EnumClockChild(HWND hwnd, LPARAM lp) {
    auto* p = reinterpret_cast<ClockSearch*>(lp);
    wchar_t cls[32]{};
    GetClassNameW(hwnd, cls, 32);
    if (wcscmp(cls, L"TrayClockWClass") == 0 || wcscmp(cls, L"ClockButton") == 0) {
        RECT r;
        if (GetWindowRect(hwnd, &r) && PtInRect(&r, p->pt)) {
            p->rect = r;
            p->found = true;
            return FALSE;
        }
    }
    return TRUE;
}

bool ClockAreaInTaskbar(HWND tb, POINT pt, RECT* out) {
    RECT tr;
    if (!GetWindowRect(tb, &tr) || !PtInRect(&tr, pt)) return false;
    ClockSearch s{pt, {}, false};
    EnumChildWindows(tb, EnumClockChild, reinterpret_cast<LPARAM>(&s));
    if (s.found) { *out = s.rect; return true; }
    return false;
}

bool ClockAreaAt(POINT pt, RECT* out) {
    HWND tb = nullptr;
    while ((tb = FindWindowExW(nullptr, tb, L"Shell_TrayWnd", nullptr)) != nullptr) {
        if (ClockAreaInTaskbar(tb, pt, out)) return true;
    }
    tb = nullptr;
    while ((tb = FindWindowExW(nullptr, tb, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        if (ClockAreaInTaskbar(tb, pt, out)) return true;
    }
    return false;
}


bool IsShellFlyoutProcess(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH]{};
    DWORD len = MAX_PATH;
    bool ok = false;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        ok = _wcsicmp(name, L"ShellExperienceHost.exe") == 0 || _wcsicmp(name, L"ShellHost.exe") == 0;
    }
    CloseHandle(proc);
    return ok;
}

void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || !hwnd || hwnd == g_hwnd) return;
    if (IsWindowVisible(g_hwnd) && GetTickCount() - g_shownTick > 300) {
        HideCalendar(true);
        return;
    }
    if (!g_replaceFlyout) return;
    wchar_t cls[40]{};
    GetClassNameW(hwnd, cls, 40);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0 && wcscmp(cls, L"ControlCenterWindow") != 0) return;
    POINT pt;
    GetCursorPos(&pt);
    RECT clock{};
    if (!ClockAreaAt(pt, &clock)) return;
    if (!IsShellFlyoutProcess(hwnd)) return;

    // Hide Windows calendar first. Our calendar is rendered only after the native flyout is gone.
    ShowWindow(hwnd, SW_HIDE);
    g_pendingClock = clock;
    PostMessageW(g_hwnd, WM_APP_CLOCK_CLICK, 0, 0);
}

// ---------------------------------------------------------------- window procedure

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN, L"Mở lịch");
    AppendMenuW(menu, MF_STRING, ID_MENU_SETTINGS, L"Cấu hình...");
    AppendMenuW(menu, MF_STRING | (g_replaceFlyout ? MF_CHECKED : 0), ID_MENU_REPLACE,
                L"Thay lịch Windows khi bấm đồng hồ");
    AppendMenuW(menu, MF_STRING | (IsAutorun() ? MF_CHECKED : 0), ID_MENU_AUTORUN, L"Chạy cùng Windows");
    AppendMenuW(menu, MF_STRING, ID_MENU_ABOUT, L"Giới thiệu");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Thoát");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == ID_MENU_OPEN) ShowCalendar(nullptr);
    else if (cmd == ID_MENU_SETTINGS) PostMessageW(g_hwnd, WM_APP_SHOW_SETTINGS, 0, 0);
    else if (cmd == ID_MENU_REPLACE) {
        bool enable = !g_replaceFlyout;
        if (enable) {
            HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (!hook) {
                MessageBoxW(g_hwnd, L"Không thể kích hoạt chức năng thay lịch Windows.", L"VN Lunar Calendar",
                    MB_OK | MB_ICONWARNING);
                enable = false;
            } else g_eventHook = hook;
        } else if (g_eventHook) {
            UnhookWinEvent(g_eventHook);
            g_eventHook = nullptr;
            if (g_replacingFlyout) SendEscape();
            g_replacingFlyout = false;
        }
        g_replaceFlyout = enable;
        g_cfgReplace = enable;
        WriteDword(L"ReplaceFlyout", enable ? 1 : 0);
        if (g_settings) InvalidateRect(g_settings, nullptr, TRUE);
    }
    else if (cmd == ID_MENU_AUTORUN) SetAutorun(!IsAutorun());
    else if (cmd == ID_MENU_ABOUT) {
        MessageBoxW(nullptr,
                    L"VN Lunar Calendar 1.0.0.1\n"
                    L"Lịch âm Việt Nam cho đồng hồ taskbar (Windows 10 / 11)\n\n"
                    L"Tác giả: bbkim\n"
                    L"\u00A9 2026 bbkim. All rights reserved.",
                    L"Giới thiệu", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }
    else if (cmd == ID_MENU_EXIT) DestroyWindow(g_hwnd);
}

void DrawCfgText(HDC dc, int x, int y, const wchar_t* text) {
    TextOutW(dc, x, y, text, static_cast<int>(wcslen(text)));
}

void DrawCfgCheck(HDC dc, int x, int y, bool checked) {
    RECT r{x, y, x + 18, y + 18};
    HBRUSH white = CreateSolidBrush(RGB(255,255,255));
    FillRect(dc, &r, white);
    DeleteObject(white);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(20,20,20));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, x, y, x + 19, y + 19);
    if (checked) {
        MoveToEx(dc, x + 3, y + 9, nullptr);
        LineTo(dc, x + 7, y + 15);
        LineTo(dc, x + 16, y + 3);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}


void DrawCfgExit(HDC dc, int x, int y) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(15,15,15));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x, y, nullptr); LineTo(dc, x + 11, y + 11);
    MoveToEx(dc, x + 11, y, nullptr); LineTo(dc, x, y + 11);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawCfgHide(HDC dc, int x, int y) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(15,15,15));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x, y, nullptr); LineTo(dc, x + 9, y + 9);
    MoveToEx(dc, x + 3, y + 9, nullptr); LineTo(dc, x + 9, y + 9);
    LineTo(dc, x + 9, y + 3);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

bool IsCfgHidePoint(HWND hwnd, int x, int y) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    RECT hit{rc.right - 13, rc.bottom - 13, rc.right - 1, rc.bottom - 1};
    POINT pt{x, y};
    return PtInRect(&hit, pt) != FALSE;
}

void ApplyCfgChanges(HWND hwnd) {
    g_replaceFlyout = g_cfgReplace;
    WriteDword(L"ReplaceFlyout", g_replaceFlyout ? 1 : 0);
    g_trayEnabled = g_cfgTray;
    WriteDword(L"TrayEnabled", g_trayEnabled ? 1 : 0);
    if (g_trayEnabled) UpdateTrayIcon(true); else Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_replaceFlyout && !g_eventHook) {
        g_eventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!g_eventHook) {
            g_replaceFlyout = g_cfgReplace = false;
            WriteDword(L"ReplaceFlyout", 0);
            MessageBoxW(hwnd, L"Không thể kích hoạt chức năng thay lịch Windows.", L"VN Lunar Calendar",
                MB_OK | MB_ICONWARNING);
        }
    } else if (!g_replaceFlyout && g_eventHook) {
        UnhookWinEvent(g_eventHook);
        g_eventHook = nullptr;
        if (g_replacingFlyout) SendEscape();
        g_replacingFlyout = false;
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_cfgAutorun = IsAutorun();
            g_cfgReplace = g_replaceFlyout;
            g_cfgTray = g_trayEnabled;
            g_cfgFont = CreateFontW(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
            g_cfgTitleFont = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 250, 122, SWP_NOMOVE | SWP_NOACTIVATE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{}; GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0,0,0));
            HGDIOBJ oldFont = g_cfgFont ? SelectObject(dc, g_cfgFont) : nullptr;
            if (g_cfgTitleFont) SelectObject(dc, g_cfgTitleFont);
            DrawCfgText(dc, 9, 5, L"VN Lunar Calendar");
            if (g_cfgFont) SelectObject(dc, g_cfgFont);
            DrawCfgCheck(dc, 10, 34, g_cfgReplace);
            DrawCfgText(dc, 34, 31, L"Thay lịch Windows");
            DrawCfgCheck(dc, 10, 60, g_cfgAutorun);
            DrawCfgText(dc, 34, 57, L"Chạy cùng Windows");
            DrawCfgCheck(dc, 10, 86, g_cfgTray);
            DrawCfgText(dc, 34, 83, L"Khay hệ thống");
            DrawCfgExit(dc, 229, 6);
            DrawCfgHide(dc, rc.right - 11, rc.bottom - 11);
            if (oldFont) SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (IsCfgHidePoint(hwnd, x, y)) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            } else if (x >= 8 && x <= 232 && y >= 31 && y <= 54) {
                g_cfgReplace = !g_cfgReplace;
                ApplyCfgChanges(hwnd);
            } else if (x >= 8 && x <= 232 && y >= 57 && y <= 80) {
                bool next = !g_cfgAutorun;
                if (SetAutorun(next)) g_cfgAutorun = next;
                else MessageBoxW(hwnd, L"Không thể cập nhật Khởi động cùng Windows. Hãy kiểm tra quyền Registry.",
                    L"VN Lunar Calendar", MB_OK | MB_ICONWARNING);
            } else if (x >= 8 && x <= 232 && y >= 82 && y <= 108) {
                g_cfgTray = !g_cfgTray;
                g_trayEnabled = g_cfgTray;
                WriteDword(L"TrayEnabled", g_trayEnabled ? 1 : 0);
                if (g_trayEnabled) UpdateTrayIcon(true); else Shell_NotifyIconW(NIM_DELETE, &g_nid);
            } else if (x >= 224 && x <= 249 && y >= 0 && y <= 24) {
                if (g_hwnd && IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
                return 0;
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            if (hit == HTCLIENT) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                if (pt.y < 26 && pt.x < 222) return HTCAPTION;
            }
            return hit;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE:
            if (g_hwnd && IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
            return 0;
        case WM_DESTROY:
            g_settings = nullptr;
            if (g_cfgFont) { DeleteObject(g_cfgFont); g_cfgFont = nullptr; }
            if (g_cfgTitleFont) { DeleteObject(g_cfgTitleFont); g_cfgTitleFont = nullptr; }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowSettings() {
    if (g_settings && IsWindow(g_settings)) { ShowWindow(g_settings, SW_SHOW); SetForegroundWindow(g_settings); return; }
    g_settings = CreateWindowExW(WS_EX_TOOLWINDOW, L"VN_LunarCalendarSettings", L"VN Lunar Calendar - Cấu hình",
                                 WS_POPUP | WS_BORDER, CW_USEDEFAULT, CW_USEDEFAULT, 250, 122,
                                 g_hwnd, nullptr, g_inst, nullptr);
    if (!g_settings) return;
    RECT r{}; GetWindowRect(g_settings, &r);
    int x = (GetSystemMetrics(SM_CXSCREEN) - (r.right-r.left))/2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - (r.bottom-r.top))/2;
    SetWindowPos(g_settings, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_SHOW_SETTINGS:
            ShowSettings();
            return 0;
        case WM_APP_CLOCK_CLICK: {
            if (IsWindowVisible(g_hwnd)) HideCalendar(false);
            else ShowCalendar(&g_pendingClock);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (g_memDC) BitBlt(dc, 0, 0, g_bufW, g_bufH, g_memDC, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!g_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                g_tracking = true;
            }
            int hit = HitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
            if (hit != g_hover) {
                g_hover = hit;
                Redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            if (g_hover != -1) {
                g_hover = -1;
                Redraw();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int hit = HitTest(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
            if (hit == 100) ChangeMonth(-1);
            else if (hit == 101) ChangeMonth(1);
            else if (hit == 102) {
                HideCalendar(false);
                ShellExecuteW(nullptr, L"open", L"ms-settings:dateandtime", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            } else if (hit >= 0 && hit < 42) g_selected = hit;
            Redraw();
            return 0;
        }
        case WM_MOUSEWHEEL:
            ChangeMonth(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            Redraw();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) HideCalendar(false);
            else if (wp == VK_PRIOR || wp == VK_UP || wp == VK_LEFT) { ChangeMonth(-1); Redraw(); }
            else if (wp == VK_NEXT || wp == VK_DOWN || wp == VK_RIGHT) { ChangeMonth(1); Redraw(); }
            else if (wp == VK_HOME) { GoToToday(); Redraw(); }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) HideCalendar(true);
            return 0;
        case WM_DPICHANGED:
            return 0;   // size is recomputed on every show
        case WM_TIMER:
            if (wp == TIMER_CLOCK) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                int jdn = vnl::Jdn(st.wDay, st.wMonth, st.wYear);
                if (jdn != g_todayJdn) { g_todayJdn = jdn; RebuildCells(); }
                Redraw();
            } else if (wp == TIMER_MIDNIGHT) {
                KillTimer(hwnd, TIMER_MIDNIGHT);
                UpdateTrayIcon(false);
                ArmMidnightTimer();
            }
            return 0;
        case WM_TIMECHANGE:
            KillTimer(hwnd, TIMER_MIDNIGHT);
            UpdateTrayIcon(false);
            ArmMidnightTimer();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) {
                KillTimer(hwnd, TIMER_MIDNIGHT);
                UpdateTrayIcon(false);
                ArmMidnightTimer();
            }
            return TRUE;
        case WM_HOTKEY:
            ToggleCalendar(nullptr);
            return 0;
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP) ToggleCalendar(nullptr);
            else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
            return 0;
        case WM_DESTROY:
            if (g_settings) DestroyWindow(g_settings);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_trayIcon) DestroyIcon(g_trayIcon);
            ReleaseResources();
            PostQuitMessage(0);
            return 0;
        default:
            if (g_taskbarCreated && msg == g_taskbarCreated) {
                UpdateTrayIcon(true);   // explorer restarted
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

typedef BOOL(WINAPI* PfnSetDpiContext)(HANDLE);
typedef HRESULT(WINAPI* PfnDwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);

void EnableDpiAwareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PfnSetDpiContext fn = u ? reinterpret_cast<PfnSetDpiContext>(GetProcAddress(u, "SetProcessDpiAwarenessContext")) : nullptr;
    if (fn && fn(reinterpret_cast<HANDLE>(-4))) return;   // per-monitor v2
    SetProcessDPIAware();
}

void RoundCorners(HWND hwnd) {   // Windows 11 only, ignored elsewhere
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    PfnDwmSetAttr fn = reinterpret_cast<PfnDwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn) {
        DWORD pref = 2;   // DWMWCP_ROUND
        fn(hwnd, 33, &pref, sizeof(pref));
    }
    FreeLibrary(dwm);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    bool autoStart = cmdLine && wcsstr(cmdLine, L"--auto") != nullptr;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kClassName, nullptr);
        if (existing && !autoStart) PostMessageW(existing, WM_APP_SHOW_SETTINGS, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    g_inst = inst;
    EnableDpiAwareness();
    g_replaceFlyout = ReadDword(L"ReplaceFlyout", 1);
    g_trayEnabled = ReadDword(L"TrayEnabled", 1);
    LoadSystemTheme();
    LoadLocaleInfo();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(ID_ICON));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { CloseHandle(mutex); return 1; }
    WNDCLASSEXW swc{}; swc.cbSize = sizeof(swc); swc.lpfnWndProc = SettingsProc; swc.hInstance = inst;
    swc.hCursor = LoadCursorW(nullptr, IDC_ARROW); swc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    swc.lpszClassName = L"VN_LunarCalendarSettings";
    if (!RegisterClassExW(&swc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { CloseHandle(mutex); return 1; }

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"VN Lunar Calendar", WS_POPUP, 0, 0, 100,
                             100, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { CloseHandle(mutex); return 1; }
    RoundCorners(g_hwnd);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_trayEnabled) UpdateTrayIcon(true);
    ArmMidnightTimer();
    RegisterHotKey(g_hwnd, 1, MOD_WIN, 'N');
    if (g_replaceFlyout) {
        g_eventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
            ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!g_eventHook) {
            g_replaceFlyout = false;
            WriteDword(L"ReplaceFlyout", 0);
        }
    }
    if (!autoStart) ShowSettings();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterHotKey(g_hwnd, 1);
    if (g_eventHook) { UnhookWinEvent(g_eventHook); g_eventHook = nullptr; }
    CloseHandle(mutex);
    return 0;
}
