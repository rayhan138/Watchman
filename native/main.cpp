#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>
#include <vector>
#include "renderer.h"
#include "bundled_fonts.h"
#include "floating_monitor_layout.h"
#include "metrics.h"
#include "app_tracker.h"
#include "ping_tester.h"
#include "storage.h"
#include "ui_style.h"
#include "speed_tester.h"
#include "taskbar_embed.h"
#include "taskbar_widget_layout.h"
#include "resource.h"
#include "runtime_policy.h"
#include "window_lifecycle.h"


// Window message handler declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WidgetWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK FloatingMonitorWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void UpdateWidgetState(bool force_reembed = false);
void UpdateFloatingMonitorState(bool reset_position = false);
bool AddTrayIcon(HWND hwnd);
void RemoveTrayIcon(HWND hwnd);
void ShowTrayMenu(HWND hwnd);
void ShowFloatingMonitorContextMenu(HWND owner_hwnd, POINT position);
void ShowNetworkAdapterPicker(HWND owner_hwnd);
void ShowFloatingMonitorDisplayPicker(HWND owner_hwnd);
void ShowToolsExportPicker(HWND owner_hwnd, ToolsExportPicker request);

#include "engine.h"

// Global App instances
AppRenderer* g_renderer = nullptr;
WatchmanEngine* g_engine = nullptr;

HWND g_main_hwnd = nullptr;
HWND g_widget_hwnd = nullptr;
HWND g_floating_monitor_hwnd = nullptr;

// The dashboard and embedded widget are painted by the same UI thread but at
// different moments. Publish one engine sample per second so both surfaces
// display the same network, CPU, and memory values for that interval.
DashboardSnapshot g_display_dashboard_snapshot{};
bool g_has_display_dashboard_snapshot = false;

constexpr UINT WM_WATCHMAN_WIDGET_MENU = WM_APP + 10;
constexpr UINT WM_WATCHMAN_WIDGET_DATA = WM_APP + 11;
constexpr UINT WM_WATCHMAN_TRAY = WM_APP + 12;
constexpr UINT WM_WATCHMAN_ACTIVATE = WM_APP + 13;
constexpr UINT TRAY_ICON_ID = 1;
const UINT WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");

namespace {

HMONITOR GetSelectedFloatingMonitorDisplay();

bool g_main_animation_timer_running = false;
// Initial quiet sign-in / replacement display is not an interactive welcome.
// Later tray/widget/second-launch restores may offer an unseen tour.
bool g_welcome_presentation_ready = false;

void UpdateMainUiActivity(HWND hwnd, bool visible, bool minimized, bool foreground) {
    const auto activity = WatchmanRuntime::GetMainUiActivity(visible, minimized, foreground);
    if (!activity.keep_graphics && g_renderer) g_renderer->ReleaseMainWindowResources();
    if (activity.animate && !g_main_animation_timer_running) {
        g_main_animation_timer_running = SetTimer(hwnd, 1, 16, nullptr) != 0;
    } else if (!activity.animate && g_main_animation_timer_running) {
        KillTimer(hwnd, 1);
        g_main_animation_timer_running = false;
    }
}

void RefreshDisplayDashboardSnapshot() {
    if (!g_engine) {
        return;
    }

    g_display_dashboard_snapshot = g_engine->GetDashboardSnapshot();
    g_has_display_dashboard_snapshot = true;
}

DashboardSnapshot GetDisplayDashboardSnapshot() {
    if (!g_has_display_dashboard_snapshot) {
        RefreshDisplayDashboardSnapshot();
    }
    return g_display_dashboard_snapshot;
}

void EvaluateWarningNotifications(HWND hwnd) {
    static WatchmanWarnings::Policy policy;
    static uint64_t last_sample = 0;
    static uint64_t retry_after_ms = 0;
    const auto& sample = g_display_dashboard_snapshot;
    if (!g_renderer || sample.sample_sequence == 0 || sample.sample_sequence == last_sample) return;
    last_sample = sample.sample_sequence;
    const auto preferences = g_renderer->GetWarningPreferences();
    const uint64_t now_ms = GetTickCount64();
    auto candidate = policy;
    const auto events = candidate.Evaluate(preferences, {sample.today_traffic_date,
        sample.today_download_bytes, sample.today_upload_bytes,
        WatchmanWarnings::FreshMemorySample(sample.ram_sample_available, sample.ram_total_bytes,
            sample.ram_sampled_at_ms, now_ms), sample.ram_usage_percent, sample.temperatures.readings}, now_ms);
    if (!events) { policy = std::move(candidate); return; }
    if (now_ms < retry_after_ms) return;

    std::wstring text;
    if (events.traffic) {
        text = L"Monitored traffic for " + sample.today_traffic_date + L" reached " + std::to_wstring(preferences.traffic_threshold)
            + (preferences.traffic_gigabytes ? L" GiB." : L" MiB.");
    }
    if (events.memory) {
        if (!text.empty()) text += L"\n";
        text += L"Memory usage reached " + std::to_wstring(static_cast<int>(sample.ram_usage_percent)) + L"%.";
    }
    for (size_t i = 0; i < WatchmanSensors::count; ++i) {
        if (!events.temperatures[i]) continue;
        if (!text.empty()) text += L"\n";
        text += std::wstring(WatchmanSensors::Name(static_cast<WatchmanSensors::Kind>(i))) + L" temperature: " +
            WatchmanSensors::Format(sample.temperatures.readings[i], now_ms) + L".";
    }
    NOTIFYICONDATAW notification{};
    notification.cbSize = sizeof(notification);
    notification.hWnd = hwnd;
    notification.uID = TRAY_ICON_ID;
    notification.uFlags = NIF_INFO;
    notification.dwInfoFlags = NIIF_WARNING;
    wcscpy_s(notification.szInfoTitle, L"Watchman warning");
    wcsncpy_s(notification.szInfo, text.c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_MODIFY, &notification)) {
        policy = std::move(candidate);
        retry_after_ms = 0;
    } else {
        retry_after_ms = now_ms + 60000;
        OutputDebugStringW(L"Watchman: Windows did not accept the warning notification; retrying after one minute.\n");
    }
}

int GetFloatingMonitorPreferredWidth() {
    return FloatingMonitorLayout::Width(g_renderer ? g_renderer->GetFloatingMonitorFontSize() : FloatingMonitorLayout::default_font_size);
}

int GetFloatingMonitorPreferredHeight() {
    return FloatingMonitorLayout::Height(
        g_renderer ? g_renderer->GetFloatingMonitorFontSize() : FloatingMonitorLayout::default_font_size);
}

POINT BoundFloatingMonitorPosition(POINT point, int width, int height) {
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor_info)) {
        const RECT& work = monitor_info.rcWork;
        point.x = std::clamp(point.x, work.left, (std::max)(work.left, work.right - width));
        point.y = std::clamp(point.y, work.top, (std::max)(work.top, work.bottom - height));
    }
    return point;
}

POINT GetFloatingMonitorDefaultPosition(int width, int height) {
    POINT point{};
    HMONITOR monitor = GetSelectedFloatingMonitorDisplay();
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(monitor, &monitor_info)) {
        point.x = monitor_info.rcWork.right - width - 18;
        point.y = monitor_info.rcWork.top + 72;
    } else {
        point.x = 24;
        point.y = 72;
    }
    return BoundFloatingMonitorPosition(point, width, height);
}

constexpr UINT kNetworkAdapterMenuFirstCommand = 1200;
constexpr UINT kToolsExportMenuFirstCommand = 1400;
constexpr UINT kFloatingMonitorDisplayMenuFirstCommand = 1600;
// Keep the native resize, move, Alt+Space, and snap behaviors, but remove the
// visible Windows caption. Watchman's client-drawn header replaces it.
constexpr DWORD kMainWindowStyle =
    WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

void ApplyMainWindowCorners(HWND hwnd, int width, int height) {
    if (!hwnd || width <= 0 || height <= 0) {
        return;
    }

    // Tauri's undecorated window has small physical corner rounding. The
    // region makes the same result reliable for this Win32 popup on Win10 and
    // Win11, where WS_POPUP otherwise renders as a square surface.
    constexpr int kCornerDiameter = 16;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                      kCornerDiameter, kCornerDiameter);
    if (region) {
        SetWindowRgn(hwnd, region, TRUE);
    }
}

void ApplyMainWindowTheme(HWND hwnd) {
    if (!g_renderer) return;
    const BOOL dark_mode = g_renderer->GetEffectiveTheme() == ThemeMode::Dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_mode, sizeof(dark_mode));
    // Keep Light's existing Mica treatment; Dark must be opaque, not wallpaper-tinted.
    const DWM_SYSTEMBACKDROP_TYPE backdrop = dark_mode ? DWMSBT_NONE : DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void PreferNativeRoundedCorners(HWND hwnd) {
    const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(
        hwnd,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &preference,
        sizeof(preference)
    );
}

HICON LoadWatchmanIcon(HINSTANCE instance, int desired_size) {
    HICON icon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_WATCHMAN_APP),
        IMAGE_ICON,
        desired_size,
        desired_size,
        LR_DEFAULTCOLOR | LR_SHARED
    ));
    if (!icon) {
        icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }
    return icon;
}

using NetworkAdapterMenuChoice = std::pair<UINT, std::wstring>;

struct FloatingMonitorDisplayInfo {
    HMONITOR monitor = nullptr;
    std::wstring device;
    std::wstring label;
    bool primary = false;
};

using FloatingMonitorDisplayMenuChoice = std::pair<UINT, std::wstring>;

BOOL CALLBACK CollectFloatingMonitorDisplay(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* displays = reinterpret_cast<std::vector<FloatingMonitorDisplayInfo>*>(context);
    if (!displays) {
        return FALSE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    const bool primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    const int number = static_cast<int>(displays->size()) + 1;
    FloatingMonitorDisplayInfo display;
    display.monitor = monitor;
    display.device = info.szDevice;
    display.label = primary ? L"Primary display" : L"Display " + std::to_wstring(number);
    display.primary = primary;
    displays->push_back(std::move(display));
    return TRUE;
}

std::vector<FloatingMonitorDisplayInfo> GetFloatingMonitorDisplays() {
    std::vector<FloatingMonitorDisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr, CollectFloatingMonitorDisplay, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

HMONITOR GetSelectedFloatingMonitorDisplay() {
    const std::wstring selected_device = g_renderer ? g_renderer->GetCommittedFloatingMonitorDisplayDevice() : L"";
    if (!selected_device.empty()) {
        for (const FloatingMonitorDisplayInfo& display : GetFloatingMonitorDisplays()) {
            if (_wcsicmp(display.device.c_str(), selected_device.c_str()) == 0) {
                return display.monitor;
            }
        }
    }
    return MonitorFromWindow(g_main_hwnd, MONITOR_DEFAULTTOPRIMARY);
}

std::wstring EscapeMenuText(const std::wstring& text) {
    std::wstring escaped;
    escaped.reserve(text.size());
    for (const wchar_t character : text) {
        if (character == L'&') {
            escaped += L"&&";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

std::wstring NetworkAdapterMenuLabel(const InterfaceInfo& adapter) {
    // Meaningful adapters have short friendly descriptions such as the Wi-Fi
    // device name or a phone's USB tethering name. Prefer that over generic
    // aliases like "Wi-Fi 2" or "Ethernet".
    std::wstring label = !adapter.description.empty()
        ? adapter.description
        : (adapter.name.empty()
        ? (adapter.type.empty() ? L"Network adapter" : adapter.type)
        : adapter.name);
    if (!adapter.is_connected) {
        label += L" (not connected)";
    }
    return EscapeMenuText(label);
}

std::vector<NetworkAdapterMenuChoice> AppendNetworkAdapterChoices(HMENU menu, UINT first_command) {
    std::vector<NetworkAdapterMenuChoice> choices;
    if (!menu || !g_renderer || !g_engine) {
        return choices;
    }

    const std::wstring selected_id = g_renderer->GetNetworkAdapterId();
    const bool automatic = selected_id.empty();
    AppendMenuW(
        menu,
        MF_STRING | (automatic ? MF_CHECKED : 0),
        first_command,
        L"Auto (follow Windows default)"
    );
    choices.emplace_back(first_command, L"");

    const std::vector<InterfaceInfo> adapters = g_engine->GetNetworkAdapters();
    if (!adapters.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    bool selected_adapter_found = automatic;
    UINT command = first_command + 1;
    for (const InterfaceInfo& adapter : adapters) {
        const bool selected = !selected_id.empty() &&
            _wcsicmp(selected_id.c_str(), adapter.id.c_str()) == 0;
        selected_adapter_found = selected_adapter_found || selected;
        AppendMenuW(menu, MF_STRING | (selected ? MF_CHECKED : 0), command, NetworkAdapterMenuLabel(adapter).c_str());
        choices.emplace_back(command, adapter.id);
        ++command;
    }

    if (!selected_adapter_found) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_CHECKED, 0, L"Selected adapter unavailable");
    }
    return choices;
}

void ApplyNetworkAdapterSelection(const std::wstring& adapter_id) {
    if (!g_renderer || !g_engine) {
        return;
    }

    // Adapter selection inside Settings belongs to the staged draft. The
    // running engine must not switch until the user explicitly saves.
    const bool settings_open = g_renderer->IsSettingsOpen();
    g_renderer->SetNetworkAdapterId(adapter_id);
    if (settings_open) {
        if (g_main_hwnd) {
            InvalidateRect(g_main_hwnd, nullptr, FALSE);
        }
        return;
    }

    g_engine->SetNetworkAdapterId(adapter_id);
    // This only performs a WLAN query when the Network tab is already active.
    // It lets a newly selected Wi-Fi adapter update its SSID immediately.
    g_engine->RefreshNetworkDetails();
    g_renderer->SaveSettings();

    if (g_main_hwnd) {
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
    }
    if (g_widget_hwnd) {
        InvalidateRect(g_widget_hwnd, nullptr, FALSE);
    }
}

bool ApplyNetworkAdapterMenuChoice(UINT command, const std::vector<NetworkAdapterMenuChoice>& choices) {
    for (const NetworkAdapterMenuChoice& choice : choices) {
        if (choice.first == command) {
            ApplyNetworkAdapterSelection(choice.second);
            return true;
        }
    }
    return false;
}

std::vector<FloatingMonitorDisplayMenuChoice> AppendFloatingMonitorDisplayChoices(HMENU menu, UINT first_command) {
    std::vector<FloatingMonitorDisplayMenuChoice> choices;
    if (!menu || !g_renderer) {
        return choices;
    }

    const std::wstring selected_device = g_renderer->GetFloatingMonitorDisplayDevice();
    const bool primary_selected = selected_device.empty();
    AppendMenuW(menu, MF_STRING | (primary_selected ? MF_CHECKED : 0), first_command, L"Primary display");
    choices.emplace_back(first_command, L"");

    const std::vector<FloatingMonitorDisplayInfo> displays = GetFloatingMonitorDisplays();
    bool selected_found = primary_selected;
    UINT command = first_command + 1;
    for (const FloatingMonitorDisplayInfo& display : displays) {
        if (display.primary) {
            continue;
        }
        const bool selected = !selected_device.empty() &&
            _wcsicmp(selected_device.c_str(), display.device.c_str()) == 0;
        selected_found = selected_found || selected;
        AppendMenuW(menu, MF_STRING | (selected ? MF_CHECKED : 0), command, display.label.c_str());
        choices.emplace_back(command, display.device);
        ++command;
    }

    if (!selected_found) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_CHECKED, 0, L"Selected display unavailable");
    }
    return choices;
}

void ApplyFloatingMonitorDisplaySelection(const std::wstring& display_device) {
    if (!g_renderer) {
        return;
    }

    const bool settings_open = g_renderer->IsSettingsOpen();
    g_renderer->SetFloatingMonitorDisplayDevice(display_device);
    if (settings_open) {
        if (g_main_hwnd) {
            InvalidateRect(g_main_hwnd, nullptr, FALSE);
        }
        return;
    }

    g_renderer->SaveSettings();
    UpdateFloatingMonitorState(true);
}

bool ApplyFloatingMonitorDisplayMenuChoice(UINT command, const std::vector<FloatingMonitorDisplayMenuChoice>& choices) {
    for (const FloatingMonitorDisplayMenuChoice& choice : choices) {
        if (choice.first == command) {
            ApplyFloatingMonitorDisplaySelection(choice.second);
            return true;
        }
    }
    return false;
}

bool IsFourDigitYear(const std::wstring& value, int& year) {
    if (value.size() < 4) return false;
    int parsed = 0;
    for (size_t index = 0; index < 4; ++index) {
        const wchar_t character = value[index];
        if (character < L'0' || character > L'9') return false;
        parsed = parsed * 10 + (character - L'0');
    }
    year = parsed;
    return true;
}

std::vector<std::wstring> GetSavedExportMonths(const TrafficHistorySnapshot& history) {
    std::vector<std::wstring> months;
    for (const TrafficHistoryItem& item : history.history) {
        if (item.date.size() == 10 && item.date[4] == L'-' && item.date[7] == L'-') {
            months.push_back(item.date.substr(0, 7));
        }
    }
    std::sort(months.begin(), months.end(), std::greater<>());
    months.erase(std::unique(months.begin(), months.end()), months.end());
    return months;
}

std::vector<int> GetSavedExportYears(const TrafficHistorySnapshot& history) {
    std::vector<int> years = history.available_years;
    for (const TrafficHistoryItem& item : history.history) {
        int year = 0;
        if (IsFourDigitYear(item.date, year) && year > 0) {
            years.push_back(year);
        }
    }
    std::sort(years.begin(), years.end(), std::greater<>());
    years.erase(std::unique(years.begin(), years.end()), years.end());
    return years;
}

std::wstring FormatExportMonthMenuLabel(const std::wstring& month_key) {
    static constexpr const wchar_t* kMonths[] = {
        L"January", L"February", L"March", L"April", L"May", L"June",
        L"July", L"August", L"September", L"October", L"November", L"December",
    };
    if (month_key.size() == 7 && month_key[4] == L'-') {
        const int month = (month_key[5] - L'0') * 10 + (month_key[6] - L'0');
        if (month >= 1 && month <= 12) {
            return std::wstring(kMonths[month - 1]) + L" " + month_key.substr(0, 4);
        }
    }
    return month_key;
}

LRESULT HitTestMainWindowFrame(HWND hwnd, LPARAM lParam) {
    POINT screen_point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    RECT window_rect{};
    GetWindowRect(hwnd, &window_rect);

    if (!IsZoomed(hwnd)) {
        const UINT dpi = GetDpiForWindow(hwnd);
        const int resize_border = std::max(
            6,
            GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
            GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)
        );
        const bool on_left = screen_point.x < window_rect.left + resize_border;
        const bool on_right = screen_point.x >= window_rect.right - resize_border;
        const bool on_top = screen_point.y < window_rect.top + resize_border;
        const bool on_bottom = screen_point.y >= window_rect.bottom - resize_border;

        if (on_top && on_left) return HTTOPLEFT;
        if (on_top && on_right) return HTTOPRIGHT;
        if (on_bottom && on_left) return HTBOTTOMLEFT;
        if (on_bottom && on_right) return HTBOTTOMRIGHT;
        if (on_left) return HTLEFT;
        if (on_right) return HTRIGHT;
        if (on_top) return HTTOP;
        if (on_bottom) return HTBOTTOM;
    }

    POINT client_point = screen_point;
    ScreenToClient(hwnd, &client_point);
    if (g_renderer && g_renderer->IsTitlebarDragRegion(client_point.x, client_point.y)) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

} // namespace

auto GetWidgetPreferredSize(const TaskbarMetrics& metrics) {
    return TaskbarWidgetLayout::PreferredSize(
        g_renderer ? g_renderer->GetWidgetMode() : TaskbarWidgetMode::All,
        TaskbarWidgetLayout::IsVertical(metrics.edge), metrics.thickness, metrics.dpi);
}

bool ApplyWidgetTaskbarMetrics(HWND hwnd, const TaskbarMetrics& metrics) {
    const HANDLE edge = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(metrics.edge + 1));
    const HANDLE dpi = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(metrics.dpi));
    const bool changed = GetPropW(hwnd, L"Watchman.TaskbarEdge") != edge ||
                         GetPropW(hwnd, L"Watchman.TaskbarDpi") != dpi;
    SetPropW(hwnd, L"Watchman.TaskbarEdge", edge);
    SetPropW(hwnd, L"Watchman.TaskbarDpi", dpi);
    return changed;
}

void ShowNetworkAdapterPicker(HWND owner_hwnd) {
    if (!owner_hwnd || !g_renderer || !g_engine) {
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    const std::vector<NetworkAdapterMenuChoice> choices =
        AppendNetworkAdapterChoices(menu, kNetworkAdapterMenuFirstCommand);
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner_hwnd);
    const UINT selected = TrackPopupMenu(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        cursor.x,
        cursor.y,
        0,
        owner_hwnd,
        nullptr
    );
    DestroyMenu(menu);
    ApplyNetworkAdapterMenuChoice(selected, choices);
    PostMessageW(owner_hwnd, WM_NULL, 0, 0);
}

void ShowFloatingMonitorDisplayPicker(HWND owner_hwnd) {
    if (!owner_hwnd || !g_renderer) {
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    const std::vector<FloatingMonitorDisplayMenuChoice> choices =
        AppendFloatingMonitorDisplayChoices(menu, kFloatingMonitorDisplayMenuFirstCommand);
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner_hwnd);
    const UINT selected = TrackPopupMenu(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        cursor.x,
        cursor.y,
        0,
        owner_hwnd,
        nullptr
    );
    DestroyMenu(menu);
    ApplyFloatingMonitorDisplayMenuChoice(selected, choices);
    PostMessageW(owner_hwnd, WM_NULL, 0, 0);
}

void ShowToolsExportPicker(HWND owner_hwnd, ToolsExportPicker request) {
    if (!owner_hwnd || !g_renderer || !g_engine || request == ToolsExportPicker::None) {
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const TrafficHistorySnapshot history = g_engine->GetTrafficHistorySnapshot();
    const UINT first_command = kToolsExportMenuFirstCommand;
    if (request == ToolsExportPicker::Period) {
        static constexpr const wchar_t* kPeriods[] = {
            L"Daily", L"Weekly", L"Monthly", L"Yearly",
        };
        const int selected_period = g_renderer->GetToolsExportPeriodIndex();
        for (int index = 0; index < static_cast<int>(std::size(kPeriods)); ++index) {
            AppendMenuW(
                menu,
                MF_STRING | (index == selected_period ? MF_CHECKED : 0),
                first_command + static_cast<UINT>(index),
                kPeriods[index]
            );
        }
    } else if (request == ToolsExportPicker::Month) {
        const std::vector<std::wstring> months = GetSavedExportMonths(history);
        if (months.empty()) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No saved months yet");
        } else {
            for (size_t index = 0; index < months.size(); ++index) {
                const bool selected = months[index] == g_renderer->GetToolsExportMonth();
                const std::wstring label = FormatExportMonthMenuLabel(months[index]);
                AppendMenuW(
                    menu,
                    MF_STRING | (selected ? MF_CHECKED : 0),
                    first_command + static_cast<UINT>(index),
                    label.c_str()
                );
            }
        }
    } else if (request == ToolsExportPicker::Year) {
        const std::vector<int> years = GetSavedExportYears(history);
        if (years.empty()) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No saved years yet");
        } else {
            for (size_t index = 0; index < years.size(); ++index) {
                const bool selected = years[index] == g_renderer->GetToolsExportYear();
                const std::wstring label = std::to_wstring(years[index]);
                AppendMenuW(
                    menu,
                    MF_STRING | (selected ? MF_CHECKED : 0),
                    first_command + static_cast<UINT>(index),
                    label.c_str()
                );
            }
        }
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner_hwnd);
    const UINT selected = TrackPopupMenu(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        cursor.x,
        cursor.y,
        0,
        owner_hwnd,
        nullptr
    );
    DestroyMenu(menu);

    if (selected >= first_command) {
        const size_t index = static_cast<size_t>(selected - first_command);
        if (request == ToolsExportPicker::Period && index < 4) {
            g_renderer->SetToolsExportPeriodIndex(static_cast<int>(index));
        } else if (request == ToolsExportPicker::Month) {
            const std::vector<std::wstring> months = GetSavedExportMonths(history);
            if (index < months.size()) g_renderer->SetToolsExportMonth(months[index]);
        } else if (request == ToolsExportPicker::Year) {
            const std::vector<int> years = GetSavedExportYears(history);
            if (index < years.size()) g_renderer->SetToolsExportYear(years[index]);
        }
        InvalidateRect(owner_hwnd, nullptr, FALSE);
    }
    PostMessageW(owner_hwnd, WM_NULL, 0, 0);
}

void ShowWidgetContextMenu(HWND owner_hwnd) {
    if (!g_renderer || !g_engine || !owner_hwnd) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (shell) {
        SendMessageW(shell, WM_CANCELMODE, 0, 0);
    }

    AppendMenuW(menu, MF_STRING, 1001, L"Open Watchman");
    AppendMenuW(menu, MF_STRING, 1002, L"Open History");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1003, L"View Today");
    AppendMenuW(menu, MF_STRING, 1004, L"View Last 7 Days");
    AppendMenuW(menu, MF_STRING, 1005, L"View Monthly");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU widget_menu = CreatePopupMenu();
    const TaskbarWidgetMode mode = g_renderer->GetWidgetMode();
    AppendMenuW(widget_menu, MF_STRING, 1101, L"Upload && download");
    AppendMenuW(widget_menu, MF_STRING, 1102, L"CPU && memory");
    AppendMenuW(widget_menu, MF_STRING, 1103, L"All four");
    CheckMenuRadioItem(widget_menu, 1101, 1103,
        mode == TaskbarWidgetMode::Network ? 1101 : mode == TaskbarWidgetMode::System ? 1102 : 1103,
        MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widget_menu), L"Widget");

    HMENU adapter_menu = CreatePopupMenu();
    const std::vector<NetworkAdapterMenuChoice> adapter_choices =
        AppendNetworkAdapterChoices(adapter_menu, kNetworkAdapterMenuFirstCommand);
    if (adapter_menu) {
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(adapter_menu), L"Network adapter");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1006, L"Reset session counters");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1007, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner_hwnd);
    tm_set_widget_context_menu_open(owner_hwnd, 1);
    UINT selected = TrackPopupMenu(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        cursor.x,
        cursor.y,
        0,
        owner_hwnd,
        nullptr
    );
    tm_set_widget_context_menu_open(owner_hwnd, 0);
    PostMessageW(owner_hwnd, WM_NULL, 0, 0);
    if (shell) {
        SendMessageW(shell, WM_CANCELMODE, 0, 0);
    }
    DestroyMenu(menu);

    if (ApplyNetworkAdapterMenuChoice(selected, adapter_choices)) {
        return;
    }

    switch (selected) {
    case 1001:
        ShowWindow(g_main_hwnd, SW_RESTORE);
        SetForegroundWindow(g_main_hwnd);
        g_renderer->SetActiveTab(ActiveTab::Dashboard);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 1002:
        ShowWindow(g_main_hwnd, SW_RESTORE);
        SetForegroundWindow(g_main_hwnd);
        g_renderer->SetActiveTab(ActiveTab::Data);
        g_renderer->SetDataRangeIndex(1);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 1003:
        ShowWindow(g_main_hwnd, SW_RESTORE);
        SetForegroundWindow(g_main_hwnd);
        g_renderer->SetActiveTab(ActiveTab::Data);
        g_renderer->SetDataRangeIndex(0);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 1004:
        ShowWindow(g_main_hwnd, SW_RESTORE);
        SetForegroundWindow(g_main_hwnd);
        g_renderer->SetActiveTab(ActiveTab::Data);
        g_renderer->SetDataRangeIndex(1);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 1005:
        ShowWindow(g_main_hwnd, SW_RESTORE);
        SetForegroundWindow(g_main_hwnd);
        g_renderer->SetActiveTab(ActiveTab::Data);
        g_renderer->SetDataRangeIndex(2);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 1101:
    case 1102:
    case 1103:
        g_renderer->SetWidgetMode(selected == 1101 ? TaskbarWidgetMode::Network :
            selected == 1102 ? TaskbarWidgetMode::System : TaskbarWidgetMode::All);
        g_renderer->SaveSettings();
        UpdateWidgetState(true);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 1006:
        g_engine->ResetSession();
        InvalidateRect(g_widget_hwnd, nullptr, FALSE);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 1007:
        PostMessageW(g_main_hwnd, WatchmanWindowLifecycle::ExitMessage, 0, 0);
        break;
    default:
        break;
    }
}

bool AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_WATCHMAN_TRAY;
    nid.hIcon = LoadWatchmanIcon(GetModuleHandleW(nullptr), GetSystemMetrics(SM_CXSMICON));
    wcscpy_s(nid.szTip, L"Watchman");
    // Refresh an existing icon, or recreate it if Explorer has dropped it.
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE ||
        Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowFloatingMonitorContextMenu(HWND owner_hwnd, POINT position) {
    if (!g_renderer || !owner_hwnd) return;

    enum : UINT {
        lock_position = 3001,
        always_on_top,
        transparent_background,
        click_through,
        reset_position,
        settings,
        hide_monitor,
        opacity_first = 3100,
        font_size_first = 3300
    };

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    auto append_toggle = [menu](UINT command, const wchar_t* label, bool checked) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), command, label);
    };
    append_toggle(lock_position, L"Lock position", g_renderer->IsFloatingMonitorPositionLocked());
    append_toggle(always_on_top, L"Always on top", g_renderer->IsFloatingMonitorAlwaysOnTop());
    append_toggle(transparent_background, L"Transparent background", g_renderer->IsFloatingMonitorBackgroundTransparent());

    HMENU opacity_menu = CreatePopupMenu();
    if (opacity_menu) {
        const int current_opacity = g_renderer->GetFloatingMonitorOpacity();
        bool current_listed = false;
        for (int opacity = FloatingMonitorLayout::minimum_opacity;
             opacity <= FloatingMonitorLayout::maximum_opacity; opacity += 20) {
            const std::wstring label = std::to_wstring(opacity) + L"%";
            const bool checked = current_opacity == opacity;
            current_listed = current_listed || checked;
            AppendMenuW(opacity_menu, MF_STRING | (checked ? MF_CHECKED : 0),
                        opacity_first + opacity, label.c_str());
        }
        if (!current_listed) {
            const std::wstring label = L"Current: " + std::to_wstring(current_opacity) + L"%";
            AppendMenuW(opacity_menu, MF_STRING | MF_GRAYED | MF_CHECKED, 0, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity_menu), L"Panel opacity");
    }

    HMENU font_size_menu = CreatePopupMenu();
    if (font_size_menu) {
        for (int size = FloatingMonitorLayout::minimum_font_size; size <= FloatingMonitorLayout::maximum_font_size; ++size) {
            const std::wstring label = std::to_wstring(size) + L" px" + (size == FloatingMonitorLayout::default_font_size ? L" (default)" : L"");
            AppendMenuW(font_size_menu, MF_STRING | (size == g_renderer->GetFloatingMonitorFontSize() ? MF_CHECKED : 0),
                        font_size_first + size, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(font_size_menu), L"Text size (Segoe UI Bold)");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    append_toggle(click_through, L"Click-through", g_renderer->IsFloatingMonitorClickThrough());
    AppendMenuW(menu, MF_STRING, reset_position, L"Reset position");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, settings, L"Floating monitor settings");
    AppendMenuW(menu, MF_STRING, hide_monitor, L"Hide monitor");

    SetForegroundWindow(owner_hwnd);
    const UINT selected = TrackPopupMenu(
        menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        position.x, position.y, 0, owner_hwnd, nullptr
    );
    PostMessageW(owner_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (selected == settings) {
        g_renderer->OpenFloatingMonitorSettings();
        ShowWindow(g_main_hwnd, SW_RESTORE);
        SetForegroundWindow(g_main_hwnd);
        SetFocus(g_main_hwnd);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        return;
    }
    if (selected == reset_position) {
        UpdateFloatingMonitorState(true);
        g_renderer->SaveSettings();
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        return;
    }

    switch (selected) {
    case lock_position:
        g_renderer->SetFloatingMonitorPositionLocked(!g_renderer->IsFloatingMonitorPositionLocked());
        break;
    case always_on_top:
        g_renderer->SetFloatingMonitorAlwaysOnTop(!g_renderer->IsFloatingMonitorAlwaysOnTop());
        break;
    case transparent_background:
        g_renderer->SetFloatingMonitorBackgroundTransparent(!g_renderer->IsFloatingMonitorBackgroundTransparent());
        break;
    case click_through:
        g_renderer->SetFloatingMonitorClickThrough(!g_renderer->IsFloatingMonitorClickThrough());
        break;
    case hide_monitor:
        g_renderer->SetFloatingMonitorEnabled(false);
        break;
    default:
        if (selected >= font_size_first + FloatingMonitorLayout::minimum_font_size &&
            selected <= font_size_first + FloatingMonitorLayout::maximum_font_size) {
            g_renderer->SetFloatingMonitorFontSize(static_cast<int>(selected - font_size_first));
            break;
        }
        if (selected < opacity_first + FloatingMonitorLayout::minimum_opacity ||
            selected > opacity_first + FloatingMonitorLayout::maximum_opacity ||
            (selected - opacity_first) % 20 != 0) return;
        const int opacity = static_cast<int>(selected - opacity_first);
        g_renderer->SetFloatingMonitorOpacity(opacity);
        break;
    }

    g_renderer->SaveSettings();
    UpdateFloatingMonitorState(false);
    InvalidateRect(g_main_hwnd, nullptr, FALSE);
}

void RestartWatchman(HWND hwnd, bool as_admin = false) {
    static bool restart_pending = false;
    if (restart_pending || !g_renderer) return;
    const auto report_failure = [hwnd](DWORD error) {
        const std::wstring text = L"Watchman could not restart (Windows error " + std::to_wstring(error)
            + L"). The current instance is still running.";
        MessageBoxW(hwnd, text.c_str(), L"Watchman restart", MB_OK | MB_ICONERROR);
    };

    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) { report_failure(GetLastError()); return; }
    if (length >= path.size()) { report_failure(ERROR_INSUFFICIENT_BUFFER); return; }
    const std::wstring executable(path.data(), length);
    const bool start_hidden = !IsWindowVisible(hwnd) || IsIconic(hwnd);
    std::wstring command = WatchmanRuntime::RestartCommand(executable, GetCurrentProcessId(), start_hidden);
    if (command.empty()) { report_failure(ERROR_INVALID_NAME); return; }

    // Save committed preferences only, not an unsaved settings draft.
    g_renderer->SaveSettings();

    if (as_admin) {
        // Use "runas" verb for administrator elevation
        const std::wstring parameters = L"--replace-pid " + std::to_wstring(GetCurrentProcessId())
            + (start_hidden ? L" --startup" : L"");
        const HINSTANCE launched = ShellExecuteW(hwnd, L"runas", executable.c_str(), parameters.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32) {
            // User cancelled UAC prompt or elevation failed
            return;
        }
        restart_pending = true;
        PostMessageW(hwnd, WatchmanWindowLifecycle::ExitMessage, 0, 0);
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // The calling token is retained, including elevation; no handles are inherited.
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process)) {
        report_failure(GetLastError());
        return;
    }
    restart_pending = true;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    // The replacement waits for this process to finish normal engine/storage cleanup.
    PostMessageW(hwnd, WatchmanWindowLifecycle::ExitMessage, 0, 0);
}

void ShowTrayMenu(HWND hwnd) {
    if (!g_renderer || !g_engine) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    bool widget_enabled = g_renderer->IsWidgetEnabled();
    bool floating_enabled = g_renderer->IsFloatingMonitorEnabled();
    AppendMenuW(menu, MF_STRING, 2001, L"Open Watchman");
    AppendMenuW(menu, MF_STRING | (widget_enabled ? MF_CHECKED : 0), 2002, widget_enabled ? L"Hide Taskbar Widget" : L"Show Taskbar Widget");
    AppendMenuW(menu, MF_STRING | (floating_enabled ? MF_CHECKED : 0), 2004, floating_enabled ? L"Hide Floating Monitor" : L"Show Floating Monitor");
    AppendMenuW(menu, MF_STRING, 2005, L"Floating monitor options...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2007, L"Run as administrator");
    AppendMenuW(menu, MF_STRING, 2006, L"Restart Watchman");
    AppendMenuW(menu, MF_STRING, 2003, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd);
    UINT selected = TrackPopupMenu(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        cursor.x,
        cursor.y,
        0,
        hwnd,
        nullptr
    );
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (selected) {
    case 2001:
        ShowWindow(g_main_hwnd, SW_RESTORE);
        SetForegroundWindow(g_main_hwnd);
        break;
    case 2002:
        g_renderer->SetWidgetEnabled(!widget_enabled);
        g_renderer->SaveSettings();
        UpdateWidgetState(true);
        break;
    case 2004:
        g_renderer->SetFloatingMonitorEnabled(!g_renderer->IsFloatingMonitorEnabled());
        g_renderer->SaveSettings();
        UpdateFloatingMonitorState(false);
        InvalidateRect(g_main_hwnd, nullptr, FALSE);
        break;
    case 2005:
        ShowFloatingMonitorContextMenu(hwnd, cursor);
        break;
    case 2007:
        RestartWatchman(hwnd, true);
        break;
    case 2006:
        RestartWatchman(hwnd, false);
        break;
    case 2003:
        PostMessageW(g_main_hwnd, WatchmanWindowLifecycle::ExitMessage, 0, 0);
        break;
    default:
        break;
    }
}



LONG SetStartupRegistry(bool enable) {
    std::wstring command;
    if (enable) {
        std::vector<wchar_t> path(32768);
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return GetLastError();
        if (length >= path.size()) return ERROR_INSUFFICIENT_BUFFER;
        command = WatchmanRuntime::StartupCommand(std::wstring(path.data(), length));
        if (command.empty()) return ERROR_INVALID_NAME;
    }
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;
    if (enable) {
        result = RegSetValueExW(key, L"Watchman", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"Watchman");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result;
}

void ShowMainPage(ActiveTab tab) {
    if (!g_renderer || !g_main_hwnd) return;
    g_renderer->DismissSettings();
    g_renderer->SetActiveTab(tab);

    ShowWindow(g_main_hwnd, SW_RESTORE);
    SetForegroundWindow(g_main_hwnd);
    InvalidateRect(g_main_hwnd, nullptr, FALSE);
}

namespace {
class InstanceGuard {
public:
    ~InstanceGuard() {
        if (owns_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }
    bool Acquire() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> user(size);
        const BOOL queried = GetTokenInformation(token, TokenUser, user.data(), size, &size);
        CloseHandle(token);
        if (!queried) return false;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return false;
        const std::wstring name = L"Local\\Watchman.Session." + std::wstring(sid);
        LocalFree(sid);
        handle_ = CreateMutexExW(nullptr, name.c_str(), CREATE_MUTEX_INITIAL_OWNER, SYNCHRONIZE | MUTEX_MODIFY_STATE);
        const DWORD error = GetLastError();
        if (!handle_) {
            // An elevated instance may deny modify access to a normal launch.
            handle_ = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
            existing_ = handle_ != nullptr;
            return false;
        }
        if (error == ERROR_ALREADY_EXISTS) {
            const DWORD wait = WaitForSingleObject(handle_, 0);
            owns_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
            existing_ = !owns_;
        } else owns_ = true;
        return owns_;
    }
    bool Existing() const { return existing_; }
private:
    HANDLE handle_ = nullptr;
    bool owns_ = false;
    bool existing_ = false;
};
} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    int argument_count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    std::vector<std::wstring> arguments;
    if (argv) {
        for (int index = 0; index < argument_count; ++index) arguments.emplace_back(argv[index]);
        LocalFree(argv);
    }
    const auto launch = WatchmanRuntime::ParseArguments(arguments);
    if (launch.replace_pid && launch.replace_pid != GetCurrentProcessId()) {
        HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, launch.replace_pid);
        if (previous) {
            const DWORD wait = WaitForSingleObject(previous, 15000);
            CloseHandle(previous);
            if (wait != WAIT_OBJECT_0) {
                MessageBoxW(nullptr, L"The previous Watchman instance is still shutting down. Wait for it to exit, then open Watchman again.",
                    L"Watchman restart", MB_OK | MB_ICONWARNING);
                return 1;
            }
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) return 1;
    }
    InstanceGuard instance;
    if (!instance.Acquire()) {
        if (instance.Existing()) {
            if (!launch.startup) {
                // The class is the stable identity; a caption can be localized
                // or changed without breaking second-launch tray recovery.
                HWND existing = FindWindowW(L"WatchmanWindowClass", nullptr);
                if (existing) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(existing, &pid);
                    AllowSetForegroundWindow(pid);
                    PostMessageW(existing, WM_WATCHMAN_ACTIVATE, 0, 0);
                } else MessageBoxW(nullptr, L"Watchman is already starting. Use its tray icon once startup completes.", L"Watchman", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        MessageBoxW(nullptr, L"Watchman could not establish its single-instance guard. No monitoring was started.", L"Watchman", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize WinSock (needed for ping sockets and general networking)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    const HRESULT com_initialize_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_initialized = SUCCEEDED(com_initialize_result);

    // GDI's layered taskbar fallback also uses the embedded fonts in memory.
    // DirectWrite owns its own explicit resource collection in each renderer.
    WatchmanFonts::GdiRegistration bundled_gdi_fonts;
    if (FAILED(bundled_gdi_fonts.Initialize(hInstance))) {
        OutputDebugStringW(L"Watchman: could not register embedded fonts for the GDI fallback.\n");
    }

    // Settings are intentionally loaded before the engine so the taskbar
    // widget can appear without waiting for storage, app tracking, or WMI.
    g_renderer = new AppRenderer();
    g_renderer->LoadSettings();
    const LONG startup_result = SetStartupRegistry(g_renderer->IsAutostartEnabled());
    if (startup_result != ERROR_SUCCESS) {
        const std::wstring message = L"Windows startup registration could not be updated (error "
            + std::to_wstring(startup_result) + L"). Check Settings > General and Windows Startup apps.";
        MessageBoxW(nullptr, message.c_str(), L"Watchman startup", MB_OK | MB_ICONWARNING);
    }



    // 1. Register Main Window Class
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = LoadWatchmanIcon(hInstance, 256);
    wcex.hIconSm = LoadWatchmanIcon(hInstance, GetSystemMetrics(SM_CXSMICON));
    wcex.hbrBackground = nullptr; // Let Direct2D/DWM draw background
    wcex.lpszClassName = L"WatchmanWindowClass";
    RegisterClassExW(&wcex);

    // 2. Register Widget Window Class
    WNDCLASSEXW wcex_widget = {};
    wcex_widget.cbSize = sizeof(WNDCLASSEXW);
    wcex_widget.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex_widget.lpfnWndProc = WidgetWndProc;
    wcex_widget.hInstance = hInstance;
    wcex_widget.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex_widget.hbrBackground = nullptr;
    wcex_widget.lpszClassName = L"WatchmanWidgetClass";
    RegisterClassExW(&wcex_widget);

    WNDCLASSEXW wcex_floating = {};
    wcex_floating.cbSize = sizeof(WNDCLASSEXW);
    wcex_floating.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex_floating.lpfnWndProc = FloatingMonitorWndProc;
    wcex_floating.hInstance = hInstance;
    wcex_floating.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex_floating.hbrBackground = nullptr;
    wcex_floating.lpszClassName = L"WatchmanFloatingMonitorClass";
    RegisterClassExW(&wcex_floating);

    // Create the tiny widget host before the heavier dashboard and services.
    TaskbarMetrics widget_metrics{};
    tm_get_taskbar_metrics(&widget_metrics);
    const auto widget_size = GetWidgetPreferredSize(widget_metrics);
    g_widget_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"WatchmanWidgetClass",
        L"Watchman Widget",
        WS_POPUP,
        0, 0,
        widget_size.width, widget_size.height,
        nullptr, nullptr, hInstance, nullptr
    );
    if (g_widget_hwnd) {
        ApplyWidgetTaskbarMetrics(g_widget_hwnd, widget_metrics);
        // Match TrafficMonitor's DirectComposition path: this starts as a
        // normal transparent composition window, not a layered GDI popup.
        tm_set_widget_layered_style(0);
        tm_set_widget_manual_layered_paint(0);
        tm_apply_widget_styles(g_widget_hwnd);
        UpdateWidgetState(true);
    }

    g_floating_monitor_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"WatchmanFloatingMonitorClass",
        L"Watchman Mini Monitor",
        WS_POPUP,
        0, 0, GetFloatingMonitorPreferredWidth(), GetFloatingMonitorPreferredHeight(),
        nullptr, nullptr, hInstance, nullptr
    );

    // The engine itself runs asynchronously, but constructing its services can
    // touch storage and diagnostics. Keep that work after the widget is shown.
    g_engine = new WatchmanEngine();
    g_engine->SetNetworkAdapterId(g_renderer->GetNetworkAdapterId());
    g_engine->Start();

    // Keep the larger, readable default inside the available desktop, including
    // laptop screens with short work areas. These are final on-screen dimensions.
    RECT main_work_area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &main_work_area, 0);
    const auto main_size = UIStyle::InitialWindowSize(
        main_work_area.right - main_work_area.left, main_work_area.bottom - main_work_area.top);
    g_main_hwnd = CreateWindowExW(
        0,
        L"WatchmanWindowClass",
        L"WATCHMAN",
        kMainWindowStyle,
        main_work_area.left + (main_work_area.right - main_work_area.left - main_size.width) / 2,
        main_work_area.top + (main_work_area.bottom - main_work_area.top - main_size.height) / 2,
        main_size.width, main_size.height,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_main_hwnd) {
        return FALSE;
    }

    SendMessageW(g_main_hwnd, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(LoadWatchmanIcon(hInstance, 256)));
    SendMessageW(g_main_hwnd, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(LoadWatchmanIcon(hInstance, GetSystemMetrics(SM_CXSMICON))));

    PreferNativeRoundedCorners(g_main_hwnd);
    ApplyMainWindowCorners(
        g_main_hwnd,
        main_size.width,
        main_size.height
    );

    // The embedded widget's hook posts commands straight to this UI thread.
    tm_set_widget_command_target(
        g_main_hwnd,
        WM_WATCHMAN_WIDGET_MENU,
        WM_WATCHMAN_WIDGET_DATA
    );

    ApplyMainWindowTheme(g_main_hwnd);

    // Initialize Direct2D Renderer with Main Window
    if (FAILED(g_renderer->Initialize(g_main_hwnd))) {
        return FALSE;
    }

    // Only this payload-free restore message crosses the elevation boundary.
    ChangeWindowMessageFilterEx(g_main_hwnd, WM_WATCHMAN_ACTIVATE, MSGFLT_ALLOW, nullptr);
    if (WM_TASKBAR_CREATED) ChangeWindowMessageFilterEx(g_main_hwnd, WM_TASKBAR_CREATED, MSGFLT_ALLOW, nullptr);
    const bool tray_ready = AddTrayIcon(g_main_hwnd);
    g_renderer->OfferWelcomeTour(WatchmanWelcome::IsInteractiveLaunch(launch.startup, launch.replace_pid));
    if (!launch.startup || !tray_ready) {
        ShowWindow(g_main_hwnd, launch.startup ? SW_SHOWNORMAL : nCmdShow);
        UpdateWindow(g_main_hwnd);
    }

    g_welcome_presentation_ready = true;

    // Run initial widget state based on loaded settings
    UpdateWidgetState(true);
    UpdateFloatingMonitorState(false);

    // Timer 1 animates only the visible, restored foreground main window.
    UpdateMainUiActivity(g_main_hwnd, IsWindowVisible(g_main_hwnd) != FALSE,
        IsIconic(g_main_hwnd) != FALSE, GetForegroundWindow() == g_main_hwnd);
    // Timer 2: Stats Update loop (1000ms = 1s)
    SetTimer(g_main_hwnd, 2, 1000, nullptr);
    // Match TrafficMonitor: quick shell-layout checks with no widget redraw.
    SetTimer(g_main_hwnd, 3, 100, nullptr);

    // Main Message Loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Stop diagnostics and clean up
    tm_restore_taskbar_layout();
    g_engine->Stop();
    delete g_engine;
    
    delete g_renderer;

    WSACleanup();
    if (com_initialized) {
        CoUninitialize();
    }

    return static_cast<int>(msg.wParam);
}

void UpdateWidgetState(bool force_reembed) {
    if (!g_widget_hwnd || !g_renderer) return;

    static bool was_enabled = false;
    static bool cleanup_pending = false;
    const bool is_enabled = g_renderer->IsWidgetEnabled();
    TaskbarMetrics metrics{};
    const bool has_metrics = tm_get_taskbar_metrics(&metrics) != 0;
    const bool metrics_changed = has_metrics && ApplyWidgetTaskbarMetrics(g_widget_hwnd, metrics);

    auto hide_widget = []() {
        // Hiding before restoring the top-level style prevents a visible
        // reparent at (0, 0) when the user changes the tray preference.
        if (IsWindowVisible(g_widget_hwnd)) {
            ShowWindow(g_widget_hwnd, SW_HIDE);
        }
        cleanup_pending = tm_restore_taskbar_layout() == 0;
    };

    if (!is_enabled) {
        if (was_enabled || cleanup_pending || IsWindowVisible(g_widget_hwnd)) {
            hide_widget();
        }
        was_enabled = false;
        return;
    }

    // Failed cleanup must finish before another embedding attempt, even if
    // the widget was hidden or the user re-enabled it in the meantime.
    if (cleanup_pending) {
        hide_widget();
        if (cleanup_pending) {
            was_enabled = true;
            return;
        }
    }

    if (!has_metrics) {
        hide_widget();
        was_enabled = true;
        return;
    }

    int visible = 1;
    const int visibility_result = tm_should_widget_be_visible(g_widget_hwnd, g_main_hwnd, &visible);
    if (visibility_result != 0 && visible == 0) {
        hide_widget();
        was_enabled = true;
        return;
    }

    // The widget remains hidden until the native helper has made it a taskbar
    // child. This is the piece that stops the show/hide flash.
    TaskbarPlacement placement{};
    const auto size = GetWidgetPreferredSize(metrics);
    const bool embedded = tm_embed_widget(g_widget_hwnd, size.width, size.height, &placement) != 0;
    // No top-level fallback: an uncertified gap must not cover native controls
    // or turn the attached widget into a floating overlay. The timer retries.
    if (!embedded) {
        hide_widget();
        was_enabled = true;
        return;
    }

    if (!IsWindowVisible(g_widget_hwnd)) {
        InvalidateRect(g_widget_hwnd, nullptr, FALSE);
        ShowWindow(g_widget_hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_widget_hwnd);
    } else if (force_reembed || metrics_changed) {
        InvalidateRect(g_widget_hwnd, nullptr, FALSE);
    }

    was_enabled = true;
}

void UpdateFloatingMonitorState(bool reset_position) {
    if (!g_floating_monitor_hwnd || !g_renderer) {
        return;
    }

    const int width = GetFloatingMonitorPreferredWidth();
    const int height = GetFloatingMonitorPreferredHeight();
    const int saved_x = g_renderer->GetFloatingMonitorX();
    const int saved_y = g_renderer->GetFloatingMonitorY();
    const POINT position = reset_position || saved_x == INT_MIN || saved_y == INT_MIN
        ? GetFloatingMonitorDefaultPosition(width, height)
        : BoundFloatingMonitorPosition(POINT{saved_x, saved_y}, width, height);
    if (position.x != saved_x || position.y != saved_y) {
        g_renderer->SetFloatingMonitorPosition(position.x, position.y);
        g_renderer->SaveSettings();
    }

    if (!g_renderer->IsFloatingMonitorEnabled()) {
        if (IsWindowVisible(g_floating_monitor_hwnd)) {
            ShowWindow(g_floating_monitor_hwnd, SW_HIDE);
        }
        return;
    }

    const LONG_PTR current_style = GetWindowLongPtrW(g_floating_monitor_hwnd, GWL_EXSTYLE);
    LONG_PTR ex_style = current_style | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
    if (g_renderer->IsFloatingMonitorClickThrough()) {
        ex_style |= WS_EX_TRANSPARENT;
    } else {
        ex_style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    }
    const bool style_changed = current_style != ex_style;
    if (style_changed) {
        SetWindowLongPtrW(g_floating_monitor_hwnd, GWL_EXSTYLE, ex_style);
    }

    RECT current_rect{};
    const bool bounds_changed = !GetWindowRect(g_floating_monitor_hwnd, &current_rect) ||
        current_rect.left != position.x || current_rect.top != position.y ||
        current_rect.right - current_rect.left != width || current_rect.bottom - current_rect.top != height;
    const bool topmost_changed = ((current_style & WS_EX_TOPMOST) != 0) !=
        g_renderer->IsFloatingMonitorAlwaysOnTop();
    if (bounds_changed || style_changed || topmost_changed || !IsWindowVisible(g_floating_monitor_hwnd)) {
        SetWindowPos(
            g_floating_monitor_hwnd,
            g_renderer->IsFloatingMonitorAlwaysOnTop() ? HWND_TOPMOST : HWND_NOTOPMOST,
            position.x, position.y, width, height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | (style_changed ? SWP_FRAMECHANGED : 0)
        );
    }
    // The renderer owns per-pixel alpha and overall opacity through UpdateLayeredWindow.
    InvalidateRect(g_floating_monitor_hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool previous_press_was_welcome = false;
    if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED) {
        if (!AddTrayIcon(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        UpdateWidgetState(true);
        return 0;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        // Preserve WS_THICKFRAME for native resize, snap, and shadow behavior,
        // but let Watchman's client surface occupy that otherwise invisible frame.
        return 0;

    case WM_NCHITTEST: {
        // Let DWM handle anything it owns first. The rest of the title area is
        // client-drawn, so we provide resize edges and a native drag region.
        LRESULT dwm_result = 0;
        if (DwmDefWindowProc(hwnd, message, wParam, lParam, &dwm_result)) {
            return dwm_result;
        }
        return HitTestMainWindowFrame(hwnd, lParam);
    }

    case WM_KEYDOWN:
        if (g_renderer && g_renderer->IsWelcomeTourOpen()) {
            // Holding Enter/right arrow must not skip through several pages.
            if ((static_cast<ULONG_PTR>(lParam) & (1ULL << 30)) == 0) {
                g_renderer->HandleWelcomeTourKeyDown(wParam, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_renderer && wParam == VK_F1) {
            g_renderer->OpenWelcomeTour();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_renderer && g_renderer->HandleSettingsNumericKeyDown(wParam)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_CHAR:
        if (g_renderer && g_renderer->IsWelcomeTourOpen()) return 0;
        if (g_renderer && g_renderer->HandleSettingsNumericCharacter(static_cast<wchar_t>(wParam))) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == 1) {
            // Keep the active main surface responsive for native UI animations;
            // the taskbar widget still updates independently at 1 Hz.
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        else if (wParam == 2) {
            // Metrics refresh timer is now handled by WatchmanEngine in the background!
            // Publish exactly one sample for both the main dashboard and the
            // taskbar widget, then let their paint messages consume that copy.
            RefreshDisplayDashboardSnapshot();
            EvaluateWarningNotifications(hwnd);
            if (g_floating_monitor_hwnd && IsWindowVisible(g_floating_monitor_hwnd)) {
                InvalidateRect(g_floating_monitor_hwnd, nullptr, FALSE);
            }

            // We just need to trigger a Wi-Fi refresh command if needed.
            const bool is_network_tab_active =
                IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
                (g_renderer->GetActiveTab() == ActiveTab::Network);
            static bool was_network_tab_active = false;
            static ULONGLONG last_wifi_refresh = 0;
            ULONGLONG now = GetTickCount64();

            if (is_network_tab_active != was_network_tab_active) {
                g_engine->SetNetworkHealthActive(is_network_tab_active);
            }

            if (is_network_tab_active) {
                if (!was_network_tab_active || (now - last_wifi_refresh > 1200000)) {
                    g_engine->RefreshNetworkDetails();
                    last_wifi_refresh = now;
                }
            }
            was_network_tab_active = is_network_tab_active;

            if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g_widget_hwnd && g_renderer->IsWidgetEnabled()) {
                InvalidateRect(g_widget_hwnd, nullptr, FALSE);
            }
        }
        else if (wParam == 3) {
            UpdateWidgetState(false);

            if (tm_take_widget_context_menu_request()) {
                PostMessageW(hwnd, WM_WATCHMAN_WIDGET_MENU, 0, 0);
            }
            if (tm_take_widget_double_click_request()) {
                PostMessageW(hwnd, WM_WATCHMAN_WIDGET_DATA, 0, 0);
            }
        }
        break;
    case WM_SHOWWINDOW:
        // WM_SHOWWINDOW precedes the visibility change, so use the requested state.
        UpdateMainUiActivity(hwnd, wParam != FALSE, IsIconic(hwnd) != FALSE, GetForegroundWindow() == hwnd);
        if (wParam && g_welcome_presentation_ready && g_renderer) g_renderer->OfferWelcomeTour(true);
        break;
    case WM_ACTIVATE:
        UpdateMainUiActivity(hwnd, IsWindowVisible(hwnd) != FALSE, HIWORD(wParam) != 0,
            LOWORD(wParam) != WA_INACTIVE);
        if (LOWORD(wParam) != WA_INACTIVE) {
            if (g_welcome_presentation_ready && g_renderer) g_renderer->OfferWelcomeTour(true);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_CANCELMODE:
        EndMenu();
        break;
    case WM_WATCHMAN_WIDGET_MENU:
        ShowWidgetContextMenu(hwnd);
        break;
    case WM_WATCHMAN_WIDGET_DATA:
        ShowMainPage(ActiveTab::Data);
        break;
    case WM_WATCHMAN_ACTIVATE:
        if (g_main_hwnd) {
            if (g_renderer) g_renderer->OfferWelcomeTour(true);
            ShowWindow(g_main_hwnd, SW_RESTORE);
            SetForegroundWindow(g_main_hwnd);
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            
            if (g_renderer && g_engine && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                const ActiveTab tab = g_renderer->GetActiveTab();
                DashboardSnapshot dash = GetDisplayDashboardSnapshot();
                NetworkSnapshot net = g_engine->GetNetworkSnapshot();
                AppActivitySnapshot app;
                TrafficHistorySnapshot hist;
                SpeedTestResult speed_res;
                int speed_prog = 0;
                double speed_live = 0.0;
                std::wstring speed_stat;
                bool speed_running = false;
                DiagnosticsSnapshot diagnostics;
                if (tab == ActiveTab::Apps) app = g_engine->GetAppActivitySnapshot();
                if (tab == ActiveTab::Dashboard || tab == ActiveTab::Data || tab == ActiveTab::Tools) {
                    hist = g_engine->GetTrafficHistorySnapshot();
                }
                if (tab == ActiveTab::Network) {
                    speed_res = g_engine->GetSpeedTestResult();
                    speed_prog = g_engine->GetSpeedTestProgress();
                    speed_live = g_engine->GetSpeedTestLiveSpeed();
                    speed_stat = g_engine->GetSpeedTestStatus();
                    speed_running = g_engine->IsSpeedTestRunning();
                }
                if (tab == ActiveTab::Tools) diagnostics = g_engine->GetDiagnosticsSnapshot();

                g_renderer->Render(
                    hwnd,
                    dash,
                    net,
                    app,
                    hist,
                    speed_res,
                    speed_prog,
                    speed_live,
                    speed_stat,
                    speed_running,
                    diagnostics
                );
            }
            
            EndPaint(hwnd, &ps);
        }
        break;

    case WM_CTLCOLOREDIT:
        if (g_renderer && g_renderer->GetEffectiveTheme() == ThemeMode::Dark) {
            HDC edit_dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(edit_dc, RGB(255, 255, 255));
            SetBkColor(edit_dc, RGB(0, 0, 0));
            SetDCBrushColor(edit_dc, RGB(0, 0, 0));
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);

    case WM_GETMINMAXINFO: {
        auto* minmax = reinterpret_cast<MINMAXINFO*>(lParam);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        const bool have_work_area = GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
        minmax->ptMinTrackSize.x = have_work_area
            ? (std::min)(static_cast<LONG>(UIStyle::MIN_WINDOW_WIDTH), monitor.rcWork.right - monitor.rcWork.left)
            : static_cast<LONG>(UIStyle::MIN_WINDOW_WIDTH);
        minmax->ptMinTrackSize.y = have_work_area
            ? (std::min)(static_cast<LONG>(UIStyle::MIN_WINDOW_HEIGHT), monitor.rcWork.bottom - monitor.rcWork.top)
            : static_cast<LONG>(UIStyle::MIN_WINDOW_HEIGHT);
        return 0;
    }

    case WM_SIZE:
        UpdateMainUiActivity(hwnd, IsWindowVisible(hwnd) != FALSE, wParam == SIZE_MINIMIZED,
            GetForegroundWindow() == hwnd);
        if (wParam != SIZE_MINIMIZED && LOWORD(lParam) > 0 && HIWORD(lParam) > 0) {
            ApplyMainWindowCorners(hwnd, LOWORD(lParam), HIWORD(lParam));
            if (g_renderer) g_renderer->Resize(LOWORD(lParam), HIWORD(lParam));
        }
        break;

    case WM_MOUSEMOVE:
        if (g_renderer) {
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            g_renderer->OnMouseMove(x, y);
        }
        break;

    case WM_MOUSELEAVE:
        if (g_renderer) {
            g_renderer->OnMouseMove(-10000, -10000);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case WM_MOUSEWHEEL:
        if (g_renderer) {
            g_renderer->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (g_renderer) {
            // A double-click message replaces the second button-down. Do not
            // discard it for theme/+/- controls, but never let the second half
            // of a tour click reach a control exposed by dismissing the tour.
            if (message == WM_LBUTTONDBLCLK && (previous_press_was_welcome ||
                    !g_renderer->IsRepeatablePrimaryControl(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))) return 0;
            previous_press_was_welcome = g_renderer->IsWelcomeTourOpen();
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            bool widget_was_enabled = g_renderer->IsWidgetEnabled();
            const TaskbarWidgetMode previous_widget_mode = g_renderer->GetWidgetMode();
            bool autostart_was_enabled = g_renderer->IsAutostartEnabled();
            const std::wstring adapter_was_selected = g_renderer->GetNetworkAdapterId();
            const ThemeMode previous_theme = g_renderer->GetTheme();
            const bool welcome_was_open = g_renderer->IsWelcomeTourOpen();
            
            // Handle clicks and update theme/settings
            if (g_engine) {
                g_renderer->OnLButtonDown(hwnd, x, y, *g_engine);
            }

            if (g_renderer->TakeNetworkAdapterPickerRequest()) {
                ShowNetworkAdapterPicker(hwnd);
            }
            if (g_renderer->TakeFloatingMonitorDisplayPickerRequest()) {
                ShowFloatingMonitorDisplayPicker(hwnd);
            }
            ToolsExportPicker tools_export_picker = ToolsExportPicker::None;
            if (g_renderer->TakeToolsExportPickerRequest(tools_export_picker)) {
                ShowToolsExportPicker(hwnd, tools_export_picker);
            }

            const bool preferences_applied = g_renderer->TakePreferencesApplyRequest();
            if (previous_theme != g_renderer->GetTheme()) ApplyMainWindowTheme(hwnd);
            
            InvalidateRect(hwnd, nullptr, FALSE);
            // Draft-only clicks need repaint, not dozens of unchanged Registry writes.
            if (!welcome_was_open && (!g_renderer->IsSettingsOpen() || preferences_applied)) g_renderer->SaveSettings();
            
            if (autostart_was_enabled != g_renderer->IsAutostartEnabled()) {
                const LONG result = SetStartupRegistry(g_renderer->IsAutostartEnabled());
                if (result != ERROR_SUCCESS) {
                    g_renderer->SetAutostartEnabled(autostart_was_enabled);
                    g_renderer->SaveSettings();
                    const std::wstring error_text = L"Windows startup could not be changed (error "
                        + std::to_wstring(result) + L"). The previous setting has been restored.";
                    MessageBoxW(hwnd, error_text.c_str(), L"Watchman startup", MB_OK | MB_ICONWARNING);
                }
            }

            bool widget_changed =
                widget_was_enabled != g_renderer->IsWidgetEnabled() ||
                previous_widget_mode != g_renderer->GetWidgetMode();
            if (widget_changed) {
                UpdateWidgetState(true);
            }

            if (preferences_applied) {
                const std::wstring adapter_now_selected = g_renderer->GetNetworkAdapterId();
                if (_wcsicmp(adapter_was_selected.c_str(), adapter_now_selected.c_str()) != 0) {
                    g_engine->SetNetworkAdapterId(adapter_now_selected);
                    g_engine->RefreshNetworkDetails();
                }
                UpdateFloatingMonitorState(g_renderer->TakeFloatingMonitorResetPositionRequest());
                if (g_widget_hwnd) {
                    InvalidateRect(g_widget_hwnd, nullptr, FALSE);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        break;

    case WM_WATCHMAN_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ShowWindow(g_main_hwnd, SW_RESTORE);
            SetForegroundWindow(g_main_hwnd);
        } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        }
        break;

    case WM_CLOSE: {
        const bool tray_ready = AddTrayIcon(hwnd);
        WatchmanWindowLifecycle::HandleMessage(hwnd, message, tray_ready);
        if (!tray_ready) {
            MessageBoxW(hwnd,
                L"Watchman could not create its system tray icon, so the window has been kept open. "
                L"Monitoring is still running. Try closing the window again after the tray is available.",
                L"Watchman system tray", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }

    case WatchmanWindowLifecycle::ExitMessage:
        WatchmanWindowLifecycle::HandleMessage(hwnd, message, true);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_main_animation_timer_running = false;
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 3);
        tm_set_widget_command_target(nullptr, 0, 0);
        RemoveTrayIcon(hwnd);
        if (g_widget_hwnd) {
            DestroyWindow(g_widget_hwnd);
        }
        if (g_floating_monitor_hwnd) {
            DestroyWindow(g_floating_monitor_hwnd);
        }
        PostQuitMessage(0);
        break;

    case WM_SETTINGCHANGE:
        if (g_renderer && g_renderer->GetTheme() == ThemeMode::System) {
            g_renderer->SetTheme(ThemeMode::System);
            ApplyMainWindowTheme(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        UpdateWidgetState(true);
        break;

    case WM_DPICHANGED:
        UpdateWidgetState(true);
        break;

    case WM_DISPLAYCHANGE:
        UpdateWidgetState(true);
        UpdateFloatingMonitorState(false);
        break;

    default:
        // This class is registered/created with the W APIs. Using the ANSI
        // default procedure here truncates its native caption at the first
        // UTF-16 NUL ("WATCHMAN" becomes "W" on non-UNICODE builds).
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK WidgetWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (g_renderer) {
                const DashboardSnapshot snapshot = GetDisplayDashboardSnapshot();
                g_renderer->RenderWidget(hwnd, snapshot);
            }
            EndPaint(hwnd, &ps);
        }
        break;

    case WM_LBUTTONDBLCLK:
        ShowMainPage(ActiveTab::Data);
        break;

    case WM_CONTEXTMENU:
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_WATCHMAN_WIDGET_MENU, 0, 0);
        }
        break;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK FloatingMonitorWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool drag_pending = false;
    static POINT drag_origin{};
    switch (message) {
    case WM_NCHITTEST:
        if (g_renderer && g_renderer->IsFloatingMonitorClickThrough()) {
            return HTTRANSPARENT;
        }
        return HTCLIENT;

    case WM_LBUTTONDOWN:
        if (g_renderer && !g_renderer->IsFloatingMonitorPositionLocked() &&
            !g_renderer->IsFloatingMonitorClickThrough()) {
            drag_origin = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &drag_origin);
            drag_pending = true;
            SetCapture(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (drag_pending && GetCapture() == hwnd && (wParam & MK_LBUTTON)) {
            POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &position);
            if (WatchmanRuntime::ExceedsDragThreshold(position.x - drag_origin.x, position.y - drag_origin.y,
                GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG))) {
                drag_pending = false;
                ReleaseCapture();
                // Start native moving only after a real drag, leaving clicks eligible for double-clicks.
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(position.x, position.y));
            }
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_CANCELMODE:
        drag_pending = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;

    case WM_CAPTURECHANGED:
        drag_pending = false;
        return 0;

    case WM_LBUTTONDBLCLK:
        drag_pending = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (g_renderer && !g_renderer->IsFloatingMonitorClickThrough()) ShowMainPage(ActiveTab::Data);
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(hwnd, &paint);
        if (g_renderer) {
            g_renderer->RenderFloatingMonitor(hwnd, hdc, GetDisplayDashboardSnapshot());
        }
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_CONTEXTMENU: {
        POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (position.x == -1 && position.y == -1) {
            RECT rect{};
            if (GetWindowRect(hwnd, &rect)) {
                position = POINT{rect.left, rect.bottom};
            } else {
                GetCursorPos(&position);
            }
        }
        ShowFloatingMonitorContextMenu(hwnd, position);
        return 0;
    }

    case WM_EXITSIZEMOVE:
        if (g_renderer && !g_renderer->IsFloatingMonitorPositionLocked() &&
            !g_renderer->IsFloatingMonitorClickThrough()) {
            RECT rect{};
            if (GetWindowRect(hwnd, &rect) &&
                (rect.left != g_renderer->GetFloatingMonitorX() || rect.top != g_renderer->GetFloatingMonitorY())) {
                g_renderer->SetFloatingMonitorPosition(rect.left, rect.top);
                g_renderer->SaveSettings();
                UpdateFloatingMonitorState(false);
                InvalidateRect(g_main_hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_DESTROY:
        if (hwnd == g_floating_monitor_hwnd) {
            g_floating_monitor_hwnd = nullptr;
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
