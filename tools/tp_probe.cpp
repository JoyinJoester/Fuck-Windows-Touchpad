// tp_probe.cpp — Touchpad capability probe (feasibility spike)
//
// Purpose: verify on real hardware whether a third-party process can
//   (1) enumerate Precision Touchpad (PTP) HID devices,
//   (2) read max contact count / supported features,
//   (3) receive raw HID contact reports via Raw Input while unfocused,
//   (4) see individual contacts (id / x / y / tip).
//
// Build (MinGW):
//   g++ -std=c++17 -O2 tp_probe.cpp -o tp_probe.exe -lhid -lsetupapi -luser32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

// ---- HID usage constants (defined locally to avoid header drift) ----
static const USAGE UP_GENERIC_DESKTOP = 0x01;
static const USAGE UP_DIGITIZER       = 0x0D;

static const USAGE U_TOUCHPAD          = 0x05;
static const USAGE U_X                 = 0x30;
static const USAGE U_Y                 = 0x31;
static const USAGE U_TIP_SWITCH        = 0x42;
static const USAGE U_CONFIDENCE        = 0x47;
static const USAGE U_WIDTH             = 0x48;
static const USAGE U_HEIGHT            = 0x49;
static const USAGE U_CONTACT_ID        = 0x51;
static const USAGE U_CONTACT_COUNT     = 0x54;
static const USAGE U_CONTACT_COUNT_MAX = 0x55;

// ============================================================
// Per-device parsed report layout
// ============================================================
struct ValueEntry {
    USAGE  usage;
    UCHAR  reportId;
    USHORT bitOffset;    // relative to report payload (report-id byte excluded)
    USHORT bitSize;
    USHORT reportCount;
    LONG   logicalMin;
    LONG   logicalMax;
};

struct DeviceCtx {
    std::wstring path;
    std::wstring product;
    std::wstring manufacturer;
    USHORT vid = 0, pid = 0, version = 0;
    bool  isPtp = false;
    bool  hasTouchpadUsage = false;
    ULONG maxContacts = 0;      // from usage 0x55 / contact-id report count
    ULONG maxContactsSeen = 0;  // runtime observed peak
    ULONGLONG frames = 0;
    std::vector<ValueEntry> entries;
    HANDLE hDev = INVALID_HANDLE_VALUE;
    PHIDP_PREPARSED_DATA pp = nullptr;  // kept alive: raw input parsing needs it
};

static std::map<HANDLE, DeviceCtx*> g_byHandle;
static std::vector<DeviceCtx*>      g_devices;
static HWND                         g_sink = nullptr;

// ============================================================
// Helpers
// ============================================================
static ULONG ExtractBits(const BYTE* data, ULONG bitOffset, ULONG bitSize) {
    ULONG v = 0;
    for (ULONG i = 0; i < bitSize && i < 32; ++i) {
        ULONG bit = bitOffset + i;
        if (data[bit >> 3] & (1u << (bit & 7))) v |= (1u << i);
    }
    return v;
}

static void PrintSeparator(const char* title) {
    printf("\n");
    printf("============================================================\n");
    printf("  %s\n", title);
    printf("============================================================\n");
}

// ============================================================
// Open a HID device with graceful permission fallback.
// PTP devices frequently deny write access (system owns the collection),
// so try RW -> R -> query-only before giving up.
// ============================================================
static HANDLE TryOpenHid(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) return h;
    DWORD eRW = GetLastError();

    h = CreateFileW(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        printf("        [open] read-only (rw err=%lu)\n", eRW);
        return h;
    }
    DWORD eR = GetLastError();

    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        printf("        [open] query-only (rw=%lu r=%lu)\n", eRW, eR);
        return h;
    }
    printf("        [open] FAILED rw=%lu read=%lu query=%lu\n", eRW, eR, GetLastError());
    return INVALID_HANDLE_VALUE;
}

// ============================================================
// Enumerate HID devices and inspect each one
// ============================================================
static ULONG g_totalInterfaces = 0;
static ULONG g_digitizerInterfaces = 0;

static void EnumerateHid() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        printf("[!] SetupDiGetClassDevs failed: %lu\n", GetLastError());
        return;
    }

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &need, nullptr);
        if (!need) continue;

        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, need, nullptr, nullptr))
            continue;

        ++g_totalInterfaces;

        HANDLE h = TryOpenHid(detail->DevicePath);
        if (h == INVALID_HANDLE_VALUE) continue;

        PHIDP_PREPARSED_DATA pp = nullptr;
        if (!HidD_GetPreparsedData(h, &pp) || !pp) {
            printf("        [preparsed] FAILED err=%lu\n", GetLastError());
            CloseHandle(h); continue;
        }

        HIDP_CAPS caps = {};
        if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) {
            HidD_FreePreparsedData(pp); CloseHandle(h); continue;
        }

        // ---- Input value caps ----
        std::vector<HIDP_VALUE_CAPS> vcaps;
        if (caps.NumberInputValueCaps > 0) {
            USHORT vlen = caps.NumberInputValueCaps;
            vcaps.resize(vlen);
            if (HidP_GetValueCaps(HidP_Input, vcaps.data(), &vlen, pp) != HIDP_STATUS_SUCCESS)
                vcaps.clear();
            else
                vcaps.resize(vlen);
        }

        // ---- Feature value caps: PTP puts Contact Count Maximum here ----
        std::vector<HIDP_VALUE_CAPS> fcaps;
        if (caps.NumberFeatureValueCaps > 0) {
            USHORT flen = caps.NumberFeatureValueCaps;
            fcaps.resize(flen);
            if (HidP_GetValueCaps(HidP_Feature, fcaps.data(), &flen, pp) != HIDP_STATUS_SUCCESS)
                fcaps.clear();
            else
                fcaps.resize(flen);
        }

        bool hasTouchpadUsage = (caps.UsagePage == UP_DIGITIZER && caps.Usage == U_TOUCHPAD);
        bool hasContactId = false, hasContactMax = false;
        ULONG contactIdCount = 0, contactMaxVal = 0;

        for (auto& vc : vcaps) {
            USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
            if (vc.UsagePage != UP_DIGITIZER) continue;
            if (u == U_CONTACT_ID) { hasContactId = true; contactIdCount = vc.ReportCount; }
        }
        for (auto& vc : fcaps) {
            USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
            if (vc.UsagePage != UP_DIGITIZER) continue;
            if (u == U_CONTACT_COUNT_MAX) { hasContactMax = true; contactMaxVal = (ULONG)vc.LogicalMax; }
        }

        bool isPtp = hasTouchpadUsage && hasContactId;
        if (caps.UsagePage == UP_DIGITIZER) ++g_digitizerInterfaces;

        // Diagnostic: show every opened interface so we can see what is present
        printf("    [%02lu] up=0x%02X u=0x%02X inLen=%u vcaps=%zu%s\n",
               g_totalInterfaces, caps.UsagePage, caps.Usage,
               caps.InputReportByteLength, vcaps.size(),
               isPtp ? "   <== PTP TOUCHPAD" : "");

        // Keep only touchpad-ish devices for the detailed section
        if (!isPtp && !hasTouchpadUsage) {
            HidD_FreePreparsedData(pp); CloseHandle(h); continue;
        }

        printf("        input value caps (%zu):\n", vcaps.size());
        for (auto& vc : vcaps) {
            USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
            printf("          up=0x%02X u=0x%02X rid=%u bit=%u size=%u cnt=%u lmin=%ld lmax=%ld%s\n",
                   vc.UsagePage, u, vc.ReportID, vc.BitField, vc.BitSize, vc.ReportCount,
                   (long)vc.LogicalMin, (long)vc.LogicalMax, vc.IsRange ? " RANGE" : "");
        }
        printf("        feature value caps (%zu):\n", fcaps.size());
        for (auto& vc : fcaps) {
            USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
            printf("          up=0x%02X u=0x%02X rid=%u bit=%u size=%u cnt=%u lmin=%ld lmax=%ld%s\n",
                   vc.UsagePage, u, vc.ReportID, vc.BitField, vc.BitSize, vc.ReportCount,
                   (long)vc.LogicalMin, (long)vc.LogicalMax, vc.IsRange ? " RANGE" : "");
        }

        auto* ctx = new DeviceCtx();
        ctx->path = detail->DevicePath;
        ctx->hDev = h;
        ctx->isPtp = isPtp;
        ctx->hasTouchpadUsage = hasTouchpadUsage;
        ctx->maxContacts = hasContactMax ? contactMaxVal : contactIdCount;

        HIDD_ATTRIBUTES attr = {};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            ctx->vid = attr.VendorID; ctx->pid = attr.ProductID; ctx->version = attr.VersionNumber;
        }
        wchar_t wbuf[512] = {};
        if (HidD_GetProductString(h, wbuf, sizeof(wbuf))) ctx->product = wbuf;
        wchar_t mbuf[512] = {};
        if (HidD_GetManufacturerString(h, mbuf, sizeof(mbuf))) ctx->manufacturer = mbuf;

        // Build value entry map
        for (auto& vc : vcaps) {
            if (vc.UsagePage != UP_DIGITIZER) continue;
            USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
            if (u != U_X && u != U_Y && u != U_TIP_SWITCH && u != U_CONTACT_ID &&
                u != U_CONFIDENCE && u != U_CONTACT_COUNT && u != U_WIDTH && u != U_HEIGHT)
                continue;
            ValueEntry e = {};
            e.usage = u;
            e.reportId = vc.ReportID;
            e.bitOffset = vc.BitField;
            e.bitSize = vc.BitSize;
            e.reportCount = vc.ReportCount;
            e.logicalMin = vc.LogicalMin;
            e.logicalMax = vc.LogicalMax;
            ctx->entries.push_back(e);
        }

        g_devices.push_back(ctx);

        // Keep preparsed data alive — parsing WM_INPUT reports needs it.
        ctx->pp = pp;
        ctx->hDev = h;
    }

    SetupDiDestroyDeviceInfoList(devInfo);
}

// ============================================================
// Map Raw Input device handles -> our DeviceCtx.
// IMPORTANT: RAWINPUTHEADER.hDevice is NOT the CreateFile handle.
// It must be matched by device *name* via GetRawInputDeviceInfo.
// ============================================================
static void BuildRawInputHandleMap() {
    UINT n = 0;
    if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) {
        printf("  [!] GetRawInputDeviceList(size) failed\n");
        return;
    }
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        printf("  [!] GetRawInputDeviceList failed\n");
        return;
    }
    printf("  raw input device list: %u entries\n", n);

    for (UINT i = 0; i < n; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        UINT sz = 0;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &sz) != 0 || sz == 0)
            continue;
        std::wstring name(sz, L'\0');
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name.data(), &sz) == (UINT)-1)
            continue;
        name.resize(wcslen(name.c_str()));

        for (auto* c : g_devices) {
            if (lstrcmpiW(c->path.c_str(), name.c_str()) == 0) {
                g_byHandle[list[i].hDevice] = c;
                printf("  mapped rawhandle -> %ls  (%04X:%04X)\n",
                       c->product.c_str(), c->vid, c->pid);
            }
        }
    }
    printf("  mapped devices: %zu\n", g_byHandle.size());
}

// ============================================================
// Report system PTP gesture settings
// ============================================================
static void PrintPtpRegistryStatus() {
    PrintSeparator("Windows Precision Touchpad registry status (HKCU)");
    const wchar_t* sub =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad\\Status";

    struct { const wchar_t* name; const char* label; } keys[] = {
        { L"ThreeFingerSlideEnabled", "ThreeFingerSlideEnabled  (system 3-finger swipe)" },
        { L"ThreeFingerTapEnabled",   "ThreeFingerTapEnabled    (system 3-finger tap)  " },
        { L"FourFingerSlideEnabled",  "FourFingerSlideEnabled   (system 4-finger swipe)" },
        { L"FourFingerTapEnabled",    "FourFingerTapEnabled     (system 4-finger tap)  " },
        { L"TapsEnabled",             "TapsEnabled              (tap to click)         " },
        { L"ScrollEnabled",           "ScrollEnabled            (two-finger scroll)   " },
    };

    HKEY hKey = nullptr;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &hKey);
    if (r != ERROR_SUCCESS) {
        printf("  [i] key not present (rc=%ld) -> system may not expose PTP settings here\n", r);
        return;
    }
    for (auto& k : keys) {
        DWORD val = 0, sz = sizeof(val), type = 0;
        if (RegQueryValueExW(hKey, k.name, nullptr, &type, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
            printf("  %s = %lu\n", k.label, val);
        else
            printf("  %s = <not set>\n", k.label);
    }
    RegCloseKey(hKey);
}

// ============================================================
// Pointer device cross-check (PT_TOUCHPAD)
// ============================================================
static void PrintPointerDevices() {
    PrintSeparator("Pointer devices (GetPointerDevices)");
    UINT32 count = 0;
    if (!GetPointerDevices(&count, nullptr)) {
        printf("  [!] GetPointerDevices failed: %lu\n", GetLastError());
        return;
    }
    printf("  count = %u\n", count);
    if (count == 0) return;
    std::vector<POINTER_DEVICE_INFO> list(count);
    if (!GetPointerDevices(&count, list.data())) return;
    for (UINT32 i = 0; i < count; ++i) {
        const char* type = "other";
        switch (list[i].pointerDeviceType) {
        case POINTER_DEVICE_TYPE_INTEGRATED_PEN: type = "INTEGRATED_PEN"; break;
        case POINTER_DEVICE_TYPE_EXTERNAL_PEN:   type = "EXTERNAL_PEN";   break;
        case POINTER_DEVICE_TYPE_TOUCH:          type = "TOUCH";          break;
        case POINTER_DEVICE_TYPE_TOUCH_PAD:      type = "TOUCH_PAD";      break;
        default: break;
        }
        printf("  [%u] type=%-14s maxContacts=%u  product=%ls\n",
               i, type, list[i].maxActiveContacts, list[i].productString);
    }
}

// ============================================================
// Raw Input listener
// ============================================================
static void DumpFrame(DeviceCtx* d, const BYTE* report, ULONG reportLen) {
    if (reportLen < 30) return;

    // --- A) generic path: let HidP decode everything for us ---
    HIDP_DATA data[256];
    ULONG dlen = 256;
    NTSTATUS st = HidP_GetData(HidP_Input, data, &dlen, d->pp,
                               (PCHAR)report, reportLen);

    // --- B) layout-specific path: 5 contacts x 5 bytes ---
    // status(1) + X(2, LE) + Y(2, LE); status = id<<4 | pad<<2 | conf<<1 | tip
    const BYTE* p = report + 1;
    ULONG cc = p[25];

    printf("  [#%llu] cc=%lu |", (unsigned long long)d->frames, cc);

    ULONG active = 0;
    for (int i = 0; i < 5; ++i) {
        const BYTE* c = p + i * 5;
        BYTE s = c[0];
        ULONG id = s >> 4, tip = s & 1;
        ULONG x = (ULONG)c[1] | ((ULONG)c[2] << 8);
        ULONG y = (ULONG)c[3] | ((ULONG)c[4] << 8);
        if (tip) {
            ++active;
            printf(" [id%lu x%lu y%lu]", id, x, y);
        }
    }

    if (d->frames <= 4) {
        printf(" || HidP_GetData st=0x%lX n=%lu:", (unsigned long)st, dlen);
        for (ULONG i = 0; i < dlen && i < 36; ++i)
            printf(" %u=%lu", data[i].DataIndex, data[i].RawValue);
    }
    printf("\n");
    if (active > d->maxContactsSeen) d->maxContactsSeen = active;
}

static void HandleRawInput(HRAWINPUT hRaw) {
    UINT size = 0;
    if (GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0) return;
    if (size == 0) return;

    std::vector<BYTE> buf(size);
    if (GetRawInputData(hRaw, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
        return;

    auto* raw = reinterpret_cast<PRAWINPUT>(buf.data());
    if (raw->header.dwType != RIM_TYPEHID) return;

    auto it = g_byHandle.find(raw->header.hDevice);
    if (it == g_byHandle.end()) return;
    DeviceCtx* d = it->second;

    d->frames++;

    const RAWHID& hid = raw->data.hid;
    for (DWORD i = 0; i < hid.dwCount; ++i) {
        const BYTE* rep = hid.bRawData + i * hid.dwSizeHid;
        // Report id is the first byte if the device uses report ids
        BYTE rid = rep[0];
        const BYTE* payload = rep + 1;

        static ULONGLONG s_dumpCount = 0;
        if (s_dumpCount++ < 150) {
            printf("  HEX [%lu/%lu len=%lu]:", (unsigned long)(i + 1),
                   (unsigned long)hid.dwCount, (unsigned long)hid.dwSizeHid);
            for (DWORD b = 0; b < hid.dwSizeHid; ++b) printf(" %02X", rep[b]);
            printf("\n");
        }

        char tag[64];
        (void)tag;
        (void)payload;
        DumpFrame(d, rep, hid.dwSizeHid);
    }
}

static LRESULT CALLBACK SinkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        HandleRawInput((HRAWINPUT)lp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Touchpad Probe");
    setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: live logs when redirected

    int listenSeconds = 30;
    if (argc > 1) listenSeconds = atoi(argv[1]);

    printf("Touchpad Probe  build=%s %s\n", __DATE__, __TIME__);

    PrintSeparator("HID touchpad devices");
    EnumerateHid();
    printf("\n  interfaces opened=%lu  digitizer-class=%lu  touchpads=%zu\n",
           g_totalInterfaces, g_digitizerInterfaces, g_devices.size());

    if (g_devices.empty()) {
        printf("  [!] No touchpad HID devices found.\n");
    } else {
        for (size_t i = 0; i < g_devices.size(); ++i) {
            DeviceCtx* d = g_devices[i];
            printf("\n  --- device [%zu] ---\n", i);
            printf("  isPTP            : %s\n", d->isPtp ? "YES" : "no");
            printf("  touchpad usage   : %s\n", d->hasTouchpadUsage ? "yes" : "no");
            printf("  VID:PID          : %04X:%04X  ver=%04X\n", d->vid, d->pid, d->version);
            printf("  manufacturer     : %ls\n", d->manufacturer.c_str());
            printf("  product          : %ls\n", d->product.c_str());
            printf("  maxContacts      : %lu\n", d->maxContacts);
            printf("  multi-touch>=3   : %s\n", d->maxContacts >= 3 ? "YES" : "NO");
            printf("  value entries    : %zu\n", d->entries.size());
            printf("  path             : %ls\n", d->path.c_str());
        }
    }

    PrintPointerDevices();
    PrintPtpRegistryStatus();

    // ---- Register raw input on a hidden top-level window ----
    PrintSeparator("Raw Input listener");
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SinkProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TpProbeSinkWnd";
    RegisterClassExW(&wc);

    g_sink = CreateWindowExW(0, L"TpProbeSinkWnd", L"", WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_sink) { printf("  [!] sink window failed\n"); return 1; }

    BuildRawInputHandleMap();

    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = UP_DIGITIZER;
    rid.usUsage = U_TOUCHPAD;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = g_sink;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        printf("  [!] RegisterRawInputDevices FAILED: %lu\n", GetLastError());
        printf("      (error 87 = bad params, 5 = access denied)\n");
        return 1;
    }
    printf("  registered usUsagePage=0x0D usUsage=0x05 flags=RIDEV_INPUTSINK\n");
    printf("  listening for %d seconds — touch the touchpad now\n", listenSeconds);
    printf("  (use 3 fingers, then 4 fingers)\n\n");

    DWORD start = GetTickCount();
    DWORD lastReport = start;
    while (GetTickCount() - start < (DWORD)listenSeconds * 1000) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        DWORD now = GetTickCount();
        if (now - lastReport >= 2000) {
            lastReport = now;
            ULONGLONG total = 0;
            ULONG peak = 0;
            for (auto* d : g_devices) { total += d->frames; peak = std::max(peak, d->maxContactsSeen); }
            printf("  -- stats: frames=%llu peakContacts=%lu\n",
                   (unsigned long long)total, peak);
        }
        Sleep(3);
    }

    PrintSeparator("RESULT");
    ULONGLONG total = 0;
    ULONG peak = 0;
    for (auto* d : g_devices) { total += d->frames; peak = std::max(peak, d->maxContactsSeen); }

    if (total == 0) {
        printf("  [X] No raw HID reports received.\n");
        printf("      => Raw Input path BLOCKED on this system. Need fallback strategy.\n");
    } else {
        printf("  [OK] Received %llu HID frames, peak contacts = %lu\n",
               (unsigned long long)total, peak);
        if (peak >= 3) printf("  [OK] 3-finger gestures are readable.\n");
        if (peak >= 4) printf("  [OK] 4-finger gestures are readable.\n");
        if (peak < 3)  printf("  [!] Never saw 3+ contacts — try a slower 3-finger swipe.\n");
    }
    printf("\n");

    for (auto* d : g_devices) {
        if (d->pp) HidD_FreePreparsedData(d->pp);
        if (d->hDev != INVALID_HANDLE_VALUE) CloseHandle(d->hDev);
        delete d;
    }
    return 0;
}
