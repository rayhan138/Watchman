use serde::{Deserialize, Serialize};
use std::fs;
use std::sync::Mutex;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ExportResult {
    pub success: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub filepath: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ExportOptions {
    pub period: Option<String>,
    pub month: Option<String>,
    pub year: Option<i32>,
}

#[tauri::command]
pub fn export_csv(
    history_state: tauri::State<'_, Mutex<super::history::HistoryState>>,
    options: ExportOptions,
) -> ExportResult {
    let period = options.period.unwrap_or_else(|| "monthly".to_string());
    let month = normalize_export_month(options.month);
    let year = options.year;

    if matches!(period.as_str(), "daily" | "weekly") && month.is_none() {
        return ExportResult {
            success: false,
            filepath: None,
            error: Some("Please choose a month with saved history before exporting.".to_string()),
        };
    }

    if matches!(period.as_str(), "monthly" | "yearly") && year.is_none() {
        return ExportResult {
            success: false,
            filepath: None,
            error: Some("Please choose a year with saved history before exporting.".to_string()),
        };
    }

    let s = history_state.lock().unwrap();
    let data = s.get_aggregated_for_export(&period, month.as_deref(), year);

    if data.is_empty() {
        return ExportResult {
            success: false,
            filepath: None,
            error: Some("No data is available for the selected export range.".to_string()),
        };
    }

    let downloads_dir = dirs::download_dir().unwrap_or_else(|| std::path::PathBuf::from("."));
    let now = chrono::Local::now();
    let scope_label = get_scope_label(&period, month.as_deref(), year);
    let filename = format!(
        "traffic-report-{}-{}-{}.csv",
        period,
        scope_label,
        now.format("%Y-%m-%d_%H-%M-%S")
    );
    let filepath = downloads_dir.join(&filename);

    let mut csv = String::from("\u{FEFF}");
    csv.push_str("Period,Download (MB),Upload (MB),Total (MB),Download (Bytes),Upload (Bytes),Total (Bytes)\n");
    for record in &data {
        let upload_mb = record.upload as f64 / (1024.0 * 1024.0);
        let download_mb = record.download as f64 / (1024.0 * 1024.0);
        let total_mb = record.total as f64 / (1024.0 * 1024.0);
        csv.push_str(&format!(
            "{},{:.2},{:.2},{:.2},{},{},{}\n",
            escape_csv_field(&record.date),
            download_mb,
            upload_mb,
            total_mb,
            record.download,
            record.upload,
            record.total,
        ));
    }

    match fs::write(&filepath, &csv) {
        Ok(_) => ExportResult {
            success: true,
            filepath: Some(filepath.to_string_lossy().to_string()),
            error: None,
        },
        Err(e) => ExportResult {
            success: false,
            filepath: None,
            error: Some(e.to_string()),
        },
    }
}

fn escape_csv_field(value: &str) -> String {
    if value.contains(',') || value.contains('"') || value.contains('\n') || value.contains('\r') {
        format!("\"{}\"", value.replace('"', "\"\""))
    } else {
        value.to_string()
    }
}

fn normalize_export_month(value: Option<String>) -> Option<String> {
    value.and_then(|raw| {
        let trimmed = raw.trim();
        let is_valid = trimmed.len() == 7
            && trimmed.chars().nth(4) == Some('-')
            && trimmed.chars().enumerate().all(|(idx, ch)| {
                if idx == 4 {
                    ch == '-'
                } else {
                    ch.is_ascii_digit()
                }
            });

        if is_valid {
            Some(trimmed.to_string())
        } else {
            None
        }
    })
}

fn get_scope_label(period: &str, month: Option<&str>, year: Option<i32>) -> String {
    match period {
        "daily" | "weekly" => month.unwrap_or("range").to_string(),
        "monthly" | "yearly" => year
            .map(|value| format!("{value:04}"))
            .unwrap_or_else(|| "range".to_string()),
        _ => "range".to_string(),
    }
}
