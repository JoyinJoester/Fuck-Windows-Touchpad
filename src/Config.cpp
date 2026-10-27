// Config.cpp
#include "Config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Every action needs a stable wire key and a way back. Keeping both directions
// in one table is what stops the two from drifting apart.
struct ActionRow {
    ActionId    id;
    const char* key;
};

const ActionRow kActions[] = {
    {ActionId::None,       "none"},
    {ActionId::Drag,       "drag"},
    {ActionId::SwapLeft,   "swap-left"},
    {ActionId::SwapRight,  "swap-right"},
    {ActionId::TaskView,   "task-view"},
    {ActionId::ShowDesktop,"show-desktop"},
    {ActionId::Maximize,   "maximize"},
    {ActionId::Minimize,   "minimize"},
    {ActionId::Close,      "close"},
    {ActionId::Fullscreen, "fullscreen"},
    {ActionId::NextWindow, "next-window"},
    {ActionId::PrevWindow, "prev-window"},
    {ActionId::PrevTab,    "prev-tab"},
    {ActionId::NextTab,    "next-tab"},
    {ActionId::PrevDesktop,"prev-desktop"},
    {ActionId::NextDesktop,"next-desktop"},
    {ActionId::LockScreen, "lock-screen"},
    {ActionId::Screenshot, "screenshot"},
    {ActionId::TaskManager,"task-manager"},
    {ActionId::VolumeUp,   "volume-up"},
    {ActionId::VolumeDown, "volume-down"},
    {ActionId::Mute,       "mute"},
    {ActionId::MediaPlay,  "media-play"},
    {ActionId::MediaNext,  "media-next"},
    {ActionId::MediaPrev,  "media-prev"},
    {ActionId::Copy,       "copy"},
    {ActionId::Paste,      "paste"},
    {ActionId::Cut,        "cut"},
    {ActionId::Undo,       "undo"},
    {ActionId::SelectAll,  "select-all"},
    {ActionId::Custom,     "custom"},
    {ActionId::Launch,     "launch"},
};

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

float clampF(float v, float lo, float hi) {
    if (!(v == v)) return lo;  // NaN
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ---- ini helpers ---------------------------------------------------------

bool readKey(const char* path, const char* key, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char buf[512];
    bool found = false;
    const std::string want = lower(key);
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        const size_t hash = line.find_first_of("#;");
        if (hash != std::string::npos) line = line.substr(0, hash);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(line.substr(0, eq))) == want) {
            out   = trim(line.substr(eq + 1));
            if (out.size() >= 2 && (out.front() == '"' || out.front() == '\'') &&
                out.back() == out.front())
                out = out.substr(1, out.size() - 2);
            found = true;
            break;
        }
    }
    std::fclose(f);
    return found;
}

int readInt(const char* path, const char* key, int fallback) {
    std::string s;
    if (!readKey(path, key, s) || s.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return fallback;
    return static_cast<int>(v);
}

float readFloat(const char* path, const char* key, float fallback) {
    std::string s;
    if (!readKey(path, key, s) || s.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return fallback;
    return static_cast<float>(v);
}

// One name per gesture kind. The ini key for a cell is
// "<fingers>-finger-<kind>", so these are the only spellings the file format
// has to agree on.
const char* kKindKeys[slot::KindCount] = {
    "tap",         "double-tap", "swipe-left", "swipe-right",
    "swipe-up",    "swipe-down", "corner-tl",  "corner-tr",
    "corner-bl",   "corner-br",  "edge-left",  "edge-right",
};

std::string slotKey(int s) {
    return std::to_string(slot::fingersOf(s)) + "-finger-" +
           kKindKeys[slot::kindOf(s)];
}

// Spelling used before the table became a (fingers x kind) matrix: swipes were
// "<n>-finger-left" and edges "<n>-edge-left". Read, never written, so an
// existing tp_gestures.ini keeps working without a rewrite.
std::string legacySlotKey(int s) {
    const std::string f = std::to_string(slot::fingersOf(s));
    switch (slot::kindOf(s)) {
    case slot::SwipeLeft:  return f + "-finger-left";
    case slot::SwipeRight: return f + "-finger-right";
    case slot::SwipeUp:    return f + "-finger-up";
    case slot::SwipeDown:  return f + "-finger-down";
    case slot::EdgeLeft:   return f + "-edge-left";
    case slot::EdgeRight:  return f + "-edge-right";
    default:               return {};
    }
}

std::string customKey(int s)    { return "custom-" + slotKey(s); }
std::string legacyCustomKey(int s) {
    const std::string l = legacySlotKey(s);
    return l.empty() ? std::string() : "custom-" + l;
}
std::string launchKey(int s)    { return "run-" + slotKey(s); }

}  // namespace

// ------------------------------------------------------------------ actions

const char* actionKey(ActionId a) {
    for (const auto& r : kActions)
        if (r.id == a) return r.key;
    return "none";
}

ActionId parseAction(const std::string& key) {
    const std::string k = lower(trim(key));
    if (k.empty()) return ActionId::None;
    for (const auto& r : kActions)
        if (k == r.key) return r.id;
    return ActionId::None;
}

bool actionIsValid(ActionId a) {
    for (const auto& r : kActions)
        if (r.id == a) return true;
    return false;
}

// ------------------------------------------------------------- slot naming

const char* slot::kindKey(int kind) {
    return (kind >= 0 && kind < slot::KindCount) ? kKindKeys[kind] : kKindKeys[0];
}

bool slot::kindFromKey(const std::string& key, int* out) {
    const std::string k = lower(trim(key));
    for (int i = 0; i < slot::KindCount; ++i)
        if (k == kKindKeys[i]) {
            if (out) *out = i;
            return true;
        }
    // Bare direction names, as used by the pre-matrix file format.
    if (k == "left")  { if (out) *out = slot::SwipeLeft;  return true; }
    if (k == "right") { if (out) *out = slot::SwipeRight; return true; }
    if (k == "up")    { if (out) *out = slot::SwipeUp;    return true; }
    if (k == "down")  { if (out) *out = slot::SwipeDown;  return true; }
    return false;
}

// -------------------------------------------------------------- key chords
// Virtual key codes are spelled out as numbers instead of including windows.h:
// this file is the only place that has to know the ini syntax, and keeping it
// free of the Win32 headers makes the parser easy to test on its own.
namespace {

struct KeyName {
    uint16_t    vk;
    const char* name;
};

const KeyName kKeyNames[] = {
    {0x08, "Backspace"}, {0x09, "Tab"},    {0x0D, "Enter"},  {0x1B, "Esc"},
    {0x20, "Space"},     {0x21, "PageUp"}, {0x22, "PageDown"}, {0x23, "End"},
    {0x24, "Home"},      {0x25, "Left"},   {0x26, "Up"},     {0x27, "Right"},
    {0x28, "Down"},      {0x2D, "Insert"}, {0x2E, "Delete"},
};

std::string keyNameOf(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x70 + 1);
    for (const auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

bool keyFromName(const std::string& name, uint16_t* out) {
    if (name.size() == 1) {
        const char c = (char)toupper((unsigned char)name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            *out = (uint16_t)c;
            return true;
        }
    }
    if ((name[0] == 'F' || name[0] == 'f') && name.size() >= 2) {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) {
            *out = (uint16_t)(0x70 + n - 1);
            return true;
        }
    }
    for (const auto& k : kKeyNames)
        if (lower(name) == lower(k.name)) {
            *out = k.vk;
            return true;
        }
    if (name.size() == 4 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        char* end = nullptr;
        const long v = std::strtol(name.c_str() + 2, &end, 16);
        if (end && *end == 0 && v > 0 && v < 0x100) {
            *out = (uint16_t)v;
            return true;
        }
    }
    return false;
}

}  // namespace

std::string chordToString(const KeyChord& k) {
    if (!k.valid()) return {};
    std::string out;
    auto add = [&](const char* part) {
        if (!out.empty()) out += '+';
        out += part;
    };
    if (k.mods & KeyChord::kCtrl) add("Ctrl");
    if (k.mods & KeyChord::kAlt) add("Alt");
    if (k.mods & KeyChord::kShift) add("Shift");
    if (k.mods & KeyChord::kWin) add("Win");
    add(keyNameOf(k.vk).c_str());
    return out;
}

bool parseChordString(const std::string& s, KeyChord* out) {
    if (!out) return false;
    const std::string t = trim(s);
    if (t.empty()) {
        *out = KeyChord{};
        return true;   // an empty value means "clear it", not "typo"
    }

    KeyChord k;
    size_t pos = 0;
    while (pos <= t.size()) {
        const size_t plus = t.find('+', pos);
        const std::string tok =
            trim(t.substr(pos, plus == std::string::npos ? std::string::npos
                                                         : plus - pos));
        const std::string low = lower(tok);
        if (low == "ctrl" || low == "control") k.mods |= KeyChord::kCtrl;
        else if (low == "shift") k.mods |= KeyChord::kShift;
        else if (low == "alt") k.mods |= KeyChord::kAlt;
        else if (low == "win" || low == "cmd" || low == "super")
            k.mods |= KeyChord::kWin;
        else if (!keyFromName(tok, &k.vk)) return false;
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    if (!k.valid()) return false;
    *out = k;
    return true;
}

// --------------------------------------------------------------------- mode

const char* modeKey(Mode m) { return m == Mode::Tile ? "tile" : "drag"; }

Mode parseMode(const std::string& s) {
    return lower(trim(s)) == "tile" ? Mode::Tile : Mode::Drag;
}

// ------------------------------------------------------------------- config

void sanitize(AppConfig& cfg) {
    cfg.sensitivity = clampF(cfg.sensitivity, 0.2f, 3.0f);
    cfg.thresholdX  = clampF(cfg.thresholdX, 50.f, 2000.f);
    cfg.thresholdY  = clampF(cfg.thresholdY, 50.f, 2000.f);
    cfg.dominance   = clampF(cfg.dominance, 1.0f, 5.0f);

    if (cfg.dragFingers < 3) cfg.dragFingers = 3;
    if (cfg.dragFingers > 4) cfg.dragFingers = 4;

    cfg.pinchSensitivity = clampF(cfg.pinchSensitivity, 0.5f, 2.0f);
    cfg.pinchDeadband    = clampF(cfg.pinchDeadband, 0.005f, 0.30f);
    if (cfg.pinchAxis > 3) cfg.pinchAxis = 0;

    cfg.tapMaxMs = cfg.tapMaxMs < 80 ? 80 : (cfg.tapMaxMs > 600 ? 600 : cfg.tapMaxMs);
    cfg.tapSlop  = clampF(cfg.tapSlop, 2.f, 60.f);
    cfg.edgeZone = clampF(cfg.edgeZone, 0.04f, 0.30f);
    if (cfg.doubleTapMs < 120) cfg.doubleTapMs = 120;
    if (cfg.doubleTapMs > 800) cfg.doubleTapMs = 800;

    if (cfg.animMs > 1200) cfg.animMs = 1200;
    if (cfg.theme > 2) cfg.theme = 0;

    for (int s = 0; s < slot::Count; ++s) {
        ActionId& a = cfg.bind[s];
        if (!actionIsValid(a)) a = ActionId::None;

        const int  kind  = slot::kindOf(s);
        const bool swipe = kind >= slot::SwipeLeft && kind <= slot::SwipeDown;
        const bool third = slot::fingersOf(s) == 3;

        // "drag" is a three-finger swipe action and nothing else: the drag
        // channel is owned by the engine, and in drag mode the same movement
        // moves the window instead. Anything else is a UI mistake that would
        // silently do nothing, so normalise it away.
        if (a == ActionId::Drag && !(swipe && third)) a = ActionId::None;

        // An action whose payload is missing would look bound in the window and
        // do nothing when used.
        if (a == ActionId::Custom && !cfg.custom[s].valid()) a = ActionId::None;
        if (a == ActionId::Launch && cfg.launch[s].empty())  a = ActionId::None;
    }
}

bool loadConfig(AppConfig& cfg, const char* path) {
    cfg = AppConfig{};  // start from defaults

    std::string s;
    if (readKey(path, "mode", s)) cfg.mode = parseMode(s);

    const int en = readInt(path, "enabled", cfg.enabled ? 1 : 0);
    cfg.enabled = en != 0;

    cfg.sensitivity = readFloat(path, "sensitivity", cfg.sensitivity);
    cfg.thresholdX  = readFloat(path, "threshold-x", cfg.thresholdX);
    cfg.thresholdY  = readFloat(path, "threshold-y", cfg.thresholdY);
    cfg.dominance   = readFloat(path, "dominance", cfg.dominance);

    cfg.dragFingers = static_cast<uint8_t>(
        readInt(path, "drag-fingers", cfg.dragFingers));

    cfg.pinchEnabled = readInt(path, "pinch-enabled", cfg.pinchEnabled ? 1 : 0) != 0;
    cfg.pinchSensitivity =
        readFloat(path, "pinch-sensitivity", cfg.pinchSensitivity);
    cfg.pinchDeadband = readFloat(path, "pinch-deadband", cfg.pinchDeadband);
    cfg.pinchAnchored =
        readInt(path, "pinch-anchored", cfg.pinchAnchored ? 1 : 0) != 0;
    cfg.pinchRubberBand =
        readInt(path, "pinch-rubber", cfg.pinchRubberBand ? 1 : 0) != 0;
    cfg.pinchAxis = static_cast<uint8_t>(
        readInt(path, "pinch-axis", static_cast<int>(cfg.pinchAxis)));

    cfg.tapEnabled = readInt(path, "tap-enabled", cfg.tapEnabled ? 1 : 0) != 0;
    cfg.tapMaxMs   = static_cast<uint32_t>(
        readInt(path, "tap-ms", static_cast<int>(cfg.tapMaxMs)));
    cfg.tapSlop    = readFloat(path, "tap-slop", cfg.tapSlop);
    cfg.doubleTapMs = static_cast<uint32_t>(
        readInt(path, "double-tap-ms", static_cast<int>(cfg.doubleTapMs)));

    cfg.edgeEnabled =
        readInt(path, "edge-enabled", cfg.edgeEnabled ? 1 : 0) != 0;
    cfg.edgeZone = readFloat(path, "edge-zone", cfg.edgeZone);

    cfg.animMs = static_cast<uint16_t>(
        readInt(path, "anim-ms", static_cast<int>(cfg.animMs)));
    cfg.theme = static_cast<uint8_t>(readInt(path, "theme", cfg.theme));

    // The mapping table. Both spellings of a key are honoured: a file written
    // by an older build says "3-finger-left", this one says "3-finger-swipe-left"
    // and both mean the same cell.
    for (int i = 0; i < slot::Count; ++i) {
        if (readKey(path, slotKey(i).c_str(), s)) {
            cfg.bind[i] = parseAction(s);
            continue;
        }
        const std::string legacy = legacySlotKey(i);
        if (!legacy.empty() && readKey(path, legacy.c_str(), s))
            cfg.bind[i] = parseAction(s);
    }

    for (int i = 0; i < slot::Count; ++i) {
        if (readKey(path, customKey(i).c_str(), s)) {
            parseChordString(s, &cfg.custom[i]);
            continue;
        }
        const std::string legacy = legacyCustomKey(i);
        if (!legacy.empty() && readKey(path, legacy.c_str(), s))
            parseChordString(s, &cfg.custom[i]);
    }

    for (int i = 0; i < slot::Count; ++i)
        if (readKey(path, launchKey(i).c_str(), s)) cfg.launch[i] = s;

    const int version = readInt(path, "config-version", 1);
    // Files written before the desktop-switch actions existed say
    // "4-finger-left = none" only because there was nothing to put there. Fill
    // those two slots once on upgrade: a user who had the system's own desktop
    // swipe should not silently lose it to this program.
    if (version < 2) {
        if (cfg.bind[slot::index(4, slot::SwipeLeft)] == ActionId::None)
            cfg.bind[slot::index(4, slot::SwipeLeft)] = ActionId::PrevDesktop;
        if (cfg.bind[slot::index(4, slot::SwipeRight)] == ActionId::None)
            cfg.bind[slot::index(4, slot::SwipeRight)] = ActionId::NextDesktop;
    }
    // Same reasoning for the four-finger tap: it was bindable before, but the
    // only way to find out was to open a menu. Files from before the gesture
    // matrix get the action the default now carries, so the feature the user
    // never discovered starts working instead of staying silently empty.
    if (version < 3 && cfg.bind[slot::index(4, slot::Tap)] == ActionId::None)
        cfg.bind[slot::index(4, slot::Tap)] = ActionId::TaskView;

    // The tap travel tolerance was raised because 10 units rejected real taps.
    // Every file written before this change has "tap-slop = 10" baked in (the
    // save path always writes it), so lift it exactly once rather than only
    // changing the default nobody's file uses.
    if (version < 4 && cfg.tapSlop <= 10.f) cfg.tapSlop = 25.f;

    sanitize(cfg);

    FILE* probe = std::fopen(path, "rb");
    if (!probe) return false;
    std::fclose(probe);
    return true;
}

bool saveConfig(const AppConfig& cfg, const char* path) {
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;

    std::fprintf(f, "# tp_gestures settings -- written by the settings window\n");
    std::fprintf(f, "# mode: drag = three fingers move the window under the cursor\n");
    std::fprintf(f, "#       tile = swipes run the mapping table below\n");
    std::fprintf(f, "# theme: 0 = follow Windows, 1 = light, 2 = dark\n");
    std::fprintf(f, "# pinch-deadband: share of the finger gap skipped before zooming\n");
    std::fprintf(f, "# pinch-axis: 0 = width and height together, 1 = follow the finger\n");
    std::fprintf(f, "#             angle, 2 = width only, 3 = height only\n");
    std::fprintf(f, "# edge-zone: share of the touchpad width that counts as an edge\n\n");

    std::fprintf(f, "mode = %s\n", modeKey(cfg.mode));
    std::fprintf(f, "config-version = 4\n");
    std::fprintf(f, "enabled = %d\n", cfg.enabled ? 1 : 0);
    std::fprintf(f, "sensitivity = %.2f\n", cfg.sensitivity);
    std::fprintf(f, "threshold-x = %.0f\n", cfg.thresholdX);
    std::fprintf(f, "threshold-y = %.0f\n", cfg.thresholdY);
    std::fprintf(f, "dominance = %.2f\n", cfg.dominance);
    std::fprintf(f, "drag-fingers = %u\n", static_cast<unsigned>(cfg.dragFingers));
    std::fprintf(f, "pinch-enabled = %d\n", cfg.pinchEnabled ? 1 : 0);
    std::fprintf(f, "pinch-sensitivity = %.2f\n", cfg.pinchSensitivity);
    std::fprintf(f, "pinch-deadband = %.3f\n", cfg.pinchDeadband);
    std::fprintf(f, "pinch-anchored = %d\n", cfg.pinchAnchored ? 1 : 0);
    std::fprintf(f, "pinch-rubber = %d\n", cfg.pinchRubberBand ? 1 : 0);
    std::fprintf(f, "pinch-axis = %u\n", static_cast<unsigned>(cfg.pinchAxis));
    std::fprintf(f, "tap-enabled = %d\n", cfg.tapEnabled ? 1 : 0);
    std::fprintf(f, "tap-ms = %u\n", static_cast<unsigned>(cfg.tapMaxMs));
    std::fprintf(f, "tap-slop = %.0f\n", cfg.tapSlop);
    std::fprintf(f, "double-tap-ms = %u\n", static_cast<unsigned>(cfg.doubleTapMs));
    std::fprintf(f, "edge-enabled = %d\n", cfg.edgeEnabled ? 1 : 0);
    std::fprintf(f, "edge-zone = %.3f\n", cfg.edgeZone);
    std::fprintf(f, "anim-ms = %u\n", static_cast<unsigned>(cfg.animMs));
    std::fprintf(f, "theme = %u\n", static_cast<unsigned>(cfg.theme));

    // One block per finger count, so the file reads like the window looks.
    for (int fi = 0; fi < slot::kFingerCounts; ++fi) {
        const int fingers = slot::kFirstFingers + fi;
        std::fprintf(f, "\n# ---- %d fingers ----\n", fingers);
        for (int k = 0; k < slot::KindCount; ++k) {
            const int s = slot::index(fingers, k);
            std::fprintf(f, "%s = %s\n", slotKey(s).c_str(),
                         actionKey(cfg.bind[s]));
        }
    }

    std::fprintf(f, "\n# payloads: a chord for \"custom\", a program for \"launch\"\n");
    std::fprintf(f, "# chord spelling: Ctrl/Alt/Shift/Win joined with '+', "
                    "then one key\n");
    for (int i = 0; i < slot::Count; ++i) {
        const std::string chord = chordToString(cfg.custom[i]);
        if (!chord.empty())
            std::fprintf(f, "%s = %s\n", customKey(i).c_str(), chord.c_str());
        if (!cfg.launch[i].empty())
            std::fprintf(f, "%s = %s\n", launchKey(i).c_str(),
                         cfg.launch[i].c_str());
    }

    if (std::fflush(f) != 0) {
        std::fclose(f);
        std::remove(tmp.c_str());
        return false;
    }
    std::fclose(f);

    // Replace atomically; on Windows rename() fails if the target exists.
    std::remove(path);
    if (std::rename(tmp.c_str(), path) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}
