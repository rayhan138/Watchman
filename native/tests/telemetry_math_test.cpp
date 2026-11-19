#include "telemetry_math.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckNear(double actual, double expected, const char* message) {
    const double tolerance = 1e-9 * (std::abs(expected) + 1.0);
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void IntervalTiming() {
    Telemetry::SampleInterval interval;
    Check(interval.Advance(1000) == 0, "First sample must only prime the clock");
    Check(interval.Advance(1500) == 500, "Subsecond elapsed time must be retained");
    Check(interval.Advance(3500) == 2000, "Delayed refresh must use actual elapsed time");
    Check(interval.Advance(3500) == 0, "Same-tick sample must not invent elapsed time");
    Check(interval.Advance(3000) == 0, "Backward clock must not underflow");
    Check(interval.Advance(4000) == 1000, "Backward clock must re-prime the baseline");
    interval.Reset(10000);
    Check(interval.Advance(11000) == 1000, "Explicit reset must discard the old interval");

    Telemetry::SampleInterval from_boot;
    from_boot.Reset(0);
    Check(from_boot.Advance(1000) == 1000, "Timestamp zero is a valid baseline");
}

void ByteRates() {
    constexpr uint64_t mib = 1024ULL * 1024ULL;
    CheckNear(Telemetry::KibibytesPerSecond(mib, 500), 2048.0, "Half-second app rate");
    CheckNear(Telemetry::KibibytesPerSecond(mib, 1000), 1024.0, "One-second app rate");
    CheckNear(Telemetry::KibibytesPerSecond(mib, 2000), 512.0, "Two-second app rate");
    CheckNear(Telemetry::KibibytesPerSecond(5 * mib, 2500), 2048.0, "Fractional-second interval");
    CheckNear(Telemetry::KibibytesPerSecond(mib, 10000), 102.4, "Long refresh stall");
    CheckNear(Telemetry::BytesPerSecond(1, 1), 1000.0, "Small interval precision");
    CheckNear(Telemetry::KibibytesPerSecond(0, 1000), 0.0, "Idle app rate");
    CheckNear(Telemetry::KibibytesPerSecond(mib, 0), 0.0, "Zero interval must not divide by zero");

    // This is the existing engine conversion from legacy KiB/s fields to bps.
    CheckNear(Telemetry::KibibytesPerSecond(mib, 2000) * 1024.0 * 8.0,
        4194304.0, "Engine conversion must not introduce another time or byte factor");
    CheckNear(Telemetry::KibibytesToBitsPerSecond(Telemetry::KibibytesPerSecond(mib, 2000)),
        4194304.0, "KibibytesToBitsPerSecond helper must match conversion factor");
    CheckNear(Telemetry::BitsPerSecondToKibibytes(4194304.0), 512.0, "BitsPerSecondToKibibytes inverse");
}

void DrainIntervals() {
    Telemetry::SampleInterval interval;
    interval.Reset(1000); // consumer startup, before the first refresh
    CheckNear(Telemetry::KibibytesPerSecond(1024, interval.Advance(1500)),
        2.0, "First app refresh must measure from startup, not assume one second");
    CheckNear(Telemetry::KibibytesPerSecond(4096, interval.Advance(3500)),
        2.0, "Each byte drain must get its own elapsed interval");
    // No Advance while unavailable: retained bytes still belong to this span.
    CheckNear(Telemetry::KibibytesPerSecond(8192, interval.Advance(7500)),
        2.0, "Retained bytes must not be divided by only the reconnect tick");
    CheckNear(Telemetry::KibibytesPerSecond(0, interval.Advance(8500)),
        0.0, "Empty drains must still advance the baseline");
    CheckNear(Telemetry::KibibytesPerSecond(1024, interval.Advance(9500)),
        1.0, "Traffic following idle must use only the next interval");
}

void CounterRatesAndResets() {
    CheckNear(Telemetry::CounterBitsPerSecond(1000100, 100, 2000),
        4000000.0, "Interface octets must become bits per actual second");
    CheckNear(Telemetry::CounterBitsPerSecond(100, 100, 1000), 0.0, "Idle counter");
    CheckNear(Telemetry::CounterBitsPerSecond(10, 1000, 1000), 0.0, "Counter reset must not spike");
    CheckNear(Telemetry::CounterBitsPerSecond(110, 10, 1000), 800.0, "Rate after rebaselining reset");
    CheckNear(Telemetry::CounterBitsPerSecond(110, 10, 0), 0.0, "Zero interval counter rate");
    CheckNear(Telemetry::CounterBitsPerSecond(10, 1000, 1000), 0.0, "RX reset");
    CheckNear(Telemetry::CounterBitsPerSecond(2000, 1000, 1000), 8000.0, "TX remains independent of RX reset");
    const uint64_t max = (std::numeric_limits<uint64_t>::max)();
    CheckNear(Telemetry::CounterBitsPerSecond(max, max - 1024, 1000),
        8192.0, "Subtract integers before converting large counters to double");
    CheckNear(Telemetry::CounterBitsPerSecond(1, max, 1000), 0.0, "Counter wrap is treated as reset");
}

void CpuSampling() {
    Telemetry::CpuTimeSampler sampler;
    Check(!sampler.Sample(100, 200, 100), "First fallback sample only primes");
    auto percent = sampler.Sample(150, 300, 200);
    Check(percent.has_value(), "Second fallback sample must produce a value");
    CheckNear(*percent, 75.0, "Kernel time includes idle; do not add idle twice");
    percent = sampler.Sample(350, 500, 200);
    Check(percent.has_value(), "Idle sample must be valid");
    CheckNear(*percent, 0.0, "Fully idle CPU");
    percent = sampler.Sample(350, 600, 300);
    Check(percent.has_value(), "Busy sample must be valid");
    CheckNear(*percent, 100.0, "Fully busy CPU");

    sampler.Reset(); // GetSystemTimes failure
    Check(!sampler.Sample(1000, 2000, 1000), "Returning to fallback must not use old times");
    percent = sampler.Sample(1050, 2100, 1100);
    Check(percent.has_value(), "Fallback must resume after re-prime");
    CheckNear(*percent, 75.0, "Resumed fallback must use only its new interval");

    Check(!sampler.Sample(1, 2, 1), "Decreasing CPU counters must re-prime, not underflow");
    percent = sampler.Sample(51, 102, 101);
    Check(percent.has_value(), "CPU counters must recover after reset");
    CheckNear(*percent, 75.0, "CPU reset recovery");
    Check(!sampler.Sample(51, 102, 101), "Zero elapsed CPU times have no new value");
    Check(!sampler.Sample(1000, 103, 102), "Idle greater than total must not underflow");
    percent = sampler.Sample(1050, 203, 202);
    Check(percent.has_value(), "Invalid CPU interval must establish a fresh baseline");
    CheckNear(*percent, 75.0, "CPU recovery after inconsistent sample");
}

void CpuCadence() {
    using Telemetry::CpuSampleAction;
    Telemetry::CpuSampleCadence cadence;
    Check(cadence.Advance(0) == CpuSampleAction::Prime, "First CPU tick only primes, including boot timestamp zero");
    Check(cadence.Advance(0) == CpuSampleAction::Skip, "Same-time CPU ticks must not manufacture a rate");
    Check(cadence.Advance(249) == CpuSampleAction::Skip, "Near-simultaneous startup must not produce a noisy reading");
    Check(cadence.Advance(250) == CpuSampleAction::Collect, "Skipped ticks must not advance the CPU baseline");
    Check(cadence.Advance(1250) == CpuSampleAction::Collect, "Normal one-second CPU cadence");
    Check(cadence.Advance(6250) == CpuSampleAction::Collect, "Five-second interval remains a valid window");
    Check(cadence.Advance(11251) == CpuSampleAction::Prime, "Long stalls/resume must discard old CPU intervals");
    Check(cadence.Advance(11252) == CpuSampleAction::Skip, "Re-prime must still wait for a meaningful interval");
    Check(cadence.Advance(12251) == CpuSampleAction::Collect, "Sampling must recover normally after resume");
    Check(cadence.Advance(100) == CpuSampleAction::Prime, "Backward timestamps must re-prime without underflow");
    Check(cadence.Advance(1100) == CpuSampleAction::Collect, "Sampling must recover after a clock reset");
}

void CpuAccuracyAndAvailability() {
    for (const wchar_t* path : Telemetry::cpu_time_counter_paths) {
        const std::wstring counter(path);
        Check(counter.find(L"(_Total)") != std::wstring::npos, "CPU must aggregate all logical processors");
        Check(counter.find(L"% Processor Time") != std::wstring::npos && counter.find(L"Utility") == std::wstring::npos,
            "Boost-scaled utility must not return as the CPU utilization source");
    }
    for (uint32_t status : {0u, 1u}) {
        Check(Telemetry::ValidPdhCpuSample(status, 13.0), "Both valid and new PDH data must be accepted");
        Check(Telemetry::ValidPdhCpuSample(status, 0.0), "Idle is a real sample");
        Check(Telemetry::ValidPdhCpuSample(status, 100.0), "Fully busy is a real sample");
        for (double invalid : {-0.1, 100.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            Check(!Telemetry::ValidPdhCpuSample(status, invalid), "Invalid PDH values must not be displayed");
        }
    }
    Check(!Telemetry::ValidPdhCpuSample(0xC0000BC6u, 13.0), "A failed counter must not reuse its value");
    Check(Telemetry::FreshCpuSample(true, 0, 5000), "Freshness includes the boundary and boot time zero");
    Check(!Telemetry::FreshCpuSample(true, 0, 5001), "Stale CPU readings must not look live");
    Check(!Telemetry::FreshCpuSample(false, 100, 100), "Missing CPU reading is not zero usage");
    Check(!Telemetry::FreshCpuSample(true, 101, 100), "Future CPU timestamp must not underflow");

    Telemetry::CpuTimeSampler sampler;
    const uint64_t base = (1ULL << 60); // Absolute OS times far exceed a double's integer precision.
    Check(!sampler.Sample(base, base, base), "Native baseline primes once");
    // 12 logical processors, 87% idle, 13% busy. Clock/turbo multiplier is
    // deliberately NOT an input and no arbitrary divide-by-two is involved.
    for (uint64_t cycle = 1; cycle <= 4; ++cycle) {
        const auto value = sampler.Sample(base + cycle * 10440, base + cycle * 11000, base + cycle * 1000);
        Check(value.has_value(), "Warm native intervals must remain available through PDH priming/retries");
        CheckNear(*value, 13.0, "Aggregate utilization must be 13%, not boost-scaled 26%");
    }
    const auto delayed = sampler.Sample(base + 6 * 10440, base + 6 * 11000, base + 6 * 1000);
    Check(delayed.has_value(), "Delayed sampling must still use elapsed CPU time");
    CheckNear(*delayed, 13.0, "A two-second interval must not double CPU usage");
}

} // namespace

int main() {
    try {
        IntervalTiming();
        ByteRates();
        DrainIntervals();
        CounterRatesAndResets();
        CpuSampling();
        CpuAccuracyAndAvailability();
        CpuCadence();
        std::cout << "Telemetry math tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Telemetry math test failed: " << error.what() << '\n';
        return 1;
    }
}
