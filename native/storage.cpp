#include "storage.h"
#include "history_files.h"
#include "sqlite3.h"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <memory>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <shlobj.h>
#include <sstream>
#include <string_view>

namespace {

constexpr uint64_t kBytesPerMegabyte = 1024ULL * 1024ULL;

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) return 0;
    return month == 2 && IsLeapYear(year) ? 29 : kDays[month - 1];
}

bool ParseDate(const std::wstring& value, int& year, int& month, int& day) {
    if (value.size() != 10 || value[4] != L'-' || value[7] != L'-') {
        return false;
    }
    const auto parse_digits = [](std::wstring_view text, int& output) {
        int parsed = 0;
        for (const wchar_t ch : text) {
            if (ch < L'0' || ch > L'9') return false;
            parsed = parsed * 10 + (ch - L'0');
        }
        output = parsed;
        return true;
    };
    if (!parse_digits(std::wstring_view(value).substr(0, 4), year) ||
        !parse_digits(std::wstring_view(value).substr(5, 2), month) ||
        !parse_digits(std::wstring_view(value).substr(8, 2), day)) {
        return false;
    }
    return month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month);
}

bool IsValidMonthKey(const std::wstring& value) {
    int year = 0;
    int month = 0;
    int day = 0;
    return value.size() == 7 &&
        ParseDate(value + L"-01", year, month, day);
}

int DayOfWeekSundayZero(int year, int month, int day) {
    static constexpr int kMonthOffsets[] = {
        0, 3, 2, 5, 0, 3,
        5, 1, 4, 6, 2, 4,
    };
    if (month < 3) --year;
    return (year + year / 4 - year / 100 + year / 400 + kMonthOffsets[month - 1] + day) % 7;
}

int DayOfYear(int year, int month, int day) {
    int ordinal = day;
    for (int current_month = 1; current_month < month; ++current_month) {
        ordinal += DaysInMonth(year, current_month);
    }
    return ordinal;
}

int WeeksInIsoYear(int year) {
    const int jan_first = DayOfWeekSundayZero(year, 1, 1);
    return jan_first == 4 || (jan_first == 3 && IsLeapYear(year)) ? 53 : 52;
}

std::wstring GetIsoWeekKey(int year, int month, int day) {
    const int sunday_zero = DayOfWeekSundayZero(year, month, day);
    const int iso_weekday = sunday_zero == 0 ? 7 : sunday_zero;
    const int ordinal = DayOfYear(year, month, day);
    int week = (ordinal - iso_weekday + 10) / 7;
    int iso_year = year;
    if (week < 1) {
        iso_year = year - 1;
        week = WeeksInIsoYear(iso_year);
    } else if (week > WeeksInIsoYear(year)) {
        iso_year = year + 1;
        week = 1;
    }

    wchar_t key[16]{};
    swprintf_s(key, L"%04d-W%02d", iso_year, week);
    return key;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
    return std::numeric_limits<uint64_t>::max() - left < right
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
}

bool MatchesExportScope(
    const std::wstring& date,
    const TrafficExportOptions& options,
    int year
) {
    switch (options.period) {
    case TrafficExportPeriod::Daily:
    case TrafficExportPeriod::Weekly:
        return date.rfind(options.month, 0) == 0;
    case TrafficExportPeriod::Monthly:
    case TrafficExportPeriod::Yearly:
        return year == options.year;
    }
    return false;
}

std::wstring GetPeriodKey(
    const std::wstring& date,
    const TrafficExportOptions& options,
    int year,
    int month,
    int day
) {
    switch (options.period) {
    case TrafficExportPeriod::Daily:
        return date;
    case TrafficExportPeriod::Weekly:
        return GetIsoWeekKey(year, month, day);
    case TrafficExportPeriod::Monthly:
        return date.substr(0, 7);
    case TrafficExportPeriod::Yearly:
        return date.substr(0, 4);
    }
    return {};
}

const wchar_t* ExportPeriodName(TrafficExportPeriod period) {
    switch (period) {
    case TrafficExportPeriod::Daily: return L"daily";
    case TrafficExportPeriod::Weekly: return L"weekly";
    case TrafficExportPeriod::Monthly: return L"monthly";
    case TrafficExportPeriod::Yearly: return L"yearly";
    }
    return L"report";
}

std::wstring ExportScopeLabel(const TrafficExportOptions& options) {
    if (options.period == TrafficExportPeriod::Daily || options.period == TrafficExportPeriod::Weekly) {
        return options.month;
    }
    wchar_t year[8]{};
    swprintf_s(year, L"%04d", options.year);
    return year;
}

std::wstring GetDownloadsDirectory() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) && path != nullptr) {
        const std::wstring downloads(path);
        CoTaskMemFree(path);
        return downloads;
    }
    if (path != nullptr) {
        CoTaskMemFree(path);
    }
    return L".";
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring EscapeCsvField(const std::wstring& value) {
    if (value.find_first_of(L",\"\r\n") == std::wstring::npos) {
        return value;
    }
    std::wstring escaped = L"\"";
    for (const wchar_t character : value) {
        if (character == L'\"') escaped += L'\"';
        escaped += character;
    }
    escaped += L"\"";
    return escaped;
}

} // namespace



TrafficStorage::TrafficStorage() : TrafficStorage(GetDatabasePath()) {}

TrafficStorage::TrafficStorage(const std::wstring& database_path, int busy_timeout_ms)
    : database_path_(database_path), busy_timeout_ms_(busy_timeout_ms) {
    OpenDatabase();
}

TrafficStorage::~TrafficStorage() {
    CloseDatabase();
}

std::wstring TrafficStorage::GetDatabasePath() {
    const DWORD size = ExpandEnvironmentStringsW(L"%APPDATA%\\Watchman.Native\\watchman.db", nullptr, 0);
    if (size == 0) return {};
    std::wstring path(size, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(
        L"%APPDATA%\\Watchman.Native\\watchman.db", path.data(), size);
    if (written == 0 || written > size || path.find(L"%APPDATA%") != std::wstring::npos) return {};
    path.resize(written - 1);
    return path;
}

std::wstring TrafficStorage::GetCurrentDateString() {
    time_t now = time(nullptr);
    tm local_tm;
    localtime_s(&local_tm, &now);

    wchar_t buf[32];
    wcsftime(buf, 32, L"%Y-%m-%d", &local_tm);
    return std::wstring(buf);
}

bool TrafficStorage::CheckResult(sqlite3* db, int result, int expected, const char* operation) {
    if (result == expected) return true;
    last_error_ = std::string(operation) + ": " + (db ? sqlite3_errmsg(db) : sqlite3_errstr(result));
    OutputDebugStringA(("Watchman history: " + last_error_ + "\n").c_str());
    return false;
}

bool TrafficStorage::ExecuteSql(sqlite3* db, const char* sql) {
    return CheckResult(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK, sql);
}

bool TrafficStorage::EnsureColumn(sqlite3* db, const char* table, const char* column, const char* alter_sql) {
    const std::string sql = std::string("PRAGMA table_info(") + table + ");";
    sqlite3_stmt* raw = nullptr;
    if (!CheckResult(db, sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr),
                     SQLITE_OK, "Read history schema")) return false;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    bool found = false;
    int result;
    while ((result = sqlite3_step(raw)) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(raw, 1));
        if (name && std::string_view(name) == column) found = true;
    }
    if (!CheckResult(db, result, SQLITE_DONE, "Read history schema")) return false;
    statement.reset();
    return found || ExecuteSql(db, alter_sql);
}

bool TrafficStorage::OpenDatabase() {
    if (db_) return true;
    if (database_path_.empty()) {
        last_error_ = "History database path is unavailable.";
        return false;
    }
    std::error_code error;
    const auto parent = std::filesystem::path(database_path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) {
        last_error_ = "Create history directory: " + error.message();
        return false;
    }

    try { CheckHistoryFiles(database_path_); }
    catch (const std::exception& e) { last_error_ = e.what(); return false; }
    sqlite3* candidate = nullptr;
    const int result = sqlite3_open16(database_path_.c_str(), &candidate);
    if (!CheckResult(candidate, result, SQLITE_OK, "Open history database")) {
        if (candidate) sqlite3_close_v2(candidate);
        return false;
    }
    // Do not publish a handle until every initialization step has succeeded.
    const auto initialize = [&]() {
        return CheckResult(candidate, sqlite3_busy_timeout(candidate, busy_timeout_ms_),
                           SQLITE_OK, "Set history busy timeout") &&
            ExecuteSql(candidate, "PRAGMA journal_mode=WAL;") &&
            ExecuteSql(candidate, "PRAGMA synchronous=NORMAL;") &&
            ExecuteSql(candidate, "BEGIN IMMEDIATE;") &&
            ExecuteSql(candidate,
                "CREATE TABLE IF NOT EXISTS daily_traffic ("
                "date TEXT PRIMARY KEY, download_bytes INTEGER NOT NULL DEFAULT 0, "
                "upload_bytes INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL);") &&
            EnsureColumn(candidate, "daily_traffic", "updated_at",
                // A constant default permits an additive migration of populated
                // native tables. Existing tables/timestamps are never replaced.
                "ALTER TABLE daily_traffic ADD COLUMN updated_at TEXT NOT NULL "
                "DEFAULT '1970-01-01T00:00:00Z';") &&
            ExecuteSql(candidate,
                "CREATE TABLE IF NOT EXISTS app_traffic ("
                "date TEXT, app_name TEXT, download_bytes INTEGER DEFAULT 0, "
                "upload_bytes INTEGER DEFAULT 0, last_seen INTEGER DEFAULT 0, "
                "PRIMARY KEY (date, app_name));") &&
            EnsureColumn(candidate, "app_traffic", "last_seen",
                "ALTER TABLE app_traffic ADD COLUMN last_seen INTEGER DEFAULT 0;") &&
            ExecuteSql(candidate, "COMMIT;");
    };
    if (!initialize()) {
        if (!sqlite3_get_autocommit(candidate)) sqlite3_exec(candidate, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close_v2(candidate);
        return false;
    }
    db_ = candidate;
    last_error_.clear();
    return true;
}

void TrafficStorage::CloseDatabase() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

std::string TrafficStorage::GetLastError() {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    return last_error_;
}

bool TrafficStorage::SaveSessionTraffic(uint64_t delta_rx_bytes, uint64_t delta_tx_bytes) {
    return SaveTrafficForDate(GetCurrentDateString(), delta_rx_bytes, delta_tx_bytes, {});
}

bool TrafficStorage::SaveAppTraffic(const std::wstring& app_name, uint64_t delta_rx, uint64_t delta_tx) {
    return SaveAppTrafficBatch({AppTrafficRecord{app_name, delta_rx, delta_tx, 0}});
}

bool TrafficStorage::SaveAppTrafficBatch(const std::vector<AppTrafficRecord>& apps) {
    return SaveTrafficForDate(GetCurrentDateString(), 0, 0, apps);
}

bool TrafficStorage::SaveTrafficForDate(const std::wstring& date, uint64_t delta_rx,
                                      uint64_t delta_tx, const std::vector<AppTrafficRecord>& apps) {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    last_error_.clear();
    int year = 0, month = 0, day = 0;
    const uint64_t limit = static_cast<uint64_t>((std::numeric_limits<sqlite3_int64>::max)());
    if (!ParseDate(date, year, month, day) || year < 1 || delta_rx > limit || delta_tx > limit) {
        last_error_ = "Invalid history date or byte delta.";
        return false;
    }
    for (const auto& app : apps) {
        if (app.app_name.empty() || app.rx_bytes > limit || app.tx_bytes > limit || app.last_seen > limit) {
            last_error_ = "Invalid application history delta.";
            return false;
        }
    }
    if (delta_rx == 0 && delta_tx == 0 && apps.empty()) return true;
    if (!OpenDatabase() || !ExecuteSql(db_, "BEGIN IMMEDIATE;")) return false;

    // All statements are finalized before COMMIT or ROLLBACK. In particular,
    // a failed app row must not commit the daily row or earlier app rows.
    const auto write = [&]() -> bool {
        const auto check = [&](int result, int expected, const char* operation) {
            return CheckResult(db_, result, expected, operation);
        };
        const auto changed = [&]() {
            if (sqlite3_changes(db_) == 1) return true;
            last_error_ = "History total is invalid or exceeds SQLite's integer range.";
            return false;
        };
        if (delta_rx != 0 || delta_tx != 0) {
            constexpr char sql[] =
                "INSERT INTO daily_traffic (date, download_bytes, upload_bytes, updated_at) "
                "VALUES (?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now')) "
                "ON CONFLICT(date) DO UPDATE SET "
                "download_bytes = download_bytes + excluded.download_bytes, "
                "upload_bytes = upload_bytes + excluded.upload_bytes, updated_at = excluded.updated_at "
                "WHERE typeof(download_bytes) = 'integer' AND typeof(upload_bytes) = 'integer' "
                "AND download_bytes BETWEEN 0 AND 9223372036854775807 - excluded.download_bytes "
                "AND upload_bytes BETWEEN 0 AND 9223372036854775807 - excluded.upload_bytes;";
            sqlite3_stmt* raw = nullptr;
            if (!check(sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr), SQLITE_OK, "Prepare daily history")) return false;
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
            if (!check(sqlite3_bind_text16(raw, 1, date.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK, "Bind history date") ||
                !check(sqlite3_bind_int64(raw, 2, static_cast<sqlite3_int64>(delta_rx)), SQLITE_OK, "Bind download delta") ||
                !check(sqlite3_bind_int64(raw, 3, static_cast<sqlite3_int64>(delta_tx)), SQLITE_OK, "Bind upload delta") ||
                !check(sqlite3_step(raw), SQLITE_DONE, "Save daily history") || !changed()) return false;
        }
        if (apps.empty()) return true;
        constexpr char sql[] =
            "INSERT INTO app_traffic (date, app_name, download_bytes, upload_bytes, last_seen) "
            "VALUES (?, ?, ?, ?, ?) ON CONFLICT(date, app_name) DO UPDATE SET "
            "download_bytes = download_bytes + excluded.download_bytes, "
            "upload_bytes = upload_bytes + excluded.upload_bytes, "
            "last_seen = MAX(COALESCE(last_seen, 0), excluded.last_seen) "
            "WHERE typeof(download_bytes) = 'integer' AND typeof(upload_bytes) = 'integer' "
            "AND download_bytes BETWEEN 0 AND 9223372036854775807 - excluded.download_bytes "
            "AND upload_bytes BETWEEN 0 AND 9223372036854775807 - excluded.upload_bytes;";
        sqlite3_stmt* raw = nullptr;
        if (!check(sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr), SQLITE_OK, "Prepare app history")) return false;
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        for (const auto& app : apps) {
            if (app.rx_bytes == 0 && app.tx_bytes == 0) continue;
            const uint64_t last_seen = app.last_seen ? app.last_seen : static_cast<uint64_t>(time(nullptr));
            if (!check(sqlite3_reset(raw), SQLITE_OK, "Reset app history statement") ||
                !check(sqlite3_bind_text16(raw, 1, date.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK, "Bind app date") ||
                !check(sqlite3_bind_text16(raw, 2, app.app_name.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK, "Bind app name") ||
                !check(sqlite3_bind_int64(raw, 3, static_cast<sqlite3_int64>(app.rx_bytes)), SQLITE_OK, "Bind app download") ||
                !check(sqlite3_bind_int64(raw, 4, static_cast<sqlite3_int64>(app.tx_bytes)), SQLITE_OK, "Bind app upload") ||
                !check(sqlite3_bind_int64(raw, 5, static_cast<sqlite3_int64>(last_seen)), SQLITE_OK, "Bind app last seen") ||
                !check(sqlite3_step(raw), SQLITE_DONE, "Save app history") || !changed()) return false;
        }
        return true;
    };
    if (!write() || !ExecuteSql(db_, "COMMIT;")) {
        if (!sqlite3_get_autocommit(db_) && sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            // Never reuse a connection with an unresolved failed transaction.
            CloseDatabase();
        }
        return false;
    }
    return true;
}

void PendingTrafficHistory::AddDaily(const std::wstring& date, uint64_t delta_rx, uint64_t delta_tx) {
    if (delta_rx == 0 && delta_tx == 0) return;
    auto& day = days_[date];
    day.rx_bytes += delta_rx;
    day.tx_bytes += delta_tx;
}

void PendingTrafficHistory::AddApp(const std::wstring& date, const AppTrafficRecord& app) {
    if (app.rx_bytes == 0 && app.tx_bytes == 0) return;
    auto& pending = days_[date].apps[app.app_name];
    pending.app_name = app.app_name;
    pending.rx_bytes += app.rx_bytes;
    pending.tx_bytes += app.tx_bytes;
    pending.last_seen = (std::max)(pending.last_seen, app.last_seen);
}

std::map<std::wstring, uint64_t> PendingTrafficHistory::GetAppTotals(const std::wstring& date) const {
    std::map<std::wstring, uint64_t> totals;
    const auto day = days_.find(date);
    if (day != days_.end()) {
        for (const auto& [name, app] : day->second.apps) {
            totals.emplace(name, SaturatingAdd(app.rx_bytes, app.tx_bytes));
        }
    }
    return totals;
}

bool PendingTrafficHistory::Flush(TrafficStorage& storage) {
    while (!days_.empty()) {
        const auto& [date, day] = *days_.begin();
        std::vector<AppTrafficRecord> apps;
        apps.reserve(day.apps.size());
        for (const auto& entry : day.apps) apps.push_back(entry.second);
        if (!storage.SaveTrafficForDate(date, day.rx_bytes, day.tx_bytes, apps)) return false;
        days_.erase(days_.begin());
    }
    return true;
}

std::vector<TrafficRecord> TrafficStorage::LoadHistory() {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    std::vector<TrafficRecord> records;
    if (!db_) OpenDatabase();
    if (!db_) return records;

    const char* select_all = "SELECT date, download_bytes, upload_bytes FROM daily_traffic ORDER BY date ASC;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, select_all, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TrafficRecord rec;
            const wchar_t* date_wstr = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 0));
            if (date_wstr) {
                rec.date = date_wstr;
            }
            rec.rx_bytes = sqlite3_column_int64(stmt, 1);
            rec.tx_bytes = sqlite3_column_int64(stmt, 2);
            records.push_back(rec);
        }
        sqlite3_finalize(stmt);
    }

    return records;
}

std::vector<TrafficRecord> TrafficStorage::LoadMonthlyHistory() {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    std::vector<TrafficRecord> records;
    if (!db_) OpenDatabase();
    if (!db_) return records;

    const char* select_monthly =
        "SELECT strftime('%Y-%m', date), "
        "SUM(download_bytes), SUM(upload_bytes) "
        "FROM daily_traffic "
        "WHERE strftime('%Y-%m', date) IS NOT NULL "
        "GROUP BY strftime('%Y-%m', date) "
        "ORDER BY strftime('%Y-%m', date) ASC;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, select_monthly, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TrafficRecord rec;
            const wchar_t* month_wstr = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(stmt, 0));
            if (month_wstr) {
                rec.date = month_wstr;
            }
            rec.rx_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
            rec.tx_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
            records.push_back(rec);
        }
        sqlite3_finalize(stmt);
    }

    return records;
}

std::vector<int> TrafficStorage::LoadAvailableHistoryYears() {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    std::vector<int> years;
    if (!db_) OpenDatabase();
    if (!db_) return years;

    const char* select_years =
        "SELECT DISTINCT CAST(strftime('%Y', date) AS INTEGER) "
        "FROM daily_traffic "
        "WHERE strftime('%Y', date) IS NOT NULL "
        "ORDER BY 1 ASC;";
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db_, select_years, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int year = sqlite3_column_int(stmt, 0);
            if (year > 0) {
                years.push_back(year);
            }
        }
        sqlite3_finalize(stmt);
    }

    return years;
}

std::vector<AppTrafficRecord> TrafficStorage::GetAppTrafficForDate(const std::wstring& date_str) {
    std::vector<AppTrafficRecord> records;
    LoadAppTrafficForDate(date_str, records);
    return records;
}

bool TrafficStorage::LoadAppTrafficForDate(const std::wstring& date_str, std::vector<AppTrafficRecord>& records) {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    if (!OpenDatabase()) return false;
    constexpr char sql[] = "SELECT app_name, download_bytes, upload_bytes, last_seen "
                           "FROM app_traffic WHERE date = ? ORDER BY download_bytes DESC;";
    sqlite3_stmt* raw = nullptr;
    if (!CheckResult(db_, sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr),
                     SQLITE_OK, "Prepare app history read")) return false;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (!CheckResult(db_, sqlite3_bind_text16(raw, 1, date_str.c_str(), -1, SQLITE_TRANSIENT),
                     SQLITE_OK, "Bind app history date")) return false;
    std::vector<AppTrafficRecord> loaded;
    int result;
    while ((result = sqlite3_step(raw)) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(raw, 0));
        const sqlite3_int64 rx = sqlite3_column_int64(raw, 1);
        const sqlite3_int64 tx = sqlite3_column_int64(raw, 2);
        const sqlite3_int64 last_seen = sqlite3_column_int64(raw, 3);
        if (!name || rx < 0 || tx < 0 || last_seen < 0) {
            last_error_ = "Application history contains invalid totals or metadata.";
            return false;
        }
        loaded.push_back({name, static_cast<uint64_t>(rx), static_cast<uint64_t>(tx),
                          static_cast<uint64_t>(last_seen)});
    }
    if (!CheckResult(db_, result, SQLITE_DONE, "Read app history")) return false;
    records = std::move(loaded);
    return true;
}

uint64_t TrafficStorage::GetAppTotalTrafficToday(const std::wstring& app_name) {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    if (!db_) OpenDatabase();
    if (!db_) return 0;

    std::wstring today = GetCurrentDateString();
    const char* select_sql = "SELECT download_bytes, upload_bytes FROM app_traffic WHERE date = ? AND app_name = ?;";
    sqlite3_stmt* stmt = nullptr;
    uint64_t total = 0;

    if (sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text16(stmt, 1, today.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text16(stmt, 2, app_name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            total = sqlite3_column_int64(stmt, 0) + sqlite3_column_int64(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }

    return total;
}

TrafficExportResult TrafficStorage::ExportTrafficCsv(const TrafficExportOptions& options) {
    TrafficExportResult result;
    const bool uses_month = options.period == TrafficExportPeriod::Daily ||
        options.period == TrafficExportPeriod::Weekly;
    if (uses_month && !IsValidMonthKey(options.month)) {
        result.error = L"Choose a month with saved history before exporting.";
        return result;
    }
    if (!uses_month && (options.year < 1970 || options.year > 9999)) {
        result.error = L"Choose a year with saved history before exporting.";
        return result;
    }

    std::map<std::wstring, std::pair<uint64_t, uint64_t>> aggregated;
    {
        // The engine updates SQLite on its worker thread while export begins
        // from the UI. Keep the report a consistent snapshot of saved data.
        std::lock_guard<std::mutex> lock(storage_mutex_);
        if (!db_) OpenDatabase();
        if (!db_) {
            result.error = L"Saved traffic history is unavailable.";
            return result;
        }

        constexpr char kSelectSql[] =
            "SELECT date, download_bytes, upload_bytes "
            "FROM daily_traffic ORDER BY date ASC;";
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, kSelectSql, -1, &statement, nullptr) != SQLITE_OK) {
            result.error = L"Saved traffic history could not be read.";
            return result;
        }

        while (sqlite3_step(statement) == SQLITE_ROW) {
            const wchar_t* raw_date = reinterpret_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
            if (raw_date == nullptr) continue;

            const std::wstring date(raw_date);
            int year = 0;
            int month = 0;
            int day = 0;
            if (!ParseDate(date, year, month, day) || !MatchesExportScope(date, options, year)) {
                continue;
            }

            const std::wstring key = GetPeriodKey(date, options, year, month, day);
            if (key.empty()) continue;

            auto& totals = aggregated[key];
            totals.first = SaturatingAdd(
                totals.first,
                static_cast<uint64_t>(sqlite3_column_int64(statement, 1))
            );
            totals.second = SaturatingAdd(
                totals.second,
                static_cast<uint64_t>(sqlite3_column_int64(statement, 2))
            );
        }
        sqlite3_finalize(statement);
    }

    if (aggregated.empty()) {
        result.error = L"No data is available for the selected export range.";
        return result;
    }

    std::wostringstream csv;
    csv << L"Period,Download (MB),Upload (MB),Total (MB),Download (Bytes),Upload (Bytes),Total (Bytes)\n";
    csv << std::fixed << std::setprecision(2);
    for (const auto& [period, totals] : aggregated) {
        const uint64_t download = totals.first;
        const uint64_t upload = totals.second;
        const uint64_t total = SaturatingAdd(download, upload);
        csv << EscapeCsvField(period) << L","
            << static_cast<double>(download) / static_cast<double>(kBytesPerMegabyte) << L","
            << static_cast<double>(upload) / static_cast<double>(kBytesPerMegabyte) << L","
            << static_cast<double>(total) / static_cast<double>(kBytesPerMegabyte) << L","
            << download << L"," << upload << L"," << total << L"\n";
    }

    time_t now = time(nullptr);
    tm local_time{};
    localtime_s(&local_time, &now);
    wchar_t timestamp[32]{};
    wcsftime(timestamp, static_cast<size_t>(std::size(timestamp)), L"%Y-%m-%d_%H-%M-%S", &local_time);

    const std::wstring filename = std::wstring(L"traffic-report-") +
        ExportPeriodName(options.period) + L"-" + ExportScopeLabel(options) + L"-" + timestamp + L".csv";
    const std::wstring filepath = GetDownloadsDirectory() + L"\\" + filename;
    const std::string utf8_csv = WideToUtf8(csv.str());
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };

    FILE* file = nullptr;
    if (_wfopen_s(&file, filepath.c_str(), L"wb") != 0 || file == nullptr) {
        result.error = L"Could not write the report to your Downloads folder.";
        return result;
    }

    const bool wrote_bom = fwrite(bom, 1, sizeof(bom), file) == sizeof(bom);
    const bool wrote_csv = utf8_csv.empty() ||
        fwrite(utf8_csv.data(), 1, utf8_csv.size(), file) == utf8_csv.size();
    const int close_result = fclose(file);
    if (!wrote_bom || !wrote_csv || close_result != 0) {
        result.error = L"The report could not be written completely.";
        return result;
    }

    result.success = true;
    result.filepath = filepath;
    return result;
}
