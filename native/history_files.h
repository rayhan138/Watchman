#pragma once
#include <filesystem>
#include <stdexcept>

// sqlite3_open would accept a zero-length file as a fresh DB. Only a genuinely
// absent database (without recovery files) may start fresh.
inline void CheckHistoryFiles(const std::filesystem::path& db) {
    namespace fs = std::filesystem;
    if (fs::exists(db)) {
        if (!fs::is_regular_file(db) || fs::file_size(db) == 0)
            throw std::runtime_error("Existing history database is empty or not a regular file; retained for recovery.");
        return;
    }
    for (auto suffix : {L"-wal", L"-shm", L"-journal"}) {
        if (fs::exists(fs::path(db).concat(suffix)))
            throw std::runtime_error("History sidecars exist without their database; retained for recovery.");
    }
}
