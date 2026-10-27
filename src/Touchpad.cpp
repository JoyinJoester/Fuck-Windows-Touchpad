// Touchpad.cpp
#include "Touchpad.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>

// ---- HID usage constants -------------------------------------------------
namespace {
constexpr USAGE UP_GENERIC_DESKTOP = 0x01;
constexpr USAGE UP_DIGITIZER       = 0x0D;

constexpr USAGE U_TOUCHPAD          = 0x05;
constexpr USAGE U_X                 = 0x30;
constexpr USAGE U_Y                 = 0x31;
constexpr USAGE U_CONTACT_ID        = 0x51;
constexpr USAGE U_CONTACT_COUNT     = 0x54;
constexpr USAGE U_CONTACT_COUNT_MAX = 0x55;

USAGE capUsage(const HIDP_VALUE_CAPS& vc) {
    return vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
}

// ---------------------------------------------------------------- timing
uint64_t nowMicrosImpl() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart * 1000000LL / freq.QuadPart);
}
}  // namespace

uint64_t nowMicros() { return nowMicrosImpl(); }

// ------------------------------------------------------------ ContactFrame
float ContactFrame::centroidX() const {
    if (contacts.empty()) return 0.f;
    float s = 0.f;
    for (const auto& c : contacts) s += c.x;
    return s / static_cast<float>(contacts.size());
}

float ContactFrame::centroidY() const {
    if (contacts.empty()) return 0.f;
    float s = 0.f;
    for (const auto& c : contacts) s += c.y;
    return s / static_cast<float>(contacts.size());
}

// --------------------------------------------------------- capabilities
const char* TouchpadCaps::capabilityLabel() const {
    if (isPrecisionTouchpad && maxContacts >= 4) return "precision touchpad (4+ fingers)";
    if (isPrecisionTouchpad && maxContacts >= 3) return "precision touchpad (3 fingers)";
    if (isPrecisionTouchpad && maxContacts >= 2) return "precision touchpad (2 fingers)";
    if (isPrecisionTouchpad)                     return "precision touchpad (single contact)";
    return "legacy single-touch";
}

// ---------------------------------------------------------------- enumerate
std::vector<TouchpadCaps> enumerateTouchpads() {
    std::vector<TouchpadCaps> result;

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return result;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &need, nullptr);
        if (!need) continue;

        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, need,
                                              nullptr, nullptr))
            continue;

        // PTP collections usually deny write (and sometimes read) access.
        HANDLE h = CreateFileW(detail->DevicePath, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) continue;

        PHIDP_PREPARSED_DATA pp = nullptr;
        if (!HidD_GetPreparsedData(h, &pp) || !pp) {
            CloseHandle(h);
            continue;
        }

        HIDP_CAPS caps{};
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
            const bool looksTouchpad =
                (caps.UsagePage == UP_DIGITIZER && caps.Usage == U_TOUCHPAD);

            if (looksTouchpad) {
                TouchpadCaps tc;
                tc.path         = detail->DevicePath;
                tc.reportLength = caps.InputReportByteLength;

                HIDD_ATTRIBUTES attr{};
                attr.Size = sizeof(attr);
                if (HidD_GetAttributes(h, &attr)) {
                    tc.vid = attr.VendorID;
                    tc.pid = attr.ProductID;
                }
                wchar_t wb[512]{};
                if (HidD_GetProductString(h, wb, sizeof(wb))) tc.product = wb;
                wchar_t mb[512]{};
                if (HidD_GetManufacturerString(h, mb, sizeof(mb))) tc.manufacturer = mb;

                // input value caps
                std::vector<HIDP_VALUE_CAPS> vcaps;
                if (caps.NumberInputValueCaps) {
                    USHORT n = caps.NumberInputValueCaps;
                    vcaps.resize(n);
                    if (HidP_GetValueCaps(HidP_Input, vcaps.data(), &n, pp) ==
                        HIDP_STATUS_SUCCESS)
                        vcaps.resize(n);
                    else
                        vcaps.clear();
                }
                bool hasContactId = false;
                uint32_t contactIdCount = 0;
                for (const auto& vc : vcaps) {
                    if (vc.UsagePage != UP_DIGITIZER) {
                        if (vc.UsagePage == UP_GENERIC_DESKTOP) {
                            const USAGE u = capUsage(vc);
                            if (u == U_X) tc.logicalMaxX = static_cast<uint16_t>(vc.LogicalMax);
                            if (u == U_Y) tc.logicalMaxY = static_cast<uint16_t>(vc.LogicalMax);
                        }
                        continue;
                    }
                    const USAGE u = capUsage(vc);
                    if (u == U_CONTACT_ID) {
                        hasContactId = true;
                        contactIdCount = vc.ReportCount;
                    }
                }

                // Contact Count Maximum lives in a FEATURE report for PTP.
                uint32_t contactMax = 0;
                if (caps.NumberFeatureValueCaps) {
                    USHORT n = caps.NumberFeatureValueCaps;
                    std::vector<HIDP_VALUE_CAPS> fcaps(n);
                    if (HidP_GetValueCaps(HidP_Feature, fcaps.data(), &n, pp) ==
                        HIDP_STATUS_SUCCESS) {
                        fcaps.resize(n);
                        for (const auto& vc : fcaps) {
                            if (vc.UsagePage == UP_DIGITIZER &&
                                capUsage(vc) == U_CONTACT_COUNT_MAX) {
                                contactMax = static_cast<uint32_t>(vc.LogicalMax);
                            }
                        }
                    }
                }
                if (contactMax == 0) contactMax = contactIdCount;
                if (contactMax == 0) contactMax = 1;

                tc.maxContacts          = static_cast<uint8_t>(std::min<uint32_t>(contactMax, 16));
                tc.isPrecisionTouchpad  = hasContactId;

                result.push_back(std::move(tc));
            }
        }

        HidD_FreePreparsedData(pp);
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

// ------------------------------------------------------------------ Touchpad
Touchpad::~Touchpad() { close(); }

bool Touchpad::open(const TouchpadCaps& caps) {
    close();
    caps_ = caps;

    hDev_ = CreateFileW(caps_.path.c_str(), 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDev_ == INVALID_HANDLE_VALUE) {
        hDev_ = CreateFileW(caps_.path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (hDev_ == INVALID_HANDLE_VALUE) return false;

    if (!HidD_GetPreparsedData(hDev_, &pp_) || !pp_) {
        CloseHandle(hDev_);
        hDev_ = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

void Touchpad::close() {
    if (pp_) {
        HidD_FreePreparsedData(pp_);
        pp_ = nullptr;
    }
    if (hDev_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hDev_);
        hDev_ = INVALID_HANDLE_VALUE;
    }
}

// Derive the packed layout from the report length:
//   reportLength = 1 (id) + stride * maxContacts + 1 (count) + padding
// On the reference device: 30 = 1 + 5*5 + 1 + 3  ->  stride 5, stride guess (30-2)/5 = 5
bool Touchpad::assumePackedLayout() {
    if (caps_.maxContacts == 0 || caps_.reportLength < 3) return false;

    const uint32_t usable = caps_.reportLength - 2;  // minus id byte and count byte
    uint32_t stride = usable / caps_.maxContacts;
    if (stride == 0) return false;
    if (stride > 32) stride = 32;

    HidLayout l;
    l.kind        = LayoutKind::PackedContacts;
    l.reportId    = 0;
    l.stride      = static_cast<uint8_t>(stride);
    l.countOffset = static_cast<uint8_t>(1 + stride * caps_.maxContacts);
    l.minLength   = static_cast<uint16_t>(l.countOffset + 1);

    if (caps_.reportLength && l.minLength > caps_.reportLength) return false;

    layout_ = l;
    return true;
}

// A layout is only accepted if EVERY sample decodes and passes sanity checks.
bool Touchpad::probeLayout(const std::vector<std::vector<uint8_t>>& samples) {
    if (samples.empty() || caps_.maxContacts == 0) return false;

    const uint8_t maxC = caps_.maxContacts;

    // Candidate 1: packed contacts, 5 bytes each, count byte after the block.
    HidLayout packed;
    packed.kind        = LayoutKind::PackedContacts;
    packed.reportId    = samples.front().empty() ? 0 : samples.front()[0];
    packed.stride      = 5;
    packed.countOffset = static_cast<uint8_t>(1 + packed.stride * maxC);
    packed.minLength   = static_cast<uint16_t>(packed.countOffset + 1);

    bool allOk = true;
    for (const auto& s : samples) {
        ContactFrame f;
        if (!decodeWith(packed, s.data(), static_cast<uint32_t>(s.size()), f)) {
            allOk = false;
            break;
        }
        for (const auto& c : f.contacts) {
            // tolerate 25% overshoot for vendor-specific scaling quirks
            if (caps_.logicalMaxX && c.x > caps_.logicalMaxX + caps_.logicalMaxX / 4) {
                allOk = false;
                break;
            }
            if (caps_.logicalMaxY && c.y > caps_.logicalMaxY + caps_.logicalMaxY / 4) {
                allOk = false;
                break;
            }
        }
        if (!allOk) break;
    }

    if (allOk) {
        layout_ = packed;
        return true;
    }

    layout_ = HidLayout{};
    return false;
}

bool Touchpad::decodeWith(const HidLayout& layout, const uint8_t* report,
                          uint32_t len, ContactFrame& out) const {
    if (!report || len < layout.minLength) return false;

    switch (layout.kind) {
    case LayoutKind::PackedContacts: {
        const uint8_t* p = report + 1;  // skip report id
        const uint8_t cc = p[layout.countOffset - 1];
        // Self-check: a bogus count means the layout guess is wrong.
        if (cc > caps_.maxContacts) return false;

        out.contactCount = cc;
        out.contacts.clear();
        for (uint8_t i = 0; i < caps_.maxContacts; ++i) {
            const uint8_t* c = p + i * layout.stride;
            if (!(c[0] & 0x01)) continue;  // bit0 = tip switch
            Contact ct;
            ct.id        = static_cast<uint8_t>(c[0] >> 4);
            ct.confident = (c[0] & 0x02) != 0;
            ct.tip       = true;
            ct.x         = static_cast<uint16_t>(c[1] | (c[2] << 8));
            ct.y         = static_cast<uint16_t>(c[3] | (c[4] << 8));
            out.contacts.push_back(ct);
        }
        return out.contacts.size() <= caps_.maxContacts;
    }
    case LayoutKind::ArrayContacts:
    default:
        return false;
    }
}

bool Touchpad::decode(const uint8_t* report, uint32_t len, ContactFrame& out) const {
    out.timestampUs = nowMicros();
    return decodeWith(layout_, report, len, out);
}
