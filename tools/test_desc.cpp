// test_desc.cpp — verify we can read the raw HID report descriptor
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winioctl.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cstdio>

// MinGW's hidsdi.h may not expose this; it is a standard HID IOCTL.
#ifndef IOCTL_HID_GET_REPORT_DESCRIPTOR
#define HID_DEVICE_BUS 0x0000000B
#define IOCTL_HID_GET_REPORT_DESCRIPTOR \
    CTL_CODE(HID_DEVICE_BUS, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const wchar_t* path =
        L"\\\\?\\hid#uniw0001&col02#5&26c74c0&0&0001"
        L"#{4d1e55b2-f16f-11cf-88cb-001111000030}";

    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) {
        printf("CreateFile failed: %lu\n", GetLastError());
        return 1;
    }
    printf("opened ok\n");

    BYTE buf[4096] = {};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_HID_GET_REPORT_DESCRIPTOR,
                              nullptr, 0, buf, sizeof(buf), &bytes, nullptr);
    printf("IOCTL_HID_GET_REPORT_DESCRIPTOR: ok=%d bytes=%lu err=%lu\n",
           ok, bytes, ok ? 0 : GetLastError());

    if (ok && bytes > 0) {
        printf("descriptor length = %lu\n", bytes);
        for (DWORD i = 0; i < bytes; ++i) {
            if (i % 16 == 0) printf("\n%04lX: ", i);
            printf("%02X ", buf[i]);
        }
        printf("\n");
    }
    CloseHandle(h);
    return 0;
}
