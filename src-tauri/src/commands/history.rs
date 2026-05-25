use chrono::{Datelike, Local, NaiveDate};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

const APP_DATA_DIR_NAME: &str = "Watchman";
const HISTORY_FILE_NAME: &str = "history.json";

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TrafficRecord {
    #[serde(default)]
    pub upload: u64,
    #[serde(default)]
    pub download: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TrafficData {
    #[serde(default)]
    pub records: BTreeMap<String, TrafficRecord>,
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
        let save_path = get_history_save_path();
        let (data, needs_save) = load_history_data(&save_path);
        log_history_summary("loaded", &save_path, &data);

        Self {
            data,
            save_path,
            needs_save,
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
        record.upload = record.upload.saturating_add(upload_bytes);
        record.download = record.download.saturating_add(download_bytes);
        self.needs_save = true;
    }

    pub fn save(&mut self) {
        if !self.needs_save {
            return;
        }

        match read_history_file(&self.save_path) {
            Ok(disk_data) => {
                self.data = merge_traffic_data(disk_data, self.data.clone());
            }
            Err(HistoryReadError::Missing) => {}
            Err(error) => {
                eprintln!(
                    "[history] ignoring unreadable history before save at {}: {}",
                    self.save_path.display(),
                    error
                );
            }
        }

        self.data = sanitize_traffic_data(self.data.clone());

        match write_history_file(&self.save_path, &self.data) {
            Ok(()) => {
                log_history_summary("saved", &self.save_path, &self.data);
                self.needs_save = false;
            }
            Err(error) => {
                eprintln!(
                    "[history] failed to save history at {}: {}",
                    self.save_path.display(),
                    error
                );
            }
        }
    }

    pub fn get_aggregated(&self, view_type: &str) -> Vec<AggregatedRecord> {
        let mut result: BTreeMap<String, (u64, u64)> = BTreeMap::new();

        for (date_str, rec) in &self.data.records {
            let Some(key) = get_period_key(date_str, view_type) else {
                eprintln!("[history] skipping invalid history date: {date_str}");
                continue;
            };
            let entry = result.entry(key).or_insert((0, 0));
            entry.0 = entry.0.saturating_add(rec.upload);
            entry.1 = entry.1.saturating_add(rec.download);
        }

        let mut records: Vec<AggregatedRecord> = result
            .into_iter()
            .map(|(date, (up, down))| AggregatedRecord {
                date,
                upload: up,
                download: down,
                total: up.saturating_add(down),
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

            let Some(key) = get_period_key(date_str, view_type) else {
                eprintln!("[history] skipping invalid export date: {date_str}");
                continue;
            };

            let entry = result.entry(key).or_insert((0, 0));
            entry.0 = entry.0.saturating_add(rec.upload);
            entry.1 = entry.1.saturating_add(rec.download);
        }

        let mut records: Vec<AggregatedRecord> = result
            .into_iter()
            .map(|(date, (up, down))| AggregatedRecord {
                date,
                upload: up,
                download: down,
                total: up.saturating_add(down),
            })
            .collect();

        records.sort_by(|a, b| a.date.cmp(&b.date));
        records
    }
}

#[derive(Debug)]
enum HistoryReadError {
    Missing,
    Read(String),
    Parse(String),
}

impl std::fmt::Display for HistoryReadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            HistoryReadError::Missing => write!(f, "missing"),
            HistoryReadError::Read(message) => write!(f, "read error: {message}"),
            HistoryReadError::Parse(message) => write!(f, "parse error: {message}"),
        }
    }
}

fn get_history_save_path() -> PathBuf {
    let app_data = dirs::data_dir()
        .or_else(dirs::config_dir)
        .unwrap_or_else(|| PathBuf::from("."));

    app_data.join(APP_DATA_DIR_NAME).join(HISTORY_FILE_NAME)
}

fn load_history_data(save_path: &Path) -> (TrafficData, bool) {
    match read_history_file(save_path) {
        Ok(data) => (data, false),
        Err(HistoryReadError::Missing) => match read_history_backup(save_path) {
            Ok(data) => {
                eprintln!(
                    "[history] recovered missing history from backup at {}",
                    backup_path(save_path).display()
                );
                (data, true)
            }
            Err(_) => {
                let data = TrafficData::default();
                if let Err(error) = write_history_file(save_path, &data) {
                    eprintln!(
                        "[history] failed to create empty history at {}: {}",
                        save_path.display(),
                        error
                    );
                    return (data, true);
                }
                (data, false)
            }
        },
        Err(error) => {
            eprintln!(
                "[history] could not load history at {}: {}",
                save_path.display(),
                error
            );
            backup_unreadable_history(save_path);

            match read_history_backup(save_path) {
                Ok(data) => {
                    eprintln!(
                        "[history] recovered unreadable history from backup at {}",
                        backup_path(save_path).display()
                    );
                    (data, true)
                }
                Err(_) => (TrafficData::default(), true),
            }
        }
    }
}

fn read_history_backup(save_path: &Path) -> Result<TrafficData, HistoryReadError> {
    read_history_file(&backup_path(save_path))
}

fn read_history_file(path: &Path) -> Result<TrafficData, HistoryReadError> {
    let content = fs::read_to_string(path).map_err(|error| {
        if error.kind() == io::ErrorKind::NotFound {
            HistoryReadError::Missing
        } else {
            HistoryReadError::Read(error.to_string())
        }
    })?;

    let data: TrafficData = serde_json::from_str(&content)
        .map_err(|error| HistoryReadError::Parse(error.to_string()))?;

    Ok(sanitize_traffic_data(data))
}

fn write_history_file(path: &Path, data: &TrafficData) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }

    let tmp_path = temp_path(path);
    let json = serde_json::to_string_pretty(data).map_err(|error| error.to_string())?;
    fs::write(&tmp_path, json).map_err(|error| error.to_string())?;

    read_history_file(&tmp_path).map_err(|error| {
        let _ = fs::remove_file(&tmp_path);
        format!("temporary history verification failed: {error}")
    })?;

    if path.exists() {
        fs::copy(path, backup_path(path)).map_err(|error| error.to_string())?;
        fs::remove_file(path).map_err(|error| error.to_string())?;
    }

    fs::rename(&tmp_path, path).or_else(|rename_error| {
        fs::copy(&tmp_path, path)
            .map(|_| ())
            .map_err(|copy_error| {
                format!("rename failed: {rename_error}; copy fallback failed: {copy_error}")
            })
            .and_then(|_| fs::remove_file(&tmp_path).map_err(|error| error.to_string()))
    })
}

fn backup_unreadable_history(path: &Path) {
    if !path.exists() {
        return;
    }

    let backup_path = timestamped_backup_path(path, "unreadable");
    match fs::rename(path, &backup_path) {
        Ok(()) => {
            eprintln!(
                "[history] moved unreadable history to {}",
                backup_path.display()
            );
        }
        Err(rename_error) => {
            eprintln!(
                "[history] failed to move unreadable history at {}: {}",
                path.display(),
                rename_error
            );
            if let Err(copy_error) = fs::copy(path, &backup_path) {
                eprintln!(
                    "[history] failed to copy unreadable history backup to {}: {}",
                    backup_path.display(),
                    copy_error
                );
            }
        }
    }
}

fn backup_path(path: &Path) -> PathBuf {
    path.with_extension("json.bak")
}

fn temp_path(path: &Path) -> PathBuf {
    path.with_extension("json.tmp")
}

fn timestamped_backup_path(path: &Path, label: &str) -> PathBuf {
    let timestamp = Local::now().format("%Y%m%d-%H%M%S");
    let file_name = path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or(HISTORY_FILE_NAME);

    path.with_file_name(format!("{file_name}.{label}.{timestamp}.bak"))
}

fn sanitize_traffic_data(data: TrafficData) -> TrafficData {
    let mut records = BTreeMap::new();

    for (date, record) in data.records {
        if parse_history_date(&date).is_none() {
            eprintln!("[history] dropping invalid history date while loading: {date}");
            continue;
        }

        records.insert(date, record);
    }

    TrafficData { records }
}

fn merge_traffic_data(existing: TrafficData, incoming: TrafficData) -> TrafficData {
    let mut merged = sanitize_traffic_data(existing);

    for (date, incoming_record) in sanitize_traffic_data(incoming).records {
        let record = merged.records.entry(date).or_default();
        record.upload = record.upload.max(incoming_record.upload);
        record.download = record.download.max(incoming_record.download);
    }

    merged
}

fn log_history_summary(action: &str, path: &Path, data: &TrafficData) {
    let count = data.records.len();
    let oldest = data
        .records
        .keys()
        .next()
        .map(String::as_str)
        .unwrap_or("-");
    let newest = data
        .records
        .keys()
        .next_back()
        .map(String::as_str)
        .unwrap_or("-");

    eprintln!(
        "[history] {action}: path={} records={} range={}..{}",
        path.display(),
        count,
        oldest,
        newest
    );
}

fn get_today_string() -> String {
    Local::now().format("%Y-%m-%d").to_string()
}

fn parse_history_date(date_str: &str) -> Option<NaiveDate> {
    NaiveDate::parse_from_str(date_str, "%Y-%m-%d").ok()
}

fn get_period_key(date_str: &str, view_type: &str) -> Option<String> {
    let date = parse_history_date(date_str)?;
    let key = match view_type {
        "weekly" => {
            let iso_week = date.iso_week();
            format!("{}-W{:02}", iso_week.year(), iso_week.week())
        }
        "monthly" => format!("{:04}-{:02}", date.year(), date.month()),
        "yearly" => format!("{:04}", date.year()),
        _ => date.format("%Y-%m-%d").to_string(),
    };

    Some(key)
}

fn matches_export_scope(
    date_str: &str,
    view_type: &str,
    month: Option<&str>,
    year: Option<i32>,
) -> bool {
    let Some(date) = parse_history_date(date_str) else {
        return false;
    };

    match view_type {
        "daily" | "weekly" => month.is_some_and(|value| date_str.starts_with(value)),
        "monthly" | "yearly" => year.is_some_and(|value| date.year() == value),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record(upload: u64, download: u64) -> TrafficRecord {
        TrafficRecord { upload, download }
    }

    fn history(records: Vec<(&str, TrafficRecord)>) -> TrafficData {
        TrafficData {
            records: records
                .into_iter()
                .map(|(date, record)| (date.to_string(), record))
                .collect(),
        }
    }

    #[test]
    fn merge_preserves_older_dates_and_larger_totals() {
        let existing = history(vec![
            ("2026-05-05", record(100, 200)),
            ("2026-05-10", record(500, 600)),
        ]);
        let incoming = history(vec![
            ("2026-05-10", record(300, 900)),
            ("2026-05-11", record(50, 70)),
        ]);

        let merged = merge_traffic_data(existing, incoming);

        assert_eq!(merged.records.len(), 3);
        assert_eq!(merged.records["2026-05-05"].upload, 100);
        assert_eq!(merged.records["2026-05-10"].upload, 500);
        assert_eq!(merged.records["2026-05-10"].download, 900);
        assert_eq!(merged.records["2026-05-11"].download, 70);
    }

    #[test]
    fn daily_history_returns_all_valid_days() {
        let state = HistoryState {
            data: history(vec![
                ("2026-05-05", record(10, 20)),
                ("2026-05-10", record(30, 40)),
                ("2026-05-11", record(50, 60)),
            ]),
            save_path: PathBuf::new(),
            needs_save: false,
        };

        let daily = state.get_aggregated("daily");

        assert_eq!(daily.len(), 3);
        assert_eq!(daily[0].date, "2026-05-11");
        assert_eq!(daily[1].date, "2026-05-10");
        assert_eq!(daily[2].date, "2026-05-05");
    }

    #[test]
    fn invalid_dates_do_not_break_aggregation() {
        let state = HistoryState {
            data: history(vec![
                ("2026-05-05", record(10, 20)),
                ("bad-date", record(30, 40)),
            ]),
            save_path: PathBuf::new(),
            needs_save: false,
        };

        let daily = state.get_aggregated("daily");

        assert_eq!(daily.len(), 1);
        assert_eq!(daily[0].date, "2026-05-05");
        assert_eq!(daily[0].total, 30);
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
