// Standalone synthetic history tests. No default TrafficStorage constructor,
// engine/services, APPDATA resolution, real history, or Downloads access.
#include "storage.h"
#include "sqlite3.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempHistory {
    std::filesystem::path directory;
    std::wstring database_path;

    TempHistory() {
        wchar_t temp[MAX_PATH]{};
        wchar_t unique[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        Require(length > 0 && length < MAX_PATH, "GetTempPathW failed");
        Require(GetTempFileNameW(temp, L"wmh", 0, unique) != 0, "GetTempFileNameW failed");
        Require(DeleteFileW(unique) != 0, "Delete temporary placeholder failed");
        directory = unique;
        Require(std::filesystem::create_directory(directory), "Create temporary directory failed");
        database_path = (directory / L"history.db").wstring();
    }

    ~TempHistory() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

class Connection {
public:
    explicit Connection(const std::wstring& path) {
        const int result = sqlite3_open16(path.c_str(), &db_);
        if (result != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "No SQLite handle";
            sqlite3_close_v2(db_);
            db_ = nullptr;
            throw std::runtime_error(error);
        }
    }
    ~Connection() { sqlite3_close_v2(db_); }

    void Exec(const char* sql) {
        Require(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK,
                std::string(sql) + ": " + sqlite3_errmsg(db_));
    }

    sqlite3_int64 Integer(const char* sql) {
        auto statement = Query(sql);
        Require(sqlite3_step(statement.get()) == SQLITE_ROW, std::string("No scalar row: ") + sql);
        return sqlite3_column_int64(statement.get(), 0);
    }

    std::string Text(const char* sql) {
        auto statement = Query(sql);
        Require(sqlite3_step(statement.get()) == SQLITE_ROW, std::string("No text row: ") + sql);
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
        return value ? value : "";
    }

private:
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
    sqlite3* db_ = nullptr;

    Statement Query(const char* sql) {
        sqlite3_stmt* raw = nullptr;
        Require(sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) == SQLITE_OK,
                std::string(sql) + ": " + sqlite3_errmsg(db_));
        return Statement(raw, sqlite3_finalize);
    }
};

void TestTauriSchema() {
    TempHistory temp;
    Connection observer(temp.database_path);
    observer.Exec(
        "CREATE TABLE daily_traffic (date TEXT PRIMARY KEY, "
        "upload_bytes INTEGER NOT NULL DEFAULT 0, download_bytes INTEGER NOT NULL DEFAULT 0, "
        "updated_at TEXT NOT NULL);"
        "INSERT INTO daily_traffic VALUES ('2026-04-01', 20, 100, 'old-timestamp');"
        "INSERT INTO daily_traffic VALUES ('2026-03-31', 3, 4, 'preserve-me');"
        "CREATE TABLE unrelated (value TEXT); INSERT INTO unrelated VALUES ('untouched');");
    TrafficStorage storage(temp.database_path, 0);
    Require(storage.SaveTrafficForDate(L"2026-04-01", 7, 9, {{L"browser.exe", 5, 6, 123}}),
            storage.GetLastError());
    Require(storage.SaveTrafficForDate(L"2026-04-02", 11, 13, {}), storage.GetLastError());
    Require(observer.Integer("SELECT download_bytes FROM daily_traffic WHERE date='2026-04-01';") == 107,
            "Existing Tauri download total not incremented");
    Require(observer.Integer("SELECT upload_bytes FROM daily_traffic WHERE date='2026-04-01';") == 29,
            "Existing Tauri upload total not incremented");
    const auto timestamp = observer.Text("SELECT updated_at FROM daily_traffic WHERE date='2026-04-01';");
    Require(timestamp.size() == 24 && timestamp[10] == 'T' && timestamp.back() == 'Z',
            "Updated timestamp is not UTC RFC3339 with milliseconds");
    Require(!observer.Text("SELECT updated_at FROM daily_traffic WHERE date='2026-04-02';").empty(),
            "New Tauri row has no timestamp");
    Require(observer.Text("SELECT updated_at FROM daily_traffic WHERE date='2026-03-31';") == "preserve-me",
            "Untouched historical timestamp changed");
    Require(observer.Text("SELECT value FROM unrelated;") == "untouched", "Unrelated table changed");
    Require(observer.Integer("SELECT COUNT(*) FROM pragma_table_info('daily_traffic') "
                             "WHERE name='updated_at' AND dflt_value IS NULL AND \"notnull\"=1;") == 1,
            "Existing Tauri timestamp constraint/default changed");
    Require(observer.Integer("SELECT last_seen FROM app_traffic;") == 123, "App sample timestamp changed");
}

void TestNativeSchemaMigration() {
    TempHistory temp;
    Connection observer(temp.database_path);
    observer.Exec(
        "CREATE TABLE daily_traffic (date TEXT PRIMARY KEY, download_bytes INTEGER DEFAULT 0, "
        "upload_bytes INTEGER DEFAULT 0);"
        "INSERT INTO daily_traffic VALUES ('2026-01-01', 100, 20);"
        "CREATE TABLE app_traffic (date TEXT, app_name TEXT, download_bytes INTEGER DEFAULT 0, "
        "upload_bytes INTEGER DEFAULT 0, PRIMARY KEY(date,app_name));"
        "INSERT INTO app_traffic VALUES ('2026-01-01', 'old.exe', 40, 3);");
    {
        TrafficStorage storage(temp.database_path, 0);
        Require(storage.GetLastError().empty(), storage.GetLastError());
        Require(observer.Integer("SELECT download_bytes FROM daily_traffic;") == 100,
                "Migration changed daily bytes");
        Require(observer.Integer("SELECT download_bytes FROM app_traffic;") == 40,
                "Migration changed app bytes");
        Require(!observer.Text("SELECT updated_at FROM daily_traffic;").empty(), "Legacy timestamp not backfilled");
        Require(observer.Integer("SELECT last_seen FROM app_traffic;") == 0, "Legacy app timestamp not initialized");
        Require(storage.SaveTrafficForDate(L"2026-01-01", 2, 4, {{L"old.exe", 5, 6, 200}}),
                storage.GetLastError());
    }
    TrafficStorage reopened(temp.database_path, 0);
    Require(reopened.SaveTrafficForDate(L"2026-01-02", 1, 2, {}), reopened.GetLastError());
    Require(observer.Integer("SELECT download_bytes FROM daily_traffic WHERE date='2026-01-01';") == 102,
            "Reopening migrated database changed bytes");
    Require(observer.Integer("SELECT download_bytes FROM app_traffic WHERE app_name='old.exe';") == 45,
            "Migrated app upsert lost existing total");
}

void TestFreshSchema() {
    TempHistory temp;
    TrafficStorage storage(temp.database_path, 0);
    Require(storage.SaveTrafficForDate(L"2026-02-28", 100, 20, {{L"fresh.exe", 50, 10, 300}}),
            storage.GetLastError());
    Connection observer(temp.database_path);
    Require(observer.Integer("SELECT COUNT(*) FROM daily_traffic WHERE updated_at IS NOT NULL;") == 1,
            "Fresh schema lacks timestamp");
    Require(!storage.SaveTrafficForDate(L"2026-02-30", 1, 1, {}), "Invalid date was accepted");
    Require(observer.Integer("SELECT COUNT(*) FROM daily_traffic;") == 1, "Invalid date wrote history");
}

void TestLockRetryAndRollover() {
    TempHistory temp;
    TrafficStorage storage(temp.database_path, 0);
    Connection writer(temp.database_path);
    PendingTrafficHistory pending;
    pending.AddDaily(L"2026-12-31", 100, 20);
    pending.AddApp(L"2026-12-31", {L"browser.exe", 40, 5, 1000});
    writer.Exec("BEGIN IMMEDIATE;");
    Require(!pending.Flush(storage), "Flush succeeded while another writer held the database");
    Require(!pending.Empty(), "Failed flush discarded pending data");
    Require(!storage.GetLastError().empty(), "Lock failure has no diagnostic");
    Require(writer.Integer("SELECT COUNT(*) FROM daily_traffic;") == 0, "Lock failure wrote daily bytes");
    pending.AddDaily(L"2026-12-31", 7, 3);
    pending.AddDaily(L"2027-01-01", 200, 30);
    pending.AddApp(L"2027-01-01", {L"browser.exe", 80, 10, 2000});
    writer.Exec("ROLLBACK;");
    Require(pending.Flush(storage), storage.GetLastError());
    Require(pending.Empty(), "Successful flush retained acknowledged bytes");
    Require(pending.Flush(storage), "Empty second flush failed");
    Require(writer.Integer("SELECT COUNT(*) FROM daily_traffic;") == 2, "Rollover dates were merged");
    Require(writer.Integer("SELECT download_bytes FROM daily_traffic WHERE date='2026-12-31';") == 107,
            "Pre-midnight pending bytes moved or duplicated");
    Require(writer.Integer("SELECT download_bytes FROM daily_traffic WHERE date='2027-01-01';") == 200,
            "New-day bytes moved or duplicated");
    Require(writer.Integer("SELECT last_seen FROM app_traffic WHERE date='2026-12-31';") == 1000,
            "Retry changed original app last-seen time");
    Require(writer.Integer("SELECT download_bytes FROM app_traffic WHERE date='2027-01-01';") == 80,
            "App rollover failed");
}

void TestAtomicFailureAndRetry() {
    TempHistory temp;
    TrafficStorage storage(temp.database_path, 0);
    Connection observer(temp.database_path);
    Require(storage.SaveTrafficForDate(L"2026-05-01", 100, 10, {}), storage.GetLastError());
    observer.Exec("CREATE TRIGGER reject_app BEFORE INSERT ON app_traffic "
                  "WHEN NEW.app_name='reject.exe' BEGIN SELECT RAISE(ABORT,'synthetic app failure'); END;");
    PendingTrafficHistory pending;
    pending.AddDaily(L"2026-05-01", 20, 2);
    pending.AddApp(L"2026-05-01", {L"good.exe", 4, 1, 100});
    pending.AddApp(L"2026-05-01", {L"reject.exe", 5, 2, 100});
    Require(!pending.Flush(storage), "Synthetic row failure was ignored");
    Require(!pending.Empty(), "Atomic failure discarded pending bucket");
    Require(observer.Integer("SELECT download_bytes FROM daily_traffic;") == 100,
            "Daily bytes committed despite app failure");
    Require(observer.Integer("SELECT COUNT(*) FROM app_traffic;") == 0, "App batch partially committed");
    observer.Exec("DROP TRIGGER reject_app;");
    Require(pending.Flush(storage), storage.GetLastError());
    Require(pending.Empty(), "Retry did not acknowledge bucket");
    Require(observer.Integer("SELECT download_bytes FROM daily_traffic;") == 120, "Retry duplicated daily bytes");
    Require(observer.Integer("SELECT SUM(download_bytes) FROM app_traffic;") == 9, "Retry duplicated app bytes");
}

void TestAppTodayTotalsAndCheckedRead() {
    TempHistory temp;
    TrafficStorage storage(temp.database_path, 0);
    Connection observer(temp.database_path);
    Require(storage.SaveTrafficForDate(L"2026-06-30", 0, 0, {{L"browser.exe", 1000, 500, 100}}),
            storage.GetLastError());
    Require(storage.SaveTrafficForDate(L"2026-07-01", 0, 0, {{L"browser.exe", 100, 20, 200}}),
            storage.GetLastError());
    PendingTrafficHistory pending;
    pending.AddApp(L"2026-07-01", {L"browser.exe", 7, 3, 300});
    pending.AddApp(L"2026-07-01", {L"browser.exe", 2, 1, 301});
    Require(pending.GetAppTotals(L"2026-06-30").empty(), "Pending totals leaked across dates");
    Require(pending.GetAppTotals(L"2026-07-01").at(L"browser.exe") == 13,
            "Same-name pending app totals did not accumulate");
    std::vector<AppTrafficRecord> stored;
    Require(storage.LoadAppTrafficForDate(L"2026-07-01", stored), storage.GetLastError());
    Require(stored.size() == 1 && stored[0].rx_bytes + stored[0].tx_bytes == 120,
            "Bulk date read included another day's persisted traffic");
    Require(stored[0].rx_bytes + stored[0].tx_bytes + pending.GetAppTotals(L"2026-07-01").at(L"browser.exe") == 133,
            "Persisted plus pending today's total is incorrect");
    observer.Exec("BEGIN IMMEDIATE;");
    Require(!pending.Flush(storage), "Synthetic app totals lock did not block flush");
    Require(pending.GetAppTotals(L"2026-07-01").at(L"browser.exe") == 13,
            "Failed save removed pending contribution to today's total");
    observer.Exec("ROLLBACK;");
    Require(pending.Flush(storage), storage.GetLastError());
    Require(pending.GetAppTotals(L"2026-07-01").empty(), "Committed app bytes remain pending");
    Require(storage.LoadAppTrafficForDate(L"2026-07-01", stored), storage.GetLastError());
    Require(stored.size() == 1 && stored[0].rx_bytes + stored[0].tx_bytes == 133,
            "Flush changed today's combined total or double-counted pending bytes");
    pending.AddApp(L"2026-07-02", {L"browser.exe", 4, 1, 400});
    Require(storage.LoadAppTrafficForDate(L"2026-07-02", stored) && stored.empty(),
            "Next-day bulk read retained previous day's totals");
    Require(pending.GetAppTotals(L"2026-07-02").at(L"browser.exe") == 5,
            "Next-day pending total includes prior-day/session bytes");

    stored = {{L"last-good.exe", 11, 12, 1}};
    observer.Exec("ALTER TABLE app_traffic RENAME TO temporarily_unavailable;");
    Require(!storage.LoadAppTrafficForDate(L"2026-07-01", stored), "Broken app query reported success");
    Require(stored.size() == 1 && stored[0].app_name == L"last-good.exe" && stored[0].rx_bytes == 11,
            "Failed bulk read replaced the last good data");
    observer.Exec("ALTER TABLE temporarily_unavailable RENAME TO app_traffic;");
    Require(storage.LoadAppTrafficForDate(L"2026-07-01", stored) && stored.size() == 1 &&
            stored[0].app_name == L"browser.exe", "Bulk read did not recover after failure");
}

void TestFailedOpenCanRetry() {
    TempHistory temp;
    Require(std::filesystem::create_directory(temp.database_path), "Create synthetic path obstruction failed");
    TrafficStorage storage(temp.database_path, 0);
    Require(!storage.GetLastError().empty(), "Failed open did not report an error");
    Require(!storage.SaveTrafficForDate(L"2026-06-01", 1, 2, {}), "Directory opened as database");
    Require(std::filesystem::remove(temp.database_path), "Remove synthetic obstruction failed");
    Require(storage.SaveTrafficForDate(L"2026-06-01", 1, 2, {}), storage.GetLastError());
    Connection observer(temp.database_path);
    Require(observer.Integer("SELECT download_bytes FROM daily_traffic;") == 1, "Failed handle prevented retry");
}

} // namespace

int main() {
    try {
        TestTauriSchema();
        TestNativeSchemaMigration();
        TestFreshSchema();
        TestLockRetryAndRollover();
        TestAtomicFailureAndRetry();
        TestFailedOpenCanRetry();
        TestAppTodayTotalsAndCheckedRead();
        std::cout << "HistoryStorage: 7 synthetic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HistoryStorage failure: " << error.what() << '\n';
        return 1;
    }
}
