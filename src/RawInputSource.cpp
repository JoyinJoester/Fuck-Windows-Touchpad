// RawInputSource.cpp
#include "RawInputSource.h"

#include <vector>

namespace {
constexpr unsigned short UP_DIGITIZER = 0x0D;
constexpr unsigned short U_TOUCHPAD   = 0x05;
const wchar_t* kClass        = L"TpGesturesRawSink";
}  // namespace

LRESULT CALLBACK RawInputSource::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        RawInputSource* self =
            reinterpret_cast<RawInputSource*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) self->handleRawInput(reinterpret_cast<HRAWINPUT>(lp));
        return 0;
    }
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

RawInputSource::~RawInputSource() { shutdown(); }

bool RawInputSource::initialize() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &RawInputSource::WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // Must be a real top-level window; message-only windows don't get raw input.
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = UP_DIGITIZER;
    rid.usUsage     = U_TOUCHPAD;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd_;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void RawInputSource::shutdown() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    cb_ = nullptr;
}

void RawInputSource::handleRawInput(HRAWINPUT hRaw) {
    UINT size = 0;
    if (GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
        return;
    if (size == 0) return;

    std::vector<uint8_t> buf(size);
    if (GetRawInputData(hRaw, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
        return;

    auto* raw = reinterpret_cast<PRAWINPUT>(buf.data());
    if (raw->header.dwType != RIM_TYPEHID) return;

    const RAWHID& hid = raw->data.hid;
    for (DWORD i = 0; i < hid.dwCount; ++i) {
        const uint8_t* rep = hid.bRawData + i * hid.dwSizeHid;
        if (cb_) cb_(rep, hid.dwSizeHid);
    }
}
