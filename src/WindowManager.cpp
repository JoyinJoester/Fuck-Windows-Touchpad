#include "WindowManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <dwmapi.h>

namespace {

constexpr int      kMinSide     = 40;  // ignore slivers / tooltip-sized windows
constexpr int      kDirSlop     = 24;  // must be at least this far on that side

// Animation length, settable from the UI. 0 means "no animation at all":
// startAnim() then jumps straight to the target.
uint32_t g_animMs = 260;

struct Anim {
    HWND     hwnd      = nullptr;
    RECT     from{};
    RECT     to{};
    uint64_t start     = 0;
    uint64_t lastApply = 0;
    uint32_t dur       = 260;
};

HWND              g_lastActive = nullptr;
HWINEVENTHOOK     g_hook       = nullptr;
std::vector<Anim> g_anims;

// Drag session state.
HWND   g_dragHwnd  = nullptr;
RECT   g_dragRect{};          // rect at the moment the drag began
float  g_scaleX    = 1.f;     // pixels per device unit
float  g_scaleY    = 1.f;
float  g_accX      = 0.f;     // accumulated pixels, keeps sub-pixel precision
float  g_accY      = 0.f;
int    g_dragAtX   = 0;       // last position the clamp actually allowed
int    g_dragAtY   = 0;
uint32_t g_rangeX  = 0;       // touchpad logical range, 0 = unknown
uint32_t g_rangeY  = 0;

// Pinch session state. The start rect and work area are latched when the
// gesture begins; every frame only the cumulative factor changes, so the
// window can never drift from repeated relative resizing.
constexpr int kPinchMinW = 320;
constexpr int kPinchMinH = 240;

HWND   g_pinchHwnd = nullptr;
RECT   g_pinchRect{};
RECT   g_pinchWork{};
// The two axes carry their own factor. A uniform pinch multiplies both by the
// same number, but a horizontal one leaves `g_pinchScaleY` alone, which is what
// lets the aspect ratio be set by hand.
double g_pinchScaleX = 1.0;
double g_pinchScaleY = 1.0;
// Last rect handed to the window, so a frame where the size did not actually
// change (the gap moved less than one pixel, or a limit was hit) does not send
// another SetWindowPos.
RECT   g_pinchSent{};
bool   g_pinchSentValid = false;

// ---------------------------------------------------------------- easing ----
// cubic-bezier(0.05, 0.9, 0.1, 1.05): the "wind" curve used by Hyprland
// configs (dots-hyprland among them). It overshoots slightly before settling,
// which is what makes a window swap read as motion instead of a teleport.
double bezAxis(double t, double p1, double p2) {
    const double mt = 1.0 - t;
    return 3.0 * mt * mt * t * p1 + 3.0 * mt * t * t * p2 + t * t * t;
}

double easeWind(double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    // Bisection on the x axis; 24 iterations is well past 1e-6 precision.
    double lo = 0.0, hi = 1.0, t = x;
    for (int i = 0; i < 24; ++i) {
        t = 0.5 * (lo + hi);
        if (bezAxis(t, 0.05, 0.10) < x) lo = t; else hi = t;
    }
    return bezAxis(t, 0.90, 1.05);
}

int lerpI(int a, int b, double e) {
    return (int)std::lround((double)a + ((double)b - (double)a) * e);
}

// ------------------------------------------------------------- filtering ----

bool isOurProcess(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

bool isShellWindow(HWND h) {
    wchar_t cls[128] = {};
    if (!GetClassNameW(h, cls, 128)) return false;
    // Terminal and conhost own the console window even when the process that
    // created it is ours, so the PID check alone does not exclude it.
    static const wchar_t* kSkip[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"ConsoleWindowClass", L"CASCADIA_HOSTING_WINDOW_CLASS"};
    for (const wchar_t* s : kSkip)
        if (wcscmp(cls, s) == 0) return true;
    return false;
}

// A window is a swap candidate only if the user would think of it as a window:
// visible, top level, not a tool panel, not suspended, not minimized, and not
// maximized (a maximized window ignores positional moves).
bool isTileable(HWND h) {
    if (!h || !IsWindow(h)) return false;
    if (!IsWindowVisible(h) || IsIconic(h)) return false;
    if (GetAncestor(h, GA_ROOT) != h) return false;
    if (isOurProcess(h) || isShellWindow(h)) return false;

    const LONG st = GetWindowLongW(h, GWL_STYLE);
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(st & WS_VISIBLE) || (st & WS_CHILD)) return false;
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;

    // UWP apps that are suspended stay "visible" but are cloaked by the DWM.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked)
        return false;

    RECT r{};
    if (!GetWindowRect(h, &r)) return false;
    if (r.right - r.left < kMinSide || r.bottom - r.top < kMinSide) return false;

    return true;
}

BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    if (isTileable(h)) static_cast<std::vector<HWND>*>((void*)lp)->push_back(h);
    return TRUE;
}

std::vector<HWND> candidates() {
    std::vector<HWND> v;
    EnumWindows(enumProc, (LPARAM)&v);
    return v;
}

// Topmost window in z-order that the user would consider an app window. Used as
// the starting target when the app is launched from a console, because then the
// foreground window is our own terminal and there is nothing remembered yet.
HWND topmostTileable() {
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        if (isTileable(h)) return h;
    return nullptr;
}

// Window the user is pointing at. This is what makes the gesture feel like
// macOS: three fingers grab whatever is under the cursor, not "the last window
// that had focus".
HWND windowUnderCursor() {
    POINT pt{};
    if (!GetCursorPos(&pt)) return nullptr;
    HWND h = WindowFromPoint(pt);
    if (!h) return nullptr;
    HWND root = GetAncestor(h, GA_ROOT);
    return root ? root : h;
}

HWND resolveTarget() {
    HWND from = isTileable(g_lastActive) ? g_lastActive : nullptr;
    if (!from) {
        HWND fg = GetForegroundWindow();
        if (isTileable(fg)) from = fg;
    }
    if (!from) from = topmostTileable();
    return from;
}

std::string titleOf(HWND h) {
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, 256);
    if (!buf[0]) return "<untitled>";
    // Narrow it for the console; titles here are display-only.
    char out[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out) - 1, nullptr,
                        nullptr);
    return out;
}

// Maximized means two different things in practice: a real WS_MAXIMIZE window,
// and the borderless fullscreen used by Chrome/Electron apps, which report
// nearly the whole work area without the style flag. Both behave the same way
// here -- they cannot be repositioned until they are restored.
bool isEffectivelyMaximized(HWND h) {
    if (IsZoomed(h)) return true;
    RECT r{};
    if (!GetWindowRect(h, &r)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    const RECT& w = mi.rcWork;
    const long  aw = w.right - w.left, ah = w.bottom - w.top;
    const long  rw = r.right - r.left, rh = r.bottom - r.top;
    if (aw <= 0 || ah <= 0) return false;
    return rw >= aw * 98 / 100 && rh >= ah * 98 / 100;
}

// Nearest window on the requested side: horizontal distance dominates, vertical
// offset is a penalty, and sharing a monitor is strongly preferred so a swipe
// does not reach across displays by accident. When the source is maximized its
// centre sits in the middle of the screen and a strict direction test rejects
// everything, so `relaxed` drops the slop requirement.
HWND findNeighbor(HWND from, SwapDir dir, bool relaxed) {
    RECT fr{};
    if (!GetWindowRect(from, &fr)) return nullptr;
    const POINT fc{(fr.left + fr.right) / 2, (fr.top + fr.bottom) / 2};
    const HMONITOR fm = MonitorFromWindow(from, MONITOR_DEFAULTTONEAREST);

    HWND   best       = nullptr;
    double bestScore  = 0.0;

    for (HWND w : candidates()) {
        if (w == from) continue;
        RECT r{};
        if (!GetWindowRect(w, &r)) continue;

        const POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const int   dx = c.x - fc.x;
        if (!relaxed) {
            if (dir == SwapDir::Right ? dx < kDirSlop : dx > -kDirSlop) continue;
        }

        const int overlapV = std::min(fr.bottom, r.bottom) - std::max(fr.top, r.top);

        double score = std::abs((double)dx) + 1.2 * std::abs((double)(c.y - fc.y));
        if (overlapV > 0) score -= 240.0;  // side-by-side beats diagonal
        if (MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST) != fm)
            score += 4000.0;

        if (!best || score < bestScore) {
            best = w;
            bestScore = score;
        }
    }
    return best;
}

void startAnim(HWND h, const RECT& from, const RECT& to) {
    if (g_animMs == 0) {
        // Animation disabled: place it and be done.
        SetWindowPos(h, nullptr, to.left, to.top, to.right - to.left,
                     to.bottom - to.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        return;
    }
    Anim a;
    a.hwnd  = h;
    a.from  = from;
    a.to    = to;
    a.start = GetTickCount64();
    a.dur   = g_animMs;
    g_anims.push_back(a);
}

void CALLBACK onForeground(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG,
                           DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (!hwnd || !IsWindow(hwnd)) return;
    if (isOurProcess(hwnd) || !IsWindowVisible(hwnd)) return;
    g_lastActive = hwnd;
}

}  // namespace

namespace wm {

void init() {
    g_lastActive = GetForegroundWindow();
    if (g_lastActive && isOurProcess(g_lastActive)) g_lastActive = nullptr;
    // Launched from a terminal: the foreground window is excluded, so fall back
    // to the topmost app window rather than starting with no target at all.
    if (!isTileable(g_lastActive)) g_lastActive = topmostTileable();

    g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                             nullptr, onForeground, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void shutdown() {
    if (g_hook) {
        UnhookWinEvent(g_hook);
        g_hook = nullptr;
    }
    g_anims.clear();
    g_dragHwnd = nullptr;
    g_pinchHwnd = nullptr;
    g_pinchSentValid = false;
}

HWND lastActive() { return g_lastActive; }

bool busy() { return !g_anims.empty(); }

void setTouchpadRange(uint32_t maxX, uint32_t maxY) {
    g_rangeX = maxX;
    g_rangeY = maxY;
}

bool dragging() { return g_dragHwnd != nullptr; }

HWND dragTarget() { return g_dragHwnd; }

std::string dragTargetTitle() {
    return g_dragHwnd ? titleOf(g_dragHwnd) : std::string();
}

// ------------------------------------------------------------------ drag ---

namespace {

// Does a strip of the window's caption still sit on a real monitor? Screen
// layouts are not rectangles -- two displays of different heights leave a gap
// that belongs to no monitor at all -- so a bounding-box test would happily
// accept a window dropped into empty space that neither screen can show.
struct CaptionProbe {
    RECT strip{};
    int  w = 0;
    int  h = 0;
};

BOOL CALLBACK captionProbeProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* p = reinterpret_cast<CaptionProbe*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;

    RECT isect{};
    if (!IntersectRect(&isect, &p->strip, &mi.rcMonitor)) return TRUE;
    const int iw = isect.right - isect.left;
    const int ih = isect.bottom - isect.top;
    // Keep the best single-monitor hit: a caption spanning two screens can be
    // unreachable on either of them.
    if (iw > p->w) {
        p->w = iw;
        p->h = ih;
    }
    return TRUE;
}

// True when the caption is still grabbable at (x, y) with this size. Used as a
// movement filter rather than a clamp: the window simply stops where it is
// instead of sliding along an invisible wall, which is what the shell does.
bool captionReachable(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    const int barH = std::min(28, h);
    const RECT strip = {x, y, x + w, y + barH};

    CaptionProbe p;
    p.strip = strip;
    EnumDisplayMonitors(nullptr, nullptr, captionProbeProc,
                        reinterpret_cast<LPARAM>(&p));

    const int needW = std::min(140, w);
    const int needH = std::min(12, barH);
    return p.w >= needW && p.h >= needH;
}

}  // namespace

bool dragBegin(std::string* message) {
    auto fail = [&](const char* m) {
        if (message) *message = m;
        return false;
    };

    if (g_dragHwnd) dragEnd();

    HWND target = windowUnderCursor();
    if (!isTileable(target)) target = resolveTarget();
    if (!target) return fail("no window under the cursor to drag");

    // A maximized window ignores positional moves. Restoring it is also what
    // Windows itself does when you drag a maximized title bar, so it matches
    // the behaviour users already expect.
    const bool wasMax = isEffectivelyMaximized(target);
    if (wasMax) ShowWindow(target, SW_RESTORE);

    RECT r{};
    if (!GetWindowRect(target, &r)) return fail("GetWindowRect failed");

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST),
                         &mi))
        return fail("GetMonitorInfo failed");

    const long ww = mi.rcWork.right - mi.rcWork.left;
    const long wh = mi.rcWork.bottom - mi.rcWork.top;
    g_scaleX = (g_rangeX > 0 && ww > 0) ? (float)ww / (float)g_rangeX : 1.f;
    g_scaleY = (g_rangeY > 0 && wh > 0) ? (float)wh / (float)g_rangeY : 1.f;

    g_dragHwnd = target;
    g_dragRect = r;
    g_accX     = 0.f;
    g_accY     = 0.f;
    g_dragAtX  = r.left;
    g_dragAtY  = r.top;
    // A drag and a swap animation must never fight over the same window.
    g_anims.clear();

    if (message) {
        char buf[512];
        snprintf(buf, sizeof(buf), "dragging \"%s\"%s", titleOf(target).c_str(),
                 wasMax ? " (restored from maximized)" : "");
        *message = buf;
    }
    return true;
}

void dragMove(float dxUnits, float dyUnits) {
    if (!g_dragHwnd) return;
    if (!IsWindow(g_dragHwnd)) {
        g_dragHwnd = nullptr;
        return;
    }

    // Sub-pixel travel is kept in the accumulator; rounding every frame would
    // silently swallow slow drags.
    g_accX += dxUnits * g_scaleX;
    g_accY += dyUnits * g_scaleY;

    const int nx = g_dragRect.left + (int)std::lround(g_accX);
    const int ny = g_dragRect.top + (int)std::lround(g_accY);

    const int w = g_dragRect.right - g_dragRect.left;
    const int h = g_dragRect.bottom - g_dragRect.top;

    // Refuse positions that would leave the window unreachable. Windows' own
    // title-bar drag never loses a window because the shell keeps the caption
    // on screen; by moving the window ourselves we lose that safety net, so it
    // has to be rebuilt here.
    //
    // The accumulator is rewound to the last accepted position rather than just
    // skipped: letting it run on would mean the gesture drifts further into
    // nowhere behind the window, and the first centimetre of the way back would
    // move nothing.
    if (!captionReachable(nx, ny, w, h)) {
        g_accX = (float)(g_dragAtX - g_dragRect.left);
        g_accY = (float)(g_dragAtY - g_dragRect.top);
        return;
    }
    g_dragAtX = nx;
    g_dragAtY = ny;

    SetWindowPos(g_dragHwnd, nullptr, nx, ny, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_ASYNCWINDOWPOS);
}

void dragEnd() {
    g_dragHwnd = nullptr;
    g_accX     = 0.f;
    g_accY     = 0.f;
}

// ------------------------------------------------------------------ pinch ---

namespace {

// Where the fingers grabbed the window, as a fraction of its width/height. That
// point stays fixed on screen while the window scales, which is what makes the
// gesture feel pulled out from between the fingers instead of the window
// inflating around its own centre.
double g_pinchAx           = 0.5;
double g_pinchAy           = 0.5;
bool   g_pinchAnchored     = true;
bool   g_pinchRubber       = true;

// Size limits expressed as scale factors relative to the rect the session
// started from. Each axis is clamped on its own: sharing one range (the previous
// behaviour) meant a wide window could never get narrower than the height limit
// allowed, which is precisely what made the two axes impossible to control
// separately.
void pinchLimits(double* loX, double* hiX, double* loY, double* hiY) {
    const double baseW =
        std::max(1.0, (double)(g_pinchRect.right - g_pinchRect.left));
    const double baseH =
        std::max(1.0, (double)(g_pinchRect.bottom - g_pinchRect.top));
    const double maxW =
        std::max(1L, g_pinchWork.right - g_pinchWork.left);
    const double maxH =
        std::max(1L, g_pinchWork.bottom - g_pinchWork.top);

    *loX = (double)kPinchMinW / baseW;
    *hiX = maxW / baseW;
    *loY = (double)kPinchMinH / baseH;
    *hiY = maxH / baseH;
    if (*hiX < *loX) *hiX = *loX;
    if (*hiY < *loY) *hiY = *loY;
}

// Compresses the part of `scale` that lies outside [lo, hi] instead of cutting
// it off, so pulling past a limit still moves the edge a little and springs
// back when the fingers let go.
double pinchSoftScale(double scale, double lo, double hi) {
    constexpr double kResist = 0.25;
    if (scale < lo) return lo - (lo - scale) * kResist;
    if (hi > lo && scale > hi) return hi + (scale - hi) * kResist;
    return scale;
}

// Rect for the given per-axis factors, anchored on the grab point and kept
// inside the work area.
RECT pinchRectFor(double sx, double sy) {
    const int baseW = g_pinchRect.right - g_pinchRect.left;
    const int baseH = g_pinchRect.bottom - g_pinchRect.top;
    int w = (int)std::lround((double)baseW * sx);
    int h = (int)std::lround((double)baseH * sy);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    const double ax = g_pinchAnchored ? g_pinchAx : 0.5;
    const double ay = g_pinchAnchored ? g_pinchAy : 0.5;
    int x = (int)std::lround((double)g_pinchRect.left + (double)baseW * ax -
                             (double)w * ax);
    int y = (int)std::lround((double)g_pinchRect.top + (double)baseH * ay -
                             (double)h * ay);

    if (x + w > g_pinchWork.right) x = g_pinchWork.right - w;
    if (y + h > g_pinchWork.bottom) y = g_pinchWork.bottom - h;
    if (x < g_pinchWork.left) x = g_pinchWork.left;
    if (y < g_pinchWork.top) y = g_pinchWork.top;

    RECT r = {x, y, x + w, y + h};
    return r;
}

}  // namespace

bool pinching() { return g_pinchHwnd != nullptr; }

std::string pinchTargetTitle() {
    return g_pinchHwnd ? titleOf(g_pinchHwnd) : std::string();
}

bool pinchBegin(std::string* message) {
    auto fail = [&](const char* m) {
        if (message) *message = m;
        return false;
    };

    if (g_pinchHwnd) pinchEnd();

    HWND target = windowUnderCursor();
    if (!isTileable(target)) target = resolveTarget();
    if (!target) return fail("no window under the cursor to resize");

    // A maximized window has no meaningful rect to scale, and Windows ignores
    // positional moves on it. Restoring first is also what the user means:
    // "pinch in to make this full-screen window smaller again".
    const bool wasMax = isEffectivelyMaximized(target);
    if (wasMax) {
        RECT before{};
        GetWindowRect(target, &before);
        ShowWindow(target, SW_RESTORE);
        // SW_RESTORE on a maximized window is asynchronous for some apps; wait
        // for the rect to actually change. Polling exits on the first frame the
        // restore lands, so the usual case costs ~4 ms instead of a fixed 32.
        for (int i = 0; i < 8; ++i) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            RECT now{};
            if (GetWindowRect(target, &now) && !EqualRect(&now, &before)) break;
            Sleep(4);
        }
    }

    RECT r{};
    if (!GetWindowRect(target, &r)) return fail("GetWindowRect failed");

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST),
                         &mi))
        return fail("GetMonitorInfo failed");

    // A drag and a resize must never fight over the same window.
    g_anims.clear();

    g_pinchHwnd  = target;
    g_pinchRect  = r;
    g_pinchWork  = mi.rcWork;
    g_pinchScaleX = 1.0;
    g_pinchScaleY = 1.0;
    g_pinchSentValid = false;

    // Grab point: the cursor sits where the fingers are, so it is the best
    // available guess at the point the user wants to stay put.
    g_pinchAx = 0.5;
    g_pinchAy = 0.5;
    if (g_pinchAnchored) {
        POINT pt{};
        if (GetCursorPos(&pt)) {
            const double bw = (double)(r.right - r.left);
            const double bh = (double)(r.bottom - r.top);
            if (bw > 1.0 && bh > 1.0) {
                g_pinchAx = std::clamp(((double)pt.x - (double)r.left) / bw, 0.0, 1.0);
                g_pinchAy = std::clamp(((double)pt.y - (double)r.top) / bh, 0.0, 1.0);
            }
        }
    }

    if (message) {
        char buf[512];
        snprintf(buf, sizeof(buf), "resizing \"%s\"%s",
                 titleOf(target).c_str(),
                 wasMax ? " (restored from maximized)" : "");
        *message = buf;
    }
    return true;
}

void pinchMove(float ratioX, float ratioY) {
    if (!g_pinchHwnd) return;
    if (!IsWindow(g_pinchHwnd)) {
        g_pinchHwnd = nullptr;
        return;
    }
    // NaN / zero guards. A frame can never legitimately double a gap; clamping
    // keeps a decode glitch or a slipping finger from slamming the window.
    if (!(ratioX > 0.f)) ratioX = 1.f;
    if (!(ratioY > 0.f)) ratioY = 1.f;
    if (ratioX < 0.5f) ratioX = 0.5f;
    if (ratioX > 2.0f) ratioX = 2.0f;
    if (ratioY < 0.5f) ratioY = 0.5f;
    if (ratioY > 2.0f) ratioY = 2.0f;

    g_pinchScaleX *= (double)ratioX;
    g_pinchScaleY *= (double)ratioY;

    double loX = 1.0, hiX = 1.0, loY = 1.0, hiY = 1.0;
    pinchLimits(&loX, &hiX, &loY, &hiY);

    // The raw factors keep accumulating past the limits; only the drawn size is
    // compressed. That way reversing the pinch moves the window again on the
    // very next frame instead of having to unwind the overshoot first.
    const double effX = g_pinchRubber ? pinchSoftScale(g_pinchScaleX, loX, hiX)
                                      : std::min(hiX, std::max(loX, g_pinchScaleX));
    const double effY = g_pinchRubber ? pinchSoftScale(g_pinchScaleY, loY, hiY)
                                      : std::min(hiY, std::max(loY, g_pinchScaleY));

    const RECT want = pinchRectFor(effX, effY);

    // PTP reports a frame per contact change, and a slow pinch moves the gap by
    // well under a pixel per frame. Re-sending an identical rect 60 times a
    // second makes some apps flicker, so it is skipped.
    if (g_pinchSentValid && EqualRect(&want, &g_pinchSent)) return;
    g_pinchSent      = want;
    g_pinchSentValid = true;

    SetWindowPos(g_pinchHwnd, nullptr, want.left, want.top,
                 want.right - want.left, want.bottom - want.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void pinchEnd() {
    // Overshoot has to spring back, otherwise a window that was pulled past a
    // limit stays slightly out of place until the next gesture.
    if (g_pinchHwnd && IsWindow(g_pinchHwnd)) {
        double loX = 1.0, hiX = 1.0, loY = 1.0, hiY = 1.0;
        pinchLimits(&loX, &hiX, &loY, &hiY);
        const double settledX = std::min(hiX, std::max(loX, g_pinchScaleX));
        const double settledY = std::min(hiY, std::max(loY, g_pinchScaleY));
        if (std::fabs(settledX - g_pinchScaleX) > 0.001 ||
            std::fabs(settledY - g_pinchScaleY) > 0.001) {
            RECT cur{};
            if (GetWindowRect(g_pinchHwnd, &cur)) {
                const RECT back = pinchRectFor(settledX, settledY);
                if (!EqualRect(&cur, &back)) startAnim(g_pinchHwnd, cur, back);
            }
        }
    }

    g_pinchHwnd      = nullptr;
    g_pinchScaleX    = 1.0;
    g_pinchScaleY    = 1.0;
    g_pinchSentValid = false;
}

void setPinchBehaviour(bool anchored, bool rubberBand) {
    g_pinchAnchored = anchored;
    g_pinchRubber   = rubberBand;
}

void setAnimMs(uint32_t ms) { g_animMs = ms > 1200 ? 1200 : ms; }

std::string describeCandidates() {
    std::string out;
    const auto  v = candidates();
    char        line[768];
    snprintf(line, sizeof(line), "  %zu candidate window(s):\n",
             v.size());
    out += line;

    for (HWND h : v) {
        RECT r{};
        GetWindowRect(h, &r);
        wchar_t cls[128] = {};
        GetClassNameW(h, cls, 128);
        char clsu[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, cls, -1, clsu, sizeof(clsu) - 1, nullptr,
                            nullptr);
        snprintf(line, sizeof(line), "    %p  %5ld,%-5ld %4ldx%-4ld  %-30s %s\n",
                 (void*)h, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 clsu, titleOf(h).c_str());
        out += line;
    }
    return out;
}

bool swapFocusedWith(SwapDir dir, std::string* message) {
    auto fail = [&](const char* m) {
        if (message) *message = m;
        return false;
    };

    if (!g_anims.empty()) return fail("previous swap still animating");

    // Prefer the remembered window so the console we were launched from never
    // becomes the target just because it has focus.
    HWND from = resolveTarget();
    if (!from)
        return fail("no candidate window - open an app window first");

    const bool maxFrom = isEffectivelyMaximized(from);
    HWND to = findNeighbor(from, dir, maxFrom);
    if (!to)
        return fail(dir == SwapDir::Left ? "no window to the left"
                                         : "no window to the right");
    const bool maxTo = isEffectivelyMaximized(to);

    // A maximized window ignores positional moves, so restore before measuring.
    if (maxFrom) ShowWindow(from, SW_RESTORE);
    if (maxTo) ShowWindow(to, SW_RESTORE);

    RECT a0{}, b0{};
    if (!GetWindowRect(from, &a0) || !GetWindowRect(to, &b0))
        return fail("GetWindowRect failed");

    RECT at{}, bt{};
    if (maxFrom || maxTo) {
        // Both were full screen: a pure swap would be invisible because the
        // rects are identical. Tile instead -- the swiped window takes the side
        // you pushed toward, the other one takes the far side.
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromWindow(from, MONITOR_DEFAULTTONEAREST),
                             &mi))
            return fail("GetMonitorInfo failed");
        const RECT& w    = mi.rcWork;
        const long  midX = w.left + (w.right - w.left) / 2;
        const RECT  left{w.left, w.top, midX, w.bottom};
        const RECT  right{midX, w.top, w.right, w.bottom};
        at = (dir == SwapDir::Left) ? left : right;
        bt = (dir == SwapDir::Left) ? right : left;
    } else {
        at = b0;
        bt = a0;
    }

    startAnim(from, a0, at);
    startAnim(to, b0, bt);

    if (message) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s \"%s\" <-> \"%s\"",
                 (maxFrom || maxTo) ? "tiling" : "swapping",
                 titleOf(from).c_str(), titleOf(to).c_str());
        *message = buf;
    }
    return true;
}

void tick() {
    if (g_anims.empty()) return;
    const uint64_t now = GetTickCount64();

    for (size_t i = 0; i < g_anims.size();) {
        Anim& a = g_anims[i];
        double x = (double)(now - a.start) / (double)a.dur;
        const bool done = x >= 1.0;
        if (x > 1.0) x = 1.0;

        // The main loop runs at ~1ms; resampling a window 250 times per swap
        // wastes GDI work without looking any smoother than 60fps.
        if (!done && now - a.lastApply < 15) {
            ++i;
            continue;
        }
        a.lastApply = now;

        const double e = easeWind(x);
        const int l = lerpI(a.from.left, a.to.left, e);
        const int t = lerpI(a.from.top, a.to.top, e);
        const int w = lerpI(a.from.right - a.from.left, a.to.right - a.to.left, e);
        const int h = lerpI(a.from.bottom - a.from.top, a.to.bottom - a.to.top, e);

        // SWP_ASYNCWINDOWPOS keeps a hung target app from stalling our loop.
        SetWindowPos(a.hwnd, nullptr, l, t,
                     done ? (a.to.right - a.to.left) : w,
                     done ? (a.to.bottom - a.to.top) : h,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);

        if (done) {
            g_anims.erase(g_anims.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
}

}  // namespace wm
