use chrono::{Datelike, Local, NaiveDate};
use rusqlite::{params, Connection};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

const APP_DATA_DIR_NAME: &str = "Watchman";
const HISTORY_FILE_NAME: &str = "history.json";
const HISTORY_DATABASE_FILE_NAME: &str = "watchman.db";

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct TrafficRecord {
    #[serde(default)]
    pub upload: u64,
    #[serde(default)]
    pub download: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
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
    pub db_path: std::path::PathBuf,
    pub needs_save: bool,
}

impl HistoryState {
    pub fn new() -> Self {
        let save_path = get_history_save_path();
        let db_path = get_history_database_path();
        let (data, needs_save) = load_history_data(&save_path, &db_path);
        log_history_summary("loaded", &db_path, &data);

        Self {
            data,
            db_path,
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

        match read_sqlite_history_file(&self.db_path) {
            Ok(disk_data) => {
                self.data = merge_traffic_data(disk_data, self.data.clone());
            }
            Err(error) => {
                eprintln!(
                    "[history] ignoring unreadable sqlite history before save at {}: {}",
                    self.db_path.display(),
                    error
                );
            }
        }

        self.data = sanitize_traffic_data(self.data.clone());

        match write_sqlite_history_file(&self.db_path, &self.data) {
            Ok(()) => {
                log_history_summary("saved", &self.db_path, &self.data);
                self.needs_save = false;
            }
            Err(error) => {
                eprintln!(
                    "[history] failed to save sqlite history at {}: {}",
                    self.db_path.display(),
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

fn get_history_database_path() -> PathBuf {
    let app_data = dirs::data_dir()
        .or_else(dirs::config_dir)
        .unwrap_or_else(|| PathBuf::from("."));

    app_data
        .join(APP_DATA_DIR_NAME)
        .join(HISTORY_DATABASE_FILE_NAME)
}

fn load_history_data(json_path: &Path, db_path: &Path) -> (TrafficData, bool) {
    let mut needs_save = false;
    let mut data = match read_sqlite_history_file(db_path) {
        Ok(data) => data,
        Err(error) => {
            eprintln!(
                "[history] could not load sqlite history at {}: {}",
                db_path.display(),
                error
            );
            backup_unreadable_database(db_path);
            needs_save = true;
            TrafficData::default()
        }
    };

    let json_data = match read_history_file(json_path) {
        Ok(data) => Some(data),
        Err(HistoryReadError::Missing) => match read_history_backup(json_path) {
            Ok(data) if !data.records.is_empty() => {
                eprintln!(
                    "[history] recovered json backup for sqlite import at {}",
                    backup_path(json_path).display()
                );
                Some(data)
            }
            Ok(_) => None,
            Err(_) => None,
        },
        Err(error) => {
            eprintln!(
                "[history] could not load json history for sqlite import at {}: {}",
                json_path.display(),
                error
            );
            backup_unreadable_history(json_path);
            match read_history_backup(json_path) {
                Ok(data) => Some(data),
                Err(_) => None,
            }
        }
    };

    if let Some(json_data) = json_data {
        let merged = merge_traffic_data(data.clone(), json_data);
        if merged != data {
            data = merged;
            needs_save = true;
        }
    }

    if needs_save || !db_path.exists() {
        match write_sqlite_history_file(db_path, &data) {
            Ok(()) => needs_save = false,
            Err(error) => {
                eprintln!(
                    "[history] failed to write sqlite history at {}: {}",
                    db_path.display(),
                    error
                );
                needs_save = true;
            }
        }
    }

    (data, needs_save)
}

fn open_history_database(path: &Path) -> Result<Connection, String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }

    let conn = Connection::open(path).map_err(|error| error.to_string())?;
    conn.execute_batch(
        "
        PRAGMA journal_mode = WAL;
        PRAGMA synchronous = NORMAL;

        CREATE TABLE IF NOT EXISTS daily_traffic (
            date TEXT PRIMARY KEY,
            upload_bytes INTEGER NOT NULL DEFAULT 0,
            download_bytes INTEGER NOT NULL DEFAULT 0,
            updated_at TEXT NOT NULL
        );
        ",
    )
    .map_err(|error| error.to_string())?;

    Ok(conn)
}

fn read_sqlite_history_file(path: &Path) -> Result<TrafficData, String> {
    let conn = open_history_database(path)?;
    let mut statement = conn
        .prepare(
            "
            SELECT date, upload_bytes, download_bytes
            FROM daily_traffic
            ORDER BY date
            ",
        )
        .map_err(|error| error.to_string())?;

    let rows = statement
        .query_map([], |row| {
            let date: String = row.get(0)?;
            let upload_bytes: i64 = row.get(1)?;
            let download_bytes: i64 = row.get(2)?;

            Ok((
                date,
                TrafficRecord {
                    upload: signed_bytes_to_u64(upload_bytes),
                    download: signed_bytes_to_u64(download_bytes),
                },
            ))
        })
        .map_err(|error| error.to_string())?;

    let mut records = BTreeMap::new();
    for row in rows {
        let (date, record) = row.map_err(|error| error.to_string())?;
        records.insert(date, record);
    }

    Ok(sanitize_traffic_data(TrafficData { records }))
}

fn write_sqlite_history_file(path: &Path, data: &TrafficData) -> Result<(), String> {
    let mut conn = open_history_database(path)?;
    let transaction = conn.transaction().map_err(|error| error.to_string())?;
    let data = sanitize_traffic_data(data.clone());
    let updated_at = Local::now().to_rfc3339();

    {
        let mut statement = transaction
            .prepare(
                "
                INSERT INTO daily_traffic (date, upload_bytes, download_bytes, updated_at)
                VALUES (?1, ?2, ?3, ?4)
                ON CONFLICT(date) DO UPDATE SET
                    upload_bytes = max(upload_bytes, excluded.upload_bytes),
                    download_bytes = max(download_bytes, excluded.download_bytes),
                    updated_at = excluded.updated_at
                ",
            )
            .map_err(|error| error.to_string())?;

        for (date, record) in data.records {
            statement
                .execute(params![
                    date,
                    u64_to_sqlite_integer(record.upload),
                    u64_to_sqlite_integer(record.download),
                    updated_at.as_str()
                ])
                .map_err(|error| error.to_string())?;
        }
    }

    transaction.commit().map_err(|error| error.to_string())
}

fn u64_to_sqlite_integer(value: u64) -> i64 {
    value.min(i64::MAX as u64) as i64
}

fn signed_bytes_to_u64(value: i64) -> u64 {
    if value <= 0 {
        0
    } else {
        value as u64
    }
}

fn backup_unreadable_database(path: &Path) {
    backup_database_file(path);
    backup_database_file(&sqlite_sidecar_path(path, "-wal"));
    backup_database_file(&sqlite_sidecar_path(path, "-shm"));
}

fn sqlite_sidecar_path(path: &Path, suffix: &str) -> PathBuf {
    PathBuf::from(format!("{}{}", path.display(), suffix))
}

fn backup_database_file(path: &Path) {
    if !path.exists() {
        return;
    }

    let backup_path = timestamped_backup_path(path, "unreadable");
    match fs::rename(path, &backup_path) {
        Ok(()) => {
            eprintln!(
                "[history] moved unreadable sqlite file to {}",
                backup_path.display()
            );
        }
        Err(rename_error) => {
            eprintln!(
                "[history] failed to move unreadable sqlite file at {}: {}",
                path.display(),
                rename_error
            );
            if let Err(copy_error) = fs::copy(path, &backup_path) {
                eprintln!(
                    "[history] failed to copy unreadable sqlite backup to {}: {}",
                    backup_path.display(),
                    copy_error
                );
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

#[cfg(test)]
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

#[cfg(test)]
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
            db_path: PathBuf::new(),
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
            db_path: PathBuf::new(),
            needs_save: false,
        };

        let daily = state.get_aggregated("daily");

        assert_eq!(daily.len(), 1);
        assert_eq!(daily[0].date, "2026-05-05");
        assert_eq!(daily[0].total, 30);
    }

    #[test]
    fn sqlite_history_round_trips_daily_records() {
        let temp_dir = unique_test_dir("round-trip");
        let db_path = temp_dir.join("watchman.db");
        let data = history(vec![
            ("2026-05-05", record(100, 200)),
            ("2026-05-06", record(300, 400)),
        ]);

        write_sqlite_history_file(&db_path, &data).expect("sqlite write should succeed");
        let loaded = read_sqlite_history_file(&db_path).expect("sqlite read should succeed");

        assert_eq!(loaded.records.len(), 2);
        assert_eq!(loaded.records["2026-05-05"].upload, 100);
        assert_eq!(loaded.records["2026-05-06"].download, 400);

        let _ = fs::remove_dir_all(temp_dir);
    }

    #[test]
    fn sqlite_load_imports_existing_json_history() {
        let temp_dir = unique_test_dir("json-import");
        let json_path = temp_dir.join("history.json");
        let db_path = temp_dir.join("watchman.db");
        let data = history(vec![("2026-05-07", record(700, 800))]);
        write_history_file(&json_path, &data).expect("json write should succeed");

        let (loaded, needs_save) = load_history_data(&json_path, &db_path);
        let db_loaded = read_sqlite_history_file(&db_path).expect("sqlite import should exist");

        assert!(!needs_save);
        assert_eq!(loaded.records["2026-05-07"].upload, 700);
        assert_eq!(db_loaded.records["2026-05-07"].download, 800);
        assert!(
            json_path.exists(),
            "old json history should remain as backup"
        );

        let _ = fs::remove_dir_all(temp_dir);
    }

    fn unique_test_dir(label: &str) -> PathBuf {
        let mut dir = std::env::temp_dir();
        dir.push(format!(
            "watchman-history-{label}-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&dir).unwrap();
        dir
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
