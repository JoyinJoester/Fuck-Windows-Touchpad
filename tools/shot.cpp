// shot.cpp — development helper: capture the app window to a PNG.
//
// Not part of the shipped program; never linked into Fuck-Windows-Touchpad.exe.
//
//   g++ -municode shot.cpp -o shot.exe -lgdiplus -lgdi32 -luser32
//
//   shot.exe out.png              capture the main window
//   shot.exe out.png <x> <y>      click at client x,y first (to switch pages)
//   shot.exe --popup out.png      capture the dropdown popup
//   shot.exe --list               list top-level windows

#include <windows.h>
#include <gdiplus.h>

#include <cstdio>

using namespace Gdiplus;

struct FindCtx {
    const wchar_t* want;
    HWND           out;
};

static BOOL CALLBACK findProc(HWND w, LPARAM p) {
    auto* c = reinterpret_cast<FindCtx*>(p);
    wchar_t buf[128] = {};
    GetClassNameW(w, buf, 128);
    if (wcscmp(buf, c->want) == 0 && IsWindowVisible(w)) {
        c->out = w;
        return FALSE;
    }
    return TRUE;
}

static HWND findByClass(const wchar_t* cls) {
    FindCtx c = {cls, nullptr};
    EnumWindows(&findProc, reinterpret_cast<LPARAM>(&c));
    return c.out;
}

static void put(const wchar_t* s) {
    if (!s || !*s) return;
    char buf[1024];
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, 1023, nullptr,
                                      nullptr);
    if (n > 0) {
        buf[n - 1] = 0;
        fputs(buf, stdout);
    }
}

static BOOL CALLBACK listProc(HWND w, LPARAM) {
    if (!IsWindowVisible(w)) return TRUE;
    wchar_t cls[128] = {};
    wchar_t title[256] = {};
    GetClassNameW(w, cls, 128);
    GetWindowTextW(w, title, 256);
    if (!title[0]) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    printf("%08llX  pid=%-6lu class=",
           (unsigned long long)(unsigned long)(size_t)w, pid);
    put(cls);
    printf("  title=");
    put(title);
    printf("\n");
    return TRUE;
}

static int pngEncoder(CLSID* out) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (!size) return -1;
    auto* codecs = static_cast<ImageCodecInfo*>(malloc(size));
    if (!codecs) return -1;
    GetImageEncoders(n, size, codecs);
    int found = -1;
    for (UINT i = 0; i < n; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *out = codecs[i].Clsid;
            found = 0;
            break;
        }
    }
    free(codecs);
    return found;
}

static int capture(HWND h, const wchar_t* path) {
    RECT rc;
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left;
    const int ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return 2;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateCompatibleBitmap(screen, w, ht);
    HGDIOBJ old = SelectObject(mem, dib);
    // PrintWindow renders the window itself, so an occluded window still comes
    // out correct. Screen BitBlt is only a fallback.
    if (!PrintWindow(h, mem, 2 /* PW_RENDERFULLCONTENT */))
        BitBlt(mem, 0, 0, w, ht, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    CLSID clsid;
    int rc2 = 3;
    if (pngEncoder(&clsid) == 0) {
        Bitmap image(dib, nullptr);
        rc2 = image.Save(path, &clsid, nullptr) == Ok ? 0 : 4;
    }
    DeleteObject(dib);
    printf("saved (%dx%d) rc=%d\n", w, ht, rc2);
    return rc2;
}

static void clickClient(HWND h, int x, int y) {
    SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    Sleep(320);
}

static void wheel(HWND h, int delta, int times) {
    for (int i = 0; i < times; ++i) {
        SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)delta), 0);
        Sleep(70);
    }
    Sleep(220);
}

// GetDpiForWindow is Win10 1607+ and not in every import library, so bind it at
// runtime and fall back to a client-width estimate.
static double dpiScale(HWND h) {
    typedef UINT(WINAPI * Fn)(HWND);
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = (Fn)(void*)GetProcAddress(u32, "GetDpiForWindow");
        if (fn) {
            const double s = (double)fn(h) / 96.0;
            if (s > 0.0) return s;
        }
    }
    RECT cr;
    GetClientRect(h, &cr);
    return cr.right > 0 ? (double)cr.right / 1020.0 : 1.0;
}

// Four pages in one process run. Driving the window by hand between calls is
// racy (the owner may close it), so this captures the whole set at once.
static int tour(HWND h, const wchar_t* prefix) {
    const double sc = dpiScale(h);
    const wchar_t* names[4] = {L"gestures", L"window", L"advanced", L"about"};
    int rc = 0;
    for (int page = 0; page < 4; ++page) {
        // Sidebar nav rows are 34px tall on a 40px pitch starting at y=78.
        const int ny = (int)((78 + 40 * page + 17) * sc);
        const int nx = (int)(100 * sc);
        clickClient(h, nx, ny);
        Sleep(180);
        wchar_t path[512];
        _snwprintf(path, 511, L"%s-%d-%s%s.png", prefix, page, names[page], L"");
        rc |= capture(h, path);
        if (page == 2) {
            // The advanced page scrolls; keep a second frame from further down.
            wheel(h, -120, 5);
            _snwprintf(path, 511, L"%s-%d-%s-b.png", prefix, page, names[page]);
            rc |= capture(h, path);
        }
    }
    return rc;
}

// Open the appearance dropdown, photograph it, pick "dark", photograph the
// result. Exercises the only popup + palette-swap path in the window.
// Coordinates are window-relative, i.e. read straight off a capture.
static int themeTest(HWND h, const wchar_t* prefix, int rowX, int rowY) {
    const double sc = dpiScale(h);
    // Get to a known state first: advanced page, scrolled to the bottom.
    clickClient(h, (int)(100 * sc), (int)((78 + 40 * 2 + 17) * sc));
    wheel(h, -120, 5);
    POINT c0 = {0, 0};
    if (!ClientToScreen(h, &c0)) return 3;
    RECT wr{};
    if (!GetWindowRect(h, &wr)) return 3;
    const int ox = c0.x - wr.left;
    const int oy = c0.y - wr.top;
    const int cx = rowX - ox;
    const int cy = rowY - oy;
    printf("click client %d,%d (window %d,%d, origin %d,%d)\n", cx, cy, rowX,
           rowY, ox, oy);

    wchar_t path[512];

    clickClient(h, cx, cy);
    Sleep(400);
    HWND d = findByClass(L"TpGesturesDropDown");
    if (!d || !IsWindowVisible(d)) {
        printf("popup not found\n");
        return 2;
    }
    _snwprintf(path, 511, L"%s-popup.png", prefix);
    capture(d, path);

    // Rows are 30 logical px tall inside a 6px inset; index 2 is "deep dark".
    clickClient(d, (int)(60 * sc), (int)((6 + 2 * 30 + 15) * sc));
    Sleep(500);
    _snwprintf(path, 511, L"%s-dark.png", prefix);
    capture(h, path);

    // And back to following the system, so the run leaves no side effect.
    clickClient(h, cx, cy);
    Sleep(350);
    d = findByClass(L"TpGesturesDropDown");
    if (d && IsWindowVisible(d)) clickClient(d, (int)(60 * sc), (int)((6 + 15) * sc));
    Sleep(450);
    _snwprintf(path, 511, L"%s-system.png", prefix);
    capture(h, path);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    // Without this the process is DPI-unaware, GetWindowRect returns virtualised
    // coordinates and PrintWindow renders a cropped corner.
    SetProcessDPIAware();

    if (argc > 1 && wcscmp(argv[1], L"--list") == 0) {
        EnumWindows(&listProc, 0);
        return 0;
    }

    GdiplusStartupInput gi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gi, nullptr);

    if (argc > 2 && wcscmp(argv[1], L"--tour") == 0) {
        HWND th = findByClass(L"TpGesturesMainWindow");
        if (!th) {
            printf("window not found\n");
            GdiplusShutdown(token);
            return 1;
        }
        const int trc = tour(th, argv[2]);
        GdiplusShutdown(token);
        return trc;
    }

    if (argc > 4 && wcscmp(argv[1], L"--themetest") == 0) {
        HWND th = findByClass(L"TpGesturesMainWindow");
        if (!th) {
            printf("window not found\n");
            GdiplusShutdown(token);
            return 1;
        }
        const int rc2 = themeTest(th, argv[2], _wtoi(argv[3]), _wtoi(argv[4]));
        GdiplusShutdown(token);
        return rc2;
    }

    if (argc > 4 && wcscmp(argv[1], L"--resize") == 0) {
        // Shrink/grow the main window, let the layout settle, photograph it.
        // This is how the elastic content column gets verified without hands.
        HWND m = findByClass(L"TpGesturesMainWindow");
        if (!m) {
            printf("window not found\n");
            GdiplusShutdown(token);
            return 1;
        }
        const int wpx = _wtoi(argv[2]);
        const int hpx = _wtoi(argv[3]);
        RECT wr;
        GetWindowRect(m, &wr);
        SetWindowPos(m, nullptr, wr.left, wr.top, wpx, hpx,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(500);
        RECT after;
        GetWindowRect(m, &after);
        printf("resized to %ldx%ld\n", after.right - after.left,
               after.bottom - after.top);
        const int rc2 = capture(m, argv[4]);
        GdiplusShutdown(token);
        return rc2;
    }

    if (argc > 2 && wcscmp(argv[1], L"--key") == 0) {
        // Posts one key press to the settings window, which is how the custom
        // shortcut recorder is driven without touching the keyboard.
        HWND m = findByClass(L"TpGesturesMainWindow");
        if (!m) {
            printf("window not found\n");
            GdiplusShutdown(token);
            return 1;
        }
        const UINT vk = (UINT)_wtoi(argv[2]);
        PostMessageW(m, WM_KEYDOWN, vk, 0);
        PostMessageW(m, WM_KEYUP, vk, 0);
        Sleep(350);
        printf("posted vk %u\n", vk);
        GdiplusShutdown(token);
        return 0;
    }

    const bool popup = (argc > 2 && wcscmp(argv[1], L"--popup") == 0);
    const wchar_t* path = popup ? argv[2] : (argc > 1 ? argv[1] : L"shot.png");

    HWND h = findByClass(popup ? L"TpGesturesDropDown"
                               : L"TpGesturesMainWindow");
    if (!h) {
        printf("window not found\n");
        GdiplusShutdown(token);
        return 1;
    }

    if (!popup) {
        // Optional click before capturing, so other pages can be verified
        // without driving the mouse by hand.
        if (argc >= 4) {
            const int cx = _wtoi(argv[2]);
            const int cy = _wtoi(argv[3]);
            SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
            SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
            SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
            Sleep(350);
        }
        // Optional wheel: 4th argument, in wheel-delta units (120 = one notch,
        // negative scrolls the page down).
        if (argc >= 5) {
            const int delta = _wtoi(argv[4]);
            for (int i = 0; i < 3; ++i) {
                SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)delta), 0);
                Sleep(60);
            }
            Sleep(200);
        }
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Sleep(250);
    } else if (argc >= 5) {
        // Pick an entry in the open dropdown, then let the caller capture the
        // main window to confirm the cell updated.
        const int cx = _wtoi(argv[3]);
        const int cy = _wtoi(argv[4]);
        SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
        Sleep(300);
        printf("clicked popup at %d,%d\n", cx, cy);
        GdiplusShutdown(token);
        return 0;
    }

    const int rc = capture(h, path);
    GdiplusShutdown(token);
    return rc;
}
