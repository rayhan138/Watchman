#include "runtime_policy.h"
#include "main_graphics_policy.h"
#include "widget_recovery_policy.h"
#include "live_readings.h"
#include "speed_test_view.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void Run() {
    WatchmanUi::MainGraphicsRetry graphics;
    Check(graphics.Due(0), "Deliberate dashboard opening needs an immediate graphics attempt");
    uint64_t graphics_time = 100;
    for (int failure = 0; failure < 20; ++failure) {
        graphics.Failed(graphics_time);
        Check(graphics.Next() >= graphics_time + 1000 && graphics.Next() <= graphics_time + 30000,
            "Dashboard graphics recovery needs a bounded 1s-30s cooldown");
        Check(!graphics.Due(graphics.Next() - 1) && graphics.Due(graphics.Next()),
            "Animation and pointer paints must not hot-loop failed main initialization");
        graphics_time = graphics.Next();
    }
    Check(graphics.Next() == graphics_time, "Graphics retry timestamp must be stable between attempts");
    graphics.Succeeded();
    Check(graphics.Due(0), "Successful dashboard recovery must clear a cached error/cooldown");
    graphics.Failed(1000);
    Check(graphics.Next() == 2000, "A later device loss starts with the prompt initial cooldown again");

    using namespace WatchmanWidgetRuntime;
    RetrySchedule shell;
    Check(shell.Due(0), "Initial attachment must not be delayed");
    uint64_t now = 0;
    for (int retry = 0; retry < 30; ++retry) {
        shell.Complete({Status::WaitingForShell, 0}, now);
        Check(shell.Next() > now && shell.Next() - now <= 2000, "Retry delay must remain bounded");
        Check(!shell.Due(shell.Next() - 1) && shell.Due(shell.Next()), "Expensive work runs only when due");
        now = shell.Next();
    }
    shell.Complete({Status::WaitingForShell, 0}, now);
    Check(shell.Next() == now + 2000, "Repeated failures settle at two seconds, not a hot 100ms loop");
    for (int retry = 0; retry < 12; ++retry) {
        now = shell.Next();
        shell.Complete({Status::WaitingForShell, static_cast<uint32_t>(retry % 2)}, now);
    }
    Check(shell.Next() == now + 2000, "Changing error codes must not reset same-status attachment backoff");
    shell.Wake(); Check(shell.Due(0), "Explicit shell/display/enable events bypass attachment backoff");
    shell.Complete({Status::Ready, 0}, 5000);
    Check(shell.Next() == 5500, "Healthy widgets need cheap layout checks at 2Hz, not 10Hz");
    shell.Complete({Status::AttachFailed, 5}, 5500);
    Check(shell.Next() == 5600, "A new failure starts with a prompt first retry");
    shell.Complete({Status::Disabled, 0}, 6000);
    Check(shell.Next() == 8000, "Disabled widgets must not repeatedly discover Explorer");

    GpuRetryPolicy gpu;
    Check(!gpu.Due(10000000), "GPU retry must not run without a prior failure");
    now = 1000;
    for (int retry = 0; retry < 15; ++retry) {
        gpu.Failed(now);
        Check(gpu.Next() - now >= 30000 && gpu.Next() - now <= 300000, "GPU retry uses a bounded 30s-5m cooldown");
        Check(!gpu.Due(gpu.Next()), "A broken GDI path is not a reason to keep probing the GPU");
        gpu.GdiFrame(true); gpu.GdiFrame(true);
        Check(!gpu.Due(gpu.Next()), "GPU retry needs three healthy fallback frames");
        gpu.GdiFrame(true);
        Check(!gpu.Due(gpu.Next() - 1) && !gpu.Due(gpu.Next(), false) && gpu.Due(gpu.Next()),
            "Cooldown and quiet-UI gate must both permit the GPU trial");
        gpu.GdiFrame(false); Check(!gpu.Due(gpu.Next()), "Rejected GDI frame resets fallback health");
        now = gpu.Next();
    }
    gpu.Reset(); Check(!gpu.Due(now + 10000000), "Successful GPU/host reset clears retry state");

    DisplayRecovery displays;
    displays.Request(0, false); displays.Request(100, true);
    Check(!displays.Due(299) && displays.Due(300), "Display messages must settle before rebuilding graphics");
    Check(displays.Consume() && !displays.Pending(), "Coalescing preserves the strongest reset request");
    for (now = 1000; now < 2000; now += 50) displays.Request(now, false);
    Check(displays.Due(2000), "An endless display-message burst must not starve recovery");
    Check(!displays.Consume(), "A work-area-only change does not require a device reset");

    DashboardSnapshot sample;
    Check(!WatchmanLiveReadings::NetworkReady(sample), "Unpublished startup throughput is not a genuine idle zero");
    Check(!WatchmanLiveReadings::MemoryReady(sample, 100), "Unpublished startup memory is unavailable");
    sample.sample_sequence = 1;
    Check(WatchmanLiveReadings::NetworkReady(sample), "A measured zero-throughput interval remains a real zero");
    sample.ram_sample_available = true; sample.ram_sampled_at_ms = 100; sample.ram_total_bytes = 1;
    Check(WatchmanLiveReadings::MemoryReady(sample, 5100), "A fresh zero-percent sample remains valid");
    Check(!WatchmanLiveReadings::MemoryReady(sample, 5101) && !WatchmanLiveReadings::MemoryReady(sample, 99),
        "Expired or backwards-time memory must show a placeholder");
    for (float invalid : {-1.f, 101.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        sample.ram_usage_percent = invalid;
        Check(!WatchmanLiveReadings::MemoryReady(sample, 100), "Invalid memory cannot enter text/gauge geometry");
        sample.gpu_available = true; sample.gpu_usage_percent = invalid;
        Check(!WatchmanLiveReadings::GpuReady(sample), "Invalid GPU values cannot enter gauge geometry");
    }
    sample.download_speed_bps = std::numeric_limits<double>::quiet_NaN();
    Check(!WatchmanLiveReadings::NetworkReady(sample), "Non-finite throughput is unavailable, not a number");
    WatchmanLiveReadings::SampleGate chart;
    Check(!chart.Accept(0) && chart.Accept(42), "Chart starts with the first acquisition");
    for (int paint = 0; paint < 1000; ++paint) Check(!chart.Accept(42), "Mouse and animation paints must not advance telemetry history");
    Check(chart.Accept(43) && chart.Accept(1), "New acquisition or engine-generation reset advances the chart");

    WatchmanRuntime::MainFrameCadence cadence;
    for (now = 0; now <= 1000; now += 16) Check(!cadence.Due(now, false, false), "Settled foreground UI must not repaint sixty times a second");
    Check(cadence.Due(1001, true, false) && cadence.Due(1017, true, false), "Finite native animations keep full cadence");
    Check(!cadence.Due(1116, false, true) && cadence.Due(1117, false, true), "Live speed result repaint cadence is 10Hz");

    for (float window_height : {650.0f, 768.0f, 820.0f, 1080.0f}) {
        for (float card_height : {300.0f, 396.0f}) {
            const float viewport_top = 88, viewport_bottom = window_height - 32, card_top = 562;
            const float offset = SpeedTestView::RevealOffset(card_top, card_height + 20, viewport_top, viewport_bottom, 0);
            Check(card_top - offset >= viewport_top && card_top + card_height + 20 - offset <= viewport_bottom,
                "Starting and completed speed-test cards, including warnings, must be fully in view at minimum window height");
            Check(SpeedTestView::RevealOffset(card_top, card_height + 20, viewport_top, viewport_bottom, offset) == offset,
                "Already visible card must not keep jumping on progress paints");
        }
    }
    Check(SpeedTestView::RevealOffset(562, 416, 88, 400, 0) == 474,
        "Exceptionally short screens show the card header without inventing a smaller unreadable layout");

    SpeedTestSnapshot state;
    Check(SpeedTestView::Rate(state, true) == L"--", "Idle test cannot claim a measured zero");
    state.phase = SpeedTestPhase::Locating; state.progress = 5;
    Check(state.Running() && SpeedTestView::Rate(state, true) == L"--", "Locating is not a zero-speed download");
    state.phase = SpeedTestPhase::Download; state.progress = 40; state.live_mbps = 17.5;
    Check(SpeedTestView::Rate(state, true).find(L"17.5 Mbps") == 0 && SpeedTestView::Rate(state, false) == L"--", "Download displays only its own live result");
    state.phase = SpeedTestPhase::PreparingUpload; state.result.download_state = network_test::TransferState::Complete;
    state.result.download_speed_mbps = 17.5;
    Check(SpeedTestView::Rate(state, true) == L"17.5 Mbps" && SpeedTestView::Rate(state, false) == L"Preparing...", "Completed download remains visible between phases");
    state.phase = SpeedTestPhase::Upload; state.progress = 95; state.live_mbps = 12.5;
    Check(SpeedTestView::Rate(state, false).find(L"12.5 Mbps (Testing...)") == 0,
        "Upload is still live at 90-95%; percentage cannot make it vanish prematurely");
    state.phase = SpeedTestPhase::Finishing;
    Check(state.Running() && SpeedTestView::Rate(state, false) == L"Waiting for summary...", "Upload summary wait is an explicit busy state");
    state.phase = SpeedTestPhase::Cancelling;
    Check(state.Running() && SpeedTestView::Progress(state) == L"Cancelling...", "Cancellation remains busy until the worker actually retires");
    state.phase = SpeedTestPhase::Completed; state.result.upload_state = network_test::TransferState::Partial; state.result.upload_speed_mbps = 8;
    Check(!state.Running() && SpeedTestView::Rate(state, false) == L"8.0 Mbps (1.00 MB/s)", "Final rates retain bits and bytes units");
    state.result.upload_state = network_test::TransferState::Failed;
    Check(SpeedTestView::Rate(state, false) == L"--", "Failed transfer must never display a valid-looking zero");
}
}
int main() {
    try { Run(); std::cout << "Responsiveness/recovery policy checks passed: " << checks << '\n'; return 0; }
    catch (const std::exception& error) { std::cerr << "Check " << checks << ": " << error.what() << '\n'; return 1; }
}
