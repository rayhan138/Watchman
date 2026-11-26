#include "update_checker.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace WatchmanUpdates;
using namespace std::chrono_literals;
namespace {
unsigned checks = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
template<class F> bool Wait(F f) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    do { if (f()) return true; std::this_thread::sleep_for(1ms); } while (std::chrono::steady_clock::now() < end);
    return false;
}
void Test() {
    std::atomic<unsigned> calls{0};
    std::atomic<bool> entered{false}, release{false};
    Worker worker([&](const std::atomic<bool>& stopping) {
        ++calls; entered = true;
        while (!release && !stopping) std::this_thread::sleep_for(1ms);
        return Result{Status::NoRelease, {}, 0};
    });
    Check(calls == 0, "Worker construction must be lazy, not an early network request");
    Check(worker.Request(), "First request should start a worker");
    Check(Wait([&] { return entered.load(); }), "Fake backend did not enter");
    for (int i = 0; i < 100; ++i) Check(!worker.Request(), "A blocked check must not spawn more work");
    Result result;
    Check(!worker.Take(result), "Pending data must not masquerade as a completed check");
    release = true;
    Check(Wait([&] { return worker.Take(result); }), "Result was not delivered to the mailbox");
    Check(result.status == Status::NoRelease && calls == 1, "Mailbox must retain the actual backend result");
    Check(!worker.Take(result), "Results should be consumed once");
    Check(worker.Request(), "Owned worker should accept a later check");
    Check(Wait([&] { return calls == 2; }), "Second check did not execute");
    Check(Wait([&] { return worker.Take(result); }), "Second result missing");
    worker.Stop(); worker.Stop();
    Check(!worker.Request(), "Stop must be idempotent and final");
    Worker failing([](const std::atomic<bool>&) -> Result { throw std::runtime_error("injected"); });
    Check(failing.Request(), "Throwing backend should still be schedulable");
    Check(Wait([&] { return failing.Take(result); }) && result.status == Status::Unavailable,
        "Exception must become an honest check failure, not terminate the process");
    std::atomic<bool> blocked{false}, returned{false};
    {
        Worker owned([&](const std::atomic<bool>& stop) {
            blocked = true;
            while (!stop) std::this_thread::sleep_for(1ms);
            returned = true;
            return Result{Status::Unavailable, {}, 1223};
        });
        Check(owned.Request() && Wait([&] { return blocked.load(); }), "Cancellation fixture did not enter");
        owned.Stop();
        Check(returned, "Stop must join cooperative work, never detach a live owner");
        Check(!owned.Take(result), "Shutdown must not deliver a stale callback/result");
    }
    for (int i = 0; i < 50; ++i) {
        Worker short_lived([](const std::atomic<bool>&) { return Result{Status::UpToDate, {}, 0}; });
        Check(short_lived.Request(), "Short-lived worker failed");
    }
}
}
int main() {
    try { Test(); std::cout << "Owned update worker: " << checks << " checks passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
