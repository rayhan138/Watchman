#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace {

constexpr int kMinWidgetHeight = 24;
constexpr int kWidgetMargin = 2;
constexpr int kTrayGap = 7;
constexpr int kMissingTrayFallbackWidth = 132;

struct TaskbarPlacement {
    int x;
    int y;
    int width;
    int height;
    int edge;
};

struct TaskbarHandles {
    HWND taskbar = nullptr;
    HWND parent = nullptr;
    HWND bar = nullptr;
    HWND min = nullptr;
    HWND tray = nullptr;
    HWND start = nullptr;
    bool legacy_layout = false;
    bool horizontal = true;
    RECT taskbar_screen{0, 0, 0, 0};
};

struct SavedTaskbarLayout {
    HWND bar = nullptr;
    HWND min = nullptr;
    RECT original_min_client{0, 0, 0, 0};
    RECT original_min_screen{0, 0, 0, 0};
    bool captured = false;
    bool active = false;

    HWND widget = nullptr;
    HWND widget_parent_before = nullptr;
    LONG_PTR widget_style_before = 0;
    WNDPROC widget_wndproc_before = nullptr;
    bool widget_state_captured = false;
    bool widget_embedded = false;
    bool widget_subclassed = false;
};

SavedTaskbarLayout g_layout;

LRESULT CALLBACK WidgetSubclassProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONUP:
        case WM_NCRBUTTONDBLCLK:
            return 0;
        default:
            break;
    }

    if (g_layout.widget_subclassed && g_layout.widget == hwnd && g_layout.widget_wndproc_before != nullptr) {
        return CallWindowProcW(g_layout.widget_wndproc_before, hwnd, message, w_param, l_param);
    }

    return DefWindowProcW(hwnd, message, w_param, l_param);
}

int RectWidth(const RECT& rect) {
    return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
    return rect.bottom - rect.top;
}

bool ReadWindowRect(HWND hwnd, RECT* rect) {
    return hwnd != nullptr && rect != nullptr && GetWindowRect(hwnd, rect) != FALSE;
}

int DetectTaskbarEdge(const RECT& rect) {
    if (RectWidth(rect) >= RectHeight(rect)) {
        return rect.top <= 0 ? 1 : 0;
    }

    return rect.left <= 0 ? 2 : 3;
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

bool GetRectInParent(HWND child, HWND parent, RECT* rect) {
    if (!ReadWindowRect(child, rect) || parent == nullptr) {
        return false;
    }

    return MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(rect), 2) != 0;
}

bool CaptureWidgetState(HWND widget) {
    if (widget == nullptr) {
        return false;
    }

    if (g_layout.widget_state_captured && g_layout.widget == widget) {
        return true;
    }

    g_layout.widget = widget;
    g_layout.widget_parent_before = GetParent(widget);
    g_layout.widget_style_before = GetWindowLongPtrW(widget, GWL_STYLE);
    g_layout.widget_wndproc_before = nullptr;
    g_layout.widget_state_captured = true;
    g_layout.widget_embedded = false;
    g_layout.widget_subclassed = false;
    return true;
}

bool RestoreWidgetSubclass() {
    if (!g_layout.widget_subclassed || g_layout.widget == nullptr || !IsWindow(g_layout.widget)) {
        g_layout.widget_subclassed = false;
        return false;
    }

    if (g_layout.widget_wndproc_before != nullptr) {
        SetWindowLongPtrW(
            g_layout.widget,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_layout.widget_wndproc_before)
        );
    }

    g_layout.widget_subclassed = false;
    g_layout.widget_wndproc_before = nullptr;
    return true;
}

bool EnsureWidgetSubclassed(HWND hwnd) {
    if (hwnd == nullptr) {
        return false;
    }

    if (g_layout.widget_subclassed && g_layout.widget == hwnd) {
        return true;
    }

    const auto current =
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (current == WidgetSubclassProc) {
        g_layout.widget = hwnd;
        g_layout.widget_subclassed = true;
        return true;
    }

    g_layout.widget_wndproc_before = current;
    const LONG_PTR result = SetWindowLongPtrW(
        hwnd,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(WidgetSubclassProc)
    );

    if (result == 0 && GetLastError() != 0) {
        return false;
    }

    g_layout.widget = hwnd;
    g_layout.widget_subclassed = true;
    return true;
}

void ApplyBaseWidgetStyles(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }

    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR target_ex_style =
        (ex_style | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;

    if (ex_style != target_ex_style) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, target_ex_style);
    }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
}

bool RestoreWidgetToTopLevel() {
    if (!g_layout.widget_state_captured || g_layout.widget == nullptr || !IsWindow(g_layout.widget)) {
        g_layout.widget_embedded = false;
        return false;
    }

    const HWND parent = g_layout.widget_parent_before;
    if (GetParent(g_layout.widget) != parent) {
        SetLastError(0);
        SetParent(g_layout.widget, parent);
        if (GetLastError() != 0) {
            return false;
        }
    }

    RestoreWidgetSubclass();
    SetWindowLongPtrW(g_layout.widget, GWL_STYLE, g_layout.widget_style_before);
    ApplyBaseWidgetStyles(g_layout.widget);
    SetWindowPos(
        g_layout.widget,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );

    g_layout.widget_embedded = false;
    return true;
}

bool InitTaskbarWindows(TaskbarHandles* handles) {
    if (handles == nullptr) {
        return false;
    }

    handles->taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (handles->taskbar == nullptr || !ReadWindowRect(handles->taskbar, &handles->taskbar_screen)) {
        return false;
    }

    HWND rebar = FindWindowExW(handles->taskbar, nullptr, L"ReBarWindow32", nullptr);
    const bool has_modern_bridge =
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Composition.DesktopWindowContentBridge") != nullptr ||
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Input.InputSite.WindowClass") != nullptr ||
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Core.CoreWindow") != nullptr;

    handles->legacy_layout = rebar != nullptr && !has_modern_bridge;
    handles->horizontal = RectWidth(handles->taskbar_screen) >= RectHeight(handles->taskbar_screen);

    handles->bar = rebar;
    if (handles->bar == nullptr) {
        handles->bar = FindWindowExW(handles->taskbar, nullptr, L"WorkerW", nullptr);
    }

    if (handles->legacy_layout) {
        handles->parent = handles->bar != nullptr ? handles->bar : handles->taskbar;
        handles->min = FindDescendantByClass(handles->parent, L"MSTaskSwWClass", L"MSTaskListWClass");
        if (handles->min == nullptr && handles->parent != handles->taskbar) {
            handles->min = FindDescendantByClass(handles->taskbar, L"MSTaskSwWClass", L"MSTaskListWClass");
        }

        return handles->parent != nullptr && handles->min != nullptr;
    }

    handles->parent = handles->taskbar;
    handles->min = FindDescendantByClass(handles->taskbar, L"MSTaskSwWClass", L"MSTaskListWClass");
    if (handles->min == nullptr && handles->bar != nullptr) {
        handles->min = FindDescendantByClass(handles->bar, L"MSTaskSwWClass", L"MSTaskListWClass");
    }

    handles->tray = FindDescendantByClass(handles->taskbar, L"TrayNotifyWnd");
    if (handles->tray == nullptr && handles->bar != nullptr) {
        handles->tray = FindDescendantByClass(handles->bar, L"TrayNotifyWnd");
    }

    handles->start = FindDescendantByClass(handles->taskbar, L"Start");
    if (handles->start == nullptr && handles->bar != nullptr) {
        handles->start = FindDescendantByClass(handles->bar, L"Start");
    }

    return handles->parent != nullptr &&
           (handles->tray != nullptr || handles->start != nullptr || handles->min != nullptr);
}

bool CaptureOriginalLayout(const TaskbarHandles& handles) {
    if (handles.min == nullptr || handles.parent == nullptr) {
        return false;
    }

    if (g_layout.captured && g_layout.bar == handles.parent && g_layout.min == handles.min) {
        return true;
    }

    RECT min_client = {};
    RECT min_screen = {};
    if (!GetRectInParent(handles.min, handles.parent, &min_client) || !ReadWindowRect(handles.min, &min_screen)) {
        return false;
    }

    g_layout.bar = handles.parent;
    g_layout.min = handles.min;
    g_layout.original_min_client = min_client;
    g_layout.original_min_screen = min_screen;
    g_layout.captured = true;
    g_layout.active = false;
    return true;
}

bool RestoreTaskbarLayoutInternal() {
    bool restored_anything = false;

    if (g_layout.captured && g_layout.min != nullptr && IsWindow(g_layout.min)) {
        const RECT& rect = g_layout.original_min_client;
        const BOOL moved = MoveWindow(
            g_layout.min,
            rect.left,
            rect.top,
            std::max(1, RectWidth(rect)),
            std::max(1, RectHeight(rect)),
            TRUE
        );

        restored_anything = moved != FALSE;
    }

    if (RestoreWidgetToTopLevel()) {
        restored_anything = true;
    }

    g_layout.active = false;
    return restored_anything;
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

bool EnsureWidgetEmbedded(HWND hwnd, const TaskbarHandles& handles) {
    if (hwnd == nullptr || handles.parent == nullptr) {
        return false;
    }

    if (!CaptureWidgetState(hwnd)) {
        return false;
    }

    if (!EnsureWidgetSubclassed(hwnd)) {
        return false;
    }

    ApplyBaseWidgetStyles(hwnd);

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~WS_POPUP;
    style |= WS_CHILD;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    if (GetParent(hwnd) != handles.parent) {
        SetLastError(0);
        SetParent(hwnd, handles.parent);
        if (GetLastError() != 0) {
            return false;
        }
    }

    SetWindowPos(
        hwnd,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW
    );

    g_layout.widget = hwnd;
    g_layout.widget_embedded = true;
    return true;
}

bool ApplyWin11Placement(
    const TaskbarHandles& handles,
    HWND hwnd,
    int preferred_width,
    int preferred_height,
    TaskbarPlacement* placement
) {
    if (!EnsureWidgetEmbedded(hwnd, handles)) {
        return false;
    }

    RECT taskbar_client{0, 0, RectWidth(handles.taskbar_screen), RectHeight(handles.taskbar_screen)};
    RECT tray_client = taskbar_client;
    RECT start_client = taskbar_client;
    RECT min_client = taskbar_client;

    const bool has_tray = handles.tray != nullptr && GetRectInParent(handles.tray, handles.parent, &tray_client);
    const bool has_start = handles.start != nullptr && GetRectInParent(handles.start, handles.parent, &start_client);
    const bool has_min = handles.min != nullptr && GetRectInParent(handles.min, handles.parent, &min_client);

    const int taskbar_width = RectWidth(taskbar_client);
    const int taskbar_height = RectHeight(taskbar_client);
    const int edge = DetectTaskbarEdge(handles.taskbar_screen);

    int child_x = 0;
    int child_y = 0;
    int widget_width = preferred_width;
    int widget_height = preferred_height;

    if (handles.horizontal) {
        int leading = has_min ? static_cast<int>(min_client.right) : 0;
        if (has_start) {
            leading = std::max(leading, static_cast<int>(start_client.right));
        }

        int trailing = has_tray ? static_cast<int>(tray_client.left) : taskbar_width - kMissingTrayFallbackWidth;
        const int available_width = std::max(1, trailing - leading - kWidgetMargin - kTrayGap);
        widget_width = ClampDimension(preferred_width, 1, available_width);
        widget_height = ClampDimension(preferred_height, kMinWidgetHeight, taskbar_height);

        child_x = std::clamp(
            trailing - widget_width - kTrayGap,
            leading + kWidgetMargin,
            taskbar_width - widget_width
        );
        child_y = std::max(0, (taskbar_height - widget_height) / 2);
    } else {
        const int leading = has_min ? static_cast<int>(min_client.bottom) : 0;
        const int trailing = has_tray ? static_cast<int>(tray_client.top) : taskbar_height;
        const int available_height = std::max(1, trailing - leading - kWidgetMargin - kTrayGap);
        widget_width = ClampDimension(preferred_width, 1, taskbar_width);
        widget_height = ClampDimension(preferred_height, kMinWidgetHeight, available_height);

        child_x = std::max(0, (taskbar_width - widget_width) / 2);
        child_y = std::clamp(
            trailing - widget_height - kTrayGap,
            leading + kWidgetMargin,
            taskbar_height - widget_height
        );
    }

    if (!SetWindowPos(
            hwnd,
            HWND_TOP,
            child_x,
            child_y,
            widget_width,
            widget_height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        return false;
    }

    placement->x = handles.taskbar_screen.left + child_x;
    placement->y = handles.taskbar_screen.top + child_y;
    placement->width = widget_width;
    placement->height = widget_height;
    placement->edge = edge;
    g_layout.active = false;
    return true;
}

bool ApplyLegacyPlacement(
    const TaskbarHandles& handles,
    HWND hwnd,
    int preferred_width,
    int preferred_height,
    TaskbarPlacement* placement
) {
    if (!CaptureOriginalLayout(handles) || !EnsureWidgetEmbedded(hwnd, handles)) {
        return false;
    }

    RECT parent_screen = {};
    if (!ReadWindowRect(handles.parent, &parent_screen)) {
        return false;
    }

    const RECT min_client = g_layout.original_min_client;
    const RECT min_screen = g_layout.original_min_screen;
    const int parent_width = RectWidth(parent_screen);
    const int parent_height = RectHeight(parent_screen);
    const int edge = DetectTaskbarEdge(handles.taskbar_screen);

    int child_x = 0;
    int child_y = 0;
    int widget_width = preferred_width;
    int widget_height = preferred_height;

    if (handles.horizontal) {
        widget_width = ClampDimension(preferred_width, 1, RectWidth(min_screen));
        widget_height = ClampDimension(preferred_height, kMinWidgetHeight, parent_height);

        const int new_width = std::max(1, RectWidth(min_client) - widget_width);
        if (!MoveWindow(
                handles.min,
                min_client.left,
                min_client.top,
                new_width,
                std::max(1, RectHeight(min_client)),
                TRUE)) {
            return false;
        }

        child_x = min_client.left + new_width + kWidgetMargin;
        child_y = std::max(0, (parent_height - widget_height) / 2);
    } else {
        widget_width = ClampDimension(preferred_width, 1, parent_width);
        widget_height = ClampDimension(preferred_height, 1, RectHeight(min_screen));

        const int new_height = std::max(1, RectHeight(min_client) - widget_height);
        if (!MoveWindow(
                handles.min,
                min_client.left,
                min_client.top,
                std::max(1, RectWidth(min_client)),
                new_height,
                TRUE)) {
            return false;
        }

        child_x = std::max(0, (parent_width - widget_width) / 2);
        child_y = min_client.top + new_height + kWidgetMargin;
    }

    if (!SetWindowPos(
            hwnd,
            HWND_TOP,
            child_x,
            child_y,
            widget_width,
            widget_height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        return false;
    }

    placement->x = parent_screen.left + child_x;
    placement->y = parent_screen.top + child_y;
    placement->width = widget_width;
    placement->height = widget_height;
    placement->edge = edge;
    g_layout.active = true;
    return true;
}

}  // namespace

extern "C" {

int tm_apply_widget_styles(void* widget_hwnd) {
    HWND hwnd = static_cast<HWND>(widget_hwnd);
    if (hwnd == nullptr) {
        return 0;
    }

    ApplyBaseWidgetStyles(hwnd);
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

    if (g_layout.active && (g_layout.bar != handles.parent || g_layout.min != handles.min)) {
        RestoreTaskbarLayoutInternal();
        g_layout.captured = false;
    }

    if (handles.legacy_layout) {
        return ApplyLegacyPlacement(handles, hwnd, preferred_width, preferred_height, placement) ? 1 : 0;
    }

    if (g_layout.active) {
        RestoreTaskbarLayoutInternal();
    }

    return ApplyWin11Placement(handles, hwnd, preferred_width, preferred_height, placement) ? 1 : 0;
}

int tm_restore_taskbar_layout() {
    return RestoreTaskbarLayoutInternal() ? 1 : 0;
}

int tm_should_widget_be_visible(void* widget_hwnd, void* main_hwnd, int* visible) {
    if (visible == nullptr) {
        return 0;
    }

    if (g_layout.widget_embedded) {
        *visible = 1;
        return 1;
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
