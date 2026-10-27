// GestureEngine.cpp
#include "GestureEngine.h"

#include <cmath>
#include <cstdio>

const char* gestureName(const GestureEvent& g) {
    static char buf[64];
    const char* dirName = g.direction == dir::Left    ? "LEFT"
                          : g.direction == dir::Right ? "RIGHT"
                          : g.direction == dir::Up    ? "UP"
                                                      : "DOWN";
    if (g.kind == GestureKind::Tap) {
        static const char* kCorner[4] = {"top-left", "top-right", "bottom-left",
                                         "bottom-right"};
        if (g.corner >= 0 && g.corner < 4)
            std::snprintf(buf, sizeof(buf), "%u-finger tap (%s)", g.fingers,
                          kCorner[g.corner]);
        else
            std::snprintf(buf, sizeof(buf), "%u-finger tap", g.fingers);
        return buf;
    }
    if (g.kind == GestureKind::Swipe) {
        std::snprintf(buf, sizeof(buf), "%u-finger swipe %s", g.fingers, dirName);
        return buf;
    }
    return "(none)";
}

GestureEngine::GestureEngine(GestureConfig cfg) : cfg_(cfg) {}

void GestureEngine::setConfig(const GestureConfig& cfg) {
    cfg_ = cfg;
    if (cfg_.dragFingers < 3) cfg_.dragFingers = 3;
    if (cfg_.dragFingers > 4) cfg_.dragFingers = 4;
    if (cfg_.pinchDeadband < 0.005f) cfg_.pinchDeadband = 0.005f;
    if (cfg_.pinchDeadband > 0.30f) cfg_.pinchDeadband = 0.30f;
    if (cfg_.pinchStableFrames < 1) cfg_.pinchStableFrames = 1;
    if (cfg_.tapMaxMs < 80) cfg_.tapMaxMs = 80;
    if (cfg_.tapSlop < 2.f) cfg_.tapSlop = 2.f;
    // A live drag session survives a config change, but only if it still
    // matches the finger count the user just chose. Otherwise the session would
    // run with a stale finger count and never terminate cleanly.
    if (dragActive_ && dragFingers_ != cfg_.dragFingers) {
        dragActive_  = false;
        dragPending_ = 0;
        dragFrames_  = 0;
        dragOut_.ev  = DragEvent::Up;
        dragOut_.dx  = 0.f;
        dragOut_.dy  = 0.f;
        dragOut_.fingers = dragFingers_;
    }
}

void GestureEngine::updateDrag(const ContactFrame& frame) {
    const size_t n = frame.contacts.size();

    if (!dragActive_) {
        // Only the configured finger count starts a drag. Everything else --
        // including more fingers than configured -- belongs to the swipe
        // recogniser instead.
        if (n != cfg_.dragFingers) {
            dragPending_ = 0;
            dragFrames_  = 0;
            return;
        }
        // Same settle requirement as swipes: the finger count has to hold for a
        // couple of frames, otherwise a palm slap followed by two fingers would
        // start a drag the user never asked for.
        if (dragPending_ != n) {
            dragPending_ = static_cast<uint8_t>(n);
            dragFrames_  = 1;
            return;
        }
        if (++dragFrames_ < cfg_.stableFrames) return;

        dragActive_  = true;
        dragFingers_ = static_cast<uint8_t>(n);
        dragLastX_   = frame.centroidX();
        dragLastY_   = frame.centroidY();
        dragOut_.ev      = DragEvent::Down;
        dragOut_.dx      = 0.f;
        dragOut_.dy      = 0.f;
        dragOut_.fingers = dragFingers_;
        return;
    }

    if (n != dragFingers_) {
        dragActive_  = false;
        dragPending_ = 0;
        dragFrames_  = 0;
        dragOut_.ev      = DragEvent::Up;
        dragOut_.dx      = 0.f;
        dragOut_.dy      = 0.f;
        dragOut_.fingers = dragFingers_;
        pinchBlockedUntilUs_ = frame.timestampUs + cfg_.pinchCooldownUs;
        return;
    }

    const float x = frame.centroidX();
    const float y = frame.centroidY();
    dragOut_.dx += x - dragLastX_;
    dragOut_.dy += y - dragLastY_;
    dragLastX_ = x;
    dragLastY_ = y;
    // Down wins if it happened in the same frame, so the consumer always sees
    // the session start before any movement.
    if (dragOut_.ev != DragEvent::Down) dragOut_.ev = DragEvent::Move;
    dragOut_.fingers = dragFingers_;
}

namespace {

// Splits one gap ratio across the two window axes. `wx`/`wy` are weights in
// [0,1]: the scalar ratio is applied as 1 + (ratio-1)*w per axis, so a purely
// horizontal pinch changes only the width while a 45 degree one is identical to
// the old uniform zoom.
void pinchWeights(float dx, float dy, PinchAxis mode, float* wx, float* wy) {
    *wx = 1.f;
    *wy = 1.f;
    switch (mode) {
    case PinchAxis::Horizontal: *wy = 0.f; return;
    case PinchAxis::Vertical:   *wx = 0.f; return;
    case PinchAxis::Uniform:    return;
    case PinchAxis::Auto:
    default: break;
    }

    const float ax    = std::fabs(dx);
    const float ay    = std::fabs(dy);
    const float major = ax > ay ? ax : ay;
    if (major < 1.f) return;  // fingers nearly on top of each other: no angle

    // 1 = dead horizontal/vertical, 0 = exactly 45 degrees. Cubed, so a roughly
    // horizontal pinch does not creep the height, while the way back to uniform
    // stays continuous -- a hard threshold would snap the aspect ratio the
    // moment the fingers drifted a few degrees.
    float t = (ax < ay ? ax : ay) / major;
    if (t > 1.f) t = 1.f;
    const float minor = t * t * t;
    if (ax >= ay) *wy = minor;
    else          *wx = minor;
}

}  // namespace

void GestureEngine::updatePinch(const ContactFrame& frame) {
    const size_t n = frame.contacts.size();

    // A pinch needs exactly two fingers. Anything else ends a running session
    // and clears the arming state, so the next two-finger contact starts from a
    // freshly measured gap instead of a stale one.
    if (n != 2) {
        pinchFrames_ = 0;
        pinchPrevValid_ = false;
        if (pinchActive_) {
            pinchActive_     = false;
            pinchArmed_      = false;
            pinchOut_.ev     = PinchEvent::End;
            pinchOut_.ratio  = 1.f;
            pinchOut_.fingers = 2;
        }
        pinchArmed_ = false;
        return;
    }

    const Contact& a = frame.contacts[0];
    const Contact& b = frame.contacts[1];
    const float dx = static_cast<float>(a.x) - static_cast<float>(b.x);
    const float dy = static_cast<float>(a.y) - static_cast<float>(b.y);
    const float dist = std::sqrt(dx * dx + dy * dy);

    // Per-contact movement since the previous frame. This is what separates a
    // pinch from a two-finger pan: pinching means the two fingers travel in
    // opposite directions, panning means they travel the same way. Matching by
    // id keeps it correct even if the decoder reorders the contacts.
    const bool havePrev = pinchPrevValid_ && pinchPrevId_[0] == a.id &&
                          pinchPrevId_[1] == b.id;
    const float aDx = havePrev ? static_cast<float>(a.x) - pinchPrevX_[0] : 0.f;
    const float aDy = havePrev ? static_cast<float>(a.y) - pinchPrevY_[0] : 0.f;
    const float bDx = havePrev ? static_cast<float>(b.x) - pinchPrevX_[1] : 0.f;
    const float bDy = havePrev ? static_cast<float>(b.y) - pinchPrevY_[1] : 0.f;
    pinchPrevValid_ = true;
    pinchPrevId_[0] = a.id;
    pinchPrevId_[1] = b.id;
    pinchPrevX_[0]  = static_cast<float>(a.x);
    pinchPrevY_[0]  = static_cast<float>(a.y);
    pinchPrevX_[1]  = static_cast<float>(b.x);
    pinchPrevY_[1]  = static_cast<float>(b.y);

    if (pinchActive_) {
        if (dist <= 1.f) return;  // degenerate sample, keep the last gap
        float raw = dist / pinchLastDist_;
        // No single frame can legitimately double the gap; the clamp keeps a
        // decode glitch or a slipping finger from slamming the window.
        if (raw < 0.5f) raw = 0.5f;
        if (raw > 2.0f) raw = 2.0f;

        float wx = 1.f, wy = 1.f;
        pinchWeights(dx, dy, cfg_.pinchAxis, &wx, &wy);

        pinchOut_.ev      = PinchEvent::Move;
        pinchOut_.ratio   = raw;
        pinchOut_.ratioX  = 1.f + (raw - 1.f) * wx;
        pinchOut_.ratioY  = 1.f + (raw - 1.f) * wy;
        pinchOut_.fingers = 2;
        pinchLastDist_    = dist;
        return;
    }

    // Refuse to arm right after a drag. Lifting the third finger leaves two
    // contacts for a few frames and their gap is whatever the hand happens to
    // be doing -- reacting to that would resize a window the user just moved.
    if (static_cast<int64_t>(frame.timestampUs - pinchBlockedUntilUs_) < 0) {
        pinchFrames_ = 0;
        pinchArmed_  = false;
        return;
    }

    if (!pinchArmed_) {
        // Fingers too close together: the gap is dominated by jitter, so a
        // percentage threshold would fire on noise. 60 units is about 4 mm --
        // narrower than any deliberate two-finger pinch starts, and the pad
        // cannot resolve two fingertips much closer than this anyway.
        constexpr float kMinArmGap = 60.f;
        if (dist < kMinArmGap) {
            pinchFrames_ = 0;
            return;
        }
        if (++pinchFrames_ < (cfg_.pinchStableFrames ? cfg_.pinchStableFrames : 1))
            return;
        pinchArmed_     = true;
        pinchArmDist_   = dist;
        pinchLastDist_  = dist;
        return;
    }

    if (pinchArmDist_ <= 1.f) {
        pinchArmDist_ = dist;
        return;
    }

    const float drift = std::fabs(dist / pinchArmDist_ - 1.f);
    if (drift < cfg_.pinchDeadband) return;

    // The dead band only has to swallow jitter, so the real gate is this test:
    // unless the two fingers are moving against each other, this is a pan and
    // not a pinch. A frame with no measurable movement is let through rather
    // than blocking the gesture on a perfectly still sample.
    if (havePrev) {
        const float dot = aDx * bDx + aDy * bDy;
        const float ma  = std::sqrt(aDx * aDx + aDy * aDy);
        const float mb  = std::sqrt(bDx * bDx + bDy * bDy);
        const float mag = ma * mb;
        if (mag > 4.f && dot > -0.15f * mag) return;
    }

    // Engaged. The gap change the dead band swallowed is handed back as the
    // opening ratio instead of being dropped: without that the window would sit
    // permanently that much smaller/larger than the fingers for the rest of the
    // gesture, which is exactly what "the pinched window lags behind my hand"
    // feels like.
    float raw = pinchArmDist_ > 1.f ? dist / pinchArmDist_ : 1.f;
    if (raw < 0.5f) raw = 0.5f;
    if (raw > 2.0f) raw = 2.0f;

    float wx = 1.f, wy = 1.f;
    pinchWeights(dx, dy, cfg_.pinchAxis, &wx, &wy);

    pinchActive_      = true;
    pinchLastDist_    = dist;
    pinchOut_.ev      = PinchEvent::Begin;
    pinchOut_.ratio   = raw;
    pinchOut_.ratioX  = 1.f + (raw - 1.f) * wx;
    pinchOut_.ratioY  = 1.f + (raw - 1.f) * wy;
    pinchOut_.fingers = 2;
}

PinchSample GestureEngine::takePinch() {
    PinchSample s = pinchOut_;
    pinchOut_.ev     = PinchEvent::None;
    pinchOut_.ratio  = 1.f;
    pinchOut_.ratioX = 1.f;
    pinchOut_.ratioY = 1.f;
    return s;
}

DragSample GestureEngine::takeDrag() {
    DragSample s = dragOut_;
    dragOut_.ev = DragEvent::None;
    dragOut_.dx = 0.f;
    dragOut_.dy = 0.f;
    return s;
}

// Tracks one "touch down -> all fingers up" session and decides, at the end,
// whether it was a tap. Doing it on release is what lets a tap coexist with the
// movement gestures: by then the swipe, drag and pinch channels have all had
// their chance and none of them fired.
//
// The session also remembers the region the touch started in, because a tap in
// a corner is a different binding from a tap in the middle even though the
// movement is identical.
void GestureEngine::updateTap(const ContactFrame& frame) {
    const size_t n = frame.contacts.size();

    if (n > 0) {
        const float cx = frame.centroidX();
        const float cy = frame.centroidY();
        if (!sessOpen_) {
            sessOpen_    = true;
            sessMaxN_    = static_cast<uint8_t>(n);
            sessStartUs_ = frame.timestampUs;
            sessStartX_  = cx;
            sessStartY_  = cy;
            sessTravel_  = 0.f;
            sessEdge_    = 0;
            sessCorner_  = -1;
            if (cfg_.edgeEnabled && cfg_.rangeX > 0) {
                const float f = cx / static_cast<float>(cfg_.rangeX);
                if (f < cfg_.edgeZone)
                    sessEdge_ = 1;
                else if (f > 1.f - cfg_.edgeZone)
                    sessEdge_ = 2;
            }
            // A corner needs both axes inside the band, which is what keeps the
            // whole left third of the pad from counting as "top-left".
            if (cfg_.rangeX > 0 && cfg_.rangeY > 0 && cfg_.cornerZone > 0.f) {
                const float fx = cx / static_cast<float>(cfg_.rangeX);
                const float fy = cy / static_cast<float>(cfg_.rangeY);
                const int ix = fx < cfg_.cornerZone
                                   ? 0
                                   : (fx > 1.f - cfg_.cornerZone ? 1 : -1);
                const int iy = fy < cfg_.cornerZone
                                   ? 0
                                   : (fy > 1.f - cfg_.cornerZone ? 1 : -1);
                if (ix >= 0 && iy >= 0) sessCorner_ = iy * 2 + ix;
            }
        } else {
            if (static_cast<uint8_t>(n) > sessMaxN_)
                sessMaxN_ = static_cast<uint8_t>(n);
            const float dx = cx - sessStartX_;
            const float dy = cy - sessStartY_;
            const float t  = std::sqrt(dx * dx + dy * dy);
            if (t > sessTravel_) sessTravel_ = t;
        }
        return;
    }

    if (!sessOpen_) return;

    const uint64_t durUs = frame.timestampUs - sessStartUs_;
    const bool quick  = durUs <= static_cast<uint64_t>(cfg_.tapMaxMs) * 1000ull;
    const bool still  = sessTravel_ <= cfg_.tapSlop;
    if (cfg_.tapEnabled && quick && still && sessMaxN_ >= 3 &&
        pending_.kind == GestureKind::None) {
        pending_.kind    = GestureKind::Tap;
        pending_.fingers = sessMaxN_ > 5 ? 5 : sessMaxN_;
        pending_.corner  = sessCorner_;
        pending_.edge    = sessEdge_;
    }

    sessOpen_   = false;
    sessMaxN_   = 0;
    sessEdge_   = 0;
    sessCorner_ = -1;
}

void GestureEngine::feed(const ContactFrame& frame) {
    // The drag and pinch channels run on their own state machines and must see
    // every frame, including the one where all fingers lift.
    updateDrag(frame);
    updatePinch(frame);
    updateTap(frame);

    const size_t n = frame.contacts.size();

    // All fingers lifted: end of an interaction.
    if (n == 0) {
        state_         = State::Idle;
        pendingCount_  = 0;
        pendingFrames_ = 0;
        fingers_       = 0;
        return;
    }

    // Once fired, stay latched until the fingers come up OR the user clearly
    // reverses direction — otherwise a single long swipe would emit dozens of
    // duplicate gestures, while a back-and-forth would only ever fire once.
    if (state_ == State::Fired) {
        const float dx = frame.centroidX() - fireX_;
        const float dy = frame.centroidY() - fireY_;
        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);

        if (static_cast<int>(n) != fingers_) {
            state_ = State::Idle;
            pendingCount_ = 0;
            pendingFrames_ = 0;
            fingers_ = 0;
            return;
        }
        if (ax > cfg_.thresholdX * 0.6f || ay > cfg_.thresholdY * 0.6f) {
            // Meaningful travel away from the fire point: treat as a new gesture.
            state_        = State::Tracking;
            startX_       = frame.centroidX();
            startY_       = frame.centroidY();
            startTimeUs_  = frame.timestampUs;
        }
        return;
    }

    if (state_ == State::Idle) {
        // Three to five fingers. Five is rare but real on larger pads, and
        // refusing it here would make the fifth finger silently behave like a
        // fourth. More than five is a palm, not a gesture.
        if (n < 3 || n > 5) return;

        if (pendingCount_ != n) {
            pendingCount_  = static_cast<uint8_t>(n);
            pendingFrames_ = 1;
            return;
        }
        if (++pendingFrames_ < cfg_.stableFrames) return;

        // Finger count is stable — start tracking from here.
        state_       = State::Tracking;
        fingers_     = static_cast<int>(n);
        startX_      = frame.centroidX();
        startY_      = frame.centroidY();
        startTimeUs_ = frame.timestampUs;
        return;
    }

    // ---- Tracking ----
    if (static_cast<int>(n) != fingers_) {
        // Contact count changed mid-gesture: abort rather than guess.
        state_        = State::Idle;
        pendingCount_ = 0;
        pendingFrames_= 0;
        fingers_      = 0;
        return;
    }

    const float dx = frame.centroidX() - startX_;
    const float dy = frame.centroidY() - startY_;
    const float ax = std::fabs(dx);
    const float ay = std::fabs(dy);

    // Fingers resting still: re-anchor so a "hold, then swipe" still works and
    // slow drift doesn't accumulate into a false positive.
    if (ax < 40.f && ay < 40.f &&
        frame.timestampUs - startTimeUs_ > cfg_.holdResetUs) {
        startX_      = frame.centroidX();
        startY_      = frame.centroidY();
        startTimeUs_ = frame.timestampUs;
        return;
    }

    GestureEvent ev;

    if (ax > cfg_.thresholdX && ax > ay * cfg_.dominance) {
        ev.kind      = GestureKind::Swipe;
        ev.direction = (dx < 0.f) ? dir::Left : dir::Right;
    } else if (ay > cfg_.thresholdY && ay > ax * cfg_.dominance) {
        // Device Y grows downward, so a negative dy is an upward swipe.
        ev.kind      = GestureKind::Swipe;
        ev.direction = (dy < 0.f) ? dir::Up : dir::Down;
    }

    if (ev.kind != GestureKind::None) {
        ev.fingers = static_cast<uint8_t>(fingers_);
        ev.edge    = sessEdge_;   // 0 = middle, 1 = left edge, 2 = right edge
        ev.corner  = sessCorner_;
        pending_   = ev;
        state_     = State::Fired;
        fireX_     = frame.centroidX();
        fireY_     = frame.centroidY();
    }
}

GestureEvent GestureEngine::poll() {
    const GestureEvent g = pending_;
    pending_ = GestureEvent{};
    return g;
}
