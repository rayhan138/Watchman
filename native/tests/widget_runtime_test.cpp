#include "widget_runtime.h"

#include <climits>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace WatchmanWidgetRuntime;
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void SafePlacement() {
    Check(CheckGap(false, 1600, 48, 148, 28, 0, 1600, true) == Status::WaitingForLayout,
        "Missing bounds must not be interpreted as empty space");
    Check(CheckGap(true, 0, 48, 148, 28, 0, 0, true) == Status::WaitingForLayout,
        "A not-yet-sized taskbar must wait for layout");
    Check(CheckGap(true, 1600, 48, 148, 28, 1400, 1450, true) == Status::NoSpace,
        "An insufficient gap must remain hidden");
    Check(CheckGap(true, 1600, 48, 148, 28, 1600, 1450, true) == Status::NoSpace,
        "A full-width task list must not be overlapped");
    Check(CheckGap(true, 1600, 48, 148, 28, 1700, 1450, true) == Status::UnsupportedLayout,
        "Bounds outside the taskbar cannot certify an attachment");
    Check(CheckGap(true, 1600, 48, 148, 28, 1000, -5, true) == Status::UnsupportedLayout,
        "Negative tray bounds must not place an off-taskbar overlay");
    Check(CheckGap(true, 1600, 48, 0, 28, 0, 1600, true) == Status::AttachFailed,
        "Invalid requested sizes are errors, not shell readiness");
    Check(CheckGap(true, 1600, 20, 148, 28, 0, 1600, true) == Status::NoSpace,
        "Cross-axis space is mandatory");
    Check(CheckGap(true, 48, 1000, 28, 148, 500, 800, false) == Status::WaitingForFrame,
        "Vertical taskbars must use vertical gap length");
    Check(CheckGap(true, 48, 1000, 60, 148, 500, 800, false) == Status::NoSpace,
        "Vertical taskbar cross-axis overflow must remain hidden");
    Check(CheckGap(true, INT_MAX, 48, INT_MAX, 28, 0, INT_MAX, true) == Status::NoSpace,
        "Margins must not overflow and falsely allow a full-width widget");
    for (int gap = 0; gap < 400; ++gap) {
        const auto result = CheckGap(true, 1600, 48, 148, 28, 1000, 1000 + gap, true);
        Check(result == (gap >= 152 ? Status::WaitingForFrame : Status::NoSpace),
            "Gap boundary must retain both safety margins");
    }
}

void HostAndFrameRecovery() {
    int handle = 0;
    int creations = 0;
    auto valid = [](int value) { return value >= 10; };
    auto create = [&] { ++creations; return 10 + creations; };
    Check(!EnsureHost(handle, false, false, valid, create) && creations == 0,
        "A disabled widget must not spawn a host");
    Check(!EnsureHost(handle, true, true, valid, create) && creations == 0,
        "App teardown must not spawn a host");
    Check(EnsureHost(handle, true, false, valid, create) && creations == 1,
        "An absent host must be created");
    Check(EnsureHost(handle, true, false, valid, create) && creations == 1,
        "A valid host must be reused");
    handle = 4; // Non-null is insufficient: models a destroyed/reused HWND.
    Check(EnsureHost(handle, true, false, valid, create) && creations == 2,
        "A stale non-null host must be replaced");
    handle = 0;
    Check(!EnsureHost(handle, true, false, valid, [] { return 0; }) && handle == 0,
        "Window creation failure must not be published as a usable host");

    FrameValidity frame;
    Check(!frame.CanReuse(true), "Before presentation even equal values require drawing");
    frame.Presented();
    Check(frame.CanReuse(true) && !frame.CanReuse(false), "Only an already-presented identical frame may be skipped");
    for (int recovery = 0; recovery < 4; ++recovery) {
        // Shell recreation, reparent, DPI/display change and hide/show.
        frame.Invalidate();
        Check(!frame.CanReuse(true), "Recovery must repaint unchanged values");
        frame.Presented();
        Check(frame.CanReuse(true), "A newly accepted frame may be reused");
    }
}

void PresentationAndStatus() {
    const auto ok = LayeredResult(true, 5);
    Check(ok.success && ok.submitted && ok.error == 0 && ok.backend == Backend::Gdi,
        "A successful fallback must be identified separately from GPU success");
    const auto fail = LayeredResult(false, 1400);
    Check(!fail.success && !fail.submitted && fail.error == 1400 && fail.backend == Backend::Gdi,
        "A rejected fallback must retain its captured Windows error");
    std::set<std::string> events;
    for (const auto status : {Status::Disabled, Status::WaitingForShell, Status::WaitingForLayout,
            Status::UnsupportedLayout, Status::NoSpace, Status::AttachFailed, Status::CleanupPending,
            Status::HiddenFullscreen, Status::WaitingForFrame, Status::RenderFailed, Status::Ready}) {
        Check(std::wstring(Description(status)).size() > 10, "Every outcome needs a readable explanation");
        Check(events.insert(EventName(status)).second, "Diagnostics must distinguish every recovery outcome");
    }
    Check(Result{Status::AttachFailed, 5} == Result{Status::AttachFailed, 5}, "Equal transitions should not be logged repeatedly");
    Check(Result{Status::AttachFailed, 5} != Result{Status::AttachFailed, 1400}, "A changed error is a diagnostic transition");
}
} // namespace

int main() {
    try {
        SafePlacement();
        HostAndFrameRecovery();
        PresentationAndStatus();
        std::cout << "Widget runtime checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Widget runtime check " << checks << " failed: " << error.what() << '\n';
        return 1;
    }
}
