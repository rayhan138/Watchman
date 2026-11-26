#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <array>
#include <climits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
#include "ui_style.h"
#include "settings_text_layout.h"
#include "warnings_settings_layout.h"
#include "welcome_tour.h"
#include "update_policy.h"
#include "update_presentation.h"
#include "floating_monitor_layout.h"
#include "floating_monitor_text.h"
#include "bundled_fonts.h"
#include "taskbar_widget_mode.h"
#include "widget_runtime.h"
#include "widget_recovery_policy.h"
#include "live_readings.h"
#include "runtime_policy.h"
#include "ui_responsiveness.h"
#include "main_graphics_policy.h"
#include "warning_policy.h"
#include "snapshots.h"
#include "metrics.h"
#include "diagnostics.h"
#include "speed_tester.h" // For SpeedTestResult

// Circular rolling buffer for the throughput history chart
struct SpeedSample {
    double download_kbps;
    double upload_kbps;
};

class TaskbarWidgetCompositor;

enum class CardSurface { Default, Session, Snapshot };

enum class ToolsExportPicker {
    None,
    Period,
    Month,
    Year,
};

class AppRenderer {
public:
    AppRenderer();
    ~AppRenderer();

    // Initialize Direct2D resources associated with the window
    HRESULT Initialize(HWND hwnd);
    void Cleanup();
    // Suspend only the main surface; independently rendered widgets stay alive.
    void ReleaseMainWindowResources();

    // Render the Main UI Window
    void Render(HWND hwnd, 
                const DashboardSnapshot& dash_snap, 
                const NetworkSnapshot& net_snap, 
                const AppActivitySnapshot& app_snap,
                const TrafficHistorySnapshot& hist_snap,
                const SpeedTestSnapshot& speed,
                const DiagnosticsSnapshot& diagnostics_snap);

    // Render the borderless Desktop Widget
    void RenderWidget(HWND hwnd, const DashboardSnapshot& dash_snap);
    bool NeedsAnimationFrame() const { return main_animation_active_; }
    bool MainGraphicsUnavailable() const { return FAILED(main_initialization_error_); }
    void AllowWidgetGpuRetry(bool allowed) { widget_gpu_retry_allowed_ = allowed; }
    // Reset host/device state without touching dashboard resources/settings.
    void ResetWidgetHost();
    void InvalidateWidgetFrame();
    const WatchmanWidgetRuntime::PaintResult& GetWidgetPaintResult() const { return widget_paint_result_; }
    void SetRuntimeStatus(const std::wstring& text) { runtime_status_ = text; }
#ifdef WATCHMAN_RENDER_TESTING
    void SetWidgetFaultsForTest(bool composition, DWORD layered_error = 0, DWORD selection_error = 0) {
        test_fail_composition_ = composition;
        test_layered_error_ = layered_error;
        test_selection_error_ = selection_error;
    }
    void SetWidgetTimeForTest(uint64_t now) { test_widget_time_ = now; }
    uint64_t WidgetGpuRetryAtForTest() const { return widget_gpu_retry_.Next(); }
    unsigned CompositionAttemptsForTest() const { return test_composition_attempts_; }
    uint64_t WidgetSubmissionsForTest() const { return test_widget_submissions_; }
    uint64_t FloatingSubmissionsForTest() const { return test_floating_submissions_; }
    const std::vector<DWORD>& FloatingPixelsForTest() const { return test_floating_pixels_; }
    void SetFloatingColorsForTest(COLORREF color) {
        floating_monitor_text_color_ = floating_monitor_upload_color_ = floating_monitor_download_color_ =
            floating_monitor_cpu_color_ = floating_monitor_memory_color_ = color;
    }
    uint64_t MainPaintsForTest() const { return test_main_paints_; }
    uint64_t SettingsWritesForTest() const { return test_settings_writes_; }
    bool MainResourcesInitializedForTest() const { return main_resources_initialized_; }
    void SetMainGraphicsFaultsForTest(HRESULT initialization, HRESULT target = S_OK) {
        test_main_initialization_error_ = initialization; test_main_target_error_ = target;
    }
    void SetMainTimeForTest(uint64_t now) { test_main_time_ = now; }
    void CompleteWelcomeForTest() {
        welcome_tour_.Activate(WatchmanWelcome::Control::Skip);
        welcome_tour_.LoadSeenVersion(WatchmanWelcome::version); // No Registry writes in an owned fixture.
    }
    uint64_t MainGraphicsRetryAtForTest() const { return main_graphics_retry_.Next(); }
    unsigned MainInitializationAttemptsForTest() const { return test_main_initialization_attempts_; }
    unsigned MainTargetAttemptsForTest() const { return test_main_target_attempts_; }
    unsigned MainFallbackPaintsForTest() const { return test_main_fallback_paints_; }
    void FollowSpeedCardForTest() { reveal_speed_test_ = true; }
    bool SpeedCardVisibleForTest() const { return test_speed_card_visible_; }
#endif
    // Render the independent always-available floating monitor surface.
    void RenderFloatingMonitor(HWND hwnd, HDC hdc, const DashboardSnapshot& dash_snap);

    // Resize the render target
    void Resize(UINT width, UINT height);

    // Handle mouse events for hover/click interactions
    void OnMouseMove(int x, int y);
    void OnMouseWheel(short delta);
    void OnLButtonDown(HWND hwnd, int x, int y, class WatchmanEngine& engine);
    bool HandleSettingsNumericKeyDown(WPARAM key);
    bool HandleSettingsNumericCharacter(wchar_t character);
    bool IsTitlebarDragRegion(int x, int y) const;
    bool IsWelcomeTourOpen() const { return welcome_tour_.IsOpen(); }
    void OfferWelcomeTour(bool interactive);
    void OpenWelcomeTour();
    bool HandleWelcomeTourKeyDown(WPARAM key, bool shift);

    void SetUpdateResult(const WatchmanUpdates::Result& result);
    const WatchmanUpdates::Notice& Updates() const { return update_notice_; }
    void MarkUpdateNotified();
    void OfferUpdatePrompt(bool interactive, bool force = false);
    bool IsUpdatePromptOpen() const { return update_prompt_open_; }
    bool HandleUpdatePromptKeyDown(HWND hwnd, WPARAM key, bool shift);
    bool TakeUpdateCheckRequest() { return std::exchange(update_check_requested_, false); }
    void SetUpdateActionStatus(const std::wstring& status) { update_action_status_ = status; }


    // Set theme mode and update palette brushes
    void SetTheme(ThemeMode mode);
    ThemeMode GetTheme() const { return current_theme_; }
    ThemeMode GetEffectiveTheme() const { return effective_theme_; }

    // Toggle states for settings
    bool IsAutostartEnabled() const { return autostart_enabled_; }
    void SetAutostartEnabled(bool enabled) {
        autostart_enabled_ = enabled;
        preferences_draft_.autostart_enabled = enabled;
    }
    bool IsAlertsEnabled() const { return alerts_enabled_; }
    WatchmanWarnings::Preferences GetWarningPreferences() const {
        WatchmanWarnings::Preferences preferences{alerts_enabled_, traffic_warning_enabled_, traffic_warning_threshold_,
            traffic_warning_gigabytes_, memory_warning_enabled_, memory_warning_threshold_};
        preferences.temperatures = {{{cpu_temp_warning_enabled_, cpu_temp_warning_threshold_},
            {gpu_temp_warning_enabled_, gpu_temp_warning_threshold_},
            {disk_temp_warning_enabled_, disk_temp_warning_threshold_},
            {mainboard_temp_warning_enabled_, mainboard_temp_warning_threshold_}}};
        return preferences;
    }
    // Windows delivers a rapid second press as WM_LBUTTONDBLCLK. Only the
    // theme shortcut and repeatable +/- controls opt in to treating it as a press.
    bool IsRepeatablePrimaryControl(int x, int y) const;
    bool IsUseBitsPerSecond() const { return use_bits_per_second_; }
    bool AreHardwareGaugesHidden() const { return hide_hardware_gauges_; }
    bool IsWidgetEnabled() const { return widget_enabled_; }
    void SetWidgetEnabled(bool enable) { widget_enabled_ = enable; }
    TaskbarWidgetMode GetWidgetMode() const { return widget_mode_; }
    void SetWidgetMode(TaskbarWidgetMode mode);
    bool IsWidgetNetworkOnly() const { return widget_mode_ == TaskbarWidgetMode::Network; }
    void SetWidgetNetworkOnly(bool network_only) { SetWidgetMode(TaskbarWidgetModes::FromLegacyNetworkOnly(network_only)); }
    const std::wstring& GetNetworkAdapterId() const { return network_adapter_id_; }
    void SetNetworkAdapterId(const std::wstring& adapter_id);
    bool IsSettingsOpen() const { return settings_open_; }
    void DismissSettings() {
        HideSettingsEditors();
        settings_open_ = false;
        speed_consent_open_ = false;
        speed_details_open_ = false;
        network_adapter_picker_requested_ = false;
    }
    bool IsFloatingMonitorEnabled() const { return floating_monitor_enabled_; }
    void SetFloatingMonitorEnabled(bool enable);
    bool IsFloatingMonitorCpuShown() const { return floating_monitor_show_cpu_; }
    bool IsFloatingMonitorMemoryShown() const { return floating_monitor_show_memory_; }
    void SetFloatingMonitorCpuShown(bool show);
    void SetFloatingMonitorMemoryShown(bool show);
    bool IsFloatingMonitorAlwaysOnTop() const { return floating_monitor_always_on_top_; }
    bool IsFloatingMonitorPositionLocked() const { return floating_monitor_lock_position_; }
    bool IsFloatingMonitorClickThrough() const { return floating_monitor_click_through_; }
    int GetFloatingMonitorOpacity() const { return floating_monitor_opacity_; }
    int GetFloatingMonitorFontSize() const { return floating_monitor_font_size_; }
    void SetFloatingMonitorFontSize(int size);
    FloatingMonitorText::Weight GetFloatingMonitorTextWeight() const { return floating_monitor_text_weight_; }
    void SetFloatingMonitorTextWeight(FloatingMonitorText::Weight weight);
    bool IsFloatingMonitorBackgroundTransparent() const { return floating_monitor_transparent_background_; }
    void SetFloatingMonitorPositionLocked(bool locked);
    void SetFloatingMonitorAlwaysOnTop(bool enabled);
    void SetFloatingMonitorClickThrough(bool enabled);
    void SetFloatingMonitorBackgroundTransparent(bool transparent);
    void SetFloatingMonitorOpacity(int opacity);
    void OpenFloatingMonitorSettings();
    const std::wstring& GetFloatingMonitorDisplayDevice() const {
        return settings_open_ ? preferences_draft_.floating_monitor_display_device : floating_monitor_display_device_;
    }
    void SetFloatingMonitorDisplayDevice(const std::wstring& display_device);
    const std::wstring& GetCommittedFloatingMonitorDisplayDevice() const { return floating_monitor_display_device_; }
    int GetFloatingMonitorX() const { return floating_monitor_x_; }
    int GetFloatingMonitorY() const { return floating_monitor_y_; }
    void SetFloatingMonitorPosition(int x, int y) { floating_monitor_x_ = x; floating_monitor_y_ = y; }
    bool TakeFloatingMonitorResetPositionRequest() {
        const bool requested = floating_monitor_reset_position_requested_;
        floating_monitor_reset_position_requested_ = false;
        return requested;
    }
    bool TakePreferencesApplyRequest() {
        const bool requested = preferences_apply_requested_;
        preferences_apply_requested_ = false;
        return requested;
    }
    bool TakeFloatingMonitorDisplayPickerRequest() {
        const bool requested = floating_monitor_display_picker_requested_;
        floating_monitor_display_picker_requested_ = false;
        return requested;
    }
    bool TakeNetworkAdapterPickerRequest() {
        const bool requested = network_adapter_picker_requested_;
        network_adapter_picker_requested_ = false;
        return requested;
    }
    bool TakeToolsExportPickerRequest(ToolsExportPicker& request) {
        request = tools_export_picker_request_;
        tools_export_picker_request_ = ToolsExportPicker::None;
        return request != ToolsExportPicker::None;
    }
    int GetToolsExportPeriodIndex() const { return tools_export_period_index_; }
    void SetToolsExportPeriodIndex(int index);
    const std::wstring& GetToolsExportMonth() const { return tools_export_month_; }
    void SetToolsExportMonth(const std::wstring& month) { tools_export_month_ = month; }
    int GetToolsExportYear() const { return tools_export_year_; }
    void SetToolsExportYear(int year) { tools_export_year_ = year; }
    ActiveTab GetActiveTab() const { return active_tab_; }
    void SetActiveTab(ActiveTab tab) {
        // Opening Data always starts at Last 7 Days, even if Data is already open.
        SetDataRangeIndex(WatchmanRuntime::DataRangeOnTabOpen(tab == ActiveTab::Data, data_range_idx_));
        if (active_tab_ != tab) {
            if (tab != ActiveTab::Network) reveal_speed_test_ = false;
            active_tab_ = tab;
            RestartContentAnimation();
            if (tab == ActiveTab::Data) {
                RestartDataChartAnimation();
            } else {
                SetDataPickerOpen(false);
                ResetDataChartTooltip();
                if (data_limit_edit_) {
                    ShowWindow(data_limit_edit_, SW_HIDE);
                }
            }
        }
    }
    void SetDataRangeIndex(int index);

    // Load/Save settings persistence
    void LoadSettings();
    void SaveSettings();

private:
    bool main_resources_initialized_ = false;
    HRESULT main_initialization_error_ = S_OK;
    WatchmanUi::MainGraphicsRetry main_graphics_retry_;
    bool main_editor_suspended_ = false;
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget* render_target_ = nullptr;
    ID2D1HwndRenderTarget* widget_render_target_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    WatchmanFonts::Collection bundled_fonts_;
    ID2D1Bitmap* titlebar_logo_bitmap_ = nullptr;

    // Text formats
    IDWriteTextFormat* format_title_ = nullptr;
    IDWriteTextFormat* format_sidebar_ = nullptr;
    IDWriteTextFormat* format_tab_ = nullptr;
    IDWriteTextFormat* format_body_ = nullptr;
    IDWriteTextFormat* format_body_bold_ = nullptr;
    IDWriteTextFormat* format_titlebar_title_ = nullptr;
    IDWriteTextFormat* format_titlebar_button_ = nullptr;
    IDWriteTextFormat* format_status_ = nullptr;
    IDWriteTextFormat* format_status_mono_ = nullptr;
    IDWriteTextFormat* format_update_footer_ = nullptr;
    IDWriteTextFormat* format_update_version_ = nullptr;
    IDWriteTextFormat* format_values_ = nullptr;
    IDWriteTextFormat* format_mono_ = nullptr;
    IDWriteTextFormat* format_chart_tick_ = nullptr;
    IDWriteTextFormat* format_data_label_ = nullptr;
    IDWriteTextFormat* format_dashboard_label_ = nullptr;
    IDWriteTextFormat* format_dashboard_note_ = nullptr;
    IDWriteTextFormat* format_theme_choice_ = nullptr;
    IDWriteTextFormat* format_welcome_title_ = nullptr;
    IDWriteTextFormat* format_welcome_button_ = nullptr;
    IDWriteTextFormat* format_floating_preview_ = nullptr;
    IDWriteTextFormat* format_dashboard_value_ = nullptr;
    IDWriteTextFormat* format_settings_version_ = nullptr;
    IDWriteTextLayout* settings_version_layout_ = nullptr;
    float settings_version_text_width_ = 0;
    float settings_title_text_width_ = 0;
    IDWriteTextFormat* format_dashboard_gauge_ = nullptr;
    IDWriteTextFormat* format_data_period_ = nullptr;
    IDWriteTextFormat* format_data_filter_ = nullptr;
    IDWriteTextFormat* format_data_range_ = nullptr;
    IDWriteTextFormat* format_picker_title_ = nullptr;
    IDWriteTextFormat* format_picker_subtitle_ = nullptr;
    IDWriteTextFormat* format_picker_range_ = nullptr;
    IDWriteTextFormat* format_picker_year_ = nullptr;
    IDWriteTextFormat* format_picker_month_ = nullptr;
    IDWriteTextFormat* format_picker_section_ = nullptr;
    IDWriteTextFormat* format_picker_nav_ = nullptr;
    IDWriteTextFormat* format_comparison_label_ = nullptr;
    IDWriteTextFormat* format_comparison_detail_ = nullptr;
    IDWriteTextFormat* format_limit_status_ = nullptr;
    IDWriteTextFormat* format_limit_pct_ = nullptr;
    IDWriteTextFormat* format_limit_input_ = nullptr;
    IDWriteTextFormat* format_limit_button_ = nullptr;
    IDWriteTextFormat* format_tooltip_title_ = nullptr;
    IDWriteTextFormat* format_tooltip_body_ = nullptr;
    IDWriteTextFormat* format_app_header_ = nullptr;
    IDWriteTextFormat* format_app_subtitle_ = nullptr;
    IDWriteTextFormat* format_app_label_ = nullptr;
    IDWriteTextFormat* format_app_body_ = nullptr;
    IDWriteTextFormat* format_app_body_bold_ = nullptr;
    IDWriteTextFormat* format_app_icon_ = nullptr;
    IDWriteTextFormat* format_app_mono_ = nullptr;
    IDWriteTextFormat* format_app_badge_ = nullptr;
    IDWriteTextFormat* format_app_conns_ = nullptr;
    // The Network tab has its own type scale in the Tauri app. Keeping these
    // separate prevents its compact diagnostics layout from inheriting the
    // smaller generic dashboard formats.
    IDWriteTextFormat* format_network_quality_ = nullptr;
    IDWriteTextFormat* format_network_quality_sub_ = nullptr;
    IDWriteTextFormat* format_network_metric_label_ = nullptr;
    IDWriteTextFormat* format_network_metric_value_ = nullptr;
    IDWriteTextFormat* format_network_metric_detail_ = nullptr;
    IDWriteTextFormat* format_network_row_label_ = nullptr;
    IDWriteTextFormat* format_network_row_value_ = nullptr;
    IDWriteTextFormat* format_network_signal_label_ = nullptr;
    IDWriteTextFormat* format_network_speed_label_ = nullptr;
    IDWriteTextFormat* format_network_speed_value_ = nullptr;
    IDWriteTextFormat* format_large_speed_ = nullptr;
    IDWriteTextFormat* format_unit_speed_ = nullptr; // Smaller font for MB/s or KB/s
    IDWriteTextFormat* format_speed_arrow_ = nullptr;
    IDWriteTextFormat* format_widget_ = nullptr;

    // Brushes
    ID2D1SolidColorBrush* brush_background_ = nullptr;
    ID2D1SolidColorBrush* brush_sidebar_bg_ = nullptr;
    ID2D1SolidColorBrush* brush_card_bg_ = nullptr;
        ID2D1LinearGradientBrush* brush_session_card_ = nullptr;
        ID2D1LinearGradientBrush* brush_snapshot_card_ = nullptr;
    ID2D1SolidColorBrush* brush_card_border_ = nullptr;
    ID2D1SolidColorBrush* brush_text_primary_ = nullptr;
    ID2D1SolidColorBrush* brush_text_secondary_ = nullptr;
    ID2D1SolidColorBrush* brush_accent_dl_ = nullptr;
    ID2D1SolidColorBrush* brush_accent_ul_ = nullptr;
    ID2D1SolidColorBrush* brush_accent_sys_ = nullptr;
    ID2D1SolidColorBrush* brush_hover_ = nullptr;
    ID2D1SolidColorBrush* brush_active_ = nullptr;
    ID2D1SolidColorBrush* brush_popup_bg_ = nullptr;
    ID2D1SolidColorBrush* brush_popup_shadow_ = nullptr;
    ID2D1SolidColorBrush* brush_picker_month_bg_ = nullptr;
    ID2D1SolidColorBrush* brush_picker_month_border_ = nullptr;
    ID2D1SolidColorBrush* brush_tooltip_bg_ = nullptr;
    ID2D1SolidColorBrush* brush_tooltip_border_ = nullptr;
    ID2D1SolidColorBrush* brush_tooltip_text_ = nullptr;
    ID2D1SolidColorBrush* brush_success_ = nullptr;
    ID2D1SolidColorBrush* brush_data_muted_ = nullptr;
    ID2D1SolidColorBrush* brush_limit_normal_ = nullptr;
    ID2D1SolidColorBrush* brush_limit_warning_ = nullptr;
    ID2D1SolidColorBrush* brush_limit_danger_ = nullptr;
    ID2D1SolidColorBrush* brush_limit_track_ = nullptr;
    ID2D1SolidColorBrush* brush_active_text_ = nullptr;
    ID2D1SolidColorBrush* brush_action_bg_ = nullptr;
    ID2D1SolidColorBrush* brush_chart_download_ = nullptr;
    ID2D1SolidColorBrush* brush_chart_upload_ = nullptr;
    ID2D1SolidColorBrush* brush_chart_grid_ = nullptr;
    ID2D1SolidColorBrush* brush_chart_tick_ = nullptr;
    ID2D1GradientStopCollection* app_gradient_stops_ = nullptr;
    ID2D1LinearGradientBrush* app_gradient_brush_ = nullptr;
    ID2D1GradientStopCollection* app_pink_glow_stops_ = nullptr;
    ID2D1GradientStopCollection* app_yellow_glow_stops_ = nullptr;
    ID2D1GradientStopCollection* app_peach_glow_stops_ = nullptr;
    ID2D1RadialGradientBrush* app_pink_glow_brush_ = nullptr;
    ID2D1RadialGradientBrush* app_yellow_glow_brush_ = nullptr;
    ID2D1RadialGradientBrush* app_peach_glow_brush_ = nullptr;

    struct DataChartHitbox {
        D2D1_RECT_F rect{};
        std::wstring key;
        uint64_t download = 0;
        uint64_t upload = 0;
    };

    // UI state
    ThemeMode current_theme_ = ThemeMode::Light; // Persisted user choice.
    ThemeMode effective_theme_ = ThemeMode::Light; // Resolved colors, including System.
    ActiveTab active_tab_ = ActiveTab::Dashboard;
    UIStyle::ColorPalette palette_;
    
    // UI layout/toggles
    WatchmanUpdates::Notice update_notice_;
    bool update_prompt_open_ = false, update_prompt_requested_ = false;
    bool update_check_requested_ = false, update_footer_visible_ = false;
    int update_focus_ = 1; // close, download, later
    std::wstring update_action_status_, update_download_error_;
    WatchmanWelcome::Tour welcome_tour_;
    bool welcome_keyboard_focus_ = false;
    float welcome_scroll_ = 0, welcome_scroll_max_ = 0;
    float welcome_text_width_ = -1;
    int welcome_text_page_ = -1;
    std::array<SettingsText::Paragraph, WatchmanWelcome::text_count> welcome_text_;
    bool settings_open_ = false;
    bool speed_consent_open_ = false;
    bool speed_details_open_ = false;
    bool reveal_speed_test_ = false;
    float speed_details_scroll_ = 0.0f;
    float speed_details_scroll_max_ = 0.0f;
    SpeedTestResult speed_details_result_{};
    int settings_section_ = 0; // 0 = General, 1 = Widgets, 2 = Warnings, 3 = Floating Monitor
    float settings_general_scroll_offset_ = 0.0f;
    float settings_general_scroll_max_ = 0.0f;
    float settings_widgets_scroll_offset_ = 0.0f;
    float settings_widgets_scroll_max_ = 0.0f;
    float settings_warnings_scroll_offset_ = 0.0f;
    float settings_warnings_scroll_max_ = 0.0f;
    float settings_warning_text_width_ = -1.0f;
    SettingsText::Paragraph settings_sensor_setup_text_;
    SettingsText::Paragraph settings_warning_details_text_;
    float settings_appearance_scroll_offset_ = 0.0f;
    float settings_appearance_scroll_max_ = 0.0f;
    int dashboard_usage_period_idx_ = 0; // 0 = Today, 1 = This Week, 2 = This Month
    bool dashboard_network_details_open_ = false;
    bool dashboard_system_info_open_ = false;
    int data_usage_period_idx_ = 0; // 0 = Daily, 1 = Weekly, 2 = Monthly
    int data_range_idx_ = WatchmanRuntime::default_data_range_index; // 0 = Today, 1 = Last 7 Days, 2 = Monthly, 3 = Yearly
    int data_history_year_ = 0;
    int data_history_month_ = 0;
    uint64_t data_limit_bytes_ = 0;
    bool data_picker_open_ = false;
    bool data_picker_animating_ = false;
    float data_picker_visibility_ = 0.0f;
    float data_picker_animation_from_ = 0.0f;
    ULONGLONG data_picker_animation_started_ms_ = 0;
    int data_picker_page_ = 0;
    HWND data_limit_edit_ = nullptr;

    // Interactions
    int mouse_x_ = -1;
    int mouse_y_ = -1;
    std::vector<DataChartHitbox> data_chart_hitboxes_;
    int hovered_data_chart_hitbox_ = -1;
    int displayed_data_chart_hitbox_ = -1;
    float data_tooltip_x_ = 0.0f;
    float data_tooltip_y_ = 0.0f;
    bool data_tooltip_position_ready_ = false;
    ULONGLONG data_tooltip_last_update_ms_ = 0;
    ULONGLONG data_tooltip_last_hover_ms_ = 0;
    int hovered_tab_item_ = -1;
    int hovered_titlebar_btn_ = -1; // 0=Feedback, 1=Globe, 2=Gear, 3=Theme, 4=Minimize, 5=Close
    bool hover_export_btn_ = false;
    bool hover_settings_close_ = false;

    enum class SettingsNumericField {
        None,
        MonthlyDataLimit,
        TrafficWarning,
        MemoryWarning,
        CpuTemperatureWarning,
        GpuTemperatureWarning,
        DiskTemperatureWarning,
        MainboardTemperatureWarning,
    };
    SettingsNumericField settings_numeric_field_ = SettingsNumericField::None;
    std::wstring settings_numeric_text_;
    bool settings_numeric_select_all_ = false;

    // Persisted preference state. The settings dialog edits a separate draft
    // so Save, Undo, and Use Recommended behave like the Tauri app.
    bool autostart_enabled_ = true;
    bool alerts_enabled_ = true;
    bool use_bits_per_second_ = false;
    bool hide_hardware_gauges_ = false;
    bool widget_enabled_ = true;
    TaskbarWidgetMode widget_mode_ = TaskbarWidgetMode::All;
    bool floating_monitor_enabled_ = false;
    bool floating_monitor_show_cpu_ = FloatingMonitorLayout::default_show_cpu;
    bool floating_monitor_show_memory_ = FloatingMonitorLayout::default_show_memory;
    bool floating_monitor_always_on_top_ = true;
    bool floating_monitor_lock_position_ = false;
    bool floating_monitor_click_through_ = false;
    bool floating_monitor_transparent_background_ = false;
    int floating_monitor_opacity_ = 92;
    std::wstring floating_monitor_display_device_;

    COLORREF floating_monitor_text_color_ = CLR_INVALID;
    COLORREF floating_monitor_upload_color_ = CLR_INVALID;
    COLORREF floating_monitor_download_color_ = CLR_INVALID;
    COLORREF floating_monitor_cpu_color_ = CLR_INVALID;
    COLORREF floating_monitor_memory_color_ = CLR_INVALID;

    int floating_monitor_font_size_ = FloatingMonitorLayout::default_font_size;
    FloatingMonitorText::Weight floating_monitor_text_weight_ = FloatingMonitorText::default_weight;
    int floating_monitor_x_ = INT_MIN;
    int floating_monitor_y_ = INT_MIN;

    struct PreferencesDraft {
        bool autostart_enabled = true;
        bool use_bits_per_second = false;
        bool hide_hardware_gauges = false;
        ThemeMode theme = ThemeMode::Light;
        uint64_t monthly_data_limit_bytes = 0;
        bool alerts_enabled = true;
        bool traffic_warning_enabled = false;
        int traffic_warning_threshold = 500;
        bool traffic_warning_gigabytes = false;
        bool memory_warning_enabled = true;
        int memory_warning_threshold = 90;
        bool cpu_temp_warning_enabled = true;
        int cpu_temp_warning_threshold = 80;
        bool gpu_temp_warning_enabled = true;
        int gpu_temp_warning_threshold = 80;
        bool disk_temp_warning_enabled = true;
        int disk_temp_warning_threshold = 80;
        bool mainboard_temp_warning_enabled = true;
        int mainboard_temp_warning_threshold = 80;
        bool widget_enabled = true;
        TaskbarWidgetMode widget_mode = TaskbarWidgetMode::All;
        std::wstring network_adapter_id;
        bool floating_monitor_enabled = false;
        bool floating_monitor_show_cpu = FloatingMonitorLayout::default_show_cpu;
        bool floating_monitor_show_memory = FloatingMonitorLayout::default_show_memory;
        bool floating_monitor_always_on_top = true;
        bool floating_monitor_lock_position = false;
        bool floating_monitor_click_through = false;
        bool floating_monitor_transparent_background = false;
        int floating_monitor_opacity = 92;
        std::wstring floating_monitor_display_device;

        COLORREF floating_monitor_text_color = CLR_INVALID;
        COLORREF floating_monitor_upload_color = CLR_INVALID;
        COLORREF floating_monitor_download_color = CLR_INVALID;
        COLORREF floating_monitor_cpu_color = CLR_INVALID;
        COLORREF floating_monitor_memory_color = CLR_INVALID;

        int floating_monitor_font_size = FloatingMonitorLayout::default_font_size;
        FloatingMonitorText::Weight floating_monitor_text_weight = FloatingMonitorText::default_weight;
        friend bool operator==(const PreferencesDraft& a, const PreferencesDraft& b) {
            return std::tie(a.autostart_enabled, a.use_bits_per_second, a.hide_hardware_gauges, a.theme,
                a.monthly_data_limit_bytes, a.alerts_enabled, a.traffic_warning_enabled, a.traffic_warning_threshold,
                a.traffic_warning_gigabytes, a.memory_warning_enabled, a.memory_warning_threshold, a.cpu_temp_warning_enabled,
                a.cpu_temp_warning_threshold, a.gpu_temp_warning_enabled, a.gpu_temp_warning_threshold, a.disk_temp_warning_enabled,
                a.disk_temp_warning_threshold, a.mainboard_temp_warning_enabled, a.mainboard_temp_warning_threshold, a.widget_enabled,
                a.widget_mode, a.network_adapter_id, a.floating_monitor_enabled, a.floating_monitor_show_cpu, a.floating_monitor_show_memory, a.floating_monitor_always_on_top,
                a.floating_monitor_lock_position, a.floating_monitor_click_through, a.floating_monitor_transparent_background, a.floating_monitor_opacity,
                a.floating_monitor_display_device, a.floating_monitor_text_color, a.floating_monitor_upload_color, a.floating_monitor_download_color,
                a.floating_monitor_cpu_color, a.floating_monitor_memory_color,
                a.floating_monitor_font_size, a.floating_monitor_text_weight) ==
                   std::tie(b.autostart_enabled, b.use_bits_per_second, b.hide_hardware_gauges, b.theme,
                b.monthly_data_limit_bytes, b.alerts_enabled, b.traffic_warning_enabled, b.traffic_warning_threshold,
                b.traffic_warning_gigabytes, b.memory_warning_enabled, b.memory_warning_threshold, b.cpu_temp_warning_enabled,
                b.cpu_temp_warning_threshold, b.gpu_temp_warning_enabled, b.gpu_temp_warning_threshold, b.disk_temp_warning_enabled,
                b.disk_temp_warning_threshold, b.mainboard_temp_warning_enabled, b.mainboard_temp_warning_threshold, b.widget_enabled,
                b.widget_mode, b.network_adapter_id, b.floating_monitor_enabled, b.floating_monitor_show_cpu, b.floating_monitor_show_memory, b.floating_monitor_always_on_top,
                b.floating_monitor_lock_position, b.floating_monitor_click_through, b.floating_monitor_transparent_background, b.floating_monitor_opacity,
                b.floating_monitor_display_device, b.floating_monitor_text_color, b.floating_monitor_upload_color, b.floating_monitor_download_color,
                b.floating_monitor_cpu_color, b.floating_monitor_memory_color,
                b.floating_monitor_font_size, b.floating_monitor_text_weight);
        }
    };
    PreferencesDraft preferences_draft_{};
    struct SavedPreferences { PreferencesDraft values; int x, y; };
    std::optional<SavedPreferences> saved_preferences_;
    bool preferences_dirty_ = false;
    bool preferences_apply_requested_ = false;
    bool floating_monitor_reset_position_requested_ = false;
    bool floating_monitor_display_picker_requested_ = false;

    bool traffic_warning_enabled_ = false;
    int traffic_warning_threshold_ = 500;
    bool traffic_warning_gigabytes_ = false;
    bool memory_warning_enabled_ = true;
    int memory_warning_threshold_ = 90;
    bool cpu_temp_warning_enabled_ = true;
    int cpu_temp_warning_threshold_ = 80;
    bool gpu_temp_warning_enabled_ = true;
    int gpu_temp_warning_threshold_ = 80;
    bool disk_temp_warning_enabled_ = true;
    int disk_temp_warning_threshold_ = 80;
    bool mainboard_temp_warning_enabled_ = true;
    int mainboard_temp_warning_threshold_ = 80;
    std::wstring network_adapter_id_;
    std::vector<InterfaceInfo> settings_network_adapters_;
    bool network_adapter_picker_requested_ = false;
    int tools_export_period_index_ = 2; // Daily, weekly, monthly, yearly
    std::wstring tools_export_month_;
    int tools_export_year_ = 0;
    ToolsExportPicker tools_export_picker_request_ = ToolsExportPicker::None;
    std::wstring tools_export_status_;
    bool tools_export_status_success_ = false;

    // Graph data (last 60 seconds)
    std::vector<SpeedSample> speed_history_;
    WatchmanLiveReadings::SampleGate chart_sample_gate_;
    bool main_animation_active_ = true;
    WatchmanUi::FrameClock animation_clock_;
    float animation_alpha_ = 1.0f;
    
    // Smooth Animation states (interpolated variables)
    float anim_dl_speed_ = 0.0f;
    float anim_ul_speed_ = 0.0f;
    float anim_cpu_usage_ = 0.0f;
    float anim_ram_usage_ = 0.0f;
    float anim_gpu_usage_ = 0.0f;
    float anim_disk_usage_ = 0.0f;
    float anim_tab_offset_x_ = 0.0f;      // Smooth sliding navigation indicator
    float anim_tab_target_x_ = 0.0f;
    float anim_content_opacity_ = 1.0f;   // Fade in transition for content
    float anim_content_translate_y_ = 0.0f;
    ULONGLONG content_animation_started_ms_ = 0;
    float data_chart_progress_ = 1.0f;
    float data_limit_progress_ = 0.0f;
    bool data_limit_animating_ = false;
    ULONGLONG data_chart_animation_started_ms_ = 0;
    D2D1_SIZE_F render_size_ = { UIStyle::WINDOW_WIDTH, UIStyle::WINDOW_HEIGHT };
    float content_scroll_offsets_[5]{};
    float content_bottoms_[5]{};

    std::unique_ptr<TaskbarWidgetCompositor> widget_compositor_;
    bool widget_composition_failed_ = false;
    bool widget_gpu_retry_allowed_ = true;
    WatchmanWidgetRuntime::GpuRetryPolicy widget_gpu_retry_;
    WatchmanUi::WidgetFirstFrame widget_first_frame_;
    WatchmanWidgetRuntime::PaintResult widget_paint_result_{};
    std::wstring runtime_status_;
#ifdef WATCHMAN_RENDER_TESTING
    bool test_fail_composition_ = false;
    DWORD test_layered_error_ = 0;
    DWORD test_selection_error_ = 0;
    unsigned test_composition_attempts_ = 0;
    uint64_t test_widget_submissions_ = 0;
    uint64_t test_widget_time_ = 0;
    uint64_t test_floating_submissions_ = 0, test_main_paints_ = 0, test_settings_writes_ = 0;
    std::vector<DWORD> test_floating_pixels_;
    bool test_speed_card_visible_ = false;
    HRESULT test_main_initialization_error_ = S_OK, test_main_target_error_ = S_OK;
    uint64_t test_main_time_ = 0;
    unsigned test_main_initialization_attempts_ = 0, test_main_target_attempts_ = 0, test_main_fallback_paints_ = 0;
#endif

    uint64_t MainGraphicsTime() const;
    void ReleaseMainDeviceIndependentResources();
    void DrawMainGraphicsFallback(HWND hwnd);
    HRESULT CreateDeviceIndependentResources();
    HRESULT CreateDeviceResources(HWND hwnd);
    void UpdatePaletteBrushes();
    void DrawAppBackground(D2D1_SIZE_F rt_size);
    float ContentViewportTop() const;
    float ContentViewportBottom() const;
    float CurrentContentScroll() const;
    float ContentScrollMax(ActiveTab tab) const;
    void ClampContentScroll(ActiveTab tab);
    void ReportContentBottom(float bottom);
    void DrawContentScrollbar(D2D1_SIZE_F rt_size);
    void UpdateDataChartHover();
    void ResetDataChartTooltip();
    void ResetDataHistoryToCurrentCalendar();

    // Specific screen renderers
    void DrawTitlebar(D2D1_SIZE_F rt_size);
    void DrawTabbar(D2D1_SIZE_F rt_size);
    void DrawStatusbar(D2D1_SIZE_F rt_size, const NetworkSnapshot& net_snap, const DashboardSnapshot& dash_snap);
    void ReleaseDeviceResources();

    void DrawDashboard(D2D1_SIZE_F rt_size, const DashboardSnapshot& dash_snap, const NetworkSnapshot& net_snap, const TrafficHistorySnapshot& hist_snap);
    void DrawData(D2D1_SIZE_F rt_size, const TrafficHistorySnapshot& hist_snap);
    void DrawAppMonitor(D2D1_SIZE_F rt_size, const AppActivitySnapshot& app_snap);
    void DrawNetworkHealth(D2D1_SIZE_F rt_size, const NetworkSnapshot& net_snap, const DashboardSnapshot& dash_snap, const SpeedTestSnapshot& speed);
    void DrawTools(
        D2D1_SIZE_F rt_size,
        const TrafficHistorySnapshot& hist_snap,
        const DiagnosticsSnapshot& diagnostics_snap
    );
    void DrawSettingsModal(D2D1_SIZE_F rt_size, const DashboardSnapshot& dash_snap);
    WarningsSettingsLayout GetMeasuredWarningsLayout(float left, float right, float top, float scroll = 0);
    void DrawSpeedDialog(D2D1_SIZE_F rt_size);
    void DrawUpdatePrompt(D2D1_SIZE_F rt_size);
    void DismissUpdatePrompt();
    void OpenUpdateDownload(HWND hwnd);
    void RememberUpdateMarker(const wchar_t* name);
    void DrawWelcomeTour(D2D1_SIZE_F rt_size);
    void DrawWelcomeMonitorPreviews(const WatchmanWelcome::Rect& area);
    void ActivateWelcomeControl(WatchmanWelcome::Control control);
    void LoadTitlebarLogoBitmap();

    // Helpers
    void DrawRadialGauge(D2D1_POINT_2F center, float radius, float percentage, D2D1_COLOR_F color, const std::wstring& value_str, const std::wstring& label_str);
    void DrawCard(D2D1_RECT_F rect, bool hover_highlight = false, CardSurface surface = CardSurface::Default);
    void RestartContentAnimation();
    void UpdateContentAnimation();
    void RestartDataChartAnimation();
    void SetDataPickerOpen(bool open);
    void UpdateDataPickerAnimation();
    std::wstring GetNetworkAdapterSelectionLabel() const;
    PreferencesDraft CapturePreferences() const;
    void BeginSettingsSession();
    void MarkPreferencesDirty();
    void RestorePreferencesDraft();
    void UseRecommendedPreferences();
    void CommitPreferencesDraft();
    void HideSettingsEditors();
    void CommitVisibleSettingsEditors();
    void BeginSettingsNumericInput(SettingsNumericField field, int value);
    void CommitSettingsNumericInput();
    void CancelSettingsNumericInput();
    bool IsSettingsNumericInputActive(SettingsNumericField field) const;
    bool ChooseAppearanceColor(HWND hwnd, COLORREF& color, COLORREF automatic_color);


    
    void FormatSpeedWithUnit(double bytesPerSec, std::wstring& valStr, std::wstring& unitStr);
    std::wstring FormatBytes(uint64_t bytes);
    std::wstring FormatAppTotalBytes(uint64_t bytes);
};
