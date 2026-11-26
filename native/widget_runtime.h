#pragma once

#include <cstdint>

namespace WatchmanWidgetRuntime {
enum class Status {
    Disabled,
    WaitingForShell,
    WaitingForLayout,
    UnsupportedLayout,
    NoSpace,
    AttachFailed,
    CleanupPending,
    HiddenFullscreen,
    WaitingForFrame,
    RenderFailed,
    Ready
};

struct Result {
    Status status = Status::WaitingForShell;
    uint32_t error = 0;
    bool operator==(const Result& other) const { return status == other.status && error == other.error; }
    bool operator!=(const Result& other) const { return !(*this == other); }
};

inline const char* EventName(Status status) {
    switch (status) {
    case Status::Disabled: return "disabled";
    case Status::WaitingForShell: return "waiting-for-taskbar";
    case Status::WaitingForLayout: return "waiting-for-layout";
    case Status::UnsupportedLayout: return "unsupported-taskbar-bounds";
    case Status::NoSpace: return "no-safe-space";
    case Status::AttachFailed: return "attachment-failed";
    case Status::CleanupPending: return "cleanup-pending";
    case Status::HiddenFullscreen: return "hidden-fullscreen";
    case Status::WaitingForFrame: return "waiting-for-frame";
    case Status::RenderFailed: return "presentation-failed";
    case Status::Ready: return "ready";
    }
    return "unknown";
}

inline const wchar_t* Description(Status status) {
    switch (status) {
    case Status::Disabled: return L"Taskbar widget is turned off.";
    case Status::WaitingForShell: return L"Waiting for Windows to create the taskbar.";
    case Status::WaitingForLayout: return L"Waiting for Windows to expose usable taskbar bounds.";
    case Status::UnsupportedLayout: return L"This taskbar layout does not expose supported safe bounds. Retrying without overlap.";
    case Status::NoSpace: return L"No safe taskbar space is available without covering Windows controls.";
    case Status::AttachFailed: return L"Windows could not attach or position the taskbar widget. Retrying.";
    case Status::CleanupPending: return L"Waiting to finish restoring the previous taskbar layout.";
    case Status::HiddenFullscreen: return L"Taskbar widget is hidden while a fullscreen window is active.";
    case Status::WaitingForFrame: return L"Taskbar attached; waiting for the first successful frame.";
    case Status::RenderFailed: return L"Windows did not accept the widget frame. Retrying.";
    case Status::Ready: return L"Taskbar widget is attached and has a successful frame.";
    }
    return L"Taskbar status is unavailable.";
}

// Missing geometry is NOT proof of free space. A positive result permits
// attachment only; callers still have to check SetParent/SetWindowPos/paint.
inline Status CheckGap(bool required_bounds_valid, int taskbar_width, int taskbar_height,
                       int widget_width, int widget_height, int leading, int trailing,
                       bool horizontal, int margins = 4) {
    if (!required_bounds_valid || taskbar_width <= 0 || taskbar_height <= 0)
        return Status::WaitingForLayout;
    if (widget_width <= 0 || widget_height <= 0) return Status::AttachFailed;
    const int extent = horizontal ? taskbar_width : taskbar_height;
    if (leading < 0 || trailing < 0 || leading > extent || trailing > extent)
        return Status::UnsupportedLayout;
    if (widget_width > taskbar_width || widget_height > taskbar_height ||
        static_cast<int64_t>(trailing) - leading <
            static_cast<int64_t>(horizontal ? widget_width : widget_height) + margins)
        return Status::NoSpace;
    return Status::WaitingForFrame;
}

// Shared by the real window host and tests. Invalid (including non-null stale)
// handles are replaced. Disabled widgets and deliberate shutdown never spawn.
template<class Handle, class IsValid, class Create>
bool EnsureHost(Handle& handle, bool enabled, bool stopping, IsValid valid, Create create) {
    if (!enabled || stopping) return false;
    if (handle && valid(handle)) return true;
    handle = Handle{};
    handle = create();
    return handle && valid(handle);
}

// A cached value is not a submitted frame after a shell reset, reparent, show
// or device recreation. This explicit invalidation must bypass value equality.
class FrameValidity {
public:
    bool CanReuse(bool values_equal) const { return valid_ && values_equal; }
    void Presented() { valid_ = true; }
    void Invalidate() { valid_ = false; }
private:
    bool valid_ = false;
};

enum class Backend { None, Composition, Gdi };
struct PaintResult {
    bool success = false;
    Backend backend = Backend::None;
    uint32_t error = 0;
    bool submitted = false;
};

// GetLastError is captured by the platform adapter immediately after a failed
// API call, not after DC/bitmap cleanup has overwritten it.
inline PaintResult LayeredResult(bool accepted, uint32_t error) {
    return {accepted, Backend::Gdi, accepted ? 0u : error, accepted};
}
} // namespace WatchmanWidgetRuntime
