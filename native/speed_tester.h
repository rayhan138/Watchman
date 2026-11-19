#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <cstdint>
#include "network_test_protocol.h"

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

#include <mutex>
#include <thread>

class SpeedTester {
public:
    SpeedTester();
    ~SpeedTester();

    // Starts the speed test in a background thread. Synchronous WinHTTP I/O is
    // not externally cancellable here: ten seconds is a transfer target, not a
    // hard deadline. Destruction joins pending I/O and can block on the OS/peer.
    void StartTest();

    // Requests cancellation of an ongoing test.
    void CancelTest();

    // Check if a test is currently running
    bool IsRunning() const { return is_running_.load(); }

    // Get current progress percentage (0 - 100)
    int GetProgress() const { return progress_.load(); }

    // Get current status string (e.g. "Locating server...", "Testing Download...")
    std::wstring GetStatusString() const;

    // Get current live speed during test (in Mbps)
    double GetLiveSpeed() const { return live_speed_.load(); }

    // Get the final test results
    SpeedTestResult GetResult() const;

private:
    std::atomic<bool> is_running_{ false };
    std::atomic<int> progress_{ 0 };
    std::atomic<double> live_speed_{ 0.0 };
    
    mutable std::mutex state_mutex_;
    std::wstring status_string_;
    SpeedTestResult result_;
    
    std::thread worker_thread_;

    void RunSpeedTest();
    void SetStatus(const std::wstring& status);
    void SetResult(const SpeedTestResult& res);
    bool QueryMLabServer(
        std::wstring& dl_url,
        std::wstring& ul_url,
        std::wstring& server_name,
        std::wstring& server_city,
        std::wstring& error
    );
    network_test::TransferResult RunDownloadTest(const std::wstring& url, double& min_rtt_ms);
    network_test::TransferResult RunUploadTest(const std::wstring& url, double& min_rtt_ms);
};
