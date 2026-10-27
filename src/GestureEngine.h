// GestureEngine.h — multi-finger swipe recognition
#pragma once

#include "Config.h"   // dir:: and slot:: — the engine reports in the same terms
#include "Touchpad.h"

#include <cstdint>

// What kind of one-shot gesture happened. The engine deliberately stays out of
// the mapping table: it says "three fingers swiped left", the caller decides
// which slot that is and what the slot runs.
enum class GestureKind : uint8_t {
    None = 0,
    Tap,     // quick touch, barely any travel; fires on release
    Swipe,   // committed directional movement
};

struct GestureEvent {
    GestureKind kind    = GestureKind::None;
    uint8_t     fingers = 0;                 // 3, 4 or 5
    uint8_t     direction = dir::Left;       // Swipe only
    int         corner  = -1;                // Tap only: 0 TL, 1 TR, 2 BL, 3 BR
    int         edge    = 0;                 // 0 middle, 1 left edge, 2 right edge

    explicit operator bool() const { return kind != GestureKind::None; }
};

// "4-finger swipe RIGHT", for the log.
const char* gestureName(const GestureEvent& g);

// Continuous three-finger drag. This is a separate channel from the swipe
// recogniser: swipes are discrete events fired once, a drag is a session that
// reports movement every frame and is what "three-finger drag" needs (move the
// window under the fingers, 1:1, like macOS).
enum class DragEvent : uint8_t {
    None = 0,
    Down,   // fingers have settled, a drag session began
    Move,   // dx/dy are the travel since the previous sample
    Up,     // fingers lifted, session over
};

struct DragSample {
    DragEvent ev      = DragEvent::None;
    float     dx      = 0.f;   // device units since the previous sample
    float     dy      = 0.f;
    uint8_t   fingers = 0;
};

// Continuous two-finger pinch. The gap between the two contacts is measured
// every frame; the ratio between consecutive frames is what the window scaler
// consumes, so the resize follows the fingers exactly and stops the moment they
// do. Begin/End bracket a session, which is how the window layer knows when to
// latch its target and when to let go.
enum class PinchEvent : uint8_t {
    None = 0,
    Begin,   // the gap has moved past the engagement threshold
    Move,    // ratio is current gap / previous gap
    End,     // a finger lifted
};

// Which axes a pinch is allowed to drive. The gap between two fingers is a
// single number, but the window has two dimensions, so something has to decide
// how that number is split.
enum class PinchAxis : uint8_t {
    Uniform = 0,     // width and height always move together (aspect kept)
    Auto    = 1,     // split by the angle of the two-finger line
    Horizontal = 2,  // width only
    Vertical   = 3,  // height only
};

struct PinchSample {
    PinchEvent ev      = PinchEvent::None;
    float      ratio   = 1.f;   // 1.0 = gap unchanged
    float      ratioX  = 1.f;   // part of `ratio` that drives the width
    float      ratioY  = 1.f;   // part that drives the height
    uint8_t    fingers = 0;
};

struct GestureConfig {
    // Displacement (device units) required to commit a swipe.
    // Measured on real hardware: a full-width 3-finger swipe travels 400-800
    // units, so 300 leaves good margin against accidental contact.
    float thresholdX = 300.f;
    float thresholdY = 250.f;

    // Axis dominance: horizontal motion must exceed vertical by this factor.
    float dominance = 1.5f;

    // If the fingers rest nearly still for this long, re-anchor the origin.
    uint64_t holdResetUs = 300000;

    // Frames the finger count must stay stable before tracking starts.
    uint8_t stableFrames = 2;

    // How many fingers start a drag session. Restricting the drag channel to
    // exactly this many fingers is what leaves the other count free for the
    // swipe mapping table -- otherwise a three-finger drag and a three-finger
    // swipe would both fire from the same movement.
    uint8_t dragFingers = 3;

    // Two-finger pinch. The gap between the fingers has to change by this
    // fraction of its value before a resize session starts.
    //
    // This is deliberately small. Telling a pinch apart from a two-finger pan
    // is the job of the direction test in updatePinch(), not of a wide dead
    // band, and a wide dead band is what makes the gesture feel like the first
    // centimetre of movement is wasted. Whatever the dead band does skip is
    // applied as the opening ratio of the session, so the window never lags
    // behind the fingers by that amount.
    float pinchDeadband = 0.015f;

    // Frames the two-finger count must hold before the gap is measured. A
    // finger lifting out of a three-finger drag passes through "two fingers"
    // for a frame or two, and that must not be read as a pinch.
    //
    // One frame is enough because arming only starts measuring; the direction
    // test still has to pass before the window is touched at all.
    uint8_t pinchStableFrames = 1;

    // A newly ended drag blocks pinch for this long, for the same reason.
    // Short on purpose: lifting into a pinch is a normal transition and the
    // old 250 ms was felt as "the first pinch after a drag does nothing".
    uint64_t pinchCooldownUs = 180000;

    // How the gap change is split across the two window axes.
    PinchAxis pinchAxis = PinchAxis::Auto;

    // ---- tap ----
    bool     tapEnabled = true;
    uint32_t tapMaxMs   = 220;   // longest touch still counted as a tap
    // A real fingertip drifts while tapping: the contact patch rolls and the
    // centroid wanders a couple of millimetres even on a deliberate tap. 10
    // units (~0.7 mm) rejected most genuine taps on real hardware, so this
    // accepts a small slide instead of demanding a perfectly still finger.
    float    tapSlop    = 25.f;  // device units of travel still counted as a tap

    // ---- edge detection ----
    // Needs the pad's logical size. 0 means "unknown", which simply disables
    // the edge and corner flags instead of guessing.
    uint32_t rangeX      = 0;
    uint32_t rangeY      = 0;
    float    edgeZone    = 0.12f;  // fraction of the width that counts as edge
    bool     edgeEnabled = false;
    // How big a corner is, as a fraction of each axis. 0 disables corner taps.
    float    cornerZone  = 0.18f;
};

class GestureEngine {
public:
    explicit GestureEngine(GestureConfig cfg = GestureConfig{});

    // Applies new recognition parameters without losing the current session.
    // Safe to call while frames are arriving; a gesture in flight keeps the
    // origin it started with.
    void setConfig(const GestureConfig& cfg);
    const GestureConfig& config() const { return cfg_; }

    void feed(const ContactFrame& frame);

    // Returns a gesture exactly once, then an empty event until the next one.
    GestureEvent poll();

    // Drains the drag channel. Call once per frame, after feed().
    DragSample takeDrag();

    // Drains the pinch channel. Call once per frame, after takeDrag().
    PinchSample takePinch();

    // Diagnostics
    int      trackedFingers() const { return fingers_; }
    bool     isTracking() const { return state_ == State::Tracking; }
    bool     isDragging() const { return dragActive_; }
    bool     isPinching() const { return pinchActive_; }

private:
    enum class State { Idle, Tracking, Fired };

    void updateDrag(const ContactFrame& frame);
    void updatePinch(const ContactFrame& frame);
    void updateTap(const ContactFrame& frame);

    GestureConfig cfg_;

    State    state_        = State::Idle;
    uint8_t  pendingCount_ = 0;
    uint8_t  pendingFrames_= 0;

    int      fingers_      = 0;
    float    startX_       = 0.f;
    float    startY_       = 0.f;
    uint64_t startTimeUs_  = 0;

    // Position when the gesture fired. Used to detect a reversal so a second
    // gesture can start without lifting the fingers.
    float    fireX_        = 0.f;
    float    fireY_        = 0.f;

    GestureEvent pending_{};

    // ---- drag channel ----
    bool       dragActive_    = false;
    uint8_t    dragFingers_   = 0;
    uint8_t    dragPending_   = 0;   // candidate finger count while settling
    uint8_t    dragFrames_    = 0;
    float      dragLastX_     = 0.f;
    float      dragLastY_     = 0.f;
    DragSample dragOut_{};

    // ---- pinch channel ----
    bool        pinchActive_   = false;
    bool        pinchArmed_    = false;  // two fingers down, gap not measured yet
    uint8_t     pinchFrames_   = 0;
    float       pinchArmDist_  = 0.f;    // gap at the moment the pair settled
    float       pinchLastDist_ = 0.f;    // gap on the previous frame
    uint64_t    pinchBlockedUntilUs_ = 0;
    PinchSample pinchOut_{};

    // Previous position of each of the two contacts, so the engine can tell
    // "fingers spreading" from "both fingers sliding the same way". Matched by
    // contact id because the decoder is free to reorder the vector.
    bool     pinchPrevValid_ = false;
    uint8_t  pinchPrevId_[2] = {0, 0};
    float    pinchPrevX_[2]  = {0.f, 0.f};
    float    pinchPrevY_[2]  = {0.f, 0.f};

    // ---- contact session (tap + edge origin) ----
    bool     sessOpen_     = false;
    uint8_t  sessMaxN_     = 0;
    uint64_t sessStartUs_  = 0;
    float    sessStartX_   = 0.f;
    float    sessStartY_   = 0.f;
    float    sessTravel_   = 0.f;   // furthest the centroid ever got from start
    bool     sessUsed_     = false; // a swipe / drag / pinch already used it
    int      sessEdge_     = 0;     // 0 none, 1 left, 2 right
    int      sessCorner_   = -1;    // 0 TL, 1 TR, 2 BL, 3 BR, -1 none
};
