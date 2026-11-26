#include <winsock2.h>
#include <ws2tcpip.h>
#include "renderer.h"
#include "startup_diagnostics.h"
#include <d2d1helper.h>
#include <wincodec.h>
#include "speed_tester.h"
#include <commdlg.h>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <shellapi.h>
#include <algorithm>
#include <ctime>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <limits>
#include <utility>



#include "engine.h"
#include "taskbar_embed.h"
#include "widget_compositor.h"
#include "resource.h"
#include "app_version.h"
#include "update.h"
#include "update_preferences.h"
#include "settings_header_layout.h"
#include "floating_monitor_settings_layout.h"
#include "floating_monitor_layout.h"
#include "taskbar_widget_layout.h"
#include "light_theme_reference.h"
#include "main_content_layout.h"
#include "theme_picker_layout.h"
#include "app_traffic_presentation.h"
#include "warnings_settings_layout.h"
#include "speed_test_presentation.h"
#include "speed_test_view.h"
#include "window_lifecycle.h"
#include "welcome_tour_preferences.h"
#include "scoped_handle.h"

namespace {

bool ReadWindowsLightTaskbarTheme();

std::array<COLORREF, FloatingMonitorSettingsLayout::color_count> GetFloatingMonitorDefaultColors() {
    constexpr COLORREF white = FloatingMonitorLayout::default_text_color;
    return { white, white, white, white, white };
}

constexpr const wchar_t* kShortMonthNames[] = {
    L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
    L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec",
};

constexpr const wchar_t* kLongMonthNames[] = {
    L"January", L"February", L"March", L"April", L"May", L"June",
    L"July", L"August", L"September", L"October", L"November", L"December",
};

std::wstring EncodePowerShellCommand(const std::wstring& script) {
    static constexpr wchar_t kBase64[] =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const auto* bytes = reinterpret_cast<const unsigned char*>(script.data());
    const size_t byte_count = script.size() * sizeof(wchar_t);
    std::wstring encoded;
    encoded.reserve(((byte_count + 2) / 3) * 4);

    for (size_t offset = 0; offset < byte_count; offset += 3) {
        const size_t remaining = byte_count - offset;
        const unsigned int first = bytes[offset];
        const unsigned int second = remaining > 1 ? bytes[offset + 1] : 0;
        const unsigned int third = remaining > 2 ? bytes[offset + 2] : 0;
        const unsigned int block = (first << 16) | (second << 8) | third;
        encoded.push_back(kBase64[(block >> 18) & 0x3f]);
        encoded.push_back(kBase64[(block >> 12) & 0x3f]);
        encoded.push_back(remaining > 1 ? kBase64[(block >> 6) & 0x3f] : L'=');
        encoded.push_back(remaining > 2 ? kBase64[block & 0x3f] : L'=');
    }
    return encoded;
}

bool LaunchHiddenPowerShell(const std::wstring& script) {
    std::wstring command_line =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand " +
        EncodePowerShellCommand(script);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup_info,
            &process_info)) {
        return false;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}

void OpenWindowsDataUsageSettings(HWND hwnd) {
    std::wstring script = LR"PS(
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$nativeCode = @'
using System;
using System.Runtime.InteropServices;
public static class WatchmanSettingsWindow {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
'@
Add-Type -TypeDefinition $nativeCode

function Open-WatchmanSettingsUri($uri) {
  Start-Process $uri
  Start-Sleep -Milliseconds 1200
}

function Get-WatchmanSettingsRoot {
  # Modern Windows can leave ApplicationFrameHost with an empty main-window
  # title.  Locate the visible automation window instead of trusting that
  # title, then make sure its owning process is the Settings app.
  $desktop = [System.Windows.Automation.AutomationElement]::RootElement
  $windows = $desktop.FindAll(
    [System.Windows.Automation.TreeScope]::Children,
    [System.Windows.Automation.Condition]::TrueCondition
  )
  foreach ($window in $windows) {
    try {
      $process = Get-Process -Id $window.Current.ProcessId -ErrorAction Stop
      $windowName = [string]$window.Current.Name
      $isSettings = $process.ProcessName -eq 'SystemSettings' -or
        ($process.ProcessName -eq 'ApplicationFrameHost' -and (
          $windowName -like '*Settings*' -or $windowName -like '*Network*'
        ))
      if (-not $isSettings) { continue }
      $handle = [intptr]$window.Current.NativeWindowHandle
      if ($handle -ne [intptr]::Zero) {
        [WatchmanSettingsWindow]::ShowWindow($handle, 9) | Out-Null
        [WatchmanSettingsWindow]::SetForegroundWindow($handle) | Out-Null
      }
      return $window
    } catch {}
  }

  $process = Get-Process -Name SystemSettings,ApplicationFrameHost -ErrorAction SilentlyContinue |
    Where-Object {
      $_.MainWindowHandle -ne 0 -and
      ($_.ProcessName -eq 'SystemSettings' -or
        $_.MainWindowTitle -like '*Settings*' -or $_.MainWindowTitle -like '*Network*')
    } |
    Sort-Object StartTime -Descending |
    Select-Object -First 1
  if (-not $process) { return $null }
  [WatchmanSettingsWindow]::ShowWindow([intptr]$process.MainWindowHandle, 9) | Out-Null
  [WatchmanSettingsWindow]::SetForegroundWindow([intptr]$process.MainWindowHandle) | Out-Null
  return [System.Windows.Automation.AutomationElement]::FromHandle([intptr]$process.MainWindowHandle)
}

function Invoke-WatchmanElement($element) {
  if (-not $element) { return $false }
  $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
  $candidate = $element
  for ($i = 0; $i -lt 8 -and $candidate; $i++) {
    try {
      $scroll = $candidate.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern)
      if ($scroll) { $scroll.ScrollIntoView(); Start-Sleep -Milliseconds 120 }
    } catch {}
    try {
      $invoke = $candidate.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
      if ($invoke) { $invoke.Invoke(); Start-Sleep -Milliseconds 700; return $true }
    } catch {}
    try {
      $selection = $candidate.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
      if ($selection) { $selection.Select(); Start-Sleep -Milliseconds 700; return $true }
    } catch {}
    try {
      $legacy = $candidate.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
      if ($legacy) { $legacy.DoDefaultAction(); Start-Sleep -Milliseconds 700; return $true }
    } catch {}
    $candidate = $walker.GetParent($candidate)
  }
  return $false
}

function Find-WatchmanDataUsage($root) {
  if (-not $root) { return $null }
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::NameProperty,
    'Data usage'
  )
  return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Try-OpenWatchmanDataUsage {
  for ($i = 0; $i -lt 8; $i++) {
    $root = Get-WatchmanSettingsRoot
    if ($root) {
      $target = Find-WatchmanDataUsage $root
      if (Invoke-WatchmanElement $target) {
        return $true
      }
    }
    Start-Sleep -Milliseconds 250
  }
  return $false
}

# Windows' Network & internet home page exposes Data usage for the active
# connection.  Invoking that card lets Settings choose Wi-Fi or Ethernet
# itself, without Watchman moving focus through the adapter controls.
Open-WatchmanSettingsUri 'ms-settings:network'
if (Try-OpenWatchmanDataUsage) { exit 0 }

Open-WatchmanSettingsUri 'ms-settings:network-advancedsettings'
if (Try-OpenWatchmanDataUsage) { exit 0 }

exit 1
)PS";

    if (!LaunchHiddenPowerShell(script)) {
        ShellExecuteW(hwnd, L"open", L"ms-settings:network", nullptr, nullptr, SW_SHOWNORMAL);
    }
}

struct DataChartBucket {
    std::wstring key;
    std::wstring label;
    uint64_t download = 0;
    uint64_t upload = 0;
};

struct DataTabLayout {
    float start_x = 16.0f;
    float start_y = UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT + 18.0f;
    float content_w = 0.0f;
    float usage_filter_y = 0.0f;
    float usage_summary_y = 0.0f;
    float usage_summary_h = 64.0f;
    float outer_y = 0.0f;
    float outer_bottom = 0.0f;
    float filter_y = 0.0f;
    float summary_y = 0.0f;
    float summary_h = 68.0f;
    float range_y = 0.0f;
    float chart_y = 0.0f;
    float chart_h = 0.0f;
    float comparison_y = 0.0f;
    float comparison_h = 78.0f;
    float limit_y = 0.0f;
    float limit_h = 104.0f;
};

DataTabLayout GetDataTabLayout(float render_width, float /*render_height*/) {
    DataTabLayout layout;
    layout.content_w = std::max(0.0f, render_width - layout.start_x * 2.0f);
    layout.usage_filter_y = layout.start_y;
    layout.usage_summary_y = layout.usage_filter_y + 52.0f;
    layout.outer_y = layout.usage_summary_y + layout.usage_summary_h + 16.0f;
    layout.filter_y = layout.outer_y + 20.0f;
    layout.summary_y = layout.filter_y + 56.0f;
    layout.range_y = layout.summary_y + layout.summary_h + 16.0f;
    layout.chart_y = layout.range_y + 60.0f;
    layout.chart_h = 248.0f;
    layout.outer_bottom = layout.chart_y + layout.chart_h + 20.0f;
    layout.comparison_y = layout.outer_bottom + 16.0f;
    layout.limit_y = layout.comparison_y + layout.comparison_h + 16.0f;
    return layout;
}

D2D1_RECT_F GetDataPickerRect(const DataTabLayout& layout, bool monthly, size_t available_year_count) {
    const float picker_width = std::min(292.0f, layout.content_w - 20.0f);
    const float picker_x = layout.start_x + 10.0f;
    const size_t visible_years = std::min<size_t>(12, std::max<size_t>(1, available_year_count));
    const size_t year_rows = (visible_years + 2) / 3;

    // These dimensions mirror Watchman-main's 14px popover padding, 28px
    // navigation buttons, 42px year cells, and 34px month pills.
    const float picker_height = monthly
        ? 254.0f
        : 310.0f + static_cast<float>(year_rows - 1) * 50.0f;
    return D2D1::RectF(
        picker_x,
        layout.range_y + 48.0f,
        picker_x + picker_width,
        layout.range_y + 48.0f + picker_height
    );
}

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
    static constexpr int kDaysPerMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) {
        return 0;
    }
    return month == 2 && IsLeapYear(year) ? 29 : kDaysPerMonth[month - 1];
}

bool ParseIsoDate(const std::wstring& value, int* year, int* month, int* day = nullptr) {
    int parsed_year = 0;
    int parsed_month = 0;
    int parsed_day = 0;
    const int expected = day != nullptr ? 3 : 2;
    const int parsed = day != nullptr
        ? swscanf_s(value.c_str(), L"%d-%d-%d", &parsed_year, &parsed_month, &parsed_day)
        : swscanf_s(value.c_str(), L"%d-%d", &parsed_year, &parsed_month);
    if (parsed != expected) {
        return false;
    }
    if (year != nullptr) *year = parsed_year;
    if (month != nullptr) *month = parsed_month;
    if (day != nullptr) *day = parsed_day;
    return true;
}

std::wstring MakeDateKey(int year, int month, int day) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%04d-%02d-%02d", year, month, day);
    return buffer;
}

std::wstring MakeMonthKey(int year, int month) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%04d-%02d", year, month);
    return buffer;
}

std::wstring DateDaysAgo(int days_ago) {
    time_t now = time(nullptr);
    tm local{};
    localtime_s(&local, &now);
    local.tm_mday -= days_ago;
    mktime(&local);
    return MakeDateKey(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

struct DashboardLayout {
    float start_x = 16.0f;
    float start_y = UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT + 16.0f;
    float content_w = 0.0f;
    float gap = 16.0f;
    float speed_h = 70.0f;
    float session_y = 0.0f;
    float session_h = 132.0f;
    float snapshot_y = 0.0f;
    float snapshot_h = 140.0f;
    float status_y = 0.0f;
    float status_h = 60.0f;
    float gauges_y = 0.0f;
    float gauges_h = 134.0f;
    float network_y = 0.0f;
    float network_h = 56.0f;
    float system_y = 0.0f;
    float system_h = 56.0f;
    float bottom = 0.0f;
};

using TitlebarLayout = MainContentLayout::Titlebar;
using MainContentLayout::GetTitlebar;

TitlebarLayout GetTitlebarLayout(float render_width) {
    return GetTitlebar(render_width);
}

bool IsPointInside(const D2D1_RECT_F& rect, int x, int y) {
    return static_cast<float>(x) >= rect.left && static_cast<float>(x) <= rect.right &&
        static_cast<float>(y) >= rect.top && static_cast<float>(y) <= rect.bottom;
}

int GetTitlebarButtonAt(float render_width, int x, int y) {
    return MainContentLayout::TitlebarButtonAt(render_width, static_cast<float>(x), static_cast<float>(y));
}

using TabbarItemLayout = MainContentLayout::TabItem;
using TabbarLayout = MainContentLayout::Tabbar;

struct SettingsModalLayout {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float navigation_width = 145.0f;
};

SettingsModalLayout GetSettingsModalLayout(D2D1_SIZE_F render_size) {
    SettingsModalLayout layout;
    layout.width = std::min(500.0f, std::max(360.0f, render_size.width - 20.0f));
    layout.height = std::min(740.0f, std::max(460.0f, render_size.height - 20.0f));
    layout.navigation_width = 124.0f;
    layout.x = (render_size.width - layout.width) * 0.5f;
    layout.y = (render_size.height - layout.height) * 0.5f;
    return layout;
}

// The Widgets page deliberately uses readable rows and scrolls within the
// fixed settings dialog. This avoids compressing the controls into a height
// that only works at one window size.
struct WidgetsSettingsLayout {
    float card_left = 0.0f;
    float card_right = 0.0f;
    float pad = 14.0f;
    float icon_size = 32.0f;
    float icon_glyph = 18.0f;
    float fl_icon_size = 30.0f;
    float fl_icon_glyph = 18.0f;
    float icon_gap = 12.0f;
    float toggle_w = 40.0f;
    float toggle_h = 22.0f;
    float thumb_r = 7.0f;
    float toggle_inset_right = 16.0f;
    float row_h = 48.0f;
    float fl_row_h = 40.0f;
    float row_gap = 0.0f;
    float card_gap = 12.0f;
    float title_top = 14.0f;
    float title_h = 19.0f;
    float title_to_row = 8.0f;
    float bottom_pad = 14.0f;

    float tb_top = 0.0f;
    float tb_title = 0.0f;
    float tb_r1 = 0.0f;
    float tb_d1 = 0.0f;
    float tb_content_title = 0.0f;
    float tb_mode_row[3] = {};
    float tb_mode_row_h = 34.0f;
    float tb_bot = 0.0f;

    float fl_top = 0.0f;
    float fl_title = 0.0f;
    float fl_row[3] = {};
    float fl_div[2] = {};
    float fl_details_top = 0.0f;
    float fl_details_bot = 0.0f;
    float fl_bot = 0.0f;
};

WidgetsSettingsLayout GetWidgetsSettingsLayout(float content_left, float content_right, float my) {
    WidgetsSettingsLayout w;
    w.card_left = content_left;
    w.card_right = content_right;

    // ── Taskbar Widget ──────────────────────────────────────────────
    w.tb_top = my + 148.0f;
    w.tb_title = w.tb_top + w.title_top;
    w.tb_r1 = w.tb_title + w.title_h + w.title_to_row;
    w.tb_d1 = w.tb_r1 + w.row_h;
    w.tb_content_title = w.tb_d1 + 12.0f;
    for (int i = 0; i < 3; ++i) {
        w.tb_mode_row[i] = w.tb_content_title + w.title_h + 6.0f + i * w.tb_mode_row_h;
    }
    w.tb_bot = w.tb_mode_row[2] + w.tb_mode_row_h + w.bottom_pad;

    // ── Floating Mini Monitor ───────────────────────────────────────
    w.fl_top = w.tb_bot + w.card_gap;
    w.fl_title = w.fl_top + w.title_top;
    float row_y = w.fl_title + w.title_h + w.title_to_row;
    for (int i = 0; i < 3; ++i) {
        w.fl_row[i] = row_y;
        if (i < 2) {
            w.fl_div[i] = row_y + w.fl_row_h;
            row_y = w.fl_div[i] + w.row_gap;
        }
    }
    w.fl_details_top = w.fl_row[2] + w.fl_row_h + 12.0f;
    w.fl_details_bot = w.fl_details_top + 34.0f;
    w.fl_bot = w.fl_details_bot + w.bottom_pad;
    return w;
}


TabbarLayout GetTabbarLayout(float render_width) {
    return MainContentLayout::GetTabbar(render_width);
}

int GetTabbarItemAt(float render_width, int x) {
    const TabbarLayout layout = GetTabbarLayout(render_width);
    for (int index = 0; index < 5; ++index) {
        const TabbarItemLayout& item = layout.items[index];
        if (static_cast<float>(x) >= item.left && static_cast<float>(x) <= item.left + item.width) {
            return index;
        }
    }
    return -1;
}

// The Tauri tab bar uses these five inline 24x24 SVGs.  Keep the same paths
// here instead of substituting unrelated Fluent glyphs.
void DrawTauriTabIcon(
    ID2D1RenderTarget* target,
    int tab_index,
    float left,
    float top,
    ID2D1Brush* brush
) {
    constexpr float kIconSize = 14.0f;
    constexpr float kSvgSize = 24.0f;
    constexpr float kStrokeWidth = 1.0f;
    const float scale = kIconSize / kSvgSize;
    const auto point = [left, top, scale](float x, float y) {
        return D2D1::Point2F(left + x * scale, top + y * scale);
    };
    const auto rect = [&point](float left_x, float top_y, float right_x, float bottom_y) {
        const D2D1_POINT_2F a = point(left_x, top_y);
        const D2D1_POINT_2F b = point(right_x, bottom_y);
        return D2D1::RectF(a.x, a.y, b.x, b.y);
    };
    const auto draw_path = [target, brush](const std::vector<D2D1_POINT_2F>& points) {
        for (size_t index = 1; index < points.size(); ++index) {
            target->DrawLine(points[index - 1], points[index], brush, kStrokeWidth);
        }
    };

    switch (tab_index) {
    case 0: // Dashboard: source SVG grid
        target->DrawRectangle(rect(3.0f, 3.0f, 10.0f, 12.0f), brush, kStrokeWidth);
        target->DrawRectangle(rect(14.0f, 3.0f, 21.0f, 8.0f), brush, kStrokeWidth);
        target->DrawRectangle(rect(14.0f, 12.0f, 21.0f, 21.0f), brush, kStrokeWidth);
        target->DrawRectangle(rect(3.0f, 16.0f, 10.0f, 21.0f), brush, kStrokeWidth);
        break;
    case 1: // Data: source SVG bars
        target->DrawLine(point(12.0f, 20.0f), point(12.0f, 10.0f), brush, kStrokeWidth);
        target->DrawLine(point(6.0f, 20.0f), point(6.0f, 4.0f), brush, kStrokeWidth);
        target->DrawLine(point(18.0f, 20.0f), point(18.0f, 16.0f), brush, kStrokeWidth);
        break;
    case 2: // Apps: source SVG monitor
        target->DrawRoundedRectangle(
            D2D1::RoundedRect(rect(2.0f, 3.0f, 22.0f, 17.0f), 1.2f, 1.2f),
            brush,
            kStrokeWidth
        );
        target->DrawLine(point(8.0f, 21.0f), point(16.0f, 21.0f), brush, kStrokeWidth);
        target->DrawLine(point(12.0f, 17.0f), point(12.0f, 21.0f), brush, kStrokeWidth);
        break;
    case 3: // Network: source SVG Wi-Fi arcs, approximated as compact polylines
        draw_path({ point(1.42f, 9.0f), point(5.5f, 6.7f), point(12.0f, 5.9f), point(18.5f, 6.7f), point(22.58f, 9.0f) });
        draw_path({ point(5.0f, 12.55f), point(8.6f, 10.8f), point(12.0f, 10.3f), point(15.4f, 10.8f), point(19.08f, 12.55f) });
        draw_path({ point(8.53f, 16.11f), point(10.25f, 15.3f), point(12.0f, 15.1f), point(13.75f, 15.3f), point(15.48f, 16.11f) });
        target->FillEllipse(D2D1::Ellipse(point(12.0f, 20.0f), 0.9f, 0.9f), brush);
        break;
    case 4: { // Tools: exact curved outline from the Tauri wrench SVG.
        // The earlier polyline joined the SVG arc endpoints with straight
        // diagonals, which reduced the wrench to something pencil-like.
        ID2D1Factory* factory = nullptr;
        target->GetFactory(&factory);
        ID2D1PathGeometry* geometry = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) ||
            FAILED(geometry->Open(&sink))) {
            if (sink) sink->Release();
            if (geometry) geometry->Release();
            if (factory) factory->Release();
            break;
        }

        const auto add_arc = [&sink, &point, scale](
            float end_x,
            float end_y,
            float radius,
            D2D1_SWEEP_DIRECTION sweep
        ) {
            D2D1_ARC_SEGMENT segment{};
            segment.point = point(end_x, end_y);
            segment.size = D2D1::SizeF(radius * scale, radius * scale);
            segment.rotationAngle = 0.0f;
            segment.sweepDirection = sweep;
            segment.arcSize = D2D1_ARC_SIZE_SMALL;
            sink->AddArc(segment);
        };

        sink->BeginFigure(point(14.7f, 6.3f), D2D1_FIGURE_BEGIN_HOLLOW);
        add_arc(14.7f, 7.7f, 1.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
        sink->AddLine(point(16.3f, 9.3f));
        add_arc(17.7f, 9.3f, 1.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE);
        sink->AddLine(point(21.47f, 5.53f));
        add_arc(13.53f, 13.47f, 6.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        sink->AddLine(point(6.62f, 20.38f));
        add_arc(3.62f, 17.38f, 2.12f, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        sink->AddLine(point(10.53f, 10.47f));
        add_arc(18.47f, 2.53f, 6.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        sink->AddLine(point(14.7f, 6.3f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

        if (SUCCEEDED(sink->Close())) {
            target->DrawGeometry(geometry, brush, kStrokeWidth);
        }
        sink->Release();
        geometry->Release();
        factory->Release();
        break;
    }
    default:
        break;
    }
}

// The Tauri app draws these controls from inline SVG paths.  Draw the native
// equivalents with Direct2D instead of relying on optional Windows icon fonts:
// missing Fluent glyphs otherwise show as empty square boxes on clean PCs.
enum class VectorIcon {
    Globe,
    Gear,
    Sun,
    Moon,
    Download,
    Upload,
    Calendar,
    Bars,
    Wifi,
    Close,
    ChevronDown,
    Undo,
    Save,
};

void DrawVectorIcon(
    ID2D1RenderTarget* target,
    VectorIcon icon,
    const D2D1_RECT_F& bounds,
    ID2D1Brush* brush,
    float stroke_width = 1.35f
) {
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    const float unit = std::max(1.0f, std::min(width, height));
    const float cx = (bounds.left + bounds.right) * 0.5f;
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    const auto p = [cx, cy, unit](float x, float y) {
        return D2D1::Point2F(cx + x * unit, cy + y * unit);
    };
    const auto r = [cx, cy, unit](float left, float top, float right, float bottom) {
        return D2D1::RectF(cx + left * unit, cy + top * unit, cx + right * unit, cy + bottom * unit);
    };
    const auto draw_path = [target, brush, stroke_width](std::initializer_list<D2D1_POINT_2F> points) {
        auto current = points.begin();
        if (current == points.end()) {
            return;
        }
        auto previous = current++;
        for (; current != points.end(); ++current, ++previous) {
            target->DrawLine(*previous, *current, brush, stroke_width);
        }
    };

    switch (icon) {
    case VectorIcon::Globe:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), unit * 0.37f, unit * 0.37f), brush, stroke_width);
        target->DrawLine(p(-0.36f, 0.0f), p(0.36f, 0.0f), brush, stroke_width);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), unit * 0.15f, unit * 0.37f), brush, stroke_width);
        break;
    case VectorIcon::Gear: {
        ID2D1Factory* factory = nullptr;
        ID2D1PathGeometry* geometry = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        target->GetFactory(&factory);
        if (factory && SUCCEEDED(factory->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
            // Flat-topped teeth and recessed roots form one continuous cog outline.
            for (int vertex = 0; vertex < 32; ++vertex) {
                const float angle = (static_cast<float>(vertex) - 0.5f) * 3.14159265f / 16.0f;
                const float radius = vertex % 4 < 2 ? 0.43f : 0.32f;
                const auto point = p(std::cos(angle) * radius, std::sin(angle) * radius);
                if (vertex == 0) sink->BeginFigure(point, D2D1_FIGURE_BEGIN_HOLLOW);
                else sink->AddLine(point);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (SUCCEEDED(sink->Close())) target->DrawGeometry(geometry, brush, stroke_width);
        }
        if (sink) sink->Release();
        if (geometry) geometry->Release();
        if (factory) factory->Release();
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), unit * 0.13f, unit * 0.13f), brush, stroke_width);
        break;
    }
    case VectorIcon::Sun:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), unit * 0.20f, unit * 0.20f), brush, stroke_width);
        for (int index = 0; index < 8; ++index) {
            const float angle = static_cast<float>(index) * 3.14159265f / 4.0f;
            const float x = std::cos(angle);
            const float y = std::sin(angle);
            target->DrawLine(p(x * 0.31f, y * 0.31f), p(x * 0.44f, y * 0.44f), brush, stroke_width);
        }
        break;
    case VectorIcon::Moon:
        draw_path({
            p(0.10f, -0.43f), p(-0.10f, -0.40f), p(-0.27f, -0.28f),
            p(-0.35f, -0.08f), p(-0.32f, 0.16f), p(-0.17f, 0.34f),
            p(0.05f, 0.43f), p(0.23f, 0.35f), p(0.04f, 0.27f),
            p(-0.09f, 0.12f), p(-0.12f, -0.08f), p(-0.05f, -0.27f),
            p(0.10f, -0.43f)
        });
        break;
    case VectorIcon::Download:
    case VectorIcon::Upload: {
        const float direction = icon == VectorIcon::Download ? 1.0f : -1.0f;
        target->DrawLine(p(0.0f, -0.36f * direction), p(0.0f, 0.20f * direction), brush, stroke_width + 0.3f);
        target->DrawLine(p(-0.18f, 0.02f * direction), p(0.0f, 0.20f * direction), brush, stroke_width + 0.3f);
        target->DrawLine(p(0.18f, 0.02f * direction), p(0.0f, 0.20f * direction), brush, stroke_width + 0.3f);
        target->DrawLine(p(-0.30f, 0.36f * direction), p(0.30f, 0.36f * direction), brush, stroke_width);
        break;
    }
    case VectorIcon::Calendar:
        target->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.38f, -0.31f, 0.38f, 0.34f), unit * 0.07f, unit * 0.07f), brush, stroke_width);
        target->DrawLine(p(-0.38f, -0.10f), p(0.38f, -0.10f), brush, stroke_width);
        target->DrawLine(p(-0.19f, -0.41f), p(-0.19f, -0.21f), brush, stroke_width);
        target->DrawLine(p(0.19f, -0.41f), p(0.19f, -0.21f), brush, stroke_width);
        target->FillEllipse(D2D1::Ellipse(p(-0.15f, 0.09f), unit * 0.04f, unit * 0.04f), brush);
        target->FillEllipse(D2D1::Ellipse(p(0.12f, 0.09f), unit * 0.04f, unit * 0.04f), brush);
        break;
    case VectorIcon::Bars:
        target->DrawLine(p(-0.30f, 0.32f), p(-0.30f, -0.12f), brush, stroke_width + 0.2f);
        target->DrawLine(p(0.0f, 0.32f), p(0.0f, -0.34f), brush, stroke_width + 0.2f);
        target->DrawLine(p(0.30f, 0.32f), p(0.30f, 0.06f), brush, stroke_width + 0.2f);
        break;
    case VectorIcon::Wifi:
        draw_path({ p(-0.45f, -0.08f), p(-0.25f, -0.27f), p(0.0f, -0.33f), p(0.25f, -0.27f), p(0.45f, -0.08f) });
        draw_path({ p(-0.29f, 0.10f), p(-0.14f, -0.01f), p(0.0f, -0.04f), p(0.14f, -0.01f), p(0.29f, 0.10f) });
        draw_path({ p(-0.13f, 0.26f), p(0.0f, 0.20f), p(0.13f, 0.26f) });
        target->FillEllipse(D2D1::Ellipse(p(0.0f, 0.37f), unit * 0.055f, unit * 0.055f), brush);
        break;
    case VectorIcon::Close:
        target->DrawLine(p(-0.31f, -0.31f), p(0.31f, 0.31f), brush, stroke_width);
        target->DrawLine(p(0.31f, -0.31f), p(-0.31f, 0.31f), brush, stroke_width);
        break;
    case VectorIcon::ChevronDown:
        target->DrawLine(p(-0.24f, -0.10f), p(0.0f, 0.15f), brush, stroke_width);
        target->DrawLine(p(0.0f, 0.15f), p(0.24f, -0.10f), brush, stroke_width);
        break;
    case VectorIcon::Undo:
        draw_path({ p(-0.32f, -0.05f), p(-0.05f, -0.27f), p(0.27f, -0.14f), p(0.34f, 0.15f), p(0.16f, 0.34f), p(-0.07f, 0.31f) });
        target->DrawLine(p(-0.32f, -0.05f), p(-0.05f, 0.13f), brush, stroke_width);
        target->DrawLine(p(-0.32f, -0.05f), p(-0.12f, -0.27f), brush, stroke_width);
        break;
    case VectorIcon::Save:
        target->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.34f, -0.36f, 0.34f, 0.36f), unit * 0.05f, unit * 0.05f), brush, stroke_width);
        target->DrawRectangle(r(-0.19f, -0.33f, 0.18f, -0.06f), brush, stroke_width);
        target->DrawRectangle(r(-0.20f, 0.08f, 0.20f, 0.33f), brush, stroke_width);
        break;
    }
}

DashboardLayout GetDashboardLayout(float render_width, bool network_open, bool system_open, bool hide_gauges) {
    DashboardLayout layout;
    layout.start_x = std::clamp(render_width * 0.03f, 12.0f, 16.0f);
    layout.content_w = std::max(0.0f, render_width - layout.start_x * 2.0f);
    layout.session_y = layout.start_y + (layout.speed_h + layout.gap) * 2.0f;
    layout.snapshot_y = layout.session_y + layout.session_h + layout.gap;
    layout.status_y = layout.snapshot_y + layout.snapshot_h + layout.gap;
    layout.gauges_y = layout.status_y + layout.status_h + layout.gap;
    if (hide_gauges) {
        layout.gauges_h = 0.0f;
        layout.network_y = layout.gauges_y;
    } else {
        layout.network_y = layout.gauges_y + layout.gauges_h + layout.gap;
    }
    layout.network_h = 56.0f + (network_open ? 9.0f * 51.0f + 10.0f : 0.0f);
    layout.system_y = layout.network_y + layout.network_h + layout.gap;
    layout.system_h = 56.0f + (system_open ? 12.0f * 51.0f + 10.0f : 0.0f);
    layout.bottom = layout.system_y + layout.system_h + 20.0f;
    return layout;
}

struct NetworkTabLayout {
    float side = 16.0f;
    float content_w = 0.0f;
    float start_y = UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT + 16.0f;
    float gap = 16.0f;
    float quality_h = 84.0f;
    float metrics_y = 0.0f;
    float metrics_h = 104.0f;
    float connection_y = 0.0f;
    float connection_h = 214.0f;
    float signal_w = 116.0f;
    float signal_h = 92.0f;
    float speed_test_y = 0.0f;
    float speed_test_h = 300.0f;
    float bottom = 0.0f;
};

NetworkTabLayout GetNetworkTabLayout(float render_width) {
    NetworkTabLayout layout;
    layout.side = std::clamp(render_width * 0.035f, 16.0f, 20.0f);
    layout.content_w = std::max(0.0f, render_width - layout.side * 2.0f);
    // The source UI reserves a 120px connection visual. It must remain large
    // enough for the signal glyph and two-line badge to breathe.
    layout.signal_w = std::clamp(layout.content_w * 0.27f, 108.0f, 120.0f);
    layout.metrics_y = layout.start_y + layout.quality_h + layout.gap;
    layout.connection_y = layout.metrics_y + layout.metrics_h + layout.gap;
    layout.speed_test_y = layout.connection_y + layout.connection_h + layout.gap;
    layout.bottom = layout.speed_test_y + layout.speed_test_h + 20.0f;
    return layout;
}

D2D1_RECT_F GetNetworkAdapterRowRect(const NetworkTabLayout& layout) {
    return D2D1::RectF(
        layout.side + 16.0f,
        layout.connection_y + layout.connection_h - 50.0f,
        layout.side + layout.content_w - 16.0f,
        layout.connection_y + layout.connection_h - 16.0f
    );
}

bool ToolsExportUsesMonth(int period_index) {
    return period_index == 0 || period_index == 1;
}

const wchar_t* ToolsExportPeriodLabel(int period_index) {
    static constexpr const wchar_t* kLabels[] = {
        L"DAILY", L"WEEKLY", L"MONTHLY", L"YEARLY",
    };
    return kLabels[std::clamp(period_index, 0, 3)];
}

TrafficExportPeriod ToTrafficExportPeriod(int period_index) {
    switch (std::clamp(period_index, 0, 3)) {
    case 0: return TrafficExportPeriod::Daily;
    case 1: return TrafficExportPeriod::Weekly;
    case 2: return TrafficExportPeriod::Monthly;
    default: return TrafficExportPeriod::Yearly;
    }
}

std::vector<std::wstring> GetToolsSavedMonths(const TrafficHistorySnapshot& history) {
    std::vector<std::wstring> months;
    months.reserve(history.history.size());
    for (const TrafficHistoryItem& item : history.history) {
        int year = 0;
        int month = 0;
        if (item.date.size() >= 7 && ParseIsoDate(item.date.substr(0, 7), &year, &month) &&
            month >= 1 && month <= 12) {
            months.push_back(item.date.substr(0, 7));
        }
    }
    std::sort(months.begin(), months.end(), std::greater<>());
    months.erase(std::unique(months.begin(), months.end()), months.end());
    return months;
}

std::vector<int> GetToolsSavedYears(const TrafficHistorySnapshot& history) {
    std::vector<int> years = history.available_years;
    for (const TrafficHistoryItem& item : history.history) {
        int year = 0;
        int month = 0;
        if (item.date.size() >= 4 && ParseIsoDate(item.date.substr(0, 7), &year, &month) && year > 0) {
            years.push_back(year);
        }
    }
    std::sort(years.begin(), years.end(), std::greater<>());
    years.erase(std::unique(years.begin(), years.end()), years.end());
    return years;
}

std::wstring FormatToolsMonthLabel(const std::wstring& key) {
    int year = 0;
    int month = 0;
    if (ParseIsoDate(key, &year, &month) && month >= 1 && month <= 12) {
        return std::wstring(kLongMonthNames[month - 1]) + L" " + std::to_wstring(year);
    }
    return key;
}

struct ToolsTabLayout {
    float side = 16.0f;
    float content_w = 0.0f;
    float start_y = UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT + 16.0f;
    float gap = 16.0f;
    float diagnostics_y = 0.0f;
    float diagnostics_h = 112.0f;
    float updates_y = 0.0f;
    float updates_h = 134.0f;
    float export_y = 0.0f;
    float export_h = 230.0f;
    float bottom = 0.0f;
};

constexpr float kToolsCardInset = 18.0f;

D2D1_RECT_F GetToolsHeaderButtonRect(const ToolsTabLayout& layout, float card_top) {
    const float width = std::clamp(layout.content_w * 0.29f, 124.0f, 136.0f);
    const float right = layout.side + layout.content_w - kToolsCardInset;
    return D2D1::RectF(right - width, card_top + 15.0f, right, card_top + 45.0f);
}

D2D1_RECT_F GetToolsDownloadButtonRect(const ToolsTabLayout& layout) {
    return D2D1::RectF(
        layout.side + kToolsCardInset, layout.export_y + 182.0f,
        layout.side + layout.content_w - kToolsCardInset, layout.export_y + 218.0f
    );
}

ToolsTabLayout GetToolsTabLayout(
    float render_width,
    const DiagnosticsSnapshot& diagnostics,
    bool export_has_status
) {
    ToolsTabLayout layout;
    layout.side = std::clamp(render_width * 0.03f, 12.0f, 16.0f);
    layout.content_w = std::max(0.0f, render_width - layout.side * 2.0f);
    if (diagnostics.has_run) {
        size_t failed_count = 0;
        for (const DiagnosticTestResult& test : diagnostics.tests) {
            failed_count += test.status == DiagnosticStatus::Failed && !test.suggestion.empty();
        }
        layout.diagnostics_h = 60.0f + static_cast<float>(diagnostics.tests.size()) * 43.0f +
            static_cast<float>(failed_count) * 38.0f + 14.0f;
    }
    layout.diagnostics_y = layout.start_y;
    layout.updates_y = layout.diagnostics_y + layout.diagnostics_h + layout.gap;
    layout.export_y = layout.updates_y + layout.updates_h + layout.gap;
    layout.export_h += export_has_status ? 34.0f : 0.0f;
    layout.bottom = layout.export_y + layout.export_h + 20.0f;
    return layout;
}

std::wstring FormatNetworkLinkSpeed(uint64_t receive_bps, uint64_t transmit_bps) {
    const auto unit = [](uint64_t bits_per_second) {
        return bits_per_second >= 1'000'000'000ULL ? L"Gbps" : L"Mbps";
    };
    const auto value = [](uint64_t bits_per_second) {
        return bits_per_second >= 1'000'000'000ULL
            ? bits_per_second / 1'000'000'000ULL
            : bits_per_second / 1'000'000ULL;
    };

    if (receive_bps == 0 && transmit_bps == 0) {
        return L"Unavailable";
    }
    if (receive_bps == 0) {
        return std::to_wstring(value(transmit_bps)) + L" " + unit(transmit_bps);
    }
    if (transmit_bps == 0) {
        return std::to_wstring(value(receive_bps)) + L" " + unit(receive_bps);
    }
    const bool receive_is_gigabit = receive_bps >= 1'000'000'000ULL;
    const bool transmit_is_gigabit = transmit_bps >= 1'000'000'000ULL;
    if (receive_is_gigabit == transmit_is_gigabit) {
        return std::to_wstring(value(receive_bps)) + L" / " +
            std::to_wstring(value(transmit_bps)) + L" " + unit(receive_bps);
    }
    return std::to_wstring(value(receive_bps)) + L" " + unit(receive_bps) + L" / " +
        std::to_wstring(value(transmit_bps)) + L" " + unit(transmit_bps);
}

std::wstring FormatSpeedTestTimestamp(uint64_t unix_seconds) {
    if (unix_seconds == 0) {
        return L"Never";
    }

    const time_t timestamp = static_cast<time_t>(unix_seconds);
    tm local{};
    if (localtime_s(&local, &timestamp) != 0) {
        return L"Never";
    }

    const int display_hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    std::wostringstream stream;
    stream << (local.tm_mon + 1) << L"/" << local.tm_mday << L"/" << (local.tm_year + 1900)
           << L", " << display_hour << L":" << std::setw(2) << std::setfill(L'0') << local.tm_min
           << L":" << std::setw(2) << std::setfill(L'0') << local.tm_sec
           << (local.tm_hour < 12 ? L" AM" : L" PM");
    return stream.str();
}

std::wstring NetworkSignalLabel(const NetworkSnapshot& snapshot) {
    if (!snapshot.is_connected) {
        return L"OFFLINE";
    }
    if (snapshot.active_adapter_type == L"Wi-Fi") {
        if (snapshot.wifi_signal_percent >= 80) return L"EXCELLENT WI-FI";
        if (snapshot.wifi_signal_percent >= 60) return L"GOOD WI-FI";
        if (snapshot.wifi_signal_percent >= 40) return L"FAIR WI-FI";
        if (snapshot.wifi_signal_percent > 0) return L"WEAK WI-FI";
        return L"WI-FI";
    }
    if (snapshot.active_adapter_type == L"Ethernet") {
        return L"WIRED";
    }
    return L"CONNECTED";
}

struct DashboardUsageTotals {
    uint64_t download = 0;
    uint64_t upload = 0;
};

std::wstring GetWeekStartKey(const std::wstring& date) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (!ParseIsoDate(date, &year, &month, &day)) {
        return date;
    }

    tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_isdst = -1;
    if (mktime(&local) == static_cast<time_t>(-1)) {
        return date;
    }
    local.tm_mday -= (local.tm_wday + 6) % 7; // Monday starts the Tauri ISO week.
    mktime(&local);
    return MakeDateKey(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

DashboardUsageTotals GetDashboardUsageTotals(
    int period_index,
    const TrafficHistorySnapshot& history,
    const DashboardSnapshot& dashboard
) {
    if (history.history.empty()) {
        return { dashboard.today_download_bytes, dashboard.today_upload_bytes };
    }

    const auto newest = std::max_element(
        history.history.begin(),
        history.history.end(),
        [](const TrafficHistoryItem& left, const TrafficHistoryItem& right) {
            return left.date < right.date;
        }
    );
    if (newest == history.history.end()) {
        return {};
    }

    const std::wstring newest_day = newest->date;
    const std::wstring newest_week = GetWeekStartKey(newest_day);
    const std::wstring newest_month = newest_day.size() >= 7 ? newest_day.substr(0, 7) : newest_day;

    DashboardUsageTotals totals;
    for (const auto& record : history.history) {
        bool belongs_to_period = false;
        if (period_index == 0) {
            belongs_to_period = record.date == newest_day;
        } else if (period_index == 1) {
            belongs_to_period = GetWeekStartKey(record.date) == newest_week;
        } else {
            belongs_to_period = record.date.rfind(newest_month, 0) == 0;
        }
        if (belongs_to_period) {
            totals.download += record.rx_bytes;
            totals.upload += record.tx_bytes;
        }
    }
    return totals;
}

const wchar_t* DashboardUsagePeriodLabel(int period_index) {
    switch (period_index) {
    case 1: return L"This Week";
    case 2: return L"This Month";
    default: return L"Today";
    }
}

float DashboardUsageChipWidth(int period_index) {
    return period_index == 0 ? 60.0f : 88.0f;
}

std::wstring TruncateDashboardText(const std::wstring& value, size_t maximum_length = 42) {
    if (value.empty()) {
        return L"N/A";
    }
    if (value.size() <= maximum_length) {
        return value;
    }
    return value.substr(0, maximum_length - 3) + L"...";
}

std::wstring FormatDashboardLinkSpeed(uint64_t bits_per_second) {
    if (bits_per_second >= 1'000'000'000ULL) {
        return std::to_wstring(bits_per_second / 1'000'000'000ULL) + L" Gbps";
    }
    if (bits_per_second > 0) {
        return std::to_wstring(bits_per_second / 1'000'000ULL) + L" Mbps";
    }
    return L"N/A";
}

std::wstring FormatDashboardUptime(uint64_t seconds) {
    const uint64_t days = seconds / 86400ULL;
    const uint64_t hours = (seconds % 86400ULL) / 3600ULL;
    const uint64_t minutes = (seconds % 3600ULL) / 60ULL;
    std::wstringstream stream;
    if (days > 0) {
        stream << days << L"d ";
    }
    stream << hours << L"h " << minutes << L"m";
    return stream.str();
}

float EaseInOutQuart(float progress) {
    const float t = std::clamp(progress, 0.0f, 1.0f);
    if (t < 0.5f) {
        return 8.0f * t * t * t * t;
    }
    const float inverse = -2.0f * t + 2.0f;
    return 1.0f - (inverse * inverse * inverse * inverse) / 2.0f;
}

float EaseOutCubic(float progress) {
    const float t = std::clamp(progress, 0.0f, 1.0f);
    const float inverse = 1.0f - t;
    return 1.0f - inverse * inverse * inverse;
}

bool CpuReadingAvailable(const DashboardSnapshot& sample) {
    return Telemetry::FreshCpuSample(sample.cpu_sample_available, sample.cpu_sampled_at_ms, GetTickCount64()) &&
        std::isfinite(sample.cpu_usage_percent) && sample.cpu_usage_percent >= 0.0f && sample.cpu_usage_percent <= 100.0f;
}

std::wstring FormatCpuUsage(const DashboardSnapshot& sample) {
    if (!CpuReadingAvailable(sample)) return L"--";
    std::wostringstream text;
    text << std::fixed << std::setprecision(0) << sample.cpu_usage_percent << L"%";
    return text.str();
}

std::wstring FormatSourcePercentage(double value) {
    const double rounded = std::round(value * 10.0) / 10.0;
    const bool whole_number = std::abs(rounded - std::round(rounded)) < 0.0001;
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(whole_number ? 0 : 1) << rounded << L"%";
    return stream.str();
}

}  // namespace

AppRenderer::AppRenderer() {
    palette_ = UIStyle::GetPalette(current_theme_);
    // Pre-fill hist_snap.history with zeros
    for (int i = 0; i < 60; ++i) {
        speed_history_.push_back({ 0.0, 0.0 });
    }
}

AppRenderer::~AppRenderer() {
    Cleanup();
}

uint64_t AppRenderer::MainGraphicsTime() const {
#ifdef WATCHMAN_RENDER_TESTING
    if (test_main_time_) return test_main_time_;
#endif
    return GetTickCount64();
}

HRESULT AppRenderer::Initialize(HWND hwnd) {
    // Even an accidental hidden-window paint/initialize request must not pull
    // dashboard D2D/DWrite/fonts into the quiet widget-first startup path.
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return S_FALSE;
    const uint64_t now = MainGraphicsTime();
    if (!main_graphics_retry_.Due(now)) return main_initialization_error_;
    HRESULT hr = S_OK;
    if (!main_resources_initialized_) {
#ifdef WATCHMAN_RENDER_TESTING
        ++test_main_initialization_attempts_;
#endif
        hr = CreateDeviceIndependentResources();
        main_resources_initialized_ = SUCCEEDED(hr);
        if (FAILED(hr)) ReleaseMainDeviceIndependentResources(); // Includes partial font/factory setup.
        WatchmanStartupDiagnostics::Record("main-graphics", SUCCEEDED(hr) ? "initialized" : "failed",
                                           static_cast<uint32_t>(hr));
    }
    if (SUCCEEDED(hr)) {
        hr = CreateDeviceResources(hwnd);
        if (FAILED(hr)) WatchmanStartupDiagnostics::Record("main-graphics", "target-failed", static_cast<uint32_t>(hr));
    }
    main_initialization_error_ = hr;
    if (FAILED(hr)) {
        ReleaseMainWindowResources();
        main_graphics_retry_.Failed(now);
    }
    return hr;
}

void AppRenderer::DrawMainGraphicsFallback(HWND hwnd) {
    // No D2D, DWrite, bundled-font or GPU dependency. A deliberately opened
    // dashboard should explain failure, not leave a featureless white HWND.
    if (data_limit_edit_ && IsWindowVisible(data_limit_edit_)) {
        main_editor_suspended_ = true;
        ShowWindow(data_limit_edit_, SW_HIDE); // Preserve the draft, do not commit/cancel an unseen edit.
    }
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    const int saved = SaveDC(dc);
    RECT client{};
    GetClientRect(hwnd, &client);
    const bool dark = effective_theme_ == ThemeMode::Dark;
    SetDCBrushColor(dc, dark ? RGB(24, 26, 32) : RGB(245, 246, 249));
    const bool filled = FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))) != 0;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dark ? RGB(240, 241, 245) : RGB(32, 34, 42));
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    RECT title{24, 12, client.right - 100, 38};
    DrawTextW(dc, L"WATCHMAN", -1, &title, DT_SINGLELINE | DT_VCENTER);
    const auto bar = MainContentLayout::GetTitlebar(static_cast<float>(client.right));
    RECT minimize{static_cast<LONG>(bar.minimize_left), 8, static_cast<LONG>(bar.minimize_left + bar.action_size), 40};
    RECT close{static_cast<LONG>(bar.close_left), 8, static_cast<LONG>(bar.close_left + bar.action_size), 40};
    DrawFrameControl(dc, &minimize, DFC_CAPTION, DFCS_CAPTIONMIN);
    DrawFrameControl(dc, &close, DFC_CAPTION, DFCS_CAPTIONCLOSE);
    RECT text{24, 80, client.right - 24, 190};
    const int drawn = DrawTextW(dc,
        L"Dashboard graphics are temporarily unavailable. Watchman will retry automatically.\n\n"
        L"Monitoring and taskbar recovery run independently of this window.",
        -1, &text, DT_WORDBREAK | DT_NOPREFIX);
    wchar_t error[64]{};
    swprintf_s(error, L"Graphics error: 0x%08lX", static_cast<unsigned long>(main_initialization_error_));
    RECT detail{24, 198, client.right - 24, 226};
    DrawTextW(dc, error, -1, &detail, DT_SINGLELINE | DT_NOPREFIX);
    RECT exit{24, 240, 174, 274};
    DrawFrameControl(dc, &exit, DFC_BUTTON, DFCS_BUTTONPUSH);
    DrawTextW(dc, L"Exit Watchman", -1, &exit, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
#ifdef WATCHMAN_RENDER_TESTING
    if (filled && drawn) ++test_main_fallback_paints_;
#else
    (void)filled; (void)drawn;
#endif
    if (saved) RestoreDC(dc, saved);
    ReleaseDC(hwnd, dc);
}

void AppRenderer::Cleanup() {
    widget_compositor_.reset();
    widget_composition_failed_ = false;
    if (data_limit_edit_ && IsWindow(data_limit_edit_)) {
        DestroyWindow(data_limit_edit_);
    }
    data_limit_edit_ = nullptr;
    main_editor_suspended_ = false;
    ReleaseDeviceResources();
    ReleaseMainDeviceIndependentResources();
    main_initialization_error_ = S_OK;
    main_graphics_retry_.Succeeded();
}

void AppRenderer::ReleaseMainDeviceIndependentResources() {
    if (format_unit_speed_) { format_unit_speed_->Release(); format_unit_speed_ = nullptr; }
    if (format_title_) { format_title_->Release(); format_title_ = nullptr; }
    if (format_sidebar_) { format_sidebar_->Release(); format_sidebar_ = nullptr; }
    if (format_tab_) { format_tab_->Release(); format_tab_ = nullptr; }
    if (format_body_) { format_body_->Release(); format_body_ = nullptr; }
    if (format_body_bold_) { format_body_bold_->Release(); format_body_bold_ = nullptr; }
    if (format_titlebar_title_) { format_titlebar_title_->Release(); format_titlebar_title_ = nullptr; }
    if (format_titlebar_button_) { format_titlebar_button_->Release(); format_titlebar_button_ = nullptr; }
    if (format_status_) { format_status_->Release(); format_status_ = nullptr; }
    if (format_status_mono_) { format_status_mono_->Release(); format_status_mono_ = nullptr; }
    if (format_update_footer_) { format_update_footer_->Release(); format_update_footer_ = nullptr; }
    if (format_update_version_) { format_update_version_->Release(); format_update_version_ = nullptr; }
    if (format_values_) { format_values_->Release(); format_values_ = nullptr; }
    if (format_mono_) { format_mono_->Release(); format_mono_ = nullptr; }
    if (format_chart_tick_) { format_chart_tick_->Release(); format_chart_tick_ = nullptr; }
    if (format_data_label_) { format_data_label_->Release(); format_data_label_ = nullptr; }
    if (format_dashboard_label_) { format_dashboard_label_->Release(); format_dashboard_label_ = nullptr; }
    settings_sensor_setup_text_.Reset();
    settings_warning_details_text_.Reset();
    settings_warning_text_width_ = -1.0f;
    if (format_dashboard_note_) { format_dashboard_note_->Release(); format_dashboard_note_ = nullptr; }
    if (format_theme_choice_) { format_theme_choice_->Release(); format_theme_choice_ = nullptr; }
    for (auto& paragraph : welcome_text_) paragraph.Reset();
    welcome_text_width_ = -1;
    welcome_text_page_ = -1;
    if (format_welcome_title_) { format_welcome_title_->Release(); format_welcome_title_ = nullptr; }
    if (format_welcome_button_) { format_welcome_button_->Release(); format_welcome_button_ = nullptr; }
    if (format_floating_preview_) { format_floating_preview_->Release(); format_floating_preview_ = nullptr; }
    if (settings_version_layout_) { settings_version_layout_->Release(); settings_version_layout_ = nullptr; }
    settings_version_text_width_ = settings_title_text_width_ = 0;
    if (format_settings_version_) { format_settings_version_->Release(); format_settings_version_ = nullptr; }
    if (format_dashboard_value_) { format_dashboard_value_->Release(); format_dashboard_value_ = nullptr; }
    if (format_dashboard_gauge_) { format_dashboard_gauge_->Release(); format_dashboard_gauge_ = nullptr; }
    if (format_picker_title_) { format_picker_title_->Release(); format_picker_title_ = nullptr; }
    if (format_picker_subtitle_) { format_picker_subtitle_->Release(); format_picker_subtitle_ = nullptr; }
    if (format_picker_range_) { format_picker_range_->Release(); format_picker_range_ = nullptr; }
    if (format_picker_year_) { format_picker_year_->Release(); format_picker_year_ = nullptr; }
    if (format_picker_month_) { format_picker_month_->Release(); format_picker_month_ = nullptr; }
    if (format_picker_section_) { format_picker_section_->Release(); format_picker_section_ = nullptr; }
    if (format_picker_nav_) { format_picker_nav_->Release(); format_picker_nav_ = nullptr; }
    if (format_comparison_label_) { format_comparison_label_->Release(); format_comparison_label_ = nullptr; }
    if (format_comparison_detail_) { format_comparison_detail_->Release(); format_comparison_detail_ = nullptr; }
    if (format_limit_status_) { format_limit_status_->Release(); format_limit_status_ = nullptr; }
    if (format_limit_pct_) { format_limit_pct_->Release(); format_limit_pct_ = nullptr; }
    if (format_limit_input_) { format_limit_input_->Release(); format_limit_input_ = nullptr; }
    if (format_limit_button_) { format_limit_button_->Release(); format_limit_button_ = nullptr; }
    if (format_tooltip_title_) { format_tooltip_title_->Release(); format_tooltip_title_ = nullptr; }
    if (format_tooltip_body_) { format_tooltip_body_->Release(); format_tooltip_body_ = nullptr; }
    if (format_app_header_) { format_app_header_->Release(); format_app_header_ = nullptr; }
    if (format_app_subtitle_) { format_app_subtitle_->Release(); format_app_subtitle_ = nullptr; }
    if (format_app_label_) { format_app_label_->Release(); format_app_label_ = nullptr; }
    if (format_app_body_) { format_app_body_->Release(); format_app_body_ = nullptr; }
    if (format_app_body_bold_) { format_app_body_bold_->Release(); format_app_body_bold_ = nullptr; }
    if (format_app_icon_) { format_app_icon_->Release(); format_app_icon_ = nullptr; }
    if (format_app_mono_) { format_app_mono_->Release(); format_app_mono_ = nullptr; }
    if (format_app_badge_) { format_app_badge_->Release(); format_app_badge_ = nullptr; }
    if (format_app_conns_) { format_app_conns_->Release(); format_app_conns_ = nullptr; }
    if (format_network_quality_) { format_network_quality_->Release(); format_network_quality_ = nullptr; }
    if (format_network_quality_sub_) { format_network_quality_sub_->Release(); format_network_quality_sub_ = nullptr; }
    if (format_network_metric_label_) { format_network_metric_label_->Release(); format_network_metric_label_ = nullptr; }
    if (format_network_metric_value_) { format_network_metric_value_->Release(); format_network_metric_value_ = nullptr; }
    if (format_network_metric_detail_) { format_network_metric_detail_->Release(); format_network_metric_detail_ = nullptr; }
    if (format_network_row_label_) { format_network_row_label_->Release(); format_network_row_label_ = nullptr; }
    if (format_network_row_value_) { format_network_row_value_->Release(); format_network_row_value_ = nullptr; }
    if (format_network_signal_label_) { format_network_signal_label_->Release(); format_network_signal_label_ = nullptr; }
    if (format_network_speed_label_) { format_network_speed_label_->Release(); format_network_speed_label_ = nullptr; }
    if (format_network_speed_value_) { format_network_speed_value_->Release(); format_network_speed_value_ = nullptr; }
    if (format_data_period_) { format_data_period_->Release(); format_data_period_ = nullptr; }
    if (format_data_filter_) { format_data_filter_->Release(); format_data_filter_ = nullptr; }
    if (format_data_range_) { format_data_range_->Release(); format_data_range_ = nullptr; }
    if (format_widget_) { format_widget_->Release(); format_widget_ = nullptr; }
    if (format_large_speed_) { format_large_speed_->Release(); format_large_speed_ = nullptr; }
    if (format_speed_arrow_) { format_speed_arrow_->Release(); format_speed_arrow_ = nullptr; }
    
    bundled_fonts_.Reset();
    if (dwrite_factory_) { dwrite_factory_->Release(); dwrite_factory_ = nullptr; }
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }
    main_resources_initialized_ = false;
}

HRESULT AppRenderer::CreateDeviceIndependentResources() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    if (SUCCEEDED(hr)) {
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite_factory_));
    }
    if (FAILED(hr)) {
        return hr;
    }

    hr = bundled_fonts_.Initialize(dwrite_factory_);
    if (FAILED(hr)) return hr;
    const auto create_format = [&](const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, IDWriteTextFormat** format) {
        return bundled_fonts_.CreateTextFormat(family, weight, size, format);
    };

    if (FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_NORMAL, 14.0f, &format_body_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 13.0f, &format_body_bold_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, UIStyle::TEXT_SMALL, &format_status_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_SMALL, &format_status_mono_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, WatchmanUpdates::Presentation::footer_font_size, &format_update_footer_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, 18.0f, &format_update_version_)) ||
        FAILED(hr = create_format(UIStyle::FONT_VALUES, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 18.0f, &format_values_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_SMALL, &format_mono_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_SMALL, &format_chart_tick_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_data_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_dashboard_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, UIStyle::TEXT_SMALL, &format_dashboard_note_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, UIStyle::TEXT_BODY, &format_theme_choice_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, WatchmanWelcome::title_font_size, &format_welcome_title_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, WatchmanWelcome::button_font_size, &format_welcome_button_)) ||
        FAILED(hr = create_format(UIStyle::FONT_VALUES, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 22.0f, &format_dashboard_value_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, SettingsHeaderLayout::version_font_size, &format_settings_version_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 13.0f, &format_dashboard_gauge_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 12.0f, &format_data_period_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, &format_data_filter_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f, &format_data_range_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 12.0f, &format_picker_title_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, &format_picker_subtitle_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 11.0f, &format_picker_range_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 13.0f, &format_picker_year_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 11.0f, &format_picker_month_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_picker_section_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_NORMAL, 16.0f, &format_picker_nav_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_comparison_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_SMALL, &format_comparison_detail_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, &format_limit_status_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_BOLD, 11.0f, &format_limit_pct_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, &format_limit_input_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_limit_button_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 12.0f, &format_tooltip_title_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, &format_tooltip_body_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 13.0f, &format_app_header_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_BODY, &format_app_subtitle_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_app_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_SMALL, &format_app_body_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, UIStyle::TEXT_BODY, &format_app_body_bold_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.0f, &format_app_icon_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_SMALL, &format_app_mono_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_app_badge_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_SEMI_BOLD, UIStyle::TEXT_BODY, &format_app_conns_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 16.0f, &format_network_quality_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_MEDIUM, UIStyle::TEXT_SMALL, &format_network_quality_sub_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_network_metric_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_VALUES, DWRITE_FONT_WEIGHT_EXTRA_BOLD, 26.0f, &format_network_metric_value_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_network_metric_detail_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_network_row_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 12.0f, &format_network_row_value_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_network_signal_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, &format_network_speed_label_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_BOLD, 13.0f, &format_network_speed_value_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_BOLD, 11.0f, &format_widget_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 18.0f, &format_title_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 14.0f, &format_titlebar_title_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 11.0f, &format_titlebar_button_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 11.0f, &format_sidebar_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, UIStyle::TEXT_SMALL, &format_tab_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 36.0f, &format_large_speed_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 14.0f, &format_unit_speed_)) ||
        FAILED(hr = create_format(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, 24.0f, &format_speed_arrow_))) {
        return hr;
    }

#ifdef WATCHMAN_RENDER_TESTING
    // Fail after real partial allocation so retry tests exercise its cleanup.
    if (FAILED(test_main_initialization_error_)) return test_main_initialization_error_;
#endif
    // Use the same built-in family/default weight as the independent GDI window.
    // This format also supplies the welcome sample; a settings layout overrides
    // its own range, never this shared format or the welcome's default weight.
    hr = dwrite_factory_->CreateTextFormat(FloatingMonitorText::family, nullptr,
        static_cast<DWRITE_FONT_WEIGHT>(FloatingMonitorText::Value(FloatingMonitorText::default_weight)),
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        static_cast<float>(FloatingMonitorLayout::default_font_size), L"", &format_floating_preview_);
    if (FAILED(hr)) return hr;
    format_floating_preview_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_floating_preview_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (FAILED(hr = SettingsText::ConfigureThemeChoice(format_theme_choice_))) return hr;
    if (FAILED(hr = SettingsText::ConfigureThemeChoice(format_welcome_button_))) return hr;
    format_welcome_button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    format_values_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_values_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_values_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_data_label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_data_label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_data_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_dashboard_label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format_dashboard_label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_dashboard_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_dashboard_note_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format_dashboard_note_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_dashboard_note_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_dashboard_value_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format_dashboard_value_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_dashboard_value_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (FAILED(hr = SettingsText::ConfigureThemeChoice(format_settings_version_)) ||
        FAILED(hr = format_settings_version_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER))) return hr;
    const auto& version_label = WatchmanVersion::Label();
    if (FAILED(hr = dwrite_factory_->CreateTextLayout(version_label.c_str(),
        static_cast<UINT32>(version_label.size()), format_settings_version_, 4096, 128,
        &settings_version_layout_))) return hr;
    DWRITE_TEXT_METRICS version_metrics{};
    if (FAILED(hr = settings_version_layout_->GetMetrics(&version_metrics))) return hr;
    settings_version_text_width_ = version_metrics.widthIncludingTrailingWhitespace;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> settings_title_measure;
    if (FAILED(hr = dwrite_factory_->CreateTextLayout(L"SETTINGS", 8, format_dashboard_value_,
        4096, 128, settings_title_measure.GetAddressOf()))) return hr;
    DWRITE_TEXT_METRICS title_metrics{};
    if (FAILED(hr = settings_title_measure->GetMetrics(&title_metrics))) return hr;
    settings_title_text_width_ = title_metrics.widthIncludingTrailingWhitespace;
    format_dashboard_gauge_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_dashboard_gauge_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_dashboard_gauge_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_status_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_status_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_update_footer_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_update_footer_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_update_footer_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_update_version_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_update_version_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_status_mono_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    format_status_mono_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_status_mono_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_sidebar_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_sidebar_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_tab_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_tab_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_titlebar_title_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_titlebar_title_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_titlebar_button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_titlebar_button_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_titlebar_button_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_speed_arrow_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_speed_arrow_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_speed_arrow_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_data_period_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_data_period_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_data_period_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_data_filter_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_data_filter_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_data_filter_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_data_range_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_data_range_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_data_range_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_chart_tick_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_chart_tick_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_chart_tick_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_picker_title_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    format_picker_subtitle_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    format_picker_range_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_picker_range_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_picker_range_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_picker_year_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_picker_year_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_picker_year_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_picker_month_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_picker_month_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_picker_month_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_picker_section_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_picker_nav_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_picker_nav_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_picker_nav_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_comparison_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_comparison_detail_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_limit_status_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    format_limit_status_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_limit_status_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_limit_pct_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    format_limit_pct_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_limit_pct_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_limit_input_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_limit_input_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_limit_button_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_limit_button_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_limit_button_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_tooltip_title_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_tooltip_body_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_header_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_subtitle_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    format_app_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_body_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_body_bold_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_icon_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_app_icon_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_app_icon_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_mono_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_badge_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_app_badge_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_app_badge_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_app_conns_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_app_conns_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_app_conns_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_quality_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_quality_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_quality_sub_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_quality_sub_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_metric_label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_network_metric_label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_metric_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_metric_value_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_network_metric_value_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_metric_value_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_metric_detail_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_network_metric_detail_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_metric_detail_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_row_label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_row_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_row_value_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    format_network_row_value_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_row_value_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_signal_label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_network_signal_label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_signal_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    format_network_speed_label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_speed_label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_network_speed_value_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    format_network_speed_value_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_network_speed_value_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(format_network_speed_value_, &ellipsis))) {
            const DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            format_network_speed_value_->SetTrimming(&trimming, ellipsis);
            ellipsis->Release();
        }
    format_mono_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_large_speed_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_large_speed_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_unit_speed_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_unit_speed_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_widget_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_widget_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return S_OK;
}

HRESULT AppRenderer::CreateDeviceResources(HWND hwnd) {
    if (render_target_) return S_OK;
#ifdef WATCHMAN_RENDER_TESTING
    ++test_main_target_attempts_;
    if (FAILED(test_main_target_error_)) return test_main_target_error_;
#endif
    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    HRESULT hr = d2d_factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(hwnd, size),
        &render_target_
    );

    if (SUCCEEDED(hr)) {
        UpdatePaletteBrushes();
        LoadTitlebarLogoBitmap();
    }
    return hr;
}

void AppRenderer::LoadTitlebarLogoBitmap() {
    if (!render_target_ || titlebar_logo_bitmap_) {
        return;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(
        instance,
        MAKEINTRESOURCEW(IDR_WATCHMAN_LOGO),
        MAKEINTRESOURCEW(10)
    );
    if (!resource) {
        return;
    }
    HGLOBAL loaded_resource = LoadResource(instance, resource);
    const DWORD resource_size = SizeofResource(instance, resource);
    void* resource_data = loaded_resource ? LockResource(loaded_resource) : nullptr;
    if (!resource_data || resource_size == 0) {
        return;
    }

    IWICImagingFactory* wic_factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_factory)
    );
    if (SUCCEEDED(hr)) hr = wic_factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(static_cast<BYTE*>(resource_data), resource_size);
    }
    if (SUCCEEDED(hr)) {
        hr = wic_factory->CreateDecoderFromStream(
            stream,
            nullptr,
            WICDecodeMetadataCacheOnLoad,
            &decoder
        );
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic_factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        );
    }
    if (SUCCEEDED(hr)) {
        render_target_->CreateBitmapFromWicBitmap(converter, nullptr, &titlebar_logo_bitmap_);
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (wic_factory) wic_factory->Release();
}

void AppRenderer::ReleaseMainWindowResources() {
    if (titlebar_logo_bitmap_) { titlebar_logo_bitmap_->Release(); titlebar_logo_bitmap_ = nullptr; }
    if (app_gradient_brush_) { app_gradient_brush_->Release(); app_gradient_brush_ = nullptr; }
    if (app_gradient_stops_) { app_gradient_stops_->Release(); app_gradient_stops_ = nullptr; }
    if (app_pink_glow_brush_) { app_pink_glow_brush_->Release(); app_pink_glow_brush_ = nullptr; }
    if (app_yellow_glow_brush_) { app_yellow_glow_brush_->Release(); app_yellow_glow_brush_ = nullptr; }
    if (app_peach_glow_brush_) { app_peach_glow_brush_->Release(); app_peach_glow_brush_ = nullptr; }
    if (app_pink_glow_stops_) { app_pink_glow_stops_->Release(); app_pink_glow_stops_ = nullptr; }
    if (app_yellow_glow_stops_) { app_yellow_glow_stops_->Release(); app_yellow_glow_stops_ = nullptr; }
    if (app_peach_glow_stops_) { app_peach_glow_stops_->Release(); app_peach_glow_stops_ = nullptr; }
    if (brush_background_) { brush_background_->Release(); brush_background_ = nullptr; }
    if (brush_sidebar_bg_) { brush_sidebar_bg_->Release(); brush_sidebar_bg_ = nullptr; }
    if (brush_card_bg_) { brush_card_bg_->Release(); brush_card_bg_ = nullptr; }
        if (brush_session_card_) { brush_session_card_->Release(); brush_session_card_ = nullptr; }
        if (brush_snapshot_card_) { brush_snapshot_card_->Release(); brush_snapshot_card_ = nullptr; }
    if (brush_card_border_) { brush_card_border_->Release(); brush_card_border_ = nullptr; }
    if (brush_text_primary_) { brush_text_primary_->Release(); brush_text_primary_ = nullptr; }
    if (brush_text_secondary_) { brush_text_secondary_->Release(); brush_text_secondary_ = nullptr; }
    if (brush_accent_dl_) { brush_accent_dl_->Release(); brush_accent_dl_ = nullptr; }
    if (brush_accent_ul_) { brush_accent_ul_->Release(); brush_accent_ul_ = nullptr; }
    if (brush_accent_sys_) { brush_accent_sys_->Release(); brush_accent_sys_ = nullptr; }
    if (brush_hover_) { brush_hover_->Release(); brush_hover_ = nullptr; }
    if (brush_active_) { brush_active_->Release(); brush_active_ = nullptr; }
    if (brush_popup_bg_) { brush_popup_bg_->Release(); brush_popup_bg_ = nullptr; }
    if (brush_popup_shadow_) { brush_popup_shadow_->Release(); brush_popup_shadow_ = nullptr; }
    if (brush_picker_month_bg_) { brush_picker_month_bg_->Release(); brush_picker_month_bg_ = nullptr; }
    if (brush_picker_month_border_) { brush_picker_month_border_->Release(); brush_picker_month_border_ = nullptr; }
    if (brush_tooltip_bg_) { brush_tooltip_bg_->Release(); brush_tooltip_bg_ = nullptr; }
    if (brush_tooltip_border_) { brush_tooltip_border_->Release(); brush_tooltip_border_ = nullptr; }
    if (brush_tooltip_text_) { brush_tooltip_text_->Release(); brush_tooltip_text_ = nullptr; }
    if (brush_success_) { brush_success_->Release(); brush_success_ = nullptr; }
    if (brush_data_muted_) { brush_data_muted_->Release(); brush_data_muted_ = nullptr; }
    if (brush_limit_normal_) { brush_limit_normal_->Release(); brush_limit_normal_ = nullptr; }
    if (brush_limit_warning_) { brush_limit_warning_->Release(); brush_limit_warning_ = nullptr; }
    if (brush_limit_danger_) { brush_limit_danger_->Release(); brush_limit_danger_ = nullptr; }
    if (brush_limit_track_) { brush_limit_track_->Release(); brush_limit_track_ = nullptr; }
    if (brush_active_text_) { brush_active_text_->Release(); brush_active_text_ = nullptr; }
    if (brush_action_bg_) { brush_action_bg_->Release(); brush_action_bg_ = nullptr; }
    if (brush_chart_download_) { brush_chart_download_->Release(); brush_chart_download_ = nullptr; }
    if (brush_chart_upload_) { brush_chart_upload_->Release(); brush_chart_upload_ = nullptr; }
    if (brush_chart_grid_) { brush_chart_grid_->Release(); brush_chart_grid_ = nullptr; }
    if (brush_chart_tick_) { brush_chart_tick_->Release(); brush_chart_tick_ = nullptr; }
    if (render_target_) { render_target_->Release(); render_target_ = nullptr; }
}

void AppRenderer::ReleaseDeviceResources() {
    ReleaseMainWindowResources();
    if (widget_render_target_) { widget_render_target_->Release(); widget_render_target_ = nullptr; }
}

void AppRenderer::UpdatePaletteBrushes() {
    if (!render_target_) return;

    if (brush_background_) brush_background_->Release();
    if (brush_sidebar_bg_) brush_sidebar_bg_->Release();
    if (brush_card_bg_) brush_card_bg_->Release();
        if (brush_session_card_) { brush_session_card_->Release(); brush_session_card_ = nullptr; }
        if (brush_snapshot_card_) { brush_snapshot_card_->Release(); brush_snapshot_card_ = nullptr; }
    if (brush_card_border_) brush_card_border_->Release();
    if (brush_text_primary_) brush_text_primary_->Release();
    if (brush_text_secondary_) brush_text_secondary_->Release();
    if (brush_accent_dl_) brush_accent_dl_->Release();
    if (brush_accent_ul_) brush_accent_ul_->Release();
    if (brush_accent_sys_) brush_accent_sys_->Release();
    if (brush_hover_) brush_hover_->Release();
    if (brush_active_) brush_active_->Release();
    if (brush_popup_bg_) brush_popup_bg_->Release();
    if (brush_popup_shadow_) brush_popup_shadow_->Release();
    if (brush_picker_month_bg_) brush_picker_month_bg_->Release();
    if (brush_picker_month_border_) brush_picker_month_border_->Release();
    if (brush_tooltip_bg_) brush_tooltip_bg_->Release();
    if (brush_tooltip_border_) brush_tooltip_border_->Release();
    if (brush_tooltip_text_) brush_tooltip_text_->Release();
    if (brush_success_) brush_success_->Release();
    if (brush_data_muted_) brush_data_muted_->Release();
    if (brush_limit_normal_) brush_limit_normal_->Release();
    if (brush_limit_warning_) brush_limit_warning_->Release();
    if (brush_limit_danger_) brush_limit_danger_->Release();
    if (brush_limit_track_) brush_limit_track_->Release();
    if (brush_active_text_) brush_active_text_->Release();
    if (brush_action_bg_) brush_action_bg_->Release();
    if (brush_chart_download_) brush_chart_download_->Release();
    if (brush_chart_upload_) brush_chart_upload_->Release();
    if (brush_chart_grid_) brush_chart_grid_->Release();
    if (brush_chart_tick_) brush_chart_tick_->Release();
    if (app_gradient_brush_) { app_gradient_brush_->Release(); app_gradient_brush_ = nullptr; }
    if (app_gradient_stops_) { app_gradient_stops_->Release(); app_gradient_stops_ = nullptr; }
    if (app_pink_glow_brush_) { app_pink_glow_brush_->Release(); app_pink_glow_brush_ = nullptr; }
    if (app_yellow_glow_brush_) { app_yellow_glow_brush_->Release(); app_yellow_glow_brush_ = nullptr; }
    if (app_peach_glow_brush_) { app_peach_glow_brush_->Release(); app_peach_glow_brush_ = nullptr; }
    if (app_pink_glow_stops_) { app_pink_glow_stops_->Release(); app_pink_glow_stops_ = nullptr; }
    if (app_yellow_glow_stops_) { app_yellow_glow_stops_->Release(); app_yellow_glow_stops_ = nullptr; }
    if (app_peach_glow_stops_) { app_peach_glow_stops_->Release(); app_peach_glow_stops_ = nullptr; }

    render_target_->CreateSolidColorBrush(palette_.Background, &brush_background_);
    render_target_->CreateSolidColorBrush(palette_.SidebarBackground, &brush_sidebar_bg_);
    render_target_->CreateSolidColorBrush(palette_.CardBackground, &brush_card_bg_);
    render_target_->CreateSolidColorBrush(palette_.CardBorder, &brush_card_border_);
    render_target_->CreateSolidColorBrush(palette_.TextPrimary, &brush_text_primary_);
    render_target_->CreateSolidColorBrush(palette_.TextSecondary, &brush_text_secondary_);
    render_target_->CreateSolidColorBrush(palette_.AccentDownload, &brush_accent_dl_);
    render_target_->CreateSolidColorBrush(palette_.AccentUpload, &brush_accent_ul_);
    render_target_->CreateSolidColorBrush(palette_.AccentSystem, &brush_accent_sys_);
    render_target_->CreateSolidColorBrush(palette_.ControlHover, &brush_hover_);
    render_target_->CreateSolidColorBrush(palette_.ControlActive, &brush_active_);
    render_target_->CreateSolidColorBrush(palette_.ActionBackground, &brush_action_bg_);
    const bool light_theme = effective_theme_ == ThemeMode::Light;
    render_target_->CreateSolidColorBrush(
        light_theme ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0x18 / 255.0f, 0x18 / 255.0f, 0x18 / 255.0f, 1.0f),
        &brush_popup_bg_
    );
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f), &brush_popup_shadow_);
    render_target_->CreateSolidColorBrush(
        light_theme ? D2D1::ColorF(0.067f, 0.094f, 0.153f, 0.03f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f),
        &brush_picker_month_bg_
    );
    render_target_->CreateSolidColorBrush(
        light_theme ? D2D1::ColorF(0.067f, 0.094f, 0.153f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f),
        &brush_picker_month_border_
    );
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.80f), &brush_tooltip_bg_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &brush_tooltip_border_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_tooltip_text_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.063f, 0.725f, 0.506f, 1.0f), &brush_success_);
    render_target_->CreateSolidColorBrush(
        light_theme ? D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f) : palette_.TextSecondary,
        &brush_data_muted_
    );
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.231f, 0.510f, 0.965f, 1.0f), &brush_limit_normal_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.961f, 0.620f, 0.043f, 1.0f), &brush_limit_warning_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.937f, 0.267f, 0.267f, 1.0f), &brush_limit_danger_);
    render_target_->CreateSolidColorBrush(
        light_theme ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f),
        &brush_limit_track_
    );
    render_target_->CreateSolidColorBrush(
        light_theme ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.02f, 0.02f, 0.03f, 1.0f),
        &brush_active_text_
    );
    const auto history_colors = UIStyle::GetHistoryPalette(effective_theme_);
    render_target_->CreateSolidColorBrush(history_colors.Download, &brush_chart_download_);
    render_target_->CreateSolidColorBrush(history_colors.Upload, &brush_chart_upload_);
    render_target_->CreateSolidColorBrush(history_colors.Grid, &brush_chart_grid_);
    render_target_->CreateSolidColorBrush(history_colors.Tick, &brush_chart_tick_);

    // Only Light uses the original CSS pastel layers. Sampling the blurred
    // disks once avoids scaling the blur with the window or doing CPU work per frame.
    if (light_theme) {
        const auto& reference = LightThemeReference::GetPalette();
        const auto color = [](const LightThemeReference::Color& value) {
            return D2D1::ColorF(value.red / 255.0f, value.green / 255.0f,
                               value.blue / 255.0f, static_cast<float>(value.alpha));
        };
        ID2D1GradientStopCollection** collections[] = {
            &app_pink_glow_stops_, &app_yellow_glow_stops_, &app_peach_glow_stops_ };
        ID2D1RadialGradientBrush** brushes[] = {
            &app_pink_glow_brush_, &app_yellow_glow_brush_, &app_peach_glow_brush_ };
        for (size_t i = 0; i < reference.layers.size(); ++i) {
            const auto& layer = reference.layers[i];
            std::array<D2D1_GRADIENT_STOP, LightThemeReference::stop_count> stops{};
            for (size_t j = 0; j < stops.size(); ++j) {
                stops[j] = { static_cast<float>(layer.stops[j].position), color(layer.source_color) };
                stops[j].color.a = static_cast<float>(layer.stops[j].alpha);
            }
            if (SUCCEEDED(render_target_->CreateGradientStopCollection(stops.data(),
                    static_cast<UINT32>(stops.size()), D2D1_GAMMA_2_2,
                    D2D1_EXTEND_MODE_CLAMP, collections[i]))) {
                render_target_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
                    D2D1::Point2F(), D2D1::Point2F(), 1.0f, 1.0f), *collections[i], brushes[i]);
            }
        }
        const auto create_warm_card = [&](const LightThemeReference::CardGradient& card,
                                           ID2D1LinearGradientBrush** brush) {
            const D2D1_GRADIENT_STOP stops[] = { {0.0f, color(card.start)}, {1.0f, color(card.end)} };
            ID2D1GradientStopCollection* collection = nullptr;
            if (SUCCEEDED(render_target_->CreateGradientStopCollection(stops, ARRAYSIZE(stops),
                    D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection))) {
                render_target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(), D2D1::Point2F(1.0f, 1.0f)), collection, brush);
                collection->Release();
            }
        };
        create_warm_card(reference.session_card, &brush_session_card_);
        create_warm_card(reference.snapshot_card, &brush_snapshot_card_);
    }
}

void AppRenderer::DrawAppBackground(D2D1_SIZE_F rt_size) {
    const D2D1_RECT_F full_rect = D2D1::RectF(0.0f, 0.0f, rt_size.width, rt_size.height);
    render_target_->FillRectangle(full_rect, brush_background_);

    if (effective_theme_ == ThemeMode::Light &&
        app_pink_glow_brush_ && app_yellow_glow_brush_ && app_peach_glow_brush_) {
        const auto& layers = LightThemeReference::GetPalette().layers;
        ID2D1RadialGradientBrush* brushes[] = {
            app_pink_glow_brush_, app_yellow_glow_brush_, app_peach_glow_brush_ };
        for (size_t i = 0; i < layers.size(); ++i) {
            const auto center = layers[i].CenterForCanvas(rt_size.width, rt_size.height);
            brushes[i]->SetCenter(D2D1::Point2F(static_cast<float>(center.x), static_cast<float>(center.y)));
            brushes[i]->SetRadiusX(static_cast<float>(layers[i].support_radius));
            brushes[i]->SetRadiusY(static_cast<float>(layers[i].support_radius));
            render_target_->FillRectangle(full_rect, brushes[i]);
        }
        return;
    }

    if (effective_theme_ == ThemeMode::Light && app_gradient_brush_) {
        app_gradient_brush_->SetStartPoint(D2D1::Point2F(0.0f, 0.0f));
        app_gradient_brush_->SetEndPoint(D2D1::Point2F(rt_size.width, rt_size.height));
        render_target_->FillRectangle(full_rect, app_gradient_brush_);
        return;
    }
}

void AppRenderer::SetTheme(ThemeMode mode) {
    current_theme_ = UIStyle::NormalizeTheme(static_cast<int>(mode));
    effective_theme_ = UIStyle::ResolveTheme(current_theme_, ReadWindowsLightTaskbarTheme());
    palette_ = UIStyle::GetPalette(effective_theme_);
    UpdatePaletteBrushes();
}

void AppRenderer::Resize(UINT width, UINT height) {
    render_size_ = D2D1::SizeF(static_cast<float>(width), static_cast<float>(height));
    if (render_target_) {
        render_target_->Resize(D2D1::SizeU(width, height));
    }
    ClampContentScroll(active_tab_);
}

float AppRenderer::ContentViewportTop() const {
    return UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT;
}

float AppRenderer::ContentViewportBottom() const {
    return std::max(ContentViewportTop(), render_size_.height - UIStyle::STATUSBAR_HEIGHT);
}

float AppRenderer::CurrentContentScroll() const {
    return content_scroll_offsets_[static_cast<int>(active_tab_)];
}

float AppRenderer::ContentScrollMax(ActiveTab tab) const {
    const float content_bottom = content_bottoms_[static_cast<int>(tab)];
    return std::max(0.0f, content_bottom - ContentViewportBottom());
}

void AppRenderer::ClampContentScroll(ActiveTab tab) {
    const int index = static_cast<int>(tab);
    content_scroll_offsets_[index] = std::clamp(
        content_scroll_offsets_[index],
        0.0f,
        ContentScrollMax(tab)
    );
}

void AppRenderer::ReportContentBottom(float bottom) {
    const int index = static_cast<int>(active_tab_);
    content_bottoms_[index] = std::max(content_bottoms_[index], bottom);
}

void AppRenderer::SetUpdateResult(const WatchmanUpdates::Result& result) {
    const auto previous = update_notice_.Key();
    update_notice_.Apply(result);
    update_action_status_.clear();
    if (update_notice_.Key() != previous) {
        update_download_error_.clear();
        update_focus_ = 1;
    }
    if (!update_notice_.Available()) update_prompt_open_ = update_prompt_requested_ = false;
}

void AppRenderer::RememberUpdateMarker(const wchar_t* name) {
#ifndef WATCHMAN_RENDER_TESTING
    Watchman::ScopedHKey key;
    const LONG opened = RegCreateKeyExW(HKEY_CURRENT_USER, WatchmanUpdates::preference_path, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, key.Put(), nullptr);
    if (opened != ERROR_SUCCESS || WatchmanUpdates::WriteMarker(key.Get(), name, update_notice_.Key()) != ERROR_SUCCESS)
        OutputDebugStringW(L"Watchman: could not remember an update notice; the in-memory marker remains active.\n");
#else
    (void)name; // Owned-window CTests must not write real update preferences.
#endif
}

void AppRenderer::MarkUpdateNotified() {
    update_notice_.Notified();
    RememberUpdateMarker(WatchmanUpdates::notified_value);
}

void AppRenderer::DismissUpdatePrompt() {
    update_prompt_open_ = update_prompt_requested_ = false;
    update_notice_.Dismissed();
    RememberUpdateMarker(WatchmanUpdates::dismissed_value);
}

void AppRenderer::OfferUpdatePrompt(bool interactive, bool force) {
    if (force) update_prompt_requested_ = true;
    if (!interactive || update_prompt_open_ || !update_notice_.Available() ||
        welcome_tour_.IsOpen() || settings_open_ || speed_consent_open_ || speed_details_open_ ||
        (data_limit_edit_ && IsWindowVisible(data_limit_edit_))) return;
    if (!update_prompt_requested_ && !update_notice_.MayPrompt()) return;
    update_prompt_open_ = true;
    update_prompt_requested_ = false;
    update_focus_ = 1;
    ResetDataChartTooltip();
}

void AppRenderer::OpenUpdateDownload(HWND hwnd) {
    if (!update_notice_.Available()) return;
    // The GitHub Releases API result is notification metadata only. Hand all
    // installation work to the existing pinned-trust native updater, which
    // verifies the signed feed and installer before showing its own consent.
#ifndef WATCHMAN_RENDER_TESTING
    update_action_status_ = L"Starting the signed official updater...";
    update_download_error_.clear();
    CheckForUpdate(hwnd);
#else
    (void)hwnd;
    update_action_status_ = L"The signed updater is disabled in this isolated rendering test.";
#endif
}

bool AppRenderer::HandleUpdatePromptKeyDown(HWND hwnd, WPARAM key, bool shift) {
    if (!update_prompt_open_) return false;
    if (key == VK_ESCAPE) DismissUpdatePrompt();
    else if (key == VK_TAB) update_focus_ = (update_focus_ + (shift ? 2 : 1)) % 3;
    else if (key == VK_RETURN || key == VK_SPACE) {
        if (update_focus_ == 1) OpenUpdateDownload(hwnd); else DismissUpdatePrompt();
    }
    return true; // Never edit/navigate the page behind a modal notice.
}

void AppRenderer::DrawUpdatePrompt(D2D1_SIZE_F size) {
    using namespace WatchmanUpdates::Presentation;
    if (!update_notice_.Available()) return;
    const auto l = GetLayout(size.width, size.height);
    const auto rect = [](Rect r) { return D2D1::RectF(r.left, r.top, r.right, r.bottom); };
    ID2D1SolidColorBrush* scrim = nullptr;
    if (SUCCEEDED(render_target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.58f), &scrim))) {
        render_target_->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), scrim);
        scrim->Release();
    }
    ID2D1SolidColorBrush* surface = nullptr;
    auto color = palette_.CardBackground;
    color.a = 1;
    if (SUCCEEDED(render_target_->CreateSolidColorBrush(color, &surface))) {
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(rect(l.card), 22, 22), surface);
        surface->Release();
    }
    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(rect(l.card), 22, 22), brush_accent_dl_, 1);
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(rect(l.icon), 17, 17), brush_action_bg_);
    DrawVectorIcon(render_target_, VectorIcon::Download,
        D2D1::RectF(l.icon.left + 17, l.icon.top + 17, l.icon.right - 17, l.icon.bottom - 17), brush_tooltip_text_, 2.5f);
    const auto text = [&](const std::wstring& value, IDWriteTextFormat* format, Rect area, ID2D1Brush* brush) {
        render_target_->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format, rect(area), brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    text(L"WATCHMAN  /  NEW RELEASE", format_dashboard_label_, l.eyebrow, brush_accent_dl_);
    text(L"Update available", format_welcome_title_, l.title, brush_text_primary_);
    text(L"Version " + WatchmanUpdates::VersionText(update_notice_.Available()->version), format_update_version_, l.version, brush_text_primary_);
    text(L"Installed: " + WatchmanVersion::Label(), format_dashboard_note_, l.installed, brush_text_secondary_);
    SettingsText::Paragraph body;
    if (SUCCEEDED(SettingsText::CreateParagraph(dwrite_factory_, format_body_,
            update_download_error_.empty() ? body_text : update_download_error_.c_str(), l.body.right - l.body.left, body))) {
        render_target_->PushAxisAlignedClip(rect(l.body), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        render_target_->DrawTextLayout(D2D1::Point2F(l.body.left, l.body.top), body.layout.Get(), brush_text_secondary_);
        render_target_->PopAxisAlignedClip();
    }
    const auto button = [&](Rect area, const wchar_t* label, int focus, bool primary) {
        const auto round = D2D1::RoundedRect(rect(area), 10, 10);
        render_target_->FillRoundedRectangle(round, primary ? brush_action_bg_ :
            (Contains(area, static_cast<float>(mouse_x_), static_cast<float>(mouse_y_)) ? brush_hover_ : brush_popup_bg_));
        render_target_->DrawRoundedRectangle(round, update_focus_ == focus ? brush_accent_dl_ : brush_card_border_,
            update_focus_ == focus ? 2.0f : 1.0f);
        text(label, format_limit_button_, area, primary ? brush_tooltip_text_ : brush_text_primary_);
    };
    button(l.download, L"Download now", 1, true);
    button(l.later, L"Not now", 2, false);
    if (Contains(l.close, static_cast<float>(mouse_x_), static_cast<float>(mouse_y_)) || update_focus_ == 0)
        render_target_->DrawRoundedRectangle(D2D1::RoundedRect(rect(l.close), 10, 10), brush_accent_dl_, 1.5f);
    const float cx = (l.close.left + l.close.right) / 2, cy = (l.close.top + l.close.bottom) / 2;
    render_target_->DrawLine(D2D1::Point2F(cx - 6, cy - 6), D2D1::Point2F(cx + 6, cy + 6), brush_text_secondary_, 2);
    render_target_->DrawLine(D2D1::Point2F(cx + 6, cy - 6), D2D1::Point2F(cx - 6, cy + 6), brush_text_secondary_, 2);
    text(L"Keep monitoring. Update when you're ready.", format_dashboard_note_, l.note, brush_text_secondary_);
}

void AppRenderer::OfferWelcomeTour(bool interactive) {
    if (update_prompt_open_ || settings_open_ || speed_consent_open_ || speed_details_open_) return;
    if (welcome_tour_.Offer(interactive)) {
        welcome_scroll_ = welcome_scroll_max_ = 0;
        welcome_keyboard_focus_ = true;
        ResetDataChartTooltip();
    }
}

void AppRenderer::OpenWelcomeTour() {
    if (update_prompt_open_ || speed_consent_open_ || speed_details_open_) return;
    // Replay does not apply or discard an unsaved Settings draft.
    CommitVisibleSettingsEditors();
    if (data_limit_edit_) ShowWindow(data_limit_edit_, SW_HIDE);
    welcome_tour_.Open();
    welcome_scroll_ = welcome_scroll_max_ = 0;
    welcome_keyboard_focus_ = true;
    ResetDataChartTooltip();
}

void AppRenderer::ActivateWelcomeControl(WatchmanWelcome::Control control) {
    const int previous_page = welcome_tour_.PageIndex();
    if (welcome_tour_.Activate(control)) {
        Watchman::ScopedHKey key;
        const LONG opened = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Watchman", 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, key.Put(), nullptr);
        if (opened != ERROR_SUCCESS ||
            WatchmanWelcome::WriteSeenVersion(key.Get(), welcome_tour_.SeenVersion()) != ERROR_SUCCESS) {
            // No blocking error dialog in a quick introduction. The in-memory
            // marker still prevents another automatic offer this session.
            OutputDebugStringW(L"Watchman: could not remember completion of the welcome tour.\n");
        }
    }
    if (previous_page != welcome_tour_.PageIndex() || !welcome_tour_.IsOpen()) welcome_scroll_ = welcome_scroll_max_ = 0;
}

bool AppRenderer::HandleWelcomeTourKeyDown(WPARAM key, bool shift) {
    if (!welcome_tour_.IsOpen()) return false;
    welcome_keyboard_focus_ = true;
    switch (key) {
    case VK_ESCAPE: ActivateWelcomeControl(WatchmanWelcome::Control::Skip); break;
    case VK_TAB: welcome_tour_.MoveFocus(shift); break;
    case VK_RETURN:
    case VK_SPACE: ActivateWelcomeControl(welcome_tour_.Focused()); break;
    case VK_LEFT: ActivateWelcomeControl(WatchmanWelcome::Control::Back); break;
    case VK_RIGHT: ActivateWelcomeControl(WatchmanWelcome::Control::Next); break;
    case VK_UP:
    case VK_PRIOR: welcome_scroll_ = std::max(0.0f, welcome_scroll_ - 48.0f); break;
    case VK_DOWN:
    case VK_NEXT: welcome_scroll_ = std::min(welcome_scroll_max_, welcome_scroll_ + 48.0f); break;
    default: break;
    }
    return true; // Never pass tour keys through to the page behind it.
}

void AppRenderer::OnMouseWheel(short delta) {
    if (MainGraphicsUnavailable()) return;
    if (update_prompt_open_) return;
    if (welcome_tour_.IsOpen()) {
        welcome_scroll_ = std::clamp(welcome_scroll_ - static_cast<float>(delta) / WHEEL_DELTA * 48.0f,
            0.0f, welcome_scroll_max_);
        return;
    }
    ResetDataChartTooltip();
    if (delta == 0) {
        return;
    }
    if (speed_consent_open_ || speed_details_open_) {
        speed_details_scroll_ = std::clamp(speed_details_scroll_ -
            static_cast<float>(delta) / WHEEL_DELTA * 48.0f, 0.0f, speed_details_scroll_max_);
        return;
    }

    if (settings_open_) {
        float* scroll_offset = nullptr;
        float scroll_max = 0.0f;
        if (settings_section_ == 0) {
            scroll_offset = &settings_general_scroll_offset_;
            scroll_max = settings_general_scroll_max_;
        } else if (settings_section_ == 1) {
            scroll_offset = &settings_widgets_scroll_offset_;
            scroll_max = settings_widgets_scroll_max_;
        } else if (settings_section_ == 2) {
            scroll_offset = &settings_warnings_scroll_offset_;
            scroll_max = settings_warnings_scroll_max_;
        } else if (settings_section_ == 3) {
            scroll_offset = &settings_appearance_scroll_offset_;
            scroll_max = settings_appearance_scroll_max_;
        }
        if (settings_section_ == 3) {
            const SettingsModalLayout modal = GetSettingsModalLayout(render_size_);
            const auto layout = GetFloatingMonitorSettingsLayout(
                modal.x + modal.navigation_width + 20.0f, modal.x + modal.width - 18.0f,
                modal.y, modal.height, settings_appearance_scroll_offset_
            );
            settings_appearance_scroll_offset_ = layout.scroll_offset;
            settings_appearance_scroll_max_ = scroll_max = layout.scroll_max;
        }
        if (scroll_offset && scroll_max > 0.0f) {
            constexpr float kSettingsPixelsPerDetent = 52.0f;
            *scroll_offset = std::clamp(
                *scroll_offset -
                    static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * kSettingsPixelsPerDetent,
                0.0f,
                scroll_max
            );
        }
        return;
    }

    reveal_speed_test_ = false; // Explicit navigation always wins over following results.
    const float max_scroll = ContentScrollMax(active_tab_);
    if (max_scroll <= 0.0f) {
        return;
    }

    const int index = static_cast<int>(active_tab_);
    constexpr float kPixelsPerDetent = 64.0f;
    const float previous_scroll = content_scroll_offsets_[index];
    content_scroll_offsets_[index] = std::clamp(
        content_scroll_offsets_[index] -
            static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * kPixelsPerDetent,
        0.0f,
        max_scroll
    );
    if (active_tab_ == ActiveTab::Apps && mouse_y_ > -10000) {
        mouse_y_ += static_cast<int>(std::lround(content_scroll_offsets_[index] - previous_scroll));
    }

    if (data_limit_edit_ && IsWindowVisible(data_limit_edit_)) {
        ShowWindow(data_limit_edit_, SW_HIDE);
    }
}

bool AppRenderer::HandleSettingsNumericKeyDown(WPARAM key) {
    if (speed_consent_open_ || speed_details_open_) {
        switch (key) {
        case VK_ESCAPE: speed_consent_open_ = false; speed_details_open_ = false; break;
        case VK_UP:
        case VK_PRIOR: speed_details_scroll_ = std::max(0.0f, speed_details_scroll_ - 48.0f); break;
        case VK_DOWN:
        case VK_NEXT: speed_details_scroll_ = std::min(speed_details_scroll_max_, speed_details_scroll_ + 48.0f); break;
        case VK_HOME: speed_details_scroll_ = 0.0f; break;
        case VK_END: speed_details_scroll_ = speed_details_scroll_max_; break;
        default: break;
        }
        // Enter never starts a data-intensive test without a deliberate consent click.
        return true;
    }
    if (!settings_open_ || settings_numeric_field_ == SettingsNumericField::None) {
        return false;
    }

    switch (key) {
    case VK_RETURN:
    case VK_TAB:
        CommitSettingsNumericInput();
        return true;
    case VK_ESCAPE:
        CancelSettingsNumericInput();
        return true;
    case VK_DELETE:
        if (settings_numeric_select_all_) {
            settings_numeric_text_.clear();
            settings_numeric_select_all_ = false;
        } else if (!settings_numeric_text_.empty()) {
            settings_numeric_text_.pop_back();
        }
        return true;
    default:
        return false;
    }
}

bool AppRenderer::HandleSettingsNumericCharacter(wchar_t character) {
    if (!settings_open_ || settings_numeric_field_ == SettingsNumericField::None) {
        return false;
    }

    if (character == L'\b') {
        if (settings_numeric_select_all_) {
            settings_numeric_text_.clear();
            settings_numeric_select_all_ = false;
        } else if (!settings_numeric_text_.empty()) {
            settings_numeric_text_.pop_back();
        }
        return true;
    }

    const bool allow_decimal = settings_numeric_field_ == SettingsNumericField::MonthlyDataLimit;
    const bool is_decimal = (character == L'.' || character == L',') && allow_decimal;
    const bool is_digit = character >= L'0' && character <= L'9';
    if (!is_digit && !is_decimal) {
        return true;
    }

    if (is_decimal && settings_numeric_text_.find(L'.') != std::wstring::npos) {
        return true;
    }
    if (settings_numeric_select_all_) {
        settings_numeric_text_.clear();
        settings_numeric_select_all_ = false;
    }
    if (settings_numeric_text_.size() < 10) {
        settings_numeric_text_.push_back(is_decimal ? L'.' : character);
    }
    return true;
}

void AppRenderer::DrawContentScrollbar(D2D1_SIZE_F rt_size) {
    const float max_scroll = ContentScrollMax(active_tab_);
    if (max_scroll <= 1.0f) {
        return;
    }

    const float viewport_top = ContentViewportTop();
    const float viewport_bottom = ContentViewportBottom();
    const float viewport_height = viewport_bottom - viewport_top;
    const float content_height = std::max(viewport_height, content_bottoms_[static_cast<int>(active_tab_)] - viewport_top);
    const float track_top = viewport_top + 8.0f;
    const float track_bottom = viewport_bottom - 8.0f;
    const float track_height = std::max(1.0f, track_bottom - track_top);
    const float min_thumb_height = std::min(28.0f, track_height);
    const float thumb_height = std::clamp(
        track_height * viewport_height / content_height,
        min_thumb_height,
        track_height
    );
    const float thumb_top = track_top +
        (track_height - thumb_height) * CurrentContentScroll() / max_scroll;
    const D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(
        D2D1::RectF(rt_size.width - 7.0f, thumb_top, rt_size.width - 3.0f, thumb_top + thumb_height),
        2.0f,
        2.0f
    );
    ID2D1SolidColorBrush* thumb_brush = nullptr;
    const D2D1_COLOR_F thumb_color = effective_theme_ == ThemeMode::Light
        ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.32f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f);
    if (SUCCEEDED(render_target_->CreateSolidColorBrush(thumb_color, &thumb_brush))) {
        render_target_->FillRoundedRectangle(thumb, thumb_brush);
        thumb_brush->Release();
    }
}

void AppRenderer::RestartContentAnimation() {
    content_animation_started_ms_ = GetTickCount64();
    anim_content_opacity_ = 0.0f;
    anim_content_translate_y_ = 10.0f;
}

void AppRenderer::UpdateContentAnimation() {
    if (content_animation_started_ms_ == 0) {
        return;
    }

    const float elapsed_ms = static_cast<float>(GetTickCount64() - content_animation_started_ms_);
    const float progress = std::clamp(elapsed_ms / 180.0f, 0.0f, 1.0f);
    const float eased = EaseOutCubic(progress);
    anim_content_opacity_ = eased;
    anim_content_translate_y_ = (1.0f - eased) * 10.0f;
    if (progress >= 1.0f) {
        anim_content_opacity_ = 1.0f;
        anim_content_translate_y_ = 0.0f;
        content_animation_started_ms_ = 0;
    }
}

void AppRenderer::RestartDataChartAnimation() {
    data_chart_animation_started_ms_ = GetTickCount64();
    data_chart_progress_ = 0.0f;
}

void AppRenderer::SetDataPickerOpen(bool open) {
    if (data_picker_open_ == open) {
        return;
    }

    data_picker_open_ = open;
    data_picker_animation_from_ = data_picker_visibility_;
    data_picker_animation_started_ms_ = GetTickCount64();
    data_picker_animating_ = true;
}

void AppRenderer::UpdateDataPickerAnimation() {
    if (!data_picker_animating_) {
        return;
    }

    const float elapsed_ms = static_cast<float>(GetTickCount64() - data_picker_animation_started_ms_);
    const float progress = std::clamp(elapsed_ms / 180.0f, 0.0f, 1.0f);
    const float target = data_picker_open_ ? 1.0f : 0.0f;
    data_picker_visibility_ = data_picker_animation_from_ +
        (target - data_picker_animation_from_) * EaseOutCubic(progress);
    if (progress >= 1.0f) {
        data_picker_visibility_ = target;
        data_picker_animating_ = false;
    }
}

void AppRenderer::UpdateDataChartHover() {
    hovered_data_chart_hitbox_ = -1;
    if (active_tab_ != ActiveTab::Data || mouse_x_ < 0 || mouse_y_ < 0) {
        return;
    }

    for (size_t index = 0; index < data_chart_hitboxes_.size(); ++index) {
        const D2D1_RECT_F& rect = data_chart_hitboxes_[index].rect;
        if (mouse_x_ >= rect.left && mouse_x_ <= rect.right &&
            mouse_y_ >= rect.top && mouse_y_ <= rect.bottom) {
            hovered_data_chart_hitbox_ = static_cast<int>(index);
            return;
        }
    }
}

void AppRenderer::ResetDataChartTooltip() {
    hovered_data_chart_hitbox_ = -1;
    displayed_data_chart_hitbox_ = -1;
    data_tooltip_x_ = 0.0f;
    data_tooltip_y_ = 0.0f;
    data_tooltip_position_ready_ = false;
    data_tooltip_last_update_ms_ = 0;
    data_tooltip_last_hover_ms_ = 0;
}

void AppRenderer::ResetDataHistoryToCurrentCalendar() {
    time_t current_time = time(nullptr);
    tm current{};
    localtime_s(&current, &current_time);
    data_history_year_ = current.tm_year + 1900;
    data_history_month_ = current.tm_mon + 1;
    data_picker_page_ = 0;
}

void AppRenderer::SetDataRangeIndex(int index) {
    if (index < 0 || index > 3 || data_range_idx_ == index) {
        return;
    }

    data_range_idx_ = index;
    if (index == 2 || index == 3) {
        // Like the Tauri filter buttons, entering a calendar view begins at
        // the live month/year. Picker navigation remains temporary.
        ResetDataHistoryToCurrentCalendar();
    }
    SetDataPickerOpen(false);
    ResetDataChartTooltip();
    RestartDataChartAnimation();
}

void AppRenderer::SetToolsExportPeriodIndex(int index) {
    const int clamped = std::clamp(index, 0, 3);
    if (tools_export_period_index_ == clamped) {
        return;
    }
    tools_export_period_index_ = clamped;
    tools_export_status_.clear();
    tools_export_status_success_ = false;
}

void AppRenderer::OnMouseMove(int x, int y) {
    mouse_x_ = x;
    mouse_y_ = y;
    if (update_prompt_open_ || welcome_tour_.IsOpen()) {
        hovered_titlebar_btn_ = -1;
        hovered_tab_item_ = -1;
        hover_settings_close_ = false;
        return;
    }

    // 1. Title bar buttons hover checks
    hovered_titlebar_btn_ = GetTitlebarButtonAt(
        std::max(render_size_.width, UIStyle::MIN_WINDOW_WIDTH),
        x,
        y
    );

    // 2. Tab Bar items hover
    hovered_tab_item_ = -1;
    if (y >= static_cast<int>(UIStyle::TITLEBAR_HEIGHT) && y <= static_cast<int>(UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT)) {
        const float width = std::max(render_size_.width, UIStyle::MIN_WINDOW_WIDTH);
        hovered_tab_item_ = GetTabbarItemAt(width, x);
    }

    // 3. Settings modal close hover
    hover_settings_close_ = false;
    if (settings_open_) {
        const SettingsModalLayout modal = GetSettingsModalLayout(render_size_);
        if (x >= modal.x + modal.width - 40.0f && x <= modal.x + modal.width - 15.0f &&
            y >= modal.y + 15.0f && y <= modal.y + 40.0f) {
            hover_settings_close_ = true;
        }
    }

    if (!settings_open_ && !speed_consent_open_ && !speed_details_open_) {
        if (y >= ContentViewportTop() && y < ContentViewportBottom()) {
            mouse_y_ = static_cast<int>(static_cast<float>(y) + CurrentContentScroll());
        } else {
            mouse_y_ = -10000;
        }
    }

    UpdateDataChartHover();
}

bool AppRenderer::IsRepeatablePrimaryControl(int x, int y) const {
    if (update_prompt_open_ || welcome_tour_.IsOpen() || speed_consent_open_ || speed_details_open_) return false;
    if (!settings_open_) {
        return MainContentLayout::IsRepeatableTitlebarButton(GetTitlebarButtonAt(
            std::max(render_size_.width, UIStyle::MIN_WINDOW_WIDTH), x, y));
    }
    if (settings_section_ != 3) return false;
    const auto modal = GetSettingsModalLayout(render_size_);
    const auto page = GetFloatingMonitorSettingsLayout(modal.x + modal.navigation_width + 20.0f,
        modal.x + modal.width - 18.0f, modal.y, modal.height, settings_appearance_scroll_offset_);
    return y >= page.view_top && y <= page.view_bottom &&
        (IsPointInside(page.font_minus, x, y) || IsPointInside(page.font_plus, x, y) ||
         IsPointInside(page.opacity_minus, x, y) || IsPointInside(page.opacity_plus, x, y));
}

bool AppRenderer::IsTitlebarDragRegion(int x, int y) const {
    if (FAILED(main_initialization_error_))
        return y >= 0 && y < 48 && GetTitlebarButtonAt(render_size_.width, x, y) < 4;
    if (update_prompt_open_ || welcome_tour_.IsOpen() || settings_open_ || speed_consent_open_ || speed_details_open_ ||
        y < 0 || y > static_cast<int>(UIStyle::TITLEBAR_HEIGHT)) {
        return false;
    }
    return GetTitlebarButtonAt(
        std::max(render_size_.width, UIStyle::MIN_WINDOW_WIDTH),
        x,
        y
    ) == -1;
}

void AppRenderer::OnLButtonDown(HWND hwnd, int x, int y, WatchmanEngine& engine) {
    if (FAILED(main_initialization_error_)) {
        const int button = GetTitlebarButtonAt(render_size_.width, x, y);
        if (button == 4) ShowWindow(hwnd, SW_MINIMIZE);
        else if (button == 5) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        else if (x >= 24 && x < 174 && y >= 240 && y < 274)
            PostMessageW(hwnd, WatchmanWindowLifecycle::ExitMessage, 0, 0);
        return; // Never activate controls hidden behind the failure surface.
    }
    if (update_prompt_open_) {
        const auto layout = WatchmanUpdates::Presentation::GetLayout(render_size_.width, render_size_.height);
        if (WatchmanUpdates::Presentation::Contains(layout.close, static_cast<float>(x), static_cast<float>(y)) ||
            WatchmanUpdates::Presentation::Contains(layout.later, static_cast<float>(x), static_cast<float>(y))) DismissUpdatePrompt();
        else if (WatchmanUpdates::Presentation::Contains(layout.download, static_cast<float>(x), static_cast<float>(y))) OpenUpdateDownload(hwnd);
        SetFocus(hwnd);
        return; // Modal: no click-through, including the titlebar controls.
    }
    if (!settings_open_ && !welcome_tour_.IsOpen() && !speed_consent_open_ && !speed_details_open_ && update_footer_visible_ &&
        WatchmanUpdates::Presentation::Contains(WatchmanUpdates::Presentation::FooterLink(render_size_.width, render_size_.height),
            static_cast<float>(x), static_cast<float>(y))) {
        OpenUpdateDownload(hwnd);
        return;
    }
    if (welcome_tour_.IsOpen()) {
        welcome_keyboard_focus_ = false;
        const auto layout = WatchmanWelcome::GetLayout(render_size_.width, render_size_.height);
        ActivateWelcomeControl(WatchmanWelcome::HitTest(layout, welcome_tour_.PageIndex(),
            static_cast<float>(x), static_cast<float>(y)));
        SetFocus(hwnd);
        return;
    }
    if (speed_consent_open_ || speed_details_open_) {
        const auto dialog = SpeedTestPresentation::GetDialog(render_size_.width, render_size_.height);
        if (IsPointInside(dialog.cancel, x, y) || !IsPointInside(dialog.card, x, y)) {
            speed_consent_open_ = false;
            speed_details_open_ = false;
        } else if (speed_consent_open_ && IsPointInside(dialog.confirm, x, y)) {
            speed_consent_open_ = false;
            if (engine.IsReady() && !engine.IsSpeedTestRunning()) {
                engine.RunSpeedTest();
                reveal_speed_test_ = true;
            }
        } else if (speed_consent_open_ && IsPointInside(dialog.privacy, x, y)) {
            ShellExecuteW(hwnd, L"open", L"https://www.measurementlab.net/privacy/", nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    // 1. Settings Close click
    if (settings_open_) {
        const SettingsModalLayout modal = GetSettingsModalLayout(render_size_);
        const float mx = modal.x;
        const float my = modal.y;
        const float mw = modal.width;
        const float mh = modal.height;

        // Click outside the settings card closes it
        if (x < mx || x > mx + mw || y < my || y > my + mh) {
            HideSettingsEditors();
            settings_open_ = false;
            network_adapter_picker_requested_ = false;
            return;
        }

        if (WatchmanWelcome::Contains(WatchmanWelcome::ReplayButton(mx, my, mw),
                static_cast<float>(x), static_cast<float>(y))) {
            OpenWelcomeTour();
            SetFocus(hwnd);
            return;
        }

        // Close button click
        if (x >= mx + mw - 40.0f && x <= mx + mw - 15.0f && y >= my + 15.0f && y <= my + 40.0f) {
            HideSettingsEditors();
            settings_open_ = false;
            network_adapter_picker_requested_ = false;
            return;
        }

        {
            const float nav_left = mx + 12.0f;
            const float nav_right = mx + modal.navigation_width - 8.0f;
            const float nav_top = my + 84.0f;
            const float content_left = mx + modal.navigation_width + 26.0f;
            const float content_right = mx + mw - 18.0f;
            const float footer_top = my + mh - 58.0f;
            const auto hit_row = [&](float top, float height) {
                return x >= content_left && x <= content_right && y >= top && y <= top + height;
            };

            for (int section = 0; section < 4; ++section) {
                const float row_top = nav_top + static_cast<float>(section) * 52.0f;
                if (x >= nav_left && x <= nav_right && y >= row_top && y <= row_top + 44.0f) {
                    CommitVisibleSettingsEditors();
                    settings_section_ = section;
                    settings_general_scroll_offset_ = 0.0f;
                    settings_widgets_scroll_offset_ = 0.0f;
                    settings_warnings_scroll_offset_ = 0.0f;
                    settings_appearance_scroll_offset_ = 0.0f;
                    return;
                }
            }

            const float btn_h = 34.0f;
            const float btn_y = footer_top + 10.0f;
            const bool compact_footer = content_right - content_left < 320.0f;
            const float btn_gap = compact_footer ? 8.0f : 12.0f;
            const float rec_w = compact_footer ? 94.0f : 118.0f;
            const float undo_w = compact_footer ? 60.0f : 72.0f;
            const float save_w = content_right - (content_left + rec_w + btn_gap + undo_w + btn_gap);
            const D2D1_RECT_F recommended_rect = D2D1::RectF(content_left, btn_y, content_left + rec_w, btn_y + btn_h);
            const D2D1_RECT_F undo_rect = D2D1::RectF(content_left + rec_w + btn_gap, btn_y, content_left + rec_w + btn_gap + undo_w, btn_y + btn_h);
            const D2D1_RECT_F save_rect = D2D1::RectF(content_right - save_w, btn_y, content_right, btn_y + btn_h);
            if (IsPointInside(recommended_rect, x, y)) {
                UseRecommendedPreferences();
                return;
            }
            if ((preferences_dirty_ || settings_numeric_field_ != SettingsNumericField::None) && IsPointInside(undo_rect, x, y)) {
                RestorePreferencesDraft();
                return;
            }
            if ((preferences_dirty_ || settings_numeric_field_ != SettingsNumericField::None) && IsPointInside(save_rect, x, y)) {
                CommitPreferencesDraft();
                return;
            }

            // Changing sections, toggles, or pickers should commit a typed
            // number before the next interaction uses the settings draft.
            CommitVisibleSettingsEditors();

            if (settings_section_ == 0) {
                // Must match the grid-based positions in DrawSettingsModal.
                if (y < my + 146.0f || y > footer_top - 14.0f) return;
                const float kPad = 20.0f;
                const float kCardGap = 12.0f;
                const float c0_top = my - settings_general_scroll_offset_ + 148.0f;
                const float c0_label = c0_top + 14.0f;
                const float c0_r1 = c0_label + 24.0f;
                const float c0_d1 = c0_r1 + 50.0f;
                const float c0_r2 = c0_d1 + 4.0f;
                const float c0_d2 = c0_r2 + 50.0f;
                const float c0_r3 = c0_d2 + 4.0f;
                const float c0_bot = c0_r3 + 50.0f + 10.0f;
                const float c1_top = c0_bot + kCardGap;
                const float c1_label = c1_top + 14.0f;
                const float c1_input_top = c1_label + 22.0f;
                const float c1_input_bot = c1_input_top + 32.0f;
                const float c1_bot = c1_input_bot + 12.0f;
                const float c2_top = c1_bot + kCardGap;
                const auto theme_picker = ThemePickerLayout::Get(content_left, content_right, c2_top);
                const int theme_hit = ThemePickerLayout::HitTest(theme_picker, static_cast<float>(x), static_cast<float>(y));
                const float c2_bot = theme_picker.bottom;
                const float c3_top = c2_bot + kCardGap;
                const float c3_label = c3_top + 14.0f;
                const float c3_dropdown = c3_label + 24.0f;

                if (hit_row(c0_r1, 46.0f)) {
                    preferences_draft_.autostart_enabled = !preferences_draft_.autostart_enabled;
                    MarkPreferencesDirty();
                } else if (hit_row(c0_r2, 46.0f)) {
                    preferences_draft_.use_bits_per_second = !preferences_draft_.use_bits_per_second;
                    MarkPreferencesDirty();
                } else if (hit_row(c0_r3, 46.0f)) {
                    preferences_draft_.hide_hardware_gauges = !preferences_draft_.hide_hardware_gauges;
                    MarkPreferencesDirty();
                } else if (x >= content_left + kPad && x <= content_right - kPad && y >= c1_input_top && y <= c1_input_bot) {
                    const uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
                    const int value = static_cast<int>(std::min<uint64_t>(preferences_draft_.monthly_data_limit_bytes / gibibyte, 1000000));
                    SetFocus(hwnd);
                    BeginSettingsNumericInput(SettingsNumericField::MonthlyDataLimit, value);
                } else if (theme_hit >= 0) {
                    preferences_draft_.theme = static_cast<ThemeMode>(theme_hit);
                    MarkPreferencesDirty();
                } else if (x >= content_left && x <= content_right && y >= c3_dropdown && y <= c3_dropdown + 38.0f) {
                    CommitVisibleSettingsEditors();
                    settings_network_adapters_ = engine.GetNetworkAdapters();
                    network_adapter_picker_requested_ = true;
                }
                return;
            }

            if (settings_section_ == 1) {
                const float widgets_content_left = mx + modal.navigation_width + 20.0f;
                const float widgets_content_right = mx + mw - 18.0f;
                const auto unscrolled = GetWidgetsSettingsLayout(widgets_content_left, widgets_content_right, my);
                settings_widgets_scroll_max_ = std::max(0.0f, unscrolled.fl_bot - (footer_top - 14.0f));
                settings_widgets_scroll_offset_ = std::clamp(settings_widgets_scroll_offset_, 0.0f, settings_widgets_scroll_max_);
                if (y < my + 76.0f || y > footer_top - 14.0f) return;
                const WidgetsSettingsLayout w = GetWidgetsSettingsLayout(
                    widgets_content_left,
                    widgets_content_right,
                    my - settings_widgets_scroll_offset_
                );
                const auto hit_widgets_row = [&](float top, float height) {
                    return x >= w.card_left && x <= w.card_right && y >= top && y <= top + height;
                };
                if (hit_widgets_row(w.tb_r1, w.row_h)) {
                    preferences_draft_.widget_enabled = !preferences_draft_.widget_enabled;
                } else if (hit_widgets_row(w.tb_mode_row[0], w.tb_mode_row_h)) {
                    preferences_draft_.widget_mode = TaskbarWidgetMode::Network;
                } else if (hit_widgets_row(w.tb_mode_row[1], w.tb_mode_row_h)) {
                    preferences_draft_.widget_mode = TaskbarWidgetMode::System;
                } else if (hit_widgets_row(w.tb_mode_row[2], w.tb_mode_row_h)) {
                    preferences_draft_.widget_mode = TaskbarWidgetMode::All;
                } else if (hit_widgets_row(w.fl_row[0], w.fl_row_h)) {
                    preferences_draft_.floating_monitor_enabled = !preferences_draft_.floating_monitor_enabled;
                } else if (hit_widgets_row(w.fl_row[1], w.fl_row_h)) {
                    preferences_draft_.floating_monitor_always_on_top = !preferences_draft_.floating_monitor_always_on_top;
                } else if (hit_widgets_row(w.fl_row[2], w.fl_row_h)) {
                    preferences_draft_.floating_monitor_click_through = !preferences_draft_.floating_monitor_click_through;
                } else if (hit_widgets_row(w.fl_details_top, w.fl_details_bot - w.fl_details_top)) {
                    settings_section_ = 3;
                    settings_appearance_scroll_offset_ = 0.0f;
                    return;
                } else {
                    return;
                }
                MarkPreferencesDirty();
                return;
            }

            if (settings_section_ == 2) {
                const float warnings_content_left = mx + modal.navigation_width + 20.0f;
                const float warnings_content_right = mx + mw - 18.0f;
                const float warnings_view_bottom = footer_top - 14.0f;
                if (y < my + 148.0f || y > warnings_view_bottom) return;
                const WarningsSettingsLayout unscrolled = GetMeasuredWarningsLayout(
                    warnings_content_left, warnings_content_right, my
                );
                settings_warnings_scroll_max_ = std::max(0.0f, unscrolled.info_bot - warnings_view_bottom);
                settings_warnings_scroll_offset_ = std::clamp(
                    settings_warnings_scroll_offset_, 0.0f, settings_warnings_scroll_max_
                );
                const WarningsSettingsLayout w = GetMeasuredWarningsLayout(
                    warnings_content_left,
                    warnings_content_right,
                    my, settings_warnings_scroll_offset_
                );

                if (x >= w.card_left && x <= w.card_right && y >= w.alert_top && y <= w.alert_bot) {
                    preferences_draft_.alerts_enabled = !preferences_draft_.alerts_enabled;
                    MarkPreferencesDirty();
                    return;
                }

                bool* enabled_values[] = {
                    &preferences_draft_.traffic_warning_enabled,
                    &preferences_draft_.memory_warning_enabled,
                    &preferences_draft_.cpu_temp_warning_enabled,
                    &preferences_draft_.gpu_temp_warning_enabled,
                    &preferences_draft_.disk_temp_warning_enabled,
                    &preferences_draft_.mainboard_temp_warning_enabled
                };
                int* threshold_values[] = {
                    &preferences_draft_.traffic_warning_threshold,
                    &preferences_draft_.memory_warning_threshold,
                    &preferences_draft_.cpu_temp_warning_threshold,
                    &preferences_draft_.gpu_temp_warning_threshold,
                    &preferences_draft_.disk_temp_warning_threshold,
                    &preferences_draft_.mainboard_temp_warning_threshold
                };
                const SettingsNumericField numeric_fields[] = {
                    SettingsNumericField::TrafficWarning,
                    SettingsNumericField::MemoryWarning,
                    SettingsNumericField::CpuTemperatureWarning,
                    SettingsNumericField::GpuTemperatureWarning,
                    SettingsNumericField::DiskTemperatureWarning,
                    SettingsNumericField::MainboardTemperatureWarning
                };
                for (int index = 0; index < w.rule_count; ++index) {
                    const float row_top = w.row[index];
                    if (x < w.card_left || x > w.card_right || y < row_top || y > row_top + w.row_h) {
                        continue;
                    }
                    if (x >= w.toggle_left && x <= w.toggle_right && y <= row_top + 36.0f) {
                        *enabled_values[index] = !*enabled_values[index];
                        MarkPreferencesDirty();
                    } else if (x >= w.input_left && x <= w.input_right && y >= row_top + 48.0f && y <= row_top + 80.0f) {
                        SetFocus(hwnd);
                        BeginSettingsNumericInput(numeric_fields[index], *threshold_values[index]);
                    } else if (index == 0 && x >= w.unit_left && x <= w.unit_right && y >= row_top + 48.0f && y <= row_top + 80.0f) {
                        preferences_draft_.traffic_warning_gigabytes = !preferences_draft_.traffic_warning_gigabytes;
                        MarkPreferencesDirty();
                    }
                    return;
                }
                return;
            }

            if (settings_section_ == 3) {
                const auto layout = GetFloatingMonitorSettingsLayout(
                    mx + modal.navigation_width + 20.0f, mx + mw - 18.0f,
                    my, mh, settings_appearance_scroll_offset_
                );
                settings_appearance_scroll_offset_ = layout.scroll_offset;
                settings_appearance_scroll_max_ = layout.scroll_max;
                if (y < layout.view_top || y > layout.view_bottom) return;

                if (IsPointInside(layout.cpu_row, x, y)) {
                    preferences_draft_.floating_monitor_show_cpu = !preferences_draft_.floating_monitor_show_cpu;
                    MarkPreferencesDirty();
                    return;
                }
                if (IsPointInside(layout.memory_row, x, y)) {
                    preferences_draft_.floating_monitor_show_memory = !preferences_draft_.floating_monitor_show_memory;
                    MarkPreferencesDirty();
                    return;
                }
                if (IsPointInside(layout.lock_row, x, y)) {
                    preferences_draft_.floating_monitor_lock_position = !preferences_draft_.floating_monitor_lock_position;
                    MarkPreferencesDirty();
                    return;
                }
                if (IsPointInside(layout.transparent_row, x, y)) {
                    preferences_draft_.floating_monitor_transparent_background = !preferences_draft_.floating_monitor_transparent_background;
                    MarkPreferencesDirty();
                    return;
                }
                if (IsPointInside(layout.opacity_minus, x, y) || IsPointInside(layout.opacity_plus, x, y)) {
                    const int delta = IsPointInside(layout.opacity_minus, x, y) ? -5 : 5;
                    preferences_draft_.floating_monitor_opacity = FloatingMonitorLayout::ClampOpacity(preferences_draft_.floating_monitor_opacity + delta);
                    MarkPreferencesDirty();
                    return;
                }
                if (IsPointInside(layout.reset_position, x, y)) {
                    floating_monitor_reset_position_requested_ = true;
                    MarkPreferencesDirty();
                    return;
                }

                const auto defaults = GetFloatingMonitorDefaultColors();
                COLORREF* colors[] = {
                    &preferences_draft_.floating_monitor_text_color,
                    &preferences_draft_.floating_monitor_upload_color,
                    &preferences_draft_.floating_monitor_download_color,
                    &preferences_draft_.floating_monitor_cpu_color,
                    &preferences_draft_.floating_monitor_memory_color
                };
                for (size_t index = 0; index < layout.color_count; ++index) {
                    if (IsPointInside(layout.color_controls[index], x, y)) {
                        ChooseAppearanceColor(hwnd, *colors[index], defaults[index]);
                        return;
                    }
                }
                for (size_t index = 0; index < layout.weight_controls.size(); ++index) {
                    if (IsPointInside(layout.weight_controls[index], x, y)) {
                        const auto weight = FloatingMonitorText::weights[index];
                        if (preferences_draft_.floating_monitor_text_weight != weight) {
                            preferences_draft_.floating_monitor_text_weight = weight;
                            MarkPreferencesDirty();
                        }
                        return;
                    }
                }
                if (IsPointInside(layout.font_minus, x, y) || IsPointInside(layout.font_plus, x, y)) {
                    const int delta = IsPointInside(layout.font_minus, x, y) ? -1 : 1;
                    preferences_draft_.floating_monitor_font_size = FloatingMonitorLayout::ClampFontSize(preferences_draft_.floating_monitor_font_size + delta);
                    MarkPreferencesDirty();
                }
                return;
            }
        }

        const float nav_left = mx + 14.0f;
        const float nav_right = mx + modal.navigation_width - 10.0f;
        const float nav_top = my + 82.0f;
        for (int section = 0; section < 4; ++section) {
            const float row_top = nav_top + static_cast<float>(section) * 40.0f;
            if (x >= nav_left && x <= nav_right && y >= row_top && y <= row_top + 34.0f) {
                settings_section_ = section;
                return;
            }
        }

        const float content_left = mx + modal.navigation_width + 20.0f;
        const float content_right = mx + mw - 22.0f;
        const auto hit_row = [&](float top) {
            return x >= content_left && x <= content_right && y >= top && y <= top + 34.0f;
        };

        if (settings_section_ == 0) {
            if (hit_row(my + 145.0f)) {
                autostart_enabled_ = !autostart_enabled_;
            }

            const float theme_top = my + 240.0f;
            if (y >= theme_top && y <= theme_top + 28.0f) {
                const float option_width = (content_right - content_left) / UIStyle::theme_count;
                for (int i = 0; i < UIStyle::theme_count; ++i) {
                    const float option_left = content_left + option_width * static_cast<float>(i);
                    if (x >= option_left && x <= option_left + option_width) {
                        SetTheme(static_cast<ThemeMode>(i));
                        return;
                    }
                }
            }

            const float adapter_row_y = my + 319.0f;
            if (x >= content_left && x <= content_right && y >= adapter_row_y && y <= adapter_row_y + 36.0f) {
                settings_network_adapters_ = engine.GetNetworkAdapters();
                network_adapter_picker_requested_ = true;
                return;
            }
        } else if (settings_section_ == 1) {
            if (hit_row(my + 145.0f)) {
                widget_enabled_ = !widget_enabled_;
            } else if (hit_row(my + 201.0f)) {
                SetWidgetMode(TaskbarWidgetMode::Network);
            } else if (hit_row(my + 257.0f)) {
                SetWidgetMode(TaskbarWidgetMode::System);
            } else if (hit_row(my + 313.0f)) {
                SetWidgetMode(TaskbarWidgetMode::All);
            } else {
                const float adapter_row_y = my + 415.0f;
                if (x >= content_left && x <= content_right && y >= adapter_row_y && y <= adapter_row_y + 36.0f) {
                    settings_network_adapters_ = engine.GetNetworkAdapters();
                    network_adapter_picker_requested_ = true;
                    return;
                }
            }
        } else if (settings_section_ == 2 && hit_row(my + 145.0f)) {
            alerts_enabled_ = !alerts_enabled_;
        }
        return;
    }

    // 2. Title bar buttons clicks
    if (y >= 0 && y <= static_cast<int>(UIStyle::TITLEBAR_HEIGHT)) {
        const int titlebar_button = GetTitlebarButtonAt(
            std::max(render_size_.width, UIStyle::MIN_WINDOW_WIDTH),
            x,
            y
        );
        if (titlebar_button == 0) { // Feedback
            ShellExecuteW(nullptr, L"open", L"https://tally.so/r/81WZrx", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (titlebar_button == 1) { // Globe (website)
            ShellExecuteW(nullptr, L"open", L"https://watchman.blinkeye.app/", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (titlebar_button == 2) { // Gear (settings modal)
            if (data_limit_edit_) {
                ShowWindow(data_limit_edit_, SW_HIDE);
            }
            settings_open_ = true;
            settings_section_ = 0;
            settings_widgets_scroll_offset_ = 0.0f;
            settings_widgets_scroll_max_ = 0.0f;
            settings_warnings_scroll_offset_ = 0.0f;
            settings_warnings_scroll_max_ = 0.0f;
            settings_appearance_scroll_offset_ = 0.0f;
            settings_appearance_scroll_max_ = 0.0f;
            settings_network_adapters_ = engine.GetNetworkAdapters();
            BeginSettingsSession();
        } else if (titlebar_button == 3) { // Theme
            SetTheme(UIStyle::ThemeOnQuickToggle(effective_theme_));
        } else if (titlebar_button == 4) { // Minimize
            ShowWindow(hwnd, SW_MINIMIZE);
        } else if (titlebar_button == 5) { // Close
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return;
    }

    // 3. Tab clicks
    if (y >= static_cast<int>(UIStyle::TITLEBAR_HEIGHT) && y <= static_cast<int>(UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT)) {
        const int clicked_tab = GetTabbarItemAt(
            std::max(render_size_.width, UIStyle::MIN_WINDOW_WIDTH),
            x
        );
        if (clicked_tab >= 0) {
            const ActiveTab next_tab = static_cast<ActiveTab>(clicked_tab);
            SetActiveTab(next_tab);
            if (next_tab == ActiveTab::Network) {
                settings_network_adapters_ = engine.GetNetworkAdapters();
            }
        }
        return;
    }

    if (y < ContentViewportTop() || y >= ContentViewportBottom()) {
        return;
    }
    y += static_cast<int>(CurrentContentScroll());

    // 4. Tab specific contents clicks
    if (active_tab_ == ActiveTab::Dashboard) {
        const DashboardLayout layout = GetDashboardLayout(
            render_size_.width,
            dashboard_network_details_open_,
            dashboard_system_info_open_,
            hide_hardware_gauges_
        );
        const auto contains = [x, y](const D2D1_RECT_F& rect) {
            return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
        };

        const D2D1_RECT_F reset_rect = D2D1::RectF(
            layout.start_x + layout.content_w - 122.0f,
            layout.session_y + 13.0f,
            layout.start_x + layout.content_w - 16.0f,
            layout.session_y + 41.0f
        );
        if (contains(reset_rect)) {
            engine.ResetSession();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        const float chip_w = DashboardUsageChipWidth(dashboard_usage_period_idx_);
        const D2D1_RECT_F chip_rect = D2D1::RectF(
            layout.start_x + layout.content_w - chip_w - 16.0f,
            layout.snapshot_y + 14.0f,
            layout.start_x + layout.content_w - 16.0f,
            layout.snapshot_y + 42.0f
        );
        if (contains(chip_rect)) {
            dashboard_usage_period_idx_ = (dashboard_usage_period_idx_ + 1) % 3;
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        const D2D1_RECT_F network_toggle = D2D1::RectF(
            layout.start_x,
            layout.network_y,
            layout.start_x + layout.content_w,
            layout.network_y + 56.0f
        );
        if (contains(network_toggle)) {
            dashboard_network_details_open_ = !dashboard_network_details_open_;
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        const D2D1_RECT_F system_toggle = D2D1::RectF(
            layout.start_x,
            layout.system_y,
            layout.start_x + layout.content_w,
            layout.system_y + 56.0f
        );
        if (contains(system_toggle)) {
            dashboard_system_info_open_ = !dashboard_system_info_open_;
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }

    if (active_tab_ == ActiveTab::Data) {
        const DataTabLayout layout = GetDataTabLayout(
            render_size_.width,
            render_size_.height
        );
        const TrafficHistorySnapshot history = engine.GetTrafficHistorySnapshot();

        auto invalidate_data = [&]() {
            InvalidateRect(hwnd, nullptr, FALSE);
        };
        auto set_picker_year_index = [&]() {
            const auto it = std::find(
                history.available_years.begin(),
                history.available_years.end(),
                data_history_year_
            );
            const int selected_index = it == history.available_years.end()
                ? 0
                : static_cast<int>(std::distance(history.available_years.begin(), it));
            data_picker_page_ = data_range_idx_ == 3 ? selected_index / 12 : selected_index;
        };
        if (data_picker_open_ || data_picker_visibility_ > 0.001f) {
            // Keep a closing popover modal until its hide animation completes.
            if (!data_picker_open_) {
                return;
            }
            const bool monthly_picker = data_range_idx_ == 2;
            const bool yearly_picker = data_range_idx_ == 3;
            if (!monthly_picker && !yearly_picker) {
                SetDataPickerOpen(false);
                invalidate_data();
                return;
            }

            const D2D1_RECT_F picker = GetDataPickerRect(layout, monthly_picker, history.available_years.size());
            if (x < picker.left || x > picker.right || y < picker.top || y > picker.bottom) {
                SetDataPickerOpen(false);
                invalidate_data();
                return;
            }

            if (history.available_years.empty()) {
                return;
            }

            const float nav_top = picker.top + 14.0f;
            const float nav_bottom = nav_top + 28.0f;
            const bool previous_clicked = x >= picker.left + 118.0f && x <= picker.left + 146.0f &&
                                          y >= nav_top && y <= nav_bottom;
            const bool next_clicked = x >= picker.right - 42.0f && x <= picker.right - 14.0f &&
                                      y >= nav_top && y <= nav_bottom;

            if (monthly_picker) {
                data_picker_page_ = std::clamp(
                    data_picker_page_,
                    0,
                    static_cast<int>(history.available_years.size()) - 1
                );
                if (previous_clicked && data_picker_page_ > 0) {
                    --data_picker_page_;
                    invalidate_data();
                    return;
                }
                if (next_clicked && data_picker_page_ + 1 < static_cast<int>(history.available_years.size())) {
                    ++data_picker_page_;
                    invalidate_data();
                    return;
                }

                const int picker_year = history.available_years[data_picker_page_];
                const float grid_x = picker.left + 14.0f;
                const float grid_y = picker.top + 126.0f;
                const float grid_w = picker.right - picker.left - 28.0f;
                const float cell_gap = 6.0f;
                const float cell_w = (grid_w - cell_gap * 3.0f) / 4.0f;
                const float cell_h = 34.0f;
                for (int month = 1; month <= 12; ++month) {
                    const int column = (month - 1) % 4;
                    const int row = (month - 1) / 4;
                    const float cell_left = grid_x + column * (cell_w + cell_gap);
                    const float cell_top = grid_y + row * (cell_h + cell_gap);
                    if (x >= cell_left && x <= cell_left + cell_w &&
                        y >= cell_top && y <= cell_top + cell_h) {
                        data_history_year_ = picker_year;
                        data_history_month_ = month;
                        SetDataPickerOpen(false);
                        RestartDataChartAnimation();
                        invalidate_data();
                        return;
                    }
                }
                return;
            }

            constexpr int kYearsPerPage = 12;
            const int page_count = std::max(
                1,
                static_cast<int>((history.available_years.size() + kYearsPerPage - 1) / kYearsPerPage)
            );
            data_picker_page_ = std::clamp(data_picker_page_, 0, page_count - 1);
            if (previous_clicked && data_picker_page_ > 0) {
                --data_picker_page_;
                invalidate_data();
                return;
            }
            if (next_clicked && data_picker_page_ + 1 < page_count) {
                ++data_picker_page_;
                invalidate_data();
                return;
            }

            const int first_year_index = data_picker_page_ * kYearsPerPage;
            const int last_year_index = std::min(
                static_cast<int>(history.available_years.size()),
                first_year_index + kYearsPerPage
            );
            const float grid_x = picker.left + 14.0f;
            const float grid_y = picker.top + 106.0f;
            const float grid_w = picker.right - picker.left - 28.0f;
            const float cell_gap = 8.0f;
            const float cell_w = (grid_w - cell_gap * 2.0f) / 3.0f;
            const float cell_h = 42.0f;
            for (int index = first_year_index; index < last_year_index; ++index) {
                const int visible_index = index - first_year_index;
                const int column = visible_index % 3;
                const int row = visible_index / 3;
                const float cell_left = grid_x + column * (cell_w + cell_gap);
                const float cell_top = grid_y + row * (cell_h + cell_gap);
                if (x >= cell_left && x <= cell_left + cell_w &&
                    y >= cell_top && y <= cell_top + cell_h) {
                    data_history_year_ = history.available_years[index];
                    SetDataPickerOpen(false);
                    RestartDataChartAnimation();
                    invalidate_data();
                    return;
                }
            }

            const int visible_year_count = last_year_index - first_year_index;
            const int visible_year_rows = std::max(1, (visible_year_count + 2) / 3);
            const float months_grid_y = grid_y + visible_year_rows * cell_h +
                (visible_year_rows - 1) * cell_gap + 34.0f;
            const float month_grid_w = picker.right - picker.left - 28.0f;
            const float month_gap = 6.0f;
            const float month_w = (month_grid_w - month_gap * 3.0f) / 4.0f;
            const float month_h = 34.0f;
            for (int month = 1; month <= 12; ++month) {
                const int column = (month - 1) % 4;
                const int row = (month - 1) / 4;
                const float month_left = grid_x + column * (month_w + month_gap);
                const float month_top = months_grid_y + row * (month_h + month_gap);
                if (x >= month_left && x <= month_left + month_w &&
                    y >= month_top && y <= month_top + month_h) {
                    data_range_idx_ = 2;
                    data_history_month_ = month;
                    SetDataPickerOpen(false);
                    RestartDataChartAnimation();
                    invalidate_data();
                    return;
                }
            }
            return;
        }

        const float usage_period_width = layout.content_w / 3.0f;
        if (y >= layout.usage_filter_y && y <= layout.usage_filter_y + 36.0f &&
            x >= layout.start_x && x <= layout.start_x + layout.content_w) {
            const int index = static_cast<int>((x - layout.start_x) / usage_period_width);
            if (index >= 0 && index < 3 && index != data_usage_period_idx_) {
                data_usage_period_idx_ = index;
                invalidate_data();
            }
            return;
        }

        const float history_x = layout.start_x + 20.0f;
        const float history_width = layout.content_w - 40.0f;
        const float range_width = history_width / 4.0f;
        if (y >= layout.filter_y && y <= layout.filter_y + 40.0f &&
            x >= history_x && x <= history_x + history_width) {
            const int index = static_cast<int>((x - history_x) / range_width);
            if (index >= 0 && index < 4) {
                if (index != data_range_idx_) {
                    SetDataRangeIndex(index);
                    if (data_limit_edit_) {
                        ShowWindow(data_limit_edit_, SW_HIDE);
                    }
                }
                if (index >= 2) {
                    set_picker_year_index();
                }
                // Selecting a range never opens the picker. The date trigger
                // below is the explicit action that opens it.
                SetDataPickerOpen(false);
                invalidate_data();
            }
            return;
        }

        const float trigger_width = 132.0f;
        const float trigger_x = history_x + (history_width - trigger_width) / 2.0f;
        if ((data_range_idx_ == 2 || data_range_idx_ == 3) &&
            x >= trigger_x && x <= trigger_x + trigger_width &&
            y >= layout.range_y && y <= layout.range_y + 38.0f) {
            if (data_limit_edit_) {
                ShowWindow(data_limit_edit_, SW_HIDE);
            }
            set_picker_year_index();
            SetDataPickerOpen(true);
            invalidate_data();
            return;
        }

        const float limit_input_left = layout.start_x + 16.0f;
        const float limit_input_top = layout.limit_y + 66.0f;
        const float limit_button_width = 82.0f;
        const float limit_button_height = 28.0f;
        const float limit_button_left = layout.start_x + layout.content_w - 16.0f - limit_button_width;
        const float limit_button_top = limit_input_top + 1.0f;
        const float limit_input_right = limit_button_left - 8.0f;
        const float limit_input_height = 30.0f;
        if (x >= limit_input_left && x <= limit_input_right &&
            y >= limit_input_top && y <= limit_input_top + limit_input_height) {
            wchar_t edit_value[64]{};
            if (data_limit_bytes_ > 0) {
                const double gib = static_cast<double>(data_limit_bytes_) / (1024.0 * 1024.0 * 1024.0);
                swprintf_s(edit_value, L"%.2f", gib);
            }
            if (!data_limit_edit_ || !IsWindow(data_limit_edit_)) {
                data_limit_edit_ = CreateWindowExW(
                    0,
                    L"EDIT",
                    edit_value,
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                    static_cast<int>(limit_input_left + 4.0f),
                    static_cast<int>(limit_input_top + 3.0f - CurrentContentScroll()),
                    static_cast<int>(limit_input_right - limit_input_left - 8.0f),
                    static_cast<int>(limit_input_height - 6.0f),
                    hwnd,
                    nullptr,
                    GetModuleHandleW(nullptr),
                    nullptr
                );
                if (data_limit_edit_) {
                    SendMessageW(
                        data_limit_edit_,
                        WM_SETFONT,
                        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                        TRUE
                    );
                }
            } else {
                SetWindowTextW(data_limit_edit_, edit_value);
                SetWindowPos(
                    data_limit_edit_,
                    HWND_TOP,
                    static_cast<int>(limit_input_left + 4.0f),
                    static_cast<int>(limit_input_top + 3.0f - CurrentContentScroll()),
                    static_cast<int>(limit_input_right - limit_input_left - 8.0f),
                    static_cast<int>(limit_input_height - 6.0f),
                    SWP_SHOWWINDOW
                );
            }
            if (data_limit_edit_) {
                SetFocus(data_limit_edit_);
            }
            return;
        }

        if (x >= limit_button_left && x <= limit_button_left + limit_button_width &&
            y >= limit_button_top && y <= limit_button_top + limit_button_height) {
            const uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
            bool accepted = false;
            if (data_limit_edit_ && IsWindowVisible(data_limit_edit_)) {
                wchar_t value[64]{};
                GetWindowTextW(data_limit_edit_, value, static_cast<int>(std::size(value)));
                wchar_t* parse_end = nullptr;
                const double requested_gib = std::wcstod(value, &parse_end);
                while (parse_end != nullptr && *parse_end == L' ') {
                    ++parse_end;
                }
                if (value[0] == L'\0') {
                    data_limit_bytes_ = 0;
                    accepted = true;
                } else if (parse_end != value && parse_end != nullptr && *parse_end == L'\0' &&
                           std::isfinite(requested_gib) && requested_gib > 0.0) {
                    const long double requested_bytes = static_cast<long double>(requested_gib) * gibibyte;
                    data_limit_bytes_ = static_cast<uint64_t>(std::min(
                        requested_bytes,
                        static_cast<long double>(std::numeric_limits<uint64_t>::max())
                    ));
                    accepted = true;
                }
                if (!accepted) {
                    SetFocus(data_limit_edit_);
                    return;
                }
                ShowWindow(data_limit_edit_, SW_HIDE);
            } else {
                // Match the source action: an empty field removes the limit.
                data_limit_bytes_ = 0;
                accepted = true;
            }
            if (accepted) {
                SaveSettings();
                invalidate_data();
            }
            return;
        }
    }

    if (active_tab_ == ActiveTab::Apps) {
        const float side = std::clamp(render_size_.width * 0.032f, 16.0f, 30.0f);
        const float start_y = UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT + 16.0f;
        const float usage_y = start_y + 80.0f;
        const float badge_width = 26.0f;
        const float button_right = render_size_.width - side - badge_width - 8.0f;
        // y was already translated from viewport to content coordinates above.
        const float content_y = static_cast<float>(y);

        const auto app_snapshot = engine.GetAppActivitySnapshot();
        if (!app_snapshot.is_admin) {
            constexpr float restart_width = MainContentLayout::app_admin_button_width;
            constexpr float restart_height = 24.0f;
            const float restart_x = render_size_.width - side - restart_width;
            const float restart_y = start_y - 2.0f;
            if (x >= restart_x && x <= restart_x + restart_width &&
                content_y >= restart_y && content_y <= restart_y + restart_height) {
                wchar_t path[MAX_PATH]{};
                GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
                const std::wstring parameters = L"--replace-pid " + std::to_wstring(GetCurrentProcessId());
                const HINSTANCE launched = ShellExecuteW(hwnd, L"runas", path, parameters.c_str(), nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(launched) > 32) {
                    PostMessageW(hwnd, WatchmanWindowLifecycle::ExitMessage, 0, 0);
                }
                return;
            }
        }

        // Source Watchman opens Windows' own Data Usage page from this control.
        if (x >= side && x <= button_right && content_y >= usage_y && content_y <= usage_y + 28.0f) {
            OpenWindowsDataUsageSettings(hwnd);
            return;
        }
    }

    if (active_tab_ == ActiveTab::Network) {
        // Keep this hit target in lockstep with the responsive Network layout.
        const NetworkTabLayout layout = GetNetworkTabLayout(render_size_.width);
        const D2D1_RECT_F adapter_row = GetNetworkAdapterRowRect(layout);
        if (x >= adapter_row.left && x <= adapter_row.right &&
            y >= adapter_row.top && y <= adapter_row.bottom) {
            settings_network_adapters_ = engine.GetNetworkAdapters();
            network_adapter_picker_requested_ = true;
            return;
        }

        const float btn_w = 84.0f;
        const float btn_h = 26.0f;
        const float btn_x = layout.side + layout.content_w - btn_w - 16.0f;
        const float btn_y = layout.speed_test_y + 15.0f;

        if (x >= btn_x && x <= btn_x + btn_w && y >= btn_y && y <= btn_y + btn_h) {
            if (!engine.IsReady()) return;
            if (engine.IsSpeedTestRunning()) {
                engine.CancelSpeedTest();
            } else {
                speed_details_scroll_ = speed_details_scroll_max_ = 0.0f;
                speed_consent_open_ = true;
            }
            return;
        }
        const auto result = engine.GetSpeedTestResult();
        const auto status = SpeedTestPresentation::GetStatus(result.download_state, result.upload_state,
            result.upload_server_confirmed, !result.error_message.empty(), !result.warning_message.empty());
        if (!engine.IsSpeedTestRunning() && status.notice && IsPointInside(
                SpeedTestPresentation::DetailsButton(layout.side, layout.side + layout.content_w, layout.speed_test_y), x, y)) {
            speed_details_result_ = result;
            speed_details_scroll_ = 0.0f;
            speed_details_open_ = true;
            return;
        }
    }

    if (active_tab_ == ActiveTab::Tools) {
        const DiagnosticsSnapshot diagnostics = engine.GetDiagnosticsSnapshot();
        const ToolsTabLayout layout = GetToolsTabLayout(
            render_size_.width,
            diagnostics,
            !tools_export_status_.empty()
        );
        const auto contains = [x, y](const D2D1_RECT_F& rect) {
            return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
        };
        const float value_w = std::min(180.0f, std::max(134.0f, layout.content_w * 0.37f));
        const float value_x = layout.side + layout.content_w - 20.0f - value_w;
        const float first_row_y = layout.export_y + 53.0f;
        const D2D1_RECT_F diagnostics_button = GetToolsHeaderButtonRect(layout, layout.diagnostics_y);
        const D2D1_RECT_F updates_button = GetToolsHeaderButtonRect(layout, layout.updates_y);
        const D2D1_RECT_F period_rect = D2D1::RectF(
            value_x, first_row_y + 36.0f, value_x + value_w, first_row_y + 64.0f
        );
        const D2D1_RECT_F scope_rect = D2D1::RectF(
            value_x, first_row_y + 72.0f, value_x + value_w, first_row_y + 100.0f
        );
        const D2D1_RECT_F export_button = GetToolsDownloadButtonRect(layout);

        if (contains(diagnostics_button)) {
            engine.RunDiagnostics();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (contains(updates_button)) {
            if (update_notice_.Last().status != WatchmanUpdates::Status::Checking) update_check_requested_ = true;
            return;
        }
        if (contains(period_rect)) {
            tools_export_picker_request_ = ToolsExportPicker::Period;
            return;
        }
        if (contains(scope_rect)) {
            tools_export_picker_request_ = ToolsExportUsesMonth(tools_export_period_index_)
                ? ToolsExportPicker::Month
                : ToolsExportPicker::Year;
            return;
        }
        if (contains(export_button)) {
            TrafficExportOptions options;
            options.period = ToTrafficExportPeriod(tools_export_period_index_);
            options.month = tools_export_month_;
            options.year = tools_export_year_;
            const TrafficExportResult result = engine.ExportTrafficCsv(options);
            tools_export_status_success_ = result.success;
            tools_export_status_ = result.success
                ? L"Report saved: " + result.filepath
                : L"Export failed: " + result.error;
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }
}

void AppRenderer::Render(HWND hwnd, 
                         const DashboardSnapshot& dash_snap, 
                         const NetworkSnapshot& net_snap, 
                         const AppActivitySnapshot& app_snap,
                         const TrafficHistorySnapshot& hist_snap,
                         const SpeedTestSnapshot& speed,
                         const DiagnosticsSnapshot& diagnostics_snap) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
    if (FAILED(Initialize(hwnd))) {
        main_animation_active_ = false;
        DrawMainGraphicsFallback(hwnd);
        return;
    }
#ifdef WATCHMAN_RENDER_TESTING
    ++test_main_paints_;
#endif
    if (main_editor_suspended_) {
        if (IsWindow(data_limit_edit_)) ShowWindow(data_limit_edit_, SW_SHOWNOACTIVATE);
        main_editor_suspended_ = false;
    }

    // Animation duration is independent of paint count, input rate or refresh
    // rate. Numeric readings still show their real sampled values.
    animation_alpha_ = animation_clock_.Advance(GetTickCount64());
    anim_dl_speed_ += (static_cast<float>(dash_snap.download_speed_bps) - anim_dl_speed_) * animation_alpha_;
    anim_ul_speed_ += (static_cast<float>(dash_snap.upload_speed_bps) - anim_ul_speed_) * animation_alpha_;
    anim_cpu_usage_ += ((CpuReadingAvailable(dash_snap) ? dash_snap.cpu_usage_percent : 0.0f) - anim_cpu_usage_) * animation_alpha_;
    const float ram_target = WatchmanLiveReadings::MemoryReady(dash_snap, GetTickCount64()) ? dash_snap.ram_usage_percent : 0.0f;
    const float gpu_target = WatchmanLiveReadings::GpuReady(dash_snap) ? dash_snap.gpu_usage_percent : 0.0f;
    anim_ram_usage_ += (ram_target - anim_ram_usage_) * animation_alpha_;
    anim_gpu_usage_ += (gpu_target - anim_gpu_usage_) * animation_alpha_;
    UpdateContentAnimation();
    UpdateDataPickerAnimation();
    if (active_tab_ == ActiveTab::Data) {
        if (data_chart_animation_started_ms_ == 0) {
            RestartDataChartAnimation();
        }
        const float elapsed_ms = static_cast<float>(GetTickCount64() - data_chart_animation_started_ms_);
        data_chart_progress_ = EaseInOutQuart(elapsed_ms / 400.0f);
    } else {
        data_chart_progress_ = 0.0f;
    }
    
    if (chart_sample_gate_.Accept(dash_snap.sample_sequence)) {
        speed_history_.erase(speed_history_.begin());
        speed_history_.push_back({dash_snap.download_speed_bps / 8.0 / 1024.0,
            dash_snap.upload_speed_bps / 8.0 / 1024.0});
    }

    render_target_->BeginDraw();
    render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    D2D1_SIZE_F rt_size = render_target_->GetSize();
    render_size_ = rt_size;

    // 1. Draw the single app-level gradient before every translucent surface.
    DrawAppBackground(rt_size);

    // 2. Render only the active tab inside the scrolling content viewport.
    const int active_tab_index = static_cast<int>(active_tab_);
    if (active_tab_ == ActiveTab::Network && reveal_speed_test_) {
        const auto network_layout = GetNetworkTabLayout(rt_size.width);
        const auto status = SpeedTestPresentation::GetStatus(speed.result.download_state, speed.result.upload_state,
            speed.result.upload_server_confirmed, !speed.result.error_message.empty(), !speed.result.warning_message.empty());
        const float height = !speed.Running() && status.notice ? 396.0f : network_layout.speed_test_h;
        content_bottoms_[active_tab_index] = (std::max)(content_bottoms_[active_tab_index], network_layout.speed_test_y + height + 20.0f);
        content_scroll_offsets_[active_tab_index] = SpeedTestView::RevealOffset(network_layout.speed_test_y, height + 20.0f,
            ContentViewportTop(), ContentViewportBottom(), CurrentContentScroll());
        if (!speed.Running()) reveal_speed_test_ = false;
    }
    ClampContentScroll(active_tab_);
    const float scroll_offset = CurrentContentScroll();
    content_bottoms_[active_tab_index] = ContentViewportBottom();
    const D2D1_RECT_F content_clip = D2D1::RectF(
        0.0f,
        ContentViewportTop(),
        rt_size.width,
        ContentViewportBottom()
    );
    render_target_->PushAxisAlignedClip(content_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const bool animate_content =
        anim_content_opacity_ < 0.999f || std::abs(anim_content_translate_y_) > 0.01f;
    ID2D1Layer* content_layer = nullptr;
    if (animate_content &&
        SUCCEEDED(render_target_->CreateLayer(nullptr, &content_layer))) {
        const D2D1_LAYER_PARAMETERS content_layer_parameters = D2D1::LayerParameters(
            D2D1::InfiniteRect(),
            nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Identity(),
            anim_content_opacity_,
            nullptr,
            D2D1_LAYER_OPTIONS_NONE
        );
        render_target_->PushLayer(content_layer_parameters, content_layer);
    }
    render_target_->SetTransform(D2D1::Matrix3x2F::Translation(
        0.0f,
        -scroll_offset + (content_layer ? anim_content_translate_y_ : 0.0f)
    ));
    switch (active_tab_) {
    case ActiveTab::Dashboard:
        DrawDashboard(rt_size, dash_snap, net_snap, hist_snap);
        break;
    case ActiveTab::Data:
        DrawData(rt_size, hist_snap);
        break;
    case ActiveTab::Apps:
        DrawAppMonitor(rt_size, app_snap);
        break;
    case ActiveTab::Network:
        DrawNetworkHealth(rt_size, net_snap, dash_snap, speed);
        break;
    case ActiveTab::Tools:
        DrawTools(rt_size, hist_snap, diagnostics_snap);
        break;
    }
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (content_layer) {
        render_target_->PopLayer();
        content_layer->Release();
    }
    render_target_->PopAxisAlignedClip();

    ClampContentScroll(active_tab_);
    DrawContentScrollbar(rt_size);

    // 3. Draw Header and Footer Components after content clip so dividers stay solid
    DrawTitlebar(rt_size);
    DrawTabbar(rt_size);
    DrawStatusbar(rt_size, net_snap, dash_snap);

    // Render settings modal if opened
    if (settings_open_) {
        DrawSettingsModal(rt_size, dash_snap);
    }
    if (speed_consent_open_ || speed_details_open_) DrawSpeedDialog(rt_size);
    if (welcome_tour_.IsOpen()) DrawWelcomeTour(rt_size);
    if (update_prompt_open_) DrawUpdatePrompt(rt_size);

    main_animation_active_ = content_animation_started_ms_ != 0 || data_picker_animating_ ||
        (active_tab_ == ActiveTab::Data && (data_chart_progress_ < 1.0f || data_limit_animating_)) ||
        (active_tab_ == ActiveTab::Dashboard && !hide_hardware_gauges_ &&
            (std::abs(anim_cpu_usage_ - (CpuReadingAvailable(dash_snap) ? dash_snap.cpu_usage_percent : 0.0f)) > 0.1f ||
             std::abs(anim_ram_usage_ - ram_target) > 0.1f ||
             std::abs(anim_gpu_usage_ - gpu_target) > 0.1f));
    HRESULT hr = render_target_->EndDraw();
    if (FAILED(hr)) {
        ReleaseMainWindowResources();
        main_initialization_error_ = hr;
        main_graphics_retry_.Failed(MainGraphicsTime());
        main_animation_active_ = false;
        WatchmanStartupDiagnostics::Record("main-graphics", "presentation-failed", static_cast<uint32_t>(hr));
        DrawMainGraphicsFallback(hwnd);
    } else main_graphics_retry_.Succeeded(); // Only an accepted dashboard frame clears repeated device failures.
}

void AppRenderer::DrawWelcomeMonitorPreviews(const WatchmanWelcome::Rect& area) {
    using namespace WatchmanWelcome;
    const auto previews = GetMonitorPreviews(area);
    if (previews.scale <= 0) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill, ink;
    if (FAILED(render_target_->CreateSolidColorBrush(D2D1::ColorF(0, 0.0f), fill.GetAddressOf())) ||
        FAILED(render_target_->CreateSolidColorBrush(D2D1::ColorF(0, 0.0f), ink.GetAddressOf()))) return;
    const auto color = [](COLORREF value, float alpha = 1.0f) {
        return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f, alpha);
    };
    D2D1_MATRIX_3X2_F previous{};
    render_target_->GetTransform(&previous);
    const auto at = [&](const Rect& bounds) {
        render_target_->SetTransform(D2D1::Matrix3x2F::Scale(previews.scale, previews.scale) *
            D2D1::Matrix3x2F::Translation(bounds.left, bounds.top) * previous);
    };
    const auto caption = [&](const Rect& bounds, const wchar_t* text) {
        at(bounds);
        render_target_->DrawText(text, static_cast<UINT32>(wcslen(text)), format_dashboard_note_,
            D2D1::RectF(0, 0, Width(bounds) / previews.scale, Height(bounds) / previews.scale),
            brush_text_secondary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    caption(previews.taskbar_label, taskbar_caption);
    caption(previews.floating_label, floating_caption);

    const bool light = effective_theme_ == ThemeMode::Light;
    const COLORREF shadow = light ? RGB(255, 255, 255) : RGB(0, 0, 0);
    const auto draw = [&](const wchar_t* text, const auto& bounds, IDWriteTextFormat* format,
                          COLORREF value, bool with_shadow) {
        const auto rect = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
            static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
        const UINT32 length = static_cast<UINT32>(wcslen(text));
        if (with_shadow) {
            auto shadow_rect = rect;
            shadow_rect.top += 1;
            shadow_rect.bottom += 1;
            ink->SetColor(color(shadow, 0.90f));
            render_target_->DrawText(text, length, format, shadow_rect, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        ink->SetColor(color(value));
        render_target_->DrawText(text, length, format, rect, ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // Use the real taskbar geometry, font, colors and two rows of numbers.
    const auto size = TaskbarWidgetLayout::PreferredSize(TaskbarWidgetMode::All, false, 36, 96);
    const auto taskbar = TaskbarWidgetLayout::Get(size.width, size.height, TaskbarWidgetMode::All, false, 96);
    at(previews.taskbar);
    fill->SetColor(D2D1::ColorF(light ? 0xF3F3F3 : 0x202020));
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(0, 0, static_cast<float>(size.width), static_cast<float>(size.height)), 4, 4), fill.Get());
    const auto alignment = format_widget_->GetTextAlignment();
    format_widget_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const COLORREF values = light ? RGB(38, 49, 58) : RGB(255, 255, 255);
    draw(L"\u2191", taskbar.upload_arrow, format_widget_, light ? RGB(209, 63, 131) : RGB(240, 125, 172), true);
    draw(L"\u2193", taskbar.download_arrow, format_widget_, light ? RGB(0, 120, 212) : RGB(99, 214, 255), true);
    auto upload = taskbar.upload_value;
    upload.right = taskbar.upload_unit.right;
    auto download = taskbar.download_value;
    download.right = taskbar.download_unit.right;
    draw(sample_upload, upload, format_widget_, values, true);
    draw(sample_download, download, format_widget_, values, true);
    const COLORREF cpu = light ? RGB(164, 95, 0) : RGB(243, 182, 74);
    const COLORREF memory = light ? RGB(22, 122, 56) : RGB(134, 217, 108);
    draw(L"CPU", taskbar.cpu_label, format_widget_, cpu, true);
    draw(sample_cpu, taskbar.cpu_value, format_widget_, cpu, true);
    draw(L"MEM", taskbar.memory_label, format_widget_, memory, true);
    draw(sample_memory, taskbar.memory_value, format_widget_, memory, true);
    format_widget_->SetTextAlignment(alignment);

    // Match the real default floating monitor, including its CPU/memory row.
    at(previews.floating);
    const int floating_width = FloatingMonitorLayout::Width(FloatingMonitorLayout::default_font_size);
    const int floating_height = FloatingMonitorLayout::Height();
    fill->SetColor(color(FloatingMonitorLayout::panel_background_color, 0.92f));
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0,
        static_cast<float>(floating_width), static_cast<float>(floating_height)), 6, 6), fill.Get());
    const auto floating = FloatingMonitorLayout::Get(floating_width, floating_height);
    const COLORREF white = FloatingMonitorLayout::default_text_color;
    draw(L"UP", floating.upload_label, format_floating_preview_, white, false);
    draw(sample_upload, floating.upload_value, format_floating_preview_, white, false);
    draw(L"DN", floating.download_label, format_floating_preview_, white, false);
    draw(sample_download, floating.download_value, format_floating_preview_, white, false);
    draw(L"CPU", floating.cpu_label, format_floating_preview_, white, false);
    draw(sample_cpu, floating.cpu_value, format_floating_preview_, white, false);
    draw(L"MEM", floating.memory_label, format_floating_preview_, white, false);
    draw(sample_memory, floating.memory_value, format_floating_preview_, white, false);
    render_target_->SetTransform(previous);
}

void AppRenderer::DrawWelcomeTour(D2D1_SIZE_F rt_size) {
    using namespace WatchmanWelcome;
    const auto layout = GetLayout(rt_size.width, rt_size.height);
    const auto rect = [](const Rect& value) { return D2D1::RectF(value.left, value.top, value.right, value.bottom); };
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> overlay, surface, tint;
    if (SUCCEEDED(render_target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.65f), overlay.GetAddressOf()))) {
        render_target_->FillRectangle(D2D1::RectF(0, 0, rt_size.width, rt_size.height), overlay.Get());
    }
    auto color = palette_.CardBackground;
    color.a = 1.0f;
    render_target_->CreateSolidColorBrush(color, surface.GetAddressOf());
    render_target_->CreateSolidColorBrush(effective_theme_ == ThemeMode::Light ?
        D2D1::ColorF(0xEFF6FF) : D2D1::ColorF(0x0C2035), tint.GetAddressOf());
    const auto card = D2D1::RoundedRect(rect(layout.card), 20.0f, 20.0f);
    render_target_->FillRoundedRectangle(card, surface ? surface.Get() : brush_popup_bg_);
    render_target_->DrawRoundedRectangle(card, brush_card_border_, 1.0f);

    float brand_left = layout.brand.left;
    if (titlebar_logo_bitmap_) {
        render_target_->DrawBitmap(titlebar_logo_bitmap_, D2D1::RectF(brand_left, layout.brand.top,
            brand_left + 24, layout.brand.bottom), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        brand_left += 32;
    }
    render_target_->DrawText(L"WATCHMAN", 8, format_titlebar_title_,
        D2D1::RectF(brand_left, layout.brand.top, layout.brand.right, layout.brand.bottom),
        brush_text_primary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const std::wstring step = L"Quick start  /  " + std::to_wstring(welcome_tour_.PageIndex() + 1) +
        L" of " + std::to_wstring(page_count);
    render_target_->DrawText(step.c_str(), static_cast<UINT32>(step.size()), format_dashboard_note_,
        rect(layout.step), brush_text_secondary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);

    // Numbered monitor previews and lightweight tray/Apps illustrations.
    // Samples are labelled; only Skip, Back and Next are interactive.
    if (Height(layout.illustration) > 0) {
        const auto& art = layout.illustration;
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(rect(art), 12, 12), tint ? tint.Get() : brush_active_);
        render_target_->PushAxisAlignedClip(rect(art), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float cx = (art.left + art.right) / 2, cy = (art.top + art.bottom) / 2;
        const float scale = std::min(Height(art) / 80.0f, Width(art) / 300.0f);
        const auto p = [&](float x, float y) { return D2D1::Point2F(cx + x * scale, cy + y * scale); };
        const auto r = [&](float left, float top, float right, float bottom) {
            return D2D1::RectF(cx + left * scale, cy + top * scale, cx + right * scale, cy + bottom * scale);
        };
        const auto line = [&](float x1, float y1, float x2, float y2, ID2D1Brush* brush) {
            render_target_->DrawLine(p(x1, y1), p(x2, y2), brush, 1.5f);
        };
        const auto box = [&](float left, float top, float right, float bottom) {
            const auto bounds = D2D1::RoundedRect(r(left, top, right, bottom), 6 * scale, 6 * scale);
            render_target_->FillRoundedRectangle(bounds, surface ? surface.Get() : brush_popup_bg_);
            render_target_->DrawRoundedRectangle(bounds, brush_accent_dl_, 1.5f);
        };
        if (welcome_tour_.PageIndex() == 0) {
            DrawWelcomeMonitorPreviews(art);
        } else if (welcome_tour_.PageIndex() == 1) {
            box(-136, 19, 136, 33); // Windows tray strip.
            line(-120, 28, -114, 23, brush_text_secondary_);
            line(-114, 23, -108, 28, brush_text_secondary_);
            render_target_->FillEllipse(D2D1::Ellipse(p(-88, 26), 4 * scale, 4 * scale), brush_accent_dl_);
            box(-46, -32, 116, 13); // The right-click menu.
            for (float y : {-21.0f, -9.0f, 3.0f}) {
                render_target_->FillEllipse(D2D1::Ellipse(p(-32, y), 2 * scale, 2 * scale), brush_accent_dl_);
                line(-20, y, 97, y, brush_text_secondary_);
            }
            DrawVectorIcon(render_target_, VectorIcon::Close, r(-126, -24, -100, 2), brush_text_secondary_);
        } else {
            for (int index = 0; index < 3; ++index) {
                const float y = -22.0f + index * 22.0f;
                render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-124, y - 6, -110, y + 7), 3, 3), brush_text_secondary_, 1.0f);
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(r(-98, y - 4, 26 - index * 30.0f, y + 4), 3, 3), brush_accent_dl_);
            }
            const D2D1_POINT_2F shield[] = {p(90, -30), p(115, -20), p(111, 9), p(90, 29), p(69, 9), p(65, -20), p(90, -30)};
            for (size_t index = 1; index < std::size(shield); ++index) render_target_->DrawLine(
                shield[index - 1], shield[index], brush_accent_dl_, 2.0f);
            line(78, -1, 87, 8, brush_accent_dl_); line(87, 8, 103, -10, brush_accent_dl_);
        }
        render_target_->PopAxisAlignedClip();
    }

    const float text_width = Width(layout.body);
    if (welcome_text_page_ != welcome_tour_.PageIndex() || welcome_text_width_ != text_width ||
        std::any_of(welcome_text_.begin(), welcome_text_.end(), [](const SettingsText::Paragraph& value) { return !value.layout; })) {
        IDWriteTextFormat* formats[text_count] = {format_welcome_title_, format_body_, format_body_bold_, format_body_, format_dashboard_note_};
        for (int index = 0; index < text_count; ++index) SettingsText::CreateParagraph(dwrite_factory_, formats[index],
            pages[welcome_tour_.PageIndex()].text[static_cast<size_t>(index)], TextWidth(layout, index), welcome_text_[static_cast<size_t>(index)]);
        welcome_text_page_ = welcome_tour_.PageIndex();
        welcome_text_width_ = text_width;
    }
    std::array<float, text_count> heights{};
    for (size_t index = 0; index < heights.size(); ++index) heights[index] = welcome_text_[index].height;
    const auto content = GetContent(layout, heights, welcome_scroll_);
    welcome_scroll_ = content.scroll;
    welcome_scroll_max_ = content.scroll_max;
    render_target_->PushAxisAlignedClip(rect(layout.body), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(rect(content.tip_card), 12, 12), tint ? tint.Get() : brush_active_);
    for (int index = 0; index < text_count; ++index) {
        const auto& paragraph = welcome_text_[static_cast<size_t>(index)];
        const auto& area = content.text[static_cast<size_t>(index)];
        if (paragraph.layout) render_target_->DrawTextLayout(D2D1::Point2F(area.left, area.top), paragraph.layout.Get(),
            index == Title || index == TipTitle ? brush_text_primary_ : brush_text_secondary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    render_target_->PopAxisAlignedClip();
    if (content.scroll_max > 0) {
        const float track = Height(layout.body);
        const float thumb = std::max(20.0f, track * track / (track + content.scroll_max));
        const float top = layout.body.top + (track - thumb) * content.scroll / content.scroll_max;
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(layout.body.right + 6, top,
            layout.body.right + 9, top + thumb), 1.5f, 1.5f), brush_text_secondary_);
    }
    const auto button = [&](const Rect& bounds, const wchar_t* label, Control control, bool primary) {
        const auto area = rect(bounds);
        const bool hover = Contains(bounds, static_cast<float>(mouse_x_), static_cast<float>(mouse_y_));
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(area, 9, 9), primary ? brush_action_bg_ :
            hover ? brush_active_ : (surface ? surface.Get() : brush_popup_bg_));
        if (control != Control::Skip) render_target_->DrawRoundedRectangle(D2D1::RoundedRect(area, 9, 9), brush_card_border_, 1.0f);
        render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_welcome_button_, area,
            primary ? brush_tooltip_text_ : brush_text_primary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (welcome_keyboard_focus_ && welcome_tour_.Focused() == control) render_target_->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(area.left - 3, area.top - 3, area.right + 3, area.bottom + 3), 12, 12), brush_text_primary_, 1.5f);
    };
    button(layout.skip, L"Skip", Control::Skip, false);
    if (welcome_tour_.CanGoBack()) button(layout.back, L"Back", Control::Back, false);
    button(layout.next, welcome_tour_.NextLabel(), Control::Next, true);
    const float center = (layout.back.right + layout.next.left) / 2;
    for (int index = 0; index < page_count; ++index) render_target_->FillEllipse(D2D1::Ellipse(
        D2D1::Point2F(center + (index - 1) * 12, (layout.next.top + layout.next.bottom) / 2), 3, 3),
        index == welcome_tour_.PageIndex() ? brush_accent_dl_ : brush_card_border_);
}

void AppRenderer::DrawSpeedDialog(D2D1_SIZE_F rt_size) {
    const auto dialog = SpeedTestPresentation::GetDialog(rt_size.width, rt_size.height);
    ID2D1SolidColorBrush* overlay = nullptr;
    if (SUCCEEDED(render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &overlay))) {
        render_target_->FillRectangle(D2D1::RectF(0, 0, rt_size.width, rt_size.height), overlay);
        overlay->Release();
    }
    ID2D1SolidColorBrush* surface = nullptr;
    auto surface_color = palette_.CardBackground;
    surface_color.a = 1.0f;
    if (SUCCEEDED(render_target_->CreateSolidColorBrush(surface_color, &surface))) {
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(dialog.card, 16.0f, 16.0f), surface);
        surface->Release();
    }
    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(dialog.card, 16.0f, 16.0f), brush_card_border_, 1.0f);
    const wchar_t* title = speed_consent_open_ ? L"Run a speed test?" : L"Measurement details";
    render_target_->DrawText(title, static_cast<UINT32>(wcslen(title)), format_dashboard_value_, dialog.title, brush_text_primary_);
    std::wstring text;
    auto body = dialog.body;
    if (speed_consent_open_) {
        text = SpeedTestPresentation::consent_text;
    } else {
        body.bottom = dialog.cancel.top - 18.0f;
        if (!speed_details_result_.error_message.empty()) text = L"Errors\n" + speed_details_result_.error_message;
        if (!speed_details_result_.warning_message.empty()) {
            if (!text.empty()) text += L"\n\n";
            text += L"Measurement notes\n" + speed_details_result_.warning_message;
        }
        if (text.empty()) text = L"A measurement did not complete normally. No further details were returned.";
    }
    // The compact app-row format is NO_WRAP. Copy it into an explicitly
    // wrapped, measured paragraph rather than silently clipping the privacy
    // disclosure (or long error details) at the right edge of the dialog.
    SettingsText::Paragraph paragraph;
    if (SUCCEEDED(SettingsText::CreateParagraph(dwrite_factory_, format_app_body_, text.c_str(),
            body.right - body.left, paragraph))) {
        speed_details_scroll_max_ = std::max(0.0f, paragraph.height - (body.bottom - body.top));
        speed_details_scroll_ = std::clamp(speed_details_scroll_, 0.0f, speed_details_scroll_max_);
        render_target_->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        render_target_->DrawTextLayout(D2D1::Point2F(body.left, body.top - speed_details_scroll_),
            paragraph.layout.Get(), brush_text_secondary_);
        render_target_->PopAxisAlignedClip();
        if (speed_details_scroll_max_ > 0.0f) {
            render_target_->DrawText(L"Scroll for more", 15, format_dashboard_note_,
                D2D1::RectF(body.left, body.bottom, body.right, body.bottom + 16.0f), brush_text_secondary_);
        }
    }
    const auto button = [&](const D2D1_RECT_F& rect, const wchar_t* label, bool primary) {
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(rect, 9.0f, 9.0f), primary ? brush_action_bg_ :
            (IsPointInside(rect, mouse_x_, mouse_y_) ? brush_hover_ : brush_popup_bg_));
        render_target_->DrawRoundedRectangle(D2D1::RoundedRect(rect, 9.0f, 9.0f), brush_card_border_, 1.0f);
        render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_limit_button_, rect,
            primary ? brush_tooltip_text_ : brush_text_primary_);
    };
    if (speed_consent_open_) {
        button(dialog.privacy, L"Read M-Lab privacy policy", false);
        button(dialog.cancel, L"Cancel", false);
        button(dialog.confirm, L"Agree & run", true);
    } else {
        button(dialog.cancel, L"Close", false);
    }
}

void AppRenderer::DrawTitlebar(D2D1_SIZE_F rt_size) {
    D2D1_RECT_F bar_rect = D2D1::RectF(0.0f, 0.0f, rt_size.width, UIStyle::TITLEBAR_HEIGHT);
    render_target_->FillRectangle(bar_rect, brush_sidebar_bg_);

    const TitlebarLayout layout = GetTitlebarLayout(rt_size.width);

    // The same icon used by the Tauri build is embedded in this executable.
    const D2D1_RECT_F logo_rect = D2D1::RectF(16.0f, 13.0f, 38.0f, 35.0f);
    if (titlebar_logo_bitmap_) {
        render_target_->DrawBitmap(
            titlebar_logo_bitmap_,
            logo_rect,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
        );
    }

    D2D1_RECT_F title_rect = D2D1::RectF(52.0f, 10.0f, layout.feedback_left - 10.0f, 38.0f);
    render_target_->DrawText(L"WATCHMAN", 8, format_titlebar_title_, title_rect, brush_text_primary_);

    const D2D1_RECT_F feed_rect = D2D1::RectF(layout.feedback_left, 12.0f, layout.feedback_right, 36.0f);
    const D2D1_ROUNDED_RECT feed_round = D2D1::RoundedRect(feed_rect, 12.0f, 12.0f);
    ID2D1SolidColorBrush* feedback_text_brush = brush_text_primary_;
    if (hovered_titlebar_btn_ == 0) {
        render_target_->FillRoundedRectangle(feed_round, brush_text_primary_);
        const D2D1_COLOR_F inverse = effective_theme_ == ThemeMode::Light
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f);
        render_target_->CreateSolidColorBrush(inverse, &feedback_text_brush);
    }
    render_target_->DrawRoundedRectangle(feed_round, brush_card_border_, 1.0f);
    render_target_->DrawText(L"Feedback", 8, format_titlebar_button_, feed_rect, feedback_text_brush);
    if (feedback_text_brush != brush_text_primary_) {
        feedback_text_brush->Release();
    }

    const auto draw_action = [&](float left, int hover_index, VectorIcon icon) {
        const D2D1_RECT_F rect = D2D1::RectF(left, 8.0f, left + layout.action_size, 40.0f);
        if (hovered_titlebar_btn_ == hover_index) {
            render_target_->DrawRoundedRectangle(
                D2D1::RoundedRect(rect, 16.0f, 16.0f),
                brush_card_border_,
                1.0f
            );
        }

        const float center_x = left + layout.action_size / 2.0f;
        if (hover_index == 4) {
            render_target_->DrawLine(
                D2D1::Point2F(center_x - 5.0f, 24.0f),
                D2D1::Point2F(center_x + 5.0f, 24.0f),
                brush_text_primary_,
                1.2f
            );
        } else if (hover_index == 5) {
            render_target_->DrawLine(D2D1::Point2F(center_x - 5.0f, 19.0f), D2D1::Point2F(center_x + 5.0f, 29.0f), brush_text_primary_, 1.2f);
            render_target_->DrawLine(D2D1::Point2F(center_x + 5.0f, 19.0f), D2D1::Point2F(center_x - 5.0f, 29.0f), brush_text_primary_, 1.2f);
        } else {
            DrawVectorIcon(
                render_target_,
                icon,
                D2D1::RectF(left + 9.0f, 17.0f, left + layout.action_size - 9.0f, 31.0f),
                brush_text_primary_,
                0.9f
            );
        }
    };
    draw_action(layout.globe_left, 1, VectorIcon::Globe);
    draw_action(layout.gear_left, 2, VectorIcon::Gear);
    draw_action(layout.theme_left, 3, effective_theme_ == ThemeMode::Light ? VectorIcon::Moon : VectorIcon::Sun);
    draw_action(layout.minimize_left, 4, VectorIcon::Close);
    draw_action(layout.close_left, 5, VectorIcon::Close);

}

void AppRenderer::DrawTabbar(D2D1_SIZE_F rt_size) {
    float bar_y = UIStyle::TITLEBAR_HEIGHT;
    D2D1_RECT_F bar_rect = D2D1::RectF(0.0f, bar_y, rt_size.width, bar_y + UIStyle::TABBAR_HEIGHT);
    render_target_->FillRectangle(bar_rect, brush_sidebar_bg_);

    // Tauri uses a solid 1px #121212 border for both header dividers. Draw on
    // half pixels so Direct2D produces one crisp physical pixel rather than a
    // two-pixel gray antialiased rule.
    render_target_->DrawLine(
        D2D1::Point2F(0.0f, bar_y + 0.5f),
        D2D1::Point2F(rt_size.width, bar_y + 0.5f),
        brush_text_primary_,
        1.0f
    );
    render_target_->DrawLine(
        D2D1::Point2F(0.0f, bar_y + UIStyle::TABBAR_HEIGHT + 0.5f),
        D2D1::Point2F(rt_size.width, bar_y + UIStyle::TABBAR_HEIGHT + 0.5f),
        brush_text_primary_,
        1.0f
    );

    const wchar_t* tab_names[] = { L"DASHBOARD", L"DATA", L"APPS", L"NETWORK", L"TOOLS" };
    const TabbarLayout layout = GetTabbarLayout(rt_size.width);

    for (int i = 0; i < 5; ++i) {
        bool is_selected = (active_tab_ == static_cast<ActiveTab>(i));
        bool is_hovered = (hovered_tab_item_ == i);
        const float tab_left = layout.items[i].left;
        const float tab_width = layout.items[i].width;
        D2D1_RECT_F tab_rect = D2D1::RectF(
            tab_left,
            bar_y + 6.0f,
            tab_left + tab_width,
            bar_y + UIStyle::TABBAR_HEIGHT - 6.0f
        );

        // The Tauri tab bar paints the selected tab itself. It is not a
        // travelling indicator behind the bar, so each tab owns its fill.
        if (is_selected) {
            D2D1_ROUNDED_RECT active_round = D2D1::RoundedRect(tab_rect, 12.5f, 12.5f);
            render_target_->FillRoundedRectangle(active_round, brush_text_primary_);
        } else if (is_hovered) {
            D2D1_ROUNDED_RECT h_round = D2D1::RoundedRect(tab_rect, 12.5f, 12.5f);
            render_target_->FillRoundedRectangle(h_round, brush_hover_);
        }

        ID2D1SolidColorBrush* text_brush = brush_text_secondary_;
        if (is_selected) {
            D2D1_COLOR_F inv_col = (effective_theme_ == ThemeMode::Light) ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.0f, 0.0f, 0.0f);
            render_target_->CreateSolidColorBrush(inv_col, &text_brush);
        } else if (is_hovered) {
            text_brush = brush_text_primary_;
        }

        const float item_scale = tab_width / MainContentLayout::tab_widths[i];
        const float inset = 10.0f * item_scale;
        const float icon_left = tab_left + inset;
        const float tab_content_top = bar_y + 6.0f;
        const float tab_content_bottom = bar_y + UIStyle::TABBAR_HEIGHT - 6.0f;
        const float icon_top = (tab_content_top + tab_content_bottom - 14.0f) / 2.0f;
        DrawTauriTabIcon(render_target_, i, icon_left, icon_top, text_brush);
        render_target_->DrawText(
            tab_names[i],
            static_cast<UINT32>(wcslen(tab_names[i])),
            format_tab_,
            D2D1::RectF(icon_left + 18.0f * item_scale, tab_content_top, tab_left + tab_width - inset, tab_content_bottom),
            text_brush
        );

        if (is_selected) {
            text_brush->Release();
        }
    }
}

void AppRenderer::DrawStatusbar(D2D1_SIZE_F rt_size, const NetworkSnapshot& net_snap, const DashboardSnapshot& dash_snap) {
    update_footer_visible_ = false;
    float bar_y = rt_size.height - UIStyle::STATUSBAR_HEIGHT;
    D2D1_RECT_F bar_rect = D2D1::RectF(0.0f, bar_y, rt_size.width, rt_size.height);
    render_target_->FillRectangle(bar_rect, brush_sidebar_bg_);

    // Divider top line
    render_target_->DrawLine(
        D2D1::Point2F(0.0f, bar_y),
        D2D1::Point2F(rt_size.width, bar_y),
        brush_card_border_,
        1.0f
    );

    // Only transient startup/widget problems replace the ordinary footer.
    // Full status and numeric errors are available from the tray on demand.
    if (!runtime_status_.empty() && dash_snap.history_save_error.empty()) {
        const auto text_bounds = D2D1::RectF(16.0f, bar_y + 1.0f, rt_size.width - 16.0f, rt_size.height - 1.0f);
        render_target_->DrawText(runtime_status_.c_str(), static_cast<UINT32>(runtime_status_.size()),
            format_status_, text_bounds, brush_text_primary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }

    // Tauri keeps this footer compact: an 8px dot, 10px label, and mono uptime.
    const bool is_connected = net_snap.is_connected;
    ID2D1SolidColorBrush* status_dot_brush = brush_accent_sys_;
    if (!is_connected) {
        render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 0.2f), &status_dot_brush);
    }

    render_target_->FillEllipse(
        D2D1::Ellipse(
            D2D1::Point2F(20.0f, bar_y + UIStyle::STATUSBAR_HEIGHT / 2.0f),
            4.0f,
            4.0f
        ),
        status_dot_brush
    );

    const bool history_failed = !dash_snap.history_save_error.empty();
        const std::wstring status_wstr = history_failed ? L"HISTORY UNSAVED"
            : (is_connected ? L"CONNECTED" : L"DISCONNECTED");
    const D2D1_RECT_F conn_rect = D2D1::RectF(32.0f, bar_y + 1.0f, 160.0f, rt_size.height - 1.0f);
    render_target_->DrawText(status_wstr.c_str(), static_cast<UINT32>(status_wstr.length()), format_status_, conn_rect, history_failed && effective_theme_ == ThemeMode::Dark ? brush_limit_warning_ : brush_text_primary_);

    if (!is_connected) {
        status_dot_brush->Release();
    }

    const uint64_t uptime_sec = dash_snap.uptime_seconds;
    const int hours = static_cast<int>(uptime_sec / 3600);
    const int mins = static_cast<int>((uptime_sec % 3600) / 60);

    std::wstringstream ss;
    ss << L"UPTIME: " << hours << L"H " << mins << L"M";

    const D2D1_RECT_F uptime_rect = D2D1::RectF(rt_size.width - 140.0f, bar_y + 1.0f, rt_size.width - 16.0f, rt_size.height - 1.0f);
    render_target_->DrawText(ss.str().c_str(), static_cast<UINT32>(ss.str().length()), format_status_mono_, uptime_rect, brush_text_primary_);

    if (update_notice_.Available()) {
        const auto link = WatchmanUpdates::Presentation::FooterLink(rt_size.width, rt_size.height);
        if (link.right - link.left >= 120) {
            update_footer_visible_ = true;
            const auto* label = WatchmanUpdates::Presentation::FooterText(rt_size.width);
            ID2D1SolidColorBrush* link_brush = brush_accent_dl_;
            if (effective_theme_ == ThemeMode::Light) {
                ID2D1SolidColorBrush* contrast = nullptr;
                if (SUCCEEDED(render_target_->CreateSolidColorBrush(D2D1::ColorF(0x165CC5), &contrast))) link_brush = contrast;
            }
            render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_update_footer_,
                D2D1::RectF(link.left, link.top, link.right, link.bottom), link_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (link_brush != brush_accent_dl_) link_brush->Release();
        }
    }
}

void AppRenderer::FormatSpeedWithUnit(double bytesPerSec, std::wstring& valStr, std::wstring& unitStr) {
    double val = std::max(0.0, bytesPerSec);
    std::wstring units[] = { L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s" };
    double div = 1024.0;
    if (use_bits_per_second_) {
        val *= 8.0;
        units[0] = L"bps";
        units[1] = L"Kbps";
        units[2] = L"Mbps";
        units[3] = L"Gbps";
        units[4] = L"Tbps";
        div = 1000.0;
    }

    std::wstringstream ss;
    ss << std::fixed;

    if (val < (div * div)) {
        // Less than 1MB/s -> format as KB/s with 1 decimal place
        ss << std::setprecision(1) << (val / div);
        valStr = ss.str();
        unitStr = units[1];
    } else if (val < (div * div * div)) {
        // Less than 1GB/s -> format as MB/s with 2 decimal places
        ss << std::setprecision(2) << (val / (div * div));
        valStr = ss.str();
        unitStr = units[2];
    } else {
        ss << std::setprecision(2) << (val / (div * div * div));
        valStr = ss.str();
        unitStr = units[3];
    }
}

void AppRenderer::DrawDashboard(
    D2D1_SIZE_F rt_size,
    const DashboardSnapshot& dash_snap,
    const NetworkSnapshot& net_snap,
    const TrafficHistorySnapshot& hist_snap
) {
    const DashboardLayout layout = GetDashboardLayout(
        rt_size.width,
        dashboard_network_details_open_,
        dashboard_system_info_open_,
        hide_hardware_gauges_
    );
    const float start_x = layout.start_x;
    const float content_w = layout.content_w;
    const auto is_hovered = [this](const D2D1_RECT_F& rect) {
        return mouse_x_ >= rect.left && mouse_x_ <= rect.right &&
               mouse_y_ >= rect.top && mouse_y_ <= rect.bottom;
    };
    // The Tauri dashboard leaves these surfaces open to the app gradient in
    // light mode. Only their border is always visible; a fill appears on hover.
    const auto draw_outline_surface = [&](const D2D1_RECT_F& rect, float radius, bool hover, bool open = false) {
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        if (effective_theme_ != ThemeMode::Light) {
            render_target_->FillRoundedRectangle(rounded, hover ? brush_hover_ : brush_card_bg_);
        } else if (hover || open) {
            render_target_->FillRoundedRectangle(rounded, brush_hover_);
        }
        render_target_->DrawRoundedRectangle(rounded, brush_card_border_, 1.0f);
    };

    const auto draw_speed_card = [&](float top, const wchar_t* label, const wchar_t* arrow, ID2D1SolidColorBrush* icon_brush, double speed_bps) {
        const D2D1_RECT_F card = D2D1::RectF(start_x, top, start_x + content_w, top + layout.speed_h);
        draw_outline_surface(card, layout.speed_h / 2.0f, is_hovered(card));
        render_target_->DrawText(
            arrow,
            1,
            format_speed_arrow_,
            D2D1::RectF(start_x + 20.0f, top + 16.0f, start_x + 45.0f, top + 52.0f),
            icon_brush
        );
        render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_body_bold_, D2D1::RectF(start_x + 54.0f, top + 23.0f, start_x + 220.0f, top + 48.0f), brush_text_primary_);

        std::wstring value;
        std::wstring unit;
        if (WatchmanLiveReadings::NetworkReady(dash_snap)) FormatSpeedWithUnit(speed_bps / 8.0, value, unit);
        else value = L"--";

        float value_width = static_cast<float>(value.size()) * 25.0f;
        IDWriteTextLayout* value_layout = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()), format_large_speed_, 180.0f, 56.0f, &value_layout))) {
            DWRITE_TEXT_METRICS metrics{};
            value_layout->GetMetrics(&metrics);
            value_width = metrics.width;
            value_layout->Release();
        }
        // Reserve a stable unit column. The old text box was too narrow and
        // let `KB/s` wrap onto a second line at idle speeds.
        const float right_edge = start_x + content_w - 16.0f;
        const float unit_width = 42.0f;
        const float value_x = std::max(start_x + 220.0f, right_edge - unit_width - 6.0f - value_width);
        render_target_->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format_large_speed_, D2D1::RectF(value_x, top + 3.0f, value_x + value_width + 4.0f, top + layout.speed_h), brush_text_primary_);
        render_target_->DrawText(unit.c_str(), static_cast<UINT32>(unit.size()), format_unit_speed_, D2D1::RectF(value_x + value_width + 6.0f, top + 25.0f, right_edge, top + 49.0f), brush_text_secondary_);
    };

    draw_speed_card(layout.start_y, L"DOWNLOAD", L"\u2193", brush_accent_dl_, dash_snap.download_speed_bps);
    draw_speed_card(layout.start_y + layout.speed_h + layout.gap, L"UPLOAD", L"\u2191", brush_accent_ul_, dash_snap.upload_speed_bps);

    const D2D1_RECT_F session_card = D2D1::RectF(start_x, layout.session_y, start_x + content_w, layout.session_y + layout.session_h);
    DrawCard(session_card, is_hovered(session_card), CardSurface::Session);
    render_target_->DrawText(L"CURRENT SESSION", 15, format_body_bold_, D2D1::RectF(start_x + 16.0f, layout.session_y + 14.0f, start_x + 260.0f, layout.session_y + 33.0f), brush_text_primary_);
    constexpr wchar_t session_note[] = L"Since app opened or last reset";
    render_target_->DrawText(session_note, static_cast<UINT32>(std::size(session_note) - 1), format_dashboard_note_, D2D1::RectF(start_x + 16.0f, layout.session_y + 32.0f, start_x + 260.0f, layout.session_y + 48.0f), brush_text_secondary_);

    const float reset_w = 106.0f;
    const D2D1_RECT_F reset_rect = D2D1::RectF(start_x + content_w - reset_w - 16.0f, layout.session_y + 13.0f, start_x + content_w - 16.0f, layout.session_y + 41.0f);
    const D2D1_ROUNDED_RECT reset_round = D2D1::RoundedRect(reset_rect, 14.0f, 14.0f);
    render_target_->FillRoundedRectangle(reset_round, is_hovered(reset_rect) ? brush_accent_dl_ : brush_text_primary_);
    render_target_->DrawRoundedRectangle(reset_round, brush_card_border_, 1.0f);
    render_target_->DrawText(L"RESET SESSION", 13, format_limit_button_, reset_rect, brush_active_text_);

    const float stat_y = layout.session_y + 62.0f;
    const float stat_w = (content_w - 32.0f) / 3.0f;
    const auto draw_session_stat = [&](int index, const wchar_t* label, const std::wstring& value, ID2D1SolidColorBrush* value_brush) {
        const float left = start_x + 16.0f + stat_w * static_cast<float>(index);
        render_target_->DrawLine(D2D1::Point2F(left, stat_y), D2D1::Point2F(left + stat_w - 8.0f, stat_y), brush_chart_grid_, 1.0f);
        render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_dashboard_label_, D2D1::RectF(left, stat_y + 10.0f, left + stat_w, stat_y + 25.0f), brush_text_secondary_);
        render_target_->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format_dashboard_value_, D2D1::RectF(left, stat_y + 27.0f, left + stat_w, stat_y + 55.0f), value_brush);
    };
    draw_session_stat(0, L"DOWNLOAD", FormatBytes(dash_snap.session_download_bytes), brush_text_primary_);
    draw_session_stat(1, L"UPLOAD", FormatBytes(dash_snap.session_upload_bytes), brush_text_primary_);
    draw_session_stat(2, L"TOTAL", FormatBytes(dash_snap.session_download_bytes + dash_snap.session_upload_bytes), brush_accent_dl_);

    const D2D1_RECT_F snapshot_card = D2D1::RectF(start_x, layout.snapshot_y, start_x + content_w, layout.snapshot_y + layout.snapshot_h);
    DrawCard(snapshot_card, is_hovered(snapshot_card), CardSurface::Snapshot);
    render_target_->DrawText(L"USAGE SNAPSHOT", 14, format_body_bold_, D2D1::RectF(start_x + 16.0f, layout.snapshot_y + 15.0f, start_x + 250.0f, layout.snapshot_y + 34.0f), brush_text_primary_);

    const float chip_w = DashboardUsageChipWidth(dashboard_usage_period_idx_);
    const D2D1_RECT_F chip_rect = D2D1::RectF(start_x + content_w - chip_w - 16.0f, layout.snapshot_y + 14.0f, start_x + content_w - 16.0f, layout.snapshot_y + 42.0f);
    const D2D1_ROUNDED_RECT chip_round = D2D1::RoundedRect(chip_rect, 14.0f, 14.0f);
    render_target_->FillRoundedRectangle(chip_round, is_hovered(chip_rect) ? brush_hover_ : brush_picker_month_bg_);
    render_target_->DrawRoundedRectangle(chip_round, brush_picker_month_border_, 1.0f);
    const wchar_t* chip_label = DashboardUsagePeriodLabel(dashboard_usage_period_idx_);
    render_target_->DrawText(chip_label, static_cast<UINT32>(wcslen(chip_label)), format_data_filter_, chip_rect, brush_text_primary_);

    const DashboardUsageTotals usage = GetDashboardUsageTotals(dashboard_usage_period_idx_, hist_snap, dash_snap);
    const float snapshot_stat_y = layout.snapshot_y + 58.0f;
    const auto draw_snapshot_stat = [&](int index, const wchar_t* label, const std::wstring& value) {
        const float left = start_x + 16.0f + stat_w * static_cast<float>(index);
        render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_dashboard_label_, D2D1::RectF(left, snapshot_stat_y, left + stat_w, snapshot_stat_y + 16.0f), brush_text_secondary_);
        render_target_->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format_dashboard_value_, D2D1::RectF(left, snapshot_stat_y + 17.0f, left + stat_w, snapshot_stat_y + 47.0f), brush_text_primary_);
    };
    draw_snapshot_stat(0, L"TOTAL", FormatBytes(usage.download + usage.upload));
    draw_snapshot_stat(1, L"\u2193 DOWNLOAD", FormatBytes(usage.download));
    draw_snapshot_stat(2, L"\u2191 UPLOAD", FormatBytes(usage.upload));
    const std::wstring snapshot_note = dash_snap.history_save_error.empty()
        ? L"Saved history updates periodically" : L"History save failed; retrying";
    render_target_->DrawText(snapshot_note.c_str(), static_cast<UINT32>(snapshot_note.size()), format_dashboard_note_,
        D2D1::RectF(start_x + 16.0f, layout.snapshot_y + 111.0f, start_x + content_w - 16.0f, layout.snapshot_y + 131.0f), brush_text_secondary_);

    const D2D1_RECT_F status_rect = D2D1::RectF(start_x, layout.status_y, start_x + content_w, layout.status_y + layout.status_h);
    draw_outline_surface(status_rect, layout.status_h / 2.0f, is_hovered(status_rect));
    render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(start_x + 39.0f, layout.status_y + 30.0f), 10.0f, 10.0f), brush_accent_sys_);
    const bool history_failed = !dash_snap.history_save_error.empty();
    const std::wstring status_title = history_failed ? L"HISTORY SAVE FAILED" :
        WatchmanLiveReadings::NetworkReady(dash_snap) ? L"MONITORING" : L"STARTING";
    const std::wstring status_note = history_failed
        ? L"Unsaved usage is in memory; keep Watchman open."
        : WatchmanLiveReadings::NetworkReady(dash_snap) ? L"Traffic is measured on the selected adapter."
        : L"Waiting for the first monitoring sample.";
    render_target_->DrawText(status_title.c_str(), static_cast<UINT32>(status_title.size()), format_body_bold_, D2D1::RectF(start_x + 73.0f, layout.status_y + 12.0f, start_x + content_w - 18.0f, layout.status_y + 32.0f), history_failed && effective_theme_ == ThemeMode::Dark ? brush_limit_warning_ : brush_text_primary_);
    render_target_->DrawText(status_note.c_str(), static_cast<UINT32>(status_note.size()), format_dashboard_note_, D2D1::RectF(start_x + 73.0f, layout.status_y + 33.0f, start_x + content_w - 18.0f, layout.status_y + 52.0f), brush_text_secondary_);

    if (!hide_hardware_gauges_) {
    const float gauge_gap = 10.0f;
    const float gauge_w = (content_w - gauge_gap * 2.0f) / 3.0f;
    const auto draw_gauge_card = [&](int index,
                                     float display_value,
                                     float animated_fill_value,
                                     const wchar_t* label,
                                     const std::wstring& subtitle,
                                     D2D1_COLOR_F fill_color,
                                     bool available = true) {
        const float left = start_x + static_cast<float>(index) * (gauge_w + gauge_gap);
        const D2D1_RECT_F card = D2D1::RectF(left, layout.gauges_y, left + gauge_w, layout.gauges_y + layout.gauges_h);
        draw_outline_surface(card, 24.0f, is_hovered(card));
        std::wstringstream value_stream;
        value_stream << std::fixed << std::setprecision(0) << std::clamp(display_value, 0.0f, 100.0f) << L"%";
        DrawRadialGauge(
            D2D1::Point2F(left + gauge_w / 2.0f, layout.gauges_y + 45.0f),
            28.0f,
            available ? animated_fill_value / 100.0f : 0.0f,
            fill_color,
            available ? value_stream.str() : L"--",
            label
        );
        render_target_->DrawText(subtitle.c_str(), static_cast<UINT32>(subtitle.size()), format_data_label_, D2D1::RectF(left + 8.0f, layout.gauges_y + 104.0f, left + gauge_w - 8.0f, layout.gauges_y + 120.0f), brush_text_secondary_);
    };

    const auto sensor_now = GetTickCount64();
    const auto& cpu_temperature = dash_snap.temperatures.readings[static_cast<size_t>(WatchmanSensors::Kind::Cpu)];
    const auto& gpu_temperature = dash_snap.temperatures.readings[static_cast<size_t>(WatchmanSensors::Kind::Gpu)];
    const auto compact_temperature = [&](const WatchmanSensors::Reading& reading) {
        return WatchmanSensors::Fresh(reading, sensor_now) ? WatchmanSensors::Format(reading, sensor_now) : L"--\u00B0C";
    };
    const std::wstring cpu_subtitle = compact_temperature(cpu_temperature) + L" \u00B7 " +
        (dash_snap.cpu_core_count > 0 ? std::to_wstring(dash_snap.cpu_core_count) + L" threads" : L"-- threads");
    std::wstringstream ram_stream;
    ram_stream << std::fixed << std::setprecision(1)
               << static_cast<double>(dash_snap.ram_used_bytes) / (1024.0 * 1024.0 * 1024.0)
               << L" / "
               << static_cast<double>(dash_snap.ram_total_bytes) / (1024.0 * 1024.0 * 1024.0)
               << L" GB";
    const std::wstring gpu_subtitle = compact_temperature(gpu_temperature) + L" \u00B7 " +
        (!dash_snap.gpu_active_engine.empty() ? dash_snap.gpu_active_engine
            : (!dash_snap.gpu_available ? L"Unavailable" : L"Idle"));
    draw_gauge_card(0, dash_snap.cpu_usage_percent, anim_cpu_usage_, L"CPU", cpu_subtitle, palette_.AccentSystem,
        CpuReadingAvailable(dash_snap));
    draw_gauge_card(1, dash_snap.ram_usage_percent, anim_ram_usage_, L"RAM", WatchmanLiveReadings::MemoryReady(dash_snap, sensor_now) ? ram_stream.str() : L"Waiting for reading",
        D2D1::ColorF(0.16f, 0.37f, 0.78f), WatchmanLiveReadings::MemoryReady(dash_snap, sensor_now));
    draw_gauge_card(2, dash_snap.gpu_usage_percent, anim_gpu_usage_, L"GPU", gpu_subtitle, D2D1::ColorF(0.52f, 0.32f, 0.72f),
        WatchmanLiveReadings::GpuReady(dash_snap));
    }

    const std::vector<std::pair<std::wstring, std::wstring>> network_rows = {
        { L"INTERFACE", net_snap.is_connected ? TruncateDashboardText(net_snap.active_adapter_name) : L"No network found" },
        { L"TYPE", net_snap.is_connected ? net_snap.active_adapter_type : L"N/A" },
        { L"IP ADDRESS", net_snap.is_connected ? net_snap.local_ip : L"N/A" },
        { L"SUBNET MASK", net_snap.is_connected ? net_snap.subnet_mask : L"N/A" },
        { L"IPV6", net_snap.is_connected ? TruncateDashboardText(net_snap.ipv6_address) : L"N/A" },
        { L"MAC ADDRESS", net_snap.is_connected ? net_snap.mac_address : L"N/A" },
        { L"SPEED", net_snap.is_connected ? FormatDashboardLinkSpeed(net_snap.link_speed_bps) : L"N/A" },
        { L"STATUS", net_snap.is_connected ? L"Up" : L"Disconnected" },
        { L"DHCP", net_snap.is_connected ? (net_snap.dhcp_enabled ? L"Yes" : L"No") : L"N/A" },
    };
    std::wstringstream cpu_speed;
    if (dash_snap.cpu_speed_ghz > 0.0f) {
        cpu_speed << std::fixed << std::setprecision(2) << dash_snap.cpu_speed_ghz << L" GHz";
    } else {
        cpu_speed << L"N/A";
    }
    const auto temperature_detail = [&](WatchmanSensors::Kind kind) {
        const auto& reading = dash_snap.temperatures.readings[static_cast<size_t>(kind)];
        const auto now = GetTickCount64();
        if (!WatchmanSensors::Fresh(reading, now)) return std::wstring(L"Unavailable - see Settings > Warnings");
        return WatchmanSensors::Format(reading, now, 1) + L" \u00B7 " + reading.sensor_name + L" \u00B7 " + reading.source;
    };
    const std::vector<std::pair<std::wstring, std::wstring>> system_rows = {
        { L"OS", TruncateDashboardText(dash_snap.os_name) },
        { L"ARCHITECTURE", dash_snap.architecture },
        { L"HOSTNAME", dash_snap.hostname },
        { L"CPU", TruncateDashboardText(dash_snap.cpu_name) },
        { L"CPU THREADS", dash_snap.cpu_core_count > 0 ? std::to_wstring(dash_snap.cpu_core_count) : L"N/A" },
        { L"CPU SPEED", cpu_speed.str() },
        { L"CPU TEMPERATURE (MAX)", temperature_detail(WatchmanSensors::Kind::Cpu) },
        { L"GPU TEMPERATURE (MAX)", temperature_detail(WatchmanSensors::Kind::Gpu) },
        { L"DISK TEMPERATURE (MAX)", temperature_detail(WatchmanSensors::Kind::Disk) },
        { L"MAINBOARD TEMPERATURE", temperature_detail(WatchmanSensors::Kind::Mainboard) },
        { L"SENSOR BACKEND", dash_snap.temperatures.scanning ? L"Checking sensors..."
            : (dash_snap.temperatures.provider_connected ? L"LibreHardwareMonitor (local, read-only)"
                : L"LHM offline; Windows GPU driver if supported") },
        { L"UPTIME", FormatDashboardUptime(dash_snap.uptime_seconds) },
    };

    const auto draw_details_panel = [&](float top, bool open, const wchar_t* title, const std::vector<std::pair<std::wstring, std::wstring>>& rows) {
        const float height = 56.0f + (open ? static_cast<float>(rows.size()) * 51.0f + 10.0f : 0.0f);
        const D2D1_RECT_F panel = D2D1::RectF(start_x, top, start_x + content_w, top + height);
        const D2D1_RECT_F toggle = D2D1::RectF(start_x, top, start_x + content_w, top + 56.0f);
        draw_outline_surface(panel, open ? UIStyle::CORNER_RADIUS_LARGE : 28.0f, is_hovered(toggle), open);
        const wchar_t* arrow = open ? L"\u2193" : L"\u2192";
        render_target_->DrawText(arrow, 1, format_body_bold_, D2D1::RectF(start_x + 19.0f, top + 19.0f, start_x + 40.0f, top + 40.0f), brush_text_primary_);
        render_target_->DrawText(title, static_cast<UINT32>(wcslen(title)), format_body_bold_, D2D1::RectF(start_x + 48.0f, top + 18.0f, start_x + content_w - 20.0f, top + 40.0f), brush_text_primary_);

        if (!open) {
            return;
        }

        render_target_->DrawLine(D2D1::Point2F(start_x + 1.0f, top + 56.0f), D2D1::Point2F(start_x + content_w - 1.0f, top + 56.0f), brush_chart_grid_, 1.0f);
        float row_y = top + 65.0f;
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            render_target_->DrawText(row.first.c_str(), static_cast<UINT32>(row.first.size()), format_dashboard_label_, D2D1::RectF(start_x + 20.0f, row_y, start_x + content_w - 20.0f, row_y + 15.0f), brush_text_secondary_);
            const std::wstring value = TruncateDashboardText(row.second, 52);
            render_target_->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format_body_bold_, D2D1::RectF(start_x + 20.0f, row_y + 17.0f, start_x + content_w - 20.0f, row_y + 36.0f), brush_text_primary_);
            if (i + 1 < rows.size()) {
                render_target_->DrawLine(D2D1::Point2F(start_x + 20.0f, row_y + 43.0f), D2D1::Point2F(start_x + content_w - 20.0f, row_y + 43.0f), brush_chart_grid_, 1.0f);
            }
            row_y += 51.0f;
        }
    };

    draw_details_panel(layout.network_y, dashboard_network_details_open_, L"NETWORK DETAILS", network_rows);
    draw_details_panel(layout.system_y, dashboard_system_info_open_, L"SYSTEM INFORMATION", system_rows);
    ReportContentBottom(layout.bottom);
}

void AppRenderer::DrawRadialGauge(D2D1_POINT_2F center, float radius, float percentage, D2D1_COLOR_F color, const std::wstring& value_str, const std::wstring& label_str) {
    percentage = std::clamp(percentage, 0.0f, 1.0f);
    const D2D1_ELLIPSE ellipse = D2D1::Ellipse(center, radius, radius);

    // Tauri fills the lower part of each gauge instead of drawing an arc.
    // Clip a simple rectangle to the circle so the native version carries the
    // same quiet, physical-meter feeling.
    ID2D1EllipseGeometry* ellipse_geometry = nullptr;
    ID2D1Layer* fill_layer = nullptr;
    if (SUCCEEDED(d2d_factory_->CreateEllipseGeometry(ellipse, &ellipse_geometry)) &&
        SUCCEEDED(render_target_->CreateLayer(nullptr, &fill_layer))) {
        D2D1_COLOR_F fill_color = color;
        fill_color.a = effective_theme_ == ThemeMode::Light ? 0.18f : 0.35f;
        ID2D1SolidColorBrush* fill_brush = nullptr;
        if (SUCCEEDED(render_target_->CreateSolidColorBrush(fill_color, &fill_brush))) {
            const D2D1_LAYER_PARAMETERS layer_parameters = D2D1::LayerParameters(
                D2D1::InfiniteRect(),
                ellipse_geometry,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(),
                1.0f,
                nullptr,
                D2D1_LAYER_OPTIONS_NONE
            );
            render_target_->PushLayer(layer_parameters, fill_layer);
            const float fill_top = center.y + radius - (radius * 2.0f * percentage);
            render_target_->FillRectangle(D2D1::RectF(center.x - radius, fill_top, center.x + radius, center.y + radius), fill_brush);
            render_target_->PopLayer();
            fill_brush->Release();
        }
    }
    if (fill_layer != nullptr) {
        fill_layer->Release();
    }
    if (ellipse_geometry != nullptr) {
        ellipse_geometry->Release();
    }

    render_target_->DrawEllipse(ellipse, brush_card_border_, 1.5f);
    render_target_->DrawText(value_str.c_str(), static_cast<UINT32>(value_str.length()), format_values_, D2D1::RectF(center.x - 36.0f, center.y - 11.0f, center.x + 36.0f, center.y + 14.0f), brush_text_primary_);

    render_target_->DrawText(
        label_str.c_str(),
        static_cast<UINT32>(label_str.length()),
        format_dashboard_gauge_,
        D2D1::RectF(center.x - 48.0f, center.y + radius + 7.0f, center.x + 48.0f, center.y + radius + 25.0f),
        brush_text_primary_
    );
}

void AppRenderer::DrawData(D2D1_SIZE_F rt_size, const TrafficHistorySnapshot& hist_snap) {
    const DataTabLayout layout = GetDataTabLayout(
        rt_size.width,
        rt_size.height
    );
    const float start_x = layout.start_x;
    const float content_w = layout.content_w;
    const float history_x = start_x + 20.0f;
    const float history_w = std::max(0.0f, content_w - 40.0f);
    const auto draw_data_surface = [&](const D2D1_RECT_F& rect, float radius) {
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        render_target_->FillRoundedRectangle(rounded, brush_card_bg_);
        render_target_->DrawRoundedRectangle(rounded, brush_card_border_, 1.0f);
    };

    time_t rawtime = time(nullptr);
    tm now_tm{};
    localtime_s(&now_tm, &rawtime);

    const int current_year = now_tm.tm_year + 1900;
    const int current_month = now_tm.tm_mon + 1;
    const std::wstring today_str = MakeDateKey(current_year, current_month, now_tm.tm_mday);

    if (data_history_year_ == 0) {
        data_history_year_ = current_year;
    }
    if (data_history_month_ == 0) {
        data_history_month_ = current_month;
    }

    std::map<std::wstring, std::pair<uint64_t, uint64_t>> daily_totals;
    for (const auto& record : hist_snap.history) {
        auto& totals = daily_totals[record.date];
        totals.first += record.rx_bytes;
        totals.second += record.tx_bytes;
    }

    std::map<std::wstring, std::pair<uint64_t, uint64_t>> monthly_totals;
    for (const auto& record : hist_snap.monthly_history) {
        auto& totals = monthly_totals[record.date];
        totals.first += record.rx_bytes;
        totals.second += record.tx_bytes;
    }

    auto daily_value = [&](const std::wstring& key) {
        const auto it = daily_totals.find(key);
        return it == daily_totals.end() ? std::pair<uint64_t, uint64_t>{0, 0} : it->second;
    };
    auto monthly_value = [&](const std::wstring& key) {
        const auto it = monthly_totals.find(key);
        return it == monthly_totals.end() ? std::pair<uint64_t, uint64_t>{0, 0} : it->second;
    };

    std::vector<DataChartBucket> buckets;
    auto append_bucket = [&](std::wstring key, std::wstring label, uint64_t download, uint64_t upload) {
        buckets.push_back({ std::move(key), std::move(label), download, upload });
    };

    if (data_range_idx_ == 0) {
        const auto [download, upload] = daily_value(today_str);
        append_bucket(today_str, L"Today", download, upload);
    } else if (data_range_idx_ == 1) {
        // TrafficHistoryChart keeps the newest day at the left, matching the
        // source filterData() order for the Last 7 Days view.
        for (int days_ago = 0; days_ago < 7; ++days_ago) {
            const std::wstring key = DateDaysAgo(days_ago);
            const auto [download, upload] = daily_value(key);
            int label_year = 0;
            int label_month = 0;
            int label_day = 0;
            const std::wstring label = ParseIsoDate(key, &label_year, &label_month, &label_day)
                ? std::wstring(kShortMonthNames[label_month - 1]) + L" " + std::to_wstring(label_day)
                : key;
            append_bucket(key, label, download, upload);
        }
    } else if (data_range_idx_ == 2) {
        const int days = DaysInMonth(data_history_year_, data_history_month_);
        for (int day = 1; day <= days; ++day) {
            const std::wstring key = MakeDateKey(data_history_year_, data_history_month_, day);
            const auto [download, upload] = daily_value(key);
            append_bucket(key, std::to_wstring(day), download, upload);
        }
    } else {
        for (int month = 1; month <= 12; ++month) {
            const std::wstring key = MakeMonthKey(data_history_year_, month);
            const auto [download, upload] = monthly_value(key);
            append_bucket(key, kShortMonthNames[month - 1], download, upload);
        }
    }

    uint64_t selected_dl = 0;
    uint64_t selected_ul = 0;
    for (const auto& bucket : buckets) {
        selected_dl += bucket.download;
        selected_ul += bucket.upload;
    }

    const auto [current_month_dl, current_month_ul] = monthly_value(MakeMonthKey(current_year, current_month));
    const uint64_t current_month_total = current_month_dl + current_month_ul;

    const uint64_t selected_total = selected_dl + selected_ul;

    uint64_t usage_download = 0;
    uint64_t usage_upload = 0;
    if (data_usage_period_idx_ == 0) {
        const auto [download, upload] = daily_value(today_str);
        usage_download = download;
        usage_upload = upload;
    } else if (data_usage_period_idx_ == 1) {
        // Watchman-main aggregates `weekly` by ISO week, not a rolling seven
        // days. Summing Monday through today produces the same current week.
        const int days_since_monday = (now_tm.tm_wday + 6) % 7;
        for (int days_ago = 0; days_ago <= days_since_monday; ++days_ago) {
            const auto [download, upload] = daily_value(DateDaysAgo(days_ago));
            usage_download += download;
            usage_upload += upload;
        }
    } else {
        usage_download = current_month_dl;
        usage_upload = current_month_ul;
    }
    const uint64_t usage_total = usage_download + usage_upload;

    // The source comparison follows the outer Daily/Weekly/Monthly selector,
    // while the inner history controls only the chart and its statistics.
    uint64_t previous_total = 0;
    if (data_usage_period_idx_ == 0) {
        const auto [download, upload] = daily_value(DateDaysAgo(1));
        previous_total = download + upload;
    } else if (data_usage_period_idx_ == 1) {
        const int days_since_monday = (now_tm.tm_wday + 6) % 7;
        for (int offset = 0; offset < 7; ++offset) {
            const auto [download, upload] = daily_value(DateDaysAgo(days_since_monday + 7 + offset));
            previous_total += download + upload;
        }
    } else {
        int previous_year = current_year;
        int previous_month = current_month - 1;
        if (previous_month == 0) {
            previous_month = 12;
            --previous_year;
        }
        const auto [download, upload] = monthly_value(MakeMonthKey(previous_year, previous_month));
        previous_total = download + upload;
    }

    const float period_y = layout.usage_filter_y;
    const D2D1_RECT_F period_rect = D2D1::RectF(start_x, period_y, start_x + content_w, period_y + 36.0f);
    const D2D1_ROUNDED_RECT period_round = D2D1::RoundedRect(period_rect, 18.0f, 18.0f);
    render_target_->FillRoundedRectangle(period_round, brush_sidebar_bg_);
    render_target_->DrawRoundedRectangle(period_round, brush_card_border_, 1.0f);

    const wchar_t* usage_periods[] = { L"DAILY", L"WEEKLY", L"MONTHLY" };
    const float usage_period_width = content_w / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F segment = D2D1::RectF(
            start_x + i * usage_period_width + 3.0f,
            period_y + 3.0f,
            start_x + (i + 1) * usage_period_width - 3.0f,
            period_y + 33.0f
        );
        const bool selected = i == data_usage_period_idx_;
        const bool hovered = mouse_x_ >= segment.left && mouse_x_ <= segment.right &&
                             mouse_y_ >= segment.top && mouse_y_ <= segment.bottom;
        if (selected || hovered) {
            const D2D1_ROUNDED_RECT segment_round = D2D1::RoundedRect(segment, 15.0f, 15.0f);
            render_target_->FillRoundedRectangle(segment_round, selected ? brush_text_primary_ : brush_hover_);
        }
        render_target_->DrawText(
            usage_periods[i],
            static_cast<UINT32>(wcslen(usage_periods[i])),
            format_data_period_,
            segment,
            selected ? brush_active_text_ : brush_text_primary_
        );
    }

    const float usage_card_y = layout.usage_summary_y;
    const float usage_card_width = (content_w - 12.0f) / 3.0f;
    const auto draw_usage_card = [&](int column, const wchar_t* label, uint64_t value) {
        const float left = start_x + column * (usage_card_width + 6.0f);
        const D2D1_RECT_F card = D2D1::RectF(left, usage_card_y, left + usage_card_width, usage_card_y + layout.usage_summary_h);
        draw_data_surface(card, 16.0f);
        render_target_->DrawText(
            label,
            static_cast<UINT32>(wcslen(label)),
            format_data_label_,
            D2D1::RectF(left + 8.0f, usage_card_y + 10.0f, left + usage_card_width - 8.0f, usage_card_y + 24.0f),
            brush_text_primary_
        );
        const std::wstring formatted = FormatBytes(value);
        render_target_->DrawText(
            formatted.c_str(),
            static_cast<UINT32>(formatted.length()),
            format_values_,
            D2D1::RectF(left + 8.0f, usage_card_y + 25.0f, left + usage_card_width - 8.0f, usage_card_y + layout.usage_summary_h - 7.0f),
            brush_text_primary_
        );
    };
    draw_usage_card(0, L"TOTAL USAGE", usage_total);
    draw_usage_card(1, L"\u2193 DOWNLOAD", usage_download);
    draw_usage_card(2, L"\u2191 UPLOAD", usage_upload);

    // The history selector is deliberately independent of the usage period above.
    draw_data_surface(D2D1::RectF(start_x, layout.outer_y, start_x + content_w, layout.outer_bottom), 24.0f);

    // Inner history summary: total, download, upload.
    const float cards_y = layout.summary_y;
    const float history_card_width = (history_w - 12.0f) / 3.0f;
    const auto draw_history_card = [&](int column, const wchar_t* label, uint64_t value) {
        const float left = history_x + column * (history_card_width + 6.0f);
        const D2D1_RECT_F card = D2D1::RectF(left, cards_y, left + history_card_width, cards_y + layout.summary_h);
        draw_data_surface(card, 16.0f);
        render_target_->DrawText(
            label,
            static_cast<UINT32>(wcslen(label)),
            format_data_label_,
            D2D1::RectF(left + 8.0f, cards_y + 10.0f, left + history_card_width - 8.0f, cards_y + 24.0f),
            brush_text_primary_
        );
        const std::wstring formatted = FormatBytes(value);
        render_target_->DrawText(
            formatted.c_str(),
            static_cast<UINT32>(formatted.length()),
            format_values_,
            D2D1::RectF(left + 8.0f, cards_y + 25.0f, left + history_card_width - 8.0f, cards_y + layout.summary_h - 7.0f),
            brush_text_primary_
        );
    };
    draw_history_card(0, L"TOTAL TRAFFIC", selected_total);
    draw_history_card(1, L"\u2193 DOWNLOAD", selected_dl);
    draw_history_card(2, L"\u2191 UPLOAD", selected_ul);

    // Time period toggle segmented control (TODAY | LAST 7 DAYS | MONTHLY | YEARLY)
    float time_y = layout.filter_y;
    float time_w = history_w;
    D2D1_RECT_F time_rect = D2D1::RectF(history_x, time_y, history_x + time_w, time_y + 40.0f);
    D2D1_ROUNDED_RECT time_round = D2D1::RoundedRect(time_rect, 20.0f, 20.0f);
    render_target_->FillRoundedRectangle(time_round, brush_sidebar_bg_);
    render_target_->DrawRoundedRectangle(time_round, brush_card_border_, 1.0f);

    const wchar_t* time_options[] = { L"TODAY", L"LAST 7 DAYS", L"MONTHLY", L"YEARLY" };
    float t_w = time_w / 4.0f;
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F p_rect = D2D1::RectF(history_x + (i * t_w) + 4.0f, time_y + 4.0f, history_x + ((i + 1) * t_w) - 4.0f, time_y + 36.0f);
        const bool hovered = mouse_x_ >= p_rect.left && mouse_x_ <= p_rect.right &&
                             mouse_y_ >= p_rect.top && mouse_y_ <= p_rect.bottom;
        if (i == data_range_idx_) {
            D2D1_ROUNDED_RECT act_round = D2D1::RoundedRect(p_rect, 15.0f, 15.0f);
            render_target_->FillRoundedRectangle(act_round, brush_text_primary_);
            render_target_->DrawText(time_options[i], static_cast<UINT32>(wcslen(time_options[i])), format_data_filter_, p_rect, brush_active_text_);
        } else {
            if (hovered) {
                const D2D1_ROUNDED_RECT hover_round = D2D1::RoundedRect(p_rect, 15.0f, 15.0f);
                render_target_->FillRoundedRectangle(hover_round, brush_hover_);
            }
            render_target_->DrawText(time_options[i], static_cast<UINT32>(wcslen(time_options[i])), format_data_filter_, p_rect, brush_text_primary_);
        }
    }

    // The date picker is an explicit trigger for monthly and yearly views only.
    if (data_range_idx_ >= 2) {
        const float trigger_width = 132.0f;
        const float range_x = history_x + (history_w - trigger_width) / 2.0f;
        const D2D1_RECT_F range_rect = D2D1::RectF(
            range_x,
            layout.range_y,
            range_x + trigger_width,
            layout.range_y + 38.0f
        );
        const bool trigger_hovered = mouse_x_ >= range_rect.left && mouse_x_ <= range_rect.right &&
                                     mouse_y_ >= range_rect.top && mouse_y_ <= range_rect.bottom;
        const D2D1_ROUNDED_RECT range_round = D2D1::RoundedRect(range_rect, 16.0f, 16.0f);
        render_target_->FillRoundedRectangle(range_round, trigger_hovered ? brush_hover_ : brush_sidebar_bg_);
        render_target_->DrawRoundedRectangle(range_round, brush_card_border_, 1.0f);

        const std::wstring range_label = data_range_idx_ == 2
            ? std::wstring(kLongMonthNames[data_history_month_ - 1]) + L" " + std::to_wstring(data_history_year_)
            : std::to_wstring(data_history_year_);
        DrawVectorIcon(
            render_target_,
            VectorIcon::Calendar,
            D2D1::RectF(range_x + 13.0f, layout.range_y + 9.0f, range_x + 29.0f, layout.range_y + 25.0f),
            brush_text_primary_,
            1.1f
        );
        render_target_->DrawText(
            range_label.c_str(),
            static_cast<UINT32>(range_label.length()),
            format_data_range_,
            D2D1::RectF(range_x + 31.0f, layout.range_y + 2.0f, range_x + trigger_width - 7.0f, layout.range_y + 36.0f),
            brush_text_primary_
        );
    } else {
        std::wstring date_label;
        if (data_range_idx_ == 0) {
            date_label = std::wstring(L"Today, ") + kShortMonthNames[current_month - 1] + L" " +
                std::to_wstring(now_tm.tm_mday) + L", " + std::to_wstring(current_year);
        } else {
            int start_year = 0;
            int start_month = 0;
            int start_day = 0;
            int end_year = 0;
            int end_month = 0;
            int end_day = 0;
            ParseIsoDate(buckets.back().key, &start_year, &start_month, &start_day);
            ParseIsoDate(buckets.front().key, &end_year, &end_month, &end_day);
            if (start_year == end_year) {
                date_label = std::wstring(kShortMonthNames[start_month - 1]) + L" " + std::to_wstring(start_day) +
                    L" - " + kShortMonthNames[end_month - 1] + L" " + std::to_wstring(end_day) +
                    L", " + std::to_wstring(end_year);
            } else {
                date_label = std::wstring(kShortMonthNames[start_month - 1]) + L" " + std::to_wstring(start_day) +
                    L", " + std::to_wstring(start_year) + L" - " + kShortMonthNames[end_month - 1] + L" " +
                    std::to_wstring(end_day) + L", " + std::to_wstring(end_year);
            }
        }
        render_target_->DrawText(
            date_label.c_str(),
            static_cast<UINT32>(date_label.length()),
            format_data_range_,
            D2D1::RectF(history_x, layout.range_y, history_x + history_w, layout.range_y + 44.0f),
            brush_text_secondary_
        );
    }

    // 5. Stacked Bar Chart for historical logs
    float chart_y = layout.chart_y;
    float chart_h = layout.chart_h;
    D2D1_RECT_F chart_card = D2D1::RectF(history_x, chart_y, history_x + history_w, chart_y + chart_h);
    draw_data_surface(chart_card, 16.0f);

    uint64_t max_bucket_total = 0;
    for (const auto& bucket : buckets) {
        max_bucket_total = std::max(max_bucket_total, bucket.download + bucket.upload);
    }
    if (max_bucket_total == 0) {
        max_bucket_total = 1;
    }

    // Keep a visible legend with exactly the colors used by bars/tooltips.
    const auto draw_legend = [&](float left, const wchar_t* label, ID2D1Brush* brush) {
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(left, chart_y + 14.0f, left + 9.0f, chart_y + 23.0f), 2.0f, 2.0f), brush);
        render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_dashboard_note_,
            D2D1::RectF(left + 15.0f, chart_y + 10.0f, left + 100.0f, chart_y + 28.0f), brush_text_secondary_);
    };
    draw_legend(history_x + 70.0f, L"Download", brush_chart_download_);
    draw_legend(history_x + 177.0f, L"Upload", brush_chart_upload_);

    const float plot_left = history_x + 70.0f;
    const float plot_right = history_x + history_w - 20.0f;
    const float plot_top = chart_y + 42.0f;
    const float base_y = chart_y + chart_h - 28.0f;
    const float plot_height = base_y - plot_top;
    float grid_step = plot_height / 5.0f;
    for (int i = 0; i < 5; ++i) {
        float gy = plot_top + (i * grid_step);
        render_target_->DrawLine(
            D2D1::Point2F(plot_left, gy),
            D2D1::Point2F(plot_right, gy),
            brush_chart_grid_,
            1.0f
        );
        // Draw grid scale labels
        uint64_t grid_value = static_cast<uint64_t>(static_cast<double>(max_bucket_total) * (5 - i) / 5.0);
        std::wstring grid_label = FormatBytes(grid_value);
        render_target_->DrawText(grid_label.c_str(), static_cast<UINT32>(grid_label.length()), format_mono_, D2D1::RectF(history_x + 15.0f, gy - 7.0f, plot_left - 5.0f, gy + 10.0f), brush_chart_tick_);
    }
    
    // Draw baseline
    render_target_->DrawLine(
        D2D1::Point2F(plot_left, base_y),
        D2D1::Point2F(plot_right, base_y),
        brush_chart_grid_,
        1.0f
    );

    float chart_area_w = plot_right - plot_left;
    size_t bar_count = std::max<size_t>(buckets.size(), 1);
    float bar_step = chart_area_w / static_cast<float>(bar_count);
    float bar_w = std::max(2.0f, bar_step * 0.68f);
    const float chart_ease = data_chart_progress_;
    data_chart_hitboxes_.clear();

    for (size_t i = 0; i < buckets.size(); ++i) {
        float bx = plot_left + (static_cast<float>(i) * bar_step) + (bar_step - bar_w) / 2.0f;
        float max_bar_h = plot_height;

        float download_h = static_cast<float>(static_cast<double>(buckets[i].download) / static_cast<double>(max_bucket_total) * max_bar_h) * chart_ease;
        float upload_h = static_cast<float>(static_cast<double>(buckets[i].upload) / static_cast<double>(max_bucket_total) * max_bar_h) * chart_ease;

        // Upload is the lighter-blue base; download is the deeper-blue top
        // in BOTH themes. Series order and byte totals never change by theme.
        D2D1_RECT_F upload_rect = D2D1::RectF(bx, base_y - upload_h, bx + bar_w, base_y);
        render_target_->FillRectangle(upload_rect, brush_chart_upload_);

        D2D1_RECT_F download_rect = D2D1::RectF(bx, base_y - upload_h - download_h, bx + bar_w, base_y - upload_h);
        render_target_->FillRectangle(download_rect, brush_chart_download_);

        const D2D1_RECT_F hitbox = D2D1::RectF(
            bx - 3.0f,
            std::min(base_y - upload_h - download_h, base_y - 8.0f),
            bx + bar_w + 3.0f,
            base_y + 2.0f
        );
        data_chart_hitboxes_.push_back({ hitbox, buckets[i].key, buckets[i].download, buckets[i].upload });

        const bool draw_label = buckets.size() <= 12 ||
                                i == 0 ||
                                i + 1 == buckets.size() ||
                                (buckets.size() <= 31 && i % 5 == 0);
        if (draw_label) {
            D2D1_RECT_F b_lbl = D2D1::RectF(bx, base_y + 4.0f, bx + bar_w, base_y + 20.0f);
            render_target_->DrawText(buckets[i].label.c_str(), static_cast<UINT32>(buckets[i].label.length()), format_chart_tick_, b_lbl, brush_chart_tick_);
        }
    }

    UpdateDataChartHover();
    if (hovered_data_chart_hitbox_ >= 0 &&
        hovered_data_chart_hitbox_ < static_cast<int>(data_chart_hitboxes_.size())) {
        const DataChartHitbox& hovered = data_chart_hitboxes_[static_cast<size_t>(hovered_data_chart_hitbox_)];
        const float tooltip_width = std::min(188.0f, history_w - 24.0f);
        const float tooltip_height = 100.0f;
        const float bar_center_x = (hovered.rect.left + hovered.rect.right) * 0.5f;
        const float tooltip_target_x = std::clamp(
            bar_center_x - tooltip_width * 0.5f,
            history_x + 8.0f,
            history_x + history_w - tooltip_width - 8.0f
        );
        const float tooltip_target_y = std::clamp(
            hovered.rect.top - tooltip_height - 10.0f,
            chart_y + 8.0f,
            chart_y + chart_h - tooltip_height - 8.0f
        );

        const ULONGLONG tooltip_now = GetTickCount64();
        if (!data_tooltip_position_ready_) {
            data_tooltip_x_ = tooltip_target_x;
            data_tooltip_y_ = tooltip_target_y;
            data_tooltip_position_ready_ = true;
        } else {
            const float elapsed_ms = data_tooltip_last_update_ms_ == 0
                ? 16.0f
                : std::clamp(
                    static_cast<float>(tooltip_now - data_tooltip_last_update_ms_),
                    0.0f,
                    80.0f
                );
            const float blend = 1.0f - std::exp(-elapsed_ms / 70.0f);
            data_tooltip_x_ += (tooltip_target_x - data_tooltip_x_) * blend;
            data_tooltip_y_ += (tooltip_target_y - data_tooltip_y_) * blend;
        }
        data_tooltip_last_update_ms_ = tooltip_now;
        data_tooltip_last_hover_ms_ = tooltip_now;
        displayed_data_chart_hitbox_ = hovered_data_chart_hitbox_;

        int tooltip_year = 0;
        int tooltip_month = 0;
        int tooltip_day = 0;
        std::wstring tooltip_title = hovered.key;
        if (ParseIsoDate(hovered.key, &tooltip_year, &tooltip_month, &tooltip_day)) {
            tooltip_title = std::wstring(kShortMonthNames[tooltip_month - 1]) + L" " + std::to_wstring(tooltip_day);
        } else if (ParseIsoDate(hovered.key, &tooltip_year, &tooltip_month)) {
            tooltip_title = std::wstring(kShortMonthNames[tooltip_month - 1]) + L" " + std::to_wstring(tooltip_year);
        }

        const D2D1_RECT_F tooltip = D2D1::RectF(
            data_tooltip_x_,
            data_tooltip_y_,
            data_tooltip_x_ + tooltip_width,
            data_tooltip_y_ + tooltip_height
        );
        const D2D1_ROUNDED_RECT tooltip_round = D2D1::RoundedRect(tooltip, 6.0f, 6.0f);
        render_target_->FillRoundedRectangle(tooltip_round, brush_tooltip_bg_);
        render_target_->DrawRoundedRectangle(tooltip_round, brush_tooltip_border_, 1.0f);
        render_target_->DrawText(
            tooltip_title.c_str(),
            static_cast<UINT32>(tooltip_title.length()),
            format_tooltip_title_,
            D2D1::RectF(tooltip.left + 10.0f, tooltip.top + 8.0f, tooltip.right - 10.0f, tooltip.top + 27.0f),
            brush_tooltip_text_
        );
        const auto draw_tooltip_row = [&](float top, ID2D1Brush* marker, const std::wstring& text) {
            render_target_->FillRectangle(
                D2D1::RectF(tooltip.left + 10.0f, top + 4.0f, tooltip.left + 19.0f, top + 13.0f),
                marker
            );
            render_target_->DrawText(
                text.c_str(),
                static_cast<UINT32>(text.length()),
                format_tooltip_body_,
                D2D1::RectF(tooltip.left + 24.0f, top, tooltip.right - 10.0f, top + 18.0f),
                brush_tooltip_text_
            );
        };
        draw_tooltip_row(tooltip.top + 33.0f, brush_chart_download_, std::wstring(L"Download: ") + FormatBytes(hovered.download));
        draw_tooltip_row(tooltip.top + 53.0f, brush_chart_upload_, std::wstring(L"Upload: ") + FormatBytes(hovered.upload));
        draw_tooltip_row(tooltip.top + 73.0f, brush_tooltip_text_, std::wstring(L"Total: ") + FormatBytes(hovered.download + hovered.upload));
    } else {
        // Preserve the previous anchor while the pointer crosses a natural
        // inter-bar gap, so the next tooltip continues its glide.
        const ULONGLONG tooltip_now = GetTickCount64();
        if (!data_tooltip_position_ready_ ||
            tooltip_now - data_tooltip_last_hover_ms_ > 160ULL) {
            displayed_data_chart_hitbox_ = -1;
            data_tooltip_position_ready_ = false;
            data_tooltip_last_update_ms_ = 0;
            data_tooltip_last_hover_ms_ = 0;
        }
    }
    if (selected_dl == 0 && selected_ul == 0) {
        std::wstring empty = L"No saved traffic for this range yet";
        const float empty_y = chart_y + chart_h * 0.5f - 10.0f;
        D2D1_RECT_F empty_rect = D2D1::RectF(history_x + 90.0f, empty_y, history_x + history_w - 24.0f, empty_y + 22.0f);
        render_target_->DrawText(empty.c_str(), static_cast<UINT32>(empty.length()), format_body_bold_, empty_rect, brush_text_secondary_);
    }

    const float comparison_y = layout.comparison_y;
    const D2D1_RECT_F comparison_card = D2D1::RectF(
        start_x,
        comparison_y,
        start_x + content_w,
        comparison_y + layout.comparison_h
    );
    draw_data_surface(comparison_card, 16.0f);
    render_target_->DrawText(
        L"VS PREVIOUS PERIOD",
        18,
        format_comparison_label_,
        D2D1::RectF(start_x + 16.0f, comparison_y + 12.0f, start_x + content_w - 16.0f, comparison_y + 26.0f),
        brush_data_muted_
    );

    double comparison_change = 0.0;
    if (previous_total > 0) {
        comparison_change = std::round(
            (static_cast<double>(usage_total) - static_cast<double>(previous_total)) /
            static_cast<double>(previous_total) * 1000.0
        ) / 10.0;
    }

    if (comparison_change != 0.0) {
        const bool trend_up = comparison_change > 0.0;
        const std::wstring comparison_value = std::wstring(trend_up ? L"\x2191 " : L"\x2193 ") +
            FormatSourcePercentage(std::abs(comparison_change));
        render_target_->DrawText(
            comparison_value.c_str(),
            static_cast<UINT32>(comparison_value.length()),
            format_title_,
            D2D1::RectF(start_x + 16.0f, comparison_y + 30.0f, start_x + content_w - 16.0f, comparison_y + 52.0f),
            trend_up ? brush_limit_danger_ : brush_success_
        );
        const std::wstring comparison_detail = std::wstring(L"Current: ") + FormatBytes(usage_total) +
            L" / Previous: " + FormatBytes(previous_total);
        render_target_->DrawText(
            comparison_detail.c_str(),
            static_cast<UINT32>(comparison_detail.length()),
            format_comparison_detail_,
            D2D1::RectF(start_x + 16.0f, comparison_y + 56.0f, start_x + content_w - 16.0f, comparison_y + 70.0f),
            brush_data_muted_
        );
    } else {
        render_target_->DrawText(
            L"No comparison available",
            23,
            format_title_,
            D2D1::RectF(start_x + 16.0f, comparison_y + 32.0f, start_x + content_w - 16.0f, comparison_y + 54.0f),
            brush_text_primary_
        );
    }

    // Match Watchman-main's monthly threshold model. The outer data-period
    // selector does not change this card: the limit always tracks this month.
    const float limit_y = layout.limit_y;
    const D2D1_RECT_F limit_card = D2D1::RectF(start_x, limit_y, start_x + content_w, limit_y + layout.limit_h);
    draw_data_surface(limit_card, 24.0f);
    render_target_->DrawText(
        L"DATA LIMIT",
        10,
        format_picker_title_,
        D2D1::RectF(start_x + 16.0f, limit_y + 16.0f, start_x + 180.0f, limit_y + 31.0f),
        brush_text_primary_
    );

    double limit_percentage = 0.0;
    uint64_t limit_remaining = 0;
    if (data_limit_bytes_ > 0) {
        limit_percentage = std::round(
            static_cast<double>(current_month_total) / static_cast<double>(data_limit_bytes_) * 1000.0
        ) / 10.0;
        limit_remaining = data_limit_bytes_ > current_month_total
            ? data_limit_bytes_ - current_month_total
            : 0;
    }
    const float limit_target_progress = static_cast<float>(std::clamp(limit_percentage / 100.0, 0.0, 1.0));
    data_limit_progress_ += (limit_target_progress - data_limit_progress_) * animation_alpha_;
    if (std::abs(limit_target_progress - data_limit_progress_) < 0.001f) {
        data_limit_progress_ = limit_target_progress;
    }

    data_limit_animating_ = std::abs(limit_target_progress - data_limit_progress_) >= 0.001f;
    const bool show_limit_usage = data_limit_bytes_ > 0 && limit_percentage > 0.0;
    const std::wstring limit_status = show_limit_usage
        ? FormatBytes(limit_remaining) + L" remaining"
        : L"No limit set";
    render_target_->DrawText(
        limit_status.c_str(),
        static_cast<UINT32>(limit_status.length()),
        format_limit_status_,
        D2D1::RectF(start_x + content_w - 190.0f, limit_y + 14.0f, start_x + content_w - 16.0f, limit_y + 32.0f),
        brush_data_muted_
    );

    const float progress_x = start_x + 16.0f;
    const float percentage_width = 36.0f;
    const float progress_gap = 10.0f;
    const float progress_w = content_w - 32.0f - progress_gap - percentage_width;
    const float progress_y = limit_y + 43.0f;
    const D2D1_ROUNDED_RECT progress_track = D2D1::RoundedRect(
        D2D1::RectF(progress_x, progress_y, progress_x + progress_w, progress_y + 8.0f),
        4.0f,
        4.0f
    );
    render_target_->FillRoundedRectangle(progress_track, brush_limit_track_);
    if (data_limit_progress_ > 0.0f) {
        ID2D1SolidColorBrush* fill_brush = brush_limit_normal_;
        if (limit_percentage >= 95.0) {
            fill_brush = brush_limit_danger_;
        } else if (limit_percentage >= 80.0) {
            fill_brush = brush_limit_warning_;
        }
        const D2D1_ROUNDED_RECT progress_fill = D2D1::RoundedRect(
            D2D1::RectF(progress_x, progress_y, progress_x + progress_w * data_limit_progress_, progress_y + 8.0f),
            4.0f,
            4.0f
        );
        render_target_->FillRoundedRectangle(progress_fill, fill_brush);
    }
    const std::wstring percentage_text = FormatSourcePercentage(limit_percentage);
    render_target_->DrawText(
        percentage_text.c_str(),
        static_cast<UINT32>(percentage_text.length()),
        format_limit_pct_,
        D2D1::RectF(progress_x + progress_w + progress_gap, progress_y - 4.0f, start_x + content_w - 16.0f, progress_y + 14.0f),
        brush_text_primary_
    );

    const float limit_input_left = start_x + 16.0f;
    const float limit_input_top = limit_y + 66.0f;
    const float limit_button_width = 82.0f;
    const float limit_button_height = 28.0f;
    const float limit_button_left = start_x + content_w - 16.0f - limit_button_width;
    const float limit_button_top = limit_input_top + 1.0f;
    const float limit_input_right = limit_button_left - 8.0f;
    const float limit_input_height = 30.0f;
    const D2D1_RECT_F limit_input = D2D1::RectF(
        limit_input_left,
        limit_input_top,
        limit_input_right,
        limit_input_top + limit_input_height
    );
    const D2D1_ROUNDED_RECT limit_input_round = D2D1::RoundedRect(limit_input, 8.0f, 8.0f);
    render_target_->FillRoundedRectangle(limit_input_round, brush_sidebar_bg_);
    render_target_->DrawRoundedRectangle(limit_input_round, brush_card_border_, 1.0f);
    if (!data_limit_edit_ || !IsWindowVisible(data_limit_edit_)) {
        std::wstring limit_value = L"Limit in GB";
        if (data_limit_bytes_ > 0) {
            const double gib = static_cast<double>(data_limit_bytes_) / (1024.0 * 1024.0 * 1024.0);
            std::wstringstream value_stream;
            value_stream << std::fixed << std::setprecision(data_limit_bytes_ % (1024ULL * 1024ULL * 1024ULL) == 0 ? 0 : 2) << gib << L" GB";
            limit_value = value_stream.str();
        }
        render_target_->DrawText(
            limit_value.c_str(),
            static_cast<UINT32>(limit_value.length()),
            format_limit_input_,
            D2D1::RectF(limit_input.left + 10.0f, limit_input.top + 2.0f, limit_input.right - 10.0f, limit_input.bottom - 2.0f),
            data_limit_bytes_ > 0 ? brush_text_primary_ : brush_text_secondary_
        );
    } else {
        SetWindowPos(
            data_limit_edit_,
            HWND_TOP,
            static_cast<int>(limit_input_left + 4.0f),
            static_cast<int>(limit_input_top + 3.0f - CurrentContentScroll()),
            static_cast<int>(limit_input_right - limit_input_left - 8.0f),
            static_cast<int>(limit_input_height - 6.0f),
            SWP_NOACTIVATE
        );
    }

    const D2D1_ROUNDED_RECT limit_button = D2D1::RoundedRect(
        D2D1::RectF(limit_button_left, limit_button_top, limit_button_left + limit_button_width, limit_button_top + limit_button_height),
        14.0f,
        14.0f
    );
    render_target_->FillRoundedRectangle(limit_button, brush_action_bg_);
    render_target_->DrawText(
        L"SET LIMIT",
        9,
        format_limit_button_,
        D2D1::RectF(limit_button_left, limit_button_top, limit_button_left + limit_button_width, limit_button_top + limit_button_height),
        brush_tooltip_text_
    );
    const bool draw_data_picker =
        (data_picker_open_ || data_picker_visibility_ > 0.001f) &&
        (data_range_idx_ == 2 || data_range_idx_ == 3);
    if (draw_data_picker) {
        const bool monthly_picker = data_range_idx_ == 2;
        const D2D1_RECT_F picker = GetDataPickerRect(layout, monthly_picker, hist_snap.available_years.size());
        const float picker_visibility = std::clamp(data_picker_visibility_, 0.0f, 1.0f);
        const bool animate_picker = picker_visibility < 0.999f;
        D2D1_MATRIX_3X2_F picker_base_transform{};
        render_target_->GetTransform(&picker_base_transform);
        ID2D1Layer* picker_layer = nullptr;
        if (animate_picker &&
            SUCCEEDED(render_target_->CreateLayer(nullptr, &picker_layer))) {
            const D2D1_LAYER_PARAMETERS picker_layer_parameters = D2D1::LayerParameters(
                D2D1::InfiniteRect(),
                nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(),
                picker_visibility,
                nullptr,
                D2D1_LAYER_OPTIONS_NONE
            );
            render_target_->PushLayer(picker_layer_parameters, picker_layer);
            const float picker_scale = 0.96f + picker_visibility * 0.04f;
            const D2D1_POINT_2F picker_origin = D2D1::Point2F(
                (picker.left + picker.right) * 0.5f,
                picker.top + 18.0f
            );
            D2D1::Matrix3x2F picker_transform = D2D1::Matrix3x2F::Scale(
                picker_scale,
                picker_scale,
                picker_origin
            );
            picker_transform._31 += picker_base_transform._31;
            picker_transform._32 += picker_base_transform._32 -
                (1.0f - picker_visibility) * 12.0f;
            render_target_->SetTransform(picker_transform);
        }
        const float nav_top = picker.top + 14.0f;
        const D2D1_RECT_F previous_rect = D2D1::RectF(
            picker.left + 118.0f,
            nav_top,
            picker.left + 146.0f,
            nav_top + 28.0f
        );
        const D2D1_RECT_F next_rect = D2D1::RectF(
            picker.right - 42.0f,
            nav_top,
            picker.right - 14.0f,
            nav_top + 28.0f
        );
        const float header_copy_left = picker.left + 14.0f;
        const float header_copy_right = previous_rect.left - 12.0f;

        const D2D1_ROUNDED_RECT picker_shadow = D2D1::RoundedRect(
            D2D1::RectF(picker.left + 1.0f, picker.top + 4.0f, picker.right + 1.0f, picker.bottom + 5.0f),
            24.0f,
            24.0f
        );
        const D2D1_ROUNDED_RECT picker_round = D2D1::RoundedRect(picker, 24.0f, 24.0f);
        render_target_->FillRoundedRectangle(picker_shadow, brush_popup_shadow_);
        render_target_->FillRoundedRectangle(picker_round, brush_popup_bg_);
        render_target_->DrawRoundedRectangle(picker_round, brush_card_border_, 1.0f);

        const std::wstring picker_title = monthly_picker ? L"CHOOSE MONTH" : L"CHOOSE YEAR";
        const std::wstring picker_subtitle = monthly_picker
            ? L"Monthly view shows daily totals"
            : L"Yearly view shows Jan-Dec totals";
        render_target_->DrawText(
            picker_title.c_str(),
            static_cast<UINT32>(picker_title.length()),
            format_picker_title_,
            D2D1::RectF(header_copy_left, picker.top + 14.0f, header_copy_right, picker.top + 45.0f),
            brush_text_primary_
        );
        render_target_->DrawText(
            picker_subtitle.c_str(),
            static_cast<UINT32>(picker_subtitle.length()),
            format_picker_subtitle_,
            D2D1::RectF(header_copy_left, picker.top + 48.0f, header_copy_right, picker.top + 94.0f),
            brush_text_secondary_
        );

        if (hist_snap.available_years.empty()) {
            const std::wstring empty_picker_text = L"No saved traffic history is available yet.";
            render_target_->DrawText(
                empty_picker_text.c_str(),
                static_cast<UINT32>(empty_picker_text.length()),
                format_picker_subtitle_,
                D2D1::RectF(picker.left + 14.0f, picker.top + 112.0f, picker.right - 14.0f, picker.top + 144.0f),
                brush_text_secondary_
            );
        } else {
            const auto draw_picker_button = [&](const D2D1_RECT_F& rect,
                                                const wchar_t* label,
                                                UINT32 length,
                                                IDWriteTextFormat* text_format,
                                                float radius,
                                                bool selected,
                                                bool enabled,
                                                bool month_style) {
                const D2D1_ROUNDED_RECT button = D2D1::RoundedRect(rect, radius, radius);
                if (selected) {
                    render_target_->FillRoundedRectangle(button, brush_text_primary_);
                } else if (month_style) {
                    render_target_->FillRoundedRectangle(button, brush_picker_month_bg_);
                    render_target_->DrawRoundedRectangle(button, brush_picker_month_border_, 1.0f);
                } else if (enabled) {
                    render_target_->FillRoundedRectangle(button, brush_sidebar_bg_);
                    render_target_->DrawRoundedRectangle(button, brush_card_border_, 1.0f);
                } else {
                    render_target_->FillRoundedRectangle(button, brush_popup_bg_);
                    render_target_->DrawRoundedRectangle(button, brush_chart_grid_, 1.0f);
                }

                ID2D1SolidColorBrush* text_brush = selected
                    ? brush_active_text_
                    : (enabled ? brush_text_primary_ : brush_chart_tick_);
                render_target_->DrawText(label, length, text_format, rect, text_brush);
            };

            if (monthly_picker) {
                render_target_->DrawText(
                    L"MONTHS",
                    6,
                    format_picker_section_,
                    D2D1::RectF(picker.left + 14.0f, picker.top + 106.0f, picker.right - 14.0f, picker.top + 120.0f),
                    brush_text_secondary_
                );

                const int year_index = std::clamp(
                    data_picker_page_,
                    0,
                    static_cast<int>(hist_snap.available_years.size()) - 1
                );
                const int picker_year = hist_snap.available_years[year_index];
                draw_picker_button(previous_rect, L"\x2039", 1, format_picker_nav_, 14.0f, false, year_index > 0, false);
                draw_picker_button(next_rect, L"\x203A", 1, format_picker_nav_, 14.0f, false,
                    year_index + 1 < static_cast<int>(hist_snap.available_years.size()), false);

                const std::wstring year_label = std::to_wstring(picker_year);
                render_target_->DrawText(
                    year_label.c_str(),
                    static_cast<UINT32>(year_label.length()),
                    format_picker_range_,
                    D2D1::RectF(picker.left + 154.0f, nav_top, next_rect.left - 8.0f, nav_top + 28.0f),
                    brush_text_primary_
                );

                const float grid_x = picker.left + 14.0f;
                const float grid_y = picker.top + 126.0f;
                const float grid_w = picker.right - picker.left - 28.0f;
                const float cell_gap = 6.0f;
                const float cell_w = (grid_w - cell_gap * 3.0f) / 4.0f;
                const float cell_h = 34.0f;
                for (int month = 1; month <= 12; ++month) {
                    const int column = (month - 1) % 4;
                    const int row = (month - 1) / 4;
                    const D2D1_RECT_F cell = D2D1::RectF(
                        grid_x + column * (cell_w + cell_gap),
                        grid_y + row * (cell_h + cell_gap),
                        grid_x + column * (cell_w + cell_gap) + cell_w,
                        grid_y + row * (cell_h + cell_gap) + cell_h
                    );
                    draw_picker_button(
                        cell,
                        kShortMonthNames[month - 1],
                        3,
                        format_picker_month_,
                        17.0f,
                        month == data_history_month_,
                        true,
                        true
                    );
                }
            } else {
                constexpr int kYearsPerPage = 12;
                const int page_count = std::max(
                    1,
                    static_cast<int>((hist_snap.available_years.size() + kYearsPerPage - 1) / kYearsPerPage)
                );
                const int page = std::clamp(data_picker_page_, 0, page_count - 1);
                const int first_index = page * kYearsPerPage;
                const int last_index = std::min(
                    static_cast<int>(hist_snap.available_years.size()),
                    first_index + kYearsPerPage
                );
                draw_picker_button(previous_rect, L"\x2039", 1, format_picker_nav_, 14.0f, false, page > 0, false);
                draw_picker_button(next_rect, L"\x203A", 1, format_picker_nav_, 14.0f, false, page + 1 < page_count, false);

                std::wstring page_label = std::to_wstring(hist_snap.available_years[first_index]);
                if (first_index + 1 < last_index) {
                    page_label += L" - ";
                    page_label += std::to_wstring(hist_snap.available_years[last_index - 1]);
                }
                render_target_->DrawText(
                    page_label.c_str(),
                    static_cast<UINT32>(page_label.length()),
                    format_picker_range_,
                    D2D1::RectF(picker.left + 154.0f, nav_top, next_rect.left - 8.0f, nav_top + 28.0f),
                    brush_text_primary_
                );

                const float grid_x = picker.left + 14.0f;
                const float grid_y = picker.top + 106.0f;
                const float grid_w = picker.right - picker.left - 28.0f;
                const float cell_gap = 8.0f;
                const float cell_w = (grid_w - cell_gap * 2.0f) / 3.0f;
                const float cell_h = 42.0f;
                for (int index = first_index; index < last_index; ++index) {
                    const int visible_index = index - first_index;
                    const int column = visible_index % 3;
                    const int row = visible_index / 3;
                    const D2D1_RECT_F cell = D2D1::RectF(
                        grid_x + column * (cell_w + cell_gap),
                        grid_y + row * (cell_h + cell_gap),
                        grid_x + column * (cell_w + cell_gap) + cell_w,
                        grid_y + row * (cell_h + cell_gap) + cell_h
                    );
                    const std::wstring year_label = std::to_wstring(hist_snap.available_years[index]);
                    draw_picker_button(
                        cell,
                        year_label.c_str(),
                        static_cast<UINT32>(year_label.length()),
                        format_picker_year_,
                        16.0f,
                        hist_snap.available_years[index] == data_history_year_,
                        true,
                        false
                    );
                }

                const int visible_year_count = last_index - first_index;
                const int visible_year_rows = std::max(1, (visible_year_count + 2) / 3);
                const float months_label_y = grid_y + visible_year_rows * cell_h +
                    (visible_year_rows - 1) * cell_gap + 14.0f;
                render_target_->DrawText(
                    L"MONTHS",
                    6,
                    format_picker_section_,
                    D2D1::RectF(picker.left + 14.0f, months_label_y, picker.right - 14.0f, months_label_y + 14.0f),
                    brush_text_secondary_
                );

                const float months_grid_y = months_label_y + 20.0f;
                const float month_gap = 6.0f;
                const float month_w = (grid_w - month_gap * 3.0f) / 4.0f;
                const float month_h = 34.0f;
                for (int month = 1; month <= 12; ++month) {
                    const int column = (month - 1) % 4;
                    const int row = (month - 1) / 4;
                    const D2D1_RECT_F cell = D2D1::RectF(
                        grid_x + column * (month_w + month_gap),
                        months_grid_y + row * (month_h + month_gap),
                        grid_x + column * (month_w + month_gap) + month_w,
                        months_grid_y + row * (month_h + month_gap) + month_h
                    );
                    draw_picker_button(
                        cell,
                        kShortMonthNames[month - 1],
                        3,
                        format_picker_month_,
                        17.0f,
                        false,
                        true,
                        true
                    );
                }
            }
        }
        if (picker_layer) {
            render_target_->SetTransform(picker_base_transform);
            render_target_->PopLayer();
            picker_layer->Release();
        }
    }
    float data_content_bottom = layout.limit_y + layout.limit_h + 12.0f;
    if (draw_data_picker) {
        data_content_bottom = std::max(
            data_content_bottom,
            GetDataPickerRect(layout, data_range_idx_ == 2, hist_snap.available_years.size()).bottom + 12.0f
        );
    }
    ReportContentBottom(data_content_bottom);
}

void AppRenderer::DrawAppMonitor(D2D1_SIZE_F rt_size, const AppActivitySnapshot& app_snap) {
    const float side = std::clamp(rt_size.width * 0.032f, 16.0f, 30.0f);
    const float start_x = side;
    const float content_w = rt_size.width - side * 2.0f;
    const float start_y = UIStyle::TITLEBAR_HEIGHT + UIStyle::TABBAR_HEIGHT + 16.0f;

    const bool show_restart_button = !app_snap.is_admin;
    constexpr float restart_button_width = MainContentLayout::app_admin_button_width;
    constexpr float restart_button_height = 24.0f;
    const float restart_button_x = start_x + content_w - restart_button_width;
    const float title_right = restart_button_x - 10.0f;
    render_target_->DrawText(
        L"APP NETWORK ACTIVITY",
        20,
        format_app_header_,
        D2D1::RectF(start_x + 4.0f, start_y, title_right, start_y + 20.0f),
        brush_text_primary_
    );

    if (show_restart_button) {
        const D2D1_RECT_F restart_rect = D2D1::RectF(
            restart_button_x,
            start_y - 2.0f,
            restart_button_x + restart_button_width,
            start_y - 2.0f + restart_button_height
        );
        const D2D1_ROUNDED_RECT restart_round = D2D1::RoundedRect(restart_rect, 12.0f, 12.0f);
        render_target_->FillRoundedRectangle(restart_round, brush_card_bg_);
        render_target_->DrawRoundedRectangle(restart_round, brush_accent_dl_, 1.0f);
        render_target_->DrawText(
            L"Run as administrator",
            20,
            format_app_body_bold_,
            D2D1::RectF(restart_rect.left + 14.0f, restart_rect.top + 5.0f, restart_rect.right - 8.0f, restart_rect.bottom - 3.0f),
            brush_accent_dl_
        );
    }

    if (app_snap.is_admin) {
        render_target_->DrawText(L"Administrator", 13, format_app_body_bold_,
            D2D1::RectF(restart_button_x + 24.0f, start_y + 2.0f, start_x + content_w, start_y + 22.0f), brush_accent_sys_);
    }

    std::wstring subtitle;
    if (app_snap.requires_admin) {
        subtitle = L"Standard access: app connections only. Run as administrator\nto capture live per-app rates and session usage.";
    } else if (!app_snap.bandwidth_available && !app_snap.status_message.empty()) {
        subtitle = app_snap.status_message;
    } else {
        subtitle = L"Live rates; session bars with totals below.\nLargest users first. All captured adapters.";
    }
    render_target_->DrawText(
        subtitle.c_str(),
        static_cast<UINT32>(subtitle.length()),
        format_app_subtitle_,
        D2D1::RectF(start_x + 4.0f, start_y + 26.0f, start_x + content_w - 4.0f, start_y + 72.0f),
        brush_text_primary_
    );

    auto apps = app_snap.active_apps;
    apps.erase(std::remove_if(apps.begin(), apps.end(), [](const AppActivityItem& app) {
        return app.total_download_bytes == 0 &&
               app.total_upload_bytes == 0 &&
               app.active_connections == 0;
    }), apps.end());
    // Active apps are pre-sorted in the telemetry snapshot; fallback sort only if ordering invariant violated
    if (!std::is_sorted(apps.begin(), apps.end(), AppTrafficPresentation::MostUsageFirst)) {
        std::sort(apps.begin(), apps.end(), AppTrafficPresentation::MostUsageFirst);
    }
    const uint64_t largest_session_bytes = apps.empty() ? 0 : AppTrafficPresentation::SessionBytes(apps.front());

    // At Watchman's compact width the source flex row stacks below the subtitle:
    // the data-usage action fills the row and the count remains a separate pill.
    const float usage_y = start_y + 80.0f;
    const float badge_width = 26.0f;
    const float usage_right = start_x + content_w - badge_width - 8.0f;
    const D2D1_RECT_F usage_rect = D2D1::RectF(start_x, usage_y, usage_right, usage_y + 28.0f);
    const D2D1_ROUNDED_RECT usage_round = D2D1::RoundedRect(usage_rect, 14.0f, 14.0f);
    render_target_->DrawRoundedRectangle(usage_round, brush_card_border_, 1.0f);

    constexpr float usage_group_width = 176.0f;
    const float usage_group_left = usage_rect.left + (usage_rect.right - usage_rect.left - usage_group_width) * 0.5f;
    DrawVectorIcon(
        render_target_,
        VectorIcon::Bars,
        D2D1::RectF(usage_group_left, usage_y + 6.0f, usage_group_left + 16.0f, usage_y + 23.0f),
        brush_text_secondary_,
        1.1f
    );
    render_target_->DrawText(
        L"Windows data usage",
        18,
        format_app_body_bold_,
        D2D1::RectF(usage_group_left + 23.0f, usage_y + 6.0f, usage_group_left + usage_group_width, usage_y + 23.0f),
        brush_text_primary_
    );

    const float count_x = start_x + content_w - badge_width;
    const D2D1_RECT_F count_rect = D2D1::RectF(count_x, usage_y + 4.0f, count_x + badge_width, usage_y + 24.0f);
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(count_rect, 10.0f, 10.0f), brush_text_primary_);
    ID2D1SolidColorBrush* count_text = nullptr;
    const D2D1_COLOR_F count_text_color = effective_theme_ == ThemeMode::Light
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
        : D2D1::ColorF(0.03f, 0.03f, 0.03f, 1.0f);
    render_target_->CreateSolidColorBrush(count_text_color, &count_text);
    const std::wstring count_label = std::to_wstring(apps.size());
    render_target_->DrawText(
        count_label.c_str(),
        static_cast<UINT32>(count_label.length()),
        format_app_badge_,
        count_rect,
        count_text
    );
    if (count_text) {
        count_text->Release();
    }

    constexpr float header_height = 34.0f;
    constexpr float row_height = 70.0f;
    const float table_y = usage_y + 46.0f;
    const float table_height = std::max(
        118.0f,
        header_height + static_cast<float>(std::max<size_t>(apps.size(), 1)) * row_height
    );
    const D2D1_RECT_F table_rect = D2D1::RectF(start_x, table_y, start_x + content_w, table_y + table_height);
    DrawCard(table_rect);

    ID2D1SolidColorBrush* table_header_brush = nullptr;
    const D2D1_COLOR_F table_header_color = effective_theme_ == ThemeMode::Light
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f)
        : D2D1::ColorF(0x18 / 255.0f, 0x18 / 255.0f, 0x18 / 255.0f, 1.0f);
    render_target_->CreateSolidColorBrush(table_header_color, &table_header_brush);
    if (table_header_brush) {
        const D2D1_RECT_F table_header_rect = D2D1::RectF(
            table_rect.left + 1.0f,
            table_rect.top + 1.0f,
            table_rect.right - 1.0f,
            table_y + header_height + 1.0f
        );
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(table_header_rect, UIStyle::CORNER_RADIUS_LARGE - 1.0f, UIStyle::CORNER_RADIUS_LARGE - 1.0f),
            table_header_brush
        );
    }

    const auto columns = MainContentLayout::GetAppColumns(table_rect.left, content_w);
    const float col_name_right = columns.name_right;
    const float col_down_right = columns.download_right;
    const float col_up_right = columns.upload_right;
    const float table_right = columns.right;
    const float cell_padding = 10.0f;

    render_target_->DrawText(L"APPLICATION", 11, format_app_label_,
        D2D1::RectF(table_rect.left + cell_padding, table_y + 9.0f, col_name_right - 4.0f, table_y + header_height),
        brush_text_secondary_);
    render_target_->DrawText(L"LIVE DOWN", 9, format_app_label_,
        D2D1::RectF(col_name_right + cell_padding, table_y + 9.0f, col_down_right - 4.0f, table_y + header_height),
        brush_text_secondary_);
    render_target_->DrawText(L"LIVE UP", 7, format_app_label_,
        D2D1::RectF(col_down_right + cell_padding, table_y + 9.0f, col_up_right - 4.0f, table_y + header_height),
        brush_text_secondary_);
    render_target_->DrawText(L"CONNS", 5, format_app_label_,
        D2D1::RectF(col_up_right + 2.0f, table_y + 9.0f, table_right - 2.0f, table_y + header_height),
        brush_text_secondary_);
    render_target_->DrawLine(
        D2D1::Point2F(table_rect.left, table_y + header_height),
        D2D1::Point2F(table_rect.right, table_y + header_height),
        brush_card_border_,
        1.0f
    );
    if (table_header_brush) {
        table_header_brush->Release();
    }

    if (apps.empty()) {
        const std::wstring empty_message = app_snap.requires_admin
            ? L"Per-app bandwidth needs administrator rights on Windows. The list can still show apps and connections, but live download/upload by app is unavailable until the app is run as administrator."
            : L"No app network activity yet. Apps that touch the network will stay listed here for this session.";
        render_target_->DrawText(
            empty_message.c_str(),
            static_cast<UINT32>(empty_message.length()),
            format_app_subtitle_,
            D2D1::RectF(table_rect.left + 24.0f, table_y + header_height + 22.0f, table_rect.right - 24.0f, table_rect.bottom - 16.0f),
            brush_text_secondary_
        );
        ReportContentBottom(table_rect.bottom + 20.0f);
        return;
    }

    ID2D1SolidColorBrush* alternate_row_brush = nullptr;
    ID2D1SolidColorBrush* high_usage_row_brush = nullptr;
    ID2D1SolidColorBrush* icon_brush = nullptr;
    ID2D1SolidColorBrush* inactive_rate_brush = nullptr;
    render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, effective_theme_ == ThemeMode::Light ? 0.30f : 0.025f), &alternate_row_brush);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.27f, 0.27f, 0.06f), &high_usage_row_brush);
    render_target_->CreateSolidColorBrush(effective_theme_ == ThemeMode::Light
        ? D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.10f) : palette_.ControlHover, &icon_brush);
    render_target_->CreateSolidColorBrush(effective_theme_ == ThemeMode::Light
        ? D2D1::ColorF(0x555555, 1.0f) : palette_.TextSecondary, &inactive_rate_brush);

    const auto status_label = [](const std::wstring& status) {
        if (status == L"live") return std::wstring(L"LIVE");
        if (status == L"background") return std::wstring(L"BACKGROUND");
        return std::wstring(L"RECENT");
    };
    const auto last_active_label = [](uint64_t seconds) {
        if (seconds <= 3) return std::wstring(L"Last active now");
        if (seconds < 60) return L"Last active " + std::to_wstring(seconds) + L"s ago";
        if (seconds < 3600) return L"Last active " + std::to_wstring(seconds / 60) + L"m ago";
        return L"Last active " + std::to_wstring(seconds / 3600) + L"h ago";
    };
    const auto ellipsize = [](const std::wstring& value, float available_width) {
        const size_t max_chars = std::max<size_t>(3, static_cast<size_t>(available_width / 7.0f));
        if (value.length() <= max_chars) {
            return value;
        }
        return value.substr(0, max_chars - 3) + L"...";
    };

    const AppActivityItem* hovered_app = nullptr;
    const float viewport_top = ContentViewportTop() + CurrentContentScroll();
    const float viewport_bottom = ContentViewportBottom() + CurrentContentScroll();
    float row_y = table_y + header_height;
    for (size_t index = 0; index < apps.size(); ++index) {
        const AppActivityItem& app = apps[index];
        const D2D1_RECT_F row_rect = D2D1::RectF(table_rect.left + 1.0f, row_y, table_rect.right - 1.0f, row_y + row_height);
        if (row_rect.bottom < viewport_top || row_rect.top > viewport_bottom) {
            row_y += row_height;
            continue;
        }
        const bool high_usage = app_snap.bandwidth_available &&
            AppTrafficPresentation::SessionBytes(app) > 10ULL * 1024ULL * 1024ULL;
        if (high_usage && high_usage_row_brush) {
            render_target_->FillRectangle(row_rect, high_usage_row_brush);
        } else if (index % 2 != 0 && alternate_row_brush) {
            render_target_->FillRectangle(row_rect, alternate_row_brush);
        }
        if (index + 1 < apps.size()) {
            render_target_->DrawLine(
                D2D1::Point2F(table_rect.left + 8.0f, row_y + row_height),
                D2D1::Point2F(table_rect.right - 8.0f, row_y + row_height),
                brush_sidebar_bg_,
                1.0f
            );
        }

        const std::wstring label = status_label(app.status);
        const float label_width = label == L"BACKGROUND" ? 96.0f : (label == L"RECENT" ? 65.0f : 48.0f);
        const float icon_left = table_rect.left + cell_padding;
        const D2D1_RECT_F icon_rect = D2D1::RectF(icon_left, row_y + 12.0f, icon_left + 24.0f, row_y + 36.0f);
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(icon_rect, 6.0f, 6.0f), icon_brush);
        wchar_t letter = app.name.empty() ? L"?"[0] : static_cast<wchar_t>(towupper(app.name[0]));
        const std::wstring icon_letter(1, letter);
        render_target_->DrawText(icon_letter.c_str(), 1, format_app_icon_, icon_rect, brush_text_primary_);

        const float name_left = icon_rect.right + 8.0f;
        const float badge_left = name_left;
        const float name_width = std::max(12.0f, col_name_right - cell_padding - name_left);
        const std::wstring display_name = ellipsize(app.name.empty() ? L"Process" : app.name, name_width);
        render_target_->DrawText(
            display_name.c_str(),
            static_cast<UINT32>(display_name.length()),
            format_app_body_bold_,
            D2D1::RectF(name_left, row_y + 6.0f, col_name_right - cell_padding, row_y + 25.0f),
            brush_text_primary_
        );

        D2D1_COLOR_F status_background = effective_theme_ == ThemeMode::Light
            ? D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.08f) : palette_.ControlHover;
        D2D1_COLOR_F status_foreground = palette_.TextSecondary;
        if (app.status == L"live") {
            status_background = D2D1::ColorF(0.23f, 0.51f, 0.96f, 0.12f);
            status_foreground = effective_theme_ == ThemeMode::Light
                ? D2D1::ColorF(0.11f, 0.31f, 0.85f, 1.0f) : palette_.AccentDownload;
        } else if (app.status == L"background") {
            status_background = D2D1::ColorF(0.06f, 0.73f, 0.51f, 0.12f);
            status_foreground = effective_theme_ == ThemeMode::Light
                ? D2D1::ColorF(0.02f, 0.47f, 0.34f, 1.0f) : palette_.AccentSystem;
        }
        ID2D1SolidColorBrush* status_background_brush = nullptr;
        ID2D1SolidColorBrush* status_foreground_brush = nullptr;
        render_target_->CreateSolidColorBrush(status_background, &status_background_brush);
        render_target_->CreateSolidColorBrush(status_foreground, &status_foreground_brush);
        const D2D1_RECT_F status_rect = D2D1::RectF(badge_left, row_y + 47.0f, badge_left + label_width, row_y + 65.0f);
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(status_rect, 8.0f, 8.0f), status_background_brush);
        render_target_->DrawRoundedRectangle(D2D1::RoundedRect(status_rect, 8.0f, 8.0f), status_foreground_brush, 0.7f);
        render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.length()), format_app_badge_, status_rect, status_foreground_brush);
        if (status_background_brush) status_background_brush->Release();
        if (status_foreground_brush) status_foreground_brush->Release();

        const std::wstring last_active = last_active_label(app.last_active_seconds);
        render_target_->DrawText(
            last_active.c_str(),
            static_cast<UINT32>(last_active.length()),
            format_app_body_,
            D2D1::RectF(name_left, row_y + 27.0f, col_name_right - cell_padding, row_y + 44.0f),
            brush_text_secondary_
        );

        const D2D1_RECT_F down_rect = D2D1::RectF(col_name_right + cell_padding, row_y + 6.0f, col_down_right - 4.0f, row_y + 43.0f);
        const D2D1_RECT_F up_rect = D2D1::RectF(col_down_right + cell_padding, row_y + 6.0f, col_up_right - 4.0f, row_y + 43.0f);
        if (app_snap.bandwidth_available) {
            const auto draw_live_rate = [&](uint64_t bits_per_second, const D2D1_RECT_F& area, ID2D1Brush* accent) {
                std::wstring value, unit;
                FormatSpeedWithUnit(bits_per_second / 8.0, value, unit);
                render_target_->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format_app_body_bold_,
                    D2D1::RectF(area.left, row_y + 10.0f, area.right, row_y + 28.0f),
                    bits_per_second > 0 ? accent : inactive_rate_brush);
                render_target_->DrawText(unit.c_str(), static_cast<UINT32>(unit.size()), format_app_mono_,
                    D2D1::RectF(area.left, row_y + 30.0f, area.right, row_y + 46.0f), inactive_rate_brush);
            };
            draw_live_rate(app.current_download_bps, down_rect, brush_accent_dl_);
            draw_live_rate(app.current_upload_bps, up_rect, brush_accent_ul_);

            const auto bar = AppTrafficPresentation::UsageBar(app, largest_session_bytes);
            const auto usage = MainContentLayout::GetAppUsageRow(down_rect.left, up_rect.right);
            const D2D1_RECT_F track = D2D1::RectF(usage.bar_left, row_y + 54.0f, usage.bar_right, row_y + 59.0f);
            render_target_->FillRectangle(track, brush_sidebar_bg_);
            const float bar_width = track.right - track.left;
            const float split = track.left + static_cast<float>(bar.download) * bar_width;
            render_target_->FillRectangle(D2D1::RectF(track.left, track.top, split, track.bottom), brush_accent_dl_);
            render_target_->FillRectangle(D2D1::RectF(split, track.top,
                split + static_cast<float>(bar.upload) * bar_width, track.bottom), brush_accent_ul_);
            const std::wstring total = FormatAppTotalBytes(AppTrafficPresentation::SessionBytes(app));
            format_app_mono_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            render_target_->DrawText(total.c_str(), static_cast<UINT32>(total.size()), format_app_mono_,
                D2D1::RectF(usage.total_left, row_y + 48.0f, usage.total_right, row_y + 65.0f), brush_text_secondary_);
            format_app_mono_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            if (mouse_x_ >= usage.bar_left && mouse_x_ <= usage.total_right && mouse_y_ >= row_y && mouse_y_ < row_y + row_height) {
                hovered_app = &app;
            }
        } else {
            render_target_->DrawText(L"\u2014", 1, format_app_body_bold_, down_rect, brush_text_secondary_);
            render_target_->DrawText(L"\u2014", 1, format_app_body_bold_, up_rect, brush_text_secondary_);
            const wchar_t* reason = app_snap.requires_admin ? L"Requires admin" : L"Capture unavailable";
            render_target_->DrawText(reason, static_cast<UINT32>(wcslen(reason)), format_app_mono_,
                D2D1::RectF(down_rect.left, row_y + 40.0f, up_rect.right, row_y + 59.0f), inactive_rate_brush);
        }

        const std::wstring connections = std::to_wstring(app.active_connections);
        render_target_->DrawText(
            connections.c_str(),
            static_cast<UINT32>(connections.length()),
            format_app_conns_,
            D2D1::RectF(col_up_right + 2.0f, row_y, table_right - 2.0f, row_y + row_height),
            brush_text_primary_
        );
        row_y += row_height;
    }

    if (hovered_app) {
        constexpr float tooltip_width = 268.0f, tooltip_height = 142.0f;
        const float left = std::clamp(static_cast<float>(mouse_x_) - tooltip_width * 0.5f,
            start_x, start_x + content_w - tooltip_width);
        const float top = std::clamp(static_cast<float>(mouse_y_) - tooltip_height - 12.0f,
            viewport_top + 4.0f, std::max(viewport_top + 4.0f, viewport_bottom - tooltip_height - 4.0f));
        const auto tooltip = D2D1::RoundedRect(D2D1::RectF(left, top, left + tooltip_width, top + tooltip_height), 6.0f, 6.0f);
        render_target_->FillRoundedRectangle(tooltip, brush_tooltip_bg_);
        render_target_->DrawRoundedRectangle(tooltip, brush_tooltip_border_, 1.0f);
        IDWriteTextLayout* title_layout = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(hovered_app->name.c_str(), static_cast<UINT32>(hovered_app->name.size()),
            format_tooltip_title_, tooltip_width - 24.0f, 19.0f, &title_layout))) {
            IDWriteInlineObject* ellipsis = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(format_tooltip_title_, &ellipsis))) {
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                title_layout->SetTrimming(&trimming, ellipsis);
                ellipsis->Release();
            }
            render_target_->DrawTextLayout(D2D1::Point2F(left + 12.0f, top + 9.0f), title_layout,
                brush_tooltip_text_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            title_layout->Release();
        }
        const std::wstring rows[] = {
            L"Captured since Watchman started",
            L"Download: " + FormatAppTotalBytes(hovered_app->total_download_bytes),
            L"Upload: " + FormatAppTotalBytes(hovered_app->total_upload_bytes),
            L"Total: " + FormatAppTotalBytes(AppTrafficPresentation::SessionBytes(*hovered_app)),
            L"Session only; all captured adapters"
        };
        for (int row = 0; row < 5; ++row) {
            const float y = top + 33.0f + row * 20.0f;
            render_target_->DrawText(rows[row].c_str(), static_cast<UINT32>(rows[row].size()), format_tooltip_body_,
                D2D1::RectF(left + 12.0f, y, left + tooltip_width - 12.0f, y + 18.0f), brush_tooltip_text_);
        }
    }

    if (alternate_row_brush) alternate_row_brush->Release();
    if (high_usage_row_brush) high_usage_row_brush->Release();
    if (icon_brush) icon_brush->Release();
    if (inactive_rate_brush) inactive_rate_brush->Release();
    ReportContentBottom(table_rect.bottom + 20.0f);
}

void AppRenderer::DrawNetworkHealth(D2D1_SIZE_F rt_size, const NetworkSnapshot& net_snap, const DashboardSnapshot& dash_snap, const SpeedTestSnapshot& speed) {
    const auto& speed_res = speed.result;
    const bool speed_running = speed.Running();
    const NetworkTabLayout layout = GetNetworkTabLayout(rt_size.width);
    const float start_x = layout.side;
    const float start_y = layout.start_y;
    const float content_w = layout.content_w;
    const auto draw_surface = [this](const D2D1_RECT_F& rect, float radius) {
        const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        if (effective_theme_ != ThemeMode::Light) render_target_->FillRoundedRectangle(rounded, brush_card_bg_);
        render_target_->DrawRoundedRectangle(rounded, brush_card_border_, 1.0f);
    };

    // Top quality status card
    const D2D1_RECT_F score_card = D2D1::RectF(
        start_x, start_y, start_x + content_w, start_y + layout.quality_h
    );
    draw_surface(score_card, layout.quality_h / 2.0f);

    // Green/Amber Dot
    const bool has_replies = net_snap.ping_samples_received > 0;
    const bool offline = !net_snap.is_connected || !has_replies;
    std::wstring health_label;
    D2D1_COLOR_F status_color = palette_.AccentSystem;
    if (offline) {
        health_label = !WatchmanLiveReadings::NetworkReady(dash_snap) ? L"Starting..." : !net_snap.is_connected ? L"Disconnected"
            : (net_snap.ping_samples_sent == 0 ? L"Not measured" : L"No ICMP reply");
        status_color = D2D1::ColorF(0.96f, 0.62f, 0.04f);
    } else if (net_snap.packet_loss_percent >= 20.0f ||
               net_snap.latency_ms >= 180.0 ||
               net_snap.jitter_ms >= 50.0) {
        health_label = L"Unstable";
        status_color = D2D1::ColorF(0.93f, 0.25f, 0.23f);
    } else if (net_snap.packet_loss_percent > 0.0f ||
               net_snap.latency_ms >= 90.0 ||
               net_snap.jitter_ms >= 20.0) {
        health_label = L"Fair";
        status_color = D2D1::ColorF(0.96f, 0.62f, 0.04f);
    } else {
        health_label = L"Stable";
    }

    ID2D1SolidColorBrush* status_brush = nullptr;
    render_target_->CreateSolidColorBrush(status_color, &status_brush);
    render_target_->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(start_x + 37.0f, start_y + 42.0f), 16.0f, 16.0f),
        status_brush
    );

    // Quality labels
    render_target_->DrawText(
        L"ICMP HEALTH / WINDOWS ROUTE", 27, format_dashboard_label_,
        D2D1::RectF(start_x + 68.0f, start_y + 15.0f, start_x + content_w - 16.0f, start_y + 30.0f),
        brush_text_secondary_
    );
    render_target_->DrawText(
        health_label.c_str(), static_cast<UINT32>(health_label.size()), format_network_quality_,
        D2D1::RectF(start_x + 68.0f, start_y + 30.0f, start_x + content_w - 16.0f, start_y + 52.0f),
        brush_text_primary_
    );

    std::wstring health_subtitle;
    if (offline) {
        health_subtitle = L"Waiting for replies from 8.8.8.8";
    } else {
        std::wstringstream subtitle;
        subtitle << std::fixed << std::setprecision(0)
                 << net_snap.latency_ms << L"ms avg \x00B7 "
                 << net_snap.jitter_ms << L"ms jitter \x00B7 "
                 << net_snap.packet_loss_percent << L"% loss";
        if (net_snap.active_adapter_type == L"Wi-Fi" && net_snap.wifi_signal_percent > 0) {
            subtitle << L" \x00B7 " << net_snap.wifi_signal_percent << L"% signal";
        } else if (net_snap.active_adapter_type == L"Ethernet") {
            subtitle << L" \x00B7 wired";
        }
        health_subtitle = subtitle.str();
    }
    render_target_->DrawText(
        health_subtitle.c_str(), static_cast<UINT32>(health_subtitle.size()), format_network_quality_sub_,
        D2D1::RectF(start_x + 68.0f, start_y + 52.0f, start_x + content_w - 16.0f, start_y + 71.0f),
        brush_text_secondary_
    );

    if (status_brush) {
        status_brush->Release();
    }


    // 3 Diagnostic split cards: Latency | Jitter | Packet Loss
    const float row_y = layout.metrics_y;
    const float metric_gap = 10.0f;
    const float card_w = (content_w - metric_gap * 2.0f) / 3.0f;
    const float detail_h = layout.metrics_h;
    const auto draw_metric = [this, &draw_surface, row_y, detail_h](
        const D2D1_RECT_F& rect,
        const wchar_t* label,
        const std::wstring& value,
        const std::wstring& detail
    ) {
        draw_surface(rect, 24.0f);
        render_target_->DrawText(
            label, static_cast<UINT32>(wcslen(label)), format_network_metric_label_,
            D2D1::RectF(rect.left + 8.0f, row_y + 15.0f, rect.right - 8.0f, row_y + 30.0f),
            brush_text_secondary_
        );
        render_target_->DrawText(
            value.c_str(), static_cast<UINT32>(value.size()), format_network_metric_value_,
            D2D1::RectF(rect.left + 6.0f, row_y + 31.0f, rect.right - 6.0f, row_y + 67.0f),
            brush_text_primary_
        );
        render_target_->DrawText(
            detail.c_str(), static_cast<UINT32>(detail.size()), format_network_metric_detail_,
            D2D1::RectF(rect.left + 6.0f, row_y + 75.0f, rect.right - 6.0f, row_y + detail_h - 8.0f),
            brush_text_secondary_
        );
    };

    std::wstring latency_value = L"--";
    std::wstring jitter_value = L"--";
    std::wstring loss_value = L"--";
    std::wstring latency_detail = L"Waiting for replies";
    std::wstring jitter_detail = L"Need more than one reply";
    std::wstring loss_detail = L"Waiting for replies";
    if (net_snap.ping_samples_sent > 0) {
        std::wstringstream latency_text;
        latency_text << std::fixed << std::setprecision(0) << net_snap.latency_ms << L"ms";
        // A firewall can block ICMP while DNS/HTTPS still work. Without a
        // reply latency is unknown, not evidence of an offline connection.
        latency_value = has_replies ? latency_text.str() : L"--";

        std::wstringstream replies;
        replies << net_snap.ping_samples_received << L"/" << net_snap.ping_samples_sent
                << L" replies from 8.8.8.8";
        latency_detail = replies.str();

        std::wstringstream loss_text;
        loss_text << std::fixed << std::setprecision(0) << net_snap.packet_loss_percent << L"%";
        loss_value = loss_text.str();

        std::wstringstream lost;
        lost << (net_snap.ping_samples_sent - net_snap.ping_samples_received)
             << L" of " << net_snap.ping_samples_sent << L" lost";
        loss_detail = lost.str();
    }
    if (net_snap.ping_samples_received > 1) {
        std::wstringstream jitter_text;
        jitter_text << std::fixed << std::setprecision(0) << net_snap.jitter_ms << L"ms";
        jitter_value = jitter_text.str();

        std::wstringstream range;
        range << L"Min " << std::fixed << std::setprecision(0) << net_snap.min_latency_ms
              << L"ms / Max " << net_snap.max_latency_ms << L"ms";
        jitter_detail = range.str();
    }

    const D2D1_RECT_F lat_rect = D2D1::RectF(start_x, row_y, start_x + card_w, row_y + detail_h);
    const D2D1_RECT_F jit_rect = D2D1::RectF(
        lat_rect.right + metric_gap, row_y, lat_rect.right + metric_gap + card_w, row_y + detail_h
    );
    const D2D1_RECT_F loss_rect = D2D1::RectF(
        jit_rect.right + metric_gap, row_y, jit_rect.right + metric_gap + card_w, row_y + detail_h
    );
    draw_metric(lat_rect, L"LATENCY", latency_value, latency_detail);
    draw_metric(jit_rect, L"JITTER", jitter_value, jitter_detail);
    draw_metric(loss_rect, L"PACKET LOSS", loss_value, loss_detail);


    // Connection Grid details card
    const float grid_y = layout.connection_y;
    const float grid_h = layout.connection_h;
    const D2D1_RECT_F grid_card = D2D1::RectF(
        start_x, grid_y, start_x + content_w, grid_y + grid_h
    );
    draw_surface(grid_card, 24.0f);

    render_target_->DrawText(
        L"CONNECTION", 10, format_body_bold_,
        D2D1::RectF(start_x + 16.0f, grid_y + 14.0f, start_x + 220.0f, grid_y + 34.0f),
        brush_text_primary_
    );

    const wchar_t* keys[] = { L"TYPE", L"SSID / ADAPTER", L"LINK SPEED", L"LOCAL IP" };
    std::wstring ssid_str = net_snap.wifi_ssid;
    if (ssid_str.empty()) {
        ssid_str = net_snap.active_adapter_name;
    }
    if (ssid_str.empty()) {
        ssid_str = L"Unavailable";
    } else if (ssid_str.length() > 24) {
        ssid_str = ssid_str.substr(0, 21) + L"...";
    }

    const std::wstring connection_type = net_snap.is_connected
        ? net_snap.active_adapter_type
        : L"Offline";
    const std::wstring values[] = {
        connection_type,
        ssid_str,
        FormatNetworkLinkSpeed(net_snap.receive_link_speed_bps, net_snap.link_speed_bps),
        net_snap.local_ip.empty() ? L"Unavailable" : net_snap.local_ip
    };

    const float icon_card_right = start_x + content_w - 16.0f;
    const float icon_card_x = icon_card_right - layout.signal_w;
    const float values_right = icon_card_x - 14.0f;
    const float values_left = start_x + content_w * 0.42f;
    for (int i = 0; i < 4; ++i) {
        const float text_y = grid_y + 43.0f + static_cast<float>(i) * 28.0f;
        render_target_->DrawText(
            keys[i], static_cast<UINT32>(wcslen(keys[i])), format_network_row_label_,
            D2D1::RectF(start_x + 16.0f, text_y, values_left - 12.0f, text_y + 17.0f),
            brush_text_secondary_
        );
        render_target_->DrawText(
            values[i].c_str(), static_cast<UINT32>(values[i].size()), format_network_row_value_,
            D2D1::RectF(values_left, text_y - 1.0f, values_right, text_y + 19.0f),
            brush_text_primary_
        );
        if (i < 3) {
            render_target_->DrawLine(
                D2D1::Point2F(start_x + 16.0f, text_y + 24.0f),
                D2D1::Point2F(values_right, text_y + 24.0f),
                brush_chart_grid_,
                1.0f
            );
        }
    }

    // Right signal card icon
    const float icon_card_top = grid_y + 52.0f;
    const D2D1_RECT_F icon_card = D2D1::RectF(
        icon_card_x, icon_card_top, icon_card_right, icon_card_top + layout.signal_h
    );
    D2D1_ROUNDED_RECT icon_round = D2D1::RoundedRect(icon_card, 16.0f, 16.0f);
    render_target_->FillRoundedRectangle(icon_round, brush_sidebar_bg_);
    render_target_->DrawRoundedRectangle(icon_round, brush_card_border_, 1.0f);

    if (net_snap.active_adapter_type == L"Wi-Fi") {
        DrawVectorIcon(
            render_target_,
            VectorIcon::Wifi,
            D2D1::RectF(icon_card.left + 26.0f, icon_card.top + 7.0f, icon_card.right - 26.0f, icon_card.top + 45.0f),
            brush_accent_sys_,
            2.0f
        );
    } else {
        render_target_->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F((icon_card.left + icon_card.right) / 2.0f, icon_card.top + 29.0f),
                7.0f,
                7.0f
            ),
            net_snap.is_connected ? brush_accent_sys_ : brush_text_secondary_
        );
        render_target_->DrawLine(
            D2D1::Point2F(icon_card.left + 30.0f, icon_card.top + 45.0f),
            D2D1::Point2F(icon_card.right - 30.0f, icon_card.top + 45.0f),
            net_snap.is_connected ? brush_accent_sys_ : brush_text_secondary_,
            3.0f
        );
    }

    std::wstring signal_label = NetworkSignalLabel(net_snap);
    if (net_snap.active_adapter_type == L"Wi-Fi") {
        const size_t separator = signal_label.rfind(L' ');
        if (separator != std::wstring::npos) {
            signal_label.replace(separator, 1, L"\n");
        }
    }
    render_target_->DrawText(
        signal_label.c_str(), static_cast<UINT32>(signal_label.size()), format_network_signal_label_,
        D2D1::RectF(icon_card.left + 6.0f, icon_card.top + 51.0f, icon_card.right - 6.0f, icon_card.bottom - 7.0f),
        brush_text_secondary_
    );

    // This choice controls monitoring across Watchman, including the dashboard
    // and taskbar widget. It intentionally lives with connection details, not
    // inside Speed Test, which always follows Windows' active route.
    const D2D1_RECT_F adapter_row = GetNetworkAdapterRowRect(layout);
    const D2D1_ROUNDED_RECT adapter_round = D2D1::RoundedRect(
        adapter_row,
        UIStyle::CORNER_RADIUS_SMALL,
        UIStyle::CORNER_RADIUS_SMALL
    );
    render_target_->FillRoundedRectangle(adapter_round, brush_card_bg_);
    render_target_->DrawRoundedRectangle(adapter_round, brush_card_border_, 1.0f);
    render_target_->DrawText(
        L"MONITORING ADAPTER",
        18,
        format_network_row_label_,
        D2D1::RectF(adapter_row.left + 12.0f, adapter_row.top + 7.0f, adapter_row.left + 156.0f, adapter_row.bottom - 7.0f),
        brush_text_secondary_
    );
    std::wstring adapter_label = GetNetworkAdapterSelectionLabel();
    if (adapter_label.size() > 28) {
        adapter_label = adapter_label.substr(0, 25) + L"...";
    }
    render_target_->DrawText(
        adapter_label.c_str(),
        static_cast<UINT32>(adapter_label.size()),
        format_network_row_value_,
        D2D1::RectF(adapter_row.left + 160.0f, adapter_row.top + 6.0f, adapter_row.right - 30.0f, adapter_row.bottom - 6.0f),
        brush_text_primary_
    );
    render_target_->DrawText(
        L">",
        1,
        format_body_bold_,
        D2D1::RectF(adapter_row.right - 22.0f, adapter_row.top + 6.0f, adapter_row.right - 9.0f, adapter_row.bottom - 6.0f),
        brush_text_primary_
    );


    // Speed test card at the bottom
    const float test_y_pos = layout.speed_test_y;
    const auto result_status = SpeedTestPresentation::GetStatus(speed_res.download_state, speed_res.upload_state,
        speed_res.upload_server_confirmed, !speed_res.error_message.empty(), !speed_res.warning_message.empty());
    const bool show_notice = !speed_running && result_status.notice;
    const float test_h = show_notice ? 396.0f : layout.speed_test_h;
#ifdef WATCHMAN_RENDER_TESTING
    test_speed_card_visible_ = test_y_pos - CurrentContentScroll() >= ContentViewportTop() &&
        test_y_pos + test_h - CurrentContentScroll() <= ContentViewportBottom();
#endif
    const D2D1_RECT_F test_rect = D2D1::RectF(
        start_x, test_y_pos, start_x + content_w, test_y_pos + test_h
    );
    draw_surface(test_rect, 24.0f);

    const std::wstring test_heading = L"SPEED TEST";
    render_target_->DrawText(test_heading.c_str(), static_cast<UINT32>(test_heading.size()), format_body_bold_,
        D2D1::RectF(start_x + 16.0f, test_y_pos + 14.0f, start_x + content_w - 110.0f, test_y_pos + 34.0f),
        brush_text_primary_);
    const std::wstring disclosure = L"Uses substantial data. M-Lab publicly publishes IP, time and results. "
        L"Privacy details and consent are shown before each test.";
    render_target_->DrawText(disclosure.c_str(), static_cast<UINT32>(disclosure.size()), format_app_subtitle_,
        D2D1::RectF(start_x + 16.0f, test_y_pos + 48.0f, start_x + content_w - 18.0f, test_y_pos + 102.0f),
        brush_text_secondary_);

    // Run test button
    const float btn_w = 84.0f;
    const float btn_h = 26.0f;
    const float btn_x = start_x + content_w - btn_w - 16.0f;
    const float btn_y = test_y_pos + 15.0f;
    D2D1_RECT_F btn_rect = D2D1::RectF(btn_x, btn_y, btn_x + btn_w, btn_y + btn_h);
    D2D1_ROUNDED_RECT btn_round = D2D1::RoundedRect(btn_rect, UIStyle::CORNER_RADIUS_LARGE, UIStyle::CORNER_RADIUS_LARGE);
    
    bool is_test_running = speed_running;
    bool hover_run = (mouse_x_ >= btn_x && mouse_x_ <= btn_x + btn_w && mouse_y_ >= btn_y && mouse_y_ <= btn_y + btn_h);
    if (hover_run) render_target_->FillRoundedRectangle(btn_round, brush_hover_);
    
    // Reuse target-owned brushes; no per-speed-frame brush allocation.
    const D2D1_COLOR_F previous_action = brush_action_bg_->GetColor();
    brush_action_bg_->SetColor(is_test_running ? D2D1::ColorF(0.85f, 0.22f, 0.22f, 1.0f) : palette_.ActionBackground);
    render_target_->FillRoundedRectangle(btn_round, brush_action_bg_);
    brush_action_bg_->SetColor(previous_action);
    const std::wstring btn_label = !speed.available ? L"STARTING" :
        speed.phase == SpeedTestPhase::Cancelling ? L"WAIT..." : is_test_running ? L"CANCEL" : L"RUN TEST";
    render_target_->DrawText(btn_label.c_str(), static_cast<UINT32>(btn_label.length()), format_limit_button_, btn_rect, brush_tooltip_text_);

    // Table of speed results
    float res_y = test_y_pos + 112.0f;
    const wchar_t* res_keys[] = { speed_running ? L"DOWNLOAD" : result_status.download_label,
        speed_running ? L"UPLOAD" : result_status.upload_label, L"LATENCY", L"SERVER", speed_running ? L"PROGRESS" : L"LAST TESTED" };
    
    const auto& result = speed_res;
    const std::wstring dl_val = SpeedTestView::Rate(speed, true);
    const std::wstring ul_val = SpeedTestView::Rate(speed, false);
    std::wstring lat_val = L"--", srv_val = L"--";
    std::wstring stat_val = FormatSpeedTestTimestamp(result.completed_at_unix_seconds);
    const auto format_server = [](const SpeedTestResult& value) {
        if (value.server_name.empty()) return std::wstring{};
        if (value.server_city.empty()) return value.server_name;
        return value.server_name + L" \x00B7 " + value.server_city;
    };
    if (result.latency_ms > 0.0) {
        std::wostringstream text;
        text << std::fixed << std::setprecision(1) << result.latency_ms << L" ms";
        lat_val = text.str();
    } else if (speed_running) lat_val = L"Measuring...";
    if (speed_running) {
        if (result.server_name.empty()) srv_val = L"Locating...";
        stat_val = SpeedTestView::Progress(speed);
    } else if (!speed.available) stat_val = L"Monitoring is starting...";
    else if (speed.phase == SpeedTestPhase::Cancelled) stat_val = L"Cancelled";

    if (!result.server_name.empty()) {
        srv_val = format_server(result);
    }
    if (!is_test_running && !result.error_message.empty()) {
        if (srv_val == L"--") srv_val = L"M-Lab unavailable";
        // Keep the actual completion time; the status callout explains failure.
    }

    std::wstring res_vals[] = { dl_val, ul_val, lat_val, srv_val, stat_val };

    for (int i = 0; i < 5; ++i) {
        render_target_->DrawText(res_keys[i], static_cast<UINT32>(wcslen(res_keys[i])), format_network_speed_label_, D2D1::RectF(start_x + 16.0f, res_y, start_x + 116.0f, res_y + 22.0f), brush_text_secondary_);
        render_target_->DrawText(res_vals[i].c_str(), static_cast<UINT32>(res_vals[i].length()), format_network_speed_value_, D2D1::RectF(start_x + 124.0f, res_y, start_x + content_w - 16.0f, res_y + 22.0f), brush_text_primary_);

        res_y += 32.0f;
    }
    if (show_notice) {
        const auto notice = D2D1::RectF(start_x + 16.0f, test_y_pos + 280.0f, start_x + content_w - 16.0f, test_y_pos + 380.0f);
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(notice, 10.0f, 10.0f), brush_popup_bg_);
        render_target_->DrawRoundedRectangle(D2D1::RoundedRect(notice, 10.0f, 10.0f), brush_card_border_, 1.0f);
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(notice.left, notice.top + 12.0f,
            notice.left + 3.0f, notice.bottom - 12.0f), 1.5f, 1.5f), brush_limit_warning_);
        const auto details = SpeedTestPresentation::DetailsButton(start_x, start_x + content_w, test_y_pos);
        render_target_->DrawText(result_status.title, static_cast<UINT32>(wcslen(result_status.title)), format_body_bold_,
            D2D1::RectF(notice.left + 12.0f, notice.top + 14.0f, details.left - 10.0f, notice.top + 36.0f), brush_text_primary_);
        render_target_->DrawText(result_status.detail, static_cast<UINT32>(wcslen(result_status.detail)), format_app_subtitle_,
            D2D1::RectF(notice.left + 12.0f, notice.top + 44.0f, notice.right - 12.0f, notice.bottom - 10.0f), brush_text_secondary_);
        render_target_->FillRoundedRectangle(D2D1::RoundedRect(details, 7.0f, 7.0f),
            IsPointInside(details, mouse_x_, mouse_y_) ? brush_hover_ : brush_card_bg_);
        render_target_->DrawText(L"Details", 7, format_limit_button_, details, brush_text_primary_);
    }
    ReportContentBottom(test_y_pos + test_h + 20.0f);
}

void AppRenderer::DrawTools(
    D2D1_SIZE_F rt_size,
    const TrafficHistorySnapshot& hist_snap,
    const DiagnosticsSnapshot& diagnostics_snap
) {
    const std::vector<std::wstring> saved_months = GetToolsSavedMonths(hist_snap);
    const std::vector<int> saved_years = GetToolsSavedYears(hist_snap);
    const bool uses_month = ToolsExportUsesMonth(tools_export_period_index_);

    if (!uses_month &&
        std::find(saved_years.begin(), saved_years.end(), tools_export_year_) == saved_years.end()) {
        tools_export_year_ = saved_years.empty() ? 0 : saved_years.front();
    }
    if (uses_month &&
        std::find(saved_months.begin(), saved_months.end(), tools_export_month_) == saved_months.end()) {
        tools_export_month_ = saved_months.empty() ? L"" : saved_months.front();
    }

    const ToolsTabLayout layout = GetToolsTabLayout(
        rt_size.width,
        diagnostics_snap,
        !tools_export_status_.empty()
    );
    const float start_x = layout.side;
    const float content_w = layout.content_w;
    const auto contains = [this](const D2D1_RECT_F& rect) {
        return mouse_x_ >= rect.left && mouse_x_ <= rect.right &&
            mouse_y_ >= rect.top && mouse_y_ <= rect.bottom;
    };
    const auto draw_button = [&](const D2D1_RECT_F& rect, const wchar_t* text, bool accent, bool enabled) {
        const D2D1_ROUNDED_RECT round = D2D1::RoundedRect(rect, 14.0f, 14.0f);
        if (accent) {
            ID2D1SolidColorBrush* accent_brush = nullptr;
            D2D1_COLOR_F color = effective_theme_ == ThemeMode::Light
                ? D2D1::ColorF(0.24f, 0.51f, 1.0f) : palette_.ActionBackground;
            if (!enabled) color.a = 0.45f;
            render_target_->CreateSolidColorBrush(color, &accent_brush);
            render_target_->FillRoundedRectangle(round, accent_brush);
            accent_brush->Release();
        } else {
            render_target_->FillRoundedRectangle(round, brush_text_primary_);
        }
        if (enabled && contains(rect)) {
            render_target_->DrawRoundedRectangle(round, brush_hover_, 1.0f);
        }
        ID2D1SolidColorBrush* text_brush = nullptr;
        const D2D1_COLOR_F text_color = (accent || effective_theme_ == ThemeMode::Light)
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
            : D2D1::ColorF(0.0f, 0.0f, 0.0f);
        render_target_->CreateSolidColorBrush(text_color, &text_brush);
        render_target_->DrawText(
            text,
            static_cast<UINT32>(wcslen(text)),
            format_limit_button_,
            rect,
            text_brush
        );
        text_brush->Release();
    };

    // Network diagnostics mirrors the Tauri flow: an idle hint, a running
    // state, then concrete results and fixes from the background service.
    const D2D1_RECT_F diagnostics_card = D2D1::RectF(
        start_x, layout.diagnostics_y, start_x + content_w, layout.diagnostics_y + layout.diagnostics_h
    );
    DrawCard(diagnostics_card);
    render_target_->DrawText(
        L"NETWORK TROUBLESHOOTER", 22, format_body_bold_,
        D2D1::RectF(start_x + 20.0f, layout.diagnostics_y + 15.0f, start_x + 286.0f, layout.diagnostics_y + 34.0f),
        brush_text_primary_
    );
    const D2D1_RECT_F diagnostics_button = GetToolsHeaderButtonRect(layout, layout.diagnostics_y);
    draw_button(
        diagnostics_button,
        diagnostics_snap.is_running ? L"RUNNING..." : L"RUN DIAGNOSTICS",
        true,
        !diagnostics_snap.is_running
    );

    if (diagnostics_snap.is_running) {
        render_target_->DrawText(
            L"Running diagnostic tests...", 27, format_body_,
            D2D1::RectF(start_x + 20.0f, layout.diagnostics_y + 62.0f, start_x + content_w - 20.0f, layout.diagnostics_y + 84.0f),
            brush_text_secondary_
        );
    } else if (!diagnostics_snap.has_run) {
        constexpr wchar_t diagnostics_hint[] = L"Click \"Run Diagnostics\" to check your network";
        render_target_->DrawText(
            diagnostics_hint, static_cast<UINT32>(std::size(diagnostics_hint) - 1), format_body_,
            D2D1::RectF(start_x + 26.0f, layout.diagnostics_y + 62.0f, start_x + content_w - 26.0f, layout.diagnostics_y + 84.0f),
            brush_text_secondary_
        );
    } else {
        float row_y = layout.diagnostics_y + 54.0f;
        for (const DiagnosticTestResult& test : diagnostics_snap.tests) {
            D2D1_COLOR_F dot_color = D2D1::ColorF(0.98f, 0.62f, 0.05f);
            if (test.status == DiagnosticStatus::Passed) {
                dot_color = D2D1::ColorF(0.05f, 0.70f, 0.48f);
            } else if (test.status == DiagnosticStatus::Failed) {
                dot_color = D2D1::ColorF(0.94f, 0.24f, 0.25f);
            }
            ID2D1SolidColorBrush* dot_brush = nullptr;
            render_target_->CreateSolidColorBrush(dot_color, &dot_brush);
            render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(start_x + 28.0f, row_y + 11.0f), 5.5f, 5.5f), dot_brush);
            dot_brush->Release();
            render_target_->DrawText(
                test.name.c_str(), static_cast<UINT32>(test.name.size()), format_body_bold_,
                D2D1::RectF(start_x + 44.0f, row_y, start_x + content_w - 18.0f, row_y + 18.0f), brush_text_primary_
            );
            render_target_->DrawText(
                test.message.c_str(), static_cast<UINT32>(test.message.size()), format_dashboard_note_,
                D2D1::RectF(start_x + 44.0f, row_y + 18.0f, start_x + content_w - 18.0f, row_y + 34.0f), brush_text_secondary_
            );
            row_y += 43.0f;
            if (test.status == DiagnosticStatus::Failed && !test.suggestion.empty()) {
                render_target_->DrawText(
                    test.suggestion.c_str(), static_cast<UINT32>(test.suggestion.size()), format_dashboard_note_,
                    D2D1::RectF(start_x + 44.0f, row_y, start_x + content_w - 18.0f, row_y + 30.0f), brush_text_secondary_
                );
                row_y += 38.0f;
            }
        }
    }

    // Real background check state; never label an offline/failed check current.
    const D2D1_RECT_F updates_card = D2D1::RectF(
        start_x, layout.updates_y, start_x + content_w, layout.updates_y + layout.updates_h
    );
    DrawCard(updates_card);
    render_target_->DrawText(
        L"UPDATES", 7, format_body_bold_,
        D2D1::RectF(start_x + 20.0f, layout.updates_y + 15.0f, start_x + 220.0f, layout.updates_y + 34.0f),
        brush_text_primary_
    );
    const D2D1_RECT_F updates_button = GetToolsHeaderButtonRect(layout, layout.updates_y);
    const bool checking_update = update_notice_.Last().status == WatchmanUpdates::Status::Checking;
    draw_button(updates_button, checking_update ? L"CHECKING..." : L"CHECK FOR UPDATES", true, !checking_update);
    const D2D1_RECT_F updates_status = D2D1::RectF(
        start_x + 20.0f, layout.updates_y + 56.0f, start_x + content_w - 20.0f, layout.updates_y + 116.0f
    );
    const D2D1_ROUNDED_RECT updates_status_round = D2D1::RoundedRect(updates_status, 18.0f, 18.0f);
    render_target_->FillRoundedRectangle(updates_status_round, brush_sidebar_bg_);
    render_target_->DrawRoundedRectangle(updates_status_round, brush_card_border_, 1.0f);
    render_target_->DrawText(
        WatchmanUpdates::StatusText(update_notice_.Last()).c_str(),
        static_cast<UINT32>(WatchmanUpdates::StatusText(update_notice_.Last()).size()), format_body_bold_,
        D2D1::RectF(start_x + 35.0f, layout.updates_y + 70.0f, start_x + content_w - 35.0f, layout.updates_y + 89.0f),
        brush_text_primary_
    );
    render_target_->DrawText(
        (update_action_status_.empty() ? L"Official stable releases - no automatic installation" : update_action_status_.c_str()),
        static_cast<UINT32>(update_action_status_.empty() ? wcslen(L"Official stable releases - no automatic installation") : update_action_status_.size()), format_dashboard_note_,
        D2D1::RectF(start_x + 35.0f, layout.updates_y + 91.0f, start_x + content_w - 35.0f, layout.updates_y + 107.0f),
        brush_text_secondary_
    );

    // Export uses the same saved-history scopes as the Rust app. Format is
    // intentionally CSV-only, while period and scope are native menu pickers.
    const D2D1_RECT_F export_card = D2D1::RectF(
        start_x, layout.export_y, start_x + content_w, layout.export_y + layout.export_h
    );
    DrawCard(export_card);
    render_target_->DrawText(
        L"EXPORT CSV", 10, format_body_bold_,
        D2D1::RectF(start_x + 20.0f, layout.export_y + 15.0f, start_x + 250.0f, layout.export_y + 34.0f),
        brush_text_primary_
    );

    const float value_w = std::min(180.0f, std::max(134.0f, content_w * 0.37f));
    const float value_x = start_x + content_w - 20.0f - value_w;
    const auto draw_label = [&](const wchar_t* label, float top) {
        render_target_->DrawText(
            label, static_cast<UINT32>(wcslen(label)), format_body_bold_,
            D2D1::RectF(start_x + 20.0f, top + 5.0f, value_x - 14.0f, top + 25.0f), brush_text_primary_
        );
    };
    const auto draw_picker = [&](const D2D1_RECT_F& rect, const std::wstring& value, bool static_pill) {
        const D2D1_ROUNDED_RECT round = D2D1::RoundedRect(rect, 13.0f, 13.0f);
        if (static_pill) {
            render_target_->FillRoundedRectangle(round, brush_text_primary_);
        } else {
            render_target_->FillRoundedRectangle(round, brush_sidebar_bg_);
            render_target_->DrawRoundedRectangle(round, brush_card_border_, 1.0f);
            // Native selects use a compact chevron, not a letter-shaped "v".
            const float chevron_x = rect.right - 14.0f;
            const float chevron_y = (rect.top + rect.bottom) * 0.5f - 0.5f;
            render_target_->DrawLine(
                D2D1::Point2F(chevron_x - 3.5f, chevron_y - 1.5f),
                D2D1::Point2F(chevron_x, chevron_y + 2.0f),
                brush_text_primary_,
                1.25f
            );
            render_target_->DrawLine(
                D2D1::Point2F(chevron_x, chevron_y + 2.0f),
                D2D1::Point2F(chevron_x + 3.5f, chevron_y - 1.5f),
                brush_text_primary_,
                1.25f
            );
        }
        ID2D1SolidColorBrush* value_brush = brush_text_primary_;
        if (static_pill) {
            const D2D1_COLOR_F inverse = effective_theme_ == ThemeMode::Light
                ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
                : D2D1::ColorF(0.0f, 0.0f, 0.0f);
            render_target_->CreateSolidColorBrush(inverse, &value_brush);
        }
        render_target_->DrawText(
            value.c_str(), static_cast<UINT32>(value.size()), format_sidebar_,
            D2D1::RectF(rect.left + 10.0f, rect.top, rect.right - (static_pill ? 10.0f : 30.0f), rect.bottom), value_brush
        );
        if (static_pill) value_brush->Release();
    };

    const float first_row_y = layout.export_y + 53.0f;
    const D2D1_RECT_F format_rect = D2D1::RectF(value_x, first_row_y, value_x + value_w, first_row_y + 28.0f);
    const D2D1_RECT_F period_rect = D2D1::RectF(value_x, first_row_y + 36.0f, value_x + value_w, first_row_y + 64.0f);
    const D2D1_RECT_F scope_rect = D2D1::RectF(value_x, first_row_y + 72.0f, value_x + value_w, first_row_y + 100.0f);
    draw_label(L"FORMAT", first_row_y);
    draw_picker(format_rect, L"CSV only", true);
    draw_label(L"PERIOD", first_row_y + 36.0f);
    draw_picker(period_rect, ToolsExportPeriodLabel(tools_export_period_index_), false);
    draw_label(uses_month ? L"MONTH" : L"YEAR", first_row_y + 72.0f);
    const std::wstring scope_value = uses_month
        ? (tools_export_month_.empty() ? L"No data yet" : FormatToolsMonthLabel(tools_export_month_))
        : (tools_export_year_ == 0 ? L"No data yet" : std::to_wstring(tools_export_year_));
    draw_picker(scope_rect, scope_value, false);

    const wchar_t* helper = uses_month
        ? (tools_export_period_index_ == 0
            ? L"Export every saved day from the selected month."
            : L"Export only saved weeks from the selected month.")
        : (tools_export_period_index_ == 2
            ? L"Export every saved month from the selected year."
            : L"Export the saved total for the selected year only.");
    render_target_->DrawText(
        helper, static_cast<UINT32>(wcslen(helper)), format_dashboard_note_,
        D2D1::RectF(start_x + 20.0f, layout.export_y + 160.0f, start_x + content_w - 20.0f, layout.export_y + 178.0f),
        brush_text_secondary_
    );

    const bool can_export = uses_month ? !tools_export_month_.empty() : tools_export_year_ != 0;
    const D2D1_RECT_F export_button = GetToolsDownloadButtonRect(layout);
    draw_button(export_button, L"DOWNLOAD CSV", false, can_export);

    if (!tools_export_status_.empty()) {
        ID2D1SolidColorBrush* status_brush = nullptr;
        render_target_->CreateSolidColorBrush(
            tools_export_status_success_
                ? D2D1::ColorF(0.04f, 0.63f, 0.43f)
                : D2D1::ColorF(0.88f, 0.20f, 0.20f),
            &status_brush
        );
        render_target_->DrawText(
            tools_export_status_.c_str(), static_cast<UINT32>(tools_export_status_.size()), format_dashboard_note_,
            D2D1::RectF(start_x + 20.0f, layout.export_y + 230.0f, start_x + content_w - 20.0f, layout.export_y + 252.0f),
            status_brush
        );
        status_brush->Release();
    }

    ReportContentBottom(layout.bottom);
}

std::wstring AppRenderer::GetNetworkAdapterSelectionLabel() const {
    const std::wstring& selected_id = settings_open_
        ? preferences_draft_.network_adapter_id
        : network_adapter_id_;
    if (selected_id.empty()) {
        return L"Auto - follow Windows";
    }

    for (const InterfaceInfo& adapter : settings_network_adapters_) {
        if (_wcsicmp(adapter.id.c_str(), selected_id.c_str()) == 0) {
            return !adapter.description.empty()
                ? adapter.description
                : (adapter.name.empty() ? adapter.type : adapter.name);
        }
    }

    return L"Selected adapter unavailable";
}

AppRenderer::PreferencesDraft AppRenderer::CapturePreferences() const {
    PreferencesDraft draft{};
    draft.autostart_enabled = autostart_enabled_;
    draft.use_bits_per_second = use_bits_per_second_;
    draft.hide_hardware_gauges = hide_hardware_gauges_;
    draft.theme = current_theme_;
    draft.monthly_data_limit_bytes = data_limit_bytes_;
    draft.alerts_enabled = alerts_enabled_;
    draft.traffic_warning_enabled = traffic_warning_enabled_;
    draft.traffic_warning_threshold = traffic_warning_threshold_;
    draft.traffic_warning_gigabytes = traffic_warning_gigabytes_;
    draft.memory_warning_enabled = memory_warning_enabled_;
    draft.memory_warning_threshold = memory_warning_threshold_;
    draft.cpu_temp_warning_enabled = cpu_temp_warning_enabled_;
    draft.cpu_temp_warning_threshold = cpu_temp_warning_threshold_;
    draft.gpu_temp_warning_enabled = gpu_temp_warning_enabled_;
    draft.gpu_temp_warning_threshold = gpu_temp_warning_threshold_;
    draft.disk_temp_warning_enabled = disk_temp_warning_enabled_;
    draft.disk_temp_warning_threshold = disk_temp_warning_threshold_;
    draft.mainboard_temp_warning_enabled = mainboard_temp_warning_enabled_;
    draft.mainboard_temp_warning_threshold = mainboard_temp_warning_threshold_;
    draft.widget_enabled = widget_enabled_;
    draft.widget_mode = widget_mode_;
    draft.network_adapter_id = network_adapter_id_;
    draft.floating_monitor_enabled = floating_monitor_enabled_;
    draft.floating_monitor_show_cpu = floating_monitor_show_cpu_;
    draft.floating_monitor_show_memory = floating_monitor_show_memory_;
    draft.floating_monitor_always_on_top = floating_monitor_always_on_top_;
    draft.floating_monitor_lock_position = floating_monitor_lock_position_;
    draft.floating_monitor_click_through = floating_monitor_click_through_;
    draft.floating_monitor_transparent_background = floating_monitor_transparent_background_;
    draft.floating_monitor_opacity = floating_monitor_opacity_;
    draft.floating_monitor_display_device = floating_monitor_display_device_;

    draft.floating_monitor_text_color = floating_monitor_text_color_;
    draft.floating_monitor_upload_color = floating_monitor_upload_color_;
    draft.floating_monitor_download_color = floating_monitor_download_color_;
    draft.floating_monitor_cpu_color = floating_monitor_cpu_color_;
    draft.floating_monitor_memory_color = floating_monitor_memory_color_;

    draft.floating_monitor_font_size = floating_monitor_font_size_;
    draft.floating_monitor_text_weight = floating_monitor_text_weight_;
    return draft;
}

void AppRenderer::SetFloatingMonitorEnabled(bool enabled) {
    floating_monitor_enabled_ = enabled;
    if (settings_open_) preferences_draft_.floating_monitor_enabled = enabled;
}

void AppRenderer::SetFloatingMonitorCpuShown(bool show) {
    floating_monitor_show_cpu_ = show;
    if (settings_open_) preferences_draft_.floating_monitor_show_cpu = show;
}

void AppRenderer::SetFloatingMonitorMemoryShown(bool show) {
    floating_monitor_show_memory_ = show;
    if (settings_open_) preferences_draft_.floating_monitor_show_memory = show;
}

void AppRenderer::SetFloatingMonitorPositionLocked(bool locked) {
    floating_monitor_lock_position_ = locked;
    if (settings_open_) preferences_draft_.floating_monitor_lock_position = locked;
}

void AppRenderer::SetFloatingMonitorAlwaysOnTop(bool enabled) {
    floating_monitor_always_on_top_ = enabled;
    if (settings_open_) preferences_draft_.floating_monitor_always_on_top = enabled;
}

void AppRenderer::SetFloatingMonitorClickThrough(bool enabled) {
    floating_monitor_click_through_ = enabled;
    if (settings_open_) preferences_draft_.floating_monitor_click_through = enabled;
}

void AppRenderer::SetFloatingMonitorBackgroundTransparent(bool transparent) {
    floating_monitor_transparent_background_ = transparent;
    if (settings_open_) preferences_draft_.floating_monitor_transparent_background = transparent;
}

void AppRenderer::SetFloatingMonitorOpacity(int opacity) {
    floating_monitor_opacity_ = FloatingMonitorLayout::ClampOpacity(opacity);
    if (settings_open_) preferences_draft_.floating_monitor_opacity = floating_monitor_opacity_;
}

void AppRenderer::SetFloatingMonitorFontSize(int size) {
    floating_monitor_font_size_ = FloatingMonitorLayout::ClampFontSize(size);
    if (settings_open_) preferences_draft_.floating_monitor_font_size = floating_monitor_font_size_;
}

void AppRenderer::SetFloatingMonitorTextWeight(FloatingMonitorText::Weight weight) {
    floating_monitor_text_weight_ = FloatingMonitorText::FromSaved(static_cast<int>(weight));
    if (settings_open_) preferences_draft_.floating_monitor_text_weight = floating_monitor_text_weight_;
}

void AppRenderer::OpenFloatingMonitorSettings() {
    if (!settings_open_) BeginSettingsSession();
    else CommitVisibleSettingsEditors();
    settings_open_ = true;
    settings_section_ = 3;
    settings_appearance_scroll_offset_ = 0.0f;
}

void AppRenderer::BeginSettingsSession() {
    HideSettingsEditors();
    settings_general_scroll_offset_ = 0.0f;
    settings_general_scroll_max_ = 0.0f;
    settings_widgets_scroll_offset_ = 0.0f;
    settings_widgets_scroll_max_ = 0.0f;
    settings_warnings_scroll_offset_ = 0.0f;
    settings_warnings_scroll_max_ = 0.0f;
    settings_appearance_scroll_offset_ = 0.0f;
    settings_appearance_scroll_max_ = 0.0f;
    preferences_draft_ = CapturePreferences();
    preferences_dirty_ = false;
    preferences_apply_requested_ = false;
    floating_monitor_reset_position_requested_ = false;
    floating_monitor_display_picker_requested_ = false;
}

void AppRenderer::MarkPreferencesDirty() {
    preferences_dirty_ = true;
}

void AppRenderer::RestorePreferencesDraft() {
    HideSettingsEditors();
    preferences_draft_ = CapturePreferences();
    preferences_dirty_ = false;
    floating_monitor_reset_position_requested_ = false;
    floating_monitor_display_picker_requested_ = false;
}

void AppRenderer::UseRecommendedPreferences() {
    HideSettingsEditors();
    if (settings_section_ == 3) {
        preferences_draft_.floating_monitor_text_color = CLR_INVALID;
        preferences_draft_.floating_monitor_upload_color = CLR_INVALID;
        preferences_draft_.floating_monitor_download_color = CLR_INVALID;
        preferences_draft_.floating_monitor_cpu_color = CLR_INVALID;
        preferences_draft_.floating_monitor_memory_color = CLR_INVALID;
        preferences_draft_.floating_monitor_show_cpu = FloatingMonitorLayout::default_show_cpu;
        preferences_draft_.floating_monitor_show_memory = FloatingMonitorLayout::default_show_memory;
        preferences_draft_.floating_monitor_font_size = FloatingMonitorLayout::default_font_size;
        preferences_draft_.floating_monitor_text_weight = FloatingMonitorText::default_weight;
        MarkPreferencesDirty();
        return;
    }
    PreferencesDraft recommended = CapturePreferences();
    recommended.autostart_enabled = true;
    recommended.use_bits_per_second = false;
    recommended.hide_hardware_gauges = false;
    recommended.theme = ThemeMode::Light;
    recommended.monthly_data_limit_bytes = 0;
    recommended.alerts_enabled = true;
    recommended.traffic_warning_enabled = false;
    recommended.traffic_warning_threshold = 500;
    recommended.traffic_warning_gigabytes = false;
    recommended.memory_warning_enabled = true;
    recommended.memory_warning_threshold = 90;
    recommended.cpu_temp_warning_enabled = true;
    recommended.cpu_temp_warning_threshold = 80;
    recommended.gpu_temp_warning_enabled = true;
    recommended.gpu_temp_warning_threshold = 80;
    recommended.disk_temp_warning_enabled = true;
    recommended.disk_temp_warning_threshold = 80;
    recommended.mainboard_temp_warning_enabled = true;
    recommended.mainboard_temp_warning_threshold = 80;
    recommended.widget_enabled = true;
    recommended.widget_mode = TaskbarWidgetMode::All;
    recommended.floating_monitor_enabled = false;
    recommended.floating_monitor_show_cpu = FloatingMonitorLayout::default_show_cpu;
    recommended.floating_monitor_show_memory = FloatingMonitorLayout::default_show_memory;
    recommended.floating_monitor_always_on_top = true;
    recommended.floating_monitor_lock_position = false;
    recommended.floating_monitor_click_through = false;
    recommended.floating_monitor_transparent_background = false;
    recommended.floating_monitor_opacity = 92;
    recommended.floating_monitor_display_device.clear();

    recommended.floating_monitor_text_color = CLR_INVALID;
    recommended.floating_monitor_upload_color = CLR_INVALID;
    recommended.floating_monitor_download_color = CLR_INVALID;
    recommended.floating_monitor_cpu_color = CLR_INVALID;
    recommended.floating_monitor_memory_color = CLR_INVALID;

    recommended.floating_monitor_font_size = FloatingMonitorLayout::default_font_size;
    recommended.floating_monitor_text_weight = FloatingMonitorText::default_weight;
    preferences_draft_ = std::move(recommended);
    MarkPreferencesDirty();
}

void AppRenderer::CommitPreferencesDraft() {
    CommitVisibleSettingsEditors();

    autostart_enabled_ = preferences_draft_.autostart_enabled;
    use_bits_per_second_ = preferences_draft_.use_bits_per_second;
    hide_hardware_gauges_ = preferences_draft_.hide_hardware_gauges;
    data_limit_bytes_ = preferences_draft_.monthly_data_limit_bytes;
    alerts_enabled_ = preferences_draft_.alerts_enabled;
    traffic_warning_enabled_ = preferences_draft_.traffic_warning_enabled;
    traffic_warning_threshold_ = preferences_draft_.traffic_warning_threshold;
    traffic_warning_gigabytes_ = preferences_draft_.traffic_warning_gigabytes;
    memory_warning_enabled_ = preferences_draft_.memory_warning_enabled;
    memory_warning_threshold_ = preferences_draft_.memory_warning_threshold;
    cpu_temp_warning_enabled_ = preferences_draft_.cpu_temp_warning_enabled;
    cpu_temp_warning_threshold_ = preferences_draft_.cpu_temp_warning_threshold;
    gpu_temp_warning_enabled_ = preferences_draft_.gpu_temp_warning_enabled;
    gpu_temp_warning_threshold_ = preferences_draft_.gpu_temp_warning_threshold;
    disk_temp_warning_enabled_ = preferences_draft_.disk_temp_warning_enabled;
    disk_temp_warning_threshold_ = preferences_draft_.disk_temp_warning_threshold;
    mainboard_temp_warning_enabled_ = preferences_draft_.mainboard_temp_warning_enabled;
    mainboard_temp_warning_threshold_ = preferences_draft_.mainboard_temp_warning_threshold;
    widget_enabled_ = preferences_draft_.widget_enabled;
    widget_mode_ = preferences_draft_.widget_mode;
    network_adapter_id_ = preferences_draft_.network_adapter_id;
    floating_monitor_enabled_ = preferences_draft_.floating_monitor_enabled;
    floating_monitor_show_cpu_ = preferences_draft_.floating_monitor_show_cpu;
    floating_monitor_show_memory_ = preferences_draft_.floating_monitor_show_memory;
    floating_monitor_always_on_top_ = preferences_draft_.floating_monitor_always_on_top;
    floating_monitor_lock_position_ = preferences_draft_.floating_monitor_lock_position;
    floating_monitor_click_through_ = preferences_draft_.floating_monitor_click_through;
    floating_monitor_transparent_background_ = preferences_draft_.floating_monitor_transparent_background;
    floating_monitor_opacity_ = FloatingMonitorLayout::ClampOpacity(preferences_draft_.floating_monitor_opacity);
    if (floating_monitor_display_device_ != preferences_draft_.floating_monitor_display_device) {
        floating_monitor_reset_position_requested_ = true;
    }
    floating_monitor_display_device_ = preferences_draft_.floating_monitor_display_device;

    floating_monitor_text_color_ = preferences_draft_.floating_monitor_text_color;
    floating_monitor_upload_color_ = preferences_draft_.floating_monitor_upload_color;
    floating_monitor_download_color_ = preferences_draft_.floating_monitor_download_color;
    floating_monitor_cpu_color_ = preferences_draft_.floating_monitor_cpu_color;
    floating_monitor_memory_color_ = preferences_draft_.floating_monitor_memory_color;
    floating_monitor_font_size_ = FloatingMonitorLayout::ClampFontSize(preferences_draft_.floating_monitor_font_size);
    floating_monitor_text_weight_ = FloatingMonitorText::FromSaved(static_cast<int>(preferences_draft_.floating_monitor_text_weight));
    SetTheme(preferences_draft_.theme);
    preferences_dirty_ = false;
    preferences_apply_requested_ = true;
}

void AppRenderer::HideSettingsEditors() {
    CancelSettingsNumericInput();
}

void AppRenderer::CommitVisibleSettingsEditors() {
    CommitSettingsNumericInput();
}

void AppRenderer::BeginSettingsNumericInput(SettingsNumericField field, int value) {
    CommitSettingsNumericInput();
    settings_numeric_field_ = field;
    settings_numeric_text_ = std::to_wstring(std::max(0, value));
    settings_numeric_select_all_ = true;
}

void AppRenderer::CommitSettingsNumericInput() {
    const SettingsNumericField field = settings_numeric_field_;
    if (field == SettingsNumericField::None) {
        return;
    }

    const std::wstring text = settings_numeric_text_;
    settings_numeric_field_ = SettingsNumericField::None;
    settings_numeric_text_.clear();
    settings_numeric_select_all_ = false;

    if (field == SettingsNumericField::MonthlyDataLimit) {
        double gibibytes = 0.0;
        if (!text.empty()) {
            wchar_t* end = nullptr;
            gibibytes = std::wcstod(text.c_str(), &end);
            if (end == text.c_str() || !end || *end != L'\0' || !std::isfinite(gibibytes) || gibibytes < 0.0) {
                return;
            }
        }
        constexpr long double kGibibyte = 1024.0L * 1024.0L * 1024.0L;
        const uint64_t updated_limit = static_cast<uint64_t>(std::min(
            static_cast<long double>(gibibytes) * kGibibyte,
            static_cast<long double>(std::numeric_limits<uint64_t>::max())
        ));
        if (preferences_draft_.monthly_data_limit_bytes != updated_limit) {
            preferences_draft_.monthly_data_limit_bytes = updated_limit;
            MarkPreferencesDirty();
        }
        return;
    }

    long parsed = 0;
    if (!text.empty()) {
        wchar_t* end = nullptr;
        parsed = std::wcstol(text.c_str(), &end, 10);
        if (end == text.c_str() || !end || *end != L'\0') {
            return;
        }
    }

    const bool temperature_field = field == SettingsNumericField::CpuTemperatureWarning ||
        field == SettingsNumericField::GpuTemperatureWarning || field == SettingsNumericField::DiskTemperatureWarning ||
        field == SettingsNumericField::MainboardTemperatureWarning;
    const int maximum = temperature_field ? 150 : (field == SettingsNumericField::MemoryWarning ? 100 : 1000000);
    const int minimum = (temperature_field || field == SettingsNumericField::TrafficWarning || field == SettingsNumericField::MemoryWarning) ? 1 : 0;
    const int value = static_cast<int>(std::clamp<long>(parsed, minimum, maximum));
    int* destination = nullptr;
    switch (field) {
    case SettingsNumericField::TrafficWarning:
        destination = &preferences_draft_.traffic_warning_threshold;
        break;
    case SettingsNumericField::MemoryWarning:
        destination = &preferences_draft_.memory_warning_threshold;
        break;
    case SettingsNumericField::CpuTemperatureWarning:
        destination = &preferences_draft_.cpu_temp_warning_threshold;
        break;
    case SettingsNumericField::GpuTemperatureWarning:
        destination = &preferences_draft_.gpu_temp_warning_threshold;
        break;
    case SettingsNumericField::DiskTemperatureWarning:
        destination = &preferences_draft_.disk_temp_warning_threshold;
        break;
    case SettingsNumericField::MainboardTemperatureWarning:
        destination = &preferences_draft_.mainboard_temp_warning_threshold;
        break;
    default:
        break;
    }
    if (destination && *destination != value) {
        *destination = value;
        MarkPreferencesDirty();
    }
}

void AppRenderer::CancelSettingsNumericInput() {
    settings_numeric_field_ = SettingsNumericField::None;
    settings_numeric_text_.clear();
    settings_numeric_select_all_ = false;
}

bool AppRenderer::IsSettingsNumericInputActive(SettingsNumericField field) const {
    return settings_numeric_field_ == field;
}

void AppRenderer::SetNetworkAdapterId(const std::wstring& adapter_id) {
    if (settings_open_) {
        preferences_draft_.network_adapter_id = adapter_id;
        MarkPreferencesDirty();
        return;
    }
    network_adapter_id_ = adapter_id;
}

void AppRenderer::SetFloatingMonitorDisplayDevice(const std::wstring& display_device) {
    if (settings_open_) {
        preferences_draft_.floating_monitor_display_device = display_device;
        MarkPreferencesDirty();
        return;
    }
    floating_monitor_display_device_ = display_device;
}

bool AppRenderer::ChooseAppearanceColor(HWND hwnd, COLORREF& color, COLORREF automatic_color) {
    static COLORREF custom_colors[16]{};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = hwnd;
    chooser.rgbResult = color == CLR_INVALID ? automatic_color : color;
    chooser.lpCustColors = custom_colors;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&chooser)) {
        return false;
    }
    color = chooser.rgbResult;
    MarkPreferencesDirty();
    return true;
}




WarningsSettingsLayout AppRenderer::GetMeasuredWarningsLayout(float left, float right, float top, float scroll) {
    const float width = std::max(1.0f, right - left - 28.0f);
    if (settings_warning_text_width_ != width || !settings_sensor_setup_text_.layout || !settings_warning_details_text_.layout) {
        SettingsText::CreateParagraph(dwrite_factory_, format_dashboard_note_, WarningsSettingsText::sensor_setup,
            width, settings_sensor_setup_text_);
        SettingsText::CreateParagraph(dwrite_factory_, format_dashboard_note_, WarningsSettingsText::alert_details,
            width, settings_warning_details_text_);
        settings_warning_text_width_ = width;
    }
    return GetWarningsSettingsLayout(left, right, top, scroll,
        settings_sensor_setup_text_.height, settings_warning_details_text_.height);
}

void AppRenderer::DrawSettingsModal(D2D1_SIZE_F rt_size, const DashboardSnapshot& dash_snap) {
    // Keep the page visible around the dialog, but never through its surface.
    D2D1_RECT_F overlay = D2D1::RectF(0.0f, 0.0f, rt_size.width, rt_size.height);
    ID2D1SolidColorBrush* modal_overlay = nullptr;
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f), &modal_overlay);
    render_target_->FillRectangle(overlay, modal_overlay);
    modal_overlay->Release();

    const SettingsModalLayout layout = GetSettingsModalLayout(rt_size);
    const float mx = layout.x;
    const float my = layout.y;
    const float mw = layout.width;
    const float mh = layout.height;
    const D2D1_RECT_F modal_rect = D2D1::RectF(mx, my, mx + mw, my + mh);

    const D2D1_ROUNDED_RECT card_round = D2D1::RoundedRect(
        modal_rect,
        UIStyle::CORNER_RADIUS_LARGE,
        UIStyle::CORNER_RADIUS_LARGE
    );
    ID2D1SolidColorBrush* modal_shadow = nullptr;
    ID2D1SolidColorBrush* modal_surface = nullptr;
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.26f), &modal_shadow);
    D2D1_COLOR_F modal_color = palette_.CardBackground;
    modal_color.a = 1.0f;
    render_target_->CreateSolidColorBrush(modal_color, &modal_surface);
    const D2D1_ROUNDED_RECT shadow_round = D2D1::RoundedRect(
        D2D1::RectF(modal_rect.left + 2.0f, modal_rect.top + 5.0f, modal_rect.right + 2.0f, modal_rect.bottom + 5.0f),
        UIStyle::CORNER_RADIUS_LARGE,
        UIStyle::CORNER_RADIUS_LARGE
    );
    render_target_->FillRoundedRectangle(shadow_round, modal_shadow);
    render_target_->FillRoundedRectangle(card_round, modal_surface);
    modal_shadow->Release();
    modal_surface->Release();
    render_target_->DrawRoundedRectangle(card_round, brush_chart_grid_, 1.0f);

    const float header_bottom = my + SettingsHeaderLayout::height;
    const auto replay = WatchmanWelcome::ReplayButton(mx, my, mw);
    const auto header = SettingsHeaderLayout::Get(mx, my, replay.left,
        settings_title_text_width_, settings_version_text_width_);
    const auto rect = [](const SettingsHeaderLayout::Rect& area) {
        return D2D1::RectF(area.left, area.top, area.right, area.bottom);
    };
    render_target_->DrawText(L"SETTINGS", 8, format_dashboard_value_, rect(header.title),
        brush_text_primary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);

    // A non-interactive, theme-aware badge identifies the running build on
    // every Settings page, without displacing Quick tour or the close control.
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(rect(header.version), 8, 8), brush_active_);
    settings_version_layout_->SetMaxWidth(header.version_text.right - header.version_text.left);
    settings_version_layout_->SetMaxHeight(header.version_text.bottom - header.version_text.top);
    settings_version_layout_->SetFontSize(SettingsHeaderLayout::version_font_size * header.version_font_scale,
        DWRITE_TEXT_RANGE{0, static_cast<UINT32>(WatchmanVersion::Label().size())});
    render_target_->DrawTextLayout(D2D1::Point2F(header.version_text.left, header.version_text.top),
        settings_version_layout_, brush_text_primary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const auto replay_rect = D2D1::RectF(replay.left, replay.top, replay.right, replay.bottom);
    render_target_->FillRoundedRectangle(D2D1::RoundedRect(replay_rect, 8.0f, 8.0f),
        IsPointInside(replay_rect, mouse_x_, mouse_y_) ? brush_active_ : brush_popup_bg_);
    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(replay_rect, 8.0f, 8.0f), brush_chart_grid_, 1.0f);
    render_target_->DrawText(L"Quick tour", 10, format_limit_button_, replay_rect, brush_text_primary_);
    D2D1_RECT_F close_box = D2D1::RectF(mx + mw - 40.0f, my + 20.0f, mx + mw - 15.0f, my + 45.0f);
    if (hover_settings_close_) {
        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx + mw - 27.5f, my + 32.5f), 15.0f, 15.0f), brush_hover_);
    }
    DrawVectorIcon(render_target_, VectorIcon::Close, close_box, brush_text_primary_, 1.5f);
    render_target_->DrawLine(
        D2D1::Point2F(mx + 1.0f, header_bottom + 0.5f),
        D2D1::Point2F(mx + mw - 1.0f, header_bottom + 0.5f),
        brush_chart_grid_,
        0.5f
    );

    const float nav_left = mx + 12.0f;
    const float nav_right = mx + layout.navigation_width - 8.0f;
    const float nav_top = my + 84.0f;
    const float content_left = mx + layout.navigation_width + 26.0f;
    const float content_right = mx + mw - 18.0f;
    const float content_width = content_right - content_left;
    // Subtle sidebar divider
    render_target_->DrawLine(
        D2D1::Point2F(mx + layout.navigation_width + 0.5f, header_bottom),
        D2D1::Point2F(mx + layout.navigation_width + 0.5f, my + mh - 18.0f),
        brush_chart_grid_,
        0.5f
    );

    // The settings references use a small, consistent line-icon family rather
    // than unrelated font glyphs. Draw the important shapes directly so they
    // remain visually stable when a machine falls back from Fluent Icons.
    enum SettingsSymbol {
        SettingsGear,
        SettingsGrid,
        SettingsWarning,
        SettingsPower,
        SettingsBits,
        SettingsEyeOff,
        SettingsMonitor,
        SettingsChip,
        SettingsMemory,
        SettingsPin,
        SettingsLock,
        SettingsMouse,
        SettingsTraffic,
        SettingsThermometer,
        SettingsGpu,
        SettingsDisk,
        SettingsBoard,
        SettingsInfo,
    };
    const auto draw_settings_symbol = [&](SettingsSymbol symbol, const D2D1_RECT_F& bounds, ID2D1Brush* brush) {
        const float w = bounds.right - bounds.left;
        const float h = bounds.bottom - bounds.top;
        const float cx = (bounds.left + bounds.right) * 0.5f;
        const float cy = (bounds.top + bounds.bottom) * 0.5f;
        const float unit = std::min(w, h);
        const auto p = [&](float x, float y) { return D2D1::Point2F(cx + x * unit, cy + y * unit); };
        const auto r = [&](float left, float top, float right, float bottom) {
            return D2D1::RectF(cx + left * unit, cy + top * unit, cx + right * unit, cy + bottom * unit);
        };
        const float stroke = unit >= 34.0f ? 1.6f : 1.25f;
        switch (symbol) {
        case SettingsGear:
            DrawVectorIcon(render_target_, VectorIcon::Gear, bounds, brush, stroke);
            break;
        case SettingsGrid:
            for (int row = 0; row < 2; ++row) {
                for (int col = 0; col < 2; ++col) {
                    const float left = -0.33f + static_cast<float>(col) * 0.38f;
                    const float top = -0.33f + static_cast<float>(row) * 0.38f;
                    render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(left, top, left + 0.24f, top + 0.24f), unit * 0.035f, unit * 0.035f), brush, stroke);
                }
            }
            break;
        case SettingsWarning:
            render_target_->DrawLine(p(0.0f, -0.37f), p(-0.36f, 0.31f), brush, stroke);
            render_target_->DrawLine(p(-0.36f, 0.31f), p(0.36f, 0.31f), brush, stroke);
            render_target_->DrawLine(p(0.36f, 0.31f), p(0.0f, -0.37f), brush, stroke);
            render_target_->DrawLine(p(0.0f, -0.13f), p(0.0f, 0.10f), brush, stroke);
            render_target_->FillEllipse(D2D1::Ellipse(p(0.0f, 0.20f), unit * 0.035f, unit * 0.035f), brush);
            break;

        case SettingsPower: {
            render_target_->DrawLine(p(0.0f, -0.40f), p(0.0f, 0.02f), brush, stroke + 0.35f);
            constexpr float pi = 3.14159265f;
            D2D1_POINT_2F previous = p(-0.226f, -0.226f);
            for (int index = 1; index <= 18; ++index) {
                const float angle = (225.0f - 270.0f * static_cast<float>(index) / 18.0f) * pi / 180.0f;
                const D2D1_POINT_2F next = p(std::cos(angle) * 0.32f, std::sin(angle) * 0.32f);
                render_target_->DrawLine(previous, next, brush, stroke);
                previous = next;
            }
            break;
        }
        case SettingsBits:
            render_target_->DrawText(L"1010", 4, format_status_, r(-0.36f, -0.31f, 0.36f, -0.02f), brush);
            render_target_->DrawText(L"0101", 4, format_status_, r(-0.36f, 0.05f, 0.36f, 0.34f), brush);
            break;
        case SettingsEyeOff:
            render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), unit * 0.34f, unit * 0.20f), brush, stroke);
            render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), unit * 0.07f, unit * 0.07f), brush);
            render_target_->DrawLine(p(-0.39f, -0.39f), p(0.39f, 0.39f), brush, stroke + 0.35f);
            break;
        case SettingsMonitor:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.36f, -0.31f, 0.36f, 0.18f), unit * 0.06f, unit * 0.06f), brush, stroke);
            render_target_->DrawLine(p(0.0f, 0.18f), p(0.0f, 0.33f), brush, stroke);
            render_target_->DrawLine(p(-0.18f, 0.33f), p(0.18f, 0.33f), brush, stroke);
            break;
        case SettingsChip:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.24f, -0.24f, 0.24f, 0.24f), unit * 0.04f, unit * 0.04f), brush, stroke);
            render_target_->DrawRectangle(r(-0.11f, -0.11f, 0.11f, 0.11f), brush, stroke);
            for (float offset : { -0.18f, 0.0f, 0.18f }) {
                render_target_->DrawLine(p(offset, -0.37f), p(offset, -0.24f), brush, stroke);
                render_target_->DrawLine(p(offset, 0.24f), p(offset, 0.37f), brush, stroke);
                render_target_->DrawLine(p(-0.37f, offset), p(-0.24f, offset), brush, stroke);
                render_target_->DrawLine(p(0.24f, offset), p(0.37f, offset), brush, stroke);
            }
            break;
        case SettingsMemory:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.37f, -0.23f, 0.37f, 0.23f), unit * 0.04f, unit * 0.04f), brush, stroke);
            render_target_->DrawRectangle(r(-0.21f, -0.08f, -0.05f, 0.08f), brush, stroke);
            render_target_->DrawRectangle(r(0.05f, -0.08f, 0.21f, 0.08f), brush, stroke);
            break;
        case SettingsPin:
            // A centered thumbtack reads clearly at the compact 18px size.
            // The former diagonal mark looked more like a stray slash than
            // an "always on top" control.
            render_target_->DrawRoundedRectangle(
                D2D1::RoundedRect(r(-0.24f, -0.35f, 0.24f, -0.20f), unit * 0.05f, unit * 0.05f),
                brush,
                stroke
            );
            render_target_->DrawLine(p(-0.15f, -0.20f), p(-0.25f, 0.08f), brush, stroke);
            render_target_->DrawLine(p(0.15f, -0.20f), p(0.25f, 0.08f), brush, stroke);
            render_target_->DrawLine(p(-0.25f, 0.08f), p(0.25f, 0.08f), brush, stroke);
            render_target_->DrawLine(p(0.0f, 0.08f), p(0.0f, 0.38f), brush, stroke);
            break;
        case SettingsLock:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.28f, -0.02f, 0.28f, 0.32f), unit * 0.06f, unit * 0.06f), brush, stroke);
            render_target_->DrawLine(p(-0.19f, -0.02f), p(-0.19f, -0.18f), brush, stroke);
            render_target_->DrawLine(p(-0.19f, -0.18f), p(0.0f, -0.31f), brush, stroke);
            render_target_->DrawLine(p(0.0f, -0.31f), p(0.19f, -0.18f), brush, stroke);
            render_target_->DrawLine(p(0.19f, -0.18f), p(0.19f, -0.02f), brush, stroke);
            break;
        case SettingsMouse:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.23f, -0.36f, 0.23f, 0.36f), unit * 0.18f, unit * 0.18f), brush, stroke);
            render_target_->DrawLine(p(0.0f, -0.31f), p(0.0f, -0.06f), brush, stroke);
            break;
        case SettingsTraffic:
            render_target_->DrawLine(p(-0.37f, 0.25f), p(-0.08f, -0.04f), brush, stroke);
            render_target_->DrawLine(p(-0.08f, -0.04f), p(0.10f, 0.12f), brush, stroke);
            render_target_->DrawLine(p(0.10f, 0.12f), p(0.37f, -0.32f), brush, stroke);
            render_target_->DrawLine(p(0.16f, -0.32f), p(0.37f, -0.32f), brush, stroke);
            render_target_->DrawLine(p(0.37f, -0.32f), p(0.37f, -0.11f), brush, stroke);
            break;
        case SettingsThermometer:
            render_target_->DrawLine(p(0.0f, -0.34f), p(0.0f, 0.17f), brush, stroke + 0.25f);
            render_target_->DrawEllipse(D2D1::Ellipse(p(0.0f, 0.25f), unit * 0.16f, unit * 0.16f), brush, stroke);
            break;
        case SettingsGpu:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.37f, -0.25f, 0.37f, 0.25f), unit * 0.04f, unit * 0.04f), brush, stroke);
            render_target_->DrawEllipse(D2D1::Ellipse(p(-0.10f, 0.0f), unit * 0.12f, unit * 0.12f), brush, stroke);
            render_target_->DrawLine(p(0.18f, -0.11f), p(0.18f, 0.11f), brush, stroke);
            break;
        case SettingsDisk:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.32f, -0.34f, 0.32f, 0.34f), unit * 0.05f, unit * 0.05f), brush, stroke);
            render_target_->DrawEllipse(D2D1::Ellipse(p(0.0f, -0.07f), unit * 0.13f, unit * 0.13f), brush, stroke);
            render_target_->DrawLine(p(-0.18f, 0.20f), p(0.18f, 0.20f), brush, stroke);
            break;
        case SettingsBoard:
            render_target_->DrawRoundedRectangle(D2D1::RoundedRect(r(-0.32f, -0.32f, 0.32f, 0.32f), unit * 0.04f, unit * 0.04f), brush, stroke);
            render_target_->DrawRectangle(r(-0.18f, -0.18f, -0.03f, -0.03f), brush, stroke);
            render_target_->DrawRectangle(r(0.04f, -0.18f, 0.19f, -0.03f), brush, stroke);
            render_target_->DrawRectangle(r(-0.18f, 0.04f, -0.03f, 0.19f), brush, stroke);
            render_target_->DrawRectangle(r(0.04f, 0.04f, 0.19f, 0.19f), brush, stroke);
            break;
        case SettingsInfo:
            render_target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), unit * 0.32f, unit * 0.32f), brush, stroke);
            render_target_->DrawText(L"i", 1, format_body_bold_, r(-0.12f, -0.25f, 0.12f, 0.29f), brush);
            break;
        }
    };

    const wchar_t* navigation_labels[] = { L"General", L"Widgets", L"Warnings", L"Floating" };
    const SettingsSymbol navigation_icons[] = { SettingsGear, SettingsGrid, SettingsWarning, SettingsMonitor };
    ID2D1SolidColorBrush* selected_nav_text = nullptr;
    const D2D1_COLOR_F selected_nav_color = effective_theme_ == ThemeMode::Light
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
        : D2D1::ColorF(0.0f, 0.0f, 0.0f);
    render_target_->CreateSolidColorBrush(selected_nav_color, &selected_nav_text);
    for (int section = 0; section < 4; ++section) {
        const float row_top = nav_top + static_cast<float>(section) * 52.0f;
        const D2D1_RECT_F row = D2D1::RectF(nav_left, row_top, nav_right, row_top + 44.0f);
        const bool selected = settings_section_ == section;
        if (selected) {
            render_target_->FillRoundedRectangle(D2D1::RoundedRect(row, 10.0f, 10.0f), brush_text_primary_);
        } else if (mouse_x_ >= row.left && mouse_x_ <= row.right && mouse_y_ >= row.top && mouse_y_ <= row.bottom) {
            render_target_->FillRoundedRectangle(D2D1::RoundedRect(row, 10.0f, 10.0f), brush_hover_);
        }
        draw_settings_symbol(
            navigation_icons[section],
            D2D1::RectF(row.left + 10.0f, row.top + 10.0f, row.left + 30.0f, row.top + 30.0f),
            selected ? selected_nav_text : brush_text_secondary_
        );
        render_target_->DrawText(
            navigation_labels[section],
            static_cast<UINT32>(wcslen(navigation_labels[section])),
            format_sidebar_,
            D2D1::RectF(row.left + 39.0f, row.top, row.right - 7.0f, row.bottom),
            selected ? selected_nav_text : brush_text_secondary_
        );
    }
    selected_nav_text->Release();

    const auto draw_page_title = [&](const wchar_t* title, const wchar_t* subtitle) {
        render_target_->DrawText(
            title, static_cast<UINT32>(wcslen(title)), format_dashboard_value_,
            D2D1::RectF(content_left, my + 84.0f, content_right, my + 114.0f), brush_text_primary_
        );
        render_target_->DrawText(
            subtitle, static_cast<UINT32>(wcslen(subtitle)), format_dashboard_note_,
            D2D1::RectF(content_left, my + 118.0f, content_right, my + 140.0f), brush_text_secondary_
        );
    };
    const auto draw_section_label = [&](const wchar_t* label, float top) {
        render_target_->DrawText(
            label, static_cast<UINT32>(wcslen(label)), format_body_bold_,
            D2D1::RectF(content_left + 20.0f, top, content_right - 20.0f, top + 20.0f), brush_text_primary_
        );
    };
    const auto draw_toggle = [&](float top, const wchar_t* title, const wchar_t* detail, bool enabled) {
        const D2D1_RECT_F track = D2D1::RectF(content_right - 32.0f, top + 5.0f, content_right, top + 21.0f);
        const D2D1_ROUNDED_RECT track_round = D2D1::RoundedRect(track, 8.0f, 8.0f);
        render_target_->FillRoundedRectangle(track_round, enabled ? brush_accent_dl_ : brush_card_border_);
        const float thumb_x = enabled ? track.right - 8.0f : track.left + 8.0f;
        render_target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(thumb_x, top + 13.0f), 5.0f, 5.0f),
            brush_background_
        );
        render_target_->DrawText(
            title, static_cast<UINT32>(wcslen(title)), format_body_bold_,
            D2D1::RectF(content_left, top - 1.0f, track.left - 10.0f, top + 18.0f), brush_text_primary_
        );
        render_target_->DrawText(
            detail, static_cast<UINT32>(wcslen(detail)), format_status_,
            D2D1::RectF(content_left, top + 19.0f, content_right, top + 36.0f), brush_text_secondary_
        );
    };
    const auto draw_adapter_row = [&](float top) {
        const D2D1_RECT_F row = D2D1::RectF(content_left + 20.0f, top, content_right - 20.0f, top + 38.0f);
        const D2D1_ROUNDED_RECT round = D2D1::RoundedRect(row, 10.0f, 10.0f);
        render_target_->FillRoundedRectangle(round, brush_popup_bg_);
        render_target_->DrawRoundedRectangle(round, brush_chart_grid_, 1.0f);
        const std::wstring selection = GetNetworkAdapterSelectionLabel();
        // Vertically centre text and chevron inside the 38px dropdown
        render_target_->DrawText(
            selection.c_str(), static_cast<UINT32>(selection.size()), format_body_,
            D2D1::RectF(row.left + 12.0f, row.top + 9.0f, row.right - 30.0f, row.bottom), brush_text_primary_
        );
        DrawVectorIcon(
            render_target_,
            VectorIcon::ChevronDown,
            D2D1::RectF(row.right - 22.0f, row.top + 10.0f, row.right - 6.0f, row.bottom - 8.0f),
            brush_text_primary_,
            1.2f
        );
    };

    // Settings are deliberately drawn from a draft. The running monitor does
    // not change until the user commits the complete set with Save.
    {
        const PreferencesDraft& draft = preferences_draft_;
        const float footer_top = my + mh - 58.0f;
        // ── Layout grid constants ──────────────────────────────────────
        // Every card, label, control, and divider snaps to this grid so
        // the interface feels balanced at any window size.
        const float kPad = 20.0f;          // Uniform card internal padding
        const float kCardRadius = 16.0f;   // Card corner radius
        const float kCtrlRadius = 10.0f;   // Input/dropdown corner radius
        const float kCardGap = 12.0f;      // Vertical gap between cards
        const float kToggleW = 40.0f;      // Toggle track width
        const float kToggleH = 22.0f;      // Toggle track height
        const float kThumbR = 7.0f;        // Toggle thumb radius
        const float kIconGap = 14.0f;      // Gap between icon and text column
        const float kToggleGap = 14.0f;    // Gap between text column and toggle

        const auto draw_group_card = [&](const D2D1_RECT_F& rect) {
            const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, kCardRadius, kCardRadius);
            render_target_->FillRoundedRectangle(rounded, brush_popup_bg_);
            render_target_->DrawRoundedRectangle(rounded, brush_chart_grid_, 1.0f);
        };
        const auto draw_group_divider = [&](float y) {
            render_target_->DrawLine(
                D2D1::Point2F(content_left + kPad, y),
                D2D1::Point2F(content_right - kPad, y),
                brush_picker_month_border_,
                1.0f
            );
        };
        const auto draw_compact_toggle = [&](float top, const wchar_t* title, const wchar_t* detail, bool enabled,
                                             int symbol, bool large_icon) {
            // Three-column layout: [icon] [title+detail] [toggle]
            // All measurements anchored to kPad from card edges.
            const float icon_size = symbol >= 0 ? (large_icon ? 34.0f : 24.0f) : 0.0f;
            const float icon_left = content_left + kPad;
            const float text_left = icon_size > 0.0f ? icon_left + icon_size + kIconGap : content_left + kPad;
            // Toggle – fixed X, never shifts regardless of text length
            const float toggle_right = content_right - kPad;
            const float toggle_left = toggle_right - kToggleW;
            const float toggle_cy = top + 16.0f; // vertical center (+2px down)
            const float text_right = toggle_left - kToggleGap;
            // Icon (vertically centred in row)
            if (icon_size > 0.0f) {
                const D2D1_RECT_F icon_rect = D2D1::RectF(icon_left, top - 1.0f, icon_left + icon_size, top - 1.0f + icon_size);
                const D2D1_ROUNDED_RECT icon_round = D2D1::RoundedRect(icon_rect, 8.0f, 8.0f);
                render_target_->FillRoundedRectangle(icon_round, brush_popup_bg_);
                render_target_->DrawRoundedRectangle(icon_round, brush_chart_grid_, 1.0f);
                // Draw the symbol directly in the icon_rect (glyph handles its own size)
                draw_settings_symbol(static_cast<SettingsSymbol>(symbol), icon_rect, brush_text_primary_);
            }
            // Toggle switch – fixed position
            const D2D1_RECT_F track = D2D1::RectF(toggle_left, toggle_cy - kToggleH * 0.5f, toggle_right, toggle_cy + kToggleH * 0.5f);
            if (enabled) {
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(track, 11.0f, 11.0f), brush_accent_dl_);
            } else {
                render_target_->DrawRoundedRectangle(D2D1::RoundedRect(track, 11.0f, 11.0f), brush_card_border_, 1.5f);
            }
            render_target_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(enabled ? track.right - 11.0f : track.left + 11.0f, toggle_cy), kThumbR, kThumbR),
                enabled ? brush_background_ : brush_card_border_
            );
            // Title + description (+2px down)
            render_target_->DrawText(title, static_cast<UINT32>(wcslen(title)), format_body_bold_,
                D2D1::RectF(text_left, top + 1.0f, text_right, top + 20.0f), brush_text_primary_);
            if (detail && detail[0]) {
                render_target_->DrawText(detail, static_cast<UINT32>(wcslen(detail)), format_dashboard_note_,
                    D2D1::RectF(text_left, top + 20.0f, text_right, top + 38.0f), brush_text_secondary_);
            }
        };
        const auto draw_value_box = [&](const D2D1_RECT_F& rect, const std::wstring& value,
                                        SettingsNumericField field = SettingsNumericField::None) {
            const bool editing = field != SettingsNumericField::None && IsSettingsNumericInputActive(field);
            const D2D1_ROUNDED_RECT round = D2D1::RoundedRect(rect, kCtrlRadius, kCtrlRadius);
            render_target_->FillRoundedRectangle(round, brush_popup_bg_);
            render_target_->DrawRoundedRectangle(round, editing ? brush_accent_dl_ : brush_chart_grid_, editing ? 1.5f : 1.0f);

            const std::wstring& display_value = editing ? settings_numeric_text_ : value;
            const float text_left = rect.left + 12.0f;
            render_target_->DrawText(
                display_value.c_str(), static_cast<UINT32>(display_value.size()), format_body_,
                D2D1::RectF(text_left, rect.top + 7.0f, rect.right - 8.0f, rect.bottom), brush_text_primary_
            );

            if (editing) {
                float caret_x = text_left;
                IDWriteTextLayout* text_layout = nullptr;
                if (dwrite_factory_ && SUCCEEDED(dwrite_factory_->CreateTextLayout(
                        display_value.c_str(), static_cast<UINT32>(display_value.size()), format_body_,
                        std::max(1.0f, rect.right - text_left - 8.0f), std::max(1.0f, rect.bottom - rect.top),
                        &text_layout))) {
                    DWRITE_TEXT_METRICS metrics{};
                    if (SUCCEEDED(text_layout->GetMetrics(&metrics))) {
                        caret_x += metrics.widthIncludingTrailingWhitespace;
                    }
                    text_layout->Release();
                }
                caret_x = std::min(caret_x, rect.right - 8.0f);
                render_target_->DrawLine(
                    D2D1::Point2F(caret_x, rect.top + 7.0f),
                    D2D1::Point2F(caret_x, rect.bottom - 7.0f),
                    brush_text_primary_,
                    1.0f
                );
            }
        };
        const auto draw_button = [&](const D2D1_RECT_F& rect, const wchar_t* label, bool enabled, bool primary) {
            const D2D1_ROUNDED_RECT round = D2D1::RoundedRect(rect, 8.0f, 8.0f);
            if (enabled && primary) {
                render_target_->FillRoundedRectangle(round, brush_text_primary_);
            } else {
                render_target_->FillRoundedRectangle(round, brush_popup_bg_);
                render_target_->DrawRoundedRectangle(round, brush_chart_grid_, 1.0f);
            }
            render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_limit_button_, rect,
                enabled ? (primary ? brush_active_text_ : brush_text_primary_) : brush_text_secondary_);
        };
        if (settings_section_ == 0) {
            draw_page_title(L"General", L"Startup, appearance and monitoring.");

            // Scroll only the cards; keep the page heading and Save controls fixed.
            settings_general_scroll_offset_ = std::clamp(settings_general_scroll_offset_, 0.0f, settings_general_scroll_max_);
            render_target_->PushAxisAlignedClip(
                D2D1::RectF(content_left - 2.0f, my + 146.0f, content_right + 2.0f, footer_top - 14.0f),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            // ── Startup card ──────────────────────────────────────────
            const float c0_top = my - settings_general_scroll_offset_ + 148.0f;
            const float c0_label = c0_top + 14.0f;
            const float c0_r1 = c0_label + 24.0f;
            const float c0_d1 = c0_r1 + 50.0f;
            const float c0_r2 = c0_d1 + 4.0f;
            const float c0_d2 = c0_r2 + 50.0f;
            const float c0_r3 = c0_d2 + 4.0f;
            const float c0_bot = c0_r3 + 50.0f + 10.0f;
            draw_group_card(D2D1::RectF(content_left, c0_top, content_right, c0_bot));
            draw_section_label(L"Startup", c0_label);
            draw_compact_toggle(c0_r1, L"Run on Windows startup", L"Start quietly in the tray\nafter you sign in.",
                draft.autostart_enabled, SettingsPower, true);
            draw_group_divider(c0_d1);
            draw_compact_toggle(c0_r2, L"Use bits/s instead of bytes/s", L"Applies to the app and both widgets.",
                draft.use_bits_per_second, SettingsBits, true);
            draw_group_divider(c0_d2);
            draw_compact_toggle(c0_r3, L"Hide hardware gauges", L"Hide CPU, memory and GPU cards.",
                draft.hide_hardware_gauges, SettingsEyeOff, true);

            // ── Monthly Data Limit card ───────────────────────────────
            const float c1_top = c0_bot + kCardGap;
            const float c1_label = c1_top + 14.0f;
            const float c1_input_top = c1_label + 22.0f;
            const float c1_input_bot = c1_input_top + 32.0f;
            const float c1_bot = c1_input_bot + 12.0f;
            draw_group_card(D2D1::RectF(content_left, c1_top, content_right, c1_bot));
            draw_section_label(L"Monthly Data Limit", c1_label);
            const D2D1_RECT_F limit_rect = D2D1::RectF(content_left + kPad, c1_input_top, content_right - kPad, c1_input_bot);
            const uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
            const std::wstring limit_value = draft.monthly_data_limit_bytes == 0
                ? L"0 = no limit"
                : std::to_wstring(draft.monthly_data_limit_bytes / gibibyte) + L" GB";
            draw_value_box(limit_rect, limit_value, SettingsNumericField::MonthlyDataLimit);

            // ── Theme Mode card ───────────────────────────────────────
            const float c2_top = c1_bot + kCardGap;
            const auto theme_picker = ThemePickerLayout::Get(content_left, content_right, c2_top);
            const float c2_bot = theme_picker.bottom;
            draw_group_card(D2D1::RectF(content_left, c2_top, content_right, c2_bot));
            draw_section_label(L"Theme Mode", c2_top + 14.0f);
            static_assert(ThemePickerLayout::option_count == UIStyle::theme_count, "Theme choices and persisted IDs must match");
            for (int i = 0; i < UIStyle::theme_count; ++i) {
                const auto& option = theme_picker.options[static_cast<size_t>(i)];
                const auto& bounds = option.bounds;
                const auto& text = option.text;
                const D2D1_RECT_F choice = D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.bottom);
                const bool selected = static_cast<int>(draft.theme) == i;
                const bool hovered = IsPointInside(choice, mouse_x_, mouse_y_);
                if (selected || hovered) render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(choice, 9.0f, 9.0f), selected ? brush_active_ : brush_hover_);
                render_target_->DrawRoundedRectangle(D2D1::RoundedRect(choice, 9.0f, 9.0f),
                    selected ? brush_accent_dl_ : brush_chart_grid_, 1.0f);
                const auto center = D2D1::Point2F(option.radio_x, option.radio_y);
                render_target_->DrawEllipse(D2D1::Ellipse(center, 6.0f, 6.0f),
                    selected ? brush_accent_dl_ : brush_text_secondary_, 1.25f);
                if (selected) render_target_->FillEllipse(D2D1::Ellipse(center, 3.0f, 3.0f), brush_accent_dl_);
                const wchar_t* label = ThemePickerLayout::labels[i];
                render_target_->DrawText(label, static_cast<UINT32>(wcslen(label)), format_theme_choice_,
                    D2D1::RectF(text.left, text.top, text.right, text.bottom), brush_text_primary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            const auto& theme_note = theme_picker.note;
            render_target_->DrawText(ThemePickerLayout::auto_note, static_cast<UINT32>(wcslen(ThemePickerLayout::auto_note)),
                format_dashboard_note_, D2D1::RectF(theme_note.left, theme_note.top, theme_note.right, theme_note.bottom),
                brush_text_secondary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);

            // ── Monitoring Adapter card ───────────────────────────────
            const float c3_top = c2_bot + kCardGap;
            const float c3_label = c3_top + 14.0f;
            const float c3_dropdown = c3_label + 24.0f;
            const float c3_helper = c3_dropdown + 42.0f;
            const float c3_bot = c3_helper + 18.0f + 8.0f;
            draw_group_card(D2D1::RectF(content_left, c3_top, content_right, c3_bot));
            draw_section_label(L"Monitoring Adapter", c3_label);
            draw_adapter_row(c3_dropdown);
            render_target_->DrawText(L"Auto follows the Windows default connection.",
                static_cast<UINT32>(wcslen(L"Auto follows the Windows default connection.")), format_dashboard_note_,
                D2D1::RectF(content_left + kPad, c3_helper, content_right - kPad, c3_helper + 18.0f), brush_text_secondary_);
            render_target_->PopAxisAlignedClip();
            settings_general_scroll_max_ = std::max(0.0f,
                c3_bot + settings_general_scroll_offset_ - (footer_top - 14.0f));
            if (settings_general_scroll_max_ > 0.0f) {
                const float track_top = my + 150.0f, track_bottom = footer_top - 18.0f;
                const float track_height = std::max(1.0f, track_bottom - track_top);
                const float thumb_height = std::max(24.0f, track_height * track_height / (track_height + settings_general_scroll_max_));
                const float thumb_top = track_top + (track_height - thumb_height) * settings_general_scroll_offset_ / settings_general_scroll_max_;
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(mx + mw - 10.0f, thumb_top, mx + mw - 7.0f, thumb_top + thumb_height), 1.5f, 1.5f), brush_text_secondary_);
            }
        } else if (settings_section_ == 1) {
            // The page scrolls inside the fixed dialog body. Sidebar, modal
            // header, and footer remain still so the dialog never feels like
            // one cramped scrolling sheet.
            const float widgets_content_left = mx + layout.navigation_width + 20.0f;
            const float widgets_content_right = mx + mw - 18.0f;
            const float widgets_view_top = my + 76.0f;
            const float widgets_view_bottom = footer_top - 14.0f;
            const WidgetsSettingsLayout unscrolled = GetWidgetsSettingsLayout(
                widgets_content_left, widgets_content_right, my
            );
            settings_widgets_scroll_max_ = std::max(0.0f, unscrolled.fl_bot - widgets_view_bottom);
            settings_widgets_scroll_offset_ = std::clamp(
                settings_widgets_scroll_offset_, 0.0f, settings_widgets_scroll_max_
            );
            const WidgetsSettingsLayout w = GetWidgetsSettingsLayout(
                widgets_content_left,
                widgets_content_right,
                my - settings_widgets_scroll_offset_
            );

            // Keep helper text secondary, but visible enough to scan in the
            // compact dialog without squinting.
            ID2D1SolidColorBrush* widgets_desc = nullptr;
            ID2D1SolidColorBrush* widgets_div = nullptr;
            {
                D2D1_COLOR_F desc = palette_.TextSecondary;
                desc.a = (effective_theme_ == ThemeMode::Light) ? 0.90f : 0.82f;
                render_target_->CreateSolidColorBrush(desc, &widgets_desc);
                D2D1_COLOR_F div = (effective_theme_ == ThemeMode::Light)
                    ? D2D1::ColorF(0.06f, 0.05f, 0.06f, 0.10f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
                render_target_->CreateSolidColorBrush(div, &widgets_div);
            }

            const auto draw_widgets_card = [&](float top, float bot) {
                draw_group_card(D2D1::RectF(w.card_left, top, w.card_right, bot));
            };
            const auto draw_widgets_title = [&](const wchar_t* label, float top) {
                render_target_->DrawText(
                    label, static_cast<UINT32>(wcslen(label)), format_body_bold_,
                    D2D1::RectF(w.card_left + w.pad, top, w.card_right - w.pad, top + w.title_h),
                    brush_text_primary_
                );
            };
            const auto draw_widgets_divider = [&](float y) {
                render_target_->DrawLine(
                    D2D1::Point2F(w.card_left + w.pad, y),
                    D2D1::Point2F(w.card_right - w.pad, y),
                    widgets_div,
                    1.0f
                );
            };
            // Strict three-column row: [icon box] [title + detail] [toggle]
            const auto draw_widgets_row = [&](float top, float row_height, float icon_size, float icon_glyph,
                                              const wchar_t* title, const wchar_t* detail, bool enabled, int symbol,
                                              bool compact) {
                const float icon_left = w.card_left + w.pad;
                const float icon_top = top + (row_height - icon_size) * 0.5f;
                const float text_left = icon_left + icon_size + w.icon_gap;
                const float toggle_right = w.card_right - w.toggle_inset_right;
                const float toggle_left = toggle_right - w.toggle_w;
                const float toggle_cy = top + row_height * 0.5f;
                const float text_right = toggle_left - 12.0f;

                // Icon container (32px) with glyph inset (~20px) so it feels light.
                const D2D1_RECT_F icon_box = D2D1::RectF(icon_left, icon_top, icon_left + icon_size, icon_top + icon_size);
                const D2D1_ROUNDED_RECT icon_round = D2D1::RoundedRect(icon_box, 8.0f, 8.0f);
                render_target_->FillRoundedRectangle(icon_round, brush_popup_bg_);
                render_target_->DrawRoundedRectangle(icon_round, brush_chart_grid_, 1.0f);
                const float glyph_inset = (icon_size - icon_glyph) * 0.5f;
                const D2D1_RECT_F glyph_rect = D2D1::RectF(
                    icon_box.left + glyph_inset, icon_box.top + glyph_inset,
                    icon_box.right - glyph_inset, icon_box.bottom - glyph_inset
                );
                draw_settings_symbol(static_cast<SettingsSymbol>(symbol), glyph_rect, brush_text_primary_);

                // Toggle — fixed right edge, never crowded against the card border.
                const D2D1_RECT_F track = D2D1::RectF(
                    toggle_left, toggle_cy - w.toggle_h * 0.5f,
                    toggle_right, toggle_cy + w.toggle_h * 0.5f
                );
                if (enabled) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(track, 10.0f, 10.0f), brush_accent_dl_);
                } else {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(track, 10.0f, 10.0f), brush_card_border_);
                }
                render_target_->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(
                            enabled ? track.right - w.toggle_h * 0.5f : track.left + w.toggle_h * 0.5f,
                            toggle_cy
                        ),
                        w.thumb_r, w.thumb_r
                    ),
                    brush_background_
                );

                // The dense floating-monitor rows use the app's smaller text
                // formats, while the primary taskbar rows keep a stronger
                // title. Both retain the same icon/text/control columns.
                const float title_top = compact ? top + 3.0f : top + 5.0f;
                const float title_bottom = compact ? top + 20.0f : top + 23.0f;
                const float detail_top = compact ? top + 20.0f : top + 25.0f;
                const float detail_bottom = compact ? top + 38.0f : top + 43.0f;
                render_target_->DrawText(
                    title, static_cast<UINT32>(wcslen(title)), format_body_bold_,
                    D2D1::RectF(text_left, title_top, text_right, title_bottom),
                    brush_text_primary_
                );
                if (detail && detail[0]) {
                    render_target_->DrawText(
                        detail, static_cast<UINT32>(wcslen(detail)), format_dashboard_note_,
                        D2D1::RectF(text_left, detail_top, text_right, detail_bottom),
                        widgets_desc
                    );
                }
            };

            render_target_->PushAxisAlignedClip(
                D2D1::RectF(widgets_content_left - 2.0f, widgets_view_top, mx + mw - 6.0f, widgets_view_bottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
            );
            render_target_->DrawText(
                L"Widgets", 7, format_dashboard_value_,
                D2D1::RectF(widgets_content_left, my - settings_widgets_scroll_offset_ + 84.0f,
                    widgets_content_right, my - settings_widgets_scroll_offset_ + 114.0f),
                brush_text_primary_
            );
            render_target_->DrawText(
                L"Configure taskbar and floating monitoring surfaces.",
                static_cast<UINT32>(wcslen(L"Configure taskbar and floating monitoring surfaces.")),
                format_dashboard_note_,
                D2D1::RectF(widgets_content_left, my - settings_widgets_scroll_offset_ + 118.0f,
                    widgets_content_right, my - settings_widgets_scroll_offset_ + 140.0f),
                brush_text_secondary_
            );

            // ── Taskbar Widget ────────────────────────────────────────
            draw_widgets_card(w.tb_top, w.tb_bot);
            draw_widgets_title(L"Taskbar Widget", w.tb_title);
            draw_widgets_row(w.tb_r1, w.row_h, w.icon_size, w.icon_glyph,
                L"Enable taskbar widget", L"Show live monitoring in the taskbar.", draft.widget_enabled, SettingsMonitor, false);
            draw_widgets_divider(w.tb_d1);
            draw_widgets_title(L"Taskbar content", w.tb_content_title);
            const wchar_t* mode_labels[] = { L"Upload & download", L"CPU & memory", L"All four" };
            const TaskbarWidgetMode modes[] = {
                TaskbarWidgetMode::Network, TaskbarWidgetMode::System, TaskbarWidgetMode::All
            };
            for (int i = 0; i < 3; ++i) {
                const float top = w.tb_mode_row[i];
                const auto center = D2D1::Point2F(w.card_left + w.pad + 9.0f, top + w.tb_mode_row_h * 0.5f);
                const bool selected = draft.widget_mode == modes[i];
                render_target_->DrawEllipse(D2D1::Ellipse(center, 7.0f, 7.0f),
                    selected ? brush_accent_dl_ : brush_text_secondary_, 1.5f);
                if (selected) {
                    render_target_->FillEllipse(D2D1::Ellipse(center, 3.5f, 3.5f), brush_accent_dl_);
                }
                render_target_->DrawText(mode_labels[i], static_cast<UINT32>(wcslen(mode_labels[i])), format_body_,
                    D2D1::RectF(w.card_left + w.pad + 28.0f, top + 7.0f, w.card_right - w.pad, top + w.tb_mode_row_h),
                    brush_text_primary_);
            }


            // ── Floating Mini Monitor ─────────────────────────────────
            draw_widgets_card(w.fl_top, w.fl_bot);
            draw_widgets_title(L"Floating Mini Monitor", w.fl_title);
            draw_widgets_row(w.fl_row[0], w.fl_row_h, w.fl_icon_size, w.fl_icon_glyph,
                L"Enable floating mini monitor", L"Upload and download only.", draft.floating_monitor_enabled, SettingsMonitor, true);
            draw_widgets_divider(w.fl_div[0]);
            draw_widgets_row(w.fl_row[1], w.fl_row_h, w.fl_icon_size, w.fl_icon_glyph,
                L"Always on top", L"Keep monitor above other windows.", draft.floating_monitor_always_on_top, SettingsPin, true);
            draw_widgets_divider(w.fl_div[1]);
            draw_widgets_row(w.fl_row[2], w.fl_row_h, w.fl_icon_size, w.fl_icon_glyph,
                L"Click-through mode", L"Recover using the tray options.", draft.floating_monitor_click_through, SettingsMouse, true);
            draw_button(D2D1::RectF(w.card_left + w.pad, w.fl_details_top, w.card_right - w.pad, w.fl_details_bot),
                L"Appearance & position...", true, false);

            render_target_->PopAxisAlignedClip();

            if (settings_widgets_scroll_max_ > 1.0f && widgets_desc && widgets_div) {
                const float track_left = mx + mw - 10.0f;
                const float track_right = mx + mw - 7.0f;
                const float track_top = widgets_view_top + 5.0f;
                const float track_bottom = widgets_view_bottom - 5.0f;
                const float track_height = std::max(1.0f, track_bottom - track_top);
                const float content_height = std::max(
                    widgets_view_bottom - widgets_view_top,
                    unscrolled.fl_bot - widgets_view_top
                );
                const float thumb_height = std::clamp(
                    track_height * (widgets_view_bottom - widgets_view_top) / content_height,
                    28.0f,
                    track_height
                );
                const float thumb_top = track_top +
                    (track_height - thumb_height) * (settings_widgets_scroll_offset_ / settings_widgets_scroll_max_);
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track_left, track_top, track_right, track_bottom), 2.0f, 2.0f),
                    widgets_div
                );
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track_left, thumb_top, track_right, thumb_top + thumb_height), 2.0f, 2.0f),
                    widgets_desc
                );
            }

            if (widgets_desc) widgets_desc->Release();
            if (widgets_div) widgets_div->Release();
        } else if (settings_section_ == 2) {
            const float warnings_content_left = mx + layout.navigation_width + 20.0f;
            const float warnings_content_right = mx + mw - 18.0f;
            const float warnings_view_top = my + 148.0f;
            const float warnings_view_bottom = footer_top - 14.0f;
            const WarningsSettingsLayout unscrolled = GetMeasuredWarningsLayout(
                warnings_content_left, warnings_content_right, my
            );
            settings_warnings_scroll_max_ = std::max(0.0f, unscrolled.info_bot - warnings_view_bottom);
            settings_warnings_scroll_offset_ = std::clamp(
                settings_warnings_scroll_offset_, 0.0f, settings_warnings_scroll_max_
            );
            const WarningsSettingsLayout w = GetMeasuredWarningsLayout(
                warnings_content_left,
                warnings_content_right,
                my, settings_warnings_scroll_offset_
            );

            const auto draw_warning_switch = [&](float center_y, bool enabled) {
                const D2D1_RECT_F track = D2D1::RectF(
                    w.toggle_left,
                    center_y - w.toggle_h * 0.5f,
                    w.toggle_right,
                    center_y + w.toggle_h * 0.5f
                );
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(track, w.toggle_h * 0.5f, w.toggle_h * 0.5f),
                    enabled ? brush_accent_dl_ : brush_card_border_
                );
                render_target_->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(enabled ? track.right - 11.0f : track.left + 11.0f, center_y),
                        7.0f,
                        7.0f
                    ),
                    brush_background_
                );
            };
            const auto draw_warning_row = [&](int index, SettingsSymbol symbol, const wchar_t* name,
                                              int value, const wchar_t* unit, bool enabled,
                                              SettingsNumericField field) {
                const float top = w.row[index];
                const D2D1_RECT_F icon_rect = D2D1::RectF(w.card_left + w.pad, top + 4.0f,
                    w.card_left + w.pad + 24.0f, top + 28.0f);
                const D2D1_RECT_F input_rect = D2D1::RectF(
                    w.input_left,
                    top + 48.0f,
                    w.input_right,
                    top + 48.0f + w.control_h
                );
                const D2D1_RECT_F unit_rect = D2D1::RectF(
                    w.unit_left,
                    input_rect.top,
                    w.unit_right,
                    input_rect.bottom
                );
                const float text_left = icon_rect.right + 8.0f;
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(icon_rect, 8.0f, 8.0f), brush_popup_bg_);
                render_target_->DrawRoundedRectangle(D2D1::RoundedRect(icon_rect, 8.0f, 8.0f), brush_chart_grid_, 1.0f);
                draw_settings_symbol(symbol, icon_rect, brush_text_primary_);
                render_target_->DrawText(
                    name, static_cast<UINT32>(wcslen(name)), format_body_bold_,
                    D2D1::RectF(text_left, top + 6.0f, w.toggle_left - 8.0f, top + 27.0f), brush_text_primary_
                );
                std::wstring rule_note = L"Notify at";
                if (index >= 2) rule_note += L" \u00B7 " + WatchmanSensors::Format(
                    dash_snap.temperatures.readings[static_cast<size_t>(index - 2)], GetTickCount64());
                render_target_->DrawText(
                    rule_note.c_str(), static_cast<UINT32>(rule_note.size()), format_dashboard_note_,
                    D2D1::RectF(w.input_left, top + 29.0f, w.card_right - w.pad, top + 45.0f), brush_text_secondary_
                );
                draw_value_box(input_rect, std::to_wstring(value), field);
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(unit_rect, 9.0f, 9.0f), brush_popup_bg_);
                render_target_->DrawRoundedRectangle(D2D1::RoundedRect(unit_rect, 9.0f, 9.0f), brush_chart_grid_, 1.0f);
                const auto previous_text_alignment = format_dashboard_note_->GetTextAlignment();
                const auto previous_paragraph_alignment = format_dashboard_note_->GetParagraphAlignment();
                format_dashboard_note_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                format_dashboard_note_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                render_target_->DrawText(
                    unit, static_cast<UINT32>(wcslen(unit)), format_dashboard_note_,
                    D2D1::RectF(unit_rect.left + 2.0f, unit_rect.top, unit_rect.right - (index == 0 ? 9.0f : 2.0f), unit_rect.bottom),
                    brush_text_primary_
                );
                format_dashboard_note_->SetTextAlignment(previous_text_alignment);
                format_dashboard_note_->SetParagraphAlignment(previous_paragraph_alignment);
                if (index == 0) {
                    DrawVectorIcon(
                        render_target_, VectorIcon::ChevronDown,
                        D2D1::RectF(unit_rect.right - 11.0f, unit_rect.top + 10.0f, unit_rect.right - 3.0f, unit_rect.bottom - 9.0f),
                        brush_text_secondary_, 0.9f
                    );
                }
                draw_warning_switch(top + 16.0f, enabled);
                if (index + 1 < w.rule_count) {
                    render_target_->DrawLine(
                        D2D1::Point2F(w.card_left + w.pad, top + w.row_h - 4.0f),
                        D2D1::Point2F(w.card_right - w.pad, top + w.row_h - 4.0f),
                        brush_picker_month_border_,
                        1.0f
                    );
                }
            };

            render_target_->DrawText(
                L"Warnings", 8, format_dashboard_value_,
                D2D1::RectF(w.card_left, w.title_y, w.card_right, w.title_y + 30.0f), brush_text_primary_
            );
            render_target_->DrawText(
                L"Traffic, memory and sensor alerts.", static_cast<UINT32>(wcslen(L"Traffic, memory and sensor alerts.")), format_dashboard_note_,
                D2D1::RectF(w.card_left, w.subtitle_y, w.card_right, w.subtitle_y + 20.0f), brush_text_secondary_
            );

            render_target_->PushAxisAlignedClip(
                D2D1::RectF(warnings_content_left - 4.0f, warnings_view_top, warnings_content_right + 4.0f, warnings_view_bottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
            );
            draw_group_card(D2D1::RectF(w.card_left, w.alert_top, w.card_right, w.alert_bot));
            render_target_->DrawText(
                L"Windows alerts", static_cast<UINT32>(wcslen(L"Windows alerts")), format_body_bold_,
                D2D1::RectF(w.card_left + w.pad, w.alert_top + 16.0f, w.toggle_left - 14.0f, w.alert_top + 34.0f), brush_text_primary_
            );
            render_target_->DrawText(
                L"Updates + warnings. Save to apply.",
                static_cast<UINT32>(wcslen(L"Updates + warnings. Save to apply.")), format_dashboard_note_,
                D2D1::RectF(w.card_left + w.pad, w.alert_top + 45.0f, w.card_right - w.pad, w.alert_bot - 12.0f), brush_text_secondary_
            );
            draw_warning_switch(w.alert_top + 26.0f, draft.alerts_enabled);

            render_target_->DrawText(
                L"Send a warning when", static_cast<UINT32>(wcslen(L"Send a warning when")), format_body_bold_,
                D2D1::RectF(w.card_left + w.pad, w.section_label, w.card_right - w.pad, w.section_label + 18.0f), brush_text_primary_
            );
            draw_group_card(D2D1::RectF(w.card_left, w.threshold_top, w.card_right, w.threshold_bot));
            draw_warning_row(0, SettingsTraffic, L"Today's traffic", draft.traffic_warning_threshold,
                draft.traffic_warning_gigabytes ? L"GiB" : L"MiB", draft.traffic_warning_enabled, SettingsNumericField::TrafficWarning);
            draw_warning_row(1, SettingsChip, L"Memory usage", draft.memory_warning_threshold, L"%",
                draft.memory_warning_enabled, SettingsNumericField::MemoryWarning);
            draw_warning_row(2, SettingsChip, L"CPU temperature", draft.cpu_temp_warning_threshold, L"\u00B0C",
                draft.cpu_temp_warning_enabled, SettingsNumericField::CpuTemperatureWarning);
            draw_warning_row(3, SettingsChip, L"GPU temperature", draft.gpu_temp_warning_threshold, L"\u00B0C",
                draft.gpu_temp_warning_enabled, SettingsNumericField::GpuTemperatureWarning);
            draw_warning_row(4, SettingsChip, L"Disk temperature", draft.disk_temp_warning_threshold, L"\u00B0C",
                draft.disk_temp_warning_enabled, SettingsNumericField::DiskTemperatureWarning);
            draw_warning_row(5, SettingsChip, L"Mainboard temp.", draft.mainboard_temp_warning_threshold, L"\u00B0C",
                draft.mainboard_temp_warning_enabled, SettingsNumericField::MainboardTemperatureWarning);

            draw_group_card(D2D1::RectF(w.card_left, w.sensors_top, w.card_right, w.sensors_bot));
            render_target_->DrawText(L"Sensor setup", 12, format_body_bold_,
                D2D1::RectF(w.card_left + w.pad, w.sensors_top + 14.0f, w.card_right - w.pad, w.sensors_top + 36.0f), brush_text_primary_);
            if (settings_sensor_setup_text_.layout) render_target_->DrawTextLayout(
                D2D1::Point2F(w.card_left + w.pad, w.sensors_text_top), settings_sensor_setup_text_.layout.Get(),
                brush_text_secondary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);

            draw_group_card(D2D1::RectF(w.card_left, w.info_top, w.card_right, w.info_bot));
            if (settings_warning_details_text_.layout) render_target_->DrawTextLayout(
                D2D1::Point2F(w.card_left + w.pad, w.info_text_top), settings_warning_details_text_.layout.Get(),
                brush_text_secondary_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            render_target_->PopAxisAlignedClip();

            if (settings_warnings_scroll_max_ > 1.0f) {
                const float track_left = mx + mw - 10.0f;
                const float track_right = mx + mw - 7.0f;
                const float track_top = warnings_view_top + 5.0f;
                const float track_bottom = warnings_view_bottom - 5.0f;
                const float track_height = std::max(1.0f, track_bottom - track_top);
                const float content_height = std::max(
                    warnings_view_bottom - warnings_view_top,
                    unscrolled.info_bot - warnings_view_top
                );
                const float thumb_height = std::clamp(
                    track_height * (warnings_view_bottom - warnings_view_top) / content_height,
                    28.0f,
                    track_height
                );
                const float thumb_top = track_top +
                    (track_height - thumb_height) * (settings_warnings_scroll_offset_ / settings_warnings_scroll_max_);
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track_left, track_top, track_right, track_bottom), 2.0f, 2.0f),
                    brush_chart_grid_
                );
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(track_left, thumb_top, track_right, thumb_top + thumb_height), 2.0f, 2.0f),
                    brush_text_secondary_
                );
            }
        } else if (settings_section_ == 3) {
            const float left = mx + layout.navigation_width + 20.0f;
            const float right = mx + mw - 18.0f;
            const auto page = GetFloatingMonitorSettingsLayout(
                left, right, my, mh, settings_appearance_scroll_offset_
            );
            settings_appearance_scroll_offset_ = page.scroll_offset;
            settings_appearance_scroll_max_ = page.scroll_max;
            const auto defaults = GetFloatingMonitorDefaultColors();

            draw_page_title(L"Floating Monitor", L"Drag to move. Lock to keep it in place.");
            render_target_->PushAxisAlignedClip(
                D2D1::RectF(left - 2.0f, page.view_top, right + 2.0f, page.view_bottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
            );
            const auto draw_monitor_card = [&](const D2D1_RECT_F& rect, const wchar_t* title) {
                draw_group_card(rect);
                render_target_->DrawText(title, static_cast<UINT32>(wcslen(title)), format_body_bold_,
                    D2D1::RectF(rect.left + 18.0f, rect.top + 14.0f, rect.right - 18.0f, rect.top + 34.0f), brush_text_primary_);
            };
            draw_monitor_card(page.controls_card, L"Position & transparency");
            const auto draw_monitor_toggle = [&](const D2D1_RECT_F& row, const wchar_t* title, const wchar_t* detail, bool enabled) {
                const D2D1_RECT_F track = D2D1::RectF(row.right - 40.0f, row.top + 12.0f, row.right, row.top + 34.0f);
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(track, 11.0f, 11.0f), enabled ? brush_accent_dl_ : brush_card_border_);
                render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(enabled ? track.right - 11.0f : track.left + 11.0f, row.top + 23.0f), 7.0f, 7.0f), brush_background_);
                render_target_->DrawText(title, static_cast<UINT32>(wcslen(title)), format_body_,
                    D2D1::RectF(row.left, row.top + 4.0f, track.left - 10.0f, row.top + 24.0f), brush_text_primary_);
                render_target_->DrawText(detail, static_cast<UINT32>(wcslen(detail)), format_dashboard_note_,
                    D2D1::RectF(row.left, row.top + 26.0f, track.left - 10.0f, row.bottom - 4.0f), brush_text_secondary_);
            };
            draw_monitor_card(page.readings_card, L"Readings");
            draw_monitor_toggle(page.cpu_row, L"CPU usage", L"Show live CPU percentage.", draft.floating_monitor_show_cpu);
            draw_monitor_toggle(page.memory_row, L"Memory usage", L"Show live memory percentage.", draft.floating_monitor_show_memory);
            draw_monitor_toggle(page.lock_row, L"Lock position", L"Drag when unlocked.", draft.floating_monitor_lock_position);
            draw_monitor_toggle(page.transparent_row, L"Transparent", L"Text stays solid.", draft.floating_monitor_transparent_background);
            render_target_->DrawText(L"Panel", 5, format_body_,
                D2D1::RectF(page.lock_row.left, page.opacity_minus.top + 7.0f, page.opacity_minus.left - 10.0f, page.opacity_minus.bottom), brush_text_primary_);
            draw_button(page.opacity_minus, L"\u2212", draft.floating_monitor_opacity > FloatingMonitorLayout::minimum_opacity, false);
            const std::wstring opacity_text = std::to_wstring(draft.floating_monitor_opacity) + L"%";
            draw_button(page.opacity_value, opacity_text.c_str(), true, false);
            draw_button(page.opacity_plus, L"+", draft.floating_monitor_opacity < FloatingMonitorLayout::maximum_opacity, false);
            draw_button(page.reset_position, L"Reset position", true, false);

            draw_monitor_card(page.colors_card, L"Colors");
            const wchar_t* labels[] = { L"Values", L"Upload", L"Download", L"CPU", L"Memory" };
            const COLORREF colors[] = {
                draft.floating_monitor_text_color, draft.floating_monitor_upload_color,
                draft.floating_monitor_download_color, draft.floating_monitor_cpu_color,
                draft.floating_monitor_memory_color
            };
            for (size_t index = 0; index < page.color_count; ++index) {
                const auto& row = page.color_rows[index];
                const auto& swatch = page.color_controls[index];
                const COLORREF value = colors[index] == CLR_INVALID ? defaults[index] : colors[index];
                render_target_->DrawText(labels[index], static_cast<UINT32>(wcslen(labels[index])), format_body_,
                    D2D1::RectF(row.left, row.top + 5.0f, swatch.left - 12.0f, row.top + 25.0f), brush_text_primary_);
                std::wstring detail = L"Default: white";
                if (colors[index] != CLR_INVALID) {
                    wchar_t hex[8]{};
                    swprintf_s(hex, L"#%02X%02X%02X", GetRValue(value), GetGValue(value), GetBValue(value));
                    detail = hex;
                }
                render_target_->DrawText(detail.c_str(), static_cast<UINT32>(detail.size()), format_dashboard_note_,
                    D2D1::RectF(row.left, row.top + 27.0f, swatch.left - 12.0f, row.bottom - 5.0f), brush_text_secondary_);
                ID2D1SolidColorBrush* color_brush = nullptr;
                if (SUCCEEDED(render_target_->CreateSolidColorBrush(
                        D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f),
                        &color_brush))) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(swatch, 8.0f, 8.0f), color_brush);
                    color_brush->Release();
                }
                const bool hovered = mouse_y_ >= page.view_top && mouse_y_ <= page.view_bottom &&
                    IsPointInside(swatch, mouse_x_, mouse_y_);
                render_target_->DrawRoundedRectangle(D2D1::RoundedRect(swatch, 8.0f, 8.0f),
                    hovered ? brush_text_primary_ : brush_chart_grid_, hovered ? 1.5f : 1.0f);
                if (index + 1 < page.color_count) {
                    render_target_->DrawLine(D2D1::Point2F(row.left, row.bottom), D2D1::Point2F(row.right, row.bottom),
                        brush_picker_month_border_, 0.75f);
                }
            }

            draw_monitor_card(page.font_card, L"Text style");
            constexpr wchar_t font_description[] = L"Segoe UI \u00b7 Light by default";
            render_target_->DrawText(font_description, static_cast<UINT32>(wcslen(font_description)),
                format_dashboard_note_, page.font_description, brush_text_secondary_);
            render_target_->DrawText(L"Weight", 6, format_body_, page.weight_label, brush_text_primary_);
            for (size_t index = 0; index < page.weight_controls.size(); ++index) {
                const auto weight = FloatingMonitorText::weights[index];
                draw_button(page.weight_controls[index], FloatingMonitorText::Label(weight), true,
                    draft.floating_monitor_text_weight == weight);
            }
            render_target_->DrawText(L"Size", 4, format_body_,
                D2D1::RectF(page.font_description.left, page.font_minus.top + 8.0f,
                    page.font_minus.left - 8.0f, page.font_minus.bottom), brush_text_primary_);
            draw_button(page.font_minus, L"\u2212", draft.floating_monitor_font_size > FloatingMonitorLayout::minimum_font_size, false);
            const std::wstring font_size_text = std::to_wstring(draft.floating_monitor_font_size) + L" px";
            draw_button(page.font_value, font_size_text.c_str(), true, false);
            draw_button(page.font_plus, L"+", draft.floating_monitor_font_size < FloatingMonitorLayout::maximum_font_size, false);
            if (page.preview.bottom >= page.view_top && page.preview.top <= page.view_bottom) {
                ID2D1SolidColorBrush* preview_brush = nullptr;
                if (SUCCEEDED(render_target_->CreateSolidColorBrush(
                        D2D1::ColorF(FloatingMonitorLayout::panel_background_color), &preview_brush))) {
                    render_target_->FillRoundedRectangle(D2D1::RoundedRect(page.preview, 8.0f, 8.0f), preview_brush);
                    std::wstring sample = L"UP   1.2 MB/s   DN   8.6 MB/s";
                    if (draft.floating_monitor_show_cpu || draft.floating_monitor_show_memory) sample += L"\n";
                    if (draft.floating_monitor_show_cpu) sample += L"CPU   24%";
                    if (draft.floating_monitor_show_cpu && draft.floating_monitor_show_memory) sample += L"   ";
                    if (draft.floating_monitor_show_memory) sample += L"MEM   58%";
                    Microsoft::WRL::ComPtr<IDWriteTextLayout> preview_text;
                    if (SUCCEEDED(dwrite_factory_->CreateTextLayout(sample.c_str(), static_cast<UINT32>(sample.size()), format_floating_preview_,
                            page.preview.right - page.preview.left - 24.0f, page.preview.bottom - page.preview.top - 16.0f,
                            preview_text.GetAddressOf()))) {
                        const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(sample.size())};
                        preview_text->SetFontSize(static_cast<float>(draft.floating_monitor_font_size), all);
                        preview_text->SetFontWeight(
                            static_cast<DWRITE_FONT_WEIGHT>(FloatingMonitorText::Value(draft.floating_monitor_text_weight)), all);
                        preview_text->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                        preview_text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        const auto color = [](COLORREF value) {
                            return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f);
                        };
                        preview_brush->SetColor(color(colors[0] == CLR_INVALID ? defaults[0] : colors[0]));
                        const wchar_t* tokens[] = {L"UP", L"DN", L"CPU", L"MEM"};
                        for (size_t index = 0; index < std::size(tokens); ++index) {
                            const size_t start = sample.find(tokens[index]);
                            if (start == std::wstring::npos) continue;
                            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> label_ink;
                            if (SUCCEEDED(render_target_->CreateSolidColorBrush(
                                    color(colors[index + 1] == CLR_INVALID ? defaults[index + 1] : colors[index + 1]),
                                    label_ink.GetAddressOf()))) {
                                preview_text->SetDrawingEffect(label_ink.Get(),
                                    DWRITE_TEXT_RANGE{static_cast<UINT32>(start), static_cast<UINT32>(wcslen(tokens[index]))});
                            }
                        }
                        render_target_->DrawTextLayout(D2D1::Point2F(page.preview.left + 12.0f, page.preview.top + 8.0f),
                            preview_text.Get(), preview_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    preview_brush->Release();
                }
                render_target_->DrawRoundedRectangle(D2D1::RoundedRect(page.preview, 8.0f, 8.0f), brush_card_border_, 1.0f);
            }
            constexpr wchar_t preview_caption[] = L"No outline. Save Changes to apply.";
            render_target_->DrawText(preview_caption, static_cast<UINT32>(wcslen(preview_caption)), format_dashboard_note_,
                page.preview_caption, brush_text_secondary_);
            render_target_->PopAxisAlignedClip();

            if (page.scroll_max > 1.0f) {
                const float track_left = mx + mw - 10.0f;
                const float track_right = mx + mw - 7.0f;
                const float track_top = page.view_top + 5.0f;
                const float track_bottom = page.view_bottom - 5.0f;
                const float track_height = std::max(1.0f, track_bottom - track_top);
                const float viewport_height = page.view_bottom - page.view_top;
                const float thumb_height = std::clamp(track_height * viewport_height / (viewport_height + page.scroll_max), 28.0f, track_height);
                const float thumb_top = track_top + (track_height - thumb_height) * (page.scroll_offset / page.scroll_max);
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track_left, track_top, track_right, track_bottom), 2.0f, 2.0f), brush_chart_grid_);
                render_target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track_left, thumb_top, track_right, thumb_top + thumb_height), 2.0f, 2.0f), brush_text_secondary_);
            }
        }

        render_target_->DrawLine(D2D1::Point2F(mx + layout.navigation_width + 0.5f, footer_top - 10.0f),
            D2D1::Point2F(mx + mw - 18.0f, footer_top - 10.0f), brush_chart_grid_, 0.5f);
        // Footer buttons — equal height, roomier gap, shared baseline
        const float btn_h = 34.0f;
        const float btn_y = footer_top + 10.0f;
        const bool compact_footer = content_width < 320.0f;
        const float btn_gap = compact_footer ? 8.0f : 12.0f;
        const float rec_w = compact_footer ? 94.0f : 118.0f;
        const float undo_w = compact_footer ? 60.0f : 72.0f;
        const float save_w = content_right - (content_left + rec_w + btn_gap + undo_w + btn_gap);
        const D2D1_RECT_F recommended_rect = D2D1::RectF(content_left, btn_y, content_left + rec_w, btn_y + btn_h);
        const D2D1_RECT_F undo_rect = D2D1::RectF(content_left + rec_w + btn_gap, btn_y, content_left + rec_w + btn_gap + undo_w, btn_y + btn_h);
        const D2D1_RECT_F save_rect = D2D1::RectF(content_right - save_w, btn_y, content_right, btn_y + btn_h);
        // Draw Recommended button with gear icon
        {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(recommended_rect, 8.0f, 8.0f);
            render_target_->FillRoundedRectangle(rr, brush_popup_bg_);
            render_target_->DrawRoundedRectangle(rr, brush_chart_grid_, 1.0f);
            // Small gear icon
            const D2D1_RECT_F gear_icon = D2D1::RectF(recommended_rect.left + 10.0f, recommended_rect.top + 8.0f, recommended_rect.left + 24.0f, recommended_rect.bottom - 8.0f);
            draw_settings_symbol(SettingsGear, gear_icon, brush_text_primary_);
            const wchar_t* defaults_label = settings_section_ == 3 ? L"Defaults" : (compact_footer ? L"Reset" : L"Recommended");
            render_target_->DrawText(defaults_label, static_cast<UINT32>(wcslen(defaults_label)), format_limit_button_,
                D2D1::RectF(recommended_rect.left + 22.0f, recommended_rect.top, recommended_rect.right, recommended_rect.bottom), brush_text_primary_);
        }
        // Draw Undo button with undo arrow icon
        {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(undo_rect, 8.0f, 8.0f);
            render_target_->FillRoundedRectangle(rr, brush_popup_bg_);
            render_target_->DrawRoundedRectangle(rr, brush_chart_grid_, 1.0f);
            DrawVectorIcon(
                render_target_,
                VectorIcon::Undo,
                D2D1::RectF(undo_rect.left + 8.0f, undo_rect.top + 7.0f, undo_rect.left + 24.0f, undo_rect.bottom - 7.0f),
                preferences_dirty_ ? brush_text_primary_ : brush_text_secondary_,
                1.15f
            );
            render_target_->DrawText(L"Undo", 4, format_limit_button_,
                D2D1::RectF(undo_rect.left + 20.0f, undo_rect.top, undo_rect.right, undo_rect.bottom),
                preferences_dirty_ ? brush_text_primary_ : brush_text_secondary_);
        }
        // Draw Save Changes button (always primary/filled style)
        {
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(save_rect, 8.0f, 8.0f);
            render_target_->FillRoundedRectangle(rr, brush_text_primary_);
            DrawVectorIcon(
                render_target_,
                VectorIcon::Save,
                D2D1::RectF(save_rect.left + 10.0f, save_rect.top + 7.0f, save_rect.left + 26.0f, save_rect.bottom - 7.0f),
                brush_active_text_,
                1.15f
            );
            const wchar_t* save_label = compact_footer ? L"Save" : L"Save Changes";
            render_target_->DrawText(save_label, static_cast<UINT32>(wcslen(save_label)), format_limit_button_,
                D2D1::RectF(save_rect.left + 24.0f, save_rect.top, save_rect.right, save_rect.bottom),
                brush_active_text_);
        }
        return;
    }

    if (settings_section_ == 0) {
        draw_page_title(L"GENERAL", L"Startup, appearance, and shared monitoring settings.");
        draw_toggle(my + 145.0f, L"Launch on Windows startup", L"Open Watchman after you sign in.", autostart_enabled_);

        draw_section_label(L"APPEARANCE", my + 212.0f);
        const wchar_t* theme_labels[] = { L"Dark", L"Light", L"Auto" };
        const float theme_top = my + 240.0f;
        const float option_width = content_width / UIStyle::theme_count;
        for (int i = 0; i < UIStyle::theme_count; ++i) {
            const float option_x = content_left + option_width * static_cast<float>(i);
            const bool selected = static_cast<int>(current_theme_) == i;
            const D2D1_POINT_2F center = D2D1::Point2F(option_x + 8.0f, theme_top + 14.0f);
            render_target_->DrawEllipse(D2D1::Ellipse(center, 7.0f, 7.0f), brush_text_secondary_, 1.25f);
            if (selected) {
                render_target_->FillEllipse(D2D1::Ellipse(center, 3.5f, 3.5f), brush_accent_dl_);
            }
            render_target_->DrawText(
                theme_labels[i], static_cast<UINT32>(wcslen(theme_labels[i])), format_status_,
                D2D1::RectF(option_x + 20.0f, theme_top, option_x + option_width - 2.0f, theme_top + 28.0f), brush_text_primary_
            );
        }

        draw_section_label(L"MONITORING ADAPTER", my + 292.0f);
        draw_adapter_row(my + 319.0f);
        render_target_->DrawText(
            L"This source is used across the dashboard and taskbar widget.",
            static_cast<UINT32>(wcslen(L"This source is used across the dashboard and taskbar widget.")),
            format_status_,
            D2D1::RectF(content_left, my + 363.0f, content_right, my + 400.0f), brush_text_secondary_
        );
    } else if (settings_section_ == 1) {
        draw_page_title(L"TASKBAR WIDGET", L"Control the lightweight taskbar monitor.");
        draw_toggle(my + 145.0f, L"Show taskbar speed widget", L"Keep upload and download visible in the taskbar.", widget_enabled_);
        draw_toggle(my + 201.0f, L"Upload & download", L"Show network speeds only.", widget_mode_ == TaskbarWidgetMode::Network);
        draw_toggle(my + 257.0f, L"CPU & memory", L"Show system usage only.", widget_mode_ == TaskbarWidgetMode::System);
        draw_toggle(my + 313.0f, L"All four", L"Show network speeds and system usage.", widget_mode_ == TaskbarWidgetMode::All);

        draw_section_label(L"MONITORING ADAPTER", my + 388.0f);
        draw_adapter_row(my + 415.0f);
        render_target_->DrawText(
            L"Choose Auto to follow the Windows default connection.",
            static_cast<UINT32>(wcslen(L"Choose Auto to follow the Windows default connection.")),
            format_status_,
            D2D1::RectF(content_left, my + 459.0f, content_right, my + 495.0f), brush_text_secondary_
        );
    } else {
        draw_page_title(L"USAGE WARNINGS", L"Manage the alerts Watchman can show.");
        draw_toggle(my + 145.0f, L"Enable Watchman alerts", L"Allow network-health notifications.", alerts_enabled_);

        const D2D1_RECT_F future = D2D1::RectF(content_left, my + 215.0f, content_right, my + 312.0f);
        const D2D1_ROUNDED_RECT future_round = D2D1::RoundedRect(future, 10.0f, 10.0f);
        render_target_->FillRoundedRectangle(future_round, brush_card_bg_);
        render_target_->DrawRoundedRectangle(future_round, brush_card_border_, 1.0f);
        render_target_->DrawText(
            L"DATA-LIMIT ALERTS", static_cast<UINT32>(wcslen(L"DATA-LIMIT ALERTS")), format_dashboard_label_,
            D2D1::RectF(future.left + 14.0f, future.top + 14.0f, future.right - 14.0f, future.top + 32.0f), brush_text_primary_
        );
        render_target_->DrawText(
            L"Threshold controls will live here when we add your high-usage warning rules.",
            static_cast<UINT32>(wcslen(L"Threshold controls will live here when we add your high-usage warning rules.")),
            format_status_,
            D2D1::RectF(future.left + 14.0f, future.top + 40.0f, future.right - 14.0f, future.bottom - 12.0f), brush_text_secondary_
        );
    }
}

namespace {
bool ReadWindowsLightTaskbarTheme() {
    HKEY key{};
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = REG_DWORD;
    bool is_light = false;

    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0,
            KEY_READ,
            &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS) {
            is_light = value != 0;
        }
        RegCloseKey(key);
    }

    return is_light;
}

bool PointInsideRect(const POINT& pt, const RECT& rect) {
    return pt.x >= rect.left && pt.x < rect.right && pt.y >= rect.top && pt.y < rect.bottom;
}

COLORREF BlendColor(COLORREF base, COLORREF overlay, int alpha) {
    const int inv = 255 - alpha;
    const int r = (GetRValue(base) * inv + GetRValue(overlay) * alpha) / 255;
    const int g = (GetGValue(base) * inv + GetGValue(overlay) * alpha) / 255;
    const int b = (GetBValue(base) * inv + GetBValue(overlay) * alpha) / 255;
    return RGB(r, g, b);
}

COLORREF SampleTaskbarBackground(HWND hwnd, bool light_taskbar) {
    const COLORREF fallback = light_taskbar ? RGB(243, 243, 243) : RGB(32, 32, 32);

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT taskbar_rect{};
    RECT widget_rect{};
    if (!taskbar || !GetWindowRect(taskbar, &taskbar_rect) || !GetWindowRect(hwnd, &widget_rect)) {
        return fallback;
    }

    const int taskbar_w = taskbar_rect.right - taskbar_rect.left;
    const int taskbar_h = taskbar_rect.bottom - taskbar_rect.top;
    const bool horizontal = taskbar_w >= taskbar_h;

    POINT candidates[4]{};
    if (horizontal) {
        const int y = taskbar_rect.top + std::max(0, taskbar_h / 2);
        candidates[0] = POINT{ widget_rect.left - 6, y };
        candidates[1] = POINT{ widget_rect.right + 6, y };
        candidates[2] = POINT{ taskbar_rect.left + 12, y };
        candidates[3] = POINT{ taskbar_rect.right - 12, y };
    } else {
        const int x = taskbar_rect.left + std::max(0, taskbar_w / 2);
        candidates[0] = POINT{ x, widget_rect.top - 6 };
        candidates[1] = POINT{ x, widget_rect.bottom + 6 };
        candidates[2] = POINT{ x, taskbar_rect.top + 12 };
        candidates[3] = POINT{ x, taskbar_rect.bottom - 12 };
    }

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        return fallback;
    }

    COLORREF sampled = CLR_INVALID;
    for (const POINT& pt : candidates) {
        if (!PointInsideRect(pt, taskbar_rect) || PointInsideRect(pt, widget_rect)) {
            continue;
        }

        sampled = GetPixel(screen_dc, pt.x, pt.y);
        if (sampled != CLR_INVALID) {
            break;
        }
    }

    ReleaseDC(nullptr, screen_dc);
    if (sampled == CLR_INVALID) {
        return fallback;
    }

    return light_taskbar
        ? BlendColor(sampled, RGB(255, 255, 255), 3)
        : BlendColor(sampled, RGB(0, 0, 0), 12);
}

void DrawWidgetText(HDC hdc, const std::wstring& text, RECT rect, UINT format, COLORREF color, COLORREF shadow) {
    RECT shadow_rect = rect;
    OffsetRect(&shadow_rect, 0, 1);
    SetTextColor(hdc, shadow);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.length()), &shadow_rect, format);

    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.length()), &rect, format);
}

std::wstring FormatTaskbarSpeed(double bytes_per_sec, bool use_bits_per_second) {
    double value = std::max(0.0, bytes_per_sec);
    double multiplier = 1024.0;
    const wchar_t* unit = L"KB/s";
    if (use_bits_per_second) {
        value *= 8.0;
        multiplier = 1000.0;
        unit = L"Kbps";
    }
    double displayed = value / multiplier;

    if (value >= multiplier * multiplier * multiplier) {
        unit = use_bits_per_second ? L"Gbps" : L"GB/s";
        displayed = value / (multiplier * multiplier * multiplier);
    } else if (value >= multiplier * multiplier) {
        unit = use_bits_per_second ? L"Mbps" : L"MB/s";
        displayed = value / (multiplier * multiplier);
    }

    std::wstringstream text;
    text << std::fixed << std::setprecision(1) << displayed << L" " << unit;
    return text.str();
}

struct FloatingMonitorSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous_bitmap = nullptr;
    DWORD* pixels = nullptr;

    FloatingMonitorSurface(HDC reference, int width, int height) {
        dc = CreateCompatibleDC(reference);
        if (!dc) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(reference, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return;
        pixels = static_cast<DWORD*>(bits);
        previous_bitmap = SelectObject(dc, bitmap);
    }

    ~FloatingMonitorSurface() {
        if (previous_bitmap) SelectObject(dc, previous_bitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }

    FloatingMonitorSurface(const FloatingMonitorSurface&) = delete;
    FloatingMonitorSurface& operator=(const FloatingMonitorSurface&) = delete;
};

void BlendWidgetPixel(DWORD* destination, COLORREF color, BYTE source_alpha) {
    if (source_alpha == 0) return;

    const DWORD target = *destination;
    const UINT inverse_alpha = 255u - source_alpha;
    const UINT src_blue = (static_cast<UINT>(GetBValue(color)) * source_alpha + 127u) / 255u;
    const UINT src_green = (static_cast<UINT>(GetGValue(color)) * source_alpha + 127u) / 255u;
    const UINT src_red = (static_cast<UINT>(GetRValue(color)) * source_alpha + 127u) / 255u;
    const UINT dst_blue = target & 0xFFu;
    const UINT dst_green = (target >> 8u) & 0xFFu;
    const UINT dst_red = (target >> 16u) & 0xFFu;
    const UINT dst_alpha = (target >> 24u) & 0xFFu;

    const DWORD out_blue = src_blue + ((dst_blue * inverse_alpha + 127u) / 255u);
    const DWORD out_green = src_green + ((dst_green * inverse_alpha + 127u) / 255u);
    const DWORD out_red = src_red + ((dst_red * inverse_alpha + 127u) / 255u);
    const DWORD out_alpha = source_alpha + ((dst_alpha * inverse_alpha + 127u) / 255u);
    *destination = out_blue | (out_green << 8u) | (out_red << 16u) | (out_alpha << 24u);
}

void DrawLayeredWidgetText(
    HDC mask_dc,
    DWORD* mask_pixels,
    DWORD* destination_pixels,
    size_t pixel_count,
    const RECT& bounds,
    const std::wstring& text,
    RECT rect,
    UINT format,
    COLORREF color,
    BYTE opacity
) {
    FillRect(mask_dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetBkMode(mask_dc, TRANSPARENT);
    SetTextColor(mask_dc, RGB(255, 255, 255));
    DrawTextW(mask_dc, text.c_str(), static_cast<int>(text.length()), &rect, format);
    GdiFlush();

    for (size_t i = 0; i < pixel_count; ++i) {
        const DWORD pixel = mask_pixels[i];
        const BYTE coverage = static_cast<BYTE>(std::max({
            pixel & 0xFFu,
            (pixel >> 8u) & 0xFFu,
            (pixel >> 16u) & 0xFFu,
        }));
        const BYTE alpha = static_cast<BYTE>((static_cast<UINT>(coverage) * opacity + 127u) / 255u);
        BlendWidgetPixel(&destination_pixels[i], color, alpha);
    }
}

void DrawLayeredWidgetTextWithShadow(
    HDC mask_dc,
    DWORD* mask_pixels,
    DWORD* destination_pixels,
    size_t pixel_count,
    const RECT& bounds,
    const std::wstring& text,
    const RECT& rect,
    UINT format,
    COLORREF color,
    COLORREF shadow
) {
    RECT shadow_rect = rect;
    OffsetRect(&shadow_rect, 0, 1);
    DrawLayeredWidgetText(
        mask_dc, mask_pixels, destination_pixels, pixel_count, bounds, text, shadow_rect,
        format, shadow, 230
    );
    DrawLayeredWidgetText(
        mask_dc, mask_pixels, destination_pixels, pixel_count, bounds, text, rect,
        format, color, 255
    );
}
}

#if 0
void AppRenderer::RenderWidget(HWND hwnd, const DashboardSnapshot& dash_snap) {
    HDC target_hdc = GetDC(hwnd);
    if (!target_hdc) return;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        ReleaseDC(hwnd, target_hdc);
        return;
    }

    HDC hdc = CreateCompatibleDC(target_hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(target_hdc, width, height);
    if (!hdc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (hdc) DeleteDC(hdc);
        ReleaseDC(hwnd, target_hdc);
        return;
    }

    HGDIOBJ old_bitmap = SelectObject(hdc, bitmap);
    SetBkMode(hdc, TRANSPARENT);

    const bool light_taskbar = ReadWindowsLightTaskbarTheme();
    HBRUSH bg = CreateSolidBrush(SampleTaskbarBackground(hwnd, light_taskbar));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    HFONT font = CreateFontW(
        -11,
        0,
        0,
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_MODERN,
        L"Consolas"
    );
    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, font));

    double dl_val = static_cast<double>(dash_snap.download_speed_bps) / 8.0;
    double ul_val = static_cast<double>(dash_snap.upload_speed_bps) / 8.0;

    std::wstring dl_str, dl_unit;
    FormatSpeedWithUnit(dl_val, dl_str, dl_unit);

    std::wstring ul_str, ul_unit;
    FormatSpeedWithUnit(ul_val, ul_str, ul_unit);

    auto shortUnit = [](const std::wstring& unit) {
        if (unit.empty()) return std::wstring();
        if (unit[0] == L'B') return std::wstring(L"B/s");
        return unit.substr(0, 1);
    };

    std::wstring up_text = ul_str + L" " + shortUnit(ul_unit);
    std::wstring down_text = dl_str + L" " + shortUnit(dl_unit);

    const int widget_height = static_cast<int>(rc.bottom - rc.top);
    const int row_h = std::max(1, widget_height / 2);
    RECT up_arrow{ 4, 0, 18, row_h };
    RECT up_value{ 18, 0, IsWidgetNetworkOnly() ? rc.right - 2 : 82, row_h };
    RECT down_arrow{ 4, row_h, 18, rc.bottom };
    RECT down_value{ 18, row_h, IsWidgetNetworkOnly() ? rc.right - 2 : 82, rc.bottom };

    const COLORREF shadow = light_taskbar ? RGB(255, 255, 255) : RGB(0, 0, 0);
    const COLORREF value_color = light_taskbar ? RGB(38, 49, 58) : RGB(255, 255, 255);
    const COLORREF up_color = light_taskbar ? RGB(209, 63, 131) : RGB(240, 125, 172);
    const COLORREF down_color = light_taskbar ? RGB(0, 120, 212) : RGB(99, 214, 255);
    const COLORREF cpu_color = light_taskbar ? RGB(164, 95, 0) : RGB(243, 182, 74);
    const COLORREF mem_color = light_taskbar ? RGB(22, 122, 56) : RGB(134, 217, 108);

    DrawWidgetText(hdc, L"\u2191", up_arrow, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, up_color, shadow);
    DrawWidgetText(hdc, up_text, up_value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, value_color, shadow);

    DrawWidgetText(hdc, L"\u2193", down_arrow, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, down_color, shadow);
    DrawWidgetText(hdc, down_text, down_value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, value_color, shadow);

#if 0

    SetTextColor(hdc, RGB(240, 125, 172));
    DrawTextW(hdc, L"↑", 1, &up_arrow, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, up_text.c_str(), static_cast<int>(up_text.length()), &up_value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(hdc, RGB(99, 214, 255));
    DrawTextW(hdc, L"↓", 1, &down_arrow, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, down_text.c_str(), static_cast<int>(down_text.length()), &down_value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

#endif

    if (TaskbarWidgetModes::ShowsSystem(widget_mode_)) {
        std::wstringstream cpu_ss;
        cpu_ss << L"CPU " << FormatCpuUsage(dash_snap);
        std::wstring cpu_text = cpu_ss.str();
        RECT cpu_rect{ 88, 0, rc.right - 2, row_h };
        DrawWidgetText(hdc, cpu_text, cpu_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, cpu_color, shadow);

        std::wstringstream mem_ss;
        mem_ss << L"MEM " << std::fixed << std::setprecision(0) << dash_snap.ram_usage_percent << L"%";
        std::wstring mem_text = mem_ss.str();
        RECT mem_rect{ 88, row_h, rc.right - 2, rc.bottom };
        DrawWidgetText(hdc, mem_text, mem_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, mem_color, shadow);
    }

    BitBlt(target_hdc, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

    SelectObject(hdc, old_font);
    DeleteObject(font);
    SelectObject(hdc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(hdc);
    ReleaseDC(hwnd, target_hdc);
}
#endif

void AppRenderer::ResetWidgetHost() {
    if (widget_compositor_) widget_compositor_->Reset();
    widget_compositor_.reset();
    if (widget_render_target_) { widget_render_target_->Release(); widget_render_target_ = nullptr; }
    widget_composition_failed_ = false;
    widget_gpu_retry_.Reset();
    widget_first_frame_.Reset();
    widget_paint_result_ = {};
    tm_set_widget_manual_layered_paint(0);
    tm_set_widget_layered_style(0);
}

void AppRenderer::InvalidateWidgetFrame() {
    if (widget_compositor_) widget_compositor_->InvalidateFrame();
    widget_paint_result_ = {};
}

void AppRenderer::RenderWidget(HWND hwnd, const DashboardSnapshot& dash_snap) {
    uint64_t now = GetTickCount64();
#ifdef WATCHMAN_RENDER_TESTING
    if (test_widget_time_) now = test_widget_time_;
#endif
    widget_paint_result_.submitted = false;
    const auto previous = widget_paint_result_;
    const auto finish = [this, previous, now](WatchmanWidgetRuntime::PaintResult result) {
        widget_paint_result_ = result;
        if (result.backend == WatchmanWidgetRuntime::Backend::Gdi)
            widget_first_frame_.GdiResult(now, result.success);
        if (result.backend == WatchmanWidgetRuntime::Backend::Gdi) widget_gpu_retry_.GdiFrame(result.success);
#ifdef WATCHMAN_RENDER_TESTING
        if (result.success && result.submitted) ++test_widget_submissions_;
#endif
        if (previous.success != result.success || previous.backend != result.backend || previous.error != result.error) {
            const char* event = !result.success ? "presentation-failed"
                : result.backend == WatchmanWidgetRuntime::Backend::Gdi ? "gdi-presented" : "composition-presented";
            WatchmanStartupDiagnostics::Record("graphics", event, result.error);
        }
    };
    const auto fail = [&](DWORD error) {
        finish(WatchmanWidgetRuntime::LayeredResult(false, error ? error : ERROR_GEN_FAILURE));
    };
    if (!IsWindow(hwnd)) { fail(ERROR_INVALID_WINDOW_HANDLE); return; }

    TaskbarWidgetFrame frame{};
    const auto taskbar_edge = reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"Watchman.TaskbarEdge"));
    const auto taskbar_dpi = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, L"Watchman.TaskbarDpi"));
    frame.taskbar_edge = taskbar_edge != 0 ? static_cast<int>(taskbar_edge) - 1 : 0;
    frame.vertical = TaskbarWidgetLayout::IsVertical(frame.taskbar_edge);
    frame.dpi = taskbar_dpi != 0 ? static_cast<unsigned>(taskbar_dpi) : 96;
    frame.mode = widget_mode_;
    frame.light_taskbar = ReadWindowsLightTaskbarTheme();
    frame.upload_text = WatchmanLiveReadings::NetworkReady(dash_snap)
        ? FormatTaskbarSpeed(dash_snap.upload_speed_bps / 8.0, use_bits_per_second_) : L"--";
    frame.download_text = WatchmanLiveReadings::NetworkReady(dash_snap)
        ? FormatTaskbarSpeed(dash_snap.download_speed_bps / 8.0, use_bits_per_second_) : L"--";

    frame.cpu_text = FormatCpuUsage(dash_snap);

    std::wstringstream memory_text;
    memory_text << std::fixed << std::setprecision(0) << dash_snap.ram_usage_percent << L"%";
    frame.memory_text = WatchmanLiveReadings::MemoryReady(dash_snap, GetTickCount64()) ? memory_text.str() : L"--";
    frame.text_color = frame.light_taskbar ? RGB(38, 49, 58) : RGB(255, 255, 255);
    frame.upload_color = frame.light_taskbar ? RGB(209, 63, 131) : RGB(240, 125, 172);
    frame.download_color = frame.light_taskbar ? RGB(0, 120, 212) : RGB(99, 214, 255);
    frame.cpu_color = frame.light_taskbar ? RGB(164, 95, 0) : RGB(243, 182, 74);
    frame.memory_color = frame.light_taskbar ? RGB(22, 122, 56) : RGB(134, 217, 108);
    frame.font_family = UIStyle::FONT_MONO;

    if (widget_composition_failed_ && widget_gpu_retry_.Due(now, widget_gpu_retry_allowed_)) {
        WatchmanStartupDiagnostics::Record("graphics", "composition-retry");
        widget_compositor_.reset();
        tm_set_widget_manual_layered_paint(0);
        tm_set_widget_layered_style(0);
        if (tm_apply_widget_styles(hwnd)) widget_composition_failed_ = false;
        else widget_gpu_retry_.Failed(now);
    }
    const bool first_frame_gdi = widget_first_frame_.PreferGdi(now, widget_gpu_retry_allowed_);
    if (!first_frame_gdi && !widget_composition_failed_) {
        // A successful fast first frame used a layered window. Return to the
        // compositor style only when the deferred GPU trial is actually due.
        tm_set_widget_manual_layered_paint(0);
        tm_set_widget_layered_style(0);
        if (!tm_apply_widget_styles(hwnd)) { fail(tm_get_widget_result().error); return; }
        if (!widget_compositor_) {
            widget_compositor_ = std::make_unique<TaskbarWidgetCompositor>();
        }
        const uint64_t submissions = widget_compositor_->SubmissionCount();
#ifdef WATCHMAN_RENDER_TESTING
        ++test_composition_attempts_;
        const bool composition_ok = !test_fail_composition_ && widget_compositor_->Render(hwnd, frame);
#else
        const bool composition_ok = widget_compositor_->Render(hwnd, frame);
#endif
        if (composition_ok) {
            widget_gpu_retry_.Reset();
            finish({true, WatchmanWidgetRuntime::Backend::Composition, 0,
                    widget_compositor_->SubmissionCount() != submissions});
            return;
        }
        HRESULT composition_error = widget_compositor_->LastError();
#ifdef WATCHMAN_RENDER_TESTING
        if (test_fail_composition_) composition_error = E_ACCESSDENIED;
#endif
        WatchmanStartupDiagnostics::Record("graphics", "composition-failed", static_cast<uint32_t>(composition_error));

        // DirectComposition is unavailable only on a small set of graphics paths.
        // Keep the old layered renderer as a functional fallback for those systems.
        widget_composition_failed_ = true;
        widget_gpu_retry_.Failed(now);
    }

    // A latched fallback is retried without reinitializing D3D every second.
    // Healthy fallback frames permit only a quiet, cooldown-controlled trial.
    // Failure restores GDI in this same paint; there is no per-tick GPU loop.
    tm_set_widget_layered_style(1);
    tm_set_widget_manual_layered_paint(1);
    if (!tm_apply_widget_styles(hwnd)) { fail(tm_get_widget_result().error); return; }
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) { fail(GetLastError()); return; }
    const UINT width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    {
        HDC screen_dc = GetDC(nullptr);
        if (!screen_dc) { fail(GetLastError()); return; }

        HDC mem_dc = CreateCompatibleDC(screen_dc);
        if (!mem_dc) {
            const DWORD error = GetLastError();
            ReleaseDC(nullptr, screen_dc);
            fail(error);
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            const DWORD error = GetLastError();
            if (dib) DeleteObject(dib);
            DeleteDC(mem_dc);
            ReleaseDC(nullptr, screen_dc);
            fail(error);
            return;
        }

        const bool light_taskbar = ReadWindowsLightTaskbarTheme();
        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        DWORD* pixels = static_cast<DWORD*>(bits);
        const BYTE overlay_alpha = light_taskbar ? 3 : 13;
        const BYTE overlay_channel = light_taskbar ? overlay_alpha : 0;
        const DWORD overlay_pixel =
            static_cast<DWORD>(overlay_channel) |
            (static_cast<DWORD>(overlay_channel) << 8u) |
            (static_cast<DWORD>(overlay_channel) << 16u) |
            (static_cast<DWORD>(overlay_alpha) << 24u);
        std::fill_n(pixels, pixel_count, overlay_pixel);

        HDC mask_dc = CreateCompatibleDC(screen_dc);
        void* mask_bits = nullptr;
        HBITMAP mask_dib = mask_dc
            ? CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &mask_bits, nullptr, 0)
            : nullptr;
        if (!mask_dc || !mask_dib || !mask_bits) {
            const DWORD error = GetLastError();
            if (mask_dib) DeleteObject(mask_dib);
            if (mask_dc) DeleteDC(mask_dc);
            DeleteObject(dib);
            DeleteDC(mem_dc);
            ReleaseDC(nullptr, screen_dc);
            fail(error);
            return;
        }

        HGDIOBJ old_bitmap = nullptr, old_mask_bitmap = nullptr, old_font = nullptr;
        HFONT font = nullptr;
        const auto selected = [](HGDIOBJ object) { return object && object != HGDI_ERROR; };
        const auto cleanup = [&] {
            if (selected(old_font)) SelectObject(mask_dc, old_font);
            // Delete the DCs before their objects even if restoring a selection
            // fails. Never leak a still-selected bitmap/font on an error path.
            if (selected(old_mask_bitmap)) SelectObject(mask_dc, old_mask_bitmap);
            if (selected(old_bitmap)) SelectObject(mem_dc, old_bitmap);
            DeleteDC(mask_dc);
            DeleteDC(mem_dc);
            if (font) DeleteObject(font);
            DeleteObject(mask_dib);
            DeleteObject(dib);
            ReleaseDC(nullptr, screen_dc);
        };
        SetLastError(0);
#ifdef WATCHMAN_RENDER_TESTING
        old_bitmap = test_selection_error_ ? (SetLastError(test_selection_error_), nullptr) :
#else
        old_bitmap =
#endif
            SelectObject(mem_dc, dib);
        if (!selected(old_bitmap)) {
            const DWORD error = GetLastError();
            cleanup(); fail(error); return;
        }
        SetLastError(0);
        old_mask_bitmap = SelectObject(mask_dc, mask_dib);
        if (!selected(old_mask_bitmap)) {
            const DWORD error = GetLastError();
            cleanup(); fail(error); return;
        }

        const auto layout = TaskbarWidgetLayout::Get(static_cast<int>(width), static_cast<int>(height), frame.mode, frame.vertical, frame.dpi, frame.taskbar_edge);
        const wchar_t* widget_font = UIStyle::FONT_MONO;
        font = CreateFontW(
            -static_cast<int>(std::round(layout.font_size)), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, widget_font
        );
        if (!font) {
            const DWORD error = GetLastError();
            cleanup(); fail(error); return;
        }
        SetLastError(0);
        old_font = SelectObject(mask_dc, font);
        if (!selected(old_font)) {
            const DWORD error = GetLastError();
            cleanup(); fail(error); return;
        }

        const auto& up_text = frame.upload_text;
        const auto& down_text = frame.download_text;
        const RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

        const COLORREF shadow = light_taskbar ? RGB(255, 255, 255) : RGB(0, 0, 0);
        const COLORREF value_color = light_taskbar ? RGB(38, 49, 58) : RGB(255, 255, 255);
        const COLORREF up_color = light_taskbar ? RGB(209, 63, 131) : RGB(240, 125, 172);
        const COLORREF down_color = light_taskbar ? RGB(0, 120, 212) : RGB(99, 214, 255);
        const COLORREF cpu_color = light_taskbar ? RGB(164, 95, 0) : RGB(243, 182, 74);
        const COLORREF mem_color = light_taskbar ? RGB(22, 122, 56) : RGB(134, 217, 108);

        const auto draw = [&](const std::wstring& text, const TaskbarWidgetLayout::Rect& area, COLORREF color, bool centered = false) {
            if (area.right <= area.left || area.bottom <= area.top) return;
            const RECT rect{area.left, area.top, area.right, area.bottom};
            DrawLayeredWidgetTextWithShadow(mask_dc, static_cast<DWORD*>(mask_bits), pixels, pixel_count, bounds,
                text, rect, (centered ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, color, shadow);
        };
        const auto measure = [&](const std::wstring& text) {
            SIZE extent{};
            return GetTextExtentPoint32W(mask_dc, text.c_str(), static_cast<int>(text.size()), &extent)
                ? static_cast<int>(extent.cx) : INT_MAX;
        };
        const auto draw_rate = [&](const std::wstring& text, const wchar_t* arrow_text,
                                   const TaskbarWidgetLayout::Rect& arrow, const TaskbarWidgetLayout::Rect& value,
                                   const TaskbarWidgetLayout::Rect& unit, COLORREF color) {
            auto combined = value;
            if (!layout.split) combined.right = unit.right;
            const auto fitted = TaskbarWidgetLayout::FitRate(text, layout.split, combined.right - combined.left, measure);
            draw(arrow_text, arrow, color, frame.vertical);
            const UINT alignment = TaskbarWidgetLayout::NetworkAlignment(frame.vertical, frame.taskbar_edge)
                == TaskbarWidgetLayout::TextAlignment::Right ? DT_RIGHT : DT_LEFT;
            const auto draw_aligned = [&](const std::wstring& rate, const TaskbarWidgetLayout::Rect& area) {
                const RECT rect{area.left, area.top, area.right, area.bottom};
                DrawLayeredWidgetTextWithShadow(mask_dc, static_cast<DWORD*>(mask_bits), pixels, pixel_count, bounds,
                    rate, rect, alignment | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, value_color, shadow);
            };
            draw_aligned(fitted.value, fitted.unit.empty() ? combined : value);
            if (!fitted.unit.empty()) draw_aligned(fitted.unit, unit);
        };
        if (layout.show_network) {
            draw_rate(up_text, L"\u2191", layout.upload_arrow, layout.upload_value, layout.upload_unit, up_color);
            draw_rate(down_text, L"\u2193", layout.download_arrow, layout.download_value, layout.download_unit, down_color);
        }
        if (layout.show_system) {
            const auto draw_system = [&](const wchar_t* label, const std::wstring& value,
                                         TaskbarWidgetLayout::Rect label_area, const TaskbarWidgetLayout::Rect& value_area, COLORREF color) {
                if (frame.vertical && !layout.split) {
                    const auto row = TaskbarWidgetLayout::CenterSystemRow(label_area, value_area,
                        measure(label), measure(value));
                    draw(label, row.label, color);
                    draw(value, row.value, color);
                } else {
                    draw(label, label_area, color, frame.vertical);
                    draw(value, value_area, color, frame.vertical);
                }
            };
            draw_system(L"CPU", frame.cpu_text, layout.cpu_label, layout.cpu_value, cpu_color);
            draw_system(L"MEM", frame.memory_text, layout.memory_label, layout.memory_value, mem_color);
        }

        SIZE size{static_cast<LONG>(width), static_cast<LONG>(height)};
        POINT src{0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        // The taskbar owns this child window's position. Updating only the
        // surface avoids a second position change that can hide or shake it.
        SetLastError(0);
#ifdef WATCHMAN_RENDER_TESTING
        const BOOL accepted = test_layered_error_ ? (SetLastError(test_layered_error_), FALSE) :
#else
        const BOOL accepted =
#endif
            UpdateLayeredWindow(hwnd, screen_dc, nullptr, &size, mem_dc, &src, 0, &blend, ULW_ALPHA);
        const DWORD presentation_error = accepted ? ERROR_SUCCESS : GetLastError();

        cleanup();
        finish(WatchmanWidgetRuntime::LayeredResult(accepted != FALSE,
            presentation_error ? presentation_error : ERROR_GEN_FAILURE));
        return;
    }

}

void AppRenderer::RenderFloatingMonitor(HWND hwnd, HDC hdc, const DashboardSnapshot& dash_snap) {
    if (!hwnd || !hdc) return;
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return;

    FloatingMonitorSurface surface(hdc, width, height);
    FloatingMonitorSurface mask(hdc, width, height);
    if (!surface.pixels || !mask.pixels) return;
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::fill_n(mask.pixels, pixel_count, 0u);
    std::fill_n(surface.pixels, pixel_count, 0u);
    const bool transparent = floating_monitor_transparent_background_;
    if (transparent) {
        // A barely visible alpha keeps the entire strip draggable; pure alpha
        // zero would make Windows pass clicks through the gaps between letters.
        std::fill_n(surface.pixels, pixel_count, FloatingMonitorLayout::PremultipliedPixel(0, 0, 0, 1));
    } else {
        const COLORREF background = FloatingMonitorLayout::panel_background_color;
        HGDIOBJ old_brush = SelectObject(mask.dc, GetStockObject(WHITE_BRUSH));
        HGDIOBJ old_pen = SelectObject(mask.dc, GetStockObject(WHITE_PEN));
        RoundRect(mask.dc, 0, 0, width, height, 12, 12);
        GdiFlush();
        SelectObject(mask.dc, old_brush);
        SelectObject(mask.dc, old_pen);
        for (size_t index = 0; index < pixel_count; ++index) {
            const BYTE coverage = static_cast<BYTE>(mask.pixels[index] & 0xFFu);
            surface.pixels[index] = FloatingMonitorLayout::PremultipliedPixel(
                GetRValue(background), GetGValue(background), GetBValue(background),
                (static_cast<unsigned>(coverage) * FloatingMonitorLayout::ClampOpacity(floating_monitor_opacity_) + 50u) / 100u);
        }
    }

    const auto defaults = GetFloatingMonitorDefaultColors();
    const COLORREF primary = floating_monitor_text_color_ == CLR_INVALID ? defaults[0] : floating_monitor_text_color_;
    const COLORREF upload = floating_monitor_upload_color_ == CLR_INVALID ? defaults[1] : floating_monitor_upload_color_;
    const COLORREF download = floating_monitor_download_color_ == CLR_INVALID ? defaults[2] : floating_monitor_download_color_;
    // Grayscale antialiasing into a coverage mask, then tint once. No ClearType
    // color fringes, artificial stroke, or color-dependent contrast halo.
    HFONT font = CreateFontW(-FloatingMonitorLayout::ClampFontSize(floating_monitor_font_size_), 0, 0, 0,
        FloatingMonitorText::Value(floating_monitor_text_weight_), FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        FloatingMonitorText::GdiFamily(floating_monitor_text_weight_));
    if (!font) return;
    HGDIOBJ old_font = SelectObject(mask.dc, font);
    const auto layout = FloatingMonitorLayout::Get(width, height, floating_monitor_font_size_,
        floating_monitor_show_cpu_, floating_monitor_show_memory_);
    const auto draw_text = [&](const std::wstring& text, const FloatingMonitorLayout::Rect& area, COLORREF color, bool align_right) {
        const RECT rect{ area.left, area.top, area.right, area.bottom };
        const UINT flags = (align_right ? DT_RIGHT : DT_LEFT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
        DrawLayeredWidgetText(mask.dc, mask.pixels, surface.pixels, pixel_count, bounds, text, rect, flags, color, 255);
    };
    const auto draw_speed = [&](double bytes_per_second, const FloatingMonitorLayout::Rect& area) {
        if (!WatchmanLiveReadings::NetworkReady(dash_snap)) {
            draw_text(L"--", area, primary, false);
            return;
        }
        std::wstring text = FormatTaskbarSpeed(bytes_per_second, use_bits_per_second_);
        const size_t separator = text.find(L' ');
        const std::wstring unit = text.substr(separator + 1);
        std::wstring number = text.substr(0, separator);
        const auto fits = [&](const std::wstring& value) {
            SIZE extent{};
            return GetTextExtentPoint32W(mask.dc, value.c_str(), static_cast<int>(value.size()), &extent) &&
                extent.cx <= area.right - area.left;
        };
        if (!fits(text)) text = number + unit;
        if (!fits(text)) {
            const size_t decimal = number.find(L'.');
            if (decimal != std::wstring::npos) number.erase(decimal);
            text = number + unit;
        }
        if (fits(text)) {
            draw_text(text, area, primary, false);
        } else {
            // Even an unusually wide font must retain the unit. Only the
            // numeric field may be clipped in this final fallback.
            SIZE extent{};
            GetTextExtentPoint32W(mask.dc, unit.c_str(), static_cast<int>(unit.size()), &extent);
            const int split = std::max(area.left + 12, area.right - static_cast<int>(extent.cx));
            auto numeric_area = area;
            numeric_area.right = split - 2;
            auto unit_area = area;
            unit_area.left = split;
            draw_text(number, numeric_area, primary, false);
            draw_text(unit, unit_area, primary, true);
        }
    };
    draw_text(L"UP", layout.upload_label, upload, false);
    draw_speed(dash_snap.upload_speed_bps / 8.0, layout.upload_value);
    draw_text(L"DN", layout.download_label, download, false);
    draw_speed(dash_snap.download_speed_bps / 8.0, layout.download_value);
    if (floating_monitor_show_cpu_) {
        const COLORREF cpu = floating_monitor_cpu_color_ == CLR_INVALID ? defaults[3] : floating_monitor_cpu_color_;
        draw_text(L"CPU", layout.cpu_label, cpu, false);
        draw_text(FormatCpuUsage(dash_snap), layout.cpu_value, primary, false);
    }
    if (floating_monitor_show_memory_) {
        const COLORREF memory = floating_monitor_memory_color_ == CLR_INVALID ? defaults[4] : floating_monitor_memory_color_;
        draw_text(L"MEM", layout.memory_label, memory, false);
        std::wostringstream text;
        text << std::fixed << std::setprecision(0) << dash_snap.ram_usage_percent << L"%";
        draw_text(WatchmanLiveReadings::MemoryReady(dash_snap, GetTickCount64()) ? text.str() : L"--",
            layout.memory_value, primary, false);
    }
    SelectObject(mask.dc, old_font);
    DeleteObject(font);
#ifdef WATCHMAN_RENDER_TESTING
    // Owned native fixture inspects the actual premultiplied production pixels.
    // This copy and its accessors do not exist in the shipped executable.
    test_floating_pixels_.assign(surface.pixels, surface.pixels + pixel_count);
#endif

    SIZE size{ width, height };
    POINT origin{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    // Panel opacity is baked into its pixels; never fade the text with it.
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SetLastError(0);
    const BOOL accepted = UpdateLayeredWindow(hwnd, nullptr, nullptr, &size, surface.dc, &origin, 0, &blend, ULW_ALPHA);
    const DWORD error = accepted ? ERROR_SUCCESS : GetLastError();
#ifdef WATCHMAN_RENDER_TESTING
    if (accepted) ++test_floating_submissions_;
#endif
    static DWORD previous_error = ERROR_SUCCESS;
    const DWORD result = accepted ? ERROR_SUCCESS : (error ? error : ERROR_GEN_FAILURE);
    if (result != previous_error) {
        WatchmanStartupDiagnostics::Record("floating-graphics", accepted ? "gdi-presented" : "presentation-failed", result);
        previous_error = result;
    }
}

std::wstring AppRenderer::FormatBytes(uint64_t bytes) {
    double kb = bytes / 1024.0;
    double mb = kb / 1024.0;
    double gb = mb / 1024.0;

    std::wstringstream ss;
    ss << std::fixed;
    
    if (gb >= 1.0) {
        ss << std::setprecision(2) << gb << L" GB";
    } else if (mb >= 1.0) {
        ss << std::setprecision(2) << mb << L" MB";
    } else if (kb >= 1.0) {
        ss << std::setprecision(1) << kb << L" KB";
    } else {
        ss << bytes << L" B";
    }
    return ss.str();
}

std::wstring AppRenderer::FormatAppTotalBytes(uint64_t bytes) {
    return AppTrafficPresentation::FormatTotalBytes(bytes);
}

void AppRenderer::DrawCard(D2D1_RECT_F rect, bool hover_highlight, CardSurface surface) {
    if (!render_target_) return;
    (void)hover_highlight;
    const float radius = effective_theme_ == ThemeMode::Light ? 24.0f : UIStyle::CORNER_RADIUS_LARGE;
    const auto rounded = D2D1::RoundedRect(rect, radius, radius);
    ID2D1LinearGradientBrush* warm = surface == CardSurface::Session ? brush_session_card_
        : (surface == CardSurface::Snapshot ? brush_snapshot_card_ : nullptr);
    if (effective_theme_ == ThemeMode::Light && warm) {
        // CSS 135deg: project onto the diagonal, including non-square cards.
        const float half_projection = ((rect.right - rect.left) + (rect.bottom - rect.top)) * 0.25f;
        const auto center = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
        warm->SetStartPoint(D2D1::Point2F(center.x - half_projection, center.y - half_projection));
        warm->SetEndPoint(D2D1::Point2F(center.x + half_projection, center.y + half_projection));
        render_target_->FillRoundedRectangle(rounded, warm);
    } else {
        render_target_->FillRoundedRectangle(rounded, brush_card_bg_);
    }
    render_target_->DrawRoundedRectangle(rounded, brush_card_border_, 1.0f);
}

void AppRenderer::SetWidgetMode(TaskbarWidgetMode mode) {
    widget_mode_ = TaskbarWidgetModes::Normalize(static_cast<int>(mode));
    if (settings_open_) {
        preferences_draft_.widget_mode = widget_mode_;
    }
}

void AppRenderer::LoadSettings() {
    {
        Watchman::ScopedHKey updates;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, WatchmanUpdates::preference_path, 0, KEY_QUERY_VALUE, updates.Put()) == ERROR_SUCCESS)
            update_notice_.Load(WatchmanUpdates::ReadMarker(updates.Get(), WatchmanUpdates::notified_value),
                WatchmanUpdates::ReadMarker(updates.Get(), WatchmanUpdates::dismissed_value));
    }
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Watchman", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwType = REG_DWORD;
        DWORD dwSize = sizeof(DWORD);
        DWORD val = 0;

        if (RegQueryValueExW(hKey, L"Autostart", nullptr, &dwType, reinterpret_cast<BYTE*>(&val), &dwSize) == ERROR_SUCCESS) {
            autostart_enabled_ = (val != 0);
        }
        if (RegQueryValueExW(hKey, L"Alerts", nullptr, &dwType, reinterpret_cast<BYTE*>(&val), &dwSize) == ERROR_SUCCESS) {
            alerts_enabled_ = (val != 0);
        }
        if (RegQueryValueExW(hKey, L"Widget", nullptr, &dwType, reinterpret_cast<BYTE*>(&val), &dwSize) == ERROR_SUCCESS) {
            widget_enabled_ = (val != 0);
        }

        const auto read_dword = [hKey](const wchar_t* name, DWORD& output) {
            DWORD type = REG_DWORD;
            DWORD value = 0;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS ||
                type != REG_DWORD || size != sizeof(value)) return false;
            output = value;
            return true;
        };
        welcome_tour_.LoadSeenVersion(WatchmanWelcome::ReadSeenVersion(hKey));
        widget_mode_ = TaskbarWidgetMode::All;
        if (read_dword(L"WidgetMode", val)) {
            if (val <= static_cast<DWORD>(TaskbarWidgetMode::System)) {
                widget_mode_ = TaskbarWidgetModes::Normalize(static_cast<int>(val));
            }
        } else if (RegQueryValueExW(hKey, L"WidgetMode", nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND &&
                   read_dword(L"WidgetNetworkOnly", val)) {
            widget_mode_ = TaskbarWidgetModes::FromLegacyNetworkOnly(val != 0);
        }
        if (read_dword(L"UnitModeBits", val)) use_bits_per_second_ = val != 0;
        if (read_dword(L"HideHardwareGauges", val)) hide_hardware_gauges_ = val != 0;
        if (read_dword(L"FloatingMonitor", val)) floating_monitor_enabled_ = val != 0;
        // Reuse the original independent visibility keys, preserving explicit
        // choices on upgrade. Missing keys use the new CPU+memory defaults.
        if (read_dword(L"FloatingMonitorCpu", val)) floating_monitor_show_cpu_ = val != 0;
        if (read_dword(L"FloatingMonitorMemory", val)) floating_monitor_show_memory_ = val != 0;
        if (read_dword(L"FloatingMonitorTopmost", val)) floating_monitor_always_on_top_ = val != 0;
        if (read_dword(L"FloatingMonitorLocked", val)) floating_monitor_lock_position_ = val != 0;
        if (read_dword(L"FloatingMonitorClickThrough", val)) floating_monitor_click_through_ = val != 0;
        if (read_dword(L"FloatingMonitorTransparentBackground", val)) floating_monitor_transparent_background_ = val != 0;
        if (read_dword(L"FloatingMonitorOpacity", val)) floating_monitor_opacity_ = FloatingMonitorLayout::ClampOpacity(static_cast<int>(val));
        if (read_dword(L"FloatingMonitorX", val)) floating_monitor_x_ = static_cast<int>(static_cast<int32_t>(val));
        if (read_dword(L"FloatingMonitorY", val)) floating_monitor_y_ = static_cast<int>(static_cast<int32_t>(val));
        // Legacy taskbar color/font overrides are intentionally ignored.
        if (read_dword(L"FloatingTextColor", val)) floating_monitor_text_color_ = static_cast<COLORREF>(val);
        if (read_dword(L"FloatingUploadColor", val)) floating_monitor_upload_color_ = static_cast<COLORREF>(val);
        if (read_dword(L"FloatingDownloadColor", val)) floating_monitor_download_color_ = static_cast<COLORREF>(val);
        if (read_dword(L"FloatingCpuColor", val)) floating_monitor_cpu_color_ = static_cast<COLORREF>(val);
        if (read_dword(L"FloatingMemoryColor", val)) floating_monitor_memory_color_ = static_cast<COLORREF>(val);

        // Ignore old family overrides. Missing weight keys intentionally get the
        // new Light default, without discarding saved size, colors, or behavior.
        if (read_dword(L"FloatingMonitorFontSize", val)) floating_monitor_font_size_ = FloatingMonitorLayout::ClampFontSize(static_cast<int>(val));
        if (read_dword(L"FloatingMonitorTextWeight", val)) floating_monitor_text_weight_ = FloatingMonitorText::FromSaved(static_cast<int>(val));
        DWORD display_type = REG_SZ;
        DWORD display_size = 0;
        if (RegQueryValueExW(hKey, L"FloatingMonitorDisplay", nullptr, &display_type, nullptr, &display_size) == ERROR_SUCCESS &&
            display_type == REG_SZ && display_size >= sizeof(wchar_t)) {
            std::vector<wchar_t> display_device((display_size / sizeof(wchar_t)) + 1, L'\0');
            if (RegQueryValueExW(
                    hKey,
                    L"FloatingMonitorDisplay",
                    nullptr,
                    &display_type,
                    reinterpret_cast<BYTE*>(display_device.data()),
                    &display_size
                ) == ERROR_SUCCESS) {
                floating_monitor_display_device_ = display_device.data();
            }
        }
        if (read_dword(L"WarningTrafficEnabled", val)) traffic_warning_enabled_ = val != 0;
        if (read_dword(L"WarningTrafficThreshold", val)) traffic_warning_threshold_ = static_cast<int>(val);
        if (read_dword(L"WarningTrafficGB", val)) traffic_warning_gigabytes_ = val != 0;
        if (read_dword(L"WarningMemoryEnabled", val)) memory_warning_enabled_ = val != 0;
        if (read_dword(L"WarningMemoryThreshold", val)) memory_warning_threshold_ = std::clamp(static_cast<int>(val), 0, 100);
        if (read_dword(L"WarningCpuTempEnabled", val)) cpu_temp_warning_enabled_ = val != 0;
        if (read_dword(L"WarningCpuTempThreshold", val)) cpu_temp_warning_threshold_ = static_cast<int>(std::clamp<DWORD>(val, 1, 150));
        if (read_dword(L"WarningGpuTempEnabled", val)) gpu_temp_warning_enabled_ = val != 0;
        if (read_dword(L"WarningGpuTempThreshold", val)) gpu_temp_warning_threshold_ = static_cast<int>(std::clamp<DWORD>(val, 1, 150));
        if (read_dword(L"WarningDiskTempEnabled", val)) disk_temp_warning_enabled_ = val != 0;
        if (read_dword(L"WarningDiskTempThreshold", val)) disk_temp_warning_threshold_ = static_cast<int>(std::clamp<DWORD>(val, 1, 150));
        if (read_dword(L"WarningMainboardTempEnabled", val)) mainboard_temp_warning_enabled_ = val != 0;
        if (read_dword(L"WarningMainboardTempThreshold", val)) mainboard_temp_warning_threshold_ = static_cast<int>(std::clamp<DWORD>(val, 1, 150));
        DWORD adapter_type = REG_SZ;
        DWORD adapter_size = 0;
        if (RegQueryValueExW(hKey, L"NetworkAdapterId", nullptr, &adapter_type, nullptr, &adapter_size) == ERROR_SUCCESS &&
            adapter_type == REG_SZ && adapter_size >= sizeof(wchar_t)) {
            std::vector<wchar_t> adapter_id((adapter_size / sizeof(wchar_t)) + 1, L'\0');
            if (RegQueryValueExW(
                    hKey,
                    L"NetworkAdapterId",
                    nullptr,
                    &adapter_type,
                    reinterpret_cast<BYTE*>(adapter_id.data()),
                    &adapter_size
                ) == ERROR_SUCCESS) {
                network_adapter_id_ = adapter_id.data();
            }
        }
        if (read_dword(L"Theme", val)) {
            current_theme_ = UIStyle::NormalizeTheme(static_cast<int>(val));
            effective_theme_ = UIStyle::ResolveTheme(current_theme_, ReadWindowsLightTaskbarTheme());
            palette_ = UIStyle::GetPalette(effective_theme_);
        }
        ULONGLONG data_limit = 0;
        DWORD limit_type = REG_QWORD;
        DWORD limit_size = sizeof(data_limit);
        if (RegQueryValueExW(
                hKey,
                L"DataLimitBytes",
                nullptr,
                &limit_type,
                reinterpret_cast<BYTE*>(&data_limit),
                &limit_size
            ) == ERROR_SUCCESS && limit_type == REG_QWORD) {
            data_limit_bytes_ = data_limit;
        }
        RegCloseKey(hKey);
    }
    saved_preferences_ = SavedPreferences{CapturePreferences(), floating_monitor_x_, floating_monitor_y_};
}

void AppRenderer::SaveSettings() {
    const SavedPreferences current{CapturePreferences(), floating_monitor_x_, floating_monitor_y_};
    if (saved_preferences_ && saved_preferences_->values == current.values &&
        saved_preferences_->x == current.x && saved_preferences_->y == current.y) return;
#ifdef WATCHMAN_STARTUP_RECOVERY_TESTING
    // Owned fake-display/window tests must never modify the real user's HKCU.
    // Normal-EXE desktop smoke separately validates actual settings persistence.
    saved_preferences_ = current;
    ++test_settings_writes_;
    return;
#endif
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Watchman", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        bool saved = true;
        const auto write_value = [&](const wchar_t* name, DWORD type, const BYTE* bytes, DWORD size) {
            const LONG result = RegSetValueExW(hKey, name, 0, type, bytes, size);
            saved = saved && result == ERROR_SUCCESS;
            return result;
        };
        DWORD val = autostart_enabled_ ? 1 : 0;
        write_value(L"Autostart", REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        val = alerts_enabled_ ? 1 : 0;
        write_value(L"Alerts", REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        val = widget_enabled_ ? 1 : 0;
        write_value(L"Widget", REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        val = static_cast<DWORD>(widget_mode_);
        write_value(L"WidgetMode", REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        val = IsWidgetNetworkOnly() ? 1 : 0;
        write_value(L"WidgetNetworkOnly", REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        const auto write_dword = [&](const wchar_t* name, DWORD value) {
            write_value(name, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        };
        write_dword(L"UnitModeBits", use_bits_per_second_ ? 1 : 0);
        write_dword(L"HideHardwareGauges", hide_hardware_gauges_ ? 1 : 0);
        write_dword(L"FloatingMonitor", floating_monitor_enabled_ ? 1 : 0);
        write_dword(L"FloatingMonitorCpu", floating_monitor_show_cpu_ ? 1 : 0);
        write_dword(L"FloatingMonitorMemory", floating_monitor_show_memory_ ? 1 : 0);
        write_dword(L"FloatingMonitorTopmost", floating_monitor_always_on_top_ ? 1 : 0);
        write_dword(L"FloatingMonitorLocked", floating_monitor_lock_position_ ? 1 : 0);
        write_dword(L"FloatingMonitorClickThrough", floating_monitor_click_through_ ? 1 : 0);
        write_dword(L"FloatingMonitorTransparentBackground", floating_monitor_transparent_background_ ? 1 : 0);
        write_dword(L"FloatingMonitorOpacity", static_cast<DWORD>(FloatingMonitorLayout::ClampOpacity(floating_monitor_opacity_)));
        const DWORD display_device_size = static_cast<DWORD>((floating_monitor_display_device_.size() + 1) * sizeof(wchar_t));
        write_value(L"FloatingMonitorDisplay", REG_SZ,
            reinterpret_cast<const BYTE*>(floating_monitor_display_device_.c_str()),
            display_device_size
        );
        write_dword(L"FloatingMonitorX", static_cast<DWORD>(floating_monitor_x_));
        write_dword(L"FloatingMonitorY", static_cast<DWORD>(floating_monitor_y_));

        write_dword(L"FloatingTextColor", static_cast<DWORD>(floating_monitor_text_color_));
        write_dword(L"FloatingUploadColor", static_cast<DWORD>(floating_monitor_upload_color_));
        write_dword(L"FloatingDownloadColor", static_cast<DWORD>(floating_monitor_download_color_));
        write_dword(L"FloatingCpuColor", static_cast<DWORD>(floating_monitor_cpu_color_));
        write_dword(L"FloatingMemoryColor", static_cast<DWORD>(floating_monitor_memory_color_));

        write_dword(L"FloatingMonitorFontSize", static_cast<DWORD>(FloatingMonitorLayout::ClampFontSize(floating_monitor_font_size_)));
        write_dword(L"FloatingMonitorTextWeight", static_cast<DWORD>(FloatingMonitorText::Value(floating_monitor_text_weight_)));
        write_dword(L"WarningTrafficEnabled", traffic_warning_enabled_ ? 1 : 0);
        write_dword(L"WarningTrafficThreshold", static_cast<DWORD>(std::max(0, traffic_warning_threshold_)));
        write_dword(L"WarningTrafficGB", traffic_warning_gigabytes_ ? 1 : 0);
        write_dword(L"WarningMemoryEnabled", memory_warning_enabled_ ? 1 : 0);
        write_dword(L"WarningMemoryThreshold", static_cast<DWORD>(std::clamp(memory_warning_threshold_, 0, 100)));
        write_dword(L"WarningCpuTempEnabled", cpu_temp_warning_enabled_ ? 1 : 0);
        write_dword(L"WarningCpuTempThreshold", static_cast<DWORD>(std::max(0, cpu_temp_warning_threshold_)));
        write_dword(L"WarningGpuTempEnabled", gpu_temp_warning_enabled_ ? 1 : 0);
        write_dword(L"WarningGpuTempThreshold", static_cast<DWORD>(std::max(0, gpu_temp_warning_threshold_)));
        write_dword(L"WarningDiskTempEnabled", disk_temp_warning_enabled_ ? 1 : 0);
        write_dword(L"WarningDiskTempThreshold", static_cast<DWORD>(std::max(0, disk_temp_warning_threshold_)));
        write_dword(L"WarningMainboardTempEnabled", mainboard_temp_warning_enabled_ ? 1 : 0);
        write_dword(L"WarningMainboardTempThreshold", static_cast<DWORD>(std::max(0, mainboard_temp_warning_threshold_)));
        const DWORD adapter_id_size = static_cast<DWORD>((network_adapter_id_.size() + 1) * sizeof(wchar_t));
        write_value(L"NetworkAdapterId", REG_SZ,
            reinterpret_cast<const BYTE*>(network_adapter_id_.c_str()),
            adapter_id_size
        );
        val = static_cast<DWORD>(current_theme_);
        write_value(L"Theme", REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
        const ULONGLONG data_limit = data_limit_bytes_;
        write_value(L"DataLimitBytes", REG_QWORD,
            reinterpret_cast<const BYTE*>(&data_limit),
            sizeof(data_limit)
        );
        RegCloseKey(hKey);
        if (saved) saved_preferences_ = current;
    }
}
