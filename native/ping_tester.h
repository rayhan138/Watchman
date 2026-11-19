#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

class PingTester {
public:
    PingTester();
    ~PingTester();

    void Start();
    void Stop();
    
    // Set active mode. If false, queries slow down to once every 15 seconds.
    void SetActiveMode(bool active);

    bool IsRunning() const { return running_.load(); }
    DWORD GetProbeError() const { return probe_error_.load(); }
    float GetLatencyMs() const { return latency_ms_.load(); }
    float GetJitterMs() const { return jitter_ms_.load(); }
    float GetPacketLoss() const { return packet_loss_percent_.load(); }
    uint32_t GetSamplesSent() const { return samples_sent_.load(); }
    uint32_t GetSamplesReceived() const { return samples_received_.load(); }
    float GetMinLatencyMs() const { return min_latency_ms_.load(); }
    float GetMaxLatencyMs() const { return max_latency_ms_.load(); }
    std::wstring GetQualityScoreString() const;
    float GetQualityScorePercent() const;

private:
    std::atomic<bool> running_{ false };
    std::atomic<bool> active_mode_{ true };
    std::thread worker_thread_;
    std::mutex lifecycle_mutex_;
    std::atomic<DWORD> probe_error_{ 0 };

    std::atomic<float> latency_ms_{ 0.0f };
    std::atomic<float> jitter_ms_{ 0.0f };
    std::atomic<float> packet_loss_percent_{ 0.0f };
    std::atomic<uint32_t> samples_sent_{ 0 };
    std::atomic<uint32_t> samples_received_{ 0 };
    std::atomic<float> min_latency_ms_{ 0.0f };
    std::atomic<float> max_latency_ms_{ 0.0f };

    struct PingSample {
        float rtt_ms = 0.0f;
        bool lost = false;
    };
    std::vector<PingSample> recent_samples_;

    void RunLoop();
    void CalculateMetrics(float new_rtt, bool was_lost);
};
