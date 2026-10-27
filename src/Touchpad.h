// Touchpad.h — touchpad capability detection + HID report decoding
#pragma once

#include <windows.h>
#include <hidusage.h>  // USAGE (hidpi.h depends on it)
#include <hidpi.h>     // PHIDP_PREPARSED_DATA
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------- data model

struct Contact {
    uint8_t  id        = 0;
    uint16_t x         = 0;      // in device units (0 .. logicalMaxX)
    uint16_t y         = 0;      // in device units (0 .. logicalMaxY)
    bool     tip       = false;  // finger down
    bool     confident = false;  // confidence bit from hardware
};

struct ContactFrame {
    uint64_t timestampUs  = 0;
    uint8_t  contactCount = 0;   // reported by the device
    std::vector<Contact> contacts;

    float centroidX() const;
    float centroidY() const;
};

// ------------------------------------------------------------ capabilities

struct TouchpadCaps {
    std::wstring path;
    std::wstring product;
    std::wstring manufacturer;
    uint16_t vid = 0;
    uint16_t pid = 0;

    bool    isPrecisionTouchpad = false;
    uint8_t maxContacts         = 0;   // Contact Count Maximum
    uint16_t logicalMaxX        = 0;
    uint16_t logicalMaxY        = 0;
    uint16_t reportLength       = 0;   // InputReportByteLength

    // Does this hardware physically report 3+ simultaneous contacts?
    bool supportsMultiTouch() const { return maxContacts >= 3; }
    // "PTP / multi-touch" | "legacy single-touch"
    const char* capabilityLabel() const;
};

// Enumerate every HID collection that looks like a touchpad.
std::vector<TouchpadCaps> enumerateTouchpads();

// --------------------------------------------------------------- decoding

// Report layout strategies. Vendors differ wildly; we probe and self-check.
enum class LayoutKind {
    Unknown = 0,
    PackedContacts,   // N contacts x fixed stride, count byte at the end
    ArrayContacts,    // single HID array: ReportCount = maxContacts
};

struct HidLayout {
    LayoutKind kind          = LayoutKind::Unknown;
    uint8_t    reportId      = 0;
    uint8_t    stride        = 0;   // bytes per contact (PackedContacts)
    uint8_t    countOffset   = 0;   // offset of the contact-count byte
    uint16_t   minLength     = 0;

    bool decode(const uint8_t* report, uint32_t len, uint8_t maxContacts,
                ContactFrame& out) const;
};

class Touchpad {
public:
    Touchpad() = default;
    ~Touchpad();
    Touchpad(const Touchpad&) = delete;
    Touchpad& operator=(const Touchpad&) = delete;

    bool open(const TouchpadCaps& caps);
    void close();

    const TouchpadCaps& caps() const { return caps_; }
    const HidLayout&    layout() const { return layout_; }

    // Probe candidate layouts against real captured reports.
    // Returns the layout whose self-checks passed on every sample.
    bool probeLayout(const std::vector<std::vector<uint8_t>>& samples);

    // Derive a packed-contacts layout from the report length alone.
    // Used at startup before any real frame has been captured; every decode
    // still self-checks the contact-count field, so a wrong guess fails loudly
    // instead of producing garbage coordinates.
    bool assumePackedLayout();

    bool decode(const uint8_t* report, uint32_t len, ContactFrame& out) const;

private:
    bool decodeWith(const HidLayout& layout, const uint8_t* report,
                    uint32_t len, ContactFrame& out) const;

    TouchpadCaps   caps_;
    HidLayout      layout_;
    HANDLE         hDev_ = INVALID_HANDLE_VALUE;
    PHIDP_PREPARSED_DATA pp_ = nullptr;
};

// Milliseconds via QueryPerformanceCounter, for gesture timing.
uint64_t nowMicros();
