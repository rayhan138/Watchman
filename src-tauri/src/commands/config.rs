use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::sync::Mutex;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    #[serde(rename = "startOnBoot", default = "default_true")]
    pub start_on_boot: bool,
    #[serde(rename = "unitModeBits", default)]
    pub unit_mode_bits: bool,
    #[serde(rename = "hideGauges", default)]
    pub hide_gauges: bool,
    #[serde(rename = "memoryWarningEnabled", default)]
    pub memory_warning_enabled: bool,
    #[serde(rename = "memoryWarningThreshold", default = "default_80")]
    pub memory_warning_threshold: u32,
    #[serde(default = "default_system")]
    pub theme: String,
    #[serde(rename = "speedTest", default)]
    pub speed_test: SpeedTestConfig,
    #[serde(rename = "dataLimit", default)]
    pub data_limit: u64,
    #[serde(rename = "dataLimitEnabled", default)]
    pub data_limit_enabled: bool,
    #[serde(rename = "currentPeriodStart", default = "default_now_str")]
    pub current_period_start: String,
    #[serde(default = "default_notifications")]
    pub notifications: NotificationConfig,
    #[serde(rename = "activeProfile", default = "default_work")]
    pub active_profile: String,
    #[serde(rename = "networkHealth", default)]
    pub network_health: NetworkHealthConfig,
    #[serde(rename = "applicationMonitor", default)]
    pub application_monitor: AppMonitorConfig,
    #[serde(default)]
    pub troubleshooter: TroubleshooterConfig,
    #[serde(default)]
    pub export: ExportConfig,
    #[serde(flatten)]
    pub extra: HashMap<String, serde_json::Value>,
}

fn default_true() -> bool {
    true
}
fn default_false() -> bool {
    false
}
fn default_80() -> u32 {
    80
}
fn default_500() -> u32 {
    500
}
fn default_system() -> String {
    "system".to_string()
}
fn default_work() -> String {
    "work".to_string()
}
fn default_now_str() -> String {
    chrono::Utc::now().to_rfc3339()
}

fn default_notifications() -> NotificationConfig {
    NotificationConfig {
        enabled: true,
        data_usage_alerts: false,
        slow_internet_alerts: false,
        connection_drop_alerts: false,
        high_usage_warnings: true,
        sound_enabled: true,
        warning_settings: HighUsageWarningConfig::default(),
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct SpeedTestConfig {
    #[serde(rename = "lastRun", default)]
    pub last_run: u64,
    #[serde(default)]
    pub results: Vec<serde_json::Value>,
    #[serde(rename = "lastServerIndex", default)]
    pub last_server_index: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NotificationConfig {
    #[serde(default = "default_true")]
    pub enabled: bool,
    #[serde(rename = "dataUsageAlerts", default = "default_false")]
    pub data_usage_alerts: bool,
    #[serde(rename = "slowInternetAlerts", default = "default_false")]
    pub slow_internet_alerts: bool,
    #[serde(rename = "connectionDropAlerts", default = "default_false")]
    pub connection_drop_alerts: bool,
    #[serde(rename = "highUsageWarnings", default = "default_true")]
    pub high_usage_warnings: bool,
    #[serde(rename = "soundEnabled", default = "default_true")]
    pub sound_enabled: bool,
    #[serde(rename = "warningSettings", default)]
    pub warning_settings: HighUsageWarningConfig,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HighUsageWarningConfig {
    #[serde(rename = "trafficEnabled", default = "default_false")]
    pub traffic_enabled: bool,
    #[serde(rename = "trafficThreshold", default = "default_500")]
    pub traffic_threshold: u32,
    #[serde(rename = "trafficUnit", default = "default_mb")]
    pub traffic_unit: String,
    #[serde(rename = "memoryEnabled", default = "default_true")]
    pub memory_enabled: bool,
    #[serde(rename = "memoryThreshold", default = "default_80")]
    pub memory_threshold: u32,
    #[serde(rename = "cpuTempEnabled", default = "default_true")]
    pub cpu_temp_enabled: bool,
    #[serde(rename = "cpuTempThreshold", default = "default_80")]
    pub cpu_temp_threshold: u32,
    #[serde(rename = "gpuTempEnabled", default = "default_true")]
    pub gpu_temp_enabled: bool,
    #[serde(rename = "gpuTempThreshold", default = "default_80")]
    pub gpu_temp_threshold: u32,
    #[serde(rename = "diskTempEnabled", default = "default_true")]
    pub disk_temp_enabled: bool,
    #[serde(rename = "diskTempThreshold", default = "default_80")]
    pub disk_temp_threshold: u32,
    #[serde(rename = "mainboardTempEnabled", default = "default_true")]
    pub mainboard_temp_enabled: bool,
    #[serde(rename = "mainboardTempThreshold", default = "default_80")]
    pub mainboard_temp_threshold: u32,
}

impl Default for HighUsageWarningConfig {
    fn default() -> Self {
        Self {
            traffic_enabled: false,
            traffic_threshold: 500,
            traffic_unit: default_mb(),
            memory_enabled: true,
            memory_threshold: 80,
            cpu_temp_enabled: true,
            cpu_temp_threshold: 80,
            gpu_temp_enabled: true,
            gpu_temp_threshold: 80,
            disk_temp_enabled: true,
            disk_temp_threshold: 80,
            mainboard_temp_enabled: true,
            mainboard_temp_threshold: 80,
        }
    }
}

fn default_mb() -> String {
    "MB".to_string()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkHealthConfig {
    #[serde(rename = "latencyTarget", default = "default_dns")]
    pub latency_target: String,
    #[serde(rename = "updateInterval", default = "default_5000")]
    pub update_interval: u64,
}

impl Default for NetworkHealthConfig {
    fn default() -> Self {
        Self {
            latency_target: "8.8.8.8".to_string(),
            update_interval: 5000,
        }
    }
}

fn default_dns() -> String {
    "8.8.8.8".to_string()
}
fn default_5000() -> u64 {
    5000
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppMonitorConfig {
    #[serde(default = "default_true")]
    pub enabled: bool,
    #[serde(rename = "updateInterval", default = "default_5000")]
    pub update_interval: u64,
    #[serde(rename = "showIcons", default = "default_true")]
    pub show_icons: bool,
}

impl Default for AppMonitorConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            update_interval: 5000,
            show_icons: true,
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TroubleshooterConfig {
    #[serde(rename = "autoRunOnConnectionIssues", default)]
    pub auto_run_on_connection_issues: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExportConfig {
    #[serde(rename = "defaultFormat", default = "default_pdf")]
    pub default_format: String,
    #[serde(rename = "includeCharts", default = "default_true")]
    pub include_charts: bool,
}

impl Default for ExportConfig {
    fn default() -> Self {
        Self {
            default_format: "pdf".to_string(),
            include_charts: true,
        }
    }
}

fn default_pdf() -> String {
    "pdf".to_string()
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            start_on_boot: true,
            unit_mode_bits: false,
            hide_gauges: false,
            memory_warning_enabled: true,
            memory_warning_threshold: 80,
            theme: "system".to_string(),
            speed_test: SpeedTestConfig::default(),
            data_limit: 0,
            data_limit_enabled: false,
            current_period_start: chrono::Utc::now().to_rfc3339(),
            notifications: default_notifications(),
            active_profile: "work".to_string(),
            network_health: NetworkHealthConfig::default(),
            application_monitor: AppMonitorConfig::default(),
            troubleshooter: TroubleshooterConfig::default(),
            export: ExportConfig::default(),
            extra: HashMap::new(),
        }
    }
}

pub struct ConfigState {
    pub config: AppConfig,
    pub undo_history: Vec<AppConfig>,
    pub config_path: std::path::PathBuf,
}

impl ConfigState {
    pub fn new() -> Self {
        let config_dir = get_config_dir();
        let config_path = config_dir.join("settings.json");
        let config = load_config_from_path(&config_path);
        Self {
            config,
            undo_history: Vec::new(),
            config_path,
        }
    }
}

fn get_config_dir() -> std::path::PathBuf {
    let base = dirs::config_dir().unwrap_or_else(|| std::path::PathBuf::from("."));
    let dir = base.join("traffic-monitor");
    let _ = fs::create_dir_all(&dir);
    dir
}

fn load_config_from_path(path: &std::path::Path) -> AppConfig {
    match fs::read_to_string(path) {
        Ok(content) => serde_json::from_str(&content).unwrap_or_default(),
        Err(_) => {
            let default = AppConfig::default();
            let _ = fs::write(
                path,
                serde_json::to_string_pretty(&default).unwrap_or_default(),
            );
            default
        }
    }
}

fn save_config_to_path(path: &std::path::Path, config: &AppConfig) {
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let _ = fs::write(
        path,
        serde_json::to_string_pretty(config).unwrap_or_default(),
    );
}

pub fn save_config_to_path_pub(path: &std::path::Path, config: &AppConfig) {
    save_config_to_path(path, config);
}

#[tauri::command]
pub fn get_config(state: tauri::State<'_, Mutex<ConfigState>>) -> serde_json::Value {
    let s = state.lock().unwrap();
    serde_json::to_value(&s.config).unwrap_or_default()
}

#[tauri::command]
pub fn save_config(
    app_handle: tauri::AppHandle,
    state: tauri::State<'_, Mutex<ConfigState>>,
    new_config: serde_json::Value,
) -> serde_json::Value {
    let mut s = state.lock().unwrap();
    // Push current to undo
    if s.undo_history.len() >= 10 {
        s.undo_history.remove(0);
    }
    let config_clone = s.config.clone();
    s.undo_history.push(config_clone);

    // Merge new values into existing config
    let mut current_val = serde_json::to_value(&s.config).unwrap_or_default();
    if let (Some(current_obj), Some(new_obj)) =
        (current_val.as_object_mut(), new_config.as_object())
    {
        for (k, v) in new_obj {
            current_obj.insert(k.clone(), v.clone());
        }
    }
    s.config = serde_json::from_value(current_val.clone()).unwrap_or_default();
    save_config_to_path(&s.config_path, &s.config);

    // Enforce autostart matching config
    {
        use tauri_plugin_autostart::ManagerExt;
        let autostart_mgr = app_handle.autolaunch();
        if s.config.start_on_boot {
            let _ = autostart_mgr.enable();
        } else {
            let _ = autostart_mgr.disable();
        }
    }

    current_val
}

#[tauri::command]
pub fn apply_recommended_settings(
    state: tauri::State<'_, Mutex<ConfigState>>,
) -> serde_json::Value {
    let mut s = state.lock().unwrap();
    let config_clone = s.config.clone();
    s.undo_history.push(config_clone);

    s.config.unit_mode_bits = false;
    s.config.hide_gauges = false;
    s.config.memory_warning_enabled = true;
    s.config.memory_warning_threshold = 80;
    s.config.notifications = default_notifications();
    s.config.notifications.sound_enabled = false;
    s.config.notifications.warning_settings = HighUsageWarningConfig::default();
    s.config.theme = "system".to_string();
    s.config.active_profile = "work".to_string();

    save_config_to_path(&s.config_path, &s.config);
    serde_json::json!({ "success": true, "applied": serde_json::to_value(&s.config).unwrap_or_default() })
}

#[tauri::command]
pub fn undo_settings(state: tauri::State<'_, Mutex<ConfigState>>) -> serde_json::Value {
    let mut s = state.lock().unwrap();
    if let Some(prev) = s.undo_history.pop() {
        s.config = prev;
        save_config_to_path(&s.config_path, &s.config);
        serde_json::json!({ "success": true, "restored": serde_json::to_value(&s.config).unwrap_or_default() })
    } else {
        serde_json::json!({ "success": false, "reason": "Nothing to undo" })
    }
}

#[tauri::command]
pub fn get_undo_history(state: tauri::State<'_, Mutex<ConfigState>>) -> Vec<serde_json::Value> {
    let s = state.lock().unwrap();
    s.undo_history
        .iter()
        .enumerate()
        .map(|(i, _)| serde_json::json!({ "index": i, "timestamp": "saved" }))
        .collect()
}

#[tauri::command]
pub fn can_undo_settings(state: tauri::State<'_, Mutex<ConfigState>>) -> bool {
    let s = state.lock().unwrap();
    !s.undo_history.is_empty()
}
