#include "tray_registration.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace WatchmanTray;
using namespace std::chrono_literals;
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Predicate> bool Wait(Predicate predicate) {
    const auto until = std::chrono::steady_clock::now() + 5s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < until);
    return predicate();
}
// Construct after the worker so any failing assertion releases blocked native
// work before its owner joins. Never leave a detached test thread behind.
struct Release {
    std::promise<void>& promise;
    bool released = false;
    void Now() { if (!released) { promise.set_value(); released = true; } }
    ~Release() { Now(); }
};
void Schedule() {
    RetrySchedule retry;
    Check(retry.Due(0) && !retry.Ready() && !retry.Pending(), "Initial tray registration must not wait for a dashboard");
    uint64_t now = 0;
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        const auto generation = retry.Begin(now);
        Check(generation && retry.Pending() && !retry.Due(now + 1000000), "One slow shell call must not queue a retry per timer tick");
        Check(retry.Begin(now) == 0, "Pending tray registration must not be submitted twice");
        Check(retry.Complete({generation, false}, now), "Current failed registration must be consumed");
        const uint64_t expected = (std::min)(uint64_t{2000}, uint64_t{100} << (std::min)(attempt, 5u));
        Check(!retry.Ready() && !retry.Pending() && retry.Next() == now + expected, "Failed tray retries must back off from 100ms to 2s");
        Check(!retry.Due(retry.Next() - 1) && retry.Due(retry.Next()), "Tray retry must respect its bounded schedule");
        now = retry.Next();
    }
    const auto before_restart = retry.Begin(now);
    retry.Invalidate();
    Check(retry.Due(0) && !retry.Ready(), "Explorer broadcast must wake retry immediately, without showing a window");
    const auto after_restart = retry.Begin(now);
    Check(before_restart != after_restart, "Explorer generations must be distinct");
    for (bool stale_ready : {false, true}) {
        Check(!retry.Complete({before_restart, stale_ready}, now) && retry.Pending() && !retry.Ready(),
            "A stale shell result must not cancel or satisfy the replacement shell's request");
    }
    Check(retry.Complete({after_restart, true}, now) && retry.Ready() && !retry.Pending(), "A recovered tray should become usable");
    Check(!retry.Due(UINT64_MAX) && retry.Begin(UINT64_MAX) == 0, "A healthy tray needs no repeating shell-add calls");
    Check(!retry.Complete({after_restart, false}, now) && retry.Ready(), "Duplicate completion must not lose a recovered tray");
    retry.Invalidate();
    const auto later = retry.Begin(now);
    Check(later && !retry.Ready() && retry.Complete({later, false}, now) && retry.Next() == now + 100,
        "A new shell failure must restart prompt recovery, not retain old backoff");
}
void OwnedWorker() {
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto gate = release.get_future().share();
    std::atomic<unsigned> calls{0}, active{0}, maximum{0};
    const auto owner = std::this_thread::get_id();
    std::atomic<bool> off_owner{true};
    Worker worker([&] {
        if (std::this_thread::get_id() == owner) off_owner = false;
        const auto count = ++active;
        maximum = (std::max)(maximum.load(), count);
        if (++calls == 1) { entered.set_value(); gate.wait(); }
        --active;
        return true;
    });
    Release unblock{release};
    Check(worker.Request(1), "First tray request should start its lazy worker");
    Check(started.wait_for(5s) == std::future_status::ready, "Worker failed to enter blocked shell work");
    Result result;
    Check(!worker.Take(result), "A blocked shell call is not a registered tray");
    const auto before = std::chrono::steady_clock::now();
    for (uint64_t generation = 2; generation <= 2000; ++generation)
        Check(worker.Request(generation), "Explorer generations must coalesce without blocking their caller");
    Check(std::chrono::steady_clock::now() - before < 1s && active == 1,
        "Pending requests must return while shell work is blocked (not a PC startup timing test)");
    unblock.Now();
    Check(Wait([&] { return worker.Take(result) && result.generation == 2000; }), "Newest Explorer generation must eventually complete");
    Check(result.ready && calls == 2 && maximum == 1 && off_owner,
        "Tray work must remain serial, coalesced and separate from the widget owner thread");
    worker.Stop(); worker.Stop();
    Check(!worker.Request(2001) && !worker.Take(result), "Stopped workers must neither restart nor deliver stale results");
}
void CancelAndJoin() {
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto gate = release.get_future().share();
    std::atomic<unsigned> calls{0};
    Worker worker([&] { ++calls; entered.set_value(); gate.wait(); return true; });
    Release unblock{release};
    Check(worker.Request(1) && started.wait_for(5s) == std::future_status::ready, "Cancellation test must own one blocked call");
    Check(worker.Request(2), "Cancellation test needs a queued replacement request");
    worker.Cancel(); // WM_DESTROY uses this nonblocking half, before history flush.
    auto stopped = std::async(std::launch::async, [&] { worker.Stop(); });
    const bool joined_early = stopped.wait_for(20ms) == std::future_status::ready;
    // Release before asserting: std::future's destructor also joins async work.
    unblock.Now();
    Check(!joined_early, "Stop must join in-flight native work, never detach it");
    Check(stopped.wait_for(5s) == std::future_status::ready, "Stop did not finish after its owned call returned");
    stopped.get();
    Result result;
    Check(calls == 1 && !worker.Take(result) && !worker.Request(3), "Shutdown must discard queued work and late completion");
}
void WorkFailure() {
    unsigned calls = 0;
    Worker worker([&] { if (++calls == 1) throw std::runtime_error("owned shell failure"); return true; });
    Result result;
    Check(!worker.Request(0), "A missing request generation must be rejected");
    Check(worker.Request(1) && Wait([&] { return worker.Take(result); }) && result.generation == 1 && !result.ready,
        "Exceptions must become a failed tray result, not terminate the process");
    Check(worker.Request(2) && Wait([&] { return worker.Take(result); }) && result.generation == 2 && result.ready,
        "The owned worker must remain usable after a recoverable call failure");
}
}
int main() {
    try {
        Schedule(); OwnedWorker(); CancelAndJoin(); WorkFailure();
        std::cout << "Tray registration policy/worker checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Tray registration check " << checks << " failed: " << error.what() << '\n';
        return 1;
    }
}
