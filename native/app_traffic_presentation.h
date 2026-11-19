#pragma once

#include "snapshots.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <iomanip>
#include <sstream>

namespace AppTrafficPresentation {
inline std::wstring FormatTotalBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB", L"EB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 6) { value /= 1024.0; ++unit; }
    if (unit == 0) return std::to_wstring(bytes) + L" B";
    std::wstringstream text;
    text << std::fixed << std::setprecision(2) << value << L" " << units[unit];
    return text.str();
}
inline uint64_t SessionBytes(const AppActivityItem& app) {
    const auto maximum = (std::numeric_limits<uint64_t>::max)();
    return app.total_download_bytes > maximum - app.total_upload_bytes
        ? maximum : app.total_download_bytes + app.total_upload_bytes;
}

inline bool MostUsageFirst(const AppActivityItem& left, const AppActivityItem& right) {
    const auto left_bytes = SessionBytes(left), right_bytes = SessionBytes(right);
    if (left_bytes != right_bytes) return left_bytes > right_bytes;
    if (left.name != right.name) return left.name < right.name;
    return left.pid < right.pid;
}

struct Bar { double download, upload; };

// Segment lengths are relative to the largest visible app, not its live rate.
inline Bar UsageBar(const AppActivityItem& app, uint64_t largest_session_bytes) {
    const double total = static_cast<double>(app.total_download_bytes)
        + static_cast<double>(app.total_upload_bytes);
    if (total == 0.0 || largest_session_bytes == 0) return {};
    const double length = (std::min)(1.0, total / static_cast<double>(largest_session_bytes));
    return {length * (app.total_download_bytes / total), length * (app.total_upload_bytes / total)};
}
} // namespace AppTrafficPresentation
