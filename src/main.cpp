// main.cpp — application entry point and wiring.
//
// This is a GUI-subsystem binary: launching it shows the native window, with no
// console in the way. The diagnostic flags that used to be the only interface
// still work, but they attach to the caller's console first, so `tp_gestures
// --windows` from cmd prints where you expect it to.
//
// Order of operations is deliberate:
//
//   preflight  -> the touchpad must be powered on before anything can work
//   detect     -> enumerate HID collections, pick one with 3+ contacts
//   decode     -> derive the report layout, self-checked every frame
//   capture    -> Raw Input on that collection
//   window     -> native UI, tray icon, event log
//   loop       -> pump messages, tick animations, refresh the UI
//
// The gesture engine only emits intents. Everything that changes the system
// (windows, keystrokes) lives behind Action.h, so a broken action can never
// take the recogniser down with it.

#include "Action.h"
#include "Config.h"
#include "EventLog.h"
#include "GestureEngine.h"
#include "Preflight.h"
#include "RawInputSource.h"
#include "Touchpad.h"
#include "UiWindow.h"
#include "WindowManager.h"

#include <windows.h>
#include <shellapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------- globals

AppConfig      g_cfg;
TouchpadCaps   g_caps;          // filled once detection succeeds, read by the engine config
Touchpad*      g_tp     = nullptr;
GestureEngine* g_engine = nullptr;
RawInputSource g_raw;
EventLog       g_log;
uint64_t       g_startedAtMs = 0;
UiWindow*      g_ui     = nullptr;

GestureSuppression g_sup;
bool               g_supActive = false;

bool g_verbose      = false;
bool g_dumpReg      = false;
bool g_findGestures = false;
bool g_testSuppress = false;
bool g_crashTest    = false;
bool g_listWindows  = false;
int  g_swapTest     = 0;      // -1 left, +1 right, 0 off
bool g_testDrag     = false;
bool g_testPinch    = false;
bool g_startHidden  = false;
bool g_logToFile    = false;
const char* g_modeArg = nullptr;

uint64_t g_frames     = 0;
uint64_t g_decodeFail = 0;
uint64_t g_gestures   = 0;

// fps is a 1-second sliding window rather than an instantaneous value: raw
// input arrives in bursts, and a per-frame delta makes the number flicker
// between 0 and 400 in the UI.
uint64_t g_fpsWindowStart  = 0;
uint64_t g_fpsWindowFrames = 0;
float    g_fps             = 0.f;

// Contacts of the most recent frame, copied out for the overview diagram.
std::vector<std::pair<float, float>> g_pointers;

constexpr const char* kConfigPath = "tp_gestures.ini";

// ---------------------------------------------------------------- helpers

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

std::vector<std::string> commandLineArgs() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return out;
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0,
                                          nullptr, nullptr);
        if (n <= 1) {
            out.emplace_back();
            continue;
        }
        std::vector<char> buf(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf.data(), n, nullptr,
                            nullptr);
        out.emplace_back(buf.data());
    }
    LocalFree(wargv);
    return out;
}

// Only the command line switches that print something need a console. When one
// is present we borrow the caller's; otherwise stdout goes to NUL so a stray
// printf in library code cannot pop up a window.
void setupStdio(bool wantsConsole, bool toFile) {
    if (wantsConsole) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
    } else if (toFile) {
        // Support aid: a GUI binary normally has nowhere to print, which makes
        // a startup failure invisible. --log leaves a trace next to the exe.
        freopen("tp_gestures.log", "w", stdout);
    } else {
        freopen("NUL", "w", stdout);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
}

void fatal(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"Fuck-Windows-Touchpad", MB_ICONWARNING | MB_OK);
}

bool hasConsoleFlag(const std::vector<std::string>& args) {
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--console" || a == "-v" || a == "--dump-reg" ||
            a == "--find-gestures" || a == "--test-suppress" ||
            a == "--windows" || a == "--swap-left" || a == "--swap-right" ||
            a == "--test-drag" || a == "--test-pinch" || a == "--help" ||
            a == "-h")
            return true;
    }
    return false;
}

// ------------------------------------------------------- gesture routing

const char* fingerLabel(int n) {
    return n <= 3 ? "三指" : (n == 4 ? "四指" : "五指");
}

// Human readable name of a slot, used for the event log and the status line.
std::string slotLabel(int s) {
    if (!slot::valid(s)) return "?";
    std::string out = fingerLabel(slot::fingersOf(s));
    switch (slot::kindOf(s)) {
    case slot::Tap:        out += "轻点"; break;
    case slot::DoubleTap:  out += "双击"; break;
    case slot::SwipeLeft:  out += "左滑"; break;
    case slot::SwipeRight: out += "右滑"; break;
    case slot::SwipeUp:    out += "上滑"; break;
    case slot::SwipeDown:  out += "下滑"; break;
    case slot::CornerTL:   out += "左上角轻点"; break;
    case slot::CornerTR:   out += "右上角轻点"; break;
    case slot::CornerBL:   out += "左下角轻点"; break;
    case slot::CornerBR:   out += "右下角轻点"; break;
    case slot::EdgeLeft:   out += "左边缘滑动"; break;
    case slot::EdgeRight:  out += "右边缘滑动"; break;
    default:               out += "?"; break;
    }
    return out;
}

int kindOfDir(int direction) {
    switch (direction) {
    case dir::Left:  return slot::SwipeLeft;
    case dir::Right: return slot::SwipeRight;
    case dir::Up:    return slot::SwipeUp;
    default:         return slot::SwipeDown;
    }
}

// Runs whatever a slot is bound to. Two actions carry their payload with the
// slot rather than with the action -- a recorded chord and a program path --
// which is what makes one action id enough for every binding.
bool runSlot(int s, std::string* detail) {
    if (!slot::valid(s)) return false;
    switch (g_cfg.bind[s]) {
    case ActionId::Custom: return runChord(g_cfg.custom[s], detail);
    case ActionId::Launch: return runLaunch(g_cfg.launch[s], detail);
    case ActionId::None:   return false;
    default:               return runAction(g_cfg.bind[s], detail);
    }
}

void fireSlot(int s) {
    const ActionId a = g_cfg.bind[s];
    if (a == ActionId::None) return;

    std::string detail;
    const bool ok = runSlot(s, &detail);

    ++g_gestures;
    std::string line = slotLabel(s) + " → " + actionKey(a);
    if (!detail.empty()) line += "：" + detail;
    g_log.log(ok ? "gesture" : "warn", line);
    if (g_ui) g_ui->notifyNewLog();
}

// A tap that has been recognised but deliberately held back: if that finger
// count has a double-tap binding, the second tap may still be on its way and
// firing the single-tap action now would do both.
struct PendingTap {
    bool         active = false;
    GestureEvent ev;
    uint64_t     dueMs  = 0;
};
PendingTap g_pendingTap;

// Corner taps take priority over the plain tap of the same finger count, but
// only when that corner actually has something bound -- the same fall-through
// rule the edge swipes use, so turning corners on never kills a binding.
int cornerSlotOf(const GestureEvent& ev) {
    if (ev.corner < 0 || ev.corner > 3) return -1;
    const int s = slot::index(ev.fingers, slot::CornerTL + ev.corner);
    return g_cfg.bind[s] == ActionId::None ? -1 : s;
}

void dispatchTap(int s) { fireSlot(s); }

void dispatchSwipe(const GestureEvent& g) {
    const int n = g.fingers;

    // In drag mode the configured finger count drives the window drag, so its
    // swipes must stay inert -- otherwise one movement would both move and
    // reshuffle windows.
    if (g_cfg.mode == Mode::Drag && g_cfg.dragFingers == n) return;

    int s = -1;
    if (g_cfg.edgeEnabled && g.edge != 0 &&
        (g.direction == dir::Left || g.direction == dir::Right)) {
        const int es = slot::index(
            n, g.edge == 1 ? slot::EdgeLeft : slot::EdgeRight);
        if (g_cfg.bind[es] != ActionId::None) s = es;
    }
    if (s < 0) {
        const int plain = slot::index(n, kindOfDir(g.direction));
        if (g_cfg.bind[plain] == ActionId::None) return;
        s = plain;
    }
    fireSlot(s);
}

void onGesture(const GestureEvent& g) {
    if (g.kind != GestureKind::Tap) {
        dispatchSwipe(g);
        return;
    }

    // A bound corner fires straight away: it is a different physical target, so
    // nobody expects to double-tap it.
    const int cornerSlot = cornerSlotOf(g);
    if (cornerSlot >= 0) {
        dispatchTap(cornerSlot);
        return;
    }

    const int tapSlot    = slot::index(g.fingers, slot::Tap);
    const int doubleSlot = slot::index(g.fingers, slot::DoubleTap);
    const bool wantDouble = g_cfg.bind[doubleSlot] != ActionId::None;

    if (g_pendingTap.active && g_pendingTap.ev.fingers == g.fingers &&
        GetTickCount64() < g_pendingTap.dueMs) {
        // Within the double-tap window. Past it, tickPendingTap() has already
        // (or is about to) fire the single tap; a late second tap starts a
        // fresh cycle instead of doubling.
        g_pendingTap.active = false;
        if (wantDouble) {
            dispatchTap(doubleSlot);
            return;
        }
        dispatchTap(tapSlot);
        return;
    }

    // A tap waiting for its partner from a different finger count: flush it,
    // since the two cannot form a double tap anyway.
    if (g_pendingTap.active) {
        g_pendingTap.active = false;
        dispatchTap(slot::index(g_pendingTap.ev.fingers, slot::Tap));
    }

    if (wantDouble && g_cfg.bind[tapSlot] != ActionId::None) {
        // Hold it back only when both halves are bound: if the single tap does
        // nothing, delaying it would just make the double tap feel slow.
        g_pendingTap.active = true;
        g_pendingTap.ev     = g;
        g_pendingTap.dueMs  = GetTickCount64() + g_cfg.doubleTapMs;
        return;
    }

    dispatchTap(tapSlot);
}

// Called from the main loop: a held-back tap whose window has expired.
void tickPendingTap() {
    if (!g_pendingTap.active) return;
    if (GetTickCount64() < g_pendingTap.dueMs) return;
    g_pendingTap.active = false;
    if (g_cfg.enabled) dispatchTap(slot::index(g_pendingTap.ev.fingers, slot::Tap));
}


// ------------------------------------------------------------ raw input

void onReport(const uint8_t* report, uint32_t len) {
    ++g_frames;

    ContactFrame frame;
    if (!g_tp->decode(report, len, frame)) {
        ++g_decodeFail;
        return;
    }

    // Keep the newest contacts for the live diagram. Frames arrive at ~100 Hz,
    // so copying a handful of 8-byte entries does not show up next to the HID
    // decode itself.
    g_pointers.clear();
    for (const auto& c : frame.contacts)
        if (c.tip) g_pointers.emplace_back(static_cast<float>(c.x),
                                           static_cast<float>(c.y));

    if (g_verbose && !frame.contacts.empty()) {
        printf("  t=%llu  n=%zu  c=(%.0f,%.0f)",
               (unsigned long long)frame.timestampUs, frame.contacts.size(),
               frame.centroidX(), frame.centroidY());
        for (const auto& c : frame.contacts)
            printf("  [%u](%u,%u)", c.id, c.x, c.y);
        printf("\n");
    }

    g_engine->feed(frame);
    const DragSample  drag  = g_engine->takeDrag();
    const PinchSample pinch = g_engine->takePinch();

    const bool dragMode = g_cfg.mode == Mode::Drag;

    if (dragMode && g_cfg.enabled) {
        switch (drag.ev) {
        case DragEvent::Down: {
            std::string msg;
            if (wm::dragBegin(&msg)) {
                ++g_gestures;
                g_log.log("drag", "开始拖动：" + msg);
                if (g_ui) g_ui->notifyNewLog();
            }
            break;
        }
        case DragEvent::Move:
            wm::dragMove(drag.dx * g_cfg.sensitivity,
                         drag.dy * g_cfg.sensitivity);
            break;
        case DragEvent::Up:
            if (wm::dragging()) {
                g_log.log("drag", "结束拖动");
                if (g_ui) g_ui->notifyNewLog();
            }
            wm::dragEnd();
            break;
        default:
            break;
        }
    } else if (wm::dragging()) {
        // Paused, or the mode just changed to tile. A drag that is still
        // holding a window would keep following the fingers forever, so end it
        // here rather than relying on a lift that may never come.
        wm::dragEnd();
    }

    // Pinch is two-finger only, so it runs in both modes: the mode selects what
    // three and four fingers do, never what two fingers do.
    if (g_cfg.enabled && g_cfg.pinchEnabled) {
        switch (pinch.ev) {
        case PinchEvent::Begin: {
            std::string msg;
            if (wm::pinchBegin(&msg)) {
                ++g_gestures;
                g_log.log("drag", "开始缩放：" + msg);
                if (g_ui) g_ui->notifyNewLog();
                // The opening ratio carries whatever the dead band skipped, so
                // the window starts out already matching the fingers instead of
                // trailing them for the rest of the gesture.
                const double k = static_cast<double>(g_cfg.pinchSensitivity);
                wm::pinchMove(static_cast<float>(std::pow(pinch.ratioX, k)),
                              static_cast<float>(std::pow(pinch.ratioY, k)));
            }
            break;
        }
        case PinchEvent::Move: {
            // Sensitivity is an exponent on the gap ratio, so 1.0 tracks the
            // fingers exactly and larger values make short pinches travel
            // further. Applied per axis, so a one-axis pinch keeps its aspect
            // change intact.
            const double k = static_cast<double>(g_cfg.pinchSensitivity);
            wm::pinchMove(static_cast<float>(std::pow(pinch.ratioX, k)),
                          static_cast<float>(std::pow(pinch.ratioY, k)));
            break;
        }
        case PinchEvent::End:
            if (wm::pinching()) {
                g_log.log("drag", "结束缩放");
                if (g_ui) g_ui->notifyNewLog();
            }
            wm::pinchEnd();
            break;
        default:
            break;
        }
    } else if (wm::pinching()) {
        wm::pinchEnd();
    }

    // Swipes and taps are always drained, even when disabled, so a queued
    // gesture can never fire minutes later when the user re-enables the service.
    const GestureEvent g = g_engine->poll();
    if (g && g_cfg.enabled) onGesture(g);
}

// ---------------------------------------------------------------- settings

void applyEngineConfig() {
    GestureConfig gc;
    gc.thresholdX  = g_cfg.thresholdX;
    gc.thresholdY  = g_cfg.thresholdY;
    gc.dominance   = g_cfg.dominance;
    gc.dragFingers = g_cfg.dragFingers;
    gc.pinchDeadband = g_cfg.pinchDeadband;
    gc.pinchAxis     = static_cast<PinchAxis>(g_cfg.pinchAxis);
    gc.tapEnabled  = g_cfg.tapEnabled;
    gc.tapMaxMs    = g_cfg.tapMaxMs;
    gc.tapSlop     = g_cfg.tapSlop;
    gc.edgeEnabled = g_cfg.edgeEnabled;
    gc.edgeZone    = g_cfg.edgeZone;
    gc.rangeX      = g_caps.logicalMaxX;
    gc.rangeY      = g_caps.logicalMaxY;
    if (g_engine) g_engine->setConfig(gc);

    wm::setPinchBehaviour(g_cfg.pinchAnchored, g_cfg.pinchRubberBand);
    wm::setAnimMs(g_cfg.animMs);
}

// Turning the service off should also hand the system's own gestures back --
// otherwise "paused" would mean "no gestures at all", which is not what the
// label promises.
void applyEnabledState(bool enable) {
    if (enable && !g_supActive) {
        g_sup       = suppressSystemGestures();
        g_supActive = true;
    } else if (!enable && g_supActive) {
        restoreSystemGestures(g_sup);
        g_supActive = false;
    }
}

void persistAndApply() {
    applyEnabledState(g_cfg.enabled);
    applyEngineConfig();
    saveConfig(g_cfg, kConfigPath);
}

bool runCommand(const std::string& cmd, std::string* detail) {
    auto set = [&](const std::string& s) {
        if (detail) *detail = s;
        return true;
    };

    if (cmd == "swap-left" || cmd == "swap-right") {
        std::string msg;
        const bool ok = wm::swapFocusedWith(
            cmd == "swap-left" ? SwapDir::Left : SwapDir::Right, &msg);
        if (detail) *detail = msg;
        if (!msg.empty()) {
            g_log.log(ok ? "gesture" : "warn", msg);
            if (g_ui) g_ui->notifyNewLog();
        }
        return ok;
    }

    if (cmd == "test-drag") {
        std::string msg;
        if (!wm::dragBegin(&msg)) {
            if (detail) *detail = msg;
            return false;
        }
        // Out and back: the desktop ends up exactly where it started.
        for (int k = 0; k < 8; ++k) {
            wm::dragMove(25.f, 0.f);
            Sleep(12);
        }
        for (int k = 0; k < 8; ++k) {
            wm::dragMove(-25.f, 0.f);
            Sleep(12);
        }
        wm::dragEnd();
        g_log.log("drag", "拖动往返测试完成");
        if (g_ui) g_ui->notifyNewLog();
        return set("拖动往返测试完成，窗口已归位");
    }

    if (cmd == "test-pinch") {
        std::string msg;
        if (!wm::pinchBegin(&msg)) {
            if (detail) *detail = msg;
            return false;
        }
        // Shrink then grow back by the same factor: the window ends up exactly
        // where it started, which makes the check safe to run on a real desktop.
        // Run three legs so both axes are exercised on their own: uniform, width
        // only, height only.
        auto leg = [](float rx, float ry, int n) {
            const float ix = 1.f / rx, iy = 1.f / ry;
            for (int k = 0; k < n; ++k) {
                wm::pinchMove(rx, ry);
                Sleep(14);
            }
            for (int k = 0; k < n; ++k) {
                wm::pinchMove(ix, iy);
                Sleep(14);
            }
        };
        leg(0.94f, 0.94f, 10);
        leg(0.96f, 1.00f, 8);
        leg(1.00f, 0.96f, 8);
        wm::pinchEnd();
        g_log.log("drag", "缩放往返测试完成");
        if (g_ui) g_ui->notifyNewLog();
        return set("缩放往返测试完成，窗口已归位");
    }

    if (cmd == "reset-thresholds") {
        const AppConfig def;
        g_cfg.thresholdX = def.thresholdX;
        g_cfg.thresholdY = def.thresholdY;
        g_cfg.dominance  = def.dominance;
        persistAndApply();
        return set("已恢复推荐阈值");
    }

    if (detail) *detail = "未知命令：" + cmd;
    return false;
}

// ------------------------------------------------------------- diagnostics

void printCaps(const TouchpadCaps& c) {
    printf("  product      : %ls\n", c.product.c_str());
    printf("  manufacturer : %ls\n", c.manufacturer.c_str());
    printf("  vid:pid      : %04X:%04X\n", c.vid, c.pid);
    printf("  kind         : %s\n", c.capabilityLabel());
    printf("  maxContacts  : %u\n", c.maxContacts);
    printf("  logical range: X 0..%u  Y 0..%u\n", c.logicalMaxX, c.logicalMaxY);
    printf("  report length: %u bytes\n", c.reportLength);
    printf("  multi-touch  : %s\n",
           c.supportsMultiTouch() ? "YES (3+ fingers)" : "NO");
}

void parseArgs(const std::vector<std::string>& args) {
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-v")                   g_verbose = true;
        else if (a == "--dump-reg")      g_dumpReg = true;
        else if (a == "--find-gestures") g_findGestures = true;
        else if (a == "--test-suppress") g_testSuppress = true;
        else if (a == "--test-crash")    g_crashTest = true;
        else if (a == "--windows")       g_listWindows = true;
        else if (a == "--swap-left")     g_swapTest = -1;
        else if (a == "--swap-right")    g_swapTest = 1;
        else if (a == "--test-drag")     g_testDrag = true;
        else if (a == "--test-pinch")    g_testPinch = true;
        else if (a == "--minimized")     g_startHidden = true;
        else if (a == "--log")           g_logToFile = true;
        else if (a == "--mode" && i + 1 < args.size()) {
            static std::string mode = args[++i];
            g_modeArg = mode.c_str();
        }
    }
}

}  // namespace

// ===========================================================================

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // Per-monitor DPI awareness (v2). Without this every window rect is
    // reported in a virtualised coordinate space and moving a window to a
    // monitor with a different scale factor lands it in the wrong place.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetDpiFn = BOOL(WINAPI*)(void*);
        auto fn = reinterpret_cast<SetDpiFn>(reinterpret_cast<void*>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (fn) fn(reinterpret_cast<void*>(-4));  // PER_MONITOR_AWARE_V2
    }

    const std::vector<std::string> args = commandLineArgs();

    // Settings, and the optional --log file, live next to the executable. Doing
    // this once with a relative name keeps every later fopen free of Unicode
    // paths, and makes a shortcut with an arbitrary working directory behave
    // like a normal double-click.
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (wchar_t* slash = wcsrchr(exePath, L'\\')) {
            *slash = 0;
            SetCurrentDirectoryW(exePath);
        }
    }

    setupStdio(hasConsoleFlag(args), false);
    parseArgs(args);
    if (g_logToFile) setupStdio(false, true);

    if (hasConsoleFlag(args) && args.size() > 1 &&
        (args[1] == "--help" || args[1] == "-h")) {
        printf("Fuck-Windows-Touchpad — 使用说明\n");
        printf("  直接双击运行即打开控制台窗口（无参数）。\n");
        printf("  --minimized        启动后只驻留托盘\n");
        printf("  --mode drag|tile   覆盖配置中的操作模式\n");
        printf("  --windows          列出可被手势操作的窗口\n");
        printf("  --swap-left/right  直接触发一次窗口互换\n");
        printf("  --test-drag        拖动往返自检，窗口会回到原位\n");
        printf("  --test-pinch       缩放往返自检，窗口会回到原位\n");
        printf("  --dump-reg         打印 PrecisionTouchPad 注册表内容\n");
        return 0;
    }

    // Settings first: everything below reads from g_cfg.
    const bool haveFile = loadConfig(g_cfg, kConfigPath);
    if (g_modeArg) g_cfg.mode = parseMode(g_modeArg);
    sanitize(g_cfg);

    printf("[config] %s\n", haveFile ? kConfigPath : "using defaults");
    printf("[mode]   %s\n", modeKey(g_cfg.mode));

    if (g_dumpReg) {
        printf("[reg] HKCU PrecisionTouchPad:\n%ls\n",
               dumpPrecisionTouchpadRegistry().c_str());
    }
    if (g_findGestures) {
        printf("[reg] searching for gesture settings...\n%ls\n",
               findGestureSettings().c_str());
    }

    // Round-trip check for the suppression logic: write 0, read back, restore,
    // read back. Useful on an unfamiliar machine without touching the touchpad.
    if (g_testSuppress) {
        printf("[test] before:\n%ls\n", dumpPrecisionTouchpadRegistry().c_str());
        const GestureSuppression s = suppressSystemGestures();
        printf("[test] suppressed (changed=%d):\n%ls\n", s.changed ? 1 : 0,
               dumpPrecisionTouchpadRegistry().c_str());
        restoreSystemGestures(s);
        printf("[test] restored:\n%ls\n",
               dumpPrecisionTouchpadRegistry().c_str());
        return 0;
    }

    // ---- 0. preflight: the touchpad must actually be powered on ----
    // A disabled touchpad produces zero HID reports, which is indistinguishable
    // from a decoder bug unless we check for it explicitly.
    const TouchpadPower pw = queryTouchpadPower();
    if (pw == TouchpadPower::Disabled) {
        printf("[!] touchpad switched off, re-enabling\n");
        setTouchpadPower(true);
    }

    // ---- 0b. take over three/four finger swipes ----
    // The system gesture engine reads the same HID stream, so its swipes have to
    // be off while we run or every gesture fires twice.
    if (g_cfg.enabled) {
        g_sup       = suppressSystemGestures();
        g_supActive = true;
        printf("[preflight] system 3/4-finger swipes: %s\n",
               g_sup.changed ? "disabled (restored on exit)" : "already off");
    }

    // Crash-recovery check: die on purpose right after the takeover and let the
    // unhandled-exception filter put the user's swipes back. Without it the
    // registry keeps the zeroed values until the next launch.
    if (g_crashTest) {
        printf("[test] crashing on purpose to exercise the restore path\n");
        fflush(stdout);
        RaiseException(0xE0000001, 0, 0, nullptr);
    }

    // ---- window layer direct tests (no touchpad, no UI) ----
    if (g_listWindows || g_swapTest != 0 || g_testDrag || g_testPinch) {
        const auto pads = enumerateTouchpads();
        if (!pads.empty())
            wm::setTouchpadRange(pads[0].logicalMaxX, pads[0].logicalMaxY);
        wm::init();
        if (g_listWindows) {
            printf("[wm] z-order, topmost first:\n%s\n",
                   wm::describeCandidates().c_str());
        }
        if (g_testDrag) {
            std::string msg;
            if (runCommand("test-drag", &msg)) printf("[drag] %s\n", msg.c_str());
        }
        if (g_testPinch) {
            std::string msg;
            if (runCommand("test-pinch", &msg))
                printf("[pinch] %s\n", msg.c_str());
        }
        if (g_swapTest != 0) {
            std::string msg;
            const bool  ok = wm::swapFocusedWith(
                g_swapTest > 0 ? SwapDir::Right : SwapDir::Left, &msg);
            printf("[wm] %s: %s\n", ok ? "swap started" : "skipped", msg.c_str());
            if (ok) {
                const uint64_t deadline = GetTickCount64() + 2000;
                while (wm::busy() && GetTickCount64() < deadline) {
                    MSG m;
                    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&m);
                        DispatchMessageW(&m);
                    }
                    wm::tick();
                    Sleep(1);
                }
            }
        }
        wm::shutdown();
        if (g_supActive) restoreSystemGestures(g_sup);
        return 0;
    }

    // ---- single instance ----
    // A second launch would fight the first for the touchpad: one of them would
    // get every frame, the other none. Instead, wake the running window.
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE,
                                        L"Local\\TouchpadGesturesInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND h = FindWindowW(kUiWindowClass, nullptr))
            PostMessageW(h, kUiRestoreMessage, 0, 0);
        return 0;
    }

    // ---- 1. capability detection ----
    printf("[1/3] detecting touchpads\n");
    const auto pads = enumerateTouchpads();
    if (pads.empty()) {
        fatal(L"没有找到触摸板。\n\n"
              L"这个程序需要 Precision Touchpad（精确式触摸板）。\n"
              L"如果你用的是台式机，请连接触摸板后重新打开。");
        return 1;
    }
    for (size_t i = 0; i < pads.size(); ++i) {
        printf("  --- device %zu ---\n", i);
        printCaps(pads[i]);
    }

    size_t chosen = 0;
    for (size_t i = 0; i < pads.size(); ++i) {
        if (pads[i].supportsMultiTouch()) { chosen = i; break; }
    }
    const TouchpadCaps caps = pads[chosen];

    // Needed to map finger travel (device units) onto screen pixels for drag.
    wm::setTouchpadRange(caps.logicalMaxX, caps.logicalMaxY);
    // The engine needs the pad width to know where "the edge" is.
    g_caps = caps;

    if (!caps.supportsMultiTouch()) {
        fatal(L"这块触摸板最多只支持 2 点触控，无法识别三指/四指手势。\n\n"
              L"多指手势需要 Precision Touchpad 硬件支持。");
        if (g_supActive) restoreSystemGestures(g_sup);
        return 1;
    }

    // ---- 2. open + layout ----
    printf("[2/3] preparing decoder\n");
    static Touchpad tp;
    if (!tp.open(caps) || !tp.assumePackedLayout()) {
        fatal(L"无法解析这块触摸板的报文格式。\n\n"
              L"程序已识别到设备，但报文布局与已知模板不匹配。");
        if (g_supActive) restoreSystemGestures(g_sup);
        return 2;
    }
    g_tp = &tp;

    // ---- 3. raw input ----
    printf("[3/3] listening for raw input\n");
    if (!g_raw.initialize()) {
        fatal(L"注册触摸板输入失败。\n\n"
              L"可能是权限不足，或触摸板被其他程序独占。");
        if (g_supActive) restoreSystemGestures(g_sup);
        return 3;
    }

    GestureEngine engine;
    g_engine = &engine;
    applyEngineConfig();
    g_raw.setCallback(onReport);

    wm::init();

    // Uptime is reported by the settings window, so it is stamped here.
    g_startedAtMs = GetTickCount64();

    // Seed the log so the events page is never empty on a fresh launch.
    g_log.log("info", std::string("程序已就绪 · ") +
                            (g_cfg.enabled ? "手势接管中" : "已暂停"));
    g_log.log("info", "设备：" + narrow(caps.product) + " · 最多 " +
                            std::to_string(caps.maxContacts) + " 点触控 · " +
                            (caps.supportsMultiTouch() ? "支持三指手势"
                                                       : "不支持多指"));
    if (g_cfg.mode == Mode::Drag)
        g_log.log("info", "拖动模式：三指按住可移动光标下的窗口");
    else
        g_log.log("info", "滑动模式：三指/四指滑动触发映射动作");
    g_log.log("info", g_cfg.pinchEnabled
                            ? std::string("双指缩放：张开手指放大窗口，捏合缩小")
                            : std::string("双指缩放：已关闭"));

    // ---- 5. the window ----
    UiHooks hooks;
    hooks.cfg      = &g_cfg;
    hooks.caps     = caps;
    hooks.haveCaps = true;
    hooks.onApply  = [] { persistAndApply(); };
    hooks.onCommand = runCommand;
    hooks.onLog = [](const char* kind, const std::string& msg) {
        g_log.log(kind, msg);
    };
    hooks.pullLogs = [] {
        std::vector<UiLogLine> out;
        for (auto& l : g_log.recentLogs(400))
            out.push_back(UiLogLine{l.atMs, l.kind, l.message});
        return out;
    };

    UiWindow ui;
    if (!ui.create(inst, &hooks)) {
        fatal(L"创建窗口失败。");
        if (g_supActive) restoreSystemGestures(g_sup);
        return 4;
    }
    g_ui = &ui;
    if (!g_startHidden) ui.show();

    g_fpsWindowStart = GetTickCount64();


    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        const uint64_t now = GetTickCount64();

        // fps: frames per elapsed second, recomputed once a second.
        if (now - g_fpsWindowStart >= 1000) {
            const uint64_t delta = g_frames - g_fpsWindowFrames;
            g_fps = static_cast<float>(static_cast<double>(delta) * 1000.0 /
                                       static_cast<double>(now - g_fpsWindowStart));
            g_fpsWindowFrames = g_frames;
            g_fpsWindowStart  = now;
        }

        wm::tick();
        tickPendingTap();
        UiLive live;
        live.frames            = g_frames;
        live.decodeFail        = g_decodeFail;
        live.gestures          = g_gestures;
        live.fps               = g_fps;
        live.dragging          = wm::dragging();
        live.dragTarget        = wm::dragTargetTitle();
        live.pinching          = wm::pinching();
        live.suppressionActive = g_supActive;
        live.startedAtMs       = g_startedAtMs;
        live.rangeX            = caps.logicalMaxX;
        live.rangeY            = caps.logicalMaxY;
        live.pointers          = g_pointers;
        ui.updateLive(live);

        if (ui.exitRequested()) break;
        Sleep(1);
    }

    ui.destroy();
    g_ui = nullptr;
    wm::shutdown();
    if (g_supActive) restoreSystemGestures(g_sup);
    g_raw.shutdown();
    tp.close();

    printf("frames=%llu decodeFail=%llu gestures=%llu\n",
           (unsigned long long)g_frames, (unsigned long long)g_decodeFail,
           (unsigned long long)g_gestures);
    return 0;
}
