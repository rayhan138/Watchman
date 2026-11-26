// Standalone synthetic history tests. No default TrafficStorage constructor,
// engine/services, APPDATA resolution, real history, or Downloads access.
#include "storage.h"
#include "sqlite3.h"

#include <filesystem>
#include <fstream>
#include <iterator>
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

std::string FileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void TestExistingNativeProfileContinuity() {
    TempHistory temp;
    const auto legacy_path = temp.directory / L"Watchman" / L"watchman.db";
    const auto native_path = temp.directory / L"Watchman.Native" / L"watchman.db";
    std::filesystem::create_directories(legacy_path.parent_path());
    std::filesystem::create_directories(native_path.parent_path() / L"original-v1");
    {
        TrafficStorage legacy(legacy_path.wstring(), 0);
        Require(legacy.SaveTrafficForDate(L"2026-11-18", 100, 20, {}), legacy.GetLastError());
        Require(legacy.SaveTrafficForDate(L"2026-11-24", 7, 3, {{L"legacy-only.exe", 4, 1, 42}}), legacy.GetLastError());
    }
    const auto legacy_before = FileBytes(legacy_path);
    const auto backup_path = native_path.parent_path() / L"original-v1" / L"watchman.db";
    const auto json_path = native_path.parent_path() / L"history.json";
    std::ofstream(backup_path, std::ios::binary) << "Do not restore this archived database";
    std::ofstream(json_path, std::ios::binary) << "{\"records\":{\"2026-11-24\":{\"download\":999999999}}}";
    std::ofstream(native_path.parent_path() / L"migration-v2.complete") << "2.0.0\n";
    const auto backup_before = FileBytes(backup_path), json_before = FileBytes(json_path);
    Connection native(native_path.wstring());
    native.Exec("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE daily_traffic(date TEXT PRIMARY KEY,upload_bytes INTEGER NOT NULL DEFAULT 0,"
        "download_bytes INTEGER NOT NULL DEFAULT 0,updated_at TEXT NOT NULL);"
        "INSERT INTO daily_traffic VALUES('2026-11-18',20,100,'preserve-18');"
        "INSERT INTO daily_traffic VALUES('2026-11-21',30,200,'preserve-21');"
        "INSERT INTO daily_traffic VALUES('2026-11-22',40,300,'preserve-22');"
        "INSERT INTO daily_traffic VALUES('2026-11-23',50,400,'preserve-23');"
        "INSERT INTO daily_traffic VALUES('2026-11-24',60,500,'preserve-24');");
    Require(std::filesystem::exists(native_path.wstring() + L"-wal"), "Fixture did not retain committed WAL data");
    const auto selection = WatchmanHistory::Select(temp.directory);
    Require(selection.error.empty() && selection.profile == WatchmanHistory::Profile::Native,
            "Native profile was not selected");
    {
        TrafficStorage storage(selection, 0);
        Require(storage.GetLastError().empty(), storage.GetLastError());
        Require(storage.GetDatabaseFilePath() == native_path.wstring(), "Wrong database was opened");
        std::vector<TrafficRecord> rows;
        Require(storage.LoadHistory(rows) && rows.size() == 5 && rows.back().rx_bytes == 500,
                "Native days/committed WAL were lost or legacy/JSON data was merged");
        Require(storage.SaveTrafficForDate(L"2026-11-24", 9, 2, {{L"native-only.exe", 6, 1, 99}}), storage.GetLastError());
        Require(native.Integer("SELECT download_bytes FROM daily_traffic WHERE date='2026-11-24'") == 509,
                "New samples were not written to the selected native profile");
        Require(native.Integer("SELECT COUNT(*) FROM app_traffic WHERE app_name='legacy-only.exe'") == 0,
                "The independent legacy session was silently merged");
    }
    {
        TrafficStorage reopened(WatchmanHistory::Select(temp.directory), 0);
        const auto rows = reopened.LoadHistory();
        Require(rows.size() == 5 && rows.back().rx_bytes == 509, "Reopen changed or duplicated native history");
        Require(native.Text("SELECT updated_at FROM daily_traffic WHERE date='2026-11-21'") == "preserve-21",
                "Untouched native historical metadata changed");
    }
    Require(FileBytes(legacy_path) == legacy_before && FileBytes(backup_path) == backup_before &&
            FileBytes(json_path) == json_before, "Independent history or archival backup was overwritten");
}

void TestNativeFailureDoesNotFallBack() {
    TempHistory temp;
    const auto legacy_path = temp.directory / L"Watchman" / L"watchman.db";
    const auto native_path = temp.directory / L"Watchman.Native" / L"watchman.db";
    std::filesystem::create_directories(legacy_path.parent_path());
    std::filesystem::create_directories(native_path.parent_path());
    { TrafficStorage legacy(legacy_path.wstring(), 0);
      Require(legacy.SaveTrafficForDate(L"2026-11-24", 7, 3, {}), legacy.GetLastError()); }
    const auto legacy_before = FileBytes(legacy_path);
    for (const auto* contents : {"", "not a SQLite database"}) {
        std::ofstream(native_path, std::ios::binary | std::ios::trunc) << contents;
        const auto damaged_before = FileBytes(native_path);
        TrafficStorage storage(WatchmanHistory::Select(temp.directory), 0);
        Require(!storage.GetLastError().empty() && !storage.SaveTrafficForDate(L"2026-11-24", 1, 1, {}),
                "Damaged native profile was replaced or redirected to legacy");
        Require(FileBytes(legacy_path) == legacy_before && FileBytes(native_path) == damaged_before,
                "Failure changed independent or damaged history");
    }
    std::filesystem::remove(native_path);
    TrafficStorage missing(WatchmanHistory::Select(temp.directory), 0);
    Require(!missing.SaveTrafficForDate(L"2026-11-24", 1, 1, {}) && !std::filesystem::exists(native_path),
            "A missing native DB was recreated empty");
    Require(FileBytes(legacy_path) == legacy_before, "Missing native DB redirected deltas to legacy");
    for (const auto* suffix : {L"-wal", L"-shm", L"-journal"}) {
        const auto sidecar = std::filesystem::path(native_path.wstring() + suffix);
        std::ofstream(sidecar, std::ios::binary) << "orphaned native recovery data";
        const auto sidecar_before = FileBytes(sidecar);
        TrafficStorage orphaned(WatchmanHistory::Select(temp.directory), 0);
        Require(!orphaned.SaveTrafficForDate(L"2026-11-24", 1, 1, {}) &&
                !std::filesystem::exists(native_path) && FileBytes(sidecar) == sidecar_before,
                "An orphaned native SQLite sidecar was overwritten or redirected to legacy");
        Require(FileBytes(legacy_path) == legacy_before, "Orphaned native sidecar redirected deltas to legacy");
        std::filesystem::remove(sidecar);
    }
}

void TestCheckedHistoryRetainsLastGood() {
    TempHistory temp;
    TrafficStorage storage(temp.database_path, 0);
    Require(storage.SaveTrafficForDate(L"2026-11-24", 100, 20, {}), storage.GetLastError());
    std::vector<TrafficRecord> rows;
    Require(storage.LoadHistory(rows) && rows.size() == 1, "Checked initial read failed");
    Connection writer(temp.database_path);
    writer.Exec("ALTER TABLE daily_traffic RENAME TO unavailable;");
    Require(!storage.LoadHistory(rows) && !storage.GetLastError().empty() &&
            rows.size() == 1 && rows[0].rx_bytes == 100, "Failed query erased last good history");
    writer.Exec("ALTER TABLE unavailable RENAME TO daily_traffic;"
                "INSERT INTO daily_traffic VALUES('2026-11-25',-1,0,'bad');");
    Require(!storage.LoadHistory(rows) && rows.size() == 1 && rows[0].rx_bytes == 100,
            "Invalid later row published a partial/unsigned history");
    writer.Exec("DELETE FROM daily_traffic WHERE date='2026-11-25';");
    Require(storage.LoadHistory(rows) && storage.GetLastError().empty() && rows.size() == 1,
            "Checked history did not recover after repair");
}

void TestExportPresentationPreservesStoredBytes() {
    TempHistory temp;
    TrafficStorage storage(temp.database_path, 0);
    constexpr uint64_t mb = 1024ULL * 1024, gb = mb * 1024;
    Require(storage.SaveTrafficForDate(L"2024-01-01", 15 * mb, 512 * 1024, {}), storage.GetLastError());
    Require(storage.SaveTrafficForDate(L"2024-01-08", 2 * gb, gb, {}), storage.GetLastError());
    Require(storage.SaveTrafficForDate(L"2024-02-29", mb, 0, {}), storage.GetLastError());
    Connection observer(temp.database_path);
    const auto download = observer.Integer("SELECT SUM(download_bytes) FROM daily_traffic;");
    const auto upload = observer.Integer("SELECT SUM(upload_bytes) FROM daily_traffic;");
    const auto timestamp = observer.Text("SELECT updated_at FROM daily_traffic WHERE date='2024-01-01';");
    std::vector<TrafficRecord> records;
    Require(storage.LoadHistory(records), storage.GetLastError());
    for (auto period : {TrafficExportPeriod::Daily, TrafficExportPeriod::Weekly, TrafficExportPeriod::Monthly, TrafficExportPeriod::Yearly}) {
        const auto csv = WatchmanExport::BuildCsv(records, {period, L"2024-01", 2024});
        Require(csv.error.empty() && csv.utf8.substr(0, 3) == "\xEF\xBB\xBF" &&
            csv.utf8.find("Summary\r\n") != std::string::npos && csv.utf8.find("Bytes") == std::string::npos,
            "Real storage records do not produce the new presentation contract");
    }
    Require(observer.Integer("SELECT SUM(download_bytes) FROM daily_traffic;") == download &&
            observer.Integer("SELECT SUM(upload_bytes) FROM daily_traffic;") == upload &&
            observer.Text("SELECT updated_at FROM daily_traffic WHERE date='2024-01-01';") == timestamp &&
            observer.Integer("SELECT COUNT(*) FROM daily_traffic;") == 3,
            "CSV presentation must never convert or rewrite persisted history");
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
        TestExistingNativeProfileContinuity();
        TestNativeFailureDoesNotFallBack();
        TestCheckedHistoryRetainsLastGood();
        TestExportPresentationPreservesStoredBytes();
        std::cout << "HistoryStorage: 11 synthetic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HistoryStorage failure: " << error.what() << '\n';
        return 1;
    }
}
