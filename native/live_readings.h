#pragma once
#include "snapshots.h"
#include "telemetry_math.h"
#include <cmath>

namespace WatchmanLiveReadings {
inline bool NetworkReady(const DashboardSnapshot& sample) {
    return sample.sample_sequence != 0 && std::isfinite(sample.download_speed_bps) && sample.download_speed_bps >= 0 &&
        std::isfinite(sample.upload_speed_bps) && sample.upload_speed_bps >= 0;
}
inline bool GpuReady(const DashboardSnapshot& sample) {
    return sample.sample_sequence != 0 && sample.gpu_available && std::isfinite(sample.gpu_usage_percent) &&
        sample.gpu_usage_percent >= 0 && sample.gpu_usage_percent <= 100;
}
inline bool MemoryReady(const DashboardSnapshot& sample, uint64_t now) {
    return sample.ram_sample_available && sample.ram_total_bytes > 0 &&
        now >= sample.ram_sampled_at_ms && now - sample.ram_sampled_at_ms <= 5000 &&
        std::isfinite(sample.ram_usage_percent) && sample.ram_usage_percent >= 0 && sample.ram_usage_percent <= 100;
}

// Charts advance on acquisition, never on mouse/animation paints.
class SampleGate {
public:
    bool Accept(uint64_t sequence) {
        if (!sequence || sequence == last_) return false;
        last_ = sequence;
        return true;
    }
private:
    uint64_t last_ = 0;
};
} // namespace WatchmanLiveReadings
