// No AppTracker/service is constructed: verify the exact aggregation operation
// used by production grouping, including the canonical fields the engine reads.
#include "app_tracker.h"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void MirrorsCanonical(const AppNetworkInfo& info) {
    Check(info.download_speed_kbps == info.download_speed_kib_per_sec, "Download alias diverged");
    Check(info.upload_speed_kbps == info.upload_speed_kib_per_sec, "Upload alias diverged");
    Check(info.activity_score_kbps == info.activity_score, "Activity alias diverged");
}
}
int main() {
    try {
        AppNetworkInfo first;
        first.download_speed_kib_per_sec = 125.5;
        first.upload_speed_kib_per_sec = 12.25;
        first.activity_score = 80.5;
        first.total_download_bytes = 10240;
        first.total_upload_bytes = 2048;
        first.interval_download_bytes = 1024;
        first.interval_upload_bytes = 128;
        // Deliberately different legacy values must not poison canonical rates.
        first.download_speed_kbps = first.upload_speed_kbps = first.activity_score_kbps = 999;
        AppNetworkInfo group;
        group.AddTrafficCounters(first);
        Check(group.download_speed_kib_per_sec == 125.5 && group.upload_speed_kib_per_sec == 12.25,
            "Grouping dropped the live KiB/s fields used by WatchmanEngine");
        Check(group.activity_score == 80.5, "Grouping dropped canonical activity");
        MirrorsCanonical(group);
        group.AddTrafficCounters(first);
        Check(group.download_speed_kib_per_sec == 251 && group.upload_speed_kib_per_sec == 24.5,
            "Multiple processes did not sum live rates");
        Check(group.total_download_bytes == 20480 && group.total_upload_bytes == 4096 && group.total_bytes == 24576,
            "Session bytes did not sum");
        Check(group.interval_download_bytes == 2048 && group.interval_upload_bytes == 256,
            "Interval bytes did not sum");
        MirrorsCanonical(group);
        group.AddTrafficCounters(AppNetworkInfo{});
        Check(group.download_speed_kib_per_sec == 251 && group.total_bytes == 24576,
            "An idle process changed grouped counters");
        MirrorsCanonical(group);
        std::cout << "App tracker canonical/legacy counter aggregation passed (no ETW or OS access)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
