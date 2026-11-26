#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TrafficRecord {
    std::wstring date; // YYYY-MM-DD, in the local sample calendar.
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
};

enum class TrafficExportPeriod {
    Daily,
    Weekly,
    Monthly,
    Yearly,
};

struct TrafficExportOptions {
    TrafficExportPeriod period = TrafficExportPeriod::Monthly;
    // Daily and weekly exports include only saved days in this YYYY-MM.
    std::wstring month;
    // Monthly and yearly exports include only this calendar year.
    int year = 0;
};

namespace WatchmanExport {
struct CsvDocument {
    // Complete UTF-8 CSV, including one BOM and CRLF line endings.
    // Empty on failure; the caller must not create a partial/empty report.
    std::string utf8;
    std::wstring error;
};

std::wstring ValidateOptions(const TrafficExportOptions& options);

// Pure presentation: never opens a database, resolves a user folder, or writes
// files. Weeks retain Monday-Sunday grouping, clipped to the selected month.
// Only saved buckets are emitted; averages count exported rows, not missing days.
CsvDocument BuildCsv(const std::vector<TrafficRecord>& saved, const TrafficExportOptions& options);
} // namespace WatchmanExport
