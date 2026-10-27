#include "Preflight.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <vector>

// The snapshot that is currently applied to the registry, so a dying process can
// still put the user's own swipes back. Only one suppression can be live at a
// time; this is that one.
static GestureSuppression g_live;
static bool               g_liveValid = false;

namespace {

const wchar_t* kPtpKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad";

bool readDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD* out) {
    DWORD type = 0, size = sizeof(DWORD), val = 0;
    const LSTATUS st = RegGetValueW(root, sub, name, RRF_RT_REG_DWORD, &type,
                                    &val, &size);
    if (st != ERROR_SUCCESS) return false;
    *out = val;
    return true;
}

std::wstring statusPath() {
    std::wstring s = kPtpKey;
    s += L"\\Status";
    return s;
}

bool icontains(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !*hay) return false;
    const std::wstring h(hay);
    std::wstring hn, nn;
    hn.reserve(h.size());
    for (wchar_t c : h) hn += (wchar_t)towlower(c);
    for (const wchar_t* p = needle; *p; ++p) nn += (wchar_t)towlower(*p);
    return hn.find(nn) != std::wstring::npos;
}

bool interestingName(const wchar_t* n) {    return icontains(n, L"finger") || icontains(n, L"gesture") ||
           icontains(n, L"touchpad") || icontains(n, L"swipe");
}

// Depth/budget bounded so a scan of CurrentVersion cannot run away.
void walk(HKEY parent, const std::wstring& path, int depth, int maxDepth,
          long* budget, std::wstring* out) {
    if (depth > maxDepth || *budget <= 0) return;
    HKEY key = nullptr;
    if (RegOpenKeyExW(parent, path.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return;

    wchar_t name[256];
    BYTE data[512];
    DWORD index = 0;
    for (;;) {
        DWORD nameLen = 256, dataLen = sizeof(data), type = 0;
        if (RegEnumValueW(key, index++, name, &nameLen, nullptr, &type, data,
                          &dataLen) != ERROR_SUCCESS)
            break;
        if (--*budget <= 0) break;
        if (!interestingName(name)) continue;

        wchar_t line[640];
        if (type == REG_DWORD && dataLen >= 4) {
            const DWORD v = *reinterpret_cast<const DWORD*>(data);
            swprintf(line, 640, L"  %ls\\%ls = %lu\n", path.c_str(), name,
                     (unsigned long)v);
        } else if (type == REG_SZ && dataLen >= 2) {
            swprintf(line, 640, L"  %ls\\%ls = \"%ls\"\n", path.c_str(), name,
                     reinterpret_cast<const wchar_t*>(data));
        } else {
            swprintf(line, 640, L"  %ls\\%ls = <type %lu>\n", path.c_str(), name,
                     (unsigned long)type);
        }
        *out += line;
    }

    index = 0;
    for (;;) {
        DWORD nameLen = 256;
        if (RegEnumKeyExW(key, index++, name, &nameLen, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS)
            break;
        if (--*budget <= 0) break;
        const std::wstring child = path + L"\\" + name;
        if (interestingName(name)) {
            *out += L"  " + child + L"  [key]\n";
        }
        walk(HKEY_CURRENT_USER, child, depth + 1, maxDepth, budget, out);
    }
    RegCloseKey(key);
}

}  // namespace

TouchpadPower queryTouchpadPower() {
    DWORD v = 0;
    const std::wstring sub = statusPath();
    // Enabled lives under the Status subkey on current builds.
    if (readDword(HKEY_CURRENT_USER, sub.c_str(), L"Enabled", &v))
        return v ? TouchpadPower::Enabled : TouchpadPower::Disabled;
    // Some builds keep it on the parent key.
    if (readDword(HKEY_CURRENT_USER, kPtpKey, L"Enabled", &v))
        return v ? TouchpadPower::Enabled : TouchpadPower::Disabled;
    // Absent key means "never touched by the user" on most systems, which
    // implies the default: touchpad on.
    return TouchpadPower::Unknown;
}

bool setTouchpadPower(bool enable) {
    const std::wstring sub = statusPath();
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &key, nullptr) !=
            ERROR_SUCCESS)
            return false;
    }
    const DWORD v = enable ? 1 : 0;
    const LSTATUS st = RegSetValueExW(key, L"Enabled", 0, REG_DWORD,
                                      reinterpret_cast<const BYTE*>(&v),
                                      sizeof(v));
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

std::wstring dumpPrecisionTouchpadRegistry() {
    std::wstring out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPtpKey, 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return L"  <key not found>\n";

    wchar_t name[256];
    BYTE data[512];
    DWORD index = 0;
    for (;;) {
        DWORD nameLen = 256, dataLen = sizeof(data), type = 0;
        const LSTATUS st = RegEnumValueW(key, index++, name, &nameLen, nullptr,
                                         &type, data, &dataLen);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) break;

        wchar_t line[512];
        if (type == REG_DWORD && dataLen >= 4) {
            const DWORD v = *reinterpret_cast<const DWORD*>(data);
            swprintf(line, 512, L"  %-28ls = %lu\n", name, (unsigned long)v);
        } else if (type == REG_SZ && dataLen >= 2) {
            swprintf(line, 512, L"  %-28ls = \"%ls\"\n", name,
                     reinterpret_cast<const wchar_t*>(data));
        } else {
            swprintf(line, 512, L"  %-28ls = <type %lu, %lu bytes>\n", name,
                     (unsigned long)type, (unsigned long)dataLen);
        }
        out += line;
    }
    RegCloseKey(key);
    return out;
}

std::wstring findGestureSettings() {
    std::wstring out;
    long budget = 250000;
    static const wchar_t* roots[] = {
        L"Control Panel",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
    };
    for (const wchar_t* r : roots) {
        out += L"[root] HKCU\\";
        out += r;
        out += L"\n";
        walk(HKEY_CURRENT_USER, r, 0, 5, &budget, &out);
        if (budget <= 0) {
            out += L"  <budget exhausted, results may be partial>\n";
            break;
        }
    }
    if (out.find(L"[key]") == std::wstring::npos &&
        out.rfind(L" = ", std::wstring::npos) == std::wstring::npos) {
        out += L"  <no match: Windows only creates 3/4-finger action values\n"
               L"   after they are changed once in the Settings app>\n";
    }
    return out;
}

namespace {

bool writeDword(const wchar_t* sub, const wchar_t* name, DWORD v) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS st = RegSetValueExW(key, name, 0, REG_DWORD,
                                      reinterpret_cast<const BYTE*>(&v),
                                      sizeof(v));
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

void deleteValue(const wchar_t* sub, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, name);
    RegCloseKey(key);
}

// Crash recovery for the swipe takeover. Being killed from Task Manager is a
// normal way to stop a tray app and skips the restore path, which would leave
// the user's own three/four finger swipes dead with nothing to notice it.
// The snapshot is therefore mirrored to a file next to the exe (main.cpp has
// already made that the working directory). If the file is present at startup
// the previous run died dirty, and it -- not the registry, which we zeroed
// ourselves -- holds the real original values.
const char* kMarker = "tp_gestures.suppress";

const wchar_t* kMarkerNames[] = {
    L"ThreeFingerSlideEnabled",
    L"FourFingerSlideEnabled",
};

// One line per value:  name|existed|oldValue
bool loadSuppressionMarker(GestureSuppression* out) {
    FILE* f = std::fopen(kMarker, "rb");
    if (!f) return false;
    char name[128] = {};
    int existed = 0;
    unsigned long value = 0;
    out->count = 0;
    while (out->count < 4 &&
           std::fscanf(f, "%127[^|]|%d|%lu\n", name, &existed, &value) == 3) {
        wchar_t wname[128] = {};
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 127);
        const wchar_t* match = nullptr;
        for (const wchar_t* n : kMarkerNames)
            if (std::wcscmp(wname, n) == 0) {
                match = n;
                break;
            }
        if (!match) {
            out->count = 0;
            break;
        }
        out->items[out->count].name     = match;
        out->items[out->count].existed  = existed != 0;
        out->items[out->count].oldValue = (DWORD)value;
        ++out->count;
    }
    std::fclose(f);
    return out->count > 0;
}

void saveSuppressionMarker(const GestureSuppression& s) {
    FILE* f = std::fopen(kMarker, "wb");
    if (!f) return;
    for (int i = 0; i < s.count; ++i) {
        char utf8[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, s.items[i].name, -1, utf8, 255, nullptr,
                            nullptr);
        std::fprintf(f, "%s|%d|%lu\n", utf8, s.items[i].existed ? 1 : 0,
                     (unsigned long)s.items[i].oldValue);
    }
    std::fclose(f);
}

void clearSuppressionMarker() { std::remove(kMarker); }

}  // namespace

void installCrashRecovery();

GestureSuppression suppressSystemGestures() {
    // Only the swipe actions conflict with the gesture engine: a tap is
    // instantaneous while a swipe runs for hundreds of milliseconds, which is
    // where double-triggering shows up. Leaving tap handling untouched keeps the
    // footprint on the user's system as small as possible.
    static const wchar_t* kNames[] = {
        L"ThreeFingerSlideEnabled",
        L"FourFingerSlideEnabled",
    };

    GestureSuppression s;

    // Being killed from Task Manager is a normal way to stop a tray app, and it
    // skips the restore path, which would leave the user's own swipes dead with
    // no way to notice. The snapshot is therefore mirrored to a file: if it is
    // there on startup, the previous run died dirty and the file -- not the
    // registry, which we ourselves zeroed -- holds the real original values.
    GestureSuppression saved;
    const bool recovered = loadSuppressionMarker(&saved);

    for (const wchar_t* n : kNames) {
        DWORD v = 0;
        const bool existed = readDword(HKEY_CURRENT_USER, kPtpKey, n, &v);
        s.items[s.count].name     = n;
        s.items[s.count].existed  = existed;
        s.items[s.count].oldValue = existed ? v : 1;
        if (recovered) {
            for (int i = 0; i < saved.count; ++i) {
                if (wcscmp(saved.items[i].name, n) == 0) {
                    s.items[s.count].existed  = saved.items[i].existed;
                    s.items[s.count].oldValue = saved.items[i].oldValue;
                    break;
                }
            }
        }
        ++s.count;

        // Missing means "default", and the default is enabled, so we write 0
        // either way. Writing the value is exactly what the Settings app does
        // on first change, so the driver understands it.
        if ((!existed || v != 0) && writeDword(kPtpKey, n, 0)) s.changed = true;
    }

    saveSuppressionMarker(s);

    // From here on the registry must be treated as ours: even when nothing was
    // written this run (a previous run died dirty and left the values at 0),
    // the original values only exist in this snapshot, so a crash has to be able
    // to put them back.
    g_live      = s;
    g_liveValid = s.count > 0;
    installCrashRecovery();

    return s;
}

bool restoreSystemGestures(const GestureSuppression& s) {
    g_liveValid = false;
    bool ok = true;
    for (int i = 0; i < s.count; ++i) {
        const auto& it = s.items[i];
        if (it.existed) {
            if (!writeDword(kPtpKey, it.name, it.oldValue)) ok = false;
        } else {
            // The value did not exist before us, so remove it and let the
            // driver fall back to its own default.
            deleteValue(kPtpKey, it.name);
        }
    }
    if (ok) clearSuppressionMarker();
    return ok;
}

// ---- crash recovery -------------------------------------------------------
// Being killed is one thing (the marker file covers the next launch), but a
// crash usually means the user never launches us again -- and then their own
// three/four finger swipes stay dead with nothing to point at. Both paths below
// run while the process is already going down, so they do the least possible
// work and then let it die.

namespace {

LONG WINAPI crashFilter(EXCEPTION_POINTERS*) {
    static bool entered = false;
    if (!entered) {
        entered = true;
        if (g_liveValid) restoreSystemGestures(g_live);
    }
    return EXCEPTION_EXECUTE_HANDLER;  // terminate on the spot
}

void terminateHandler() {
    // An uncaught C++ exception never reaches the SEH filter on this toolchain;
    // std::terminate is the only place to catch it.
    if (g_liveValid) restoreSystemGestures(g_live);
    std::_Exit(3);  // skip static destruction: it may be what is broken
}

}  // namespace

void installCrashRecovery() {
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate(terminateHandler);
}
