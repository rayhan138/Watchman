use chrono::Datelike;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::sync::Mutex;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrafficRecord {
    pub upload: u64,
    pub download: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrafficData {
    pub records: BTreeMap<String, TrafficRecord>,
}

impl Default for TrafficData {
    fn default() -> Self {
        Self {
            records: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AggregatedRecord {
    pub date: String,
    pub upload: u64,
    pub download: u64,
    pub total: u64,
}

pub struct HistoryState {
    pub data: TrafficData,
    pub save_path: std::path::PathBuf,
    pub needs_save: bool,
}

impl HistoryState {
    pub fn new() -> Self {
        // Use same path as Electron app for backward compat
        let app_data = dirs::data_dir()
            .or_else(|| dirs::config_dir())
            .unwrap_or_else(|| std::path::PathBuf::from("."));
        // Electron stores in userData which is AppData/Roaming/electron-app
        let electron_path = app_data.join("electron-app").join("traffic_history.json");
        let tauri_path = app_data
            .join("traffic-monitor")
            .join("traffic_history.json");

        // Try Electron path first for backward compat, then Tauri path
        let save_path = if electron_path.exists() {
            electron_path
        } else {
            let _ = fs::create_dir_all(
                tauri_path
                    .parent()
                    .unwrap_or(&std::path::PathBuf::from(".")),
            );
            tauri_path
        };

        let data = match fs::read_to_string(&save_path) {
            Ok(content) => serde_json::from_str(&content).unwrap_or_default(),
            Err(_) => TrafficData::default(),
        };

        Self {
            data,
            save_path,
            needs_save: false,
        }
    }

    pub fn add_traffic(&mut self, upload_bytes: u64, download_bytes: u64) {
        if upload_bytes == 0 && download_bytes == 0 {
            return;
        }
        let today = get_today_string();
        let record = self.data.records.entry(today).or_insert(TrafficRecord {
            upload: 0,
            download: 0,
        });
        record.upload += upload_bytes;
        record.download += download_bytes;
        self.needs_save = true;
    }

    pub fn save(&mut self) {
        if !self.needs_save {
            return;
        }
        if let Some(parent) = self.save_path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let _ = fs::write(
            &self.save_path,
            serde_json::to_string(&self.data).unwrap_or_default(),
        );
        self.needs_save = false;
    }

    pub fn get_aggregated(&self, view_type: &str) -> Vec<AggregatedRecord> {
        let mut result: BTreeMap<String, (u64, u64)> = BTreeMap::new();

        for (date_str, rec) in &self.data.records {
            let key = match view_type {
                "weekly" => get_week_string(date_str),
                "monthly" => date_str[..7].to_string(), // YYYY-MM
                "yearly" => date_str[..4].to_string(),  // YYYY
                _ => date_str.clone(),                  // daily
            };
            let entry = result.entry(key).or_insert((0, 0));
            entry.0 += rec.upload;
            entry.1 += rec.download;
        }

        let mut records: Vec<AggregatedRecord> = result
            .into_iter()
            .map(|(date, (up, down))| AggregatedRecord {
                date,
                upload: up,
                download: down,
                total: up + down,
            })
            .collect();

        records.sort_by(|a, b| b.date.cmp(&a.date));
        records
    }

    pub fn get_aggregated_for_export(
        &self,
        view_type: &str,
        month: Option<&str>,
        year: Option<i32>,
    ) -> Vec<AggregatedRecord> {
        let mut result: BTreeMap<String, (u64, u64)> = BTreeMap::new();

        for (date_str, rec) in &self.data.records {
            if !matches_export_scope(date_str, view_type, month, year) {
                continue;
            }

            let key = match view_type {
                "weekly" => get_week_string(date_str),
                "monthly" => date_str[..7].to_string(), // YYYY-MM
                "yearly" => date_str[..4].to_string(),  // YYYY
                _ => date_str.clone(),                  // daily
            };

            let entry = result.entry(key).or_insert((0, 0));
            entry.0 += rec.upload;
            entry.1 += rec.download;
        }

        let mut records: Vec<AggregatedRecord> = result
            .into_iter()
            .map(|(date, (up, down))| AggregatedRecord {
                date,
                upload: up,
                download: down,
                total: up + down,
            })
            .collect();

        records.sort_by(|a, b| a.date.cmp(&b.date));
        records
    }
}

fn get_today_string() -> String {
    chrono::Local::now().format("%Y-%m-%d").to_string()
}

fn get_week_string(date_str: &str) -> String {
    if let Ok(date) = chrono::NaiveDate::parse_from_str(date_str, "%Y-%m-%d") {
        let iso_week = date.iso_week();
        format!("{}-W{:02}", iso_week.year(), iso_week.week())
    } else {
        date_str.to_string()
    }
}

fn matches_export_scope(
    date_str: &str,
    view_type: &str,
    month: Option<&str>,
    year: Option<i32>,
) -> bool {
    match view_type {
        "daily" | "weekly" => month.is_some_and(|value| date_str.starts_with(value)),
        "monthly" | "yearly" => year.is_some_and(|value| {
            let year_prefix = format!("{value:04}-");
            date_str.starts_with(&year_prefix)
        }),
        _ => false,
    }
}

#[tauri::command]
pub fn get_traffic_history(
    state: tauri::State<'_, Mutex<HistoryState>>,
    view_type: Option<String>,
) -> Vec<AggregatedRecord> {
    let s = state.lock().unwrap();
    s.get_aggregated(&view_type.unwrap_or_else(|| "daily".to_string()))
}
