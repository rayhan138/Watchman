#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace WatchmanUi {
// The 1Hz telemetry timer is independent. Do not wake at 60Hz when the visible
// main window is settled; pointer work is coalesced onto the next animation tick.
inline unsigned FrameTimerInterval(bool visible, bool minimized, bool foreground,
                                   bool animation, bool pointer_pending, bool live_transfer) {
    if (!visible || minimized || !foreground) return 0;
    if (animation || pointer_pending) return 16;
    return live_transfer ? 100 : 0;
}

// Fixed-per-paint lerps slow down on a busy/low-refresh display and accelerate
// with high-rate mouse paints. Use elapsed time instead. A long hidden/resume
// gap snaps to current values rather than replaying stale animations.
class FrameClock {
public:
    float Advance(uint64_t now) {
        const uint64_t elapsed = !started_ || now < previous_ ? 16 : now - previous_;
        previous_ = now;
        started_ = true;
        return elapsed >= 1000 ? 1.0f : static_cast<float>(-std::expm1(-static_cast<double>(elapsed) / 100.0));
    }
    void Reset() { *this = {}; }
private:
    bool started_ = false;
    uint64_t previous_ = 0;
};

// First and recovery frames use the independent GDI renderer. Cold D3D/DWrite
// initialization must not delay first visibility or the tray/message loop.
// A failed GDI attempt permits an immediate GPU trial; otherwise defer the
// optional GPU setup until a quiet frame after 1.5s. This is not a backend wait.
class WidgetFirstFrame {
public:
    bool PreferGdi(uint64_t now, bool quiet) {
        if (gpu_started_) return false;
        if (attempted_ && (!accepted_ || (quiet && now >= first_at_ && now - first_at_ >= 1500))) {
            gpu_started_ = true;
            return false;
        }
        return true;
    }
    void GdiResult(uint64_t now, bool accepted) {
        if (!attempted_) first_at_ = now;
        attempted_ = true;
        accepted_ = accepted;
    }
    void Reset() { *this = {}; }
private:
    bool attempted_ = false, accepted_ = false, gpu_started_ = false;
    uint64_t first_at_ = 0;
};
} // namespace WatchmanUi
