// gesture_selftest.cpp — replay captured HID frames through the gesture engine
//
// Validates recognition logic offline against real hardware data, so threshold
// changes can be regression-tested without touching the touchpad.
//
// Build:
//   g++ -std=c++17 -O2 gesture_selftest.cpp ../src/GestureEngine.cpp
//       ../src/Touchpad.cpp -I../src -o gesture_selftest.exe -lhid -lsetupapi

#include "GestureEngine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

bool decodePacked5(const unsigned* b, int n, ContactFrame& fr) {
    if (n < 30) return false;
    const unsigned* p = b + 1;  // skip report id
    fr.contactCount = static_cast<uint8_t>(p[25]);
    if (fr.contactCount > 5) return false;
    for (int i = 0; i < 5; ++i) {
        const unsigned* c = p + i * 5;
        if (!(c[0] & 1)) continue;
        Contact ct;
        ct.id        = static_cast<uint8_t>(c[0] >> 4);
        ct.confident = (c[0] & 2) != 0;
        ct.tip       = true;
        ct.x         = static_cast<uint16_t>(c[1] | (c[2] << 8));
        ct.y         = static_cast<uint16_t>(c[3] | (c[4] << 8));
        fr.contacts.push_back(ct);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: gesture_selftest <probe.log>\n");
        return 1;
    }
    FILE* f = fopen(argv[1], "r");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }

    GestureEngine engine;
    char line[2048];
    int frame = 0, decoded = 0, failed = 0, gestureCount = 0;
    int maxContacts = 0;

    while (fgets(line, sizeof(line), f)) {
        const char* marker = strstr(line, "len=");
        const char* colon  = marker ? strchr(marker, ':') : nullptr;
        if (!colon) continue;

        // tokenize the hex payload
        unsigned b[64];
        int n = 0;
        char buf[512];
        strncpy(buf, colon + 1, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* tok = strtok(buf, " \t\r\n");
        while (tok && n < 64) {
            b[n++] = static_cast<unsigned>(strtoul(tok, nullptr, 16));
            tok = strtok(nullptr, " \t\r\n");
        }
        if (n < 30) continue;

        ContactFrame fr;
        fr.timestampUs = static_cast<uint64_t>(frame) * 20000ULL;  // assume 50 Hz
        if (!decodePacked5(b, n, fr)) { ++failed; ++frame; continue; }
        ++decoded;
        if (static_cast<int>(fr.contacts.size()) > maxContacts)
            maxContacts = static_cast<int>(fr.contacts.size());

        engine.feed(fr);
        const GestureEvent g = engine.poll();
        if (g) {
            ++gestureCount;
            printf("  frame %4d (t=%.0f ms)  contacts=%zu  ->  %s\n",
                   frame, fr.timestampUs / 1000.0, fr.contacts.size(),
                   gestureName(g));
        }
        ++frame;
    }
    fclose(f);

    printf("\n--- summary ---\n");
    printf("  frames      : %d\n", frame);
    printf("  decoded     : %d\n", decoded);
    printf("  decode fail : %d\n", failed);
    printf("  max contacts: %d\n", maxContacts);
    printf("  gestures    : %d\n", gestureCount);
    return 0;
}
