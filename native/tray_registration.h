#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace WatchmanTray {
struct Result {
    uint64_t generation = 0;
    bool ready = false;
};

// UI-owned state. Explorer readiness is independent of dashboard visibility
// and widget attachment. Never turn a failed registration into a window show.
class RetrySchedule {
public:
    void Invalidate() {
        ++generation_;
        ready_ = pending_ = false;
        failures_ = 0;
        next_ = 0;
    }
    bool Due(uint64_t now) const { return !ready_ && !pending_ && now >= next_; }
    uint64_t Begin(uint64_t now) {
        if (!Due(now)) return 0;
        pending_ = true;
        return generation_;
    }
    bool Complete(Result result, uint64_t now) {
        // An in-flight call from the previous Explorer must not mark its
        // replacement ready, or postpone the replacement's pending retry.
        if (!pending_ || result.generation != generation_) return false;
        pending_ = false;
        ready_ = result.ready;
        if (!ready_) {
            next_ = now + (std::min)(uint64_t{2000}, uint64_t{100} << (std::min)(failures_, 5u));
            failures_ = (std::min)(failures_ + 1, 5u);
        }
        return true;
    }
    bool Ready() const { return ready_; }
    bool Pending() const { return pending_; }
    uint64_t Next() const { return next_; }
private:
    uint64_t generation_ = 1, next_ = 0;
    unsigned failures_ = 0;
    bool ready_ = false, pending_ = false;
};

// A lazy, owned, serial shell worker. An unresponsive Shell_NotifyIcon must not
// hold the widget/message-loop thread. Explorer broadcasts coalesce to the
// newest generation; no detached work, HWND posts or renderer pointers escape.
class Worker {
public:
    using Work = std::function<bool()>;
    explicit Worker(Work work) : work_(std::move(work)) {}
    ~Worker() { Stop(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    bool Request(uint64_t generation);
    bool Take(Result& result);
    // Nonblocking cancellation during WM_DESTROY. Stop joins after the engine
    // has flushed history; Windows shell calls have no hard cancellation bound.
    void Cancel();
    void Stop();
private:
    void Run();
    Work work_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    std::optional<uint64_t> pending_;
    std::optional<Result> result_;
    bool stopping_ = false;
};
} // namespace WatchmanTray
