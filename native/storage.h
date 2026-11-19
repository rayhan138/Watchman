#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3; // Forward declaration of SQLite database structure

struct TrafficRecord {
    std::wstring date; // Format: YYYY-MM-DD
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
};

struct AppTrafficRecord {
    std::wstring app_name;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint64_t last_seen = 0; // Unix timestamp
};

enum class TrafficExportPeriod {
    Daily,
    Weekly,
    Monthly,
    Yearly,
};

struct TrafficExportOptions {
    TrafficExportPeriod period = TrafficExportPeriod::Monthly;
    // Daily and weekly exports are scoped to YYYY-MM.
    std::wstring month;
    // Monthly and yearly exports are scoped to one calendar year.
    int year = 0;
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
    ~TrafficStorage();

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

    // Mirrors Watchman's Tauri export contract. Reports are written as UTF-8
    // CSV files to the user's Downloads folder and contain readable and raw
    // byte totals for the selected saved-history range.
    TrafficExportResult ExportTrafficCsv(const TrafficExportOptions& options);

private:
    sqlite3* db_ = nullptr;
    std::mutex storage_mutex_;
    std::wstring database_path_;
    int busy_timeout_ms_ = 5000;
    std::string last_error_;

    static std::wstring GetDatabasePath();
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
