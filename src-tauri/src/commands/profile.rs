use serde::{Deserialize, Serialize};
use std::sync::Mutex;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProfileInfo {
    pub id: String,
    pub name: String,
    pub description: String,
    pub icon: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProfileConfig {
    #[serde(rename = "priorityMetrics")]
    pub priority_metrics: Vec<String>,
    #[serde(rename = "notificationSettings")]
    pub notification_settings: serde_json::Value,
    #[serde(rename = "updateInterval")]
    pub update_interval: u64,
    #[serde(rename = "dashboardLayout")]
    pub dashboard_layout: serde_json::Value,
    #[serde(rename = "emphasizedMetrics")]
    pub emphasized_metrics: Vec<String>,
}

fn get_profiles_data() -> Vec<(ProfileInfo, ProfileConfig)> {
    vec![
        (
            ProfileInfo {
                id: "gaming".into(),
                name: "Gaming Mode".into(),
                description: "Optimized for online gaming with focus on latency".into(),
                icon: "🎮".into(),
            },
            ProfileConfig {
                priority_metrics: vec!["latency".into(), "ping".into()],
                notification_settings: serde_json::json!({"enabled":true,"dataUsageAlerts":false,"slowInternetAlerts":false,"connectionDropAlerts":false,"highUsageWarnings":true,"soundEnabled":true,"warningSettings":{"trafficEnabled":false,"trafficThreshold":500,"trafficUnit":"MB","memoryEnabled":true,"memoryThreshold":80,"cpuTempEnabled":true,"cpuTempThreshold":80,"gpuTempEnabled":true,"gpuTempThreshold":80,"diskTempEnabled":true,"diskTempThreshold":80,"mainboardTempEnabled":true,"mainboardTempThreshold":80}}),
                update_interval: 1000,
                dashboard_layout: serde_json::json!({"emphasizeLatency":true,"emphasizeSpeed":false,"emphasizeUsage":false,"showApplicationMonitor":true}),
                emphasized_metrics: vec!["latency".into(), "ping".into(), "jitter".into()],
            },
        ),
        (
            ProfileInfo {
                id: "streaming".into(),
                name: "Streaming Mode".into(),
                description: "Optimized for video streaming".into(),
                icon: "📺".into(),
            },
            ProfileConfig {
                priority_metrics: vec!["download-speed".into(), "connection-quality".into()],
                notification_settings: serde_json::json!({"enabled":true,"dataUsageAlerts":false,"slowInternetAlerts":false,"connectionDropAlerts":false,"highUsageWarnings":true,"soundEnabled":false,"warningSettings":{"trafficEnabled":false,"trafficThreshold":500,"trafficUnit":"MB","memoryEnabled":true,"memoryThreshold":80,"cpuTempEnabled":true,"cpuTempThreshold":80,"gpuTempEnabled":true,"gpuTempThreshold":80,"diskTempEnabled":true,"diskTempThreshold":80,"mainboardTempEnabled":true,"mainboardTempThreshold":80}}),
                update_interval: 2000,
                dashboard_layout: serde_json::json!({"emphasizeLatency":false,"emphasizeSpeed":true,"emphasizeUsage":false,"showApplicationMonitor":true}),
                emphasized_metrics: vec!["download-speed".into(), "connection-quality".into()],
            },
        ),
        (
            ProfileInfo {
                id: "work".into(),
                name: "Work Mode".into(),
                description: "Balanced monitoring for productivity".into(),
                icon: "💼".into(),
            },
            ProfileConfig {
                priority_metrics: vec![
                    "download-speed".into(),
                    "upload-speed".into(),
                    "latency".into(),
                ],
                notification_settings: serde_json::json!({"enabled":true,"dataUsageAlerts":false,"slowInternetAlerts":false,"connectionDropAlerts":false,"highUsageWarnings":true,"soundEnabled":false,"warningSettings":{"trafficEnabled":false,"trafficThreshold":500,"trafficUnit":"MB","memoryEnabled":true,"memoryThreshold":80,"cpuTempEnabled":true,"cpuTempThreshold":80,"gpuTempEnabled":true,"gpuTempThreshold":80,"diskTempEnabled":true,"diskTempThreshold":80,"mainboardTempEnabled":true,"mainboardTempThreshold":80}}),
                update_interval: 2000,
                dashboard_layout: serde_json::json!({"emphasizeLatency":false,"emphasizeSpeed":false,"emphasizeUsage":false,"showApplicationMonitor":true}),
                emphasized_metrics: vec![
                    "download-speed".into(),
                    "upload-speed".into(),
                    "latency".into(),
                ],
            },
        ),
        (
            ProfileInfo {
                id: "data-saver".into(),
                name: "Data Saver Mode".into(),
                description: "Focused on conserving data usage".into(),
                icon: "💾".into(),
            },
            ProfileConfig {
                priority_metrics: vec!["data-usage".into(), "remaining-allowance".into()],
                notification_settings: serde_json::json!({"enabled":true,"dataUsageAlerts":false,"slowInternetAlerts":false,"connectionDropAlerts":false,"highUsageWarnings":true,"soundEnabled":true,"warningSettings":{"trafficEnabled":false,"trafficThreshold":500,"trafficUnit":"MB","memoryEnabled":true,"memoryThreshold":80,"cpuTempEnabled":true,"cpuTempThreshold":80,"gpuTempEnabled":true,"gpuTempThreshold":80,"diskTempEnabled":true,"diskTempThreshold":80,"mainboardTempEnabled":true,"mainboardTempThreshold":80}}),
                update_interval: 2000,
                dashboard_layout: serde_json::json!({"emphasizeLatency":false,"emphasizeSpeed":false,"emphasizeUsage":true,"showApplicationMonitor":true}),
                emphasized_metrics: vec!["data-usage".into(), "remaining-allowance".into()],
            },
        ),
    ]
}

#[tauri::command]
pub fn get_profiles() -> Vec<ProfileInfo> {
    get_profiles_data()
        .into_iter()
        .map(|(info, _)| info)
        .collect()
}

#[tauri::command]
pub fn get_active_profile(
    config_state: tauri::State<'_, Mutex<super::config::ConfigState>>,
) -> ProfileInfo {
    let s = config_state.lock().unwrap();
    let id = &s.config.active_profile;
    get_profiles_data()
        .into_iter()
        .find(|(info, _)| info.id == *id)
        .map(|(info, _)| info)
        .unwrap_or_else(|| ProfileInfo {
            id: "work".into(),
            name: "Work Mode".into(),
            description: "Balanced".into(),
            icon: "💼".into(),
        })
}

#[tauri::command]
pub fn set_active_profile(
    config_state: tauri::State<'_, Mutex<super::config::ConfigState>>,
    profile_id: String,
) -> ProfileInfo {
    {
        let mut s = config_state.lock().unwrap();
        s.config.active_profile = profile_id.clone();
        super::config::save_config_to_path_pub(&s.config_path, &s.config);
    }
    get_active_profile(config_state)
}

#[tauri::command]
pub fn get_profile_config(profile_id: String) -> ProfileConfig {
    get_profiles_data()
        .into_iter()
        .find(|(info, _)| info.id == profile_id)
        .map(|(_, cfg)| cfg)
        .unwrap_or_else(|| get_profiles_data()[2].1.clone())
}
