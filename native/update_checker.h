#pragma once

#include "update_policy.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace WatchmanUpdates {
Result CheckOfficialRelease(const std::atomic<bool>& stopping);
#ifdef WATCHMAN_UPDATE_CHECKING_TESTING
// Compiled only into the owned-loopback CTest, never the shipped executable.
Result CheckLoopbackReleaseForTest(unsigned short port, const std::atomic<bool>& stopping);
#endif

// Lazy, owned, serial worker. Results contain values only, never HWND/renderer
// pointers; the UI polls the mailbox. Stop discards pending work and joins.
class Worker {
public:
    using Check = std::function<Result(const std::atomic<bool>&)>;
    explicit Worker(Check check) : check_(std::move(check)) {}
    ~Worker() { Stop(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    bool Request();
    bool Take(Result& result);
    void Stop();
private:
    void Run();
    Check check_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread thread_;
    std::optional<Result> result_;
    bool requested_ = false, busy_ = false;
};
} // namespace WatchmanUpdates
