// UiWindow.cpp — the native window, drawn entirely with GDI+.
//
// Everything is painted into an offscreen 32bpp bitmap and blitted once per
// WM_PAINT. That is what keeps the UI free of flicker without WS_EX_COMPOSITED,
// and it also means every control is just a rounded rectangle plus a hit-test
// region -- no owner-drawn Win32 control classes to fight with.
//
// Visual language: a settings window in the macOS Ventura sense -- a tinted
// sidebar of icon+label rows, a white page, grouped rounded cards, one system
// accent colour, and controls (switch / popup / slider) that look like the
// platform ones instead of like a dashboard.
//
// Layout happens during paint() and is pushed into hits_, which the mouse
// handlers read back: a click can only ever land on something that was drawn.

#include "UiWindow.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

// ------------------------------------------------------------------ palette

// Two full palettes. The window follows the system app theme, so every colour
// has to exist twice; there is no "tint one dark colour and hope" here.
struct Palette {
    ARGB page;      // content background
    ARGB side;      // sidebar background
    ARGB card;      // grouped card fill
    ARGB cardHover;
    ARGB sep;       // row separator
    ARGB text;
    ARGB text2;
    ARGB text3;
    ARGB accent;
    ARGB selText;   // text on the accent-filled sidebar row
    ARGB switchOff;
    ARGB knob;
    ARGB warn;
    ARGB err;
    ARGB ok;
};

const Palette kLight = {
    /*page*/      0xFFFFFFFF,
    /*side*/      0xFFF2F2F4,
    /*card*/      0xFFF5F5F7,
    /*cardHover*/ 0xFFEBEBEF,
    /*sep*/       0xFFE3E3E6,
    /*text*/      0xFF1D1D1F,
    /*text2*/     0xFF6E6E73,
    /*text3*/     0xFF9A9AA0,
    /*accent*/    0xFF0A7AFF,
    /*selText*/   0xFFFFFFFF,
    /*switchOff*/ 0xFFD6D6DB,
    /*knob*/      0xFFFFFFFF,
    /*warn*/      0xFFB25000,
    /*err*/       0xFFD70015,
    /*ok*/        0xFF248A3D,
};

const Palette kDark = {
    /*page*/      0xFF1E1E1E,
    /*side*/      0xFF262628,
    /*card*/      0xFF2C2C2E,
    /*cardHover*/ 0xFF37373A,
    /*sep*/       0xFF3A3A3C,
    /*text*/      0xFFF2F2F4,
    /*text2*/     0xFFA1A1A6,
    /*text3*/     0xFF77777C,
    /*accent*/    0xFF0A84FF,
    /*selText*/   0xFFFFFFFF,
    /*switchOff*/ 0xFF48484A,
    /*knob*/      0xFFFAFAFA,
    /*warn*/      0xFFFFB020,
    /*err*/       0xFFFF5C5C,
    /*ok*/        0xFF32D583,
};

// Active palette for this paint pass.
const Palette* P = &kLight;

constexpr wchar_t kDropClass[] = L"TpGesturesDropDown";
constexpr wchar_t kAppName[]   = L"Fuck-Windows-Touchpad";
constexpr wchar_t kVersion[]   = L"1.1.0";

constexpr UINT kTrayMsg   = WM_APP + 11;
constexpr UINT kShowMsg   = kUiRestoreMessage;
constexpr UINT_PTR kTimerTick = 1;

// Unscaled design metrics (96 dpi). Everything is multiplied by the window's
// DPI scale before it reaches GDI+.
constexpr int kW      = 1020;
constexpr int kH      = 700;
constexpr int kSideW  = 216;
constexpr int kPadX   = 30;                  // page left margin
constexpr int kContentX = kSideW + kPadX;

// Row metrics.
constexpr int kRowH      = 44;
constexpr int kSliderH   = 52;
constexpr int kGroupHead = 30;
constexpr int kGroupGap  = 18;
constexpr int kCardRad   = 10;

// -------------------------------------------------------------- action list

struct ActionLabel {
    ActionId       id;
    const wchar_t* text;
};

const ActionLabel kActions[] = {
    {ActionId::None,        L"无操作"},
    {ActionId::Drag,        L"拖动窗口"},
    {ActionId::SwapLeft,    L"与左侧窗口互换"},
    {ActionId::SwapRight,   L"与右侧窗口互换"},
    {ActionId::Maximize,    L"最大化"},
    {ActionId::Minimize,    L"最小化"},
    {ActionId::Close,       L"关闭窗口"},
    {ActionId::Fullscreen,  L"全屏"},
    {ActionId::TaskView,    L"任务视图"},
    {ActionId::ShowDesktop, L"显示桌面"},
    {ActionId::NextWindow,  L"切换窗口"},
    {ActionId::PrevWindow,  L"上一个窗口"},
    {ActionId::NextTab,     L"下一个标签页"},
    {ActionId::PrevTab,     L"上一个标签页"},
    {ActionId::NextDesktop, L"下一个桌面"},
    {ActionId::PrevDesktop, L"上一个桌面"},
    {ActionId::LockScreen,  L"锁定屏幕"},
    {ActionId::Screenshot,  L"截图"},
    {ActionId::TaskManager, L"任务管理器"},
    {ActionId::VolumeUp,    L"音量增大"},
    {ActionId::VolumeDown,  L"音量减小"},
    {ActionId::Mute,        L"静音"},
    {ActionId::MediaPlay,   L"播放 / 暂停"},
    {ActionId::MediaNext,   L"下一首"},
    {ActionId::MediaPrev,   L"上一首"},
    {ActionId::Copy,        L"复制"},
    {ActionId::Paste,       L"粘贴"},
    {ActionId::Cut,         L"剪切"},
    {ActionId::Undo,        L"撤销"},
    {ActionId::SelectAll,   L"全选"},
    {ActionId::Custom,      L"自定义快捷键…"},
    {ActionId::Launch,      L"打开程序…"},
};
constexpr int kActionCount =
    static_cast<int>(sizeof(kActions) / sizeof(kActions[0]));

int actionIndex(ActionId a) {
    for (int i = 0; i < kActionCount; ++i)
        if (kActions[i].id == a) return i;
    return 0;
}

const wchar_t* actionText(ActionId a) { return kActions[actionIndex(a)].text; }

// The menu is grouped, because a flat list of thirty actions is a wall. Each
// group is a caption row the user cannot click.
struct ActionGroup {
    const wchar_t*        title;
    std::vector<ActionId> items;
};

std::vector<ActionGroup> actionGroups(bool allowDrag) {
    std::vector<ActionGroup> g;
    g.push_back({L"常规", {ActionId::None}});
    ActionGroup window{L"窗口", {ActionId::Maximize, ActionId::Minimize,
                                 ActionId::Fullscreen, ActionId::Close,
                                 ActionId::SwapLeft, ActionId::SwapRight}};
    if (allowDrag) window.items.insert(window.items.begin(), ActionId::Drag);
    g.push_back(window);
    g.push_back({L"切换", {ActionId::TaskView, ActionId::ShowDesktop,
                           ActionId::NextWindow, ActionId::PrevWindow,
                           ActionId::NextTab, ActionId::PrevTab,
                           ActionId::NextDesktop, ActionId::PrevDesktop}});
    g.push_back({L"系统", {ActionId::LockScreen, ActionId::Screenshot,
                           ActionId::TaskManager}});
    g.push_back({L"声音与媒体", {ActionId::VolumeUp, ActionId::VolumeDown,
                                 ActionId::Mute, ActionId::MediaPlay,
                                 ActionId::MediaNext, ActionId::MediaPrev}});
    g.push_back({L"编辑", {ActionId::Copy, ActionId::Paste, ActionId::Cut,
                           ActionId::Undo, ActionId::SelectAll}});
    g.push_back({L"自定义", {ActionId::Custom, ActionId::Launch}});
    return g;
}

// ---------------------------------------------------------------- font cache

constexpr wchar_t kFamily[] = L"Microsoft YaHei UI";

Font* fontFor(float px, bool bold) {
    static std::vector<std::pair<int, Font*>> cache;
    const int key = static_cast<int>(lround(px * 4.f)) * 2 + (bold ? 1 : 0);
    for (const auto& kv : cache)
        if (kv.first == key) return kv.second;
    Font* f = new Font(kFamily, px, bold ? FontStyleBold : FontStyleRegular,
                       UnitPixel);
    cache.emplace_back(key, f);
    return f;
}

StringFormat* fmtFor(int align) {
    static StringFormat f[3];
    static bool init = false;
    if (!init) {
        init = true;
        for (int i = 0; i < 3; ++i) {
            f[i].SetLineAlignment(StringAlignmentCenter);
            f[i].SetAlignment(i == 0   ? StringAlignmentNear
                              : i == 1 ? StringAlignmentCenter
                                       : StringAlignmentFar);
            f[i].SetTrimming(StringTrimmingEllipsisCharacter);
            f[i].SetFormatFlags(StringFormatFlagsNoWrap);
        }
    }
    return &f[align < 0 ? 0 : (align > 2 ? 2 : align)];
}

// --------------------------------------------------------------- drawing kit

struct Draw {
    Graphics* g = nullptr;
    float     s = 1.f;

    static void addRound(GraphicsPath& p, const RECT& r, float rad) {
        const REAL x = (REAL)r.left + .5f, y = (REAL)r.top + .5f;
        const REAL w = (REAL)(r.right - r.left) - 1.f;
        const REAL h = (REAL)(r.bottom - r.top) - 1.f;
        const REAL d = std::min({rad * 2.f, w, h});
        p.AddArc(x, y, d, d, 180.f, 90.f);
        p.AddArc(x + w - d, y, d, d, 270.f, 90.f);
        p.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
        p.AddArc(x, y + h - d, d, d, 90.f, 90.f);
        p.CloseFigure();
    }

    void fillRound(const RECT& r, float rad, ARGB color) {
        GraphicsPath p;
        addRound(p, r, rad);
        SolidBrush b{Color(color)};
        g->FillPath(&b, &p);
    }

    void strokeRound(const RECT& r, float rad, ARGB color, float w = 1.f) {
        GraphicsPath p;
        addRound(p, r, rad);
        Pen pen(Color(color), w);
        g->DrawPath(&pen, &p);
    }

    void fillRect(const RECT& r, ARGB color) {
        SolidBrush b{Color(color)};
        g->FillRectangle(&b, RectF((REAL)r.left, (REAL)r.top,
                                   (REAL)(r.right - r.left),
                                   (REAL)(r.bottom - r.top)));
    }

    void circle(float cx, float cy, float rad, ARGB color) {
        SolidBrush b{Color(color)};
        g->FillEllipse(&b, cx - rad, cy - rad, rad * 2, rad * 2);
    }

    void ring(float cx, float cy, float rad, ARGB color, float w) {
        Pen pen(Color(color), w);
        g->DrawEllipse(&pen, cx - rad, cy - rad, rad * 2, rad * 2);
    }

    void hline(int x1, int x2, int y, ARGB color) {
        Pen pen(Color(color), 1.f);
        g->DrawLine(&pen, (REAL)x1, (REAL)y + .5f, (REAL)x2, (REAL)y + .5f);
    }

    void line(float x1, float y1, float x2, float y2, ARGB color, float w) {
        Pen pen(Color(color), w);
        g->DrawLine(&pen, x1, y1, x2, y2);
    }

    // `size` is the unscaled pixel height; it is multiplied by the DPI scale.
    void txt(const wchar_t* t, RECT r, float size, ARGB color, int align = 0,
             bool bold = false) {
        if (!t || !*t) return;
        SolidBrush b{Color(color)};
        Font* f = fontFor(size * s, bold);
        g->DrawString(t, -1, f,
                      RectF((REAL)r.left, (REAL)r.top,
                            (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)),
                      fmtFor(align), &b);
    }

    void txt(const std::wstring& t, RECT r, float size, ARGB color,
             int align = 0, bool bold = false) {
        txt(t.c_str(), r, size, color, align, bold);
    }
};

std::wstring wfmt(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    return buf;
}

// The slant tolerance is stored in the config as a |dy|/|dx| ratio, because
// that is what the recogniser compares. A ratio means nothing to a human; the
// angle it implies does, so the slider runs in degrees and the ratio is derived
// on write. Drag right = looser tolerance = wider angle.
constexpr float kSlantMinDeg = 14.f;   // tightest: ratio ~4.0
constexpr float kSlantMaxDeg = 45.f;   // loosest: ratio 1.0

float degOfDominance(float ratio) {
    const double r = std::max(0.2f, ratio);
    return (float)(std::atan(1.0 / r) * 180.0 / 3.14159265358979323846);
}

float dominanceOfDeg(float deg) {
    const double d = (double)std::min(89.f, std::max(1.f, deg));
    return (float)(1.0 / std::tan(d * 3.14159265358979323846 / 180.0));
}

// Every string that crosses a module boundary in this project is UTF-8; the
// log ring buffer and the action results are no exception.
std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

// macOS-style switch: 40x24 pill, 19 px knob, accent when on.
void drawSwitch(Draw& d, const RECT& r, bool on, bool dim) {
    const float rad = (float)(r.bottom - r.top) * .5f;
    const ARGB fill = on ? (dim ? 0x660A7AFF : P->accent) : P->switchOff;
    d.fillRound(r, rad, fill);

    const float k = rad - d.s * 2.5f;
    const float cx = on ? (float)r.right - rad : (float)r.left + rad;
    const float cy = (float)(r.top + r.bottom) * .5f;

    // A soft drop shadow under the knob is what makes it read as a physical
    // control rather than a dot on a pill.
    d.circle(cx, cy + d.s * .6f, k + d.s * .6f, 0x22000000);
    d.circle(cx, cy, k, P->knob);
}

void drawChevron(Draw& d, float cx, float cy, ARGB color) {
    const float w = 3.2f * d.s, h = 4.6f * d.s;
    d.line(cx - w * .5f, cy - h, cx + w * .5f, cy, color, 1.4f * d.s);
    d.line(cx + w * .5f, cy, cx - w * .5f, cy + h, color, 1.4f * d.s);
}

void drawSlider(Draw& d, const RECT& track, float t, bool dim) {
    t = std::min(1.f, std::max(0.f, t));
    const float cy = (track.top + track.bottom) * .5f;
    const float rad = (track.bottom - track.top) * .5f;

    d.fillRound(track, rad, dim ? 0x33FFFFFF : P->switchOff);

    const int splitX =
        track.left + (int)lround((float)(track.right - track.left) * t);
    if (splitX > track.left + 1) {
        RECT fill = {track.left, track.top, splitX, track.bottom};
        d.fillRound(fill, rad, dim ? 0x660A7AFF : P->accent);
    }

    const float kr = 9.f * d.s;
    d.circle((float)splitX, cy + d.s, kr + d.s, 0x26000000);
    d.circle((float)splitX, cy, kr, P->knob);
    d.ring((float)splitX, cy, kr, dim ? 0x33000000 : 0x22000000, 1.f);
}

std::wstring humanCount(uint64_t v) {
    if (v < 10000) return std::to_wstring(v);
    return wfmt(L"%.1f 万", (double)v / 10000.0);
}

std::wstring humanDuration(uint64_t ms) {
    const uint64_t sec = ms / 1000;
    const uint64_t h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
    if (h > 0)
        return wfmt(L"%llu 小时 %llu 分", (unsigned long long)h,
                    (unsigned long long)m);
    if (m > 0)
        return wfmt(L"%llu 分 %llu 秒", (unsigned long long)m,
                    (unsigned long long)s);
    return wfmt(L"%llu 秒", (unsigned long long)s);
}

std::wstring logClock(uint64_t atMs) {
    const uint64_t nowTick = GetTickCount64();
    const uint64_t delta = nowTick > atMs ? nowTick - atMs : 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart > delta * 10000ULL) u.QuadPart -= delta * 10000ULL;
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    return wfmt(L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
}

ARGB kindColor(const std::string& kind) {
    if (kind == "warn") return P->warn;
    if (kind == "error") return P->err;
    if (kind == "gesture") return P->accent;
    if (kind == "drag") return P->ok;
    return P->text3;
}

float queryScale(HWND h) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = nullptr;
    static bool init = false;
    if (!init) {
        init = true;
        if (HMODULE u = GetModuleHandleW(L"user32.dll"))
            fn = reinterpret_cast<Fn>(
                reinterpret_cast<void*>(GetProcAddress(u, "GetDpiForWindow")));
    }
    if (fn) {
        const UINT d = fn(h);
        if (d >= 72 && d <= 480) return d / 96.f;
    }
    HDC dc = GetDC(nullptr);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi / 96.f : 1.f;
}

// Follows Settings > Personalization > Colors. Missing key means "light",
// which is also what a fresh Windows install reports.
bool systemUsesLightTheme() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) != ERROR_SUCCESS)
        return true;

    DWORD v = 1, cb = sizeof(v), type = 0;
    const bool ok = RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&v), &cb) ==
                        ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(k);
    return ok ? v != 0 : true;
}

// Resolves the effective theme: an explicit choice in the config wins, 0 means
// "ask Windows".
bool resolveLight(const AppConfig* c) {
    if (c) {
        if (c->theme == 1) return true;
        if (c->theme == 2) return false;
    }
    return systemUsesLightTheme();
}

// Window icon, drawn at runtime so the build needs no .ico resource compiler.
HICON buildIcon(int size) {    BITMAPV5HEADER bi = {};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = size;
    bi.bV5Height      = -size;
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, color);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));

        const REAL e = (REAL)size;
        GraphicsPath body;
        const REAL r = e * .26f;
        body.AddArc(0.f, 0.f, r * 2, r * 2, 180.f, 90.f);
        body.AddArc(e - r * 2, 0.f, r * 2, r * 2, 270.f, 90.f);
        body.AddArc(e - r * 2, e - r * 2, r * 2, r * 2, 0.f, 90.f);
        body.AddArc(0.f, e - r * 2, r * 2, r * 2, 90.f, 90.f);
        body.CloseFigure();

        SolidBrush bg{Color(255, 16, 122, 255)};
        g.FillPath(&bg, &body);

        SolidBrush dot{Color(255, 255, 255, 255)};
        const REAL cy = e * .48f;
        const REAL rad = e * .085f;
        for (int i = -1; i <= 1; ++i)
            g.FillEllipse(&dot, e * .5f + i * e * .22f - rad, cy - rad,
                          rad * 2, rad * 2);

        SolidBrush bar{Color(200, 255, 255, 255)};
        g.FillRectangle(&bar, e * .26f, e * .72f, e * .48f, e * .07f);
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void applyTitleBarTheme(HWND hwnd, bool light) {
    BOOL dark = light ? FALSE : TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

    // Undocumented but stable since 1809: makes native menus follow the theme
    // instead of punching a white hole in the tray menu.
    if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) {
        using SetMode = int(WINAPI*)(int);
        auto fn = reinterpret_cast<SetMode>(
            reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
        if (fn) fn(light ? 0 : 2);
        FreeLibrary(ux);
    }
}

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Fuck-Windows-Touchpad";
// The value name this program was shipped with before the rename, so an old
// autostart entry can still be found and cleaned up.
constexpr wchar_t kRunValueOld[] = L"TouchpadGestures";

bool autostartEnabled() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) !=
        ERROR_SUCCESS)
        return false;
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(k, kRunValue, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &cb) ==
                        ERROR_SUCCESS &&
                    type == REG_SZ && buf[0] != 0;
    RegCloseKey(k);
    return ok;
}

bool setAutostart(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    if (on) {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        // --minimized so a login does not throw a window in the user's face.
        std::wstring val = L"\"";
        val += path;
        val += L"\" --minimized";
        ok = RegSetValueExW(k, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(val.c_str()),
                            static_cast<DWORD>((val.size() + 1) *
                                               sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        RegDeleteValueW(k, kRunValue);
        RegDeleteValueW(k, kRunValueOld);   // rename housekeeping
        ok = true;
    }
    RegCloseKey(k);
    return ok;
}

}  // namespace

// ===========================================================================
//  Creation
// ===========================================================================

bool UiWindow::create(HINSTANCE inst, UiHooks* hooks) {
    inst_  = inst;
    hooks_ = hooks;

    printf("[ui] gdiplus startup\n");
    GdiplusStartupInput gi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gi, nullptr);   // process lifetime, never shut down

    light_ = resolveLight(hooks_ ? hooks_->cfg : nullptr);
    P      = light_ ? &kLight : &kDark;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &UiWindow::wndProcThunk;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(255, 255, 255));
    wc.lpszClassName = kUiWindowClass;
    if (!RegisterClassExW(&wc)) {
        printf("[ui] RegisterClassEx failed err=%lu\n", GetLastError());
        return false;
    }

    WNDCLASSEXW dc = {};
    dc.cbSize        = sizeof(dc);
    dc.style         = CS_DROPSHADOW;
    dc.lpfnWndProc   = &UiWindow::dropProcThunk;
    dc.hInstance     = inst;
    dc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    dc.lpszClassName = kDropClass;
    RegisterClassExW(&dc);

    appIcon_ = buildIcon(32);

    scale_ = queryScale(nullptr);

    RECT rc = {0, 0, (LONG)lround(kW * scale_), (LONG)lround(kH * scale_)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    // On a 1080p laptop at 150% the design height does not fit; clamp to the
    // work area rather than opening a window taller than the screen.
    RECT work = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        const int maxW = (work.right - work.left) - (int)lround(40 * scale_);
        const int maxH = (work.bottom - work.top) - (int)lround(60 * scale_);
        if (ww > maxW) ww = maxW;
        if (wh > maxH) wh = maxH;
    }

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);

    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kUiWindowClass, kAppName,
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            (sw - ww) / 2, (sh - wh) / 2, ww, wh, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) {
        printf("[ui] CreateWindowEx failed err=%lu\n", GetLastError());
        return false;
    }

    if (appIcon_) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(appIcon_));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(appIcon_));
    }
    applyTitleBarTheme(hwnd_, light_);

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    drop_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kDropClass, L"",
                            WS_POPUP, 0, 0, 10, 10, hwnd_, nullptr, inst, this);

    addTrayIcon();
    SetTimer(hwnd_, kTimerTick, 500, nullptr);
    return true;
}

void UiWindow::destroy() {
    removeTrayIcon();
    if (drop_) {
        DestroyWindow(drop_);
        drop_ = nullptr;
    }
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (appIcon_) {
        DestroyIcon(appIcon_);
        appIcon_ = nullptr;
    }
}

// ===========================================================================
//  Message plumbing
// ===========================================================================

LRESULT CALLBACK UiWindow::wndProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        // Publish the handle before anything else runs: WM_NCCREATE and
        // WM_GETMINMAXINFO arrive *during* CreateWindowEx, while hwnd_ would
        // still be null, and the default branch would forward them to
        // DefWindowProc with an invalid handle (which aborts creation).
        if (cs->lpCreateParams)
            reinterpret_cast<UiWindow*>(cs->lpCreateParams)->hwnd_ = h;
    }
    auto* self = reinterpret_cast<UiWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, wp, lp);
    return self->handle(m, wp, lp);
}

LRESULT CALLBACK UiWindow::dropProcThunk(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<UiWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, wp, lp);
    return self->handleDrop(h, m, wp, lp);
}

LRESULT UiWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd_, &ps);
        paint(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            hideToTray();
            return 0;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        // 560 logical px of content plus sidebar and margins; the layout is
        // elastic now, so narrow is cramped but never clipped.
        mm->ptMinTrackSize.x = (LONG)lround(840 * scale_);
        mm->ptMinTrackSize.y = (LONG)lround(600 * scale_);
        return 0;
    }

    case WM_DPICHANGED: {
        scale_ = queryScale(hwnd_);
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // While recording a custom chord every keystroke belongs to the
        // recorder, including Alt combinations (which arrive as WM_SYSKEYDOWN)
        // and Esc, which cancels.
        if (recordSlot_ >= 0) {
            finishRecording((uint32_t)wp);
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        onMouseMove(pt);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        onLButtonDown(pt);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        onLButtonUp(pt);
        return 0;
    }

    case WM_MOUSEWHEEL:
        onMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            const int id = hitTest(pt);
            const bool clickable =
                id != kIdNone && !(id >= kIdSliderBase &&
                                   id < kIdSliderBase + (int)sliderDefs_.size());
            SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }

    case WM_TIMER:
        if (wp == kTimerTick) {
            // Follow a theme change without needing a restart.
            const bool light = resolveLight(hooks_ ? hooks_->cfg : nullptr);
            if (light != light_) {
                light_ = light;
                P      = light_ ? &kLight : &kDark;
                applyTitleBarTheme(hwnd_, light_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (tab_ == kAdvanced || tab_ == kAbout)
                InvalidateRect(hwnd_, nullptr, FALSE);
            // Keep the tray tooltip honest about pause state.
            static bool lastEnabled = true;
            const bool en = hooks_ && hooks_->cfg ? hooks_->cfg->enabled : true;
            if (en != lastEnabled) {
                lastEnabled = en;
                updateTrayTip();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        hideToTray();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ENDSESSION:
        // Being logged off: nothing to persist, but make sure the system's own
        // swipes are handed back before the process dies.
        exitRequested_ = true;
        PostQuitMessage(0);
        return 0;

    case kShowMsg:
        show();
        return 0;

    case kTrayMsg:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            show();
            break;
        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            showTrayMenu(pt);
            break;
        }
        default:
            break;
        }
        return 0;

    default:
        if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
            trayAdded_ = false;
            addTrayIcon();
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ===========================================================================
//  Painting
// ===========================================================================

void UiWindow::addHit(int id, RECT r) {
    // Page content is drawn through a scroll translation, so its hit rects have
    // to be shifted back into client space. Sidebar rows are drawn without the
    // translation and therefore see hitShift_ == 0.
    r.top += hitShift_;
    r.bottom += hitShift_;
    hits_.push_back({id, r});
}

void UiWindow::paint(HDC hdc) {
    RECT client;
    GetClientRect(hwnd_, &client);
    const int w = client.right;
    const int h = client.bottom;
    if (w <= 0 || h <= 0) return;

    Bitmap buffer(w, h, PixelFormat32bppRGB);
    Graphics g(&buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    Draw d;
    d.g = &g;
    d.s = scale_;

    g.Clear(Color(P->page));

    hits_.clear();
    rowDefs_.clear();
    sliderDefs_.clear();
    for (auto& t : sliderTrack_) t = RECT{0, 0, 0, 0};

    const int cx     = (int)lround(kContentX * scale_);
    // The content column follows the window: every extra pixel of width goes
    // to the rows, until a logical cap past which the column stops growing and
    // the surplus stays as breathing room on the right (line length reads
    // badly beyond that). kW only decides the initial window size now.
    const float cwLogic = std::min(
        1240.f, std::max(560.f, (float)w / scale_ - kContentX - kPadX));
    const int cw     = (int)lround(cwLogic * scale_);
    const int top    = 0;
    const int bottom = h - (int)lround(28 * scale_);
    viewportH_       = std::max(1, bottom - top);

    // Clamp against the height measured on the previous paint. The value only
    // changes when the window or the page changes, so this settles in one frame
    // and never fights the scrollbar.
    scrollMax_ = std::max(0, contentH_ - viewportH_);
    if (scrollPx_ > scrollMax_) scrollPx_ = scrollMax_;
    if (scrollPx_ < 0) scrollPx_ = 0;

    hitShift_ = 0;
    paintSidebar(&g, w, h);

    // Page content, clipped to its own column and scrolled.
    {
        const GraphicsState st = g.Save();
        g.SetClip(RectF((REAL)cx, (REAL)top, (REAL)cw,
                        (REAL)std::max(1, bottom - top)));
        g.TranslateTransform(0.f, -(REAL)scrollPx_);
        hitShift_ = -scrollPx_;

        switch (tab_) {
        case kGestures:  paintGestures(&g, cx, cw, 0); break;
        case kWindow:    paintWindowPage(&g, cx, cw, 0); break;
        case kAdvanced:  paintAdvanced(&g, cx, cw, 0); break;
        default:         paintAbout(&g, cx, cw, 0); break;
        }
        hitShift_ = 0;

        g.Restore(st);
    }

    // Scrollbar: only when the page actually overflows.
    if (contentH_ > viewportH_ && contentH_ > 0) {
        const int trackH = viewportH_;
        const int barH   = std::max((int)lround(40 * scale_),
                                    trackH * viewportH_ / contentH_);
        const int span   = contentH_ - viewportH_;
        const int pos    = span > 0 ? (trackH - barH) * scrollPx_ / span : 0;
        const int bx     = cx + cw + (int)lround(8 * scale_);
        RECT bar = {bx, top + pos, bx + (int)lround(4 * scale_),
                    top + pos + barH};
        d.fillRound(bar, 2.f * scale_, light_ ? 0x33000000 : 0x40FFFFFF);
    }

    // Status line at the very bottom, outside the scroll area. A hairline above
    // it keeps the footer from reading as a clipped piece of content.
    {
        Pen pen(Color(P->sep), 1.f);
        g.DrawLine(&pen, (REAL)cx, (REAL)bottom - .5f, (REAL)(cx + cw),
                   (REAL)bottom - .5f);
    }
    if (!status_.empty()) {
        RECT r = {(int)lround(kContentX * scale_), h - (int)lround(22 * scale_),
                  cx + cw,
                  h - (int)lround(4 * scale_)};
        d.txt(status_, r, 10.5f, statusError_ ? P->err : P->text3, 0);
    }

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0);
}

void UiWindow::paintSidebar(void* gp, int w, int h) {
    (void)w;
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    RECT side = {0, 0, S((float)kSideW), h};
    d.fillRect(side, P->side);

    {
        Pen pen(Color(P->sep), 1.f);
        d.g->DrawLine(&pen, (REAL)S((float)kSideW) - .5f, 0.f,
                      (REAL)S((float)kSideW) - .5f, (REAL)h);
    }

    // Brand: a small accent tile plus the name.
    {
        RECT tile = {S(20), S(22), S(46), S(48)};
        d.fillRound(tile, 7.f * scale_, P->accent);
        const float cx = (float)S(33), cy = (float)S(35);
        for (int i = -1; i <= 1; ++i)
            d.circle(cx + (float)S(6 * i), cy, 1.7f * scale_, 0xFFFFFFFF);
    }
    d.txt(kAppName, {S(56), S(18), S((float)kSideW - 8), S(38)}, 10.f,
          P->text, 0, true);
    d.txt(L"Windows", {S(56), S(38), S((float)kSideW - 8), S(54)}, 9.5f,
          P->text3);

    const wchar_t* names[4] = {L"手势", L"窗口", L"高级", L"关于"};
    int top = 78;
    for (int i = 0; i < 4; ++i) {
        RECT r = {S(10), S((float)top), S((float)kSideW - 10),
                  S((float)top + 34)};
        const bool active = (tab_ == i);
        if (active) d.fillRound(r, 7.f * scale_, P->accent);
        else if (hover_ == kIdNav0 + i)
            d.fillRound(r, 7.f * scale_, light_ ? 0x0F000000 : 0x18FFFFFF);

        const ARGB fg = active ? P->selText : P->text;
        const ARGB ic = active ? P->selText : P->text3;
        const float ix = (float)S(30), iy = (float)(S(top) + S(17));

        switch (i) {
        case 0: {  // three finger dots + a line: the gesture mark
            for (int k = -1; k <= 1; ++k)
                d.circle(ix + (float)S(5 * k), iy - 2.f * scale_,
                         1.9f * scale_, ic);
            d.line(ix - (float)S(4), iy + 3.f * scale_, ix + (float)S(4),
                   iy + 3.f * scale_, ic, 1.6f * scale_);
            break;
        }
        case 1: {  // two overlapping window frames
            RECT a = {S(22), S(top + 8), S(31), S(top + 17)};
            RECT b = {S(27), S(top + 13), S(36), S(top + 22)};
            d.strokeRound(a, 2.f * scale_, ic, 1.4f * scale_);
            d.strokeRound(b, 2.f * scale_, ic, 1.4f * scale_);
            break;
        }
        case 2: {  // slider bars
            for (int k = 0; k < 3; ++k) {
                const float yy = iy - (float)S(4) + (float)S(4 * k);
                d.line(ix - (float)S(5), yy, ix + (float)S(5), yy, ic,
                       1.3f * scale_);
                d.circle(ix + (float)S(k == 1 ? -3 : 2), yy, 1.7f * scale_, ic);
            }
            break;
        }
        default: {  // info
            d.ring(ix, iy, 4.6f * scale_, ic, 1.4f * scale_);
            d.circle(ix, iy - 2.f * scale_, 1.2f * scale_, ic);
            d.line(ix, iy + .5f * scale_, ix, iy + 3.f * scale_, ic,
                   1.4f * scale_);
            break;
        }
        }

        d.txt(names[i], {S(46), S((float)top), S((float)kSideW - 12),
                         S((float)top + 34)},
              12.5f, fg, 0, active);
        addHit(kIdNav0 + i, r);
        top += 40;
    }

    // Bottom: what the service is doing right now.
    const bool en = hooks_ && hooks_->cfg ? hooks_->cfg->enabled : true;
    const wchar_t* state = live_.pinching
                               ? L"正在缩放窗口"
                               : (live_.dragging ? L"正在拖动窗口"
                                                 : (en ? L"手势接管中" : L"已暂停"));
    RECT pill = {S(12), h - S(74), S((float)kSideW - 12), h - S(26)};
    d.fillRound(pill, 8.f * scale_, light_ ? 0x0A000000 : 0x14FFFFFF);

    const ARGB dotc = !en ? P->warn
                          : (live_.dragging || live_.pinching ? P->accent
                                                              : P->ok);
    d.circle((float)S(28), (float)(h - S(50)), 3.4f * scale_, dotc);
    d.txt(state, {S(40), h - S(62), S((float)kSideW - 16), h - S(38)}, 11.f,
          P->text);
    d.txt(wfmt(L"v%ls", kVersion), {S(28), h - S(44),
                                    S((float)kSideW - 16), h - S(28)},
          9.f, P->text3);
}

// ---------------------------------------------------------------- row kit

int UiWindow::groupHeader(void* gp, int cx, int cw, int y, const wchar_t* title) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };
    d.txt(title, {cx, S((float)y), cx + cw, S((float)y + 22)}, 10.5f,
          P->text3, 0, true);
    return y + kGroupHead;
}

int UiWindow::rowSwitch(void* gp, int cx, int cw, int y, const wchar_t* label,
                        const wchar_t* desc, bool on, Field f, bool dim) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    const int id = kIdRowBase + (int)rowDefs_.size();
    rowDefs_.push_back({f, 0});

    RECT row = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
    if (rowsInCard_ > 0) d.hline(cx + S(16), cx + cw - S(16), row.top, P->sep);
    ++rowsInCard_;
    if (hover_ == id && !pressed_)
        d.fillRound(row, 8.f * scale_, P->cardHover);
    addHit(id, row);

    const ARGB fg = dim ? P->text3 : P->text;
    if (desc && *desc) {
        d.txt(label, {row.left + S(16), row.top + S(6), row.right - S(80),
                      row.top + S(24)},
              12.5f, fg);
        d.txt(desc, {row.left + S(16), row.top + S(23), row.right - S(80),
                     row.top + S(39)},
              9.8f, P->text3);
    } else {
        d.txt(label, {row.left + S(16), row.top, row.right - S(80), row.bottom},
              12.5f, fg);
    }

    RECT sw = {row.right - S(16) - S(40), row.top + S((kRowH - 24) / 2.f),
               row.right - S(16), row.top + S((kRowH - 24) / 2.f) + S(24)};
    drawSwitch(d, sw, on, dim);
    return y + kRowH;
}

int UiWindow::rowChoice(void* gp, int cx, int cw, int y, const wchar_t* label,
                        int slotIndex, bool dim) {
    return rowChoiceText(gp, cx, cw, y, label, slotValue(slotIndex), Field::Bind,
                         slotIndex, dim);
}

// What a binding row shows on the right. Three actions have no useful name to
// show -- the point of the row is the payload that will actually fire.
std::wstring UiWindow::slotValue(int s) const {
    AppConfig* c = hooks_ ? hooks_->cfg : nullptr;
    if (!c || !slot::valid(s)) return L"无操作";
    if (recordSlot_ == s) return L"请按键…";

    switch (c->bind[s]) {
    case ActionId::None:
        return L"无操作";
    case ActionId::Custom:
        return c->custom[s].valid() ? fromUtf8(chordToString(c->custom[s]))
                                    : std::wstring(L"未设置");
    case ActionId::Launch: {
        const std::string& p = c->launch[s];
        if (p.empty()) return L"未选择";
        const size_t slash = p.find_last_of("\\/");
        return fromUtf8(slash == std::string::npos ? p : p.substr(slash + 1));
    }
    default:
        return actionText(c->bind[s]);
    }
}

int UiWindow::slotOf(Field f, int arg) {
    return f == Field::Bind ? arg : -1;
}

// Same vocabulary the gesture page uses for its row labels, so a status line
// like "已设置：三指轻点 → 任务视图" names the thing the user just clicked.
std::wstring UiWindow::slotLabelW(int s) {
    if (!slot::valid(s)) return L"手势";
    static const wchar_t* kKind[slot::KindCount] = {
        L"轻点",       L"双击",     L"左滑",      L"右滑",
        L"上滑",       L"下滑",     L"左上角",    L"右上角",
        L"左下角",     L"右下角",   L"左边缘滑动", L"右边缘滑动",
    };
    const int fingers = slot::fingersOf(s);
    const wchar_t* f  = fingers == 3 ? L"三指"
                        : fingers == 4 ? L"四指"
                                       : L"五指";
    const int k = slot::kindOf(s);
    return std::wstring(f) + (k >= 0 && k < slot::KindCount ? kKind[k] : L"");
}

int UiWindow::rowSegment(void* gp, int cx, int cw, int y, const wchar_t* label,
                         const wchar_t* const* names, int count, int cur,
                         int idBase, bool dim) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    RECT row = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
    if (rowsInCard_ > 0) d.hline(cx + S(16), cx + cw - S(16), row.top, P->sep);
    ++rowsInCard_;

    d.txt(label, {row.left + S(16), row.top, row.left + S(200), row.bottom},
          12.5f, dim ? P->text3 : P->text);

    const int segW = S(64);
    const int segH = S(28);
    const int totalW = segW * count;
    const int x0 = row.right - S(16) - totalW;
    const int y0 = row.top + (kRowH * (int)scale_ - segH) / 2;

    RECT track = {x0, y0, x0 + totalW, y0 + segH};
    d.fillRound(track, 7.f * scale_,
                light_ ? 0xFFE9E9ED : 0xFF3A3A3C);

    for (int i = 0; i < count; ++i) {
        RECT seg = {x0 + i * segW, y0, x0 + (i + 1) * segW, y0 + segH};
        const int id = idBase + i;
        const bool on = (i == cur);
        if (on) {
            RECT fill = seg;
            fill.left += S(2);
            fill.right -= S(2);
            fill.top += S(2);
            fill.bottom -= S(2);
            d.fillRound(fill, 5.5f * scale_, light_ ? 0xFFFFFFFF : 0xFF58585C);
        } else if (hover_ == id) {
            d.fillRound(seg, 6.f * scale_,
                        light_ ? 0x14000000 : 0x1AFFFFFF);
        }
        d.txt(names[i], seg, 11.5f, on ? P->text : P->text2, 1, on);
        addHit(id, seg);
    }
    return y + kRowH;
}

int UiWindow::rowChoiceText(void* gp, int cx, int cw, int y,
                            const wchar_t* label, const std::wstring& value,
                            Field f, int arg, bool dim) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    const int id = kIdRowBase + (int)rowDefs_.size();
    rowDefs_.push_back({f, arg});

    RECT row = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
    if (rowsInCard_ > 0) d.hline(cx + S(16), cx + cw - S(16), row.top, P->sep);
    ++rowsInCard_;
    if (hover_ == id) d.fillRound(row, 8.f * scale_, P->cardHover);
    addHit(id, row);

    d.txt(label, {row.left + S(16), row.top, row.right - S(200), row.bottom},
          12.5f, dim ? P->text3 : P->text);

    const int chevX = row.right - S(22);
    drawChevron(d, (float)chevX, (float)((row.top + row.bottom) / 2),
                P->text3);

    d.txt(value, {row.right - S(190), row.top, chevX - S(8), row.bottom},
          12.5f, P->text2, 2);
    return y + kRowH;
}

int UiWindow::rowSlider(void* gp, int cx, int cw, int y, const wchar_t* label,
                        float value, const wchar_t* text, float t, Field f,
                        float lo, float hi, bool dim) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    const int idx = (int)sliderDefs_.size();
    const int id  = kIdSliderBase + idx;
    sliderDefs_.push_back({f, lo, hi});

    RECT row = {cx, S((float)y), cx + cw, S((float)y + kSliderH)};
    if (rowsInCard_ > 0) d.hline(cx + S(16), cx + cw - S(16), row.top, P->sep);
    ++rowsInCard_;
    addHit(id, row);

    d.txt(label, {row.left + S(16), row.top, row.left + S(200),
                  row.top + S(26)},
          12.5f, dim ? P->text3 : P->text);
    if (text && *text)
        d.txt(text, {row.right - S(180), row.top, row.right - S(16),
                     row.top + S(26)},
              11.f, dim ? P->text3 : P->text2, 2);

    RECT track = {row.left + S(16), row.top + S(36), row.right - S(16),
                  row.top + S(42)};
    if (idx < 8) sliderTrack_[idx] = track;
    drawSlider(d, track, t, dim);
    (void)value;
    return y + kSliderH;
}

int UiWindow::rowButton(void* gp, int cx, int cw, int y, const wchar_t* label,
                        int id, bool destructive) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    RECT row = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
    if (rowsInCard_ > 0) d.hline(cx + S(16), cx + cw - S(16), row.top, P->sep);
    ++rowsInCard_;
    if (hover_ == id) d.fillRound(row, 8.f * scale_, P->cardHover);
    addHit(id, row);

    d.txt(label, {row.left + S(16), row.top, row.right - S(16), row.bottom},
          12.5f, destructive ? P->err : P->accent);
    return y + kRowH;
}

int UiWindow::rowInfo(void* gp, int cx, int cw, int y, const wchar_t* label,
                      const std::wstring& value) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    RECT row = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
    if (rowsInCard_ > 0) d.hline(cx + S(16), cx + cw - S(16), row.top, P->sep);
    ++rowsInCard_;
    d.txt(label, {row.left + S(16), row.top, row.right - S(240), row.bottom},
          12.5f, P->text);
    d.txt(value, {row.right - S(320), row.top, row.right - S(16), row.bottom},
          11.5f, P->text2, 2);
    return y + kRowH;
}

// ------------------------------------------------------------------- pages

void UiWindow::paintGestures(void* gp, int cx, int cw, int y) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };
    AppConfig* c = hooks_->cfg;
    if (!c) return;

    d.txt(L"手势", {cx, S(26), cx + cw, S(58)}, 20.f, P->text, 0, true);
    d.txt(L"每一个手势都能单独指派动作。改动立即生效。",
          {cx, S(58), cx + cw, S(78)}, 11.f, P->text3);

    y = 100;

    // ---- master switch (single-row card, no group header) ----
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowSwitch(gp, cx, cw, y, L"启用手势",
                      L"关闭后手势全部失效，触摸板恢复系统默认行为", c->enabled,
                      Field::Enabled);
        y += 8;
    }

    // ---- which finger count is being edited ----
    // Ten rows per finger count would be a wall, so the page shows one count at
    // a time and every count keeps its own complete table.
    const int fingers = slot::kFirstFingers +
                        std::min(fingerTab_, slot::kFingerCounts - 1);
    {
        static const wchar_t* kSegNames[3] = {L"三指", L"四指", L"五指"};

        y = groupHeader(gp, cx, cw, y, L"手指数");
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowSegment(gp, cx, cw, y, L"当前编辑", kSegNames,
                       slot::kFingerCounts, fingerTab_, kIdFingerSeg);
        y += 8;

        if (fingers == 5 && hooks_->haveCaps && hooks_->caps.maxContacts < 5) {
            d.txt(wfmt(L"这块触摸板最多识别 %u 点触控，五指手势不会触发。",
                       (unsigned)hooks_->caps.maxContacts),
                  {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f,
                  P->warn);
            y += 22;
        }
    }

    // ---- tap / double tap ----
    {
        y = groupHeader(gp, cx, cw, y, L"轻点");
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 3)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowSwitch(gp, cx, cw, y, L"启用轻点",
                      L"手指快速点一下，不滑动；轻点在手势识别结束后触发",
                      c->tapEnabled, Field::TapEnabled);
        y = rowChoice(gp, cx, cw, y, L"轻点",
                      slot::index(fingers, slot::Tap), !c->tapEnabled);
        y = rowChoice(gp, cx, cw, y, L"双击",
                      slot::index(fingers, slot::DoubleTap), !c->tapEnabled);
        y += 8;
    }
    d.txt(L"绑定了双击时，单击会等一小会儿确认没有第二下；只绑单击则立即触发。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);
    y += 24;

    // ---- corner taps ----
    {
        static const wchar_t* kCornerNames[4] = {L"左上角", L"右上角", L"左下角",
                                                 L"右下角"};
        y = groupHeader(gp, cx, cw, y, L"角落轻点");
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 4)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        for (int i = 0; i < 4; ++i)
            y = rowChoice(gp, cx, cw, y, kCornerNames[i],
                          slot::index(fingers, slot::CornerTL + i),
                          !c->tapEnabled);
        y += 8;
    }
    d.txt(L"只绑角落时，在其他位置轻点不受影响；角落绑了动作就优先于普通轻点。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);
    y += 24;

    // ---- swipes ----
    {
        const bool reserved =
            (c->mode == Mode::Drag && c->dragFingers == fingers);
        static const wchar_t* kDirNames[4] = {L"向左滑动", L"向右滑动",
                                              L"向上滑动", L"向下滑动"};
        y = groupHeader(gp, cx, cw, y, L"滑动");
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 4)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        for (int i = 0; i < 4; ++i) {
            const int kind = i == dir::Left    ? slot::SwipeLeft
                             : i == dir::Right ? slot::SwipeRight
                             : i == dir::Up    ? slot::SwipeUp
                                               : slot::SwipeDown;
            y = rowChoice(gp, cx, cw, y, kDirNames[i],
                          slot::index(fingers, kind), reserved);
        }
        y += 8;

        if (reserved)
            d.txt(L"这个手指数当前用于拖动窗口，四个方向都不会触发。",
                  {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f,
                  P->warn);
        else
            d.txt(L"斜着划也算：容差在“高级”里调整。",
                  {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f,
                  P->text3);
        y += 24;
    }

    // ---- edge swipes ----
    {
        y = groupHeader(gp, cx, cw, y, L"边缘滑动");
        if (c->edgeEnabled) {
            RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 2)};
            d.fillRound(card, (float)kCardRad * scale_, P->card);
            rowsInCard_ = 0;
            y = rowChoice(gp, cx, cw, y, L"从左边缘起手",
                          slot::index(fingers, slot::EdgeLeft));
            y = rowChoice(gp, cx, cw, y, L"从右边缘起手",
                          slot::index(fingers, slot::EdgeRight));
            y += 8;
        } else {
            RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
            d.fillRound(card, (float)kCardRad * scale_, P->card);
            rowsInCard_ = 0;
            y = rowChoiceText(gp, cx, cw, y, L"从左边缘起手", L"未开启",
                              Field::EdgeEnabled, 0, true);
            y += 8;
        }
    }
    d.txt(c->edgeEnabled
              ? L"从触摸板边缘起手的左右滑动可以单独指派，不影响中间的滑动。"
              : L"在下面的“双指与边缘”里打开后，边缘可以单独指派动作。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);
    y += 24;

    // ---- two fingers ----
    y = groupHeader(gp, cx, cw, y, L"双指");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kSliderH + kRowH)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowSwitch(gp, cx, cw, y, L"捏合缩放窗口",
                      c->pinchAnchored ? L"张开手指放大，捏合缩小；以手指位置为中心"
                                       : L"张开手指放大，捏合缩小",
                      c->pinchEnabled, Field::PinchEnabled);
        y = rowSlider(gp, cx, cw, y, L"缩放灵敏度", c->pinchSensitivity,
                      wfmt(L"%.2f×", (double)c->pinchSensitivity).c_str(),
                      (c->pinchSensitivity - 0.5f) / 1.5f, Field::PinchSens,
                      0.5f, 2.0f, !c->pinchEnabled);
        y += 8;
    }
    d.txt(L"双指只做缩放；双指滑动仍是系统自己的滚动。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);

    contentH_ = S((float)(y + 40));
}

void UiWindow::paintWindowPage(void* gp, int cx, int cw, int y) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };
    AppConfig* c = hooks_->cfg;
    if (!c) return;

    d.txt(L"窗口", {cx, S(26), cx + cw, S(58)}, 20.f, P->text, 0, true);
    d.txt(L"拖动与缩放的手感，以及窗口动画。",
          {cx, S(58), cx + cw, S(78)}, 11.f, P->text3);

    y = 100;

    y = groupHeader(gp, cx, cw, y, L"拖动");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kSliderH + kRowH)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowSlider(gp, cx, cw, y, L"跟随灵敏度", c->sensitivity,
                      wfmt(L"%.2f×", (double)c->sensitivity).c_str(),
                      (c->sensitivity - 0.3f) / 2.7f, Field::Sens, 0.3f, 3.0f);
        // Which finger count grabs a window. Four fingers is easier to trigger
        // on purpose and leaves three fingers free for the swipe table.
        const wchar_t* fingerNames[2] = {L"三指", L"四指"};
        const int fi = (c->dragFingers == 4) ? 1 : 0;
        y = rowChoiceText(gp, cx, cw, y, L"拖动指头", fingerNames[fi],
                          Field::DragFingers, 0);
        y += 8;
    }
    d.txt(L"手指滑过整个触摸板宽度 ≈ 窗口横穿整个屏幕。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);
    y += 24;

    y = groupHeader(gp, cx, cw, y, L"双指缩放");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 3)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        const wchar_t* axisNames[4] = {L"等比", L"跟随手指", L"仅横向", L"仅纵向"};
        const int ai = c->pinchAxis > 3 ? 0 : (int)c->pinchAxis;
        y = rowChoiceText(gp, cx, cw, y, L"缩放方向", axisNames[ai],
                          Field::PinchAxis, 0);
        y = rowSwitch(gp, cx, cw, y, L"以手指位置为中心",
                      L"窗口从两指之间向外长，而不是原地胀缩", c->pinchAnchored,
                      Field::PinchAnchored);
        y = rowSwitch(gp, cx, cw, y, L"到达极限时回弹",
                      L"缩到最小或放到最大时有一点阻力，松手弹回", c->pinchRubberBand,
                      Field::PinchRubber);
        y += 8;
    }
    d.txt(L"跟随手指：横向张开只改宽度，纵向张开只改高度，斜着捏才是等比。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);
    y += 24;

    y = groupHeader(gp, cx, cw, y, L"动画");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kSliderH)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowSlider(gp, cx, cw, y, L"动画时长", (float)c->animMs,
                      c->animMs == 0 ? L"关闭"
                                     : wfmt(L"%u 毫秒", (unsigned)c->animMs).c_str(),
                      (float)c->animMs / 600.f, Field::AnimMs, 0.f, 600.f);
        y += 8;
    }

    y = groupHeader(gp, cx, cw, y, L"快捷测试");
    {
        const int ids[4] = {kIdTestLeft, kIdTestRight, kIdTestDrag, kIdTestPinch};
        const wchar_t* names[4] = {L"与左侧窗口互换", L"与右侧窗口互换",
                                   L"拖动往返测试", L"缩放往返测试"};
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 4)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        for (int i = 0; i < 4; ++i)
            y = rowButton(gp, cx, cw, y, names[i], ids[i]);
        y += 8;
    }
    d.txt(L"不需要触摸板，直接验证窗口层是否正常。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);

    contentH_ = S((float)(y + 40));
}

void UiWindow::paintAdvanced(void* gp, int cx, int cw, int y) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };
    AppConfig* c = hooks_->cfg;
    if (!c) return;

    d.txt(L"高级", {cx, S(26), cx + cw, S(58)}, 20.f, P->text, 0, true);
    d.txt(L"识别参数与运行状态。改坏了可以恢复推荐值。",
          {cx, S(58), cx + cw, S(78)}, 11.f, P->text3);

    y = 100;

    y = groupHeader(gp, cx, cw, y, L"识别");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kSliderH * 3)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;

        // Device units are meaningless to a user, so translate them into the
        // travel distance they actually correspond to. The logical range spans
        // roughly 105 mm of glass on a laptop touchpad and the X and Y ranges
        // agree on the same density (1500/105 vs 927/65), so a single scale
        // factor is good enough for both axes.
        const float unitsPerMm =
            (hooks_->haveCaps && hooks_->caps.logicalMaxX > 0)
                ? (float)hooks_->caps.logicalMaxX / 105.f
                : 14.3f;
        const float mm = c->thresholdX / unitsPerMm;
        y = rowSlider(gp, cx, cw, y, L"滑动触发距离", c->thresholdX,
                      wfmt(L"%.1f 厘米", (double)(mm / 10.f)).c_str(),
                      (c->thresholdX - 100.f) / (600.f - 100.f), Field::Threshold,
                      100.f, 600.f);

        // The config stores the tolerance as a |dy|/|dx| ratio; the angle it
        // implies is what a user can actually picture. 1.5 -> about 34 degrees.
        // The slider space is degrees so the knob always agrees with the label.
        const float deg = degOfDominance(c->dominance);
        y = rowSlider(gp, cx, cw, y, L"斜滑容差", c->dominance,
                      wfmt(L"±%.0f°", (double)deg).c_str(),
                      (deg - kSlantMinDeg) / (kSlantMaxDeg - kSlantMinDeg),
                      Field::Dominance, kSlantMinDeg, kSlantMaxDeg);

        // How much the two-finger gap has to change before a pinch counts.
        // Small values feel instant; too small and a resting pair of fingers
        // jitters the window.
        const float dbPct = c->pinchDeadband * 100.f;
        y = rowSlider(gp, cx, cw, y, L"双指起手死区", dbPct,
                      wfmt(L"%.1f%%", (double)dbPct).c_str(),
                      (c->pinchDeadband - 0.005f) / (0.20f - 0.005f),
                      Field::PinchDeadband, 0.5f, 20.f);
        y += 8;
    }
    d.txt(L"触发距离按实际滑动行程换算；斜滑容差是允许的方向夹角，角度越小越严格；"
          L"起手死区越小越跟手。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);
    y += 24;

    y = groupHeader(gp, cx, cw, y, L"轻点与边缘");
    {
        RECT card = {cx, S((float)y), cx + cw,
                     S((float)y + kSliderH * 2 + kRowH + (c->edgeEnabled ? kSliderH : 0))};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowSlider(gp, cx, cw, y, L"轻点最长按住", (float)c->tapMaxMs,
                      wfmt(L"%u 毫秒", (unsigned)c->tapMaxMs).c_str(),
                      (float)(c->tapMaxMs - 80) / 320.f, Field::TapMs, 80.f,
                      400.f, !c->tapEnabled);
        // Shown in millimetres: raw device units mean nothing to a user. The
        // same glass-density conversion the swipe distance uses.
        const float unitsPerMm =
            (hooks_->haveCaps && hooks_->caps.logicalMaxX > 0)
                ? (float)hooks_->caps.logicalMaxX / 105.f
                : 14.3f;
        y = rowSlider(gp, cx, cw, y, L"轻点位移容差", c->tapSlop,
                      wfmt(L"%.1f 毫米", (double)(c->tapSlop / unitsPerMm)).c_str(),
                      (c->tapSlop - 5.f) / 45.f, Field::TapSlop, 5.f, 50.f,
                      !c->tapEnabled);
        y = rowSwitch(gp, cx, cw, y, L"区分边缘滑动",
                      L"从触摸板左右边缘起手的滑动可以单独绑定", c->edgeEnabled,
                      Field::EdgeEnabled);
        if (c->edgeEnabled) {
            y = rowSlider(gp, cx, cw, y, L"边缘区域宽度", c->edgeZone * 100.f,
                          wfmt(L"%.0f%%", (double)(c->edgeZone * 100.f)).c_str(),
                          (c->edgeZone - 0.04f) / 0.26f, Field::EdgeZone, 4.f,
                          30.f);
        }
        y += 8;
    }

    y = groupHeader(gp, cx, cw, y, L"运行状态");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 5)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;

        const bool live = (GetTickCount64() - lastDataAt_) < 1500;
        y = rowInfo(gp, cx, cw, y, L"设备",
                    hooks_->haveCaps
                        ? wfmt(L"%ls", hooks_->caps.product.c_str())
                        : std::wstring(L"未检测到"));
        if (hooks_->haveCaps) {
            y = rowInfo(gp, cx, cw, y, L"硬件标识",
                        wfmt(L"%04X:%04X · 最多 %u 点", hooks_->caps.vid,
                             hooks_->caps.pid,
                             (unsigned)hooks_->caps.maxContacts));
            y = rowInfo(gp, cx, cw, y, L"逻辑范围",
                        wfmt(L"X 0..%u   Y 0..%u", live_.rangeX, live_.rangeY));
        }
        y = rowInfo(gp, cx, cw, y, L"帧率",
                    live_.frames ? wfmt(L"%.0f fps", (double)live_.fps)
                                 : std::wstring(L"—"));
        y = rowInfo(gp, cx, cw, y, L"累计帧数 / 手势",
                    humanCount(live_.frames) + L" / " + humanCount(live_.gestures));
        (void)live;
        y += 8;
    }
    d.txt(live_.decodeFail
              ? wfmt(L"解码失败 %llu 次，报文布局可能不匹配。",
                     (unsigned long long)live_.decodeFail)
              : std::wstring(L"报文解码正常。"),
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f,
          live_.decodeFail ? P->warn : P->text3);
    y += 24;

    // A picture of the glass, drawn to the device's own logical range, so a
    // finger in the top-left corner lands in the top-left of the drawing. When
    // a gesture "does not work" this is what separates "the decoder sees
    // nothing" from "the recogniser rejected it".
    y = groupHeader(gp, cx, cw, y, L"触点");
    {
        const float rx = (float)(live_.rangeX
                                     ? live_.rangeX
                                     : (hooks_->haveCaps ? hooks_->caps.logicalMaxX
                                                         : 1500u));
        const float ry = (float)(live_.rangeY
                                     ? live_.rangeY
                                     : (hooks_->haveCaps ? hooks_->caps.logicalMaxY
                                                         : 927u));
        const float padW = 320.f * scale_;
        const float padH = padW * (ry / std::max(1.f, rx));
        const int   cardH = (int)lround(padH + 44.f * scale_);
        RECT card = {cx, S((float)y), cx + cw, S((float)y) + cardH};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;

        const int padLeft = card.left + (int)lround(20.f * scale_);
        const int padTop  = card.top + (int)lround(22.f * scale_);
        RECT pad = {padLeft, padTop, padLeft + (int)lround(padW),
                    padTop + (int)lround(padH)};

        d.fillRound(pad, 10.f * scale_, P->page);
        d.strokeRound(pad, 10.f * scale_, P->sep);

        // Edge bands, only while they are in use: they show where a swipe has
        // to start to count as an edge gesture.
        if (c->edgeEnabled) {
            const int zw = (int)lround(padW * c->edgeZone);
            const ARGB band = (P->accent & 0x00FFFFFFu) | 0x22000000u;
            RECT l = {pad.left + 1, pad.top + 1, pad.left + zw, pad.bottom - 1};
            RECT r = {pad.right - zw, pad.top + 1, pad.right - 1, pad.bottom - 1};
            d.fillRound(l, 8.f * scale_, band);
            d.fillRound(r, 8.f * scale_, band);
        }

        // A live readout next to the glass: enough to answer "is anything
        // arriving at all" without leaving the page.
        const int infoX = pad.right + (int)lround(28.f * scale_);
        const int infoW = card.right - infoX - (int)lround(16.f * scale_);
        if (infoW > 120) {
            int iy = pad.top;
            const int step = (int)lround(34.f * scale_);
            d.txt(wfmt(L"当前 %u 点", (unsigned)live_.pointers.size()),
                  {infoX, iy, infoX + infoW, iy + step}, 11.5f, P->text);
            iy += step;
            d.txt(hooks_->haveCaps
                      ? wfmt(L"最多 %u 点", (unsigned)hooks_->caps.maxContacts)
                      : std::wstring(L"未知上限"),
                  {infoX, iy, infoX + infoW, iy + step}, 10.5f, P->text3);
            iy += step;
            d.txt(c->edgeEnabled
                      ? wfmt(L"边缘区 %.0f%%", (double)(c->edgeZone * 100.f))
                      : std::wstring(L"边缘滑动已关闭"),
                  {infoX, iy, infoX + infoW, iy + step}, 10.5f, P->text3);
        }

        const size_t n = live_.pointers.size();
        if (n == 0) {
            d.txt(L"把手指放上去", {pad.left, pad.top, pad.right, pad.bottom}, 10.5f,
                  P->text3, 1);
        } else {
            const float fw = (float)(pad.right - pad.left);
            const float fh = (float)(pad.bottom - pad.top);
            for (const auto& p : live_.pointers) {
                const float fx = std::min(1.f, std::max(0.f, p.first / std::max(1.f, rx)));
                const float fy = std::min(1.f, std::max(0.f, p.second / std::max(1.f, ry)));
                const float px = (float)pad.left + fx * fw;
                const float py = (float)pad.top + fy * fh;
                d.circle(px, py, 15.f * scale_,
                         (P->accent & 0x00FFFFFFu) | 0x33000000u);
                d.circle(px, py, 7.f * scale_, P->accent);
            }
        }

        y += cardH + 8;
    }

    y = groupHeader(gp, cx, cw, y, L"其他");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 3)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        const wchar_t* themeNames[3] = {L"跟随系统", L"浅色", L"深色"};
        const int ti = c->theme > 2 ? 0 : c->theme;
        y = rowChoiceText(gp, cx, cw, y, L"外观", themeNames[ti], Field::Theme,
                          0);
        y = rowSwitch(gp, cx, cw, y, L"开机时自动启动",
                      L"登录后静默运行，不弹出窗口", autostartEnabled(),
                      Field::Autostart);
        y = rowButton(gp, cx, cw, y, L"恢复推荐设置", kIdResetAll);
        y += 8;
    }

    // ---- recent events ----
    y = groupHeader(gp, cx, cw, y, L"最近事件");
    {
        const int rows = 6;
        RECT card = {cx, S((float)y), cx + cw, S((float)y + 24 * rows + 12)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;

        if (logsDirty_ && hooks_->pullLogs) {
            logs_ = hooks_->pullLogs();
            logsDirty_ = false;
        }
        int ly = y + 8;
        if (logs_.empty()) {
            d.txt(L"还没有记录。做一次手势就会出现在这里。",
                  {cx + S(16), S((float)ly), cx + cw - S(16),
                   S((float)ly + 20)},
                  10.5f, P->text3);
        } else {
            // Newest first, and only as many as the card holds.
            for (int i = 0; i < rows; ++i) {
                const int idx = (int)logs_.size() - 1 - i;
                if (idx < 0) break;
                const UiLogLine& l = logs_[idx];
                d.circle((float)(cx + S(22)), (float)(S(ly) + S(10)),
                         3.f * scale_, kindColor(l.kind));
                d.txt(logClock(l.atMs),
                      {cx + S(34), S((float)ly), cx + S(34) + S(58),
                       S((float)ly + 20)},
                      9.5f, P->text3);
                d.txt(fromUtf8(l.message),
                      {cx + S(96), S((float)ly), cx + cw - S(16),
                       S((float)ly + 20)},
                      10.5f, P->text);
                ly += 24;
            }
        }
        y += 24 * rows + 12 + 8;
    }
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowButton(gp, cx, cw, y, L"清空事件记录", kIdClearLog);
        y += 8;
    }

    contentH_ = S((float)(y + 40));
}

void UiWindow::paintAbout(void* gp, int cx, int cw, int y) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    d.txt(L"关于", {cx, S(26), cx + cw, S(58)}, 20.f, P->text, 0, true);

    y = 96;

    // Icon + name block.
    {
        RECT tile = {cx, S((float)y), cx + S(64), S((float)y + 64)};
        d.fillRound(tile, 13.f * scale_, P->accent);
        const float ccx = (float)(cx + S(32)), ccy = (float)(S((float)y) + S(30));
        for (int i = -1; i <= 1; ++i)
            d.circle(ccx + (float)S(11 * i), ccy, 3.4f * scale_, 0xFFFFFFFF);
        d.line(ccx - (float)S(8), ccy + (float)S(10), ccx + (float)S(8),
               ccy + (float)S(10), 0xCCFFFFFF, 2.2f * scale_);

        d.txt(kAppName, {cx + S(80), S((float)y + 8), cx + cw, S((float)y + 34)},
              16.f, P->text, 0, true);
        d.txt(wfmt(L"版本 %ls · 单文件绿色版", kVersion),
              {cx + S(80), S((float)y + 34), cx + cw, S((float)y + 56)}, 10.5f,
              P->text3);
        y += 84;
    }

    y = groupHeader(gp, cx, cw, y, L"说明");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + 96)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        const wchar_t* notes[4] = {
            L"直接读取触摸板触点，不注入进程、不改系统文件。",
            L"运行时会临时关闭系统自带的三指/四指滑动，退出即还原。",
            L"关闭窗口只是收到托盘，手势继续工作。",
            L"所有设置保存在同目录的 tp_gestures.ini。",
        };
        int ny = y + 12;
        for (int i = 0; i < 4; ++i) {
            d.circle((float)(cx + S(22)), (float)(S(ny) + S(9)),
                     1.8f * scale_, P->text3);
            d.txt(notes[i],
                  {cx + S(34), S((float)ny), cx + cw - S(16),
                   S((float)ny + 18)},
                  10.5f, P->text2);
            ny += 21;
        }
        y += 96 + 8;
    }

    y = groupHeader(gp, cx, cw, y, L"当前设备");
    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH * 3)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowInfo(gp, cx, cw, y, L"触摸板",
                    hooks_->haveCaps
                        ? wfmt(L"%ls  (%04X:%04X)", hooks_->caps.product.c_str(),
                               hooks_->caps.vid, hooks_->caps.pid)
                        : std::wstring(L"未检测到"));
        y = rowInfo(gp, cx, cw, y, L"系统手势",
                    live_.suppressionActive ? L"已接管，退出时还原"
                                            : std::wstring(L"未接管"));
        y = rowInfo(gp, cx, cw, y, L"运行时长",
                    live_.startedAtMs
                        ? humanDuration(GetTickCount64() - live_.startedAtMs)
                        : std::wstring(L"—"));
        y += 8;
    }

    {
        RECT card = {cx, S((float)y), cx + cw, S((float)y + kRowH)};
        d.fillRound(card, (float)kCardRad * scale_, P->card);
        rowsInCard_ = 0;
        y = rowButton(gp, cx, cw, y, L"退出程序", kIdQuit, true);
        y += 8;
    }
    d.txt(L"退出会同时把系统手势设置还原。",
          {cx, S((float)y - 2), cx + cw, S((float)y + 18)}, 10.f, P->text3);

    contentH_ = S((float)(y + 40));
}

// ===========================================================================
//  Interaction
// ===========================================================================

int UiWindow::hitTest(POINT pt) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (PtInRect(&it->rc, pt)) return it->id;
    return kIdNone;
}

void UiWindow::onMouseMove(POINT pt) {
    if (activeSlider_ != kIdNone) {
        setSliderFromPoint(activeSlider_, pt);
        return;
    }

    const int id = hitTest(pt);
    if (id != hover_) {
        hover_ = id;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void UiWindow::onLButtonDown(POINT pt) {
    const int id = hitTest(pt);
    pressed_ = id;

    if (id >= kIdSliderBase && id < kIdSliderBase + (int)sliderDefs_.size()) {
        activeSlider_ = id;
        SetCapture(hwnd_);
        setSliderFromPoint(id, pt);
        return;
    }

    if (id == kIdNone) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::onLButtonUp(POINT pt) {
    const int id = hitTest(pt);
    const int pressed = pressed_;
    pressed_ = kIdNone;

    if (activeSlider_ != kIdNone) {
        activeSlider_ = kIdNone;
        ReleaseCapture();
        applyAndSave();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (id != kIdNone && id == pressed) activate(id);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::onMouseWheel(int delta) {
    if (scrollMax_ <= 0) return;
    const int step = (int)lround(60 * scale_);
    scrollPx_ -= (delta > 0 ? 1 : -1) * step;
    if (scrollPx_ < 0) scrollPx_ = 0;
    if (scrollPx_ > scrollMax_) scrollPx_ = scrollMax_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::setSliderFromPoint(int id, POINT pt) {
    AppConfig* c = hooks_->cfg;
    if (!c) return;
    const int idx = id - kIdSliderBase;
    if (idx < 0 || idx >= (int)sliderDefs_.size()) return;
    const RECT& tr = sliderTrack_[idx];
    if (tr.right <= tr.left) return;

    float t = (float)(pt.x - tr.left) / (float)(tr.right - tr.left);
    t = std::min(1.f, std::max(0.f, t));

    const SliderDef& def = sliderDefs_[idx];
    const float v = def.lo + t * (def.hi - def.lo);

    switch (def.field) {
    case Field::Sens:       c->sensitivity = v; break;
    case Field::PinchSens:  c->pinchSensitivity = v; break;
    case Field::PinchDeadband: c->pinchDeadband = v / 100.f; break;
    case Field::Threshold:  c->thresholdX = v; c->thresholdY = v * 0.83f; break;
    case Field::Dominance:  c->dominance = dominanceOfDeg(v); break;
    case Field::TapMs:      c->tapMaxMs = (uint32_t)v; break;
    case Field::TapSlop:    c->tapSlop  = v; break;
    case Field::EdgeZone:   c->edgeZone = v / 100.f; break;
    case Field::AnimMs:     c->animMs = (uint16_t)v; break;
    default: return;
    }
    // Values take effect on the next frame because the engine reads AppConfig
    // through the hooks; only the persist step waits for mouse-up.
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::activate(int id) {
    AppConfig* c = hooks_->cfg;
    if (!c) return;

    if (id >= kIdNav0 && id <= kIdNav3) {
        tab_ = id - kIdNav0;
        scrollPx_ = 0;
        scrollMax_ = 0;
        if (tab_ == kAdvanced) logsDirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // ---- rows ----
    if (id >= kIdRowBase && id < kIdRowBase + (int)rowDefs_.size()) {
        const RowDef def = rowDefs_[(size_t)(id - kIdRowBase)];
        switch (def.field) {
        case Field::Enabled:
            c->enabled = !c->enabled;
            applyAndSave();
            updateTrayTip();
            statusMessage(c->enabled ? L"手势已启用" : L"手势已暂停");
            return;

        case Field::Drag3: {
            const bool on = (c->mode == Mode::Drag);
            c->mode = on ? Mode::Tile : Mode::Drag;
            applyAndSave();
            statusMessage(c->mode == Mode::Drag
                              ? L"三指按住可移动窗口，三指滑动已暂停"
                              : L"三指滑动按映射表执行，拖动窗口已关闭");
            return;
        }

        case Field::PinchEnabled:
            c->pinchEnabled = !c->pinchEnabled;
            applyAndSave();
            statusMessage(c->pinchEnabled ? L"双指缩放已开启"
                                          : L"双指缩放已关闭");
            return;

        case Field::PinchAnchored:
            c->pinchAnchored = !c->pinchAnchored;
            applyAndSave();
            return;

        case Field::PinchRubber:
            c->pinchRubberBand = !c->pinchRubberBand;
            applyAndSave();
            return;

        case Field::TapEnabled:
            c->tapEnabled = !c->tapEnabled;
            applyAndSave();
            statusMessage(c->tapEnabled ? L"轻点已开启" : L"轻点已关闭");
            return;

        case Field::EdgeEnabled:
            c->edgeEnabled = !c->edgeEnabled;
            if (c->edgeEnabled) c->edgeZone = std::max(c->edgeZone, 0.10f);
            applyAndSave();
            statusMessage(c->edgeEnabled
                              ? L"已开启边缘滑动，可在手势页单独绑定"
                              : L"已关闭边缘滑动");
            return;

        case Field::Autostart: {
            const bool wasOn = autostartEnabled();
            const bool ok    = setAutostart(!wasOn);
            if (ok)
                statusMessage(wasOn ? L"已取消开机自启" : L"已设置为开机自启");
            else
                statusMessage(L"写入启动项失败，可能被安全软件拦截", true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        case Field::Bind:
        case Field::Theme:
        case Field::PinchAxis:
        case Field::DragFingers:
            openDropdown(id);
            return;

        default:
            return;
        }
    }

    // ---- finger-count segments ----
    if (id >= kIdFingerSeg && id < kIdFingerSeg + slot::kFingerCounts) {
        fingerTab_ = id - kIdFingerSeg;
        scrollPx_  = 0;
        scrollMax_ = 0;
        if (fingerTab_ == 2 && hooks_->haveCaps && hooks_->caps.maxContacts < 5)
            statusMessage(L"这块触摸板最多 4 点触控，五指手势不会触发", true);
        else
            statusMessage(wfmt(L"正在设置 %d 指手势",
                               slot::kFirstFingers + fingerTab_));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // ---- buttons ----
    switch (id) {
    case kIdClearLog:
        logs_.clear();
        logsDirty_ = true;
        statusMessage(L"事件记录已清空");
        break;

    case kIdResetAll: {
        const AppConfig def;
        c->thresholdX  = def.thresholdX;
        c->thresholdY  = def.thresholdY;
        c->dominance   = def.dominance;
        c->sensitivity = def.sensitivity;
        c->pinchSensitivity = def.pinchSensitivity;
        c->pinchDeadband    = def.pinchDeadband;
        c->pinchAxis        = def.pinchAxis;
        c->tapMaxMs         = def.tapMaxMs;
        c->edgeZone         = def.edgeZone;
        c->animMs           = def.animMs;
        applyAndSave();
        statusMessage(L"已恢复推荐设置（映射与开关未改动）");
        break;
    }

    case kIdQuit:
        exitRequested_ = true;
        DestroyWindow(hwnd_);
        break;

    case kIdTestLeft:
    case kIdTestRight:
    case kIdTestDrag:
    case kIdTestPinch: {
        std::string msg;
        const char* cmd = id == kIdTestLeft    ? "swap-left"
                          : id == kIdTestRight ? "swap-right"
                          : id == kIdTestDrag  ? "test-drag"
                                               : "test-pinch";
        const bool ok = hooks_->onCommand && hooks_->onCommand(cmd, &msg);
        statusMessage(msg, !ok);
        break;
    }

    default:
        break;
    }
}

void UiWindow::applyAndSave() {
    if (hooks_ && hooks_->onApply) hooks_->onApply();

    // Switching appearance should be instant, not "on the next timer tick".
    const bool want = resolveLight(hooks_ ? hooks_->cfg : nullptr);
    if (want != light_) {
        light_ = want;
        P      = light_ ? &kLight : &kDark;
        if (hwnd_) applyTitleBarTheme(hwnd_, light_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::statusMessage(const std::wstring& text, bool isError) {
    status_ = text;
    statusError_ = isError;
    statusAt_ = GetTickCount64();
    notifyNewLog();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::statusMessage(const std::string& utf8, bool isError) {
    statusMessage(fromUtf8(utf8), isError);
}

// ===========================================================================
//  Live data
// ===========================================================================

void UiWindow::updateLive(const UiLive& live) {
    const bool framesChanged = live.frames != lastFrameSeen_;
    if (framesChanged) {
        lastFrameSeen_ = live.frames;
        lastDataAt_ = GetTickCount64();
    }

    const bool stateChanged =
        live.dragging != live_.dragging ||
        live.pinching != live_.pinching ||
        live.suppressionActive != live_.suppressionActive;

    live_ = live;

    if (!hwnd_) return;
    const uint64_t now = GetTickCount64();
    // The sidebar shows a live state dot, so both pages that display live data
    // need the repaint, not just one.
    if ((framesChanged || stateChanged) && now - lastLivePaint_ >= 40) {
        lastLivePaint_ = now;
        if (tab_ == kAdvanced || stateChanged)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void UiWindow::notifyNewLog() {
    logsDirty_ = true;
    if (tab_ == kAdvanced && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ===========================================================================
//  Dropdown
// ===========================================================================

void UiWindow::openDropdown(int rowId) {
    if (!drop_) {
        printf("[ui] dropdown window missing\n");
        return;
    }
    AppConfig* c = hooks_->cfg;
    if (!c) return;

    RECT cell = {0, 0, 0, 0};
    for (const auto& h : hits_)
        if (h.id == rowId) {
            cell = h.rc;
            break;
        }
    if (cell.right <= cell.left) return;

    dropRow_   = rowId;
    dropHover_ = -1;
    dropScroll_ = 0;
    dropItems_.clear();

    const RowDef def =
        (rowId >= kIdRowBase && rowId < kIdRowBase + (int)rowDefs_.size())
            ? rowDefs_[(size_t)(rowId - kIdRowBase)]
            : RowDef{};
    dropField_ = def.field;
    dropArg_   = def.arg;

    dropIndexMenu_ = (def.field != Field::Bind);

    if (def.field == Field::Bind) {
        const int  s     = def.arg;
        const int  kind  = slot::kindOf(s);
        // "Drag" is offered only where it can actually do something: a
        // three-finger swipe. A menu full of dead options is worse than a
        // shorter one.
        const bool swipe = kind >= slot::SwipeLeft && kind <= slot::SwipeDown;
        for (const auto& group : actionGroups(swipe)) {
            DropItem head;
            head.header = true;
            head.label  = group.title;
            dropItems_.push_back(head);
            for (ActionId a : group.items) {
                DropItem it;
                it.action   = a;
                it.label    = actionText(a);
                it.selected = slot::valid(s) && c->bind[s] == a;
                dropItems_.push_back(it);
            }
        }
        // A bound slot gets one extra row to unbind it without hunting for
        // "无操作" at the top.
        if (slot::valid(s) && c->bind[s] != ActionId::None) {
            DropItem clear;
            clear.label  = L"清除这个手势的绑定";
            clear.danger = true;
            dropItems_.push_back(clear);
        }
    } else {
        // Short fixed lists: appearance, pinch axis, drag finger count.
        const wchar_t* names[4] = {nullptr, nullptr, nullptr, nullptr};
        int count = 0, cur = 0;
        if (def.field == Field::Theme) {
            static const wchar_t* t[3] = {L"跟随系统", L"浅色", L"深色"};
            for (int i = 0; i < 3; ++i) names[i] = t[i];
            count = 3;
            cur   = c->theme > 2 ? 0 : c->theme;
        } else if (def.field == Field::PinchAxis) {
            static const wchar_t* t[4] = {L"等比缩放", L"跟随手指方向", L"仅横向",
                                          L"仅纵向"};
            for (int i = 0; i < 4; ++i) names[i] = t[i];
            count = 4;
            cur   = c->pinchAxis > 3 ? 0 : c->pinchAxis;
        } else if (def.field == Field::DragFingers) {
            static const wchar_t* t[2] = {L"三指", L"四指"};
            for (int i = 0; i < 2; ++i) names[i] = t[i];
            count = 2;
            cur   = (c->dragFingers == 4) ? 1 : 0;
        }
        for (int i = 0; i < count; ++i) {
            DropItem it;
            it.index    = i;
            it.label    = names[i];
            it.selected = (i == cur);
            dropItems_.push_back(it);
        }
    }

    // ---- layout ----
    const int itemH = (int)lround(30 * scale_);
    const int headH = (int)lround(24 * scale_);
    const int padY  = (int)lround(6 * scale_);
    int top = padY;
    for (auto& it : dropItems_) {
        it.top = top;
        it.h   = it.header ? headH : itemH;
        top += it.h;
    }
    dropContentH_ = top + padY;

    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int maxH = sh - (int)lround(40 * scale_);
    const int h = std::min(dropContentH_, maxH);
    dropViewH_      = h;
    dropMaxScroll_  = std::max(0, dropContentH_ - h);

    const int w = dropIndexMenu_
                      ? (int)lround((def.field == Field::PinchAxis ? 232 : 196) *
                                    scale_)
                      : std::max((int)(cell.right - cell.left),
                                 (int)lround(300 * scale_));

    // Anchor in client coordinates: the popup is a separate window, so the
    // cell rect has to be mapped to the screen.
    POINT tl = {cell.left, cell.bottom + (int)lround(4 * scale_)};
    ClientToScreen(hwnd_, &tl);
    int y = tl.y;
    if (y + h > sh - 8) {
        POINT above = {cell.left, cell.top - (int)lround(4 * scale_)};
        ClientToScreen(hwnd_, &above);
        y = above.y - h;
    }
    // Flipping above is not always enough: the action table is taller than the
    // space on either side of the row. Clamping keeps it on screen, and the
    // menu scrolls if even that is not enough.
    const int maxY = sh - h - (int)lround(8 * scale_);
    if (y > maxY) y = maxY;
    if (y < (int)lround(8 * scale_)) y = (int)lround(8 * scale_);

    // Keep the menu inside the window's horizontal span where possible.
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    if (tl.x + w > sw - 8) tl.x = sw - w - 8;
    if (tl.x < 8) tl.x = 8;

    SetWindowPos(drop_, HWND_TOPMOST, tl.x, y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetCapture(drop_);
    InvalidateRect(drop_, nullptr, FALSE);
}

void UiWindow::closeDropdown() {
    if (!drop_ || !IsWindowVisible(drop_)) return;
    if (GetCapture() == drop_) ReleaseCapture();
    ShowWindow(drop_, SW_HIDE);
    dropRow_ = kIdNone;
    dropHover_ = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::commitDropdown(int itemIndex) {
    AppConfig* c = hooks_->cfg;
    const int   row   = dropRow_;
    const Field field = dropField_;
    const int   arg   = dropArg_;
    DropItem    item;
    if (itemIndex >= 0 && itemIndex < (int)dropItems_.size())
        item = dropItems_[(size_t)itemIndex];
    closeDropdown();
    if (!c || item.header) return;
    if (row < kIdRowBase || row >= kIdRowBase + (int)rowDefs_.size()) return;

    // ---- short index menus (appearance, axis, drag fingers) ----
    if (dropIndexMenu_) {
        switch (field) {
        case Field::Theme:
            c->theme = static_cast<uint8_t>(item.index < 0 ? 0 : item.index);
            applyAndSave();
            statusMessage(item.index == 0 ? L"外观跟随系统"
                                          : (item.index == 1 ? L"外观已切换为浅色"
                                                             : L"外观已切换为深色"));
            return;
        case Field::PinchAxis: {
            static const wchar_t* kAxisNames[4] = {L"等比缩放", L"跟随手指方向",
                                                   L"仅横向", L"仅纵向"};
            const int ai = (item.index >= 0 && item.index < 4) ? item.index : 0;
            c->pinchAxis = static_cast<uint8_t>(ai);
            applyAndSave();
            statusMessage(std::wstring(L"缩放方向：") + kAxisNames[ai]);
            return;
        }
        case Field::DragFingers:
            c->dragFingers = (item.index == 1) ? 4 : 3;
            applyAndSave();
            statusMessage(c->dragFingers == 4
                              ? L"四指按住移动窗口，三指滑动按映射表执行"
                              : L"三指按住移动窗口");
            return;
        default:
            return;
        }
    }

    // ---- a mapping row ----
    if (field != Field::Bind || !slot::valid(arg)) return;
    const int      s    = arg;
    const ActionId prev = c->bind[s];

    if (item.danger) {
        c->bind[s] = ActionId::None;
        applyAndSave();
        statusMessage(L"已清除：" + slotLabelW(s));
        logMapping(slotLabelW(s) + L" → 无操作");
        return;
    }

    const ActionId a = item.action;
    c->bind[s] = a;

    if (a == ActionId::Custom) {
        // Deliberately not persisted yet: sanitize() drops a custom binding with
        // no chord behind it, and the chord is exactly what the recorder is
        // about to collect.
        recordPrev_ = prev;
        startRecording(s);
        return;
    }
    if (a == ActionId::Launch) {
        chooseProgram(s, prev);
        return;
    }

    applyAndSave();
    const std::wstring line = slotLabelW(s) + L" → " + actionText(a);
    statusMessage(L"已设置：" + line);
    logMapping(line);
}

void UiWindow::chooseProgram(int s, ActionId prev) {
    AppConfig* c = hooks_ ? hooks_->cfg : nullptr;
    if (!c || !slot::valid(s)) return;

    wchar_t buf[MAX_PATH * 2] = {};
    if (!c->launch[s].empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, c->launch[s].c_str(), -1,
                                          buf, MAX_PATH * 2 - 1);
        if (n <= 0) buf[0] = 0;
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFilter = L"程序 (*.exe;*.lnk;*.bat)\0*.exe;*.lnk;*.bat\0所有文件\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH * 2;
    ofn.lpstrTitle  = L"选择这个手势要打开的程序";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        // Cancelled: the row must not be left claiming to launch something.
        c->bind[s] = prev;
        applyAndSave();
        statusMessage(L"已取消选择");
        return;
    }

    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr,
                                      nullptr);
    std::string path;
    if (n > 1) {
        path.resize(static_cast<size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, &path[0], n, nullptr, nullptr);
    }

    c->launch[s] = path;
    c->bind[s]   = ActionId::Launch;
    applyAndSave();

    const size_t slash = path.find_last_of("\\/");
    const std::wstring line =
        slotLabelW(s) + L" → 打开 " +
        fromUtf8(slash == std::string::npos ? path : path.substr(slash + 1));
    statusMessage(L"已设置：" + line);
    logMapping(line);
}

void UiWindow::logMapping(const std::wstring& line) {
    if (!hooks_ || !hooks_->onLog) return;
    char utf8[512];
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8, sizeof(utf8),
                        nullptr, nullptr);
    hooks_->onLog("gesture", std::string("映射已修改：") + utf8);
}

void UiWindow::startRecording(int slot) {
    if (slot < 0 || slot >= slot::Count) return;
    recordSlot_ = slot;
    // The recorder needs the keyboard, which means stealing focus. Remember
    // who had it: the whole point of a chord gesture is where it lands later.
    recordPrevFocus_ = GetForegroundWindow();
    statusMessage(L"请按下要发送的按键组合，Esc 取消");
    if (hwnd_) {
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void UiWindow::finishRecording(uint32_t vk) {
    const int slot = recordSlot_;
    recordSlot_    = -1;
    AppConfig* c   = hooks_ ? hooks_->cfg : nullptr;
    if (slot < 0 || !c) return;

    // Recording is over in every branch below, and the keyboard focus must go
    // back to the window the user was working in -- otherwise the chord they
    // just bound would fire into the settings window the first time.
    auto restoreFocus = [&]() {
        if (recordPrevFocus_ && IsWindow(recordPrevFocus_) &&
            recordPrevFocus_ != hwnd_) {
            SetForegroundWindow(recordPrevFocus_);
        }
        recordPrevFocus_ = nullptr;
    };

    if (vk == VK_ESCAPE) {
        // Put the previous binding back: the row must not be left showing a
        // custom action with nothing behind it.
        c->bind[slot]   = recordPrev_;
        c->custom[slot] = KeyChord{};
        applyAndSave();
        statusMessage(L"已取消录制");
        InvalidateRect(hwnd_, nullptr, FALSE);
        restoreFocus();
        return;
    }

    // A modifier on its own is not a binding; keep waiting for the real key.
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN ||
        vk == VK_RWIN || vk == VK_CAPITAL || vk == VK_NUMLOCK ||
        vk == VK_SCROLL) {
        recordSlot_ = slot;
        return;
    }

    KeyChord k;
    if (GetKeyState(VK_CONTROL) & 0x8000) k.mods |= KeyChord::kCtrl;
    if (GetKeyState(VK_MENU) & 0x8000)    k.mods |= KeyChord::kAlt;
    if (GetKeyState(VK_SHIFT) & 0x8000)   k.mods |= KeyChord::kShift;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
        k.mods |= KeyChord::kWin;
    k.vk = static_cast<uint16_t>(vk);

    c->custom[slot] = k;
    applyAndSave();
    statusMessage(L"已录制：" + fromUtf8(chordToString(k)));
    InvalidateRect(hwnd_, nullptr, FALSE);
    restoreFocus();

    if (hooks_ && hooks_->onLog) {
        char utf8[256];
        const std::wstring line = L"自定义快捷键：" + fromUtf8(chordToString(k));
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8, 256, nullptr,
                            nullptr);
        hooks_->onLog("gesture", std::string("映射已修改：") + utf8);
    }
}

void UiWindow::paintDropdown(void* gp, int w, int h) {
    Draw d;
    d.g = static_cast<Graphics*>(gp);
    d.s = scale_;
    auto S = [&](float v) { return (int)lround(v * scale_); };

    d.fillRound({0, 0, w, h}, 8.f * scale_, P->card);
    d.strokeRound({0, 0, w, h}, 8.f * scale_, P->sep);
    d.g->SetClip(RectF(0.f, 0.f, (REAL)w, (REAL)h));

    const int padX = S(6);
    const int itemH = S(30);

    // Grouped action menu: captions between groups are drawn but never
    // hoverable, and "danger" (clear binding) reads as a warning, not a choice.
    for (size_t i = 0; i < dropItems_.size(); ++i) {
        const DropItem& it = dropItems_[i];
        const int y = it.top - dropScroll_;
        if (y + it.h < 0 || y > h) continue;   // virtualised: skip offscreen rows

        if (it.header) {
            d.txt(it.label.c_str(), {padX + S(10), y, w - padX - S(10), y + it.h},
                  9.5f, P->text3, 0, true);
            continue;
        }

        RECT row = {padX, y, w - padX, y + it.h};
        const bool hovered = ((int)i == dropHover_);
        if (hovered) d.fillRound(row, 5.f * scale_, P->accent);

        const ARGB fg = hovered ? 0xFFFFFFFF
                                : (it.danger ? P->warn : P->text);
        d.txt(it.label.c_str(),
              {row.left + S(10), row.top, row.right - S(26), row.bottom},
              11.5f, fg, 0, it.selected);

        if (it.selected) {
            const float ax = (float)(row.right - S(14));
            const float ay = (float)(row.top + itemH / 2);
            const ARGB tick = hovered ? 0xFFFFFFFF : P->accent;
            d.line(ax - (float)S(5), ay, ax - (float)S(1), ay + (float)S(4),
                   tick, 1.9f * scale_);
            d.line(ax - (float)S(1), ay + (float)S(4), ax + (float)S(5),
                   ay - (float)S(3), tick, 1.9f * scale_);
        }
    }

    // A scrollbar is the only honest signal that there are more choices below
    // when the list is taller than the screen allows.
    if (dropMaxScroll_ > 0) {
        const float view = (float)dropViewH_;
        const float frac = view / (float)dropContentH_;
        const float barH = std::max(view * frac, 24.f * scale_);
        const float maxT = view - barH;
        const float t = maxT * ((float)dropScroll_ / (float)dropMaxScroll_);
        d.fillRound({w - S(4), (int)t, w - S(2), (int)(t + barH)},
                    1.5f * scale_, P->text3);
    }

    d.g->ResetClip();
}

LRESULT UiWindow::handleDrop(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        Bitmap buf(std::max(1L, rc.right), std::max(1L, rc.bottom),
                   PixelFormat32bppRGB);
        Graphics g(&buf);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        paintDropdown(&g, rc.right, rc.bottom);
        Graphics screen(hdc);
        screen.DrawImage(&buf, 0, 0);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        // Rows are not evenly spaced (group captions sit between them), so the
        // hover target is whichever item rect actually contains the cursor.
        const int my = GET_Y_LPARAM(lp) + dropScroll_;
        int idx = -1;
        for (int i = 0; i < (int)dropItems_.size(); ++i) {
            const DropItem& it = dropItems_[(size_t)i];
            if (!it.header && my >= it.top && my < it.top + it.h) {
                idx = i;
                break;
            }
        }
        if (idx != dropHover_) {
            dropHover_ = idx;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        // Positive delta scrolls up. The list is clamped the same way the main
        // scroll area is, so a trackpad flick cannot overscroll the menu.
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        const int step  = (int)lround(40 * scale_);
        int ns = dropScroll_ - (delta > 0 ? step : -step);
        if (ns < 0) ns = 0;
        if (ns > dropMaxScroll_) ns = dropMaxScroll_;
        if (ns != dropScroll_) {
            dropScroll_ = ns;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const int my = GET_Y_LPARAM(lp) + dropScroll_;
        int idx = -1;
        for (int i = 0; i < (int)dropItems_.size(); ++i) {
            const DropItem& it = dropItems_[(size_t)i];
            if (!it.header && my >= it.top && my < it.top + it.h) {
                idx = i;
                break;
            }
        }
        if (idx >= 0)
            commitDropdown(idx);
        else
            closeDropdown();
        return 0;
    }

    case WM_CAPTURECHANGED:
        // Fires when the user clicks elsewhere; hide without touching capture
        // again (that is what caused a re-entrancy loop).
        if (dropRow_ != kIdNone) {
            ShowWindow(h, SW_HIDE);
            dropRow_ = kIdNone;
            dropHover_ = -1;
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) closeDropdown();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ===========================================================================
//  Tray
// ===========================================================================

void UiWindow::addTrayIcon() {
    if (!hwnd_) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd_;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon            = appIcon_;
    lstrcpynW(nid.szTip, L"Fuck-Windows-Touchpad · 运行中", 128);

    if (Shell_NotifyIconW(trayAdded_ ? NIM_MODIFY : NIM_ADD, &nid))
        trayAdded_ = true;

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void UiWindow::removeTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void UiWindow::updateTrayTip() {
    if (!trayAdded_ || !hwnd_) return;
    const bool en = hooks_ && hooks_->cfg ? hooks_->cfg->enabled : true;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = 1;
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, en ? L"Fuck-Windows-Touchpad · 接管中" : L"Fuck-Windows-Touchpad · 已暂停",
              128);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void UiWindow::showTrayMenu(POINT pt) {
    AppConfig* c = hooks_ ? hooks_->cfg : nullptr;
    const bool en = c ? c->enabled : true;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (en ? MF_CHECKED : 0), 1,
                en ? L"手势已启用（点击暂停）" : L"手势已暂停（点击启用）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"打开设置");
    AppendMenuW(menu, MF_STRING, 3, L"与左侧窗口互换");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"退出程序");

    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x,
                                    pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    switch (cmd) {
    case 1:
        if (c) {
            c->enabled = !c->enabled;
            applyAndSave();
            updateTrayTip();
            statusMessage(c->enabled ? L"手势已启用" : L"手势已暂停");
        }
        break;
    case 2:
        show();
        break;
    case 3: {
        std::string msg;
        if (hooks_->onCommand) hooks_->onCommand("swap-left", &msg);
        break;
    }
    case 4:
        exitRequested_ = true;
        DestroyWindow(hwnd_);
        break;
    default:
        break;
    }
}

// ===========================================================================
//  Show / hide
// ===========================================================================

void UiWindow::show() {
    if (!hwnd_) return;
    trayOnly_ = false;
    ShowWindow(hwnd_, SW_SHOW);
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiWindow::hideToTray() {
    if (!hwnd_) return;
    trayOnly_ = true;
    ShowWindow(hwnd_, SW_HIDE);
    if (!trayAdded_) addTrayIcon();
}
