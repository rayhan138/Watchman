// Pure filesystem policy: isolated fixtures only, no APPDATA or SQLite access.
#include "history_profile.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        root = std::filesystem::temp_directory_path() /
            ("watchman-profile-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Check(std::filesystem::create_directory(root), "Cannot create isolated profile fixture");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void File(const std::filesystem::path& relative, const char* content = "unchanged fixture") {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
    }
};
} // namespace

int main() {
    using namespace WatchmanHistory;
    try {
        Check(!Select({}).error.empty(), "An unavailable profile must not use the current directory");
        {
            Fixture f;
            const auto fresh = Select(f.root);
            Check(fresh.error.empty() && fresh.profile == Profile::Legacy && fresh.allow_create &&
                fresh.database == f.root / "Watchman/watchman.db", "Fresh standalone path changed");
            Check(std::filesystem::is_empty(f.root), "Selection created data/directories");
            f.File("Watchman/history.json", "archival JSON must not be imported");
            Check(Select(f.root).allow_create, "JSON-only legacy must not be treated as a SQLite import");
            f.File("Watchman/watchman.db");
            auto legacy = Select(f.root);
            Check(legacy.error.empty() && legacy.profile == Profile::Legacy && !legacy.allow_create,
                "An existing legacy DB must be reopened, not recreated");
            f.File("Watchman.Native/watchman.db");
            f.File("Watchman.Native/migration-v2.complete", "2.0.0\n");
            f.File("Watchman.Native/original-v1/watchman.db", "archival DB is never selected");
            const auto native = Select(f.root);
            Check(native.error.empty() && native.profile == Profile::Native && !native.allow_create &&
                native.database == f.root / "Watchman.Native/watchman.db" && native.separate_legacy_history,
                "Existing native DB must win over the stale legacy DB");
            f.File("Watchman/watchman.db", "larger and newer LEGACY data must not be merged or preferred");
            for (int repeat = 0; repeat < 5; ++repeat)
                Check(Select(f.root).database == native.database, "Selection used size, recency, or an archival backup");
            Check(Description(native).find(L"not merged") != std::wstring::npos,
                "Separate legacy history needs a non-merge explanation");
            std::filesystem::remove(f.root / "Watchman.Native/watchman.db");
            auto missing = Select(f.root);
            Check(!missing.error.empty() && missing.profile == Profile::Native && !missing.allow_create &&
                missing.database == native.database, "Missing native DB must not silently fall back to legacy/backup");
            f.File("Watchman.Native/watchman.db", "");
            Check(!Select(f.root).error.empty(), "Zero-length native history must be retained, not initialized");
            std::filesystem::remove(f.root / "Watchman.Native/watchman.db");
            for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
                const std::string sidecar = std::string("Watchman.Native/watchman.db") + suffix;
                f.File(sidecar);
                const auto rejected = Select(f.root);
                Check(!rejected.error.empty() && rejected.profile == Profile::Native && !rejected.allow_create,
                    "Orphaned native SQLite sidecars must not cause an empty replacement/fallback");
                std::filesystem::remove(f.root / sidecar);
            }
        }
        {
            Fixture f;
            std::filesystem::create_directory(f.root / "Watchman.Native");
            Check(!Select(f.root).error.empty(), "An incomplete native directory must not be repurposed");
            std::filesystem::remove(f.root / "Watchman.Native");
            f.File("Watchman.Native", "not a directory");
            Check(!Select(f.root).error.empty(), "An obstructed native directory must not fall back");
        }
        for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
            Fixture f;
            f.File(std::string("Watchman/watchman.db") + suffix);
            Check(!Select(f.root).error.empty(), "Legacy recovery files must not be overwritten by a new DB");
        }
        std::cout << "HistoryProfile checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HistoryProfile check " << checks << " failed: " << error.what() << '\n';
        return 1;
    }
}
