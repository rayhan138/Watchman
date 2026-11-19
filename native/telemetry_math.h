#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace Telemetry {

inline double BytesPerSecond(uint64_t bytes, uint64_t elapsed_ms) {
    return elapsed_ms > 0
        ? static_cast<double>(bytes) * 1000.0 / static_cast<double>(elapsed_ms)
        : 0.0;
}

inline double KibibytesPerSecond(uint64_t bytes, uint64_t elapsed_ms) {
    return BytesPerSecond(bytes, elapsed_ms) / 1024.0;
}

inline double CounterBitsPerSecond(uint64_t current, uint64_t previous, uint64_t elapsed_ms) {
    // A driver reset is a new baseline, not an unsigned counter wrap.
    return current >= previous ? BytesPerSecond(current - previous, elapsed_ms) * 8.0 : 0.0;
}

inline double KibibytesToBitsPerSecond(double kib_per_sec) {
    return kib_per_sec * 1024.0 * 8.0;
}

inline double BitsPerSecondToKibibytes(double bits_per_sec) {
    return bits_per_sec / (1024.0 * 8.0);
}

class SampleInterval {
public:
    void Reset(uint64_t now_ms) {
        previous_ms_ = now_ms;
        initialized_ = true;
    }

    uint64_t Advance(uint64_t now_ms) {
        const uint64_t elapsed_ms = initialized_ && now_ms > previous_ms_
            ? now_ms - previous_ms_ : 0;
        Reset(now_ms);
        return elapsed_ms;
    }

private:
    uint64_t previous_ms_ = 0;
    bool initialized_ = false;
};

// Never substitute % Processor Utility: it measures frequency-scaled work,
// not the busy-time percentage used by the dashboard and modern Task Manager.
inline constexpr const wchar_t* cpu_time_counter_paths[] = {
    L"\\Processor Information(_Total)\\% Processor Time",
    L"\\Processor(_Total)\\% Processor Time",
};

// PDH_CSTATUS_VALID_DATA (0) and PDH_CSTATUS_NEW_DATA (1) both contain
// usable data. Keep this OS-independent so the validation is regression-tested.
inline bool ValidPdhCpuSample(uint32_t status, double percent) {
    return (status == 0 || status == 1) && std::isfinite(percent) &&
        percent >= 0.0 && percent <= 100.0;
}

inline bool FreshCpuSample(bool available, uint64_t sampled_at_ms, uint64_t now_ms) {
    return available && now_ms >= sampled_at_ms && now_ms - sampled_at_ms <= 5000;
}

enum class CpuSampleAction { Skip, Prime, Collect };

class CpuSampleCadence {
public:
    CpuSampleAction Advance(uint64_t now_ms) {
        // Leave the baseline untouched on near-simultaneous constructor/engine
        // ticks. Long stalls/resume must prime, not average across a stale gap.
        if (previous_ms_ && now_ms >= *previous_ms_ && now_ms - *previous_ms_ < 250) return CpuSampleAction::Skip;
        const bool prime = !previous_ms_ || now_ms < *previous_ms_ || now_ms - *previous_ms_ > 5000;
        previous_ms_ = now_ms;
        return prime ? CpuSampleAction::Prime : CpuSampleAction::Collect;
    }
private:
    std::optional<uint64_t> previous_ms_;
};

class CpuTimeSampler {
public:
    void Reset() { initialized_ = false; }

    std::optional<double> Sample(uint64_t idle, uint64_t kernel, uint64_t user) {
        std::optional<double> percent;
        if (initialized_ && idle >= idle_ && kernel >= kernel_ && user >= user_) {
            // GetSystemTimes kernel time already includes idle time.
            const double total = static_cast<double>(kernel - kernel_) +
                static_cast<double>(user - user_);
            const double idle_delta = static_cast<double>(idle - idle_);
            if (total > 0.0 && idle_delta <= total) {
                percent = (total - idle_delta) * 100.0 / total;
            }
        }
        idle_ = idle;
        kernel_ = kernel;
        user_ = user;
        initialized_ = true;
        return percent;
    }

private:
    uint64_t idle_ = 0;
    uint64_t kernel_ = 0;
    uint64_t user_ = 0;
    bool initialized_ = false;
};

} // namespace Telemetry
