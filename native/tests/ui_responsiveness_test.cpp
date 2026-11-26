#include "ui_responsiveness.h"
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
float Animate(unsigned interval) {
    WatchmanUi::FrameClock clock;
    clock.Advance(1000);
    float value = 0;
    for (unsigned elapsed = interval; elapsed <= 800; elapsed += interval)
        value += (100 - value) * clock.Advance(1000 + elapsed);
    return value;
}
}
int main() {
    using namespace WatchmanUi;
    try {
        for (int flags = 0; flags < 64; ++flags) {
            const bool visible = flags & 1, minimized = flags & 2, foreground = flags & 4;
            const bool animation = flags & 8, pointer = flags & 16, transfer = flags & 32;
            const unsigned expected = !visible || minimized || !foreground ? 0 :
                animation || pointer ? 16 : transfer ? 100 : 0;
            Check(FrameTimerInterval(visible, minimized, foreground, animation, pointer, transfer) == expected,
                  "Main animation timer must be event-driven, not an idle 60Hz wakeup");
        }
        Check(std::abs(Animate(8) - Animate(16)) < 0.0001f &&
              std::abs(Animate(16) - Animate(40)) < 0.0001f &&
              std::abs(Animate(40) - Animate(100)) < 0.0001f,
              "Animation duration changed with frame/input rate");
        FrameClock clock;
        Check(clock.Advance(50) > 0 && clock.Advance(50) == 0, "Duplicate paints must not advance time-based animation");
        Check(clock.Advance(5000) == 1, "A long hidden/resume gap must not replay stale gauge transitions");
        Check(std::isfinite(clock.Advance(1)), "Clock rollback must not produce invalid geometry");
        WidgetFirstFrame widget;
        Check(widget.PreferGdi(100, true), "First widget paint must not initialize Direct3D");
        widget.GdiResult(100, true);
        Check(widget.PreferGdi(1599, true) && widget.PreferGdi(1600, false),
              "Healthy first paint must defer optional GPU initialization until a quiet frame");
        Check(!widget.PreferGdi(1600, true) && !widget.PreferGdi(1700, false),
              "The GPU trial should run once, without introducing a permanent GDI latch");
        widget.Reset();
        Check(widget.PreferGdi(2000, false), "A replacement Explorer host needs a fast independent first frame");
        widget.GdiResult(2000, false);
        Check(!widget.PreferGdi(2001, false), "Failed first GDI paint must permit an immediate GPU recovery attempt");
        std::cout << "UiResponsiveness checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UiResponsiveness check " << checks << " failed: " << error.what() << '\n';
        return 1;
    }
}
