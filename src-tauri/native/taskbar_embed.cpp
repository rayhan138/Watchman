#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace {

constexpr int kMinWidgetHeight = 24;

struct TaskbarPlacement {
    int x;
    int y;
    int width;
    int height;
    int edge;
};

struct TaskbarHandles {
    HWND taskbar;
    HWND bar;
    HWND min;
    HWND tray;
    bool legacy_layout;
};

struct SavedTaskbarLayout {
    HWND bar = nullptr;
    HWND min = nullptr;
    RECT original_min_client{0, 0, 0, 0};
    RECT original_min_screen{0, 0, 0, 0};
    bool captured = false;
    bool active = false;
};

SavedTaskbarLayout g_layout;

int RectWidth(const RECT& rect) {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
    return rect.bottom - rect.top;
}

bool ReadWindowRect(HWND hwnd, RECT* rect) {
    return hwnd != nullptr && rect != nullptr && GetWindowRect(hwnd, rect) != FALSE;
}

bool IsClassName(HWND hwnd, const wchar_t* first, const wchar_t* second = nullptr) {
    wchar_t buffer[256] = {};
    const int length = GetClassNameW(
        hwnd,
        buffer,
        static_cast<int>(sizeof(buffer) / sizeof(buffer[0]))
    );
    if (length <= 0) {
        return false;
    }

    if (first != nullptr && lstrcmpiW(buffer, first) == 0) {
        return true;
    }

    return second != nullptr && lstrcmpiW(buffer, second) == 0;
}

HWND FindDescendantByClass(HWND parent, const wchar_t* first, const wchar_t* second = nullptr) {
    if (parent == nullptr) {
        return nullptr;
    }

    HWND child = nullptr;
    while ((child = FindWindowExW(parent, child, nullptr, nullptr)) != nullptr) {
        if (IsClassName(child, first, second)) {
            return child;
        }

        if (HWND nested = FindDescendantByClass(child, first, second)) {
            return nested;
        }
    }

    return nullptr;
}

bool InitTaskbarWindows(TaskbarHandles* handles) {
    if (handles == nullptr) {
        return false;
    }

    handles->taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (handles->taskbar == nullptr) {
        return false;
    }

    HWND rebar = FindWindowExW(handles->taskbar, nullptr, L"ReBarWindow32", nullptr);
    const bool has_modern_bridge =
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Composition.DesktopWindowContentBridge") != nullptr ||
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Input.InputSite.WindowClass") != nullptr ||
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Core.CoreWindow") != nullptr;
    handles->legacy_layout = rebar != nullptr && !has_modern_bridge;

    handles->bar = rebar;
    if (handles->bar == nullptr) {
        handles->bar = FindWindowExW(handles->taskbar, nullptr, L"WorkerW", nullptr);
    }
    if (handles->bar == nullptr) {
        handles->bar = handles->taskbar;
    }

    handles->min = FindDescendantByClass(handles->bar, L"MSTaskSwWClass", L"MSTaskListWClass");
    if (handles->min == nullptr && handles->bar != handles->taskbar) {
        handles->min = FindDescendantByClass(handles->taskbar, L"MSTaskSwWClass", L"MSTaskListWClass");
    }

    handles->tray = FindDescendantByClass(handles->taskbar, L"TrayNotifyWnd");
    if (handles->tray == nullptr && handles->bar != handles->taskbar) {
        handles->tray = FindDescendantByClass(handles->bar, L"TrayNotifyWnd");
    }

    return handles->min != nullptr || handles->tray != nullptr;
}

bool GetRectInParent(HWND child, RECT* rect) {
    if (!ReadWindowRect(child, rect)) {
        return false;
    }

    HWND parent = GetParent(child);
    if (parent == nullptr) {
        return false;
    }

    return MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(rect), 2) != 0;
}

bool CaptureOriginalLayout(const TaskbarHandles& handles) {
    if (g_layout.captured && g_layout.bar == handles.bar && g_layout.min == handles.min) {
        return true;
    }

    RECT min_client = {};
    RECT min_screen = {};
    if (!GetRectInParent(handles.min, &min_client) || !ReadWindowRect(handles.min, &min_screen)) {
        return false;
    }

    g_layout.bar = handles.bar;
    g_layout.min = handles.min;
    g_layout.original_min_client = min_client;
    g_layout.original_min_screen = min_screen;
    g_layout.captured = true;
    g_layout.active = false;
    return true;
}

bool RestoreTaskbarLayoutInternal() {
    if (!g_layout.captured || g_layout.min == nullptr || !IsWindow(g_layout.min)) {
        return false;
    }

    const RECT& rect = g_layout.original_min_client;
    const BOOL moved = MoveWindow(
        g_layout.min,
        rect.left,
        rect.top,
        std::max(1, RectWidth(rect)),
        std::max(1, RectHeight(rect)),
        TRUE
    );

    g_layout.active = false;
    return moved != FALSE;
}

bool IsFullscreenWindow(HWND foreground) {
    if (foreground == nullptr || !IsWindowVisible(foreground) || IsIconic(foreground)) {
        return false;
    }

    RECT window_rect = {};
    if (!ReadWindowRect(foreground, &window_rect)) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return false;
    }

    MONITORINFO monitor_info = {};
    monitor_info.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
        return false;
    }

    constexpr int tolerance = 2;
    return std::abs(window_rect.left - monitor_info.rcMonitor.left) <= tolerance &&
           std::abs(window_rect.top - monitor_info.rcMonitor.top) <= tolerance &&
           std::abs(window_rect.right - monitor_info.rcMonitor.right) <= tolerance &&
           std::abs(window_rect.bottom - monitor_info.rcMonitor.bottom) <= tolerance;
}

int ClampDimension(int preferred, int minimum, int maximum) {
    const int safe_maximum = std::max(1, maximum);
    const int safe_minimum = std::min(minimum, safe_maximum);
    return std::clamp(preferred, safe_minimum, safe_maximum);
}

bool ApplyModernPlacement(
    const TaskbarHandles& handles,
    HWND hwnd,
    int preferred_width,
    int preferred_height,
    TaskbarPlacement* placement
) {
    RECT taskbar_screen = {};
    if (!ReadWindowRect(handles.taskbar, &taskbar_screen)) {
        return false;
    }

    RECT min_screen = taskbar_screen;
    if (handles.min != nullptr) {
        ReadWindowRect(handles.min, &min_screen);
    }

    RECT tray_screen = taskbar_screen;
    if (handles.tray != nullptr) {
        ReadWindowRect(handles.tray, &tray_screen);
    }

    const int taskbar_width = RectWidth(taskbar_screen);
    const int taskbar_height = RectHeight(taskbar_screen);
    const bool horizontal = taskbar_width >= taskbar_height;

    if (horizontal) {
        const int leading = static_cast<int>(handles.min != nullptr ? min_screen.right : taskbar_screen.left);
        const int trailing = static_cast<int>(handles.tray != nullptr ? tray_screen.left : taskbar_screen.right);
        const int available_width = std::max(1, trailing - leading - 12);
        const int widget_width = ClampDimension(preferred_width, 1, available_width);
        const int widget_height = ClampDimension(preferred_height, kMinWidgetHeight, taskbar_height);

        placement->width = widget_width;
        placement->height = widget_height;
        placement->x = std::clamp(
            trailing - widget_width - 6,
            leading + 6,
            static_cast<int>(taskbar_screen.right) - widget_width - 6
        );
        placement->y = taskbar_screen.top + std::max(0, (taskbar_height - widget_height) / 2);
        placement->edge = taskbar_screen.top <= 0 ? 1 : 0;
    } else {
        const int leading = static_cast<int>(handles.min != nullptr ? min_screen.bottom : taskbar_screen.top);
        const int trailing = static_cast<int>(handles.tray != nullptr ? tray_screen.top : taskbar_screen.bottom);
        const int available_height = std::max(1, trailing - leading - 12);
        const int widget_width = ClampDimension(preferred_width, 1, taskbar_width);
        const int widget_height = ClampDimension(preferred_height, kMinWidgetHeight, available_height);

        placement->width = widget_width;
        placement->height = widget_height;
        placement->x = taskbar_screen.left + std::max(0, (taskbar_width - widget_width) / 2);
        placement->y = std::clamp(
            trailing - widget_height - 6,
            leading + 6,
            static_cast<int>(taskbar_screen.bottom) - widget_height - 6
        );
        placement->edge = taskbar_screen.left <= 0 ? 2 : 3;
    }

    return SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        placement->x,
        placement->y,
        placement->width,
        placement->height,
        SWP_NOACTIVATE
    ) != FALSE;
}

}  // namespace

extern "C" {

int tm_apply_widget_styles(void* widget_hwnd) {
    HWND hwnd = static_cast<HWND>(widget_hwnd);
    if (hwnd == nullptr) {
        return 0;
    }

    const LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    const LONG target_style = ex_style |
        WS_EX_LAYERED |
        WS_EX_NOACTIVATE |
        WS_EX_TOOLWINDOW;

    if (ex_style != target_style) {
        SetWindowLongW(hwnd, GWL_EXSTYLE, target_style);
    }

    return SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED
    ) != FALSE;
}

int tm_embed_widget(void* widget_hwnd, int preferred_width, int preferred_height, TaskbarPlacement* placement) {
    HWND hwnd = static_cast<HWND>(widget_hwnd);
    if (hwnd == nullptr || placement == nullptr) {
        return 0;
    }

    TaskbarHandles handles{};
    if (!InitTaskbarWindows(&handles)) {
        return 0;
    }

    if (g_layout.active && (g_layout.bar != handles.bar || g_layout.min != handles.min)) {
        RestoreTaskbarLayoutInternal();
        g_layout.captured = false;
    }

    if (!CaptureOriginalLayout(handles)) {
        return 0;
    }

    if (!handles.legacy_layout) {
        RestoreTaskbarLayoutInternal();
        const bool placed = ApplyModernPlacement(
            handles,
            hwnd,
            preferred_width,
            preferred_height,
            placement
        );
        g_layout.active = false;
        return placed ? 1 : 0;
    }

    RECT bar_screen = {};
    if (!ReadWindowRect(handles.bar, &bar_screen)) {
        return 0;
    }

    const RECT min_client = g_layout.original_min_client;
    const RECT min_screen = g_layout.original_min_screen;
    const int bar_width = RectWidth(bar_screen);
    const int bar_height = RectHeight(bar_screen);
    const bool horizontal = bar_width >= bar_height;

    if (horizontal) {
        const int widget_width = ClampDimension(preferred_width, 1, RectWidth(min_screen));
        const int widget_height = ClampDimension(preferred_height, kMinWidgetHeight, bar_height);
        const int new_width = std::max(1, RectWidth(min_client) - widget_width);

        if (!MoveWindow(
                handles.min,
                min_client.left,
                min_client.top,
                new_width,
                std::max(1, RectHeight(min_client)),
                TRUE)) {
            return 0;
        }

        placement->width = widget_width;
        placement->height = widget_height;
        placement->x = min_screen.right - widget_width;
        placement->y = bar_screen.top + std::max(0, (bar_height - widget_height) / 2);
        placement->edge = bar_screen.top <= 0 ? 1 : 0;
    } else {
        const int widget_width = ClampDimension(preferred_width, 1, bar_width);
        const int widget_height = ClampDimension(preferred_height, 1, RectHeight(min_screen));
        const int new_height = std::max(1, RectHeight(min_client) - widget_height);

        if (!MoveWindow(
                handles.min,
                min_client.left,
                min_client.top,
                std::max(1, RectWidth(min_client)),
                new_height,
                TRUE)) {
            return 0;
        }

        placement->width = widget_width;
        placement->height = widget_height;
        placement->x = bar_screen.left + std::max(0, (bar_width - widget_width) / 2);
        placement->y = min_screen.bottom - widget_height;
        placement->edge = bar_screen.left <= 0 ? 2 : 3;
    }

    if (!SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            placement->x,
            placement->y,
            placement->width,
            placement->height,
            SWP_NOACTIVATE)) {
        return 0;
    }

    g_layout.active = true;
    return 1;
}

int tm_restore_taskbar_layout() {
    return RestoreTaskbarLayoutInternal() ? 1 : 0;
}

int tm_should_widget_be_visible(void* widget_hwnd, void* main_hwnd, int* visible) {
    if (visible == nullptr) {
        return 0;
    }

    HWND widget = static_cast<HWND>(widget_hwnd);
    HWND main = static_cast<HWND>(main_hwnd);
    HWND foreground = GetForegroundWindow();

    if (foreground == nullptr ||
        foreground == widget ||
        (main != nullptr && foreground == main) ||
        foreground == FindWindowW(L"Shell_TrayWnd", nullptr)) {
        *visible = 1;
        return 1;
    }

    *visible = IsFullscreenWindow(foreground) ? 0 : 1;
    return 1;
}

}  // extern "C"
