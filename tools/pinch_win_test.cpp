// pinch_win_test.cpp -- development helper: prove that the pinch path really
// resizes a window and that a matching counter-pinch puts it back.
//
// Not part of the shipped program.
//
//   g++ -std=c++17 -O2 pinch_win_test.cpp ../src/WindowManager.cpp ^
//       -I../src -o pinch_win_test.exe -luser32 -ldwmapi ^
//       -municode
//   pinch_win_test.exe
//
// The target window lives in a child process on purpose: WindowManager refuses
// to grab windows owned by its own process (the control panel must never resize
// itself), so an in-process window would never be picked.
//
//   pinch_win_test.exe --target    (internal) opens the target and idles

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../src/WindowManager.h"

static const wchar_t* kClass = L"PinchHarnessTarget";
static const wchar_t* kTitle = L"pinch harness target";

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void pump(int ms) {
    const DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(8);
    }
}

// ---- drag clamp helper ----------------------------------------------------
struct StripProbe {
    RECT strip{};
    int  w = 0;
    int  h = 0;
};

static BOOL CALLBACK stripProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* p = reinterpret_cast<StripProbe*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    RECT x{};
    if (!IntersectRect(&x, &p->strip, &mi.rcMonitor)) return TRUE;
    if (x.right - x.left > p->w) {
        p->w = x.right - x.left;
        p->h = x.bottom - x.top;
    }
    return TRUE;
}

// Widest intersection of the window's caption with any single monitor.
static void captionExtent(HWND win, int* outW, int* outH) {
    RECT r{};
    GetWindowRect(win, &r);
    StripProbe p;
    p.strip = {r.left, r.top, r.right, r.top + 28};
    EnumDisplayMonitors(nullptr, nullptr, stripProc,
                        reinterpret_cast<LPARAM>(&p));
    *outW = p.w;
    *outH = p.h;
}

static int runTarget() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND h = CreateWindowExW(0, kClass, kTitle, WS_OVERLAPPEDWINDOW, 200, 200,
                             800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) {
        printf("target: CreateWindow failed err=%lu\n", GetLastError());
        return 1;
    }
    ShowWindow(h, SW_SHOW);
    SetWindowPos(h, HWND_TOPMOST, 200, 200, 800, 600, SWP_SHOWWINDOW);
    printf("target ready %p\n", (void*)h);

    // Idle until the tester closes it, with a hard stop so a crashed tester
    // cannot leave a window behind forever.
    const DWORD deadline = GetTickCount() + 30000;
    while (GetTickCount() < deadline && IsWindow(h)) pump(16);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && wcscmp(argv[1], L"--target") == 0) return runTarget();

    // ---- tester ------------------------------------------------------------
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --target", self);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        printf("CreateProcess failed err=%lu\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    HWND target = nullptr;
    for (int i = 0; i < 60 && !target; ++i) {
        target = FindWindowW(kClass, kTitle);
        if (!target) pump(50);
    }
    if (!target) {
        printf("[FAIL] target window never appeared\n");
        return 1;
    }
    pump(300);

    RECT tr{};
    GetWindowRect(target, &tr);
    SetForegroundWindow(target);
    SetCursorPos((tr.left + tr.right) / 2, (tr.top + tr.bottom) / 2);
    pump(200);

    {
        POINT p{};
        GetCursorPos(&p);
        HWND under = GetAncestor(WindowFromPoint(p), GA_ROOT);
        if (under != target) {
            printf("[FAIL] the target is not the window under the cursor\n");
            return 1;
        }
    }

    auto size = [&](int* w, int* h) {
        RECT r{};
        GetWindowRect(target, &r);
        *w = r.right - r.left;
        *h = r.bottom - r.top;
    };

    // ---- drag clamp only ---------------------------------------------------
    // Pushing far past the edge has to leave the caption reachable. A window
    // that can be shoved completely off screen cannot be grabbed again, which
    // is the whole reason the clamp exists.
    if (argc > 1 && wcscmp(argv[1], L"--drag") == 0) {
        wm::setTouchpadRange(1500, 927);
        std::string m;
        RECT start{};
        GetWindowRect(target, &start);
        printf("start            %ld,%ld\n", start.left, start.top);

        if (!wm::dragBegin(&m)) {
            printf("[FAIL] dragBegin: %s\n", m.c_str());
            PostMessageW(target, WM_CLOSE, 0, 0);
            return 1;
        }

        int fails = 0;
        auto report = [&](const char* what) {
            RECT r{};
            GetWindowRect(target, &r);
            int cw = 0, ch = 0;
            captionExtent(target, &cw, &ch);
            printf("%-16s at %ld,%ld caption visible %dx%d\n", what, r.left,
                   r.top, cw, ch);
            if (cw < 100 || ch < 8) ++fails;
        };

        for (int i = 0; i < 40; ++i) { wm::dragMove(400.f, 0.f); pump(4); }
        report("push right");
        for (int i = 0; i < 80; ++i) { wm::dragMove(-400.f, 0.f); pump(4); }
        report("push left");
        for (int i = 0; i < 80; ++i) { wm::dragMove(0.f, -400.f); pump(4); }
        report("push up");
        for (int i = 0; i < 160; ++i) { wm::dragMove(0.f, 400.f); pump(4); }
        report("push down");

        // Still able to follow small movements afterwards (the clamp must not
        // latch the window in place).
        RECT before{};
        GetWindowRect(target, &before);
        for (int i = 0; i < 10; ++i) { wm::dragMove(-60.f, -60.f); pump(4); }
        RECT after{};
        GetWindowRect(target, &after);
        printf("small move       %ld,%ld -> %ld,%ld\n", before.left, before.top,
               after.left, after.top);
        if (after.left >= before.left || after.top >= before.top) {
            printf("[FAIL] the window stopped following the gesture\n");
            ++fails;
        }

        // Put it back where it started, to leave the desktop as we found it.
        const int backX = (int)((start.left - after.left) / 1.28f);
        const int backY = (int)((start.top - after.top) / 1.28f);
        for (int i = 0; i < 60; ++i) {
            wm::dragMove((float)backX / 60.f, (float)backY / 60.f);
            pump(4);
        }
        wm::dragEnd();

        printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails,
               fails == 1 ? "" : "s");
        PostMessageW(target, WM_CLOSE, 0, 0);
        return fails ? 1 : 0;
    }

    int w0 = 0, h0 = 0;
    size(&w0, &h0);
    printf("start            %dx%d\n", w0, h0);

    std::string msg;
    if (!wm::pinchBegin(&msg)) {
        printf("[FAIL] pinchBegin: %s\n", msg.c_str());
        PostMessageW(target, WM_CLOSE, 0, 0);
        return 1;
    }
    printf("pinchBegin       %s\n", msg.c_str());

    // Shrink by ~15% in three steps, then grow back by the exact inverse.
    for (int i = 0; i < 3; ++i) {
        wm::pinchMove(0.947f, 0.947f);
        pump(20);
    }
    int w1 = 0, h1 = 0;
    size(&w1, &h1);
    printf("after pinch in   %dx%d (%.1f%% of start)\n", w1, h1,
           100.0 * (double)w1 / (double)w0);

    for (int i = 0; i < 3; ++i) {
        wm::pinchMove(1.f / 0.947f, 1.f / 0.947f);
        pump(20);
    }
    int w2 = 0, h2 = 0;
    size(&w2, &h2);
    printf("after pinch out  %dx%d\n", w2, h2);
    wm::pinchEnd();

    // Clamps: a single frame cannot halve the window, and the work area is a
    // hard ceiling.
    wm::pinchBegin(&msg);
    for (int i = 0; i < 40; ++i) {
        wm::pinchMove(0.6f, 0.6f);
        pump(4);
    }
    int w3 = 0, h3 = 0;
    size(&w3, &h3);
    printf("min clamp        %dx%d\n", w3, h3);
    wm::pinchEnd();

    wm::pinchBegin(&msg);
    for (int i = 0; i < 40; ++i) {
        wm::pinchMove(1.8f, 1.8f);
        pump(4);
    }
    int w4 = 0, h4 = 0;
    size(&w4, &h4);
    printf("max clamp        %dx%d\n", w4, h4);
    wm::pinchEnd();

    int fails = 0;
    if (!(w1 < w0 - 50)) {
        printf("[FAIL] pinch in did not shrink the window\n");
        ++fails;
    } else {
        printf("[ ok ] pinch in shrinks the window\n");
    }
    if (!(w2 == w0 || (w2 - w0 < 4 && w0 - w2 < 4))) {
        printf("[FAIL] round trip did not restore: %d vs %d\n", w2, w0);
        ++fails;
    } else {
        printf("[ ok ] round trip restores the original size\n");
    }
    if (!(w3 >= 320 && h3 >= 240)) {
        printf("[FAIL] min clamp violated: %dx%d\n", w3, h3);
        ++fails;
    } else {
        printf("[ ok ] min size honoured (%d >= 320)\n", w3);
    }
    {
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int maxW = wa.right - wa.left;
        if (!(w4 <= maxW + 2)) {
            printf("[FAIL] max clamp exceeded work area: %d > %d\n", w4, maxW);
            ++fails;
        } else {
            printf("[ ok ] never wider than the work area (%d <= %d)\n", w4,
                   maxW);
        }
    }

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    PostMessageW(target, WM_CLOSE, 0, 0);
    pump(200);
    return fails ? 1 : 0;
}
