#pragma once
#include "widget_runtime.h"
#include <algorithm>
#include <cstdint>

namespace WatchmanWidgetRuntime {
// The cheap command-poll timer may keep ticking; expensive shell discovery and
// positioning run only when due. Explicit close/enable/shell/display events wake it.
class RetrySchedule {
public:
    bool Due(uint64_t now) const { return now >= next_; }
    void Wake() { next_ = 0; failures_ = 0; }
    void Complete(Result result, uint64_t now) {
        if (result.status != previous_.status) failures_ = 0;
        uint64_t delay = 100;
        if (result.status == Status::Ready || result.status == Status::HiddenFullscreen) delay = 500;
        else if (result.status == Status::Disabled) delay = 2000;
        else if (result.status != Status::WaitingForFrame) {
            delay = (std::min)(uint64_t{2000}, uint64_t{100} << (std::min)(failures_, 5u));
            failures_ = (std::min)(failures_ + 1, 5u);
        }
        previous_ = result;
        next_ = now + delay;
    }
    uint64_t Next() const { return next_; }
private:
    Result previous_{};
    uint64_t next_ = 0;
    unsigned failures_ = 0;
};

// Keep a working GDI path. Probe the GPU only after 3 accepted fallback frames,
// an initial 30s cooldown, and a quiet UI; repeated failures back off to 5 min.
class GpuRetryPolicy {
public:
    void Reset() { *this = {}; }
    void Failed(uint64_t now) {
        const uint64_t delay = (std::min)(uint64_t{300000}, uint64_t{30000} << (std::min)(failures_, 4u));
        failures_ = (std::min)(failures_ + 1, 5u);
        next_ = now + delay;
        healthy_frames_ = 0;
    }
    void GdiFrame(bool accepted) { healthy_frames_ = accepted ? (std::min)(healthy_frames_ + 1, 3u) : 0; }
    bool Due(uint64_t now, bool quiet = true) const { return quiet && failures_ && healthy_frames_ >= 3 && now >= next_; }
    uint64_t Next() const { return next_; }
private:
    unsigned failures_ = 0, healthy_frames_ = 0;
    uint64_t next_ = 0;
};

// DPI/display/work-area notifications arrive in bursts. Settle for 200ms, but
// never postpone the recovery forever under a stream of notifications.
class DisplayRecovery {
public:
    void Request(uint64_t now, bool reset_graphics) {
        if (!pending_) first_ = now;
        pending_ = true;
        reset_ = reset_ || reset_graphics;
        due_ = (std::min)(now + 200, first_ + 1000);
    }
    bool Due(uint64_t now) const { return pending_ && now >= due_; }
    bool Consume() { const bool reset = reset_; pending_ = reset_ = false; return reset; }
    bool Pending() const { return pending_; }
private:
    bool pending_ = false, reset_ = false;
    uint64_t first_ = 0, due_ = 0;
};
} // namespace WatchmanWidgetRuntime
