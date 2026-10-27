// EventLog.cpp
#include "EventLog.h"

#include <windows.h>

#include <algorithm>

void EventLog::log(const char* kind, const std::string& message) {
    LogLine l;
    l.atMs    = GetTickCount64();
    l.kind    = kind ? kind : "info";
    l.message = message;
    lines_.push_back(std::move(l));
    while (lines_.size() > kMaxLogs) lines_.pop_front();
}

std::vector<LogLine> EventLog::recentLogs(size_t max) const {
    const size_t n = std::min(max, lines_.size());
    std::vector<LogLine> out;
    out.reserve(n);
    for (size_t i = lines_.size() - n; i < lines_.size(); ++i)
        out.push_back(lines_[i]);
    return out;
}
