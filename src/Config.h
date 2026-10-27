// Config.h — persisted user settings.
//
// Everything the control panel can change lives here, in one struct, so the
// HTTP layer never has to know about ini syntax and main.cpp never has to
// re-parse strings while handling a gesture.
//
// The file format is still the same flat `key = value` ini the project started
// with, so an existing tp_gestures.ini keeps working unchanged.

#pragma once

#include <cstdint>
#include <string>

// ---------------------------------------------------------------- actions

// What a swipe does. The identifiers are stable strings on purpose: the web UI
// owns the human readable labels (and their translations), the backend only
// deals with keys. Adding an action means adding a case here and a <option> in
// the UI -- no data migration.
enum class ActionId : uint8_t {
    None = 0,
    Drag,          // three-finger window drag (only meaningful for the 3-finger row)
    // ---- window ----
    SwapLeft,
    SwapRight,
    TaskView,      // Win+Tab
    ShowDesktop,   // Win+D
    Maximize,
    Minimize,
    Close,         // Alt+F4
    Fullscreen,    // F11
    // ---- switching ----
    NextWindow,    // Alt+Tab
    PrevWindow,    // Alt+Shift+Tab
    PrevTab,       // Ctrl+Shift+Tab
    NextTab,       // Ctrl+Tab
    // Virtual desktops. These matter because we switch the system's own
    // three/four finger swipes off while running; without an equivalent here,
    // taking over the touchpad would silently remove a feature the user had.
    PrevDesktop,   // Win+Ctrl+Left
    NextDesktop,   // Win+Ctrl+Right
    // ---- system ----
    LockScreen,    // Win+L
    Screenshot,    // Win+Shift+S
    TaskManager,   // Ctrl+Shift+Esc
    // ---- sound ----
    VolumeUp,
    VolumeDown,
    Mute,
    MediaPlay,     // play / pause
    MediaNext,
    MediaPrev,
    // ---- editing ----
    Copy,
    Paste,
    Cut,
    Undo,
    SelectAll,
    // Runs whatever chord is stored in the slot's KeyChord. One action id for
    // every slot, because the chord lives with the binding, not with the action.
    Custom,
    // Same idea for a program: the executable lives with the slot, not here.
    Launch,
};

const char* actionKey(ActionId a);
ActionId    parseAction(const std::string& key);
bool        actionIsValid(ActionId a);

// ------------------------------------------------------------------ config

// A key combination for the "custom" action. Modifiers are a bitmask so the
// whole thing survives a round trip through the ini as one readable string
// ("Ctrl+Shift+K"); vk == 0 means nothing has been recorded yet.
struct KeyChord {
    static constexpr uint16_t kCtrl  = 1;
    static constexpr uint16_t kShift = 2;
    static constexpr uint16_t kAlt   = 4;
    static constexpr uint16_t kWin   = 8;

    uint16_t mods = 0;
    uint16_t vk   = 0;
    bool valid() const { return vk != 0; }
};

// ---------------------------------------------------------------- slot table
//
// Every bindable gesture is one cell of a (finger count x kind) matrix, and the
// cell index is the address used by the mapping table, the chord table and the
// program table alike. Keeping the three tables parallel is what lets a new
// gesture kind exist without touching any of the storage code: add the kind
// here, add its name below, and the rest follows.
//
// Fingers 3, 4 and 5. Five only ever fires on hardware that reports five
// contacts; on a four-contact pad the row is drawn dimmed and stays inert,
// which is honest instead of hiding a capability the user might want.
namespace slot {

enum Kind : uint8_t {
    Tap = 0,
    DoubleTap,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    CornerTL,
    CornerTR,
    CornerBL,
    CornerBR,
    EdgeLeft,
    EdgeRight,
    KindCount
};

constexpr int kFirstFingers = 3;
constexpr int kFingerCounts = 3;                  // 3, 4, 5
constexpr int Count        = kFingerCounts * KindCount;

inline int fingerIndex(int fingers) {
    return fingers <= kFirstFingers
               ? 0
               : (fingers >= kFirstFingers + kFingerCounts - 1
                      ? kFingerCounts - 1
                      : fingers - kFirstFingers);
}
inline int index(int fingers, int kind) {
    return fingerIndex(fingers) * KindCount +
           (kind < 0 ? 0 : (kind >= KindCount ? Tap : kind));
}
inline int fingersOf(int s) {
    const int f = s < 0 ? 0 : s / KindCount;
    return kFirstFingers + (f > kFingerCounts - 1 ? kFingerCounts - 1 : f);
}
inline int kindOf(int s) { return s < 0 ? 0 : s % KindCount; }
inline bool valid(int s) { return s >= 0 && s < Count; }

// "3-finger-tap" .. "5-finger-corner-br". Used as the ini key suffix and as the
// identifier the UI talks about a slot with.
const char* kindKey(int kind);
bool        kindFromKey(const std::string& key, int* out);

}  // namespace slot

// "Ctrl+Shift+K", or "" when unbound.
std::string chordToString(const KeyChord& k);
// Accepts the same spelling; anything unrecognised leaves `out` untouched and
// returns false, so a typo cannot silently clear a binding.
bool parseChordString(const std::string& s, KeyChord* out);

enum class Mode : uint8_t {
    Drag = 0,   // three fingers move the window under the cursor (macOS-like)
    Tile = 1,   // three/four finger swipes run the mapping table
};

const char* modeKey(Mode m);          // "drag" / "tile"
Mode        parseMode(const std::string& s);

// Direction order used by every array below and by the API payload.
namespace dir {
constexpr int Left  = 0;
constexpr int Right = 1;
constexpr int Up    = 2;
constexpr int Down  = 3;
constexpr int Count = 4;
}  // namespace dir

// Which edge a swipe started on, used by the edge tables below.
namespace edgeSide {
constexpr int Left  = 0;
constexpr int Right = 1;
constexpr int Count = 2;
}  // namespace edgeSide

struct AppConfig {
    Mode  mode        = Mode::Drag;
    bool  enabled     = true;    // master switch, lets the UI pause gestures
    float sensitivity = 1.0f;    // drag gain, 0.3 .. 2.0

    // Swipe recognition, in touchpad device units.
    float thresholdX = 300.f;
    float thresholdY = 250.f;
    float dominance  = 1.5f;

    // How many fingers start a window drag. Keeping this at 3 is what frees the
    // fourth finger for the mapping table in drag mode.
    uint8_t dragFingers = 3;

    // ---- two-finger pinch resize ----
    // Independent of `mode`: pinching is a two-finger gesture and the mode only
    // decides what three and four fingers do.
    bool  pinchEnabled     = true;
    float pinchSensitivity = 1.0f;   // 0.5 .. 2.0, exponent on the gap ratio
    // Fraction of the finger gap that has to change before the window starts
    // resizing. Kept small on purpose: the engine tells a pinch apart from a
    // two-finger scroll by looking at whether the fingers move apart or
    // together, so the dead band only has to swallow touch jitter. A large
    // value here is what made the gesture feel like it "uses up" the first
    // centimetre of movement.
    float pinchDeadband    = 0.015f;
    // Grow away from the point under the fingers instead of from the window
    // centre. This is what makes the gesture feel anchored to the hand.
    bool  pinchAnchored    = true;
    // Overshoot with resistance instead of stopping dead at the size limits.
    bool  pinchRubberBand  = true;
    // How the gap change is split across the two axes:
    //   0 = uniform (width and height move together)
    //   1 = auto    (the angle of the two-finger line decides)
    //   2 = width only
    //   3 = height only
    uint8_t pinchAxis = 1;

    // ---- tap ----
    // A touch that lifts quickly and barely moved. Coexists with every other
    // gesture because it fires on release, after the movement channels have
    // already given up.
    bool     tapEnabled = true;
    uint32_t tapMaxMs   = 220;
    float    tapSlop    = 10.f;      // device units of travel still counted as a tap
    // Second tap has to land within this to count as a double tap. The window
    // is only honoured when the finger count actually has a double-tap binding,
    // so a user who does not use them never waits for it.
    uint32_t doubleTapMs = 320;

    // ---- edge swipes ----
    // A swipe that starts inside a narrow band along the left or right edge of
    // the touchpad fires its own action, so edge gestures stop colliding with
    // the same swipe made in the middle.
    bool       edgeEnabled = false;
    float      edgeZone    = 0.12f;  // fraction of the pad width

    // Window animation length in milliseconds (swap / tile / rubber-band
    // settle). 0 disables the animation and moves instantly.
    uint16_t animMs = 260;

    // 0 = follow Windows, 1 = light, 2 = dark. The window reads this instead of
    // only listening to the system setting, so the look can be overridden.
    uint8_t theme = 0;

    // The whole mapping table, indexed by slot::index(fingers, kind).
    ActionId bind[slot::Count];

    // Payloads that belong to a slot rather than to an action: the chord for
    // ActionId::Custom and the executable for ActionId::Launch.
    KeyChord    custom[slot::Count];
    std::string launch[slot::Count];

    AppConfig() {
        // Four-finger defaults mirror what Windows does out of the box, so
        // taking the system swipes over changes nothing the user had before.
        bind[slot::index(4, slot::SwipeLeft)]  = ActionId::PrevDesktop;
        bind[slot::index(4, slot::SwipeRight)] = ActionId::NextDesktop;
        bind[slot::index(4, slot::SwipeUp)]    = ActionId::TaskView;
        bind[slot::index(4, slot::SwipeDown)]  = ActionId::ShowDesktop;
        // Three-finger left/right swaps windows side by side, which is the
        // macOS "move this window over there" pair.
        bind[slot::index(3, slot::SwipeLeft)]  = ActionId::SwapLeft;
        bind[slot::index(3, slot::SwipeRight)] = ActionId::SwapRight;
        // The one gesture people ask for first. Bound by default so the feature
        // is visible without having to read the manual.
        bind[slot::index(4, slot::Tap)]        = ActionId::TaskView;
    }

    // True when the three-finger row actually does something in this mode.
    // Dragging four fingers hands the three-finger row back to the mapping
    // table, which is the whole reason that option exists.
    bool finger3Active() const { return mode == Mode::Tile || dragFingers == 4; }
};

// Loads `path`, leaving defaults in place for anything missing or malformed.
// Returns false only when the file could not be opened, which is not an error.
bool loadConfig(AppConfig& cfg, const char* path);

// Writes the whole struct, preserving nothing but our own keys. Written via a
// temporary file + rename so a crash mid-write cannot truncate the settings.
bool saveConfig(const AppConfig& cfg, const char* path);

// Clamps every field into its documented range. Called after load and after an
// API update so a hostile or buggy client cannot push NaN thresholds in.
void sanitize(AppConfig& cfg);
