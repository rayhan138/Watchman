// Prevents additional console window on Windows in release
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod commands;
mod taskbar_embed;

use chrono::{Datelike, Local};
use commands::config::ConfigState;
use commands::history::HistoryState;
use commands::monitor::MonitorState;
use serde::Serialize;
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{
    image::Image,
    menu::{MenuBuilder, MenuItemBuilder},
    tray::TrayIconBuilder,
    Emitter, LogicalSize, Manager, Size, WindowEvent,
};
use tauri_plugin_autostart::{MacosLauncher, ManagerExt};
use tauri_plugin_notification::NotificationExt;

#[cfg(target_os = "windows")]
use std::{ffi::OsStr, os::windows::ffi::OsStrExt};

#[cfg(target_os = "windows")]
use windows::core::PCWSTR;

#[cfg(target_os = "windows")]
use windows::Win32::{
    Foundation::{GetLastError, ERROR_ALREADY_EXISTS, HANDLE, HWND, LPARAM, POINT, RECT, WPARAM},
    System::Threading::CreateMutexW,
    UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_LBUTTON, VK_RBUTTON},
    UI::WindowsAndMessaging::{
        AppendMenuW, CreatePopupMenu, DestroyMenu, FindWindowW, GetCursorPos, GetWindowRect,
        MessageBoxW, PostMessageW, SendMessageW, SetForegroundWindow, ShowWindow, TrackPopupMenu,
        MB_ICONINFORMATION, MB_OK, MF_CHECKED, MF_POPUP, MF_SEPARATOR, MF_STRING, SW_RESTORE,
        TPM_LEFTALIGN, TPM_RETURNCMD, TPM_RIGHTBUTTON, TPM_TOPALIGN, WM_CANCELMODE, WM_NULL,
    },
};

pub struct HistoryWriterState {
    pub can_write: bool,
    #[cfg(target_os = "windows")]
    _lock: Option<HistoryWriterLock>,
}

struct WidgetDisplayState {
    network_only: bool,
    hidden: bool,
}

impl Default for WidgetDisplayState {
    fn default() -> Self {
        Self {
            network_only: false,
            hidden: false,
        }
    }
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct WidgetDisplayModePayload {
    network_only: bool,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct TaskbarThemePayload {
    is_light: bool,
    source: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct WidgetMenuActionPayload {
    tab: Option<String>,
    history_filter: Option<String>,
    year: Option<i32>,
    month: Option<u32>,
}

#[derive(Default)]
struct AlertRuntimeState {
    memory_warning_active: bool,
    traffic_warning_active: bool,
    cpu_temp_warning_active: bool,
    gpu_temp_warning_active: bool,
    disk_temp_warning_active: bool,
    mainboard_temp_warning_active: bool,
    traffic_day: String,
}

const WIDGET_FULL_WIDTH: f64 = 136.0;
const WIDGET_NETWORK_ONLY_WIDTH: f64 = 84.0;
const WIDGET_HEIGHT: f64 = 32.0;

fn current_day_string() -> String {
    Local::now().format("%Y-%m-%d").to_string()
}

fn current_timestamp_millis() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn traffic_threshold_to_bytes(threshold: u32, unit: &str) -> u64 {
    let multiplier = match unit.trim().to_ascii_uppercase().as_str() {
        "GB" => 1024_u64 * 1024 * 1024,
        _ => 1024_u64 * 1024,
    };

    (threshold as u64).saturating_mul(multiplier)
}

fn format_bytes_for_alert(bytes: u64) -> String {
    if bytes == 0 {
        return "0 B".to_string();
    }

    let units = ["B", "KB", "MB", "GB", "TB"];
    let mut value = bytes as f64;
    let mut unit_index = 0usize;

    while value >= 1024.0 && unit_index < units.len() - 1 {
        value /= 1024.0;
        unit_index += 1;
    }

    let precision = if unit_index == 0 { 0 } else { 2 };
    format!(
        "{value:.precision$} {}",
        units[unit_index],
        precision = precision
    )
}

fn format_temperature_for_alert(value: f64) -> String {
    format!("{value:.1}°C")
}

fn emit_high_usage_notification(
    app_handle: &tauri::AppHandle,
    category: &str,
    title: &str,
    message: &str,
) {
    let timestamp = current_timestamp_millis();
    let notification = commands::notifications::InAppNotification {
        id: format!("{category}-{timestamp}"),
        notif_type: "warning".to_string(),
        category: category.to_string(),
        title: title.to_string(),
        message: message.to_string(),
        actions: Vec::new(),
        timestamp,
    };

    let _ = app_handle.emit("notification", &notification);
    let _ = app_handle
        .notification()
        .builder()
        .title(title)
        .body(message)
        .show();
}

fn evaluate_high_usage_warnings(
    app_handle: &tauri::AppHandle,
    payload: &commands::monitor::MetricsPayload,
    todays_total_bytes: u64,
) {
    let config = {
        let config_state = app_handle.state::<Mutex<ConfigState>>();
        let config_result = match config_state.lock() {
            Ok(state) => state.config.clone(),
            Err(_) => return,
        };
        config_result
    };

    let notifications = &config.notifications;
    let warning_settings = &notifications.warning_settings;
    let today = current_day_string();

    let mut emit_memory = false;
    let mut emit_traffic = false;
    let mut emit_cpu_temp = false;
    let mut emit_gpu_temp = false;
    let mut emit_disk_temp = false;
    let mut emit_mainboard_temp = false;

    {
        let alert_state = app_handle.state::<Mutex<AlertRuntimeState>>();
        let Ok(mut state) = alert_state.lock() else {
            return;
        };

        if state.traffic_day != today {
            state.traffic_day = today.clone();
            state.traffic_warning_active = false;
        }

        let warnings_enabled = notifications.enabled && notifications.high_usage_warnings;
        if !warnings_enabled {
            state.memory_warning_active = false;
            state.traffic_warning_active = false;
            state.cpu_temp_warning_active = false;
            state.gpu_temp_warning_active = false;
            state.disk_temp_warning_active = false;
            state.mainboard_temp_warning_active = false;
            return;
        }

        let memory_threshold = warning_settings.memory_threshold as f64;
        let memory_exceeded = warning_settings.memory_enabled
            && warning_settings.memory_threshold > 0
            && payload.memory.percent_used >= memory_threshold;

        if memory_exceeded && !state.memory_warning_active {
            state.memory_warning_active = true;
            emit_memory = true;
        } else if !memory_exceeded {
            state.memory_warning_active = false;
        }

        let traffic_threshold_bytes = traffic_threshold_to_bytes(
            warning_settings.traffic_threshold,
            &warning_settings.traffic_unit,
        );
        let traffic_exceeded = warning_settings.traffic_enabled
            && warning_settings.traffic_threshold > 0
            && todays_total_bytes >= traffic_threshold_bytes;

        if traffic_exceeded && !state.traffic_warning_active {
            state.traffic_warning_active = true;
            emit_traffic = true;
        } else if !traffic_exceeded {
            state.traffic_warning_active = false;
        }

        let cpu_temp_exceeded = warning_settings.cpu_temp_enabled
            && warning_settings.cpu_temp_threshold > 0
            && payload
                .temperatures
                .cpu
                .map(|value| value >= warning_settings.cpu_temp_threshold as f64)
                .unwrap_or(false);

        if cpu_temp_exceeded && !state.cpu_temp_warning_active {
            state.cpu_temp_warning_active = true;
            emit_cpu_temp = true;
        } else if !cpu_temp_exceeded {
            state.cpu_temp_warning_active = false;
        }

        let gpu_temp_exceeded = warning_settings.gpu_temp_enabled
            && warning_settings.gpu_temp_threshold > 0
            && payload
                .temperatures
                .gpu
                .map(|value| value >= warning_settings.gpu_temp_threshold as f64)
                .unwrap_or(false);

        if gpu_temp_exceeded && !state.gpu_temp_warning_active {
            state.gpu_temp_warning_active = true;
            emit_gpu_temp = true;
        } else if !gpu_temp_exceeded {
            state.gpu_temp_warning_active = false;
        }

        let disk_temp_exceeded = warning_settings.disk_temp_enabled
            && warning_settings.disk_temp_threshold > 0
            && payload
                .temperatures
                .disk
                .map(|value| value >= warning_settings.disk_temp_threshold as f64)
                .unwrap_or(false);

        if disk_temp_exceeded && !state.disk_temp_warning_active {
            state.disk_temp_warning_active = true;
            emit_disk_temp = true;
        } else if !disk_temp_exceeded {
            state.disk_temp_warning_active = false;
        }

        let mainboard_temp_exceeded = warning_settings.mainboard_temp_enabled
            && warning_settings.mainboard_temp_threshold > 0
            && payload
                .temperatures
                .mainboard
                .map(|value| value >= warning_settings.mainboard_temp_threshold as f64)
                .unwrap_or(false);

        if mainboard_temp_exceeded && !state.mainboard_temp_warning_active {
            state.mainboard_temp_warning_active = true;
            emit_mainboard_temp = true;
        } else if !mainboard_temp_exceeded {
            state.mainboard_temp_warning_active = false;
        }
    }

    if emit_memory {
        emit_high_usage_notification(
            app_handle,
            "memory-usage",
            "Memory usage warning",
            &format!(
                "Memory usage reached {:.1}% and crossed your {}% limit.",
                payload.memory.percent_used, warning_settings.memory_threshold
            ),
        );
    }

    if emit_traffic {
        emit_high_usage_notification(
            app_handle,
            "today-traffic",
            "Today's traffic warning",
            &format!(
                "Today's traffic reached {} and crossed your {} {} limit.",
                format_bytes_for_alert(todays_total_bytes),
                warning_settings.traffic_threshold,
                warning_settings.traffic_unit
            ),
        );
    }

    if emit_cpu_temp {
        if let Some(value) = payload.temperatures.cpu {
            emit_high_usage_notification(
                app_handle,
                "cpu-temperature",
                "CPU temperature warning",
                &format!(
                    "CPU temperature reached {} and crossed your {}°C limit.",
                    format_temperature_for_alert(value),
                    warning_settings.cpu_temp_threshold
                ),
            );
        }
    }

    if emit_gpu_temp {
        if let Some(value) = payload.temperatures.gpu {
            emit_high_usage_notification(
                app_handle,
                "gpu-temperature",
                "GPU temperature warning",
                &format!(
                    "GPU temperature reached {} and crossed your {}°C limit.",
                    format_temperature_for_alert(value),
                    warning_settings.gpu_temp_threshold
                ),
            );
        }
    }

    if emit_disk_temp {
        if let Some(value) = payload.temperatures.disk {
            emit_high_usage_notification(
                app_handle,
                "disk-temperature",
                "Disk temperature warning",
                &format!(
                    "Disk temperature reached {} and crossed your {}°C limit.",
                    format_temperature_for_alert(value),
                    warning_settings.disk_temp_threshold
                ),
            );
        }
    }

    if emit_mainboard_temp {
        if let Some(value) = payload.temperatures.mainboard {
            emit_high_usage_notification(
                app_handle,
                "mainboard-temperature",
                "Mainboard temperature warning",
                &format!(
                    "Mainboard temperature reached {} and crossed your {}°C limit.",
                    format_temperature_for_alert(value),
                    warning_settings.mainboard_temp_threshold
                ),
            );
        }
    }
}

#[cfg(target_os = "windows")]
struct HistoryWriterLock {
    _handle: HANDLE,
}

#[cfg(target_os = "windows")]
fn create_history_writer_state() -> HistoryWriterState {
    let mut name: Vec<u16> = std::ffi::OsStr::new("Local\\TrafficMonitorHistoryWriter")
        .encode_wide()
        .collect();
    name.push(0);

    let handle = match unsafe { CreateMutexW(None, true, PCWSTR(name.as_ptr())) } {
        Ok(handle) => handle,
        Err(_) => {
            return HistoryWriterState {
                can_write: true,
                _lock: None,
            }
        }
    };

    let already_exists = unsafe { GetLastError() } == ERROR_ALREADY_EXISTS;
    if already_exists {
        HistoryWriterState {
            can_write: false,
            _lock: None,
        }
    } else {
        HistoryWriterState {
            can_write: true,
            _lock: Some(HistoryWriterLock { _handle: handle }),
        }
    }
}

#[cfg(not(target_os = "windows"))]
fn create_history_writer_state() -> HistoryWriterState {
    HistoryWriterState { can_write: true }
}

fn main() {
    let start_minimized = std::env::args().any(|arg| arg == "--minimized");

    let mut builder = tauri::Builder::default()
        .plugin(tauri_plugin_autostart::init(
            MacosLauncher::LaunchAgent,
            Some(vec!["--minimized"]),
        ))
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_notification::init())
        .manage(Mutex::new(MonitorState::new()))
        .manage(Mutex::new(ConfigState::new()))
        .manage(Mutex::new(HistoryState::new()))
        .manage(Mutex::new(AlertRuntimeState::default()))
        .manage(Mutex::new(WidgetDisplayState::default()))
        .manage(create_history_writer_state());

    if !cfg!(debug_assertions) {
        builder = builder.plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            show_main_window(app);
        }));
    }

    builder
        .invoke_handler(tauri::generate_handler![
            // Monitor
            commands::monitor::cmd_get_cpu_usage,
            commands::monitor::cmd_get_memory_usage,
            commands::monitor::cmd_get_network_stats,
            commands::monitor::cmd_get_disk_usage,
            commands::monitor::cmd_get_temperature_readings,
            commands::monitor::cmd_get_network_interfaces,
            commands::monitor::cmd_get_system_info,
            // Config
            commands::config::get_config,
            commands::config::save_config,
            commands::config::apply_recommended_settings,
            commands::config::undo_settings,
            commands::config::can_undo_settings,
            // History
            commands::history::get_traffic_history,
            // Network Health
            commands::network_health::get_quality,
            commands::network_health::measure_latency,
            commands::network_health::get_signal_strength,
            commands::network_health::get_network_overview,
            commands::network_health::run_speed_test,
            commands::network_health::get_speed_test_history,
            // App Monitor
            commands::app_monitor::get_active_applications,
            commands::app_monitor::get_app_monitor_status,
            commands::app_monitor::terminate_application,
            // Data Usage
            commands::data_usage::get_usage,
            commands::data_usage::set_data_limit,
            commands::data_usage::get_remaining_allowance,
            commands::data_usage::get_data_thresholds,
            commands::data_usage::compare_usage,
            // Profile
            commands::profile::get_profiles,
            commands::profile::get_active_profile,
            commands::profile::set_active_profile,
            commands::profile::get_profile_config,
            // Troubleshooter
            commands::troubleshooter::run_diagnostics,
            // Export
            commands::export::export_csv,
            // Notifications
            commands::notifications::dismiss_notification,
            commands::notifications::notification_action,
            // Window management
            cmd_minimize_window,
            cmd_close_window,
            cmd_toggle_always_on_top,
            cmd_show_main_window,
            cmd_show_history_window,
            cmd_toggle_widget_lock,
            cmd_show_widget_context_menu,
            cmd_reset_session_counters,
            cmd_get_widget_display_mode,
            cmd_get_taskbar_theme,
        ])
        .setup(move |app| {
            if start_minimized {
                if let Some(window) = app.get_webview_window("main") {
                    let _ = window.minimize();
                    let _ = window.hide();
                }
            } else {
                // Window state persistence can remember a hidden main window after
                // "close to tray", so force a visible startup on normal launches.
                show_main_window(app.handle());
                let startup_handle = app.handle().clone();
                tauri::async_runtime::spawn(async move {
                    tokio::time::sleep(Duration::from_millis(350)).await;
                    show_main_window(&startup_handle);
                    tokio::time::sleep(Duration::from_millis(950)).await;
                    show_main_window(&startup_handle);
                });
            }

            // Create taskbar widget window
            let taskbar_result = tauri::WebviewWindowBuilder::new(
                app,
                "taskbar",
                tauri::WebviewUrl::App("taskbar.html".into()),
            )
            .title("Watchman Widget")
            .inner_size(WIDGET_FULL_WIDTH, WIDGET_HEIGHT)
            .resizable(false)
            .decorations(false)
            .transparent(true)
            .shadow(false)
            .always_on_top(true)
            .skip_taskbar(true)
            .visible(false)
            .build();

            // Apply initial Windows-specific hardening + grab the raw HWND for the keep-on-top loop
            #[cfg(target_os = "windows")]
            let widget_hwnd: Option<isize> = if let Ok(ref taskbar_win) = taskbar_result {
                if let Ok(hwnd_val) = taskbar_win.hwnd() {
                    let raw = hwnd_val.0 as isize;
                    crate::taskbar_embed::apply_widget_styles(raw);
                    let (preferred_width, preferred_height) =
                        crate::taskbar_embed::get_window_size(raw).unwrap_or((136, 32));
                    let _ = crate::taskbar_embed::enforce_widget(
                        raw,
                        preferred_width,
                        preferred_height,
                    );
                    Some(raw)
                } else {
                    None
                }
            } else {
                None
            };

            #[cfg(not(target_os = "windows"))]
            let widget_hwnd: Option<isize> = None;

            #[cfg(target_os = "windows")]
            if let Some(raw_widget_hwnd) = widget_hwnd {
                let startup_handle = app.handle().clone();
                std::thread::spawn(move || {
                    let widget_hwnd = HWND(raw_widget_hwnd);
                    let mut right_button_was_down = false;
                    let mut context_menu_armed = false;
                    let mut left_button_was_down = false;
                    let mut left_click_armed = false;
                    let mut last_left_click_at: Option<Instant> = None;
                    let mut last_left_click_pos: Option<(i32, i32)> = None;
                    let mut last_context_menu_at: Option<Instant> = None;
                    let mut last_history_at: Option<Instant> = None;

                    loop {
                        std::thread::sleep(Duration::from_millis(35));

                        if crate::taskbar_embed::take_widget_double_click_request() {
                            let now = Instant::now();
                            let can_open = last_history_at
                                .map(|last| now.duration_since(last) > Duration::from_millis(450))
                                .unwrap_or(true);

                            if can_open {
                                last_history_at = Some(now);
                                let history_handle = startup_handle.clone();
                                let _ = startup_handle.run_on_main_thread(move || {
                                    cmd_show_history_window(history_handle);
                                });
                            }
                            continue;
                        }

                        if crate::taskbar_embed::take_widget_context_menu_request() {
                            let now = Instant::now();
                            let can_open = last_context_menu_at
                                .map(|last| now.duration_since(last) > Duration::from_millis(350))
                                .unwrap_or(true);

                            if can_open {
                                last_context_menu_at = Some(now);
                                let menu_handle = startup_handle.clone();
                                let _ = startup_handle.run_on_main_thread(move || {
                                    let _ = show_widget_context_menu_for_app(&menu_handle);
                                });
                            }
                            continue;
                        }

                        let right_button_down =
                            unsafe { (GetAsyncKeyState(VK_RBUTTON.0 as i32) as u16 & 0x8000) != 0 };
                        let left_button_down =
                            unsafe { (GetAsyncKeyState(VK_LBUTTON.0 as i32) as u16 & 0x8000) != 0 };

                        if left_button_down {
                            if !left_button_was_down {
                                left_button_was_down = true;

                                let mut rect = RECT::default();
                                let mut cursor = POINT::default();
                                left_click_armed = unsafe { GetWindowRect(widget_hwnd, &mut rect) }
                                    .is_ok()
                                    && unsafe { GetCursorPos(&mut cursor) }.is_ok()
                                    && cursor.x >= rect.left
                                    && cursor.x < rect.right
                                    && cursor.y >= rect.top
                                    && cursor.y < rect.bottom;
                            }
                        } else if left_button_was_down {
                            left_button_was_down = false;

                            let mut rect = RECT::default();
                            let mut cursor = POINT::default();
                            let released_inside = unsafe { GetWindowRect(widget_hwnd, &mut rect) }
                                .is_ok()
                                && unsafe { GetCursorPos(&mut cursor) }.is_ok()
                                && cursor.x >= rect.left
                                && cursor.x < rect.right
                                && cursor.y >= rect.top
                                && cursor.y < rect.bottom;

                            if left_click_armed && released_inside {
                                let now = Instant::now();
                                let previous_click_matches = last_left_click_at
                                    .zip(last_left_click_pos)
                                    .map(|(last, (x, y))| {
                                        now.duration_since(last) <= Duration::from_millis(420)
                                            && (cursor.x - x).abs() <= 8
                                            && (cursor.y - y).abs() <= 8
                                    })
                                    .unwrap_or(false);

                                if previous_click_matches {
                                    last_left_click_at = None;
                                    last_left_click_pos = None;

                                    let can_open = last_history_at
                                        .map(|last| {
                                            now.duration_since(last) > Duration::from_millis(450)
                                        })
                                        .unwrap_or(true);

                                    if can_open {
                                        last_history_at = Some(now);
                                        let history_handle = startup_handle.clone();
                                        let _ = startup_handle.run_on_main_thread(move || {
                                            cmd_show_history_window(history_handle);
                                        });
                                    }
                                } else {
                                    last_left_click_at = Some(now);
                                    last_left_click_pos = Some((cursor.x, cursor.y));
                                }
                            }

                            left_click_armed = false;
                        }

                        if right_button_down {
                            if right_button_was_down {
                                if context_menu_armed {
                                    cancel_shell_taskbar_menu();
                                }
                                continue;
                            }

                            right_button_was_down = true;

                            let mut rect = RECT::default();
                            if unsafe { GetWindowRect(widget_hwnd, &mut rect) }.is_err() {
                                context_menu_armed = false;
                                continue;
                            }

                            let mut cursor = POINT::default();
                            if unsafe { GetCursorPos(&mut cursor) }.is_err() {
                                context_menu_armed = false;
                                continue;
                            }

                            context_menu_armed = cursor.x >= rect.left
                                && cursor.x < rect.right
                                && cursor.y >= rect.top
                                && cursor.y < rect.bottom;

                            if context_menu_armed {
                                cancel_shell_taskbar_menu();
                            }

                            continue;
                        }

                        if right_button_was_down && context_menu_armed {
                            context_menu_armed = false;
                            right_button_was_down = false;
                            cancel_shell_taskbar_menu();

                            let now = Instant::now();
                            let can_open = last_context_menu_at
                                .map(|last| now.duration_since(last) > Duration::from_millis(350))
                                .unwrap_or(true);

                            if can_open {
                                last_context_menu_at = Some(now);
                                let menu_handle = startup_handle.clone();
                                let _ = startup_handle.run_on_main_thread(move || {
                                    let _ = show_widget_context_menu_for_app(&menu_handle);
                                });
                            }
                            continue;
                        }

                        right_button_was_down = false;
                        context_menu_armed = false;
                    }
                });
            }

            // System tray
            setup_tray(app)?;

            // Setup autostart based on saved config
            let do_autostart = {
                if let Some(state) = app.try_state::<Mutex<crate::commands::config::ConfigState>>()
                {
                    if let Ok(s) = state.lock() {
                        s.config.start_on_boot
                    } else {
                        true
                    }
                } else {
                    true
                }
            };

            let autostart_mgr = app.autolaunch();
            if do_autostart {
                let _ = autostart_mgr.enable();
            } else {
                let _ = autostart_mgr.disable();
            }

            // App-level network tracking can be slow when Watchman is elevated.
            // Start it after the UI is already responsive instead of blocking launch.
            std::thread::spawn(|| {
                std::thread::sleep(Duration::from_secs(8));
                commands::app_monitor::start_tracker();
            });

            // Start metrics polling loop
            let app_handle = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                metrics_loop(app_handle).await;
            });

            // Start history save loop
            let app_handle2 = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                history_save_loop(app_handle2).await;
            });

            // Keep-on-top loop: every 500 ms re-assert TOPMOST so Windows can't push the widget behind
            let app_handle3 = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                keep_widget_on_top_loop(app_handle3, widget_hwnd).await;
            });

            Ok(())
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                // Hide instead of close
                let _ = window.hide();
                api.prevent_close();
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

fn setup_tray(app: &tauri::App) -> Result<(), Box<dyn std::error::Error>> {
    let show_item = MenuItemBuilder::with_id("show", "Open Watchman").build(app)?;
    let preferences_item = MenuItemBuilder::with_id("preferences", "Preferences").build(app)?;
    let toggle_widget_item =
        MenuItemBuilder::with_id("toggle_widget", "Show/Hide Widget").build(app)?;
    let run_as_admin_item =
        MenuItemBuilder::with_id("run_as_admin", "Run as Administrator").build(app)?;
    let restart_item = MenuItemBuilder::with_id("restart", "Restart Watchman").build(app)?;
    let help_about_item = MenuItemBuilder::with_id("help_about", "Help & About").build(app)?;
    let quit_item = MenuItemBuilder::with_id("quit", "Quit").build(app)?;
    let menu = MenuBuilder::new(app)
        .item(&show_item)
        .item(&preferences_item)
        .item(&toggle_widget_item)
        .item(&run_as_admin_item)
        .item(&restart_item)
        .item(&help_about_item)
        .separator()
        .item(&quit_item)
        .build()?;

    let icon = Image::from_bytes(include_bytes!("../icons/icon.png")).expect("Failed to load icon");

    TrayIconBuilder::new()
        .icon(icon)
        .tooltip("Watchman")
        .menu(&menu)
        .on_menu_event(move |app, event| match event.id().as_ref() {
            "show" => show_main_window(app),
            "preferences" => open_preferences(app),
            "toggle_widget" => toggle_widget_visibility(app),
            "run_as_admin" => relaunch_watchman_as_admin(app),
            "restart" => restart_watchman(app),
            "help_about" => show_help_about(app),
            "quit" => quit_application(app),
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let tauri::tray::TrayIconEvent::Click {
                button: tauri::tray::MouseButton::Left,
                ..
            } = event
            {
                let app = tray.app_handle();
                show_main_window(app);
            }
        })
        .build(app)?;

    Ok(())
}

fn show_main_window<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    if let Some(window) = manager.get_webview_window("main") {
        let _ = window.show();
        let _ = window.unminimize();
        let _ = window.set_focus();
        #[cfg(target_os = "windows")]
        {
            if let Ok(hwnd) = window.hwnd() {
                unsafe {
                    let raw = HWND(hwnd.0 as isize);
                    let _ = ShowWindow(raw, SW_RESTORE);
                    let _ = SetForegroundWindow(raw);
                }
            }
        }
    }
}

fn emit_main_window_action<R: tauri::Runtime>(app_handle: &tauri::AppHandle<R>, action: &str) {
    if let Some(window) = app_handle.get_webview_window("main") {
        let _ = window.emit(
            "tray-menu-action",
            serde_json::json!({
                "action": action,
            }),
        );
    }
}

fn open_preferences<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    show_main_window(manager);
    let app_handle = manager.app_handle().clone();
    emit_main_window_action(&app_handle, "open-preferences");
}

fn quit_application<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    if let Some(state) = manager.try_state::<Mutex<HistoryState>>() {
        if let Ok(mut s) = state.lock() {
            s.save();
        }
    }
    commands::app_monitor::shutdown_tracker();
    #[cfg(target_os = "windows")]
    crate::taskbar_embed::restore_taskbar_layout();
    manager.app_handle().exit(0);
}

fn apply_widget_display_mode(app_handle: &tauri::AppHandle, network_only: bool) {
    if let Some(taskbar) = app_handle.get_webview_window("taskbar") {
        let width = if network_only {
            WIDGET_NETWORK_ONLY_WIDTH
        } else {
            WIDGET_FULL_WIDTH
        };
        let _ = taskbar.set_size(Size::Logical(LogicalSize::new(width, WIDGET_HEIGHT)));
        let _ = taskbar.emit(
            "widget-display-mode-changed",
            WidgetDisplayModePayload { network_only },
        );
    }
}

fn toggle_widget_visibility<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    let app_handle = manager.app_handle().clone();
    let hidden = {
        let widget_state = app_handle.state::<Mutex<WidgetDisplayState>>();
        let result = if let Ok(mut state) = widget_state.lock() {
            state.hidden = !state.hidden;
            state.hidden
        } else {
            return;
        };
        result
    };

    if let Some(taskbar) = app_handle.get_webview_window("taskbar") {
        if hidden {
            #[cfg(target_os = "windows")]
            crate::taskbar_embed::restore_taskbar_layout();
            let _ = taskbar.hide();
            emit_widget_feedback(&app_handle, "Taskbar widget hidden", "info");
        } else {
            let _ = taskbar.show();
            emit_widget_feedback(&app_handle, "Taskbar widget shown", "info");
        }
    }
}

fn restart_watchman<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    let current_exe = match std::env::current_exe() {
        Ok(path) => path,
        Err(_) => {
            let app_handle = manager.app_handle().clone();
            emit_widget_feedback(&app_handle, "Could not restart Watchman", "error");
            return;
        }
    };

    if std::process::Command::new(current_exe).spawn().is_ok() {
        quit_application(manager);
    } else {
        let app_handle = manager.app_handle().clone();
        emit_widget_feedback(&app_handle, "Could not restart Watchman", "error");
    }
}

#[cfg(target_os = "windows")]
fn relaunch_watchman_as_admin<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    let current_exe = match std::env::current_exe() {
        Ok(path) => path,
        Err(_) => {
            let app_handle = manager.app_handle().clone();
            emit_widget_feedback(&app_handle, "Could not relaunch as administrator", "error");
            return;
        }
    };

    let exe_path = current_exe.to_string_lossy().replace('\'', "''");
    let command = format!("Start-Process -FilePath '{exe_path}' -Verb RunAs");
    let status = std::process::Command::new("powershell.exe")
        .arg("-NoProfile")
        .arg("-WindowStyle")
        .arg("Hidden")
        .arg("-Command")
        .arg(command)
        .status();

    match status {
        Ok(status) if status.success() => quit_application(manager),
        _ => {
            let app_handle = manager.app_handle().clone();
            emit_widget_feedback(&app_handle, "Could not relaunch as administrator", "error")
        }
    }
}

#[cfg(not(target_os = "windows"))]
fn relaunch_watchman_as_admin<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    emit_widget_feedback(
        &manager.app_handle(),
        "Administrator relaunch is only available on Windows",
        "info",
    );
}

#[cfg(target_os = "windows")]
fn show_help_about<R: tauri::Runtime, M: Manager<R>>(_manager: &M) {
    const VERSION: &str = env!("CARGO_PKG_VERSION");
    let title = wide_string("Help & About");
    let message = wide_string(&format!(
        "Watchman v{VERSION}\n\nWatchman monitors your network activity, taskbar widget, and system status.\n\nTips:\n- Use Preferences for warning thresholds and startup options.\n- Run as administrator for per-app bandwidth and temperature warnings.\n- Use Show/Hide Widget from the tray if you want to hide the taskbar widget temporarily."
    ));

    unsafe {
        let _ = MessageBoxW(
            None,
            PCWSTR(message.as_ptr()),
            PCWSTR(title.as_ptr()),
            MB_OK | MB_ICONINFORMATION,
        );
    }
}

#[cfg(not(target_os = "windows"))]
fn show_help_about<R: tauri::Runtime, M: Manager<R>>(manager: &M) {
    emit_widget_feedback(
        &manager.app_handle(),
        &format!("Watchman v{}", env!("CARGO_PKG_VERSION")),
        "info",
    );
}

fn emit_widget_menu_action(
    app_handle: &tauri::AppHandle,
    tab: Option<&str>,
    history_filter: Option<&str>,
) {
    if let Some(window) = app_handle.get_webview_window("main") {
        let now = Local::now();
        let payload = WidgetMenuActionPayload {
            tab: tab.map(str::to_string),
            history_filter: history_filter.map(str::to_string),
            year: (history_filter == Some("monthly")).then_some(now.year()),
            month: (history_filter == Some("monthly")).then_some(now.month()),
        };
        let _ = window.emit("widget-menu-action", payload);
    }
}

fn emit_widget_feedback<R: tauri::Runtime>(
    app_handle: &tauri::AppHandle<R>,
    message: &str,
    level: &str,
) {
    if let Some(window) = app_handle.get_webview_window("main") {
        let payload = serde_json::json!({
            "message": message,
            "level": level,
        });
        let _ = window.emit("widget-feedback", payload);
    }
}

#[cfg(target_os = "windows")]
#[derive(Clone, Copy)]
enum WidgetMenuCommand {
    OpenDashboard,
    OpenHistory,
    ViewToday,
    ViewLast7Days,
    ViewMonthly,
    ShowNetworkOnly,
    ShowCpuMem,
    ResetSessionCounters,
    Exit,
}

#[cfg(target_os = "windows")]
fn show_widget_context_menu_native(
    owner_hwnd: HWND,
    network_only: bool,
) -> Option<WidgetMenuCommand> {
    const MENU_OPEN_DASHBOARD: usize = 1001;
    const MENU_OPEN_HISTORY: usize = 1002;
    const MENU_VIEW_TODAY: usize = 1101;
    const MENU_VIEW_LAST_7_DAYS: usize = 1102;
    const MENU_VIEW_MONTHLY: usize = 1103;
    const MENU_SHOW_NETWORK_ONLY: usize = 1201;
    const MENU_SHOW_CPU_MEM: usize = 1202;
    const MENU_RESET_SESSION_COUNTERS: usize = 1301;
    const MENU_EXIT: usize = 1302;

    let menu = unsafe { CreatePopupMenu() }.ok()?;
    let view_menu = unsafe { CreatePopupMenu() }.ok()?;
    let widget_menu = unsafe { CreatePopupMenu() }.ok()?;

    let open_dashboard = wide_string("Open Dashboard");
    let open_history = wide_string("Open History");
    let view = wide_string("View");
    let today = wide_string("Today");
    let last_7_days = wide_string("Last 7 Days");
    let monthly = wide_string("Monthly");
    let widget = wide_string("Widget");
    let show_network_only = wide_string("Show Network Only");
    let show_cpu_mem = wide_string("Show CPU/MEM");
    let reset_session_counters = wide_string("Reset Session Counters");
    let exit = wide_string("Exit");

    let menu_ok = unsafe {
        AppendMenuW(
            menu,
            MF_STRING,
            MENU_OPEN_DASHBOARD,
            PCWSTR(open_dashboard.as_ptr()),
        )
        .is_ok()
            && AppendMenuW(
                menu,
                MF_STRING,
                MENU_OPEN_HISTORY,
                PCWSTR(open_history.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(
                view_menu,
                MF_STRING,
                MENU_VIEW_TODAY,
                PCWSTR(today.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(
                view_menu,
                MF_STRING,
                MENU_VIEW_LAST_7_DAYS,
                PCWSTR(last_7_days.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(
                view_menu,
                MF_STRING,
                MENU_VIEW_MONTHLY,
                PCWSTR(monthly.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(menu, MF_POPUP, view_menu.0 as usize, PCWSTR(view.as_ptr())).is_ok()
            && AppendMenuW(
                widget_menu,
                if network_only {
                    MF_STRING | MF_CHECKED
                } else {
                    MF_STRING
                },
                MENU_SHOW_NETWORK_ONLY,
                PCWSTR(show_network_only.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(
                widget_menu,
                if network_only {
                    MF_STRING
                } else {
                    MF_STRING | MF_CHECKED
                },
                MENU_SHOW_CPU_MEM,
                PCWSTR(show_cpu_mem.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(
                menu,
                MF_POPUP,
                widget_menu.0 as usize,
                PCWSTR(widget.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null()).is_ok()
            && AppendMenuW(
                menu,
                MF_STRING,
                MENU_RESET_SESSION_COUNTERS,
                PCWSTR(reset_session_counters.as_ptr()),
            )
            .is_ok()
            && AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null()).is_ok()
            && AppendMenuW(menu, MF_STRING, MENU_EXIT, PCWSTR(exit.as_ptr())).is_ok()
    };

    if !menu_ok {
        unsafe {
            let _ = DestroyMenu(menu);
        }
        return None;
    }

    let mut cursor = POINT::default();
    if unsafe { GetCursorPos(&mut cursor) }.is_err() {
        unsafe {
            let _ = DestroyMenu(menu);
        }
        return None;
    }

    unsafe {
        let _ = SetForegroundWindow(owner_hwnd);
    }

    let selected = unsafe {
        TrackPopupMenu(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
            cursor.x,
            cursor.y,
            0,
            owner_hwnd,
            None,
        )
    };

    unsafe {
        let _ = PostMessageW(owner_hwnd, WM_NULL, WPARAM(0), LPARAM(0));
        let _ = DestroyMenu(menu);
    }

    match selected.0 as usize {
        MENU_OPEN_DASHBOARD => Some(WidgetMenuCommand::OpenDashboard),
        MENU_OPEN_HISTORY => Some(WidgetMenuCommand::OpenHistory),
        MENU_VIEW_TODAY => Some(WidgetMenuCommand::ViewToday),
        MENU_VIEW_LAST_7_DAYS => Some(WidgetMenuCommand::ViewLast7Days),
        MENU_VIEW_MONTHLY => Some(WidgetMenuCommand::ViewMonthly),
        MENU_SHOW_NETWORK_ONLY => Some(WidgetMenuCommand::ShowNetworkOnly),
        MENU_SHOW_CPU_MEM => Some(WidgetMenuCommand::ShowCpuMem),
        MENU_RESET_SESSION_COUNTERS => Some(WidgetMenuCommand::ResetSessionCounters),
        MENU_EXIT => Some(WidgetMenuCommand::Exit),
        _ => None,
    }
}

#[cfg(target_os = "windows")]
fn wide_string(value: &str) -> Vec<u16> {
    OsStr::new(value)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

async fn metrics_loop(app_handle: tauri::AppHandle) {
    // Wait a moment for window to be ready
    tokio::time::sleep(Duration::from_secs(1)).await;
    let resource_dir = app_handle.path().resource_dir().ok();
    let temperature_probe_paths =
        commands::monitor::resolve_temperature_probe_paths(resource_dir.as_deref());
    let temperature_probe_ready_at = Instant::now() + Duration::from_secs(12);

    loop {
        // Get metrics
        let (payload, uploaded, downloaded) = {
            let monitor_state = app_handle.state::<Mutex<MonitorState>>();
            let mut ms = monitor_state.lock().unwrap();

            let cpu = commands::monitor::get_cpu_usage(&mut ms);
            let memory = commands::monitor::get_memory_usage(&mut ms);
            let gpu = commands::monitor::get_gpu_usage(&mut ms);
            let temperatures = if Instant::now() >= temperature_probe_ready_at {
                commands::monitor::get_temperature_readings(
                    &mut ms,
                    temperature_probe_paths.as_ref(),
                )
            } else {
                ms.temperatures.clone()
            };
            let network = commands::monitor::get_network_stats(&mut ms);

            let up = network.uploaded_bytes;
            let down = network.downloaded_bytes;
            (
                commands::monitor::MetricsPayload {
                    network,
                    cpu,
                    memory,
                    gpu,
                    temperatures,
                },
                up,
                down,
            )
        };

        // Record traffic in history (separate scope)
        let todays_total_bytes = {
            let history_writer = app_handle.state::<HistoryWriterState>();
            let history_state = app_handle.state::<Mutex<HistoryState>>();
            let total = if let Ok(mut hs) = history_state.lock() {
                if history_writer.can_write {
                    hs.add_traffic(uploaded, downloaded);
                }

                hs.data
                    .records
                    .get(&current_day_string())
                    .map(|record| record.upload.saturating_add(record.download))
                    .unwrap_or(0)
            } else {
                0
            };
            total
        };

        // Emit to all windows
        let _ = app_handle.emit("metrics", &payload);
        evaluate_high_usage_warnings(&app_handle, &payload, todays_total_bytes);

        tokio::time::sleep(Duration::from_secs(1)).await;
    }
}

async fn history_save_loop(app_handle: tauri::AppHandle) {
    loop {
        tokio::time::sleep(Duration::from_secs(30)).await;
        let history_state = app_handle.state::<Mutex<HistoryState>>();
        if let Ok(mut hs) = history_state.lock() {
            hs.save();
        };
    }
}

/// Keeps the taskbar widget pinned over the taskbar by periodically re-applying
/// native placement and style flags. Windows can reshuffle taskbar child
/// geometry whenever icons, flyouts, or full-screen apps change state.
async fn keep_widget_on_top_loop(app_handle: tauri::AppHandle, hwnd_raw: Option<isize>) {
    // Wait a bit for things to settle on startup
    tokio::time::sleep(Duration::from_millis(1500)).await;
    #[cfg(target_os = "windows")]
    let mut last_placement: Option<crate::taskbar_embed::WidgetPlacement> = None;

    loop {
        tokio::time::sleep(Duration::from_millis(500)).await;

        let taskbar_window = app_handle.get_webview_window("taskbar");
        let main_window = app_handle.get_webview_window("main");
        let widget_hidden = app_handle
            .state::<Mutex<WidgetDisplayState>>()
            .lock()
            .map(|state| state.hidden)
            .unwrap_or(false);

        if widget_hidden {
            #[cfg(target_os = "windows")]
            crate::taskbar_embed::restore_taskbar_layout();
            if let Some(win) = taskbar_window.as_ref() {
                let _ = win.hide();
            }
            continue;
        }

        #[cfg(target_os = "windows")]
        if let Some(raw) = hwnd_raw {
            let main_hwnd_raw = main_window
                .as_ref()
                .and_then(|window| window.hwnd().ok())
                .map(|hwnd| hwnd.0 as isize);

            let should_show = crate::taskbar_embed::should_widget_be_visible(raw, main_hwnd_raw);
            if let Some(win) = taskbar_window.as_ref() {
                if should_show {
                    let _ = win.show();
                } else {
                    crate::taskbar_embed::restore_taskbar_layout();
                    let _ = win.hide();
                    continue;
                }
            }

            let (preferred_width, preferred_height) =
                crate::taskbar_embed::get_window_size(raw).unwrap_or((136, 32));
            if let Some(placement) =
                crate::taskbar_embed::enforce_widget(raw, preferred_width, preferred_height)
            {
                if last_placement.as_ref() != Some(&placement) {
                    let _ = app_handle.emit("taskbar-placement", &placement);
                    last_placement = Some(placement);
                }
            }
        }

        #[cfg(not(target_os = "windows"))]
        if let Some(win) = taskbar_window.as_ref() {
            let _ = win.show();
        }
    }
}

// Window management commands

#[tauri::command]
fn cmd_minimize_window(window: tauri::Window) {
    let _ = window.minimize();
}

#[tauri::command]
fn cmd_close_window(window: tauri::Window) {
    let _ = window.hide();
}

#[tauri::command]
fn cmd_toggle_always_on_top(window: tauri::Window) -> bool {
    let is_on_top = window.is_always_on_top().unwrap_or(false);
    let _ = window.set_always_on_top(!is_on_top);
    !is_on_top
}

#[tauri::command]
fn cmd_show_main_window(app_handle: tauri::AppHandle) {
    show_main_window(&app_handle);
}

#[tauri::command]
fn cmd_show_history_window(app_handle: tauri::AppHandle) {
    show_main_window(&app_handle);
    emit_widget_menu_action(&app_handle, Some("data-usage"), Some("last7days"));
}

#[tauri::command]
fn cmd_toggle_widget_lock(app_handle: tauri::AppHandle, locked: bool) -> bool {
    if let Some(taskbar) = app_handle.get_webview_window("taskbar") {
        // When locked: no drag. When unlocked: allow drag via CSS -webkit-app-region
        // We just return the state; the CSS handles drag behavior
        let _ = locked; // state tracked in frontend
        let _ = taskbar;
    }
    locked
}

#[tauri::command]
fn cmd_get_widget_display_mode(
    widget_state: tauri::State<'_, Mutex<WidgetDisplayState>>,
) -> WidgetDisplayModePayload {
    let network_only = widget_state
        .lock()
        .map(|state| state.network_only)
        .unwrap_or(false);
    WidgetDisplayModePayload { network_only }
}

#[cfg(target_os = "windows")]
fn read_windows_taskbar_light_theme() -> Option<bool> {
    use winreg::enums::{HKEY_CURRENT_USER, KEY_READ};
    use winreg::RegKey;

    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let personalize = hkcu
        .open_subkey_with_flags(
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            KEY_READ,
        )
        .ok()?;
    let value: u32 = personalize.get_value("SystemUsesLightTheme").ok()?;
    Some(value != 0)
}

#[tauri::command]
fn cmd_get_taskbar_theme() -> TaskbarThemePayload {
    #[cfg(target_os = "windows")]
    {
        if let Some(is_light) = read_windows_taskbar_light_theme() {
            return TaskbarThemePayload {
                is_light,
                source: "windows-system-theme".to_string(),
            };
        }
    }

    TaskbarThemePayload {
        is_light: false,
        source: "fallback-dark".to_string(),
    }
}

#[cfg(target_os = "windows")]
fn handle_widget_menu_action(
    app_handle: &tauri::AppHandle,
    widget_state: &Mutex<WidgetDisplayState>,
    monitor_state: &Mutex<MonitorState>,
    action: WidgetMenuCommand,
) {
    match action {
        WidgetMenuCommand::OpenDashboard => {
            show_main_window(app_handle);
            emit_widget_menu_action(app_handle, Some("dashboard"), None);
        }
        WidgetMenuCommand::OpenHistory => {
            show_main_window(app_handle);
            emit_widget_menu_action(app_handle, Some("data-usage"), Some("last7days"));
        }
        WidgetMenuCommand::ViewToday => {
            show_main_window(app_handle);
            emit_widget_menu_action(app_handle, Some("data-usage"), Some("today"));
        }
        WidgetMenuCommand::ViewLast7Days => {
            show_main_window(app_handle);
            emit_widget_menu_action(app_handle, Some("data-usage"), Some("last7days"));
        }
        WidgetMenuCommand::ViewMonthly => {
            show_main_window(app_handle);
            emit_widget_menu_action(app_handle, Some("data-usage"), Some("monthly"));
        }
        WidgetMenuCommand::ShowNetworkOnly => {
            if let Ok(mut state) = widget_state.lock() {
                state.network_only = true;
            }
            apply_widget_display_mode(app_handle, true);
        }
        WidgetMenuCommand::ShowCpuMem => {
            if let Ok(mut state) = widget_state.lock() {
                state.network_only = false;
            }
            apply_widget_display_mode(app_handle, false);
        }
        WidgetMenuCommand::ResetSessionCounters => {
            if let Ok(mut state) = monitor_state.lock() {
                commands::monitor::reset_session_counters(&mut state);
            }
            emit_widget_feedback(app_handle, "Session counters reset", "info");
        }
        WidgetMenuCommand::Exit => {
            quit_application(app_handle);
        }
    }
}

#[cfg(target_os = "windows")]
fn show_widget_context_menu_for_app(app_handle: &tauri::AppHandle) -> bool {
    let taskbar = match app_handle.get_webview_window("taskbar") {
        Some(window) => window,
        None => return false,
    };

    let _widget_hwnd = match taskbar.hwnd() {
        Ok(hwnd) => HWND(hwnd.0 as isize),
        Err(_) => return false,
    };

    let owner_hwnd = if let Some(main_window) = app_handle.get_webview_window("main") {
        match main_window.hwnd() {
            Ok(hwnd) => HWND(hwnd.0 as isize),
            Err(_) => return false,
        }
    } else {
        return false;
    };

    let Some(widget_state) = app_handle.try_state::<Mutex<WidgetDisplayState>>() else {
        return false;
    };
    let Some(monitor_state) = app_handle.try_state::<Mutex<MonitorState>>() else {
        return false;
    };

    let network_only = widget_state
        .lock()
        .map(|state| state.network_only)
        .unwrap_or(false);

    cancel_shell_taskbar_menu();
    cancel_shell_taskbar_menu_soon();

    let Some(action) = show_widget_context_menu_native(owner_hwnd, network_only) else {
        cancel_shell_taskbar_menu();
        cancel_shell_taskbar_menu_soon();
        return true;
    };

    cancel_shell_taskbar_menu();
    cancel_shell_taskbar_menu_soon();
    handle_widget_menu_action(app_handle, &widget_state, &monitor_state, action);
    true
}

#[cfg(target_os = "windows")]
fn cancel_shell_taskbar_menu() {
    let class_name = wide_string("Shell_TrayWnd");
    let shell = unsafe { FindWindowW(PCWSTR(class_name.as_ptr()), PCWSTR::null()) };
    if shell.0 != 0 {
        unsafe {
            let _ = SendMessageW(shell, WM_CANCELMODE, WPARAM(0), LPARAM(0));
        }
    }
}

#[cfg(target_os = "windows")]
fn cancel_shell_taskbar_menu_soon() {
    std::thread::spawn(|| {
        for delay_ms in [30_u64, 90, 180] {
            std::thread::sleep(Duration::from_millis(delay_ms));
            cancel_shell_taskbar_menu();
        }
    });
}

#[tauri::command]
fn cmd_show_widget_context_menu(
    app_handle: tauri::AppHandle,
    widget_state: tauri::State<'_, Mutex<WidgetDisplayState>>,
    monitor_state: tauri::State<'_, Mutex<MonitorState>>,
) -> bool {
    #[cfg(target_os = "windows")]
    {
        let _ = widget_state;
        let _ = monitor_state;
        show_widget_context_menu_for_app(&app_handle)
    }

    #[cfg(not(target_os = "windows"))]
    {
        let _ = app_handle;
        let _ = widget_state;
        let _ = monitor_state;
        false
    }
}

#[tauri::command]
fn cmd_reset_session_counters(
    _app_handle: tauri::AppHandle,
    monitor_state: tauri::State<'_, Mutex<MonitorState>>,
) -> serde_json::Value {
    match monitor_state.lock() {
        Ok(mut state) => {
            commands::monitor::reset_session_counters(&mut state);
            serde_json::json!({
                "success": true,
                "message": "Current session reset"
            })
        }
        Err(_) => serde_json::json!({
            "success": false,
            "message": "Could not reset current session"
        }),
    }
}
