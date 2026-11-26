#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include "speed_test_state.h"
#ifdef WATCHMAN_SPEED_TESTING
#include <functional>
#endif

class SpeedTester {
public:
    SpeedTester();
    ~SpeedTester();

    // Never joins an unfinished transfer on the UI thread. A cancelled worker
    // remains busy until its in-flight synchronous Windows I/O has returned.
    void StartTest();
    void CancelTest();
    bool IsRunning() const { return is_running_.load(); }
    SpeedTestSnapshot GetSnapshot() const;
    int GetProgress() const { return GetSnapshot().progress; }
    std::wstring GetStatusString() const { return GetSnapshot().status; }
    double GetLiveSpeed() const { return GetSnapshot().live_mbps; }
    SpeedTestResult GetResult() const { return GetSnapshot().result; }
#ifdef WATCHMAN_SPEED_TESTING
    // Compiled only into the lifecycle test. No shipped EXE fault switches.
    void SetWorkerForTest(std::function<void(SpeedTester&)> worker) { test_worker_ = std::move(worker); }
    void SetEndpointsForTest(std::wstring download, std::wstring upload) {
        test_download_ = std::move(download); test_upload_ = std::move(upload);
    }
    bool CancelRequestedForTest() const { return cancel_requested_.load(); }
    void PublishForTest(SpeedTestPhase phase, int progress, double live) { SetPhase(phase, L"test", progress, live); }
#endif
private:
    std::atomic<bool> is_running_{false};
    std::atomic<bool> cancel_requested_{false};
    mutable std::mutex state_mutex_;
    SpeedTestSnapshot state_;
    std::thread worker_thread_;
    uint64_t last_live_publish_ms_ = 0; // Worker-owned; at most ten UI publications/s.
#ifdef WATCHMAN_SPEED_TESTING
    std::function<void(SpeedTester&)> test_worker_;
    std::wstring test_download_, test_upload_;
#endif
    void RunSpeedTest();
    void FinishWorker();
    void SetPhase(SpeedTestPhase phase, const std::wstring& status, int progress, double live = 0.0,
        const SpeedTestResult* result = nullptr);
    void PublishLive(int progress, double live);
    void SetResult(const SpeedTestResult& result);
    bool QueryMLabServer(std::wstring& dl_url, std::wstring& ul_url,
        std::wstring& server_name, std::wstring& server_city, std::wstring& error);
    network_test::TransferResult RunDownloadTest(const std::wstring& url, double& min_rtt_ms);
    network_test::TransferResult RunUploadTest(const std::wstring& url, double& min_rtt_ms);
};
