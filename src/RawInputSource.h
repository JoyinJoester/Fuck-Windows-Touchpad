// RawInputSource.h — raw HID touchpad input on a hidden window
#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>

class RawInputSource {
public:
    // Called once per HID report (report id included in the buffer).
    using ReportCallback = std::function<void(const uint8_t* report, uint32_t len)>;

    ~RawInputSource();

    // Creates a hidden top-level window and registers for touchpad raw input.
    // RIDEV_INPUTSINK lets us receive reports while unfocused.
    bool initialize();
    void shutdown();

    HWND hwnd() const { return hwnd_; }
    void setCallback(ReportCallback cb) { cb_ = std::move(cb); }

    void handleRawInput(HRAWINPUT hRaw);

private:
    HWND             hwnd_ = nullptr;
    ReportCallback   cb_;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
};
