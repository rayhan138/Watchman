#include "warning_policy.h"
#include "warnings_settings_layout.h"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Traffic() {
    using namespace WatchmanWarnings;
    Preferences p;
    p.memory_enabled = false;
    p.traffic_enabled = true;
    p.traffic_threshold = 1;
    Sample s{L"2026-10-30", 600000, 448576};
    Policy policy;
    auto event = policy.Evaluate(p, s, 0);
    Check(event.traffic && !event.memory, "combined download and upload must trigger at exact threshold");
    Check(!policy.Evaluate(p, s, 1000), "traffic must notify only once for a date and configuration per process");
    s.traffic_date = L"2026-10-31";
    Check(policy.Evaluate(p, s, 2000).traffic, "new sample date re-arms daily traffic");
    s.traffic_date.clear();
    Check(!policy.Evaluate(p, s, 3000), "missing traffic date is unavailable, not today");
    s.traffic_date = L"2026-11-01";
    p.enabled = false;
    Check(!policy.Evaluate(p, s, 4000), "master off disables traffic");
    p.enabled = true;
    p.traffic_threshold = 0;
    Check(!policy.Evaluate(p, s, 5000), "zero threshold must not alert continuously");
    p.traffic_threshold = -1;
    Check(!policy.Evaluate(p, s, 6000), "negative threshold must not underflow");
    p.traffic_threshold = 1;
    p.traffic_gigabytes = true;
    Check(TrafficThresholdBytes(p) == 1073741824ULL && !policy.Evaluate(p, s, 7000), "GiB conversion");
    s.download_bytes = (std::numeric_limits<uint64_t>::max)();
    s.upload_bytes = 1;
    Check(policy.Evaluate(p, s, 8000).traffic, "counter sum must saturate instead of wrapping");
    p.memory_threshold = 80;
    Check(!policy.Evaluate(p, s, 9000), "unrelated memory preference must not reset traffic deduplication");
    p.traffic_threshold = 2;
    Check(policy.Evaluate(p, s, 10000).traffic, "changed traffic threshold must re-evaluate the current date");
    p.traffic_enabled = false;
    Check(!policy.Evaluate(p, s, 11000), "individual traffic switch disables the rule");
}
void Memory() {
    using namespace WatchmanWarnings;
    Check(FreshMemorySample(true, 100, 0, 15000), "boot timestamp zero and freshness boundary are valid");
    Check(!FreshMemorySample(true, 100, 0, 15001), "stale memory cannot notify after a sampler stall");
    Check(!FreshMemorySample(false, 100, 1000, 1000), "failed OS read cannot reuse cached percentage");
    Check(!FreshMemorySample(true, 0, 1000, 1000), "missing physical total is unavailable");
    Check(!FreshMemorySample(true, 100, 1001, 1000), "backward timestamp cannot underflow");
    Preferences p;
    Sample s;
    s.memory_percent = 95;
    Policy policy;
    Check(!policy.Evaluate(p, s, 0), "unavailable memory must not alert");
    s.memory_available = true;
    Check(policy.Evaluate(p, s, 0).memory, "first high memory sample alerts");
    Check(!policy.Evaluate(p, s, Policy::memory_cooldown_ms + 1), "sustained high usage must not repeat after cooldown");
    s.memory_percent = 86;
    policy.Evaluate(p, s, Policy::memory_cooldown_ms + 2);
    s.memory_percent = 95;
    Check(!policy.Evaluate(p, s, Policy::memory_cooldown_ms + 3), "small threshold noise must not re-arm");
    s.memory_percent = 85;
    policy.Evaluate(p, s, Policy::memory_cooldown_ms + 4);
    s.memory_percent = 90;
    Check(policy.Evaluate(p, s, Policy::memory_cooldown_ms + 5).memory, "five-point recovery and cooldown re-arm");
    s.memory_percent = 0;
    policy.Evaluate(p, s, Policy::memory_cooldown_ms + 6);
    s.memory_percent = 95;
    Check(!policy.Evaluate(p, s, Policy::memory_cooldown_ms + 7), "cooldown blocks fast repeated crossings");
    Check(policy.Evaluate(p, s, 2 * Policy::memory_cooldown_ms + 5).memory, "pending recovered crossing may alert after cooldown");
    p.enabled = false;
    Check(!policy.Evaluate(p, s, 3 * Policy::memory_cooldown_ms), "master off disables memory");
    p.enabled = true;
    p.memory_threshold = 101;
    Check(!policy.Evaluate(p, s, 3 * Policy::memory_cooldown_ms + 1), "out-of-range memory threshold rejected");
    p.memory_threshold = 0;
    Check(!policy.Evaluate(p, s, 3 * Policy::memory_cooldown_ms + 2), "zero memory threshold rejected");
    p.memory_threshold = 90;
    s.memory_percent = std::numeric_limits<double>::quiet_NaN();
    Check(!policy.Evaluate(p, s, 4 * Policy::memory_cooldown_ms), "NaN sample rejected");
    s.memory_percent = 101;
    Check(!policy.Evaluate(p, s, 4 * Policy::memory_cooldown_ms), "invalid percentage rejected");
    s.memory_percent = 95;
    p.traffic_enabled = true;
    p.traffic_threshold = 1;
    s.traffic_date = L"2026-10-30";
    s.download_bytes = 1048576;
    const auto both = policy.Evaluate(p, s, 4 * Policy::memory_cooldown_ms);
    Check(both.memory && both.traffic, "simultaneous rules must combine into a single notification");
    p.memory_enabled = false;
    Check(!policy.Evaluate(p, s, 5 * Policy::memory_cooldown_ms), "individual memory switch disables the rule");
}
void Temperatures() {
    using namespace WatchmanWarnings;
    using namespace WatchmanSensors;
    for (size_t index = 0; index < count; ++index) {
        Preferences p;
        p.memory_enabled = false;
        p.temperatures[index] = {true, 80};
        Sample s;
        Policy policy;
        auto sample = [&](double celsius, uint64_t at, bool available = true) {
            s.temperatures[index] = {available, celsius, at, L"Real sensor", L"Device", L"Test provider"};
        };
        sample(85, 0, false);
        Check(!policy.Evaluate(p, s, 0), "unavailable temperature must not notify");
        sample(85, 0);
        Check(!policy.Evaluate(p, s, freshness_ms + 1), "stale temperature must not notify");
        const auto first = policy.Evaluate(p, s, 0);
        Check(first.temperatures[index] && !first.traffic && !first.memory,
            "a fresh hot sensor must trigger exactly its own temperature rule");
        sample(85, Policy::memory_cooldown_ms);
        Check(!policy.Evaluate(p, s, Policy::memory_cooldown_ms), "sustained heat must not repeat after cooldown");
        sample(76, Policy::memory_cooldown_ms + 1);
        policy.Evaluate(p, s, Policy::memory_cooldown_ms + 1);
        sample(85, Policy::memory_cooldown_ms + 2);
        Check(!policy.Evaluate(p, s, Policy::memory_cooldown_ms + 2), "four degrees of recovery must not re-arm");
        sample(75, Policy::memory_cooldown_ms + 3);
        policy.Evaluate(p, s, Policy::memory_cooldown_ms + 3);
        sample(80, Policy::memory_cooldown_ms + 4);
        Check(policy.Evaluate(p, s, Policy::memory_cooldown_ms + 4).temperatures[index],
            "five-degree recovery and cooldown must re-arm the correct device rule");
        sample(60, Policy::memory_cooldown_ms + 5);
        policy.Evaluate(p, s, Policy::memory_cooldown_ms + 5);
        sample(85, Policy::memory_cooldown_ms + 6);
        Check(!policy.Evaluate(p, s, Policy::memory_cooldown_ms + 6), "cooldown must block fast repeated heat crossings");
        sample(85, 2 * Policy::memory_cooldown_ms + 4);
        p.enabled = false;
        Check(!policy.Evaluate(p, s, 2 * Policy::memory_cooldown_ms + 4), "master off must suppress temperature warnings");
        p.enabled = true;
        p.temperatures[index].enabled = false;
        Check(!policy.Evaluate(p, s, 2 * Policy::memory_cooldown_ms + 4), "individual temperature switch must work");
        p.temperatures[index] = {true, 151};
        Check(!policy.Evaluate(p, s, 2 * Policy::memory_cooldown_ms + 4), "invalid temperature threshold must not notify");
        p.temperatures[index].threshold = 80;
        sample(std::numeric_limits<double>::quiet_NaN(), 2 * Policy::memory_cooldown_ms + 4);
        Check(!policy.Evaluate(p, s, 2 * Policy::memory_cooldown_ms + 4), "NaN sensor must not notify");
    }
    Preferences p;
    p.memory_enabled = false;
    Sample s;
    for (size_t i = 0; i < count; ++i) {
        p.temperatures[i] = {true, 80};
        s.temperatures[i] = {true, 85, 1000, L"Sensor", L"Device", L"Test provider"};
    }
    Policy policy;
    const auto events = policy.Evaluate(p, s, 1000);
    Check(static_cast<bool>(events), "temperature events must participate in the shared notification");
    for (bool event : events.temperatures) Check(event, "all hot device categories must notify independently");
}

void Layout() {
    for (float width : {238.0f, 278.0f, 338.0f}) {
        const auto initial = GetWarningsSettingsLayout(154, 154 + width, 10);
        for (float height : {460.0f, 630.0f, 740.0f}) {
            const float bottom = 10 + height - 72;
            const float maximum = (std::max)(0.0f, initial.info_bot - bottom);
            const auto scrolled = GetWarningsSettingsLayout(154, 154 + width, 10, maximum);
            Check(scrolled.title_y == initial.title_y && scrolled.subtitle_y == initial.subtitle_y, "warnings title must remain fixed");
            Check(scrolled.info_bot <= bottom + 0.01f, "last warning note must be reachable above footer");
            for (int i = 1; i < scrolled.rule_count; ++i) {
                Check(scrolled.row[i - 1] + scrolled.row_h == scrolled.row[i], "warning rows must not overlap");
            }
            Check(scrolled.threshold_bot >= scrolled.row[5] + scrolled.row_h,
                "all four sensor controls must fit inside the threshold card");
            Check(scrolled.unit_right < scrolled.toggle_left, "threshold controls must not collide with switches");
            Check(scrolled.sensors_top >= scrolled.threshold_bot + 16 && scrolled.info_top >= scrolled.sensors_bot + 16,
                "sensor setup and explanation need separate cards");
        }
    }
}
}
int main() {
    try { Traffic(); Memory(); Temperatures(); Layout(); std::cout << "Warning policy and layout checks passed (no OS notifications sent)\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
