// EventLog.h — the in-memory history the settings window shows on its
// "recent events" page.
//
// This used to be part of an HTTP control panel: the same buffer backed a web
// page that could also rewrite the configuration over the network. The native
// window replaced that page entirely, so all that is left is the ring buffer --
// no socket, no JSON, and no second way to change settings behind the UI's
// back.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// One line as handed to the native window.
struct LogLine {
    uint64_t    atMs = 0;
    std::string kind;      // "info" / "gesture" / "warn" / "drag"
    std::string message;
};

class EventLog {
public:
    // Newest last, capped at `max`.
    std::vector<LogLine> recentLogs(size_t max) const;

    // Appends one line. Thread-unsafe on purpose: everything already runs on
    // the message-loop thread.
    void log(const char* kind, const std::string& message);

private:
    static constexpr size_t kMaxLogs = 400;
    std::deque<LogLine> lines_;
};
