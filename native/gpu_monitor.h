#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GpuMetrics {
    float percent_used = 0.0f;
    std::wstring label = L"N/A";
    std::wstring active_engine = L"Unavailable";
    bool available = false;
};

// Optional GPU discovery/PDH work must not gate the first network/RAM sample
// or stall their one-second loop. All native handles stay on this owned worker;
// the short snapshot lock is never held across DXGI/PDH calls.
class GpuMonitor {
public:
    using InitializationHook = std::function<void(const std::atomic<bool>& stopping)>;
    explicit GpuMonitor(InitializationHook before_initialize = {});
    ~GpuMonitor();
    GpuMetrics GetSnapshot() const;
private:
    void Run() noexcept;
    void InitializeLabel();
    void UpdateGpu();
    void InitGpuPdh();
    void RefreshGpuPdhCounters();
    void CloseQuery();
    struct GpuPdhCounter { void* handle = nullptr; std::wstring path; };
    void* pdh_query_ = nullptr;
    std::vector<GpuPdhCounter> pdh_gpu_counters_;
    bool pdh_gpu_initialized_ = false, pdh_gpu_primed_ = false;
    uint64_t last_gpu_counter_refresh_ms_ = 0, last_retry_ms_ = 0;
    GpuMetrics gpu_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false};
    GpuMetrics snapshot_;
    uint64_t sampled_at_ms_ = 0;
    InitializationHook before_initialize_;
    std::thread worker_;
};
