#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class DiagnosticStatus {
    Passed,
    Failed,
    Warning,
};

struct DiagnosticTestResult {
    std::wstring name;
    DiagnosticStatus status = DiagnosticStatus::Warning;
    std::wstring message;
    std::wstring details;
    std::wstring suggestion;
};

struct DiagnosticsSnapshot {
    bool is_running = false;
    bool has_run = false;
    std::wstring overall_status;
    uint64_t completed_at_unix_seconds = 0;
    uint64_t duration_ms = 0;
    std::vector<DiagnosticTestResult> tests;
};

// Runs DNS, IPv4 route/gateway, and HTTPS probes on a worker thread. These
// observations do not prove general Internet availability. Synchronous DNS and
// WinHTTP calls have no overall cancellation deadline; destruction may block.
class DiagnosticsRunner {
public:
    DiagnosticsRunner() = default;
    ~DiagnosticsRunner();

    DiagnosticsRunner(const DiagnosticsRunner&) = delete;
    DiagnosticsRunner& operator=(const DiagnosticsRunner&) = delete;

    bool Start();
    bool IsRunning() const { return is_running_.load(); }
    DiagnosticsSnapshot GetSnapshot() const;

private:
    void Run();

    std::atomic<bool> is_running_{false};
    mutable std::mutex snapshot_mutex_;
    DiagnosticsSnapshot snapshot_;
    std::thread worker_thread_;
};
