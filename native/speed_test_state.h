#pragma once

#include "network_test_protocol.h"
#include <cstdint>
#include <string>

struct SpeedTestResult {
    double download_speed_mbps = 0.0;
    double upload_speed_mbps = 0.0;
    double latency_ms = 0.0;
    double jitter_ms = 0.0;
    std::wstring server_name;
    std::wstring server_city;
    std::wstring error_message;
    std::wstring warning_message;
    network_test::TransferState download_state = network_test::TransferState::NotRun;
    network_test::TransferState upload_state = network_test::TransferState::NotRun;
    bool upload_server_confirmed = false;
    uint64_t completed_at_unix_seconds = 0;
};

enum class SpeedTestPhase { Idle, Locating, Download, PreparingUpload, Upload, Finishing, Cancelling, Completed, Cancelled, Failed };

inline const char* SpeedTestPhaseName(SpeedTestPhase phase) {
    switch (phase) {
    case SpeedTestPhase::Idle: return "idle";
    case SpeedTestPhase::Locating: return "locating";
    case SpeedTestPhase::Download: return "download";
    case SpeedTestPhase::PreparingUpload: return "preparing-upload";
    case SpeedTestPhase::Upload: return "upload";
    case SpeedTestPhase::Finishing: return "finishing";
    case SpeedTestPhase::Cancelling: return "cancelling";
    case SpeedTestPhase::Completed: return "completed";
    case SpeedTestPhase::Cancelled: return "cancelled";
    case SpeedTestPhase::Failed: return "failed";
    }
    return "unknown";
}
inline bool SpeedTestBusy(SpeedTestPhase phase) {
    return phase == SpeedTestPhase::Locating || phase == SpeedTestPhase::Download ||
        phase == SpeedTestPhase::PreparingUpload || phase == SpeedTestPhase::Upload ||
        phase == SpeedTestPhase::Finishing || phase == SpeedTestPhase::Cancelling;
}

// One coherent UI publication. Phase must never be inferred from a percentage:
// upload is still active at 90-95%, and locating is not a zero-speed download.
struct SpeedTestSnapshot {
    bool available = true;
    SpeedTestResult result;
    SpeedTestPhase phase = SpeedTestPhase::Idle;
    int progress = 0;
    double live_mbps = 0.0;
    std::wstring status;
    bool Running() const { return SpeedTestBusy(phase); }
};
