// WindowManager.h — the "do something" half of a gesture.
//
// The gesture engine only produces an intent (swipe left / right). This module
// owns everything that touches other applications' windows: picking a target,
// validating it, and animating the exchange.
//
// Deliberate design choices:
//   * We never intercept the system's own drag logic (no hooks, no injection).
//     We simply call SetWindowPos on windows we are allowed to touch. FancyZones
//     works the same way, and it keeps us out of the "desktop crashes when we
//     crash" failure mode that injected mods have.
//   * The foreground window is tracked by an out-of-context WinEvent hook, and
//     windows belonging to our own process are ignored. Otherwise the console
//     window the user launched us from would be the swap target.

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

enum class SwapDir { Left, Right };

namespace wm {

// Installs the foreground hook. Call once, from the thread that pumps messages.
void init();
void shutdown();

// Last foreground window that is not ours. May be null or stale; every use is
// re-validated.
HWND lastActive();

// Swaps the tracked window with its nearest neighbour in `dir`, animating both.
// `message` always receives a human readable result or the reason it was
// skipped.
bool swapFocusedWith(SwapDir dir, std::string* message);

// ---- three-finger window drag ---------------------------------------------
// Device units are converted to pixels using the touchpad logical range and the
// work area of the monitor the target lives on, so a full-width finger sweep
// crosses the whole screen no matter how large the touchpad is.
//
// The target is the window under the cursor (matching how the same gesture
// behaves on macOS), falling back to the last active window if the cursor is
// over the desktop or over our own console.
void setTouchpadRange(uint32_t maxX, uint32_t maxY);

bool dragBegin(std::string* message);
void dragMove(float dxUnits, float dyUnits);
void dragEnd();
bool dragging();
HWND dragTarget();

// Title of the window currently being dragged, or "" when idle. Used by the
// control panel to show what the gesture grabbed.
std::string dragTargetTitle();

// ---- two-finger pinch resize ----------------------------------------------
// Same target rule as the drag (window under the cursor). The window is scaled
// by the ratio between the current and the previous finger gap, anchored on the
// point under the fingers so it grows away from the hand rather than around its
// own centre. The size is tracked in floating point across the whole session,
// so a slow pinch that nudges the gap by a fraction of a pixel per frame still
// accumulates instead of being rounded away.
//
// Limits are the work area and a minimum usable size, applied to each axis on
// its own so the aspect ratio can be changed by hand. Overshooting one
// compresses the movement instead of stopping dead, and pinchend() animates the
// window back inside the limits.
bool pinchBegin(std::string* message);
// ratioX / ratioY are per-axis multipliers for this frame; pass the same value
// twice for a uniform zoom.
void pinchMove(float ratioX, float ratioY);
void pinchEnd();
bool pinching();
std::string pinchTargetTitle();

// anchored = scale around the point under the fingers instead of the centre.
// rubberBand = allow compressed overshoot and spring back on release.
void setPinchBehaviour(bool anchored, bool rubberBand);

// Length of the swap / tile / rubber-band animations. 0 disables them.
void setAnimMs(uint32_t ms);

// Advances running animations. Call on every main-loop pass; it is cheap when
// nothing is animating.
void tick();

bool busy();

// Debug: every window that currently passes the candidate filter, with its
// class and rect. The first thing to look at when a swap is skipped.
std::string describeCandidates();

}  // namespace wm
