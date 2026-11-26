#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace WatchmanHistory {
enum class Profile { Legacy, Native, Explicit };

// Selection is read-only and happens once per storage instance. Retry may
// reopen this database, but must never switch pending deltas to another one.
struct Selection {
    std::filesystem::path database;
    Profile profile = Profile::Legacy;
    bool allow_create = false;
    bool separate_legacy_history = false;
    std::string error;
};

inline bool Exists(const std::filesystem::path& path, bool& exists, std::string& error) {
    std::error_code code;
    exists = std::filesystem::exists(path, code);
    if (code) {
        error = "History profile cannot be inspected; no alternative database was opened.";
        return false;
    }
    return true;
}

// Never initialize an empty replacement for an existing profile, or discard
// recovery files. This check deliberately does not open/checkpoint a database.
inline bool ValidateExisting(const std::filesystem::path& database, bool allow_create, std::string& error) {
    bool exists = false;
    if (!Exists(database, exists, error)) return false;
    std::error_code code;
    if (exists) {
        if (!std::filesystem::is_regular_file(database, code) || code ||
            std::filesystem::file_size(database, code) == 0 || code) {
            error = "Existing history database is empty or is not a readable regular file; retained for recovery.";
            return false;
        }
        return true;
    }
    for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        auto sidecar = database;
        sidecar += suffix;
        if (!Exists(sidecar, exists, error)) return false;
        if (exists) {
            error = "History recovery files exist without their database; retained for recovery.";
            return false;
        }
    }
    if (!allow_create) {
        error = "Existing history profile has no database; no empty replacement or legacy fallback was created.";
        return false;
    }
    return true;
}

inline Selection Select(const std::filesystem::path& appdata) {
    Selection result;
    if (appdata.empty()) {
        result.error = "History profile path is unavailable.";
        return result;
    }
    const auto native_directory = appdata / "Watchman.Native";
    const auto legacy_database = appdata / "Watchman" / "watchman.db";
    result.database = native_directory / "watchman.db";
    result.profile = Profile::Native;
    bool native_exists = false;
    if (!Exists(native_directory, native_exists, result.error)) return result;
    if (native_exists) {
        result.profile = Profile::Native;
        std::error_code code;
        if (!std::filesystem::is_directory(native_directory, code) || code) {
            result.error = "Existing native history profile is not an accessible directory.";
            return result;
        }
        // A completed native profile is authoritative even when the legacy
        // copy is newer/larger or contains extra dates. Never sum/max/restore.
        if (!ValidateExisting(result.database, false, result.error)) return result;
        bool legacy_exists = false;
        std::string ignored;
        if (Exists(legacy_database, legacy_exists, ignored)) result.separate_legacy_history = legacy_exists;
        return result;
    }
    result.database = legacy_database;
    result.profile = Profile::Legacy;
    bool legacy_exists = false;
    if (!Exists(legacy_database, legacy_exists, result.error)) return result;
    result.allow_create = !legacy_exists;
    ValidateExisting(result.database, result.allow_create, result.error);
    // Preserve the established standalone path for fresh/legacy-only users.
    // Creating an unmarked Watchman.Native folder would break older v2 builds.
    return result;
}

inline std::wstring Description(const Selection& selection) {
    if (selection.profile == Profile::Explicit) return L"History: explicit database.";
    if (selection.profile == Profile::Native) {
        return std::wstring(L"History: %APPDATA%\\Watchman.Native\\watchman.db.") +
            (selection.separate_legacy_history
                ? L"\nSeparate history in %APPDATA%\\Watchman is retained, not merged. Keep both folders backed up."
                : L"");
    }
    return L"History: %APPDATA%\\Watchman\\watchman.db (legacy/standalone profile).";
}
} // namespace WatchmanHistory
