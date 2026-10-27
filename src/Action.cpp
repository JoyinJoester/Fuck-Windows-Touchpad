// Action.cpp
#include "Action.h"

#include "WindowManager.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <vector>

namespace {

// One key press or release, expressed as a virtual key. Modifiers are ordinary
// entries so a chord is just a list.
struct KeyStroke {
    WORD vk;
    bool up;
};

void sendStrokes(std::vector<KeyStroke> strokes) {
    std::vector<INPUT> in(strokes.size());
    for (size_t i = 0; i < strokes.size(); ++i) {
        INPUT& x = in[i];
        x.type       = INPUT_KEYBOARD;
        x.ki.wVk     = strokes[i].vk;
        x.ki.wScan   = static_cast<WORD>(MapVirtualKeyW(strokes[i].vk, MAPVK_VK_TO_VSC));
        x.ki.dwFlags = strokes[i].up ? KEYEVENTF_KEYUP : 0;
        // Extended keys (arrows, Win, the media cluster, ...) need the flag or
        // the target receives the numpad variant instead.
        switch (strokes[i].vk) {
        case VK_LWIN: case VK_RWIN:
        case VK_TAB:  case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END:  case VK_INSERT: case VK_DELETE:
        case VK_PRIOR: case VK_NEXT:
        case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
        case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK:
        case VK_MEDIA_PLAY_PAUSE: case VK_MEDIA_STOP:
            x.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            break;
        default:
            break;
        }
    }
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

// A gesture fired while the settings window happens to have focus would send
// the chord into the settings window itself -- copy nothing, switch no tab,
// adjust no volume on the app the user is actually looking at. The engine
// tracks the last real window the user was in; pull it forward first.
void ensureRealForeground() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return;
    wchar_t cls[128] = {};
    if (!GetClassNameW(fg, cls, 128)) return;
    // Must match kUiWindowClass in UiWindow.h (kept literal to avoid dragging
    // the GDI+ UI headers into the action layer).
    if (wcscmp(cls, L"TpGesturesMainWindow") != 0) return;
    const HWND t = wm::lastActive();
    if (t && IsWindow(t)) SetForegroundWindow(t);
}

// Chord helper: mods down, key down, key up, mods up in reverse order.
void chord(std::vector<WORD> mods, WORD key) {
    ensureRealForeground();
    std::vector<KeyStroke> s;
    for (WORD m : mods) s.push_back({m, false});
    s.push_back({key, false});
    s.push_back({key, true});
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) s.push_back({*it, true});
    sendStrokes(std::move(s));
}

// The window an action should act on: the tracked foreground window, falling
// back to whatever is foreground right now. Deliberately tolerates a null
// result -- the caller reports it instead of crashing.
HWND targetWindow() {
    HWND h = wm::lastActive();
    if (!h || !IsWindow(h)) h = GetForegroundWindow();
    if (h && !IsWindow(h)) h = nullptr;
    return h;
}

bool isOurConsole(HWND h) {
    wchar_t cls[128] = {};
    if (!GetClassNameW(h, cls, 128)) return false;
    return wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0 ||
           wcscmp(cls, L"ConsoleWindowClass") == 0;
}

}  // namespace

bool runChord(const KeyChord& k, std::string* detail) {
    if (!k.valid()) {
        if (detail) *detail = "no key recorded";
        return false;
    }

    // A bare key with no modifier is still a chord: pressing F5 is the whole
    // binding. Modifiers are added in the same order the recorder displays.
    std::vector<WORD> mods;
    if (k.mods & KeyChord::kCtrl) mods.push_back(VK_CONTROL);
    if (k.mods & KeyChord::kAlt) mods.push_back(VK_MENU);
    if (k.mods & KeyChord::kShift) mods.push_back(VK_SHIFT);
    if (k.mods & KeyChord::kWin) mods.push_back(VK_LWIN);

    chord(mods, static_cast<WORD>(k.vk));

    if (detail) *detail = chordToString(k);
    return true;
}

bool actionAllowedInMode(ActionId a, Mode m) {
    if (a == ActionId::None) return true;
    // Three-finger drag is the whole point of drag mode, and it cannot coexist
    // with a three-finger swipe binding -- the same movement would do both.
    if (a == ActionId::Drag) return m == Mode::Tile;
    return true;
}

bool runLaunch(const std::string& path, std::string* detail) {
    auto skip = [&](const char* why) {
        if (detail) *detail = why;
        return false;
    };
    if (path.empty()) return skip("没有选择程序");

    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 1) return skip("程序路径无法识别");
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);

    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES)
        return skip("找不到该程序，可能已移动或删除");

    const HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr,
                                     nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) return skip("系统拒绝启动该程序");

    // Only the file name is interesting in a log line or the status bar.
    const size_t slash = path.find_last_of("\\/");
    if (detail)
        *detail = "已启动 " + (slash == std::string::npos ? path
                                                        : path.substr(slash + 1));
    return true;
}

bool runAction(ActionId a, std::string* detail) {
    auto done = [&](const char* what) {
        if (detail) *detail = what;
        return true;
    };
    auto skip = [&](const char* why) {
        if (detail) *detail = why;
        return false;
    };

    switch (a) {
    case ActionId::None:
        return skip("no action bound");

    case ActionId::Drag:
        // Not reachable through the swipe path: dragging is a continuous
        // channel owned by the gesture engine, not a one-shot action.
        return skip("drag is started by holding three fingers, not by a swipe");

    case ActionId::Custom:
        // The chord lives with the binding, not with the action, so the caller
        // has to look it up before getting here.
        return skip("custom action needs its slot");

    case ActionId::Launch:
        // Same idea: the program lives with the slot.
        return skip("launch action needs its slot");

    case ActionId::SwapLeft:
    case ActionId::SwapRight: {
        std::string msg;
        const bool ok = wm::swapFocusedWith(
            a == ActionId::SwapLeft ? SwapDir::Left : SwapDir::Right, &msg);
        if (detail) *detail = msg;
        return ok;
    }

    case ActionId::TaskView:
        chord({VK_LWIN}, VK_TAB);
        return done("task view");

    case ActionId::ShowDesktop:
        chord({VK_LWIN}, 'D');
        return done("show desktop");

    case ActionId::NextWindow:
        chord({VK_MENU}, VK_TAB);
        return done("next window");

    case ActionId::PrevWindow:
        chord({VK_MENU, VK_SHIFT}, VK_TAB);
        return done("previous window");

    case ActionId::PrevDesktop:
        chord({VK_LWIN, VK_CONTROL}, VK_LEFT);
        return done("previous desktop");

    case ActionId::NextDesktop:
        chord({VK_LWIN, VK_CONTROL}, VK_RIGHT);
        return done("next desktop");

    case ActionId::VolumeUp:
        chord({}, VK_VOLUME_UP);
        return done("volume up");

    case ActionId::VolumeDown:
        chord({}, VK_VOLUME_DOWN);
        return done("volume down");

    case ActionId::Mute:
        chord({}, VK_VOLUME_MUTE);
        return done("mute");

    case ActionId::MediaPlay:
        chord({}, VK_MEDIA_PLAY_PAUSE);
        return done("play/pause");

    case ActionId::MediaNext:
        chord({}, VK_MEDIA_NEXT_TRACK);
        return done("next track");

    case ActionId::MediaPrev:
        chord({}, VK_MEDIA_PREV_TRACK);
        return done("previous track");

    case ActionId::Screenshot:
        chord({VK_LWIN, VK_SHIFT}, 'S');
        return done("screenshot");

    case ActionId::TaskManager:
        chord({VK_CONTROL, VK_SHIFT}, VK_ESCAPE);
        return done("task manager");

    case ActionId::Fullscreen:
        chord({}, VK_F11);
        return done("fullscreen");

    case ActionId::Copy:
        chord({VK_CONTROL}, 'C');
        return done("copy");

    case ActionId::Paste:
        chord({VK_CONTROL}, 'V');
        return done("paste");

    case ActionId::Cut:
        chord({VK_CONTROL}, 'X');
        return done("cut");

    case ActionId::Undo:
        chord({VK_CONTROL}, 'Z');
        return done("undo");

    case ActionId::SelectAll:
        chord({VK_CONTROL}, 'A');
        return done("select all");

    case ActionId::LockScreen:
        chord({VK_LWIN}, 'L');
        return done("lock screen");

    case ActionId::NextTab:
        chord({VK_CONTROL}, VK_TAB);
        return done("next tab");

    case ActionId::PrevTab:
        chord({VK_CONTROL, VK_SHIFT}, VK_TAB);
        return done("previous tab");

    case ActionId::Maximize:
    case ActionId::Minimize:
    case ActionId::Close: {
        HWND h = targetWindow();
        if (!h) return skip("no target window");
        // Never let a gesture close the terminal the user launched us from --
        // that reads as a crash, not as a feature.
        if (a == ActionId::Close && isOurConsole(h))
            return skip("refusing to close the host terminal");

        if (a == ActionId::Maximize) {
            const bool zoomed = IsZoomed(h) != 0;
            ShowWindow(h, zoomed ? SW_RESTORE : SW_MAXIMIZE);
            return done(zoomed ? "restored" : "maximized");
        }
        if (a == ActionId::Minimize) {
            ShowWindow(h, SW_MINIMIZE);
            return done("minimized");
        }
        PostMessageW(h, WM_CLOSE, 0, 0);
        return done("close requested");
    }
    }

    return skip("unknown action");
}
