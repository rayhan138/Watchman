#include "legacy_migration.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include "history_files.h"
#include "sqlite3.h"
#include "scoped_handle.h"
#include "third_party/nlohmann/json.hpp"

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::string_literals;
void require(bool ok, const char *reason)
{
    if (!ok)
        throw std::runtime_error(reason);
}

// No junction traversal when making a backup. Never open original DBs with SQLite.
void copy_tree(const fs::path &from, const fs::path &to)
{
    auto attrs = GetFileAttributesW(from.c_str());
    require(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT),
            "Cannot safely snapshot profile (link or unreadable file)");
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    {
        fs::create_directory(to);
        for (const auto &entry : fs::directory_iterator(from))
            copy_tree(entry.path(), to / entry.path().filename());
    }
    else
        fs::copy_file(from, to);
}
Json read_json(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return Json::parse(f);
}

void migrate_settings(const Json &s)
{
    require(s.is_object(), "Invalid settings.json");
    HKEY raw;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Watchman", 0, nullptr, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &raw, nullptr) == ERROR_SUCCESS,
            "Cannot open user settings");
    std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(raw, RegCloseKey);
    auto set = [&](const wchar_t *name, DWORD type, const void *data, DWORD bytes) {
        // Native preferences explicitly set by this user take precedence.
        auto rc = RegQueryValueExW(raw, name, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_SUCCESS)
            return;
        require(rc == ERROR_FILE_NOT_FOUND &&
                    RegSetValueExW(raw, name, 0, type, (const BYTE *)data, bytes) == ERROR_SUCCESS,
                "Cannot save native settings");
    };
    auto dw = [&](const wchar_t *k, DWORD n) { set(k, REG_DWORD, &n, sizeof(n)); };
    auto flag = [&](const Json &j, const char *k, const wchar_t *dest) {
        if (j.contains(k) && j[k].is_boolean())
            dw(dest, j[k].get<bool>());
    };
    auto number = [&](const Json &j, const char *k, const wchar_t *dest, unsigned lo, unsigned hi) {
        if (j.contains(k) && j[k].is_number_unsigned())
        {
            auto n = j[k].get<uint64_t>();
            if (n >= lo && n <= hi)
                dw(dest, static_cast<DWORD>(n));
        }
    };
    flag(s, "startOnBoot", L"Autostart");
    flag(s, "unitModeBits", L"UnitModeBits");
    flag(s, "hideGauges", L"HideHardwareGauges");
    if (s.contains("theme"))
    {
        for (auto [name, id] : {std::pair{"dark", 0}, {"light", 1}, {"system", 2}})
            if (s["theme"] == name)
                dw(L"Theme", id);
    }
    if (s.contains("dataLimit") && s["dataLimit"].is_number_unsigned())
    {
        uint64_t n = s.value("dataLimitEnabled", true) ? s["dataLimit"].get<uint64_t>() : 0;
        set(L"DataLimitBytes", REG_QWORD, &n, sizeof(n));
    }
    auto widget = s.value("widgetSettings", Json::object());
    if (widget.contains("mode"))
        for (auto [name, id] : {std::pair{"full", 0}, {"network-only", 1}, {"system-only", 2}})
            if (widget["mode"] == name)
                dw(L"WidgetMode", id);
    auto notifications = s.value("notifications", Json::object());
    if (notifications.contains("enabled") || notifications.contains("highUsageWarnings"))
        dw(L"Alerts", notifications.value("enabled", true) && notifications.value("highUsageWarnings", true));
    auto w = notifications.value("warningSettings", Json::object());
    for (auto [src, dst] : {std::pair{"traffic", L"Traffic"},
                            {"memory", L"Memory"},
                            {"cpuTemp", L"CpuTemp"},
                            {"gpuTemp", L"GpuTemp"},
                            {"diskTemp", L"DiskTemp"},
                            {"mainboardTemp", L"MainboardTemp"}})
    {
        auto base = L"Warning"s + dst;
        flag(w, (src + "Enabled"s).c_str(), (base + L"Enabled").c_str());
        bool traffic = "traffic"s == src, memory = "memory"s == src;
        number(w, (src + "Threshold"s).c_str(), (base + L"Threshold").c_str(), traffic || memory ? 0 : 1,
               traffic  ? INT_MAX
               : memory ? 100
                        : 150);
    }
    flag(s, "memoryWarningEnabled", L"WarningMemoryEnabled");
    number(s, "memoryWarningThreshold", L"WarningMemoryThreshold", 0, 100);
    if (w.contains("trafficUnit") && (w["trafficUnit"] == "GB" || w["trafficUnit"] == "MB"))
        dw(L"WarningTrafficGB", w["trafficUnit"] == "GB");
}
void prepare_history(const fs::path &stage, const fs::path &backup)
{
    // Both v1 and older C++ builds stored SQLite here. History JSON in the
    // snapshot is archival only; never interpret it or reconcile its rows.
    CheckHistoryFiles(backup / L"watchman.db");
    for (auto name : {L"watchman.db", L"watchman.db-wal", L"watchman.db-shm", L"watchman.db-journal"})
        if (fs::exists(backup / name))
            fs::copy_file(backup / name, stage / name);
    sqlite3 *raw = nullptr;
    auto rc = sqlite3_open16((stage / L"watchman.db").c_str(), &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    require(rc == SQLITE_OK, "Cannot open native history copy");
    auto exec = [&](const char *sql) {
        require(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK,
                "Cannot migrate history schema");
    };
    auto stmt = [&](const char *sql) {
        sqlite3_stmt *p = nullptr;
        require(sqlite3_prepare_v2(raw, sql, -1, &p, nullptr) == SQLITE_OK, "Invalid history schema");
        return std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(p, sqlite3_finalize);
    };
    exec("PRAGMA trusted_schema=OFF");
    {
        auto check = stmt("PRAGMA integrity_check");
        require(sqlite3_step(check.get()) == SQLITE_ROW &&
                    std::string(reinterpret_cast<const char *>(sqlite3_column_text(check.get(), 0))) == "ok",
                "Damaged SQLite history");
    }
    if (!fs::exists(backup / L"watchman.db"))
        exec("CREATE TABLE daily_traffic(date TEXT PRIMARY KEY,upload_bytes INTEGER NOT NULL DEFAULT 0,"
             "download_bytes INTEGER NOT NULL DEFAULT 0,updated_at TEXT NOT NULL)");
    // For an existing database leave all tables/rows to TrafficStorage's
    // normal additive schema checks. Never rebuild from a different source.
}
} // namespace
void PrepareNativeProfile()
{
    wchar_t buf[32768];
    auto len = GetEnvironmentVariableW(L"APPDATA", buf, 32768);
    require(len && len < 32768, "APPDATA unavailable");
    fs::path parent = buf, legacy = parent / L"Watchman", output = parent / L"Watchman.Native";
    if (fs::is_regular_file(output / L"migration-v2.complete"))
        return;
    require(!fs::exists(output), "Existing native directory lacks migration marker");
    GUID id{};
    require(SUCCEEDED(CoCreateGuid(&id)), "Cannot create staging directory");
    wchar_t guid[40];
    StringFromGUID2(id, guid, 40);
    auto stage = parent / (L"Watchman.Migrating-"s + guid), backup = stage / L"original-v1";
    require(fs::create_directory(stage), "Cannot stage native profile");
    // Keep interrupted/failed staging copies for inspection. Never replace originals.
    if (fs::exists(legacy))
    {
        // Deny writes/deletion until the full DB/WAL snapshot has finished. An open
        // writer makes CreateFile fail; never quietly copy a live, inconsistent DB.
        std::vector<Watchman::ScopedHandle> locks;
        for (auto name : {L"watchman.db", L"watchman.db-wal", L"watchman.db-shm", L"watchman.db-journal",
                          L"settings.json"})
        {
            if (fs::exists(legacy / name))
            {
                locks.emplace_back(CreateFileW((legacy / name).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                               nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                               nullptr));
                require(locks.back().IsValid(),
                        "Legacy history/settings are in use. Close Watchman before retrying.");
            }
        }
        copy_tree(legacy, backup);
    }
    prepare_history(stage, backup);
    if (fs::exists(backup / L"settings.json"))
        migrate_settings(read_json(backup / L"settings.json"));
    std::ofstream marker(stage / L"migration-v2.complete");
    marker << "2.0.0\n";
    marker.close();
    require(marker.good(), "Cannot commit migration marker");
    fs::rename(stage, output);
}
