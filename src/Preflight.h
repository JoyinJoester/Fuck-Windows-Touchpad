// Preflight.h — environment checks that must pass before any gesture can work.
//
// Two failure modes silently produce "zero frames" and look like a broken
// decoder, when in reality the hardware is simply not delivering data:
//
//   1. The touchpad itself is switched off
//      (Settings > Bluetooth & devices > Touchpad, master toggle).
//      Stored in HKCU\...\PrecisionTouchPad\Status\Enabled = 0.
//
//   2. Windows' own three/four finger gestures are still enabled, so every
//      swipe both drives our action and the system's.

#pragma once

#include <windows.h>
#include <string>

enum class TouchpadPower {
    Unknown,   // key missing / unreadable (older drivers, non-PTP hardware)
    Enabled,
    Disabled,
};

// Reads the per-user master switch. Does not modify anything.
TouchpadPower queryTouchpadPower();

// Writes the master switch. Requires no elevation (HKCU).
bool setTouchpadPower(bool enable);

// Returns a human readable dump of every value under the PrecisionTouchPad
// key. The three/four finger *action* settings are created lazily by the
// Settings app, so dumping this after the user changes them is how we learn
// where to write when we take over gesture suppression automatically.
std::wstring dumpPrecisionTouchpadRegistry();

// Bounded recursive search for anything named like a gesture/touchpad setting.
// Used once, to discover where Windows stores the three/four finger actions on
// this build -- they are absent from the PrecisionTouchPad key until the user
// changes them at least once.
std::wstring findGestureSettings();

// ---- system gesture suppression -------------------------------------------
// Windows' own three/four finger swipes stay active on the same HID stream we
// read, so every swipe would fire both our action and the system's. The flags
// live in HKCU\...\PrecisionTouchPad as ThreeFingerSlideEnabled /
// FourFingerSlideEnabled (created by the Settings app on first change).
//
// We snapshot the current state, write 0, and put the original values back when
// the app exits, so the user's system is left exactly as we found it.

struct GestureSuppression {
    struct Saved {
        const wchar_t* name = nullptr;
        bool  existed  = false;
        DWORD oldValue = 0;
    };
    Saved  items[4];
    int    count    = 0;
    bool   changed  = false;   // something was actually written
};

GestureSuppression suppressSystemGestures();
bool restoreSystemGestures(const GestureSuppression& s);
