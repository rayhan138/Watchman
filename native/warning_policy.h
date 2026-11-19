#pragma once

#include "temperature_sensors.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

// No OS calls: only committed preferences and fresh engine samples enter here.
namespace WatchmanWarnings {
struct TemperatureRule {
    bool enabled = false;
    int threshold = 80;
};
struct Preferences {
    bool enabled = true;
    bool traffic_enabled = false;
    int traffic_threshold = 500;
    bool traffic_gigabytes = false;
    bool memory_enabled = true;
    int memory_threshold = 90;
    std::array<TemperatureRule, WatchmanSensors::count> temperatures{};
};
struct Sample {
    std::wstring traffic_date;
    uint64_t download_bytes = 0;
    uint64_t upload_bytes = 0;
    bool memory_available = false;
    double memory_percent = 0.0;
    std::array<WatchmanSensors::Reading, WatchmanSensors::count> temperatures{};
};
struct Events {
    bool traffic = false;
    bool memory = false;
    std::array<bool, WatchmanSensors::count> temperatures{};
    explicit operator bool() const {
        return traffic || memory || std::any_of(temperatures.begin(), temperatures.end(), [](bool value) { return value; });
    }
};
inline uint64_t TrafficThresholdBytes(const Preferences& p) {
    if (p.traffic_threshold <= 0) return 0;
    return static_cast<uint64_t>(p.traffic_threshold) * (p.traffic_gigabytes ? 1024ULL * 1024 * 1024 : 1024ULL * 1024);
}
inline bool FreshMemorySample(bool available, uint64_t total_bytes, uint64_t sampled_at_ms, uint64_t now_ms) {
    return available && total_bytes > 0 && now_ms >= sampled_at_ms && now_ms - sampled_at_ms <= 15000;
}
class Policy {
public:
    static constexpr uint64_t memory_cooldown_ms = 5 * 60 * 1000;
    Events Evaluate(const Preferences& p, const Sample& s, uint64_t now_ms) {
        if (p.traffic_enabled != previous_.traffic_enabled || p.enabled != previous_.enabled ||
            p.traffic_threshold != previous_.traffic_threshold || p.traffic_gigabytes != previous_.traffic_gigabytes) {
            traffic_notified_date_.clear();
        }
        if (p.memory_enabled != previous_.memory_enabled || p.enabled != previous_.enabled ||
            p.memory_threshold != previous_.memory_threshold) memory_armed_ = true;
        for (size_t i = 0; i < WatchmanSensors::count; ++i) {
            if (p.enabled != previous_.enabled ||
                p.temperatures[i].enabled != previous_.temperatures[i].enabled ||
                p.temperatures[i].threshold != previous_.temperatures[i].threshold) {
                temperature_state_[i].armed = true;
            }
        }
        previous_ = p;
        Events events;
        if (!p.enabled) return events;
        const uint64_t threshold = TrafficThresholdBytes(p);
        const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
        const uint64_t total = maximum - s.download_bytes < s.upload_bytes ? maximum : s.download_bytes + s.upload_bytes;
        if (p.traffic_enabled && threshold && !s.traffic_date.empty() &&
            s.traffic_date != traffic_notified_date_ && total >= threshold) {
            events.traffic = true;
            traffic_notified_date_ = s.traffic_date;
        }
        if (p.memory_enabled && p.memory_threshold > 0 && p.memory_threshold <= 100 &&
            s.memory_available && std::isfinite(s.memory_percent) && s.memory_percent >= 0.0 && s.memory_percent <= 100.0) {
            // Five percentage points of hysteresis avoid notifications near a noisy boundary.
            if (s.memory_percent <= (std::max)(0, p.memory_threshold - 5)) memory_armed_ = true;
            const bool cooled_down = !memory_has_notified_ ||
                (now_ms >= memory_last_ms_ && now_ms - memory_last_ms_ >= memory_cooldown_ms);
            if (memory_armed_ && cooled_down && s.memory_percent >= p.memory_threshold) {
                events.memory = true;
                memory_armed_ = false;
                memory_has_notified_ = true;
                memory_last_ms_ = now_ms;
            }
        }
        for (size_t i = 0; i < WatchmanSensors::count; ++i) {
            const auto& rule = p.temperatures[i];
            const auto& reading = s.temperatures[i];
            auto& state = temperature_state_[i];
            if (!rule.enabled || rule.threshold <= 0 || rule.threshold > 150 ||
                !WatchmanSensors::Fresh(reading, now_ms)) continue;
            if (reading.celsius <= (std::max)(0, rule.threshold - 5)) state.armed = true;
            const bool cooled = !state.notified ||
                (now_ms >= state.last_ms && now_ms - state.last_ms >= memory_cooldown_ms);
            if (state.armed && cooled && reading.celsius >= rule.threshold) {
                events.temperatures[i] = true;
                state.armed = false;
                state.notified = true;
                state.last_ms = now_ms;
            }
        }
        return events;
    }
private:
    struct TemperatureState { bool armed = true, notified = false; uint64_t last_ms = 0; };
    std::array<TemperatureState, WatchmanSensors::count> temperature_state_{};
    Preferences previous_{};
    std::wstring traffic_notified_date_;
    bool memory_armed_ = true;
    bool memory_has_notified_ = false;
    uint64_t memory_last_ms_ = 0;
};
} // namespace WatchmanWarnings
