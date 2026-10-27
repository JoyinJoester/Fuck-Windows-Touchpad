// pinch_selftest.cpp — offline regression tests for the continuous channels
// (two-finger pinch, three/four-finger tap) and for the edge flag.
//
// Synthesised frames only: the point is to pin down the *policy* (dead band,
// direction test, cooldown, scroll rejection, tap timing) rather than to
// re-decode hardware data.
//
// Build:
//   g++ -std=c++17 -O2 pinch_selftest.cpp ../src/GestureEngine.cpp
//       ../src/Touchpad.cpp -I../src -o pinch_selftest.exe -lhid -lsetupapi

#include "GestureEngine.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

constexpr uint64_t kFrameUs = 20000;  // 50 Hz, same ballpark as the real device

int g_fail = 0;

void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

ContactFrame twoFingers(float dist, float cx, float cy, uint64_t t) {
    ContactFrame f;
    f.timestampUs  = t;
    f.contactCount = 2;
    Contact a;
    a.id = 1; a.tip = true;
    a.x = static_cast<uint16_t>(cx - dist / 2.f);
    a.y = static_cast<uint16_t>(cy);
    Contact b;
    b.id = 2; b.tip = true;
    b.x = static_cast<uint16_t>(cx + dist / 2.f);
    b.y = static_cast<uint16_t>(cy);
    f.contacts = {a, b};
    return f;
}

ContactFrame manyFingers(int n, float dist, float cx, float cy, uint64_t t) {
    ContactFrame f;
    f.timestampUs  = t;
    f.contactCount = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) {
        Contact c;
        c.id  = static_cast<uint8_t>(i + 1);
        c.tip = true;
        c.x   = static_cast<uint16_t>(cx + i * dist);
        c.y   = static_cast<uint16_t>(cy);
        f.contacts.push_back(c);
    }
    return f;
}

ContactFrame none(uint64_t t) {
    ContactFrame f;
    f.timestampUs = t;
    return f;
}

// Two contacts separated by (dx, dy), centred on (cx, cy). Lets a test pinch
// along any direction, which is what the per-axis split keys off.
ContactFrame twoFingersVec(float dx, float dy, float cx, float cy,
                           uint64_t t) {
    ContactFrame f;
    f.timestampUs  = t;
    f.contactCount = 2;
    Contact a;
    a.id = 1; a.tip = true;
    a.x = static_cast<uint16_t>(cx - dx / 2.f);
    a.y = static_cast<uint16_t>(cy - dy / 2.f);
    Contact b;
    b.id = 2; b.tip = true;
    b.x = static_cast<uint16_t>(cx + dx / 2.f);
    b.y = static_cast<uint16_t>(cy + dy / 2.f);
    f.contacts = {a, b};
    return f;
}

// Runs a frame through the engine and accumulates the pinch channel output.
struct PinchRun {
    int   begins = 0;
    int   ends   = 0;
    int   moves  = 0;
    double scale = 1.0;   // product of every ratio, Begin included
    double scaleX = 1.0;  // same, per axis
    double scaleY = 1.0;
    float  firstRatio = 0.f;
};

PinchRun run(GestureEngine& e, const std::vector<ContactFrame>& frames,
             std::function<void(int)> onFrame = nullptr) {
    PinchRun r;
    for (size_t i = 0; i < frames.size(); ++i) {
        e.feed(frames[i]);
        e.takeDrag();  // drained and ignored, exactly like main.cpp
        const PinchSample p = e.takePinch();
        switch (p.ev) {
        case PinchEvent::Begin:
            ++r.begins;
            if (r.firstRatio == 0.f) r.firstRatio = p.ratio;
            // The application applies the opening ratio too, so the dead band
            // travel is not lost. The test has to model that or it would be
            // measuring the old behaviour.
            r.scale  *= p.ratio;
            r.scaleX *= p.ratioX;
            r.scaleY *= p.ratioY;
            break;
        case PinchEvent::Move:
            ++r.moves;
            r.scale  *= p.ratio;
            r.scaleX *= p.ratioX;
            r.scaleY *= p.ratioY;
            break;
        case PinchEvent::End:
            ++r.ends;
            break;
        default:
            break;
        }
        if (onFrame) onFrame(static_cast<int>(i));
    }
    return r;
}

std::vector<ContactFrame> spread(float from, float to, int frames,
                                 uint64_t& t, float cx = 750.f,
                                 float cy = 460.f) {
    std::vector<ContactFrame> v;
    for (int i = 0; i < frames; ++i) {
        const float k = frames > 1 ? (float)i / (float)(frames - 1) : 1.f;
        v.push_back(twoFingers(from + (to - from) * k, cx, cy, t));
        t += kFrameUs;
    }
    return v;
}

}  // namespace

int main() {
    // ---- 1. pinch out: gap 400 -> 700 --------------------------------------
    {
        printf("1. pinch OUT 400 -> 700 units\n");
        GestureEngine e;
        uint64_t t = 1000000;  // well past any cooldown
        std::vector<ContactFrame> fr = spread(400.f, 700.f, 31, t);
        fr.push_back(none(t));
        PinchRun r = run(e, fr);
        check(r.begins == 1, "exactly one Begin");
        check(r.ends == 1, "exactly one End when the fingers lift");
        check(r.moves > 20, "Move fires once per frame while engaged");
        // The dead band no longer costs anything: whatever it swallowed is
        // handed back as the opening ratio, so the final size is exactly the
        // finger ratio. This is the property that makes the window feel like it
        // is glued to the hand instead of trailing it.
        const double expect = 700.0 / 400.0;
        const double err = std::fabs(r.scale - expect) / expect;
        printf("       scale=%.4f expect=%.4f (err %.2f%%), first ratio %.4f\n",
               r.scale, expect, err * 100.0, (double)r.firstRatio);
        check(err < 0.001, "final size equals the finger ratio exactly");
        check(r.firstRatio > 1.0f,
              "Begin already carries the gap change (no lost travel)");
    }

    // ---- 2. pinch in: gap 700 -> 400 ---------------------------------------
    {
        printf("2. pinch IN 700 -> 400 units\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr = spread(700.f, 400.f, 31, t);
        fr.push_back(none(t));
        PinchRun r = run(e, fr);
        check(r.begins == 1, "exactly one Begin");
        check(r.scale < 0.85, "window shrinks");
        printf("       scale=%.4f\n", r.scale);
    }

    // ---- 3. two-finger scroll must not resize ------------------------------
    {
        printf("3. two-finger SCROLL (gap constant, centroid moves)\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 60; ++i)
            fr.push_back(twoFingers(400.f + (i % 3) * 4.f, 750.f,
                                    460.f - i * 12.f, t + i * kFrameUs));
        fr.push_back(none(t + 60 * kFrameUs));
        PinchRun r = run(e, fr);
        check(r.begins == 0, "no Begin: the gap never passed the dead band");
        check(r.moves == 0, "no Move");
    }

    // ---- 4. fingers too close together ------------------------------------
    {
        printf("4. two fingers resting 20 units apart, jittering\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 40; ++i)
            fr.push_back(twoFingers(20.f + (i % 5) * 3.f, 750.f, 460.f,
                                    t + i * kFrameUs));
        fr.push_back(none(t + 40 * kFrameUs));
        PinchRun r = run(e, fr);
        check(r.begins == 0, "no Begin: jitter on a tiny gap is ignored");
    }

    // ---- 5. a three-finger drag ending in two fingers ---------------------
    {
        printf("5. three-finger drag, third finger lifts, pair drifts apart\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 20; ++i)  // drag
            fr.push_back(manyFingers(3, 120.f, 700.f, 460.f, t + i * kFrameUs));
        // Third finger lifts and the remaining two spread out hard, which is
        // exactly what a sloppy release looks like. Three frames is well inside
        // the cooldown.
        for (int i = 0; i < 3; ++i)
            fr.push_back(twoFingers(300.f + i * 60.f, 750.f, 460.f,
                                    t + (20 + i) * kFrameUs));
        PinchRun r = run(e, fr);
        check(r.begins == 0, "no Begin inside the post-drag cooldown");

        // After the cooldown a deliberate pinch on the same pair must work.
        uint64_t t2 = t + 30 * kFrameUs + 400000;
        std::vector<ContactFrame> fr2 = spread(400.f, 700.f, 31, t2);
        PinchRun r2 = run(e, fr2);
        check(r2.begins == 1, "Begin once the cooldown has passed");
    }

    // ---- 6. a single finger and four fingers are not a pinch --------------
    {
        printf("6. one and four fingers never pinch\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 10; ++i) fr.push_back(none(t + i * kFrameUs));
        for (int i = 0; i < 20; ++i)
            fr.push_back(manyFingers(4, 100.f + i * 10.f, 600.f, 460.f,
                                     t + (10 + i) * kFrameUs));
        for (int i = 0; i < 10; ++i) fr.push_back(none(t + (30 + i) * kFrameUs));
        PinchRun r = run(e, fr);
        check(r.begins == 0 && r.moves == 0, "no pinch from 0 or 4 contacts");
    }

    // ---- 7. both fingers sliding the same way is a pan, not a pinch --------
    {
        printf("7. two fingers moving the same direction, gap drifting\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        // Both fingers travel right, the trailing one faster: the gap changes
        // by well over the dead band, but this is a pan and must be ignored.
        for (int i = 0; i < 40; ++i) {
            const float f = (float)i;
            fr.push_back(twoFingers(300.f + f * 3.f, 500.f + f * 8.f, 460.f,
                                    t + (uint64_t)i * kFrameUs));
        }
        fr.push_back(none(t + 40 * kFrameUs));
        PinchRun r = run(e, fr);
        check(r.begins == 0, "no Begin: the fingers are not moving against each other");
    }

    // ---- 8. three-finger tap --------------------------------------------
    {
        printf("8. three-finger TAP (short, still)\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 5; ++i)  // ~100 ms
            fr.push_back(manyFingers(3, 100.f, 700.f, 460.f,
                                     t + (uint64_t)i * kFrameUs));
        fr.push_back(none(t + 5 * kFrameUs));

        GestureEvent got;
        for (size_t i = 0; i < fr.size(); ++i) {
            e.feed(fr[i]);
            e.takeDrag();
            e.takePinch();
            const GestureEvent g = e.poll();
            if (g) got = g;
        }
        check(got.kind == GestureKind::Tap && got.fingers == 3,
              "a short still touch reports a 3-finger tap");
    }

    // ---- 9. a long hold is not a tap -------------------------------------
    {
        printf("9. three fingers held for 600 ms\n");
        GestureEngine e;
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 30; ++i)
            fr.push_back(manyFingers(3, 100.f, 700.f, 460.f,
                                     t + (uint64_t)i * kFrameUs));
        fr.push_back(none(t + 30 * kFrameUs));

        GestureEvent got;
        for (size_t i = 0; i < fr.size(); ++i) {
            e.feed(fr[i]);
            e.takeDrag();
            e.takePinch();
            const GestureEvent g = e.poll();
            if (g) got = g;
        }
        check(!got, "holding longer than tapMaxMs is not a tap");
    }

    // ---- 10. a swipe is a swipe, not a tap, and carries the edge flag ----
    {
        printf("10. three-finger swipe from the left edge\n");
        GestureEngine e;
        GestureConfig cfg;
        cfg.rangeX      = 1500;
        cfg.edgeZone    = 0.12f;   // edge = x < 180
        cfg.edgeEnabled = true;
        e.setConfig(cfg);

        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 20; ++i)
            fr.push_back(manyFingers(3, 40.f, 60.f + (float)i * 35.f, 460.f,
                                     t + (uint64_t)i * kFrameUs));
        fr.push_back(none(t + 20 * kFrameUs));

        GestureEvent got;
        int edge = 0;
        for (size_t i = 0; i < fr.size(); ++i) {
            e.feed(fr[i]);
            e.takeDrag();
            e.takePinch();
            const GestureEvent g = e.poll();
            if (g) {
                got  = g;
                edge = g.edge;
            }
        }
        check(got.kind == GestureKind::Swipe && got.fingers == 3 &&
                  got.direction == dir::Right,
              "the swipe fires as a 3-finger right swipe");
        check(edge == 1, "the event reports the left edge");
        check(got.kind != GestureKind::Tap,
              "the same movement is not reported as a tap");
    }

    // ---- 11. a horizontal pinch moves the width only ---------------------
    {
        printf("11. pinch along X (two fingers level with each other)\n");
        GestureEngine e;
        GestureConfig cfg;
        cfg.pinchAxis = PinchAxis::Auto;
        e.setConfig(cfg);
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 31; ++i) {
            fr.push_back(twoFingersVec(400.f + (float)i * 10.f, 0.f, 750.f,
                                       460.f, t));
            t += kFrameUs;
        }
        fr.push_back(none(t));
        PinchRun r = run(e, fr);
        printf("       scaleX=%.4f scaleY=%.4f\n", r.scaleX, r.scaleY);
        check(r.begins == 1, "Begin");
        check(std::fabs(r.scaleX - 1.75) < 0.005, "width follows the gap");
        check(std::fabs(r.scaleY - 1.0) < 0.001, "height untouched");
    }

    // ---- 12. a vertical pinch moves the height only ----------------------
    {
        printf("12. pinch along Y (two fingers stacked)\n");
        GestureEngine e;
        GestureConfig cfg;
        cfg.pinchAxis = PinchAxis::Auto;
        e.setConfig(cfg);
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 31; ++i) {
            fr.push_back(twoFingersVec(0.f, 400.f + (float)i * 10.f, 750.f,
                                       460.f, t));
            t += kFrameUs;
        }
        fr.push_back(none(t));
        PinchRun r = run(e, fr);
        printf("       scaleX=%.4f scaleY=%.4f\n", r.scaleX, r.scaleY);
        check(r.begins == 1, "Begin");
        check(std::fabs(r.scaleY - 1.75) < 0.005, "height follows the gap");
        check(std::fabs(r.scaleX - 1.0) < 0.001, "width untouched");
    }

    // ---- 13. a 45 degree pinch is uniform again --------------------------
    {
        printf("13. pinch at 45 degrees\n");
        GestureEngine e;
        GestureConfig cfg;
        cfg.pinchAxis = PinchAxis::Auto;
        e.setConfig(cfg);
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 31; ++i) {
            const float d = 400.f + (float)i * 10.f;
            fr.push_back(twoFingersVec(d * 0.7071f, d * 0.7071f, 750.f, 460.f, t));
            t += kFrameUs;
        }
        fr.push_back(none(t));
        PinchRun r = run(e, fr);
        printf("       scaleX=%.4f scaleY=%.4f\n", r.scaleX, r.scaleY);
        check(r.begins == 1, "Begin");
        check(std::fabs(r.scaleX - r.scaleY) < 0.02, "both axes move by the same amount");
        check(std::fabs(r.scaleX - 1.75) < 0.03, "and by the full finger ratio");
    }

    // ---- 14. Uniform mode ignores the finger angle ----------------------
    {
        printf("14. uniform mode, horizontal pinch\n");
        GestureEngine e;
        GestureConfig cfg;
        cfg.pinchAxis = PinchAxis::Uniform;
        e.setConfig(cfg);
        uint64_t t = 1000000;
        std::vector<ContactFrame> fr;
        for (int i = 0; i < 31; ++i) {
            fr.push_back(twoFingersVec(400.f + (float)i * 10.f, 0.f, 750.f,
                                       460.f, t));
            t += kFrameUs;
        }
        fr.push_back(none(t));
        PinchRun r = run(e, fr);
        printf("       scaleX=%.4f scaleY=%.4f\n", r.scaleX, r.scaleY);
        check(std::fabs(r.scaleX - r.scaleY) < 0.001, "aspect ratio preserved");
        check(std::fabs(r.scaleX - 1.75) < 0.005, "both axes take the full ratio");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail,
           g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
