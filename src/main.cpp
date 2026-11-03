// VN_LunarCalendar - Vietnamese lunar calendar popup for the Windows taskbar clock (Win 10 / Win 11).
// One small exe: pure Win32 + GDI, no DLL injection, no mouse hook, no third-party code, no C++ runtime library.
// Idle cost: a hidden window + a WinEvent hook that only fires when the foreground window changes.
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
#include <shellapi.h>
#include <uiautomation.h>
#include <cwchar>
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
constexpr UINT_PTR TIMER_CLOCK = 1;
constexpr UINT_PTR TIMER_MIDNIGHT = 2;
constexpr int ID_MENU_OPEN = 1001;
constexpr int ID_MENU_REPLACE = 1002;
constexpr int ID_MENU_AUTORUN = 1003;
constexpr int ID_MENU_ABOUT = 1005;
constexpr int ID_MENU_EXIT = 1004;
constexpr int ID_ICON = 1;   // resource.rc

// ---- layout in DIP (96 dpi) ----
constexpr double W = 344, MARGIN = 14;
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
HWINEVENTHOOK g_eventHook = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreated = 0;
HICON g_trayIcon = nullptr;
bool g_replaceFlyout = true;
DWORD g_focusLossTick = 0;
DWORD g_shownTick = 0;

double g_scale = 1.0;
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

bool UiaStringEquals(BSTR s, const wchar_t* v) { return s && v && wcscmp(s, v) == 0; }
bool UiaStringStarts(BSTR s, const wchar_t* v) {
    if (!s || !v) return false;
    size_t n = wcslen(v);
    return wcsncmp(s, v, n) == 0;
}
bool UiaStringContains(BSTR s, const wchar_t* v) { return s && v && wcsstr(s, v) != nullptr; }

bool IsClockUiaElement(IUIAutomationElement* e, RECT* out) {
    if (!e) return false;
    BSTR cls = nullptr, aid = nullptr, name = nullptr;
    e->get_CurrentClassName(&cls);
    e->get_CurrentAutomationId(&aid);
    e->get_CurrentName(&name);
    bool trayIcon = UiaStringEquals(aid, L"SystemTrayIcon") &&
                    (UiaStringEquals(cls, L"SystemTray.IconView") || UiaStringContains(cls, L"SystemTray.IconView"));
    bool namedClock = UiaStringStarts(name, L"Clock ");
    if (!namedClock && trayIcon) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t timeBuf[80]{}, dateBuf[80]{};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, timeBuf, 80);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, dateBuf, 80, nullptr);
        namedClock = (timeBuf[0] && UiaStringContains(name, timeBuf)) || (dateBuf[0] && UiaStringContains(name, dateBuf));
    }
    bool ok = trayIcon && namedClock;
    if (ok && out) e->get_CurrentBoundingRectangle(out);
    if (cls) SysFreeString(cls);
    if (aid) SysFreeString(aid);
    if (name) SysFreeString(name);
    return ok;
}

bool ClockAreaViaUIA(POINT pt, RECT* out) {
    IUIAutomation* ui = nullptr;
    IUIAutomationElement* hit = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    bool found = false;
    CLSID clsidUIAutomation{};
    if (FAILED(CLSIDFromString(L"{FF48DBA4-60EF-4201-AA87-54103EEF594E}", &clsidUIAutomation))) return false;
    if (FAILED(CoCreateInstance(clsidUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui))) || !ui) return false;
    if (SUCCEEDED(ui->ElementFromPoint(pt, &hit)) && hit) {
        if (SUCCEEDED(ui->get_RawViewWalker(&walker)) && walker) {
            IUIAutomationElement* cur = hit;
            hit = nullptr;
            for (int i = 0; cur && i < 12; ++i) {
                if (IsClockUiaElement(cur, out)) { found = true; cur->Release(); cur = nullptr; break; }
                IUIAutomationElement* parent = nullptr;
                if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) { cur->Release(); cur = nullptr; break; }
                cur->Release();
                cur = parent;
            }
            if (cur) cur->Release();
        }
    }
    if (hit) hit->Release();
    if (walker) walker->Release();
    if (ui) ui->Release();
    return found;
}

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
    double cw = (W - 2 * MARGIN) / 7.0;
    RECT r;
    r.left = S(MARGIN + col * cw);
    r.right = S(MARGIN + (col + 1) * cw);
    r.top = S(GRID_Y + row * CELL_H);
    r.bottom = S(GRID_Y + (row + 1) * CELL_H);
    return r;
}

RECT PrevRect() { return RectDip(W - MARGIN - 72, MONTH_Y, 36, MONTH_H); }
RECT NextRect() { return RectDip(W - MARGIN - 36, MONTH_Y, 36, MONTH_H); }
RECT FootRect() { return RectDip(0, FOOT_Y, W, FOOT_H); }

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

void AcquireResources(int w, int h) {
    g_fTime = MakeFont(46, FW_LIGHT, L"Segoe UI Light");
    g_fDate = MakeFont(13, FW_NORMAL, L"Segoe UI");
    g_fMonth = MakeFont(15, FW_NORMAL, L"Segoe UI");
    g_fDay = MakeFont(16, FW_NORMAL, L"Segoe UI");
    g_fLunar = MakeFont(10, FW_NORMAL, L"Segoe UI");
    g_fSmall = MakeFont(12, FW_NORMAL, L"Segoe UI");
    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    g_memBmp = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);
    g_memOld = static_cast<HBITMAP>(SelectObject(g_memDC, g_memBmp));
    g_bufW = w;
    g_bufH = h;
}

void ReleaseResources() {
    HFONT* all[] = {&g_fTime, &g_fDate, &g_fMonth, &g_fDay, &g_fLunar, &g_fSmall};
    for (HFONT* f : all) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    if (g_memDC) {
        SelectObject(g_memDC, g_memOld);
        DeleteDC(g_memDC);
        g_memDC = nullptr;
    }
    if (g_memBmp) {
        DeleteObject(g_memBmp);
        g_memBmp = nullptr;
    }
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
    DrawStr(dc, g_fTime, t.text, buf, RectDip(MARGIN + 2, TIME_Y, W - 2 * MARGIN, TIME_H), DT_LEFT | DT_VCENTER);
    FormatDate(now, DATE_LONGDATE, nullptr, buf, 128);
    DrawStr(dc, g_fDate, t.link, buf, RectDip(MARGIN + 2, DATE_Y, W - 2 * MARGIN, LINE_H), DT_LEFT | DT_VCENTER);
    Str canChi;
    CanChiLine(g_todayJdn, vnl::ToLunar(now.wYear, now.wMonth, now.wDay), canChi);
    DrawStr(dc, g_fDate, t.link, canChi.s, RectDip(MARGIN + 2, CANCHI_Y, W - 2 * MARGIN, LINE_H),
            DT_LEFT | DT_VCENTER);
    FillRectColor(dc, RectDip(0, DIV_Y, W, 1), t.divider);

    SYSTEMTIME first = MakeDate(g_viewYear, g_viewMonth, 1);
    FormatDate(first, 0, L"MMMM yyyy", buf, 128);
    DrawStr(dc, g_fMonth, t.text, buf, RectDip(MARGIN + 2, MONTH_Y, W - 2 * MARGIN - 80, MONTH_H),
            DT_LEFT | DT_VCENTER);
    RECT pr = PrevRect(), nr = NextRect();
    if (g_hover == 100) FillRectColor(dc, pr, t.hover);
    if (g_hover == 101) FillRectColor(dc, nr, t.hover);
    DrawChevron(dc, pr, true, t.text);
    DrawChevron(dc, nr, false, t.text);

    double cw = (W - 2 * MARGIN) / 7.0;
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
        FillRectColor(dc, RectDip(MARGIN, INFO_Y - 2, W - 2 * MARGIN, 1), t.divider);
        UINT f = DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS;
        DrawStr(dc, g_fSmall, t.text, l1.s, RectDip(MARGIN + 2, INFO_Y, W - 2 * MARGIN, lh), f);
        DrawStr(dc, g_fSmall, t.lunar, l2.s, RectDip(MARGIN + 2, INFO_Y + lh, W - 2 * MARGIN, lh), f);
        DrawStr(dc, g_fSmall, l3Color, l3, RectDip(MARGIN + 2, INFO_Y + 2 * lh, W - 2 * MARGIN, lh), f);
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
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
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
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", kRunKeyName,
                        RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutorun(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE,
                      &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH + 4]{};
        path[0] = L'"';
        GetModuleFileNameW(nullptr, path + 1, MAX_PATH);
        wcscat(path, L"\"");
        RegSetValueExW(key, kRunKeyName, 0, REG_SZ, reinterpret_cast<const BYTE*>(path),
                       static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunKeyName);
    }
    RegCloseKey(key);
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
    if (byFocusLoss) g_focusLossTick = GetTickCount();
    TrimMemory();
}

void SendEscape() {
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
}

void ShowCalendar(const RECT* anchor, bool fromFlyout = false) {
    if (IsWindowVisible(g_hwnd)) return;
    LoadSystemTheme();
    LoadLocaleInfo();
    GoToToday();
    g_hover = -1;

    RECT probe;
    if (anchor) probe = *anchor;
    else {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray || !GetWindowRect(tray, &probe)) { POINT pt; GetCursorPos(&pt); probe = {pt.x, pt.y, pt.x, pt.y}; }
    }
    g_scale = DpiOfRect(&probe) / 96.0;
    int w = S(W), h = S(H), x = 0, y = 0;
    HMONITOR mon = nullptr;
    PlaceWindow(anchor, w, h, x, y, mon);

    AcquireResources(w, h);
    Render();   // fully painted before the window becomes visible: no blank flash
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);

    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = false;
    if (fgThread && fgThread != me) attached = AttachThreadInput(me, fgThread, TRUE) != FALSE;
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
    if (fromFlyout && GetForegroundWindow() != g_hwnd) SendEscape();   // could not take focus: close the Windows flyout underneath
    g_shownTick = GetTickCount();
    SetTimer(g_hwnd, TIMER_CLOCK, 1000, nullptr);
}

void ToggleCalendar(const RECT* anchor) {
    if (IsWindowVisible(g_hwnd)) HideCalendar(false);
    else ShowCalendar(anchor);
}

// ---------------------------------------------------------------- detect the Windows clock flyout

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
    if (ClockAreaViaUIA(pt, out)) return true;
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

    // any other window taking the foreground closes our popup
    // (ignore the short burst of foreground changes while the Windows flyout is closing right after we opened)
    if (IsWindowVisible(g_hwnd) && GetTickCount() - g_shownTick > 300) HideCalendar(true);
    if (!g_replaceFlyout) return;

    wchar_t cls[40]{};
    GetClassNameW(hwnd, cls, 40);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0 && wcscmp(cls, L"ControlCenterWindow") != 0) return;

    POINT pt;
    GetCursorPos(&pt);
    RECT clock;
    if (!ClockAreaAt(pt, &clock)) return;   // opened by keyboard / other flyout: leave it alone
    if (!IsShellFlyoutProcess(hwnd)) return;

    if (GetTickCount() - g_focusLossTick < 500) {
        SendEscape();   // second click on the clock while our popup was open = close both
        return;
    }
    ShowCalendar(&clock, true);
}

// ---------------------------------------------------------------- window procedure

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN, L"Mở lịch");
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
    else if (cmd == ID_MENU_REPLACE) { g_replaceFlyout = !g_replaceFlyout; WriteDword(L"ReplaceFlyout", g_replaceFlyout); }
    else if (cmd == ID_MENU_AUTORUN) SetAutorun(!IsAutorun());
    else if (cmd == ID_MENU_ABOUT) {
        MessageBoxW(nullptr,
                    L"VN Lunar Calendar 1.0\n"
                    L"Lịch âm Việt Nam cho đồng hồ taskbar (Windows 10 / 11)\n\n"
                    L"Tác giả: bbkim\n"
                    L"\u00A9 2026 bbkim. All rights reserved.",
                    L"Giới thiệu", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }
    else if (cmd == ID_MENU_EXIT) DestroyWindow(g_hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
            if (g_eventHook) UnhookWinEvent(g_eventHook);
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

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    EnableDpiAwareness();
    g_replaceFlyout = ReadDword(L"ReplaceFlyout", 1);
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
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"VN Lunar Calendar", WS_POPUP, 0, 0, 100,
                             100, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    RoundCorners(g_hwnd);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    UpdateTrayIcon(true);
    ArmMidnightTimer();
    RegisterHotKey(g_hwnd, 1, MOD_CONTROL | MOD_ALT, 'L');   // Ctrl+Alt+L (silently ignored if already taken)
    g_eventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundProc, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    TrimMemory();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterHotKey(g_hwnd, 1);
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}
