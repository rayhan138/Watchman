use serde::{Deserialize, Serialize};
use std::sync::Mutex;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UsageData {
    pub upload: u64,
    pub download: u64,
    pub total: u64,
    pub period: String,
    #[serde(rename = "startDate")]
    pub start_date: String,
    #[serde(rename = "endDate")]
    pub end_date: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ThresholdStatus {
    pub percentage: f64,
    pub level: String,
    pub remaining: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ComparisonData {
    pub current: u64,
    pub previous: u64,
    #[serde(rename = "percentageChange")]
    pub percentage_change: f64,
    pub trend: String,
}

#[tauri::command]
pub fn get_usage(
    history_state: tauri::State<'_, Mutex<super::history::HistoryState>>,
    period: String,
) -> UsageData {
    let s = history_state.lock().unwrap();
    let aggregated = s.get_aggregated(&period);
    if let Some(latest) = aggregated.first() {
        UsageData {
            upload: latest.upload,
            download: latest.download,
            total: latest.total,
            period: period.clone(),
            start_date: latest.date.clone(),
            end_date: latest.date.clone(),
        }
    } else {
        UsageData {
            upload: 0,
            download: 0,
            total: 0,
            period,
            start_date: String::new(),
            end_date: String::new(),
        }
    }
}

#[tauri::command]
pub fn set_data_limit(
    config_state: tauri::State<'_, Mutex<super::config::ConfigState>>,
    limit_bytes: u64,
) -> serde_json::Value {
    let mut s = config_state.lock().unwrap();
    s.config.data_limit = limit_bytes;
    s.config.data_limit_enabled = limit_bytes > 0;
    super::config::save_config_to_path_pub(&s.config_path, &s.config);
    serde_json::json!({ "success": true })
}

#[tauri::command]
pub fn get_remaining_allowance(
    config_state: tauri::State<'_, Mutex<super::config::ConfigState>>,
    history_state: tauri::State<'_, Mutex<super::history::HistoryState>>,
) -> u64 {
    let cs = config_state.lock().unwrap();
    if !cs.config.data_limit_enabled || cs.config.data_limit == 0 {
        return 0;
    }
    let hs = history_state.lock().unwrap();
    let aggregated = hs.get_aggregated("monthly");
    let total = aggregated.first().map(|r| r.total).unwrap_or(0);
    cs.config.data_limit.saturating_sub(total)
}

#[tauri::command]
pub fn get_data_thresholds(
    config_state: tauri::State<'_, Mutex<super::config::ConfigState>>,
    history_state: tauri::State<'_, Mutex<super::history::HistoryState>>,
) -> ThresholdStatus {
    let cs = config_state.lock().unwrap();
    if !cs.config.data_limit_enabled || cs.config.data_limit == 0 {
        return ThresholdStatus {
            percentage: 0.0,
            level: "normal".to_string(),
            remaining: 0,
        };
    }
    let hs = history_state.lock().unwrap();
    let aggregated = hs.get_aggregated("monthly");
    let total = aggregated.first().map(|r| r.total).unwrap_or(0);
    let percentage = (total as f64 / cs.config.data_limit as f64) * 100.0;
    let percentage = (percentage * 10.0).round() / 10.0;
    let remaining = cs.config.data_limit.saturating_sub(total);
    let level = if percentage >= 100.0 {
        "exceeded"
    } else if percentage >= 95.0 {
        "critical"
    } else if percentage >= 80.0 {
        "warning"
    } else {
        "normal"
    };
    ThresholdStatus {
        percentage,
        level: level.to_string(),
        remaining,
    }
}

#[tauri::command]
pub fn compare_usage(
    history_state: tauri::State<'_, Mutex<super::history::HistoryState>>,
    period: String,
) -> ComparisonData {
    let s = history_state.lock().unwrap();
    let aggregated = s.get_aggregated(&period);
    let current = aggregated.first().map(|r| r.total).unwrap_or(0);
    let previous = aggregated.get(1).map(|r| r.total).unwrap_or(0);

    let percentage_change = if previous > 0 {
        (((current as f64 - previous as f64) / previous as f64) * 1000.0).round() / 10.0
    } else {
        0.0
    };
    let trend = if current > previous {
        "up"
    } else if current < previous {
        "down"
    } else {
        "stable"
    };

    ComparisonData {
        current,
        previous,
        percentage_change,
        trend: trend.to_string(),
    }
}
