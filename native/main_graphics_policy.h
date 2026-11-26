#pragma once

#include <algorithm>
#include <cstdint>

namespace WatchmanUi {
// A cold dashboard may meet a temporarily unavailable graphics/font service.
// Retry on visible paints only, with a bounded cooldown, never at animation
// cadence and never as a prerequisite for independent widget presentation.
class MainGraphicsRetry {
public:
    bool Due(uint64_t now) const { return !failed_ || now >= next_; }
    void Failed(uint64_t now) {
        failed_ = true;
        next_ = now + (std::min)(uint64_t{30000}, uint64_t{1000} << (std::min)(failures_, 5u));
        failures_ = (std::min)(failures_ + 1, 5u);
    }
    void Succeeded() { *this = {}; }
    uint64_t Next() const { return next_; }
private:
    bool failed_ = false;
    unsigned failures_ = 0;
    uint64_t next_ = 0;
};
} // namespace WatchmanUi
