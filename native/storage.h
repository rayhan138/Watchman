#pragma once
#include "history_profile.h"
#include "traffic_export.h"
#include <windows.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3; // Forward declaration of SQLite database structure

struct AppTrafficRecord {
    std::wstring app_name;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint64_t last_seen = 0; // Unix timestamp
};

struct TrafficExportResult {
    bool success = false;
    std::wstring filepath;
    std::wstring error;
};

class TrafficStorage {
public:
    TrafficStorage();
    // Explicit paths never resolve APPDATA; useful for isolated history tests.
    explicit TrafficStorage(const std::wstring& database_path, int busy_timeout_ms = 5000);
    explicit TrafficStorage(const WatchmanHistory::Selection& profile, int busy_timeout_ms = 5000);
    ~TrafficStorage();
    const std::wstring& GetDatabaseFilePath() const { return database_path_; }
    WatchmanHistory::Profile GetProfileKind() const { return profile_.profile; }
    std::wstring GetProfileDescription() const { return WatchmanHistory::Description(profile_); }

    bool SaveSessionTraffic(uint64_t delta_rx_bytes, uint64_t delta_tx_bytes);
    bool SaveAppTraffic(const std::wstring& app_name, uint64_t delta_rx, uint64_t delta_tx);
    bool SaveAppTrafficBatch(const std::vector<AppTrafficRecord>& apps);

    // One atomic, additive commit for a sample-date bucket. False means retry
    // the unchanged bucket; no daily or app deltas have been acknowledged.
    bool SaveTrafficForDate(const std::wstring& date, uint64_t delta_rx,
                            uint64_t delta_tx, const std::vector<AppTrafficRecord>& apps);
    std::string GetLastError();
    static std::wstring GetCurrentDateString();

    // Retrieves all total daily records
    std::vector<TrafficRecord> LoadHistory();
    // Checked whole-history read. A failed/partial query never replaces the
    // caller's last good history with an apparently empty database.
    bool LoadHistory(std::vector<TrafficRecord>& records);

    // Aggregates daily records into YYYY-MM buckets for the yearly chart.
    std::vector<TrafficRecord> LoadMonthlyHistory();

    // Returns only years that contain saved daily traffic.
    std::vector<int> LoadAvailableHistoryYears();

    // Retrieves per-app records for a specific date
    std::vector<AppTrafficRecord> GetAppTrafficForDate(const std::wstring& date_str);
    // Checked bulk read. Leaves records unchanged on failure.
    bool LoadAppTrafficForDate(const std::wstring& date_str, std::vector<AppTrafficRecord>& records);

    // Retrieves today's total traffic for a given application
    uint64_t GetAppTotalTrafficToday(const std::wstring& app_name);

    // Excel-ready five-column reports in Downloads: binary MB/GB numbers,
    // friendly period labels and summaries, UTF-8 BOM and CRLF line endings.
    // This only reads history; it never changes stored byte totals.
    TrafficExportResult ExportTrafficCsv(const TrafficExportOptions& options);

private:
    sqlite3* db_ = nullptr;
    std::mutex storage_mutex_;
    std::wstring database_path_;
    WatchmanHistory::Selection profile_;
    int busy_timeout_ms_ = 5000;
    std::string last_error_;

    static WatchmanHistory::Selection GetDefaultProfile();
    bool OpenDatabase();
    void CloseDatabase();
    bool CheckResult(sqlite3* db, int result, int expected, const char* operation);
    bool ExecuteSql(sqlite3* db, const char* sql);
    bool EnsureColumn(sqlite3* db, const char* table, const char* column, const char* alter_sql);
};

// Owned exclusively by the engine worker (or by Stop after joining it).
// Sample dates survive write failures and midnight rollovers.
class PendingTrafficHistory {
public:
    void AddDaily(const std::wstring& date, uint64_t delta_rx, uint64_t delta_tx);
    void AddApp(const std::wstring& date, const AppTrafficRecord& app);
    bool Flush(TrafficStorage& storage);
    bool Empty() const { return days_.empty(); }
    std::map<std::wstring, uint64_t> GetAppTotals(const std::wstring& date) const;

private:
    struct Day {
        uint64_t rx_bytes = 0;
        uint64_t tx_bytes = 0;
        std::map<std::wstring, AppTrafficRecord> apps;
    };
    std::map<std::wstring, Day> days_;
};
