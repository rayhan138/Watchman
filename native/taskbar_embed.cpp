#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "taskbar_embed.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace {

constexpr int kWidgetMargin = 2;
constexpr int kTrayGap = 2;

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
    RECT original_parent_client{};
    RECT reserved_min_client{};
    RECT taskbar_screen{};
    bool horizontal = true;
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

struct Win11PlacementState {
    HWND parent = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool valid = false;
};

SavedTaskbarLayout g_layout;
Win11PlacementState g_win11_placement;
TaskbarHandles g_cached_taskbar_handles;
LONG g_pending_widget_context_menu = 0;
LONG g_pending_widget_double_click = 0;
LONG g_mouse_hook_started = 0;
HHOOK g_mouse_hook = nullptr;
POINT g_last_left_click_point{0, 0};
DWORD g_last_left_click_time = 0;
bool g_use_layered_widget_style = true;
bool g_use_manual_layered_paint = false;
std::atomic<HWND> g_widget_menu_owner{nullptr};
std::atomic<bool> g_widget_menu_open{false};
std::atomic<HWND> g_widget_command_target{nullptr};
std::atomic<UINT> g_widget_context_command{0};
std::atomic<UINT> g_widget_history_command{0};

bool IsPointInsideNativePopupMenu(POINT point);
void CancelWidgetMenuForOutsideClick(POINT point);

bool PostWidgetCommand(UINT message) {
    const HWND target = g_widget_command_target.load(std::memory_order_acquire);
    return target != nullptr &&
           message != 0 &&
           IsWindow(target) != FALSE &&
           PostMessageW(target, message, 0, 0) != FALSE;
}

void RequestWidgetContextMenu() {
    const UINT message = g_widget_context_command.load(std::memory_order_acquire);
    if (!PostWidgetCommand(message)) {
        InterlockedExchange(&g_pending_widget_context_menu, 1);
    }
}

void RequestWidgetHistory() {
    const UINT message = g_widget_history_command.load(std::memory_order_acquire);
    if (!PostWidgetCommand(message)) {
        InterlockedExchange(&g_pending_widget_double_click, 1);
    }
}

bool IsPointInsideWidget(POINT point) {
    if (!g_layout.widget_embedded || g_layout.widget == nullptr || !IsWindow(g_layout.widget)) {
        return false;
    }

    RECT rect{};
    if (!GetWindowRect(g_layout.widget, &rect)) {
        return false;
    }

    return point.x >= rect.left &&
           point.x < rect.right &&
           point.y >= rect.top &&
           point.y < rect.bottom;
}

void QueueLeftClickOrDoubleClick(POINT point) {
    const DWORD now = GetTickCount();
    const DWORD double_click_time = GetDoubleClickTime();
    const int max_dx = GetSystemMetrics(SM_CXDOUBLECLK);
    const int max_dy = GetSystemMetrics(SM_CYDOUBLECLK);

    const bool is_double_click =
        g_last_left_click_time != 0 &&
        now - g_last_left_click_time <= double_click_time &&
        std::abs(point.x - g_last_left_click_point.x) <= max_dx &&
        std::abs(point.y - g_last_left_click_point.y) <= max_dy;

    if (is_double_click) {
        g_last_left_click_time = 0;
        RequestWidgetHistory();
        return;
    }

    g_last_left_click_point = point;
    g_last_left_click_time = now;
}

LRESULT CALLBACK WidgetMouseHookProc(int code, WPARAM w_param, LPARAM l_param) {
    if (code == HC_ACTION && l_param != 0) {
        const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(l_param);

        if (mouse != nullptr) {
            switch (w_param) {
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_XBUTTONDOWN:
                case WM_NCLBUTTONDOWN:
                case WM_NCRBUTTONDOWN:
                case WM_NCMBUTTONDOWN:
                case WM_NCXBUTTONDOWN:
                    CancelWidgetMenuForOutsideClick(mouse->pt);
                    break;
                default:
                    break;
            }
        }

        if (mouse != nullptr && IsPointInsideWidget(mouse->pt)) {
            switch (w_param) {
                case WM_RBUTTONDOWN:
                case WM_NCRBUTTONDOWN:
                    return 1;
                case WM_RBUTTONUP:
                case WM_RBUTTONDBLCLK:
                case WM_NCRBUTTONUP:
                case WM_NCRBUTTONDBLCLK:
                    RequestWidgetContextMenu();
                    return 1;
                case WM_LBUTTONDOWN:
                case WM_NCLBUTTONDOWN:
                    return 1;
                case WM_LBUTTONUP:
                case WM_NCLBUTTONUP:
                    QueueLeftClickOrDoubleClick(mouse->pt);
                    return 1;
                default:
                    break;
            }
        }
    }

    return CallNextHookEx(g_mouse_hook, code, w_param, l_param);
}

DWORD WINAPI WidgetMouseHookThread(LPVOID) {
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, WidgetMouseHookProc, GetModuleHandleW(nullptr), 0);
    if (g_mouse_hook == nullptr) {
        InterlockedExchange(&g_mouse_hook_started, 0);
        return 0;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
    InterlockedExchange(&g_mouse_hook_started, 0);
    return 0;
}

void EnsureWidgetMouseHook() {
    if (InterlockedCompareExchange(&g_mouse_hook_started, 1, 0) != 0) {
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, WidgetMouseHookThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        InterlockedExchange(&g_mouse_hook_started, 0);
        return;
    }

    CloseHandle(thread);
}

LRESULT CALLBACK WidgetSubclassProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_CONTEXTMENU:
            RequestWidgetContextMenu();
            return 0;
        case WM_RBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
            return 0;
        case WM_LBUTTONDBLCLK:
        case WM_NCLBUTTONDBLCLK:
            RequestWidgetHistory();
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

bool HasUsableTaskbarHandle(HWND hwnd) {
    return hwnd != nullptr && IsWindow(hwnd) != FALSE;
}

bool HasUsableCachedTaskbarHandles(HWND taskbar) {
    const TaskbarHandles& cached = g_cached_taskbar_handles;
    if (cached.taskbar != taskbar ||
        !HasUsableTaskbarHandle(cached.taskbar) ||
        !HasUsableTaskbarHandle(cached.parent)) {
        return false;
    }

    if (cached.legacy_layout) {
        return HasUsableTaskbarHandle(cached.bar) && HasUsableTaskbarHandle(cached.min) &&
               GetParent(cached.min) == cached.parent &&
               (cached.parent == taskbar || IsChild(taskbar, cached.parent));
    }

    return HasUsableTaskbarHandle(cached.tray) && IsChild(taskbar, cached.tray) &&
           HasUsableTaskbarHandle(cached.min) && IsChild(taskbar, cached.min) &&
           (cached.start == nullptr || (HasUsableTaskbarHandle(cached.start) && IsChild(taskbar, cached.start)));
}

int DetectTaskbarEdge(const RECT& rect) {
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor_info)) {
        return RectWidth(rect) >= RectHeight(rect) ? 0 : 2;
    }
    const RECT& monitor = monitor_info.rcMonitor;
    if (RectWidth(rect) >= RectHeight(rect)) {
        return std::abs(rect.top - monitor.top) <= std::abs(monitor.bottom - rect.bottom) ? 1 : 0;
    }
    return std::abs(rect.left - monitor.left) <= std::abs(monitor.right - rect.right) ? 2 : 3;
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

    SetLastError(0);
    return MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(rect), 2) != 0 || GetLastError() == 0;
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
        return true;
    }

    if (g_layout.widget_wndproc_before != nullptr) {
        SetLastError(0);
        if (SetWindowLongPtrW(
                g_layout.widget,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_layout.widget_wndproc_before)
            ) == 0 && GetLastError() != 0) {
            return false;
        }
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
    LONG_PTR target_ex_style =
        (ex_style | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
    if (g_use_layered_widget_style) {
        target_ex_style |= WS_EX_LAYERED;
    } else {
        target_ex_style &= ~WS_EX_LAYERED;
    }

    if (ex_style != target_ex_style) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, target_ex_style);
    }

    if (g_use_layered_widget_style && !g_use_manual_layered_paint) {
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    }
}

bool IsPointInsideNativePopupMenu(POINT point) {
    HWND window = WindowFromPoint(point);
    while (window != nullptr) {
        wchar_t class_name[32]{};
        GetClassNameW(window, class_name, static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
        if (lstrcmpW(class_name, L"#32768") == 0) {
            return true;
        }
        window = GetParent(window);
    }
    return false;
}

void CancelWidgetMenuForOutsideClick(POINT point) {
    if (!g_widget_menu_open.load() || IsPointInsideNativePopupMenu(point)) {
        return;
    }

    if (!g_widget_menu_open.exchange(false)) {
        return;
    }

    HWND owner = g_widget_menu_owner.exchange(nullptr);
    if (owner != nullptr && IsWindow(owner)) {
        PostMessageW(owner, WM_CANCELMODE, 0, 0);
    }
}

bool RestoreWidgetToTopLevel() {
    if (!g_layout.widget_state_captured || g_layout.widget == nullptr || !IsWindow(g_layout.widget)) {
        g_layout.widget_embedded = false;
        g_layout.widget_subclassed = false;
        return true;
    }

    const HWND parent = g_layout.widget_parent_before;
    if (!g_layout.widget_embedded && !g_layout.widget_subclassed &&
        GetParent(g_layout.widget) == parent &&
        GetWindowLongPtrW(g_layout.widget, GWL_STYLE) == g_layout.widget_style_before) {
        return true;
    }
    if (GetParent(g_layout.widget) != parent) {
        SetLastError(0);
        SetParent(g_layout.widget, parent);
        if (GetLastError() != 0) {
            return false;
        }
    }

    if (!RestoreWidgetSubclass()) {
        return false;
    }
    SetLastError(0);
    if (SetWindowLongPtrW(g_layout.widget, GWL_STYLE, g_layout.widget_style_before) == 0 &&
        GetLastError() != 0) {
        return false;
    }
    ApplyBaseWidgetStyles(g_layout.widget);
    if (!SetWindowPos(
            g_layout.widget,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED)) {
        return false;
    }

    g_layout.widget_embedded = false;
    g_win11_placement = {};
    return true;
}

bool InitTaskbarWindows(TaskbarHandles* handles) {
    if (handles == nullptr) {
        return false;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT taskbar_screen{};
    if (taskbar == nullptr || !ReadWindowRect(taskbar, &taskbar_screen)) {
        g_cached_taskbar_handles = {};
        return false;
    }

    const bool horizontal = RectWidth(taskbar_screen) >= RectHeight(taskbar_screen);
    if (HasUsableCachedTaskbarHandles(taskbar) &&
        g_cached_taskbar_handles.horizontal == horizontal &&
        RectWidth(g_cached_taskbar_handles.taskbar_screen) == RectWidth(taskbar_screen) &&
        RectHeight(g_cached_taskbar_handles.taskbar_screen) == RectHeight(taskbar_screen)) {
        *handles = g_cached_taskbar_handles;
        handles->taskbar_screen = taskbar_screen;
        handles->horizontal = horizontal;
        g_cached_taskbar_handles = *handles;
        return true;
    }
    // Rotation can change the embedding hierarchy even while its HWNDs survive.

    g_cached_taskbar_handles = {};
    handles->taskbar = taskbar;
    handles->taskbar_screen = taskbar_screen;

    HWND rebar = FindWindowExW(handles->taskbar, nullptr, L"ReBarWindow32", nullptr);
    const bool has_modern_bridge =
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Composition.DesktopWindowContentBridge") != nullptr ||
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Input.InputSite.WindowClass") != nullptr ||
        FindDescendantByClass(handles->taskbar, L"Windows.UI.Core.CoreWindow") != nullptr;

    handles->legacy_layout = rebar != nullptr && !has_modern_bridge;
    handles->horizontal = horizontal;

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

        if (handles->min != nullptr) {
            handles->parent = GetParent(handles->min);
        }
        const bool initialized = handles->parent != nullptr && handles->min != nullptr;
        if (initialized) {
            g_cached_taskbar_handles = *handles;
        }
        return initialized;
    }

    handles->parent = handles->taskbar;
    handles->min = FindDescendantByClass(handles->taskbar, L"MSTaskSwWClass", L"MSTaskListWClass");
    if (handles->min == nullptr && handles->bar != nullptr) {
        handles->min = FindDescendantByClass(handles->bar, L"MSTaskSwWClass", L"MSTaskListWClass");
    }

    // Windows 11's normal hierarchy matches TrafficMonitor's fast path.
    handles->tray = FindWindowExW(handles->taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (handles->tray == nullptr) {
        handles->tray = FindDescendantByClass(handles->taskbar, L"TrayNotifyWnd");
    }
    if (handles->tray == nullptr && handles->bar != nullptr) {
        handles->tray = FindDescendantByClass(handles->bar, L"TrayNotifyWnd");
    }

    handles->start = FindWindowExW(handles->taskbar, nullptr, L"Start", nullptr);
    if (handles->start == nullptr) {
        handles->start = FindDescendantByClass(handles->taskbar, L"Start");
    }
    if (handles->start == nullptr && handles->bar != nullptr) {
        handles->start = FindDescendantByClass(handles->bar, L"Start");
    }

    const bool initialized = handles->parent != nullptr &&
                             (handles->tray != nullptr || handles->start != nullptr || handles->min != nullptr);
    if (initialized) {
        g_cached_taskbar_handles = *handles;
    }
    return initialized;
}

bool PositionWindowIfChanged(HWND hwnd, HWND parent, const RECT& target) {
    RECT current{};
    if (GetRectInParent(hwnd, parent, &current) && EqualRect(&current, &target)) {
        return true;
    }
    return SetWindowPos(hwnd, HWND_TOP, target.left, target.top,
                        RectWidth(target), RectHeight(target), SWP_NOACTIVATE) != FALSE;
}

constexpr bool RestoreReservationGeometry(const RECT& original, const RECT& reserved,
                                          const RECT& original_parent, const RECT& parent,
                                          const RECT& current, RECT* restored) {
    const auto same_size = [](const RECT& a, const RECT& b) {
        return a.right - a.left == b.right - b.left &&
               a.bottom - a.top == b.bottom - b.top;
    };
    RECT resized = reserved;
    resized.right += (parent.right - parent.left) - (original_parent.right - original_parent.left);
    resized.bottom += (parent.bottom - parent.top) - (original_parent.bottom - original_parent.top);
    if (!same_size(current, reserved) && !same_size(current, resized)) {
        return false;
    }
    // Preserve Explorer's translation/resize and undo only our saved insets.
    // An unknown size is ambiguous: callers must not capture it as a baseline.
    *restored = {current.left + original.left - reserved.left,
                 current.top + original.top - reserved.top,
                 current.right + original.right - reserved.right,
                 current.bottom + original.bottom - reserved.bottom};
    restored->left = std::clamp(restored->left, parent.left, parent.right);
    restored->top = std::clamp(restored->top, parent.top, parent.bottom);
    restored->right = std::clamp(restored->right, restored->left, parent.right);
    restored->bottom = std::clamp(restored->bottom, restored->top, parent.bottom);
    return restored->right > restored->left && restored->bottom > restored->top;
}

static_assert([]() constexpr {
    RECT restored{};
    const RECT original{0, 0, 900, 48}, reserved{0, 0, 760, 48};
    const RECT old_parent{0, 0, 1000, 48}, parent{0, 0, 1200, 48};
    if (!RestoreReservationGeometry(original, reserved, old_parent, parent,
                                    RECT{10, 0, 970, 48}, &restored) ||
        restored.left != 10 || restored.right != 1110) return false;
    if (!RestoreReservationGeometry(original, reserved, old_parent, old_parent,
                                    RECT{10, 0, 770, 48}, &restored) ||
        restored.left != 10 || restored.right != 910) return false;
    if (!RestoreReservationGeometry(RECT{0, 0, 48, 900}, RECT{0, 0, 48, 760},
                                    RECT{0, 0, 48, 1000}, RECT{0, 0, 48, 1200},
                                    RECT{0, 10, 48, 970}, &restored) ||
        restored.top != 10 || restored.bottom != 1110) return false;
    return !RestoreReservationGeometry(original, reserved, old_parent, parent,
                                       RECT{10, 0, 960, 48}, &restored);
}(), "Retained reservations must survive translation/resize without double shrinking");

bool RestoreReservedLayout() {
    if (!g_layout.active || !IsWindow(g_layout.min) ||
        GetParent(g_layout.min) != g_layout.bar) {
        return true;
    }
    RECT current{}, parent{};
    if (!GetRectInParent(g_layout.min, g_layout.bar, &current) ||
        !GetClientRect(g_layout.bar, &parent)) {
        return false;
    }
    RECT restored{};
    const bool retained = RestoreReservationGeometry(
        g_layout.original_min_client, g_layout.reserved_min_client,
        g_layout.original_parent_client, parent, current, &restored);
    RECT resized_original = g_layout.original_min_client;
    resized_original.right += RectWidth(parent) - RectWidth(g_layout.original_parent_client);
    resized_original.bottom += RectHeight(parent) - RectHeight(g_layout.original_parent_client);
    const bool already_restored = EqualRect(&current, &g_layout.original_min_client) ||
                                  EqualRect(&current, &resized_original);
    // If both interpretations fit (e.g. parent delta equals reservation size),
    // keep cleanup pending instead of guessing and expanding/shrinking again.
    if (retained && already_restored) {
        return false;
    }
    if (retained) {
        return PositionWindowIfChanged(g_layout.min, g_layout.bar, restored);
    }
    // Unknown geometry must not become a newly shortened baseline.
    return already_restored;
}

bool CaptureOriginalLayout(const TaskbarHandles& handles) {
    if (handles.min == nullptr || handles.parent == nullptr) {
        return false;
    }
    RECT parent{}, current{};
    if (!GetClientRect(handles.parent, &parent) ||
        !GetRectInParent(handles.min, handles.parent, &current)) {
        return false;
    }
    if (g_layout.captured && g_layout.bar == handles.parent && g_layout.min == handles.min &&
        g_layout.horizontal == handles.horizontal &&
        RectWidth(g_layout.taskbar_screen) == RectWidth(handles.taskbar_screen) &&
        RectHeight(g_layout.taskbar_screen) == RectHeight(handles.taskbar_screen) &&
        EqualRect(&g_layout.original_parent_client, &parent) &&
        EqualRect(&current, g_layout.active ? &g_layout.reserved_min_client : &g_layout.original_min_client)) {
        return true;
    }
    if (!RestoreReservedLayout()) {
        return false;
    }
    g_layout.active = false;
    g_layout.captured = false;
    if (!GetRectInParent(handles.min, handles.parent, &current)) {
        return false;
    }
    g_layout.bar = handles.parent;
    g_layout.min = handles.min;
    g_layout.original_min_client = current;
    g_layout.original_parent_client = parent;
    g_layout.taskbar_screen = handles.taskbar_screen;
    g_layout.horizontal = handles.horizontal;
    g_layout.captured = true;
    return true;
}

bool RestoreTaskbarLayoutInternal() {
    const bool restored_layout = RestoreReservedLayout();
    const bool restored_widget = RestoreWidgetToTopLevel();
    if (restored_layout) {
        g_layout.active = false;
        g_layout.captured = false;
    }
    return restored_layout && restored_widget;
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

    EnsureWidgetMouseHook();
    ApplyBaseWidgetStyles(hwnd);

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR child_style = (style & ~WS_POPUP) | WS_CHILD;
    const bool style_changed = style != child_style;
    const bool parent_changed = GetParent(hwnd) != handles.parent;
    if ((style_changed || parent_changed) && IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    }
    if (style_changed) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, child_style & ~WS_VISIBLE);
    }

    if (parent_changed) {
        SetLastError(0);
        SetParent(hwnd, handles.parent);
        if (GetLastError() != 0) {
            return false;
        }
    }

    if (style_changed || parent_changed) {
        SetWindowPos(
            hwnd,
            HWND_TOP,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED
        );
        g_win11_placement = {};
    }

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
    RECT taskbar_client{};
    if (!GetClientRect(handles.parent, &taskbar_client)) {
        return false;
    }
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
    const int widget_width = preferred_width;
    const int widget_height = preferred_height;
    // Without both the app-list and tray bounds we cannot certify a free gap.
    // Compatibility limit: some shells expose a full-width task-list container,
    // not occupied app bounds. Those shells may keep the widget hidden even
    // when visually empty space exists; never infer an overlapping fallback.
    // A missing Win11 task-list HWND is likewise not evidence of space.
    if (!has_tray || !has_min || (handles.start != nullptr && !has_start) ||
        RectWidth(tray_client) <= 0 || RectHeight(tray_client) <= 0 ||
        RectWidth(min_client) <= 0 || RectHeight(min_client) <= 0 ||
        widget_width <= 0 || widget_height <= 0 ||
        widget_width > taskbar_width || widget_height > taskbar_height) {
        return false;
    }

    if (handles.horizontal) {
        int leading = std::max(0, static_cast<int>(min_client.right));
        if (has_start) {
            leading = std::max(leading, static_cast<int>(start_client.right));
        }
        const int trailing = std::min(taskbar_width, static_cast<int>(tray_client.left));
        if (trailing - leading < widget_width + kWidgetMargin + kTrayGap) {
            return false;
        }
        child_x = trailing - widget_width - kTrayGap;
        child_y = (taskbar_height - widget_height) / 2;
    } else {
        int leading = std::max(0, static_cast<int>(min_client.bottom));
        if (has_start) {
            leading = std::max(leading, static_cast<int>(start_client.bottom));
        }
        const int trailing = std::min(taskbar_height, static_cast<int>(tray_client.top));
        if (trailing - leading < widget_height + kWidgetMargin + kTrayGap) {
            return false;
        }
        child_x = (taskbar_width - widget_width) / 2;
        child_y = trailing - widget_height - kTrayGap;
    }

    if (!EnsureWidgetEmbedded(hwnd, handles)) {
        return false;
    }
    RECT current{};
    const RECT target{child_x, child_y, child_x + widget_width, child_y + widget_height};
    const bool actual_matches = GetRectInParent(hwnd, handles.parent, &current) && EqualRect(&current, &target);

    const bool placement_changed =
        !actual_matches ||
        !g_win11_placement.valid ||
        g_win11_placement.parent != handles.parent ||
        g_win11_placement.x != child_x ||
        g_win11_placement.y != child_y ||
        g_win11_placement.width != widget_width ||
        g_win11_placement.height != widget_height;

    if (placement_changed) {
        if (!SetWindowPos(
                hwnd,
                HWND_TOP,
                child_x,
                child_y,
                widget_width,
                widget_height,
                SWP_NOACTIVATE)) {
            return false;
        }

        g_win11_placement = {
            handles.parent,
            child_x,
            child_y,
            widget_width,
            widget_height,
            true,
        };
    }

    POINT origin{child_x, child_y};
    ClientToScreen(handles.parent, &origin);
    placement->x = origin.x;
    placement->y = origin.y;
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
    if (!CaptureOriginalLayout(handles)) {
        return false;
    }

    RECT bounds{};
    if (!IntersectRect(&bounds, &g_layout.original_min_client, &g_layout.original_parent_client) ||
        preferred_width <= 0 || preferred_height <= 0 ||
        preferred_width > RectWidth(bounds) || preferred_height > RectHeight(bounds)) {
        return false;
    }
    RECT reserved = g_layout.original_min_client;
    RECT widget{};
    if (handles.horizontal) {
        const int remaining = RectWidth(bounds) - preferred_width - kWidgetMargin - kTrayGap;
        if (remaining < 1) {
            return false;
        }
        reserved.right = bounds.left + remaining;
        widget.left = reserved.right + kWidgetMargin;
        widget.top = bounds.top + (RectHeight(bounds) - preferred_height) / 2;
    } else {
        const int remaining = RectHeight(bounds) - preferred_height - kWidgetMargin - kTrayGap;
        if (remaining < 1) {
            return false;
        }
        reserved.bottom = bounds.top + remaining;
        widget.left = bounds.left + (RectWidth(bounds) - preferred_width) / 2;
        widget.top = reserved.bottom + kWidgetMargin;
    }
    widget.right = widget.left + preferred_width;
    widget.bottom = widget.top + preferred_height;

    if (!EnsureWidgetEmbedded(hwnd, handles) ||
        !PositionWindowIfChanged(handles.min, handles.parent, reserved)) {
        return false;
    }
    // Record the reservation before moving the widget so failure can roll back.
    g_layout.reserved_min_client = reserved;
    g_layout.active = true;
    if (!PositionWindowIfChanged(hwnd, handles.parent, widget)) {
        return false;
    }

    POINT origin{widget.left, widget.top};
    ClientToScreen(handles.parent, &origin);
    placement->x = origin.x;
    placement->y = origin.y;
    placement->width = preferred_width;
    placement->height = preferred_height;
    placement->edge = DetectTaskbarEdge(handles.taskbar_screen);
    return true;
}

}  // namespace

extern "C" {

int tm_get_taskbar_metrics(TaskbarMetrics* metrics) {
    if (metrics == nullptr) {
        return 0;
    }
    *metrics = TaskbarMetrics{};
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT rect{};
    if (!ReadWindowRect(taskbar, &rect) || RectWidth(rect) <= 0 || RectHeight(rect) <= 0) {
        return 0;
    }
    metrics->edge = DetectTaskbarEdge(rect);
    metrics->thickness = metrics->edge < 2 ? RectHeight(rect) : RectWidth(rect);
    const UINT dpi = GetDpiForWindow(taskbar);
    metrics->dpi = dpi != 0 ? dpi : 96;
    return 1;
}

int tm_set_widget_layered_style(int enabled) {
    g_use_layered_widget_style = enabled != 0;
    return 1;
}

int tm_set_widget_manual_layered_paint(int enabled) {
    g_use_manual_layered_paint = enabled != 0;
    return 1;
}

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
    if (hwnd == nullptr || placement == nullptr || preferred_width <= 0 || preferred_height <= 0) {
        return 0;
    }

    TaskbarHandles handles{};
    if (!InitTaskbarWindows(&handles)) {
        return 0;
    }

    if (g_layout.active && (g_layout.bar != handles.parent || g_layout.min != handles.min)) {
        if (!RestoreReservedLayout()) {
            return 0;
        }
        g_layout.active = false;
        g_layout.captured = false;
    }

    if (handles.legacy_layout) {
        return ApplyLegacyPlacement(handles, hwnd, preferred_width, preferred_height, placement) ? 1 : 0;
    }

    if (g_layout.active) {
        if (!RestoreReservedLayout()) {
            return 0;
        }
        g_layout.active = false;
        g_layout.captured = false;
    }

    return ApplyWin11Placement(handles, hwnd, preferred_width, preferred_height, placement) ? 1 : 0;
}

int tm_restore_taskbar_layout() {
    return RestoreTaskbarLayoutInternal() ? 1 : 0;
}

int tm_set_widget_context_menu_open(void* owner_hwnd, int open) {
    if (open == 0) {
        g_widget_menu_open.store(false);
        g_widget_menu_owner.store(nullptr);
        return 1;
    }

    HWND owner = static_cast<HWND>(owner_hwnd);
    if (owner == nullptr || !IsWindow(owner)) {
        return 0;
    }

    g_widget_menu_owner.store(owner);
    g_widget_menu_open.store(true);
    return 1;
}

int tm_set_widget_command_target(void* target_hwnd, unsigned int context_menu_message, unsigned int history_message) {
    const HWND target = static_cast<HWND>(target_hwnd);
    if (target != nullptr && IsWindow(target) == FALSE) {
        return 0;
    }

    g_widget_context_command.store(context_menu_message, std::memory_order_release);
    g_widget_history_command.store(history_message, std::memory_order_release);
    g_widget_command_target.store(target, std::memory_order_release);
    return 1;
}

int tm_take_widget_context_menu_request() {
    return InterlockedExchange(&g_pending_widget_context_menu, 0) != 0 ? 1 : 0;
}

int tm_take_widget_double_click_request() {
    return InterlockedExchange(&g_pending_widget_double_click, 0) != 0 ? 1 : 0;
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
