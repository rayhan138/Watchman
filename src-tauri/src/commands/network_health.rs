use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashMap;
use std::process::Command;
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tokio_tungstenite::{
    connect_async,
    tungstenite::{client::IntoClientRequest, protocol::Message},
};

use super::config::{save_config_to_path_pub, ConfigState};

#[cfg(windows)]
use std::os::windows::process::CommandExt;

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;

const DEFAULT_LATENCY_TARGET: &str = "8.8.8.8";
const LATENCY_SAMPLE_COUNT: u32 = 5;
const LATENCY_TIMEOUT_MS: u64 = 1000;
const MLAB_LOCATE_URL: &str = "https://locate.measurementlab.net/v2/nearest/ndt/ndt7";
const MLAB_SOURCE_LABEL: &str = "M-Lab NDT7";
const NDT7_WEBSOCKET_PROTOCOL: &str = "net.measurementlab.ndt.v7";
const NDT7_DOWNLOAD_WINDOW: Duration = Duration::from_secs(10);
const NDT7_UPLOAD_WINDOW: Duration = Duration::from_secs(10);
const NDT7_CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const NDT7_MESSAGE_TIMEOUT: Duration = Duration::from_secs(18);
const NDT7_UPLOAD_CHUNK_SIZE: usize = 64 * 1024;

fn hidden_cmd(program: &str) -> Command {
    let mut cmd = Command::new(program);
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    cmd
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct QualityStatus {
    pub level: String,
    pub color: String,
    #[serde(rename = "downloadSpeed")]
    pub download_speed: f64,
    pub latency: u64,
    pub timestamp: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LatencyResult {
    pub latency: u64,
    pub label: String,
    #[serde(rename = "jitter")]
    pub jitter: u64,
    #[serde(rename = "packetLoss")]
    pub packet_loss: f64,
    #[serde(rename = "samplesSent")]
    pub samples_sent: u32,
    #[serde(rename = "samplesReceived")]
    pub samples_received: u32,
    #[serde(rename = "minLatency")]
    pub min_latency: u64,
    #[serde(rename = "maxLatency")]
    pub max_latency: u64,
    #[serde(rename = "averageLatency")]
    pub average_latency: u64,
    pub target: String,
    pub timestamp: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SignalStrength {
    pub percentage: u32,
    pub bars: u32,
    pub quality: String,
    #[serde(rename = "connectionType")]
    pub connection_type: String,
    #[serde(rename = "adapterName")]
    pub adapter_name: String,
    #[serde(rename = "adapterDescription")]
    pub adapter_description: String,
    pub ssid: String,
    #[serde(rename = "linkSpeed")]
    pub link_speed: String,
    #[serde(rename = "localIp")]
    pub local_ip: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkHealthSummary {
    pub level: String,
    pub color: String,
    pub subtitle: String,
    pub timestamp: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkOverview {
    pub health: NetworkHealthSummary,
    pub latency: LatencyResult,
    pub connection: SignalStrength,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SpeedTestResult {
    #[serde(rename = "downloadSpeed")]
    pub download_speed: f64,
    #[serde(rename = "uploadSpeed")]
    pub upload_speed: f64,
    #[serde(rename = "downloadMbps")]
    pub download_mbps: f64,
    #[serde(rename = "uploadMbps")]
    pub upload_mbps: f64,
    pub ping: u64,
    pub timestamp: u64,
    pub server: String,
    #[serde(rename = "serverLabel")]
    pub server_label: String,
    pub source: String,
    #[serde(rename = "serverCity")]
    pub server_city: String,
    #[serde(rename = "serverCountry")]
    pub server_country: String,
    #[serde(rename = "dataUsedMb")]
    pub data_used_mb: f64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct AdapterSnapshot {
    name: String,
    description: String,
    #[serde(default)]
    physical_medium: String,
    link_speed: String,
    local_ip: String,
}

#[derive(Debug, Clone)]
struct PingStats {
    average: u64,
    min: u64,
    max: u64,
    jitter: u64,
    sent: u32,
    received: u32,
    packet_loss: f64,
}

#[derive(Debug, Deserialize)]
struct MlabLocateResponse {
    results: Vec<MlabLocateResult>,
}

#[derive(Debug, Deserialize)]
struct MlabLocateResult {
    machine: String,
    #[serde(default)]
    hostname: String,
    location: MlabLocation,
    urls: HashMap<String, String>,
}

#[derive(Debug, Default, Deserialize)]
struct MlabLocation {
    #[serde(default)]
    city: String,
    #[serde(default)]
    country: String,
}

#[derive(Debug, Clone)]
struct MlabServer {
    machine: String,
    hostname: String,
    city: String,
    country: String,
    download_url: String,
    upload_url: String,
}

impl MlabServer {
    fn host_label(&self) -> String {
        if self.hostname.is_empty() {
            self.machine.clone()
        } else {
            self.hostname.clone()
        }
    }

    fn label(&self) -> String {
        match (self.city.is_empty(), self.country.is_empty()) {
            (false, false) => format!("{}, {} · {}", self.city, self.country, MLAB_SOURCE_LABEL),
            (false, true) => format!("{} · {}", self.city, MLAB_SOURCE_LABEL),
            (true, false) => format!("{} · {}", self.country, MLAB_SOURCE_LABEL),
            (true, true) => MLAB_SOURCE_LABEL.to_string(),
        }
    }
}

#[derive(Debug, Default)]
struct MlabTransferStats {
    mbps: f64,
    bytes: u64,
    min_rtt_ms: Option<u64>,
}

#[tauri::command]
pub fn get_quality(download_speed: f64, latency: u64) -> QualityStatus {
    let level = if download_speed > 10.0 && latency < 50 {
        "good"
    } else if download_speed < 1.0 || latency > 150 {
        "poor"
    } else {
        "fair"
    };
    let color = match level {
        "good" => "green",
        "fair" => "yellow",
        _ => "red",
    };
    QualityStatus {
        level: level.to_string(),
        color: color.to_string(),
        download_speed,
        latency,
        timestamp: now_ms(),
    }
}

#[tauri::command]
pub async fn measure_latency(
    config_state: tauri::State<'_, Mutex<ConfigState>>,
) -> Result<LatencyResult, String> {
    let target = configured_latency_target(&config_state);
    tokio::task::spawn_blocking(move || build_latency_result(&target))
        .await
        .map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn get_signal_strength() -> Result<SignalStrength, String> {
    tokio::task::spawn_blocking(collect_connection_details)
        .await
        .map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn get_network_overview(
    config_state: tauri::State<'_, Mutex<ConfigState>>,
) -> Result<NetworkOverview, String> {
    let target = configured_latency_target(&config_state);
    let latency_task = tokio::task::spawn_blocking({
        let target = target.clone();
        move || build_latency_result(&target)
    });
    let connection_task = tokio::task::spawn_blocking(collect_connection_details);

    let latency = latency_task.await.map_err(|e| e.to_string())?;
    let connection = connection_task.await.map_err(|e| e.to_string())?;
    let health = build_health_summary(&latency, &connection);

    Ok(NetworkOverview {
        health,
        latency,
        connection,
    })
}

#[tauri::command]
pub async fn run_speed_test(
    config_state: tauri::State<'_, Mutex<ConfigState>>,
) -> Result<SpeedTestResult, String> {
    let selected_server = match locate_mlab_server().await {
        Ok(server) => server,
        Err(error) => {
            return Ok(SpeedTestResult {
                download_speed: 0.0,
                upload_speed: 0.0,
                download_mbps: 0.0,
                upload_mbps: 0.0,
                ping: 0,
                timestamp: now_ms(),
                server: String::new(),
                server_label: "M-Lab unavailable".to_string(),
                source: MLAB_SOURCE_LABEL.to_string(),
                server_city: String::new(),
                server_country: String::new(),
                data_used_mb: 0.0,
                error: Some(error),
            });
        }
    };

    let download_result = measure_mlab_download(&selected_server.download_url).await;
    tokio::time::sleep(Duration::from_millis(250)).await;
    let upload_result = measure_mlab_upload(&selected_server.upload_url).await;

    let mut errors = Vec::new();
    let download = match download_result {
        Ok(stats) => stats,
        Err(error) => {
            errors.push(format!("download: {error}"));
            MlabTransferStats::default()
        }
    };
    let upload = match upload_result {
        Ok(stats) => stats,
        Err(error) => {
            errors.push(format!("upload: {error}"));
            MlabTransferStats::default()
        }
    };

    let ping = download
        .min_rtt_ms
        .or(upload.min_rtt_ms)
        .unwrap_or_default();
    let data_used_mb = (download.bytes + upload.bytes) as f64 / (1024.0 * 1024.0);

    let result = SpeedTestResult {
        // Keep the legacy fields in MB/s so older saved results and frontend
        // fallbacks remain readable while Mbps becomes the primary display.
        download_speed: download.mbps / 8.0,
        upload_speed: upload.mbps / 8.0,
        download_mbps: download.mbps,
        upload_mbps: upload.mbps,
        ping,
        timestamp: now_ms(),
        server: selected_server.host_label(),
        server_label: selected_server.label(),
        source: MLAB_SOURCE_LABEL.to_string(),
        server_city: selected_server.city.clone(),
        server_country: selected_server.country.clone(),
        data_used_mb,
        error: if errors.is_empty() {
            None
        } else {
            Some(errors.join("; "))
        },
    };

    persist_speed_test_result(&config_state, &result);
    Ok(result)
}

#[tauri::command]
pub fn get_speed_test_history(
    config_state: tauri::State<'_, Mutex<ConfigState>>,
) -> Vec<serde_json::Value> {
    let s = config_state.lock().unwrap();
    s.config
        .speed_test
        .results
        .iter()
        .cloned()
        .map(sanitize_speed_test_result)
        .collect()
}

fn configured_latency_target(config_state: &tauri::State<'_, Mutex<ConfigState>>) -> String {
    let s = config_state.lock().unwrap();
    let target = s.config.network_health.latency_target.trim();
    if target.is_empty() {
        DEFAULT_LATENCY_TARGET.to_string()
    } else {
        target.to_string()
    }
}

fn build_latency_result(target: &str) -> LatencyResult {
    let stats = ping_target(target, LATENCY_SAMPLE_COUNT, LATENCY_TIMEOUT_MS);
    let timestamp = now_ms();

    match stats {
        Ok(stats) if stats.received > 0 => {
            let label = classify_latency_quality(stats.average, stats.jitter, stats.packet_loss);
            LatencyResult {
                latency: stats.average,
                label: label.to_string(),
                jitter: stats.jitter,
                packet_loss: stats.packet_loss,
                samples_sent: stats.sent,
                samples_received: stats.received,
                min_latency: stats.min,
                max_latency: stats.max,
                average_latency: stats.average,
                target: target.to_string(),
                timestamp,
            }
        }
        _ => LatencyResult {
            latency: 999,
            label: "offline".to_string(),
            jitter: 0,
            packet_loss: 100.0,
            samples_sent: LATENCY_SAMPLE_COUNT,
            samples_received: 0,
            min_latency: 0,
            max_latency: 0,
            average_latency: 0,
            target: target.to_string(),
            timestamp,
        },
    }
}

fn collect_connection_details() -> SignalStrength {
    let adapter = fetch_primary_adapter().unwrap_or_else(|_| AdapterSnapshot {
        name: String::new(),
        description: String::new(),
        physical_medium: String::new(),
        link_speed: String::new(),
        local_ip: String::new(),
    });

    let connection_type = classify_connection_type(
        &adapter.name,
        &adapter.description,
        &adapter.physical_medium,
    );
    let wifi_details = fetch_wifi_details().unwrap_or_default();

    // Only let Wi-Fi details override when the primary adapter is not clearly
    // wired. Ethernet machines can keep Wi-Fi connected in the background.
    if connection_type != "ethernet" {
        if let Some(wifi) = wifi_details {
            return build_wifi_signal(adapter, wifi);
        }
    }

    if connection_type == "cellular" {
        if let Some((percentage, provider_name)) = fetch_cellular_details().unwrap_or_default() {
            let quality = classify_signal_quality(percentage);
            return SignalStrength {
                percentage,
                bars: signal_bars(percentage),
                quality: quality.to_string(),
                connection_type: "cellular".to_string(),
                adapter_name: if provider_name.is_empty() {
                    adapter.name
                } else {
                    provider_name
                },
                adapter_description: adapter.description,
                ssid: String::new(),
                link_speed: adapter.link_speed,
                local_ip: adapter.local_ip,
            };
        }
    }

    SignalStrength {
        percentage: 0,
        bars: 0,
        quality: if connection_type == "ethernet" {
            "wired".to_string()
        } else if connection_type == "cellular" {
            "mobile".to_string()
        } else {
            "unknown".to_string()
        },
        connection_type: connection_type.to_string(),
        adapter_name: adapter.name,
        adapter_description: adapter.description,
        ssid: String::new(),
        link_speed: adapter.link_speed,
        local_ip: adapter.local_ip,
    }
}

fn build_wifi_signal(
    adapter: AdapterSnapshot,
    wifi: (String, u32, String, String, String),
) -> SignalStrength {
    let (ssid, percentage, adapter_name, adapter_description, link_speed) = wifi;
    let quality = classify_signal_quality(percentage);

    SignalStrength {
        percentage,
        bars: signal_bars(percentage),
        quality: quality.to_string(),
        connection_type: "wifi".to_string(),
        adapter_name: if adapter_name.is_empty() {
            adapter.name
        } else {
            adapter_name
        },
        adapter_description: if adapter_description.is_empty() {
            adapter.description
        } else {
            adapter_description
        },
        ssid,
        link_speed: if link_speed.is_empty() {
            adapter.link_speed
        } else {
            link_speed
        },
        local_ip: adapter.local_ip,
    }
}

fn build_health_summary(
    latency: &LatencyResult,
    connection: &SignalStrength,
) -> NetworkHealthSummary {
    let (level, color) = if latency.samples_received == 0 {
        ("Offline", "#ef4444")
    } else if latency.packet_loss >= 20.0 || latency.latency >= 180 || latency.jitter >= 50 {
        ("Unstable", "#ef4444")
    } else if latency.packet_loss > 0.0 || latency.latency >= 90 || latency.jitter >= 20 {
        ("Fair", "#f59e0b")
    } else {
        ("Stable", "#10b981")
    };

    let mut subtitle = if latency.samples_received == 0 {
        format!("No replies from {}", latency.target)
    } else {
        format!(
            "{}ms avg · {}ms jitter · {:.0}% loss",
            latency.average_latency, latency.jitter, latency.packet_loss
        )
    };

    if (connection.connection_type == "wifi" || connection.connection_type == "cellular")
        && connection.percentage > 0
    {
        subtitle.push_str(&format!(" · {}% signal", connection.percentage));
    } else if !connection.connection_type.is_empty() {
        subtitle.push_str(&format!(" · {}", connection_label(connection)));
    }

    NetworkHealthSummary {
        level: level.to_string(),
        color: color.to_string(),
        subtitle,
        timestamp: now_ms(),
    }
}

fn classify_latency_quality(latency: u64, jitter: u64, packet_loss: f64) -> &'static str {
    if packet_loss >= 20.0 || latency >= 180 || jitter >= 50 {
        "poor"
    } else if packet_loss > 0.0 || latency >= 90 || jitter >= 20 {
        "fair"
    } else if latency < 40 && jitter < 10 {
        "excellent"
    } else {
        "good"
    }
}

fn classify_signal_quality(percentage: u32) -> &'static str {
    if percentage >= 80 {
        "excellent"
    } else if percentage >= 60 {
        "good"
    } else if percentage >= 40 {
        "fair"
    } else {
        "poor"
    }
}

fn signal_bars(percentage: u32) -> u32 {
    if percentage >= 80 {
        4
    } else if percentage >= 60 {
        3
    } else if percentage >= 40 {
        2
    } else if percentage >= 20 {
        1
    } else {
        0
    }
}

fn ping_target(target: &str, count: u32, timeout_ms: u64) -> Result<PingStats, String> {
    let count_str = count.to_string();
    let timeout_str = timeout_ms.to_string();
    let output = hidden_cmd("ping")
        .args(["-4", "-n", &count_str, "-w", &timeout_str, target])
        .output()
        .map_err(|e| e.to_string())?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let latencies = parse_ping_latencies(&stdout);
    let received = latencies.len() as u32;
    if received == 0 {
        return Err("No ping replies received".to_string());
    }

    let sent = count;
    let sum: u64 = latencies.iter().sum();
    let average = sum / received as u64;
    let min = *latencies.iter().min().unwrap_or(&0);
    let max = *latencies.iter().max().unwrap_or(&0);
    let jitter = latencies
        .windows(2)
        .map(|pair| pair[0].abs_diff(pair[1]))
        .sum::<u64>()
        / latencies.windows(2).count().max(1) as u64;
    let packet_loss = ((sent - received) as f64 / sent as f64) * 100.0;

    Ok(PingStats {
        average,
        min,
        max,
        jitter,
        sent,
        received,
        packet_loss,
    })
}

fn parse_ping_latencies(stdout: &str) -> Vec<u64> {
    stdout
        .lines()
        .filter_map(|line| {
            let lower = line.to_lowercase();
            if let Some(index) = lower.find("time=") {
                let fragment = &lower[index + 5..];
                let digits: String = fragment
                    .chars()
                    .take_while(|c| c.is_ascii_digit())
                    .collect();
                return digits.parse::<u64>().ok();
            }
            if let Some(index) = lower.find("time<") {
                let fragment = &lower[index + 5..];
                let digits: String = fragment
                    .chars()
                    .take_while(|c| c.is_ascii_digit())
                    .collect();
                return digits.parse::<u64>().ok().or(Some(1));
            }
            None
        })
        .collect()
}

fn fetch_wifi_details() -> Result<Option<(String, u32, String, String, String)>, String> {
    let output = hidden_cmd("netsh")
        .args(["wlan", "show", "interfaces"])
        .output()
        .map_err(|e| e.to_string())?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let is_connected = stdout.lines().any(|line| {
        let trimmed = line.trim().to_lowercase();
        trimmed.starts_with("state") && trimmed.contains(": connected")
    });
    if !is_connected {
        return Ok(None);
    }

    let ssid = parse_key_value(&stdout, "ssid").unwrap_or_default();
    let percentage = parse_key_value(&stdout, "signal")
        .and_then(|value| value.trim_end_matches('%').trim().parse::<u32>().ok())
        .unwrap_or(0);
    let adapter_name = parse_key_value(&stdout, "name").unwrap_or_default();
    let adapter_description = parse_key_value(&stdout, "description").unwrap_or_default();
    let rx_rate = parse_key_value(&stdout, "receive rate (mbps)").unwrap_or_default();
    let tx_rate = parse_key_value(&stdout, "transmit rate (mbps)").unwrap_or_default();
    let link_speed = match (rx_rate.is_empty(), tx_rate.is_empty()) {
        (false, false) if rx_rate == tx_rate => format!("{} Mbps", rx_rate),
        (false, false) => format!("{} / {} Mbps", rx_rate, tx_rate),
        (false, true) => format!("{} Mbps", rx_rate),
        (true, false) => format!("{} Mbps", tx_rate),
        _ => String::new(),
    };

    Ok(Some((
        ssid,
        percentage,
        adapter_name,
        adapter_description,
        link_speed,
    )))
}

fn fetch_cellular_details() -> Result<Option<(u32, String)>, String> {
    let output = hidden_cmd("netsh")
        .args(["mbn", "show", "interfaces"])
        .output()
        .map_err(|e| e.to_string())?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let is_connected = stdout.lines().any(|line| {
        let trimmed = line.trim().to_lowercase();
        trimmed.starts_with("state") && trimmed.contains("connected")
    });
    if !is_connected {
        return Ok(None);
    }

    let percentage = parse_key_value(&stdout, "signal")
        .and_then(|value| value.trim_end_matches('%').trim().parse::<u32>().ok())
        .unwrap_or(0);
    let provider_name = parse_key_value(&stdout, "provider name")
        .or_else(|| parse_key_value(&stdout, "name"))
        .unwrap_or_default();

    Ok(Some((percentage, provider_name)))
}

fn parse_key_value(stdout: &str, key: &str) -> Option<String> {
    stdout.lines().find_map(|line| {
        let trimmed = line.trim();
        let lower = trimmed.to_lowercase();
        if key == "ssid" {
            if lower.starts_with("bssid") || !lower.starts_with("ssid") {
                return None;
            }
        } else if !lower.starts_with(key) {
            return None;
        }

        trimmed
            .split_once(':')
            .map(|(_, value)| value.trim().to_string())
            .filter(|value| !value.is_empty())
    })
}

fn fetch_primary_adapter() -> Result<AdapterSnapshot, String> {
    let script = r#"
$route = Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
  Where-Object { $_.State -eq 'Alive' } |
  Sort-Object RouteMetric, InterfaceMetric |
  Select-Object -First 1

$adapter = $null
if ($route) {
  $adapter = Get-NetAdapter -InterfaceIndex $route.InterfaceIndex -ErrorAction SilentlyContinue |
    Where-Object Status -eq 'Up' |
    Select-Object -First 1 Name, InterfaceDescription, LinkSpeed, ifIndex, NdisPhysicalMedium, MediaType
}

if (-not $adapter) {
  $adapter = Get-NetIPConfiguration |
    Where-Object {
      $_.NetAdapter.Status -eq 'Up' -and
      $_.IPv4Address -and
      ($_.IPv4DefaultGateway -or $_.NetAdapter.InterfaceDescription -match 'Wi-?Fi|Wireless|WLAN|WWAN|Cellular|Ethernet')
    } |
    Sort-Object InterfaceMetric |
    Select-Object -First 1 -ExpandProperty NetAdapter |
    Select-Object -First 1 Name, InterfaceDescription, LinkSpeed, ifIndex, NdisPhysicalMedium, MediaType
}

if ($adapter) {
  $ip = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -and $_.IPAddress -notlike '169.254*' } |
    Select-Object -First 1 -ExpandProperty IPAddress

  [pscustomobject]@{
    name = $adapter.Name
    description = $adapter.InterfaceDescription
    physicalMedium = if ($adapter.NdisPhysicalMedium) { [string]$adapter.NdisPhysicalMedium } elseif ($adapter.MediaType) { [string]$adapter.MediaType } else { '' }
    linkSpeed = [string]$adapter.LinkSpeed
    localIp = if ($ip) { $ip } else { '' }
  } | ConvertTo-Json -Compress
}
"#;

    let output = hidden_cmd("powershell")
        .args(["-NoProfile", "-Command", script])
        .output()
        .map_err(|e| e.to_string())?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let json = stdout.trim();
    if json.is_empty() {
        return Err("No active adapter found".to_string());
    }

    serde_json::from_str::<AdapterSnapshot>(json).map_err(|e| e.to_string())
}

fn classify_connection_type(name: &str, description: &str, physical_medium: &str) -> &'static str {
    let combined = format!("{name} {description} {physical_medium}").to_ascii_lowercase();

    if has_wifi_indicator(&combined) {
        "wifi"
    } else if has_strong_cellular_indicator(&combined) {
        "cellular"
    } else if has_ethernet_indicator(&combined) {
        "ethernet"
    } else if has_cellular_indicator(&combined) {
        "cellular"
    } else {
        "unknown"
    }
}

fn has_wifi_indicator(value: &str) -> bool {
    value.contains("wi-fi")
        || value.contains("wifi")
        || value.contains("wireless lan")
        || value.contains("wlan")
        || value.contains("802.11")
}

fn has_ethernet_indicator(value: &str) -> bool {
    value.contains("ethernet")
        || value.contains("802.3")
        || value.contains("gbe")
        || value.contains("gigabit")
        || value.contains("realtek pcie")
        || value.contains("intel ethernet")
        || value.contains("killer e")
        || has_token(value, "lan")
}

fn has_cellular_indicator(value: &str) -> bool {
    has_strong_cellular_indicator(value) || has_token(value, "5g") || has_token(value, "4g")
}

fn has_strong_cellular_indicator(value: &str) -> bool {
    value.contains("wwan")
        || value.contains("wireless wan")
        || value.contains("cellular")
        || value.contains("mobile broadband")
        || value.contains("mbim")
        || value.contains("modem")
        || value.contains("sim")
        || has_token(value, "lte")
}

fn has_token(value: &str, token: &str) -> bool {
    value
        .split(|ch: char| !ch.is_ascii_alphanumeric())
        .any(|part| part == token)
}

fn connection_label(connection: &SignalStrength) -> String {
    match connection.connection_type.as_str() {
        "wifi" => {
            if !connection.ssid.is_empty() {
                format!("WiFi · {}", connection.ssid)
            } else {
                "WiFi".to_string()
            }
        }
        "ethernet" => "Wired".to_string(),
        "cellular" => "Cellular".to_string(),
        _ => "Unknown".to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classifies_gbe_adapters_as_ethernet_not_cellular() {
        assert_eq!(
            classify_connection_type("Ethernet", "Realtek PCIe 2.5GbE Family Controller", "802.3"),
            "ethernet"
        );
        assert_eq!(
            classify_connection_type("Ethernet", "Intel(R) Ethernet Controller I225-V", "802.3"),
            "ethernet"
        );
    }

    #[test]
    fn classifies_real_mobile_broadband_as_cellular() {
        assert_eq!(
            classify_connection_type(
                "Cellular",
                "Fibocom LTE Mobile Broadband Adapter",
                "Wireless WAN"
            ),
            "cellular"
        );
    }

    #[test]
    fn classifies_wifi_adapters_as_wifi() {
        assert_eq!(
            classify_connection_type("Wi-Fi", "Intel(R) Wi-Fi 6 AX201 160MHz", "Native 802.11"),
            "wifi"
        );
    }
}

async fn locate_mlab_server() -> Result<MlabServer, String> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .build()
        .map_err(|e| e.to_string())?;

    let response = client
        .get(MLAB_LOCATE_URL)
        .send()
        .await
        .map_err(|e| format!("M-Lab locate request failed: {e}"))?
        .error_for_status()
        .map_err(|e| format!("M-Lab locate returned an error: {e}"))?
        .json::<MlabLocateResponse>()
        .await
        .map_err(|e| format!("M-Lab locate response was unreadable: {e}"))?;

    for result in response.results {
        let download_url = result
            .urls
            .get("wss:///ndt/v7/download")
            .or_else(|| result.urls.get("ws:///ndt/v7/download"))
            .cloned();
        let upload_url = result
            .urls
            .get("wss:///ndt/v7/upload")
            .or_else(|| result.urls.get("ws:///ndt/v7/upload"))
            .cloned();

        if let (Some(download_url), Some(upload_url)) = (download_url, upload_url) {
            return Ok(MlabServer {
                machine: result.machine,
                hostname: result.hostname,
                city: result.location.city,
                country: result.location.country,
                download_url,
                upload_url,
            });
        }
    }

    Err("M-Lab did not return a usable NDT7 server".to_string())
}

async fn connect_ndt7(
    url: &str,
) -> Result<
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>,
    String,
> {
    let mut request = url
        .into_client_request()
        .map_err(|e| format!("Invalid M-Lab WebSocket URL: {e}"))?;
    request.headers_mut().insert(
        "Sec-WebSocket-Protocol",
        NDT7_WEBSOCKET_PROTOCOL
            .parse()
            .map_err(|e| format!("Invalid NDT7 protocol header: {e}"))?,
    );

    let (socket, _) = tokio::time::timeout(NDT7_CONNECT_TIMEOUT, connect_async(request))
        .await
        .map_err(|_| "M-Lab WebSocket connection timed out".to_string())?
        .map_err(|e| format!("M-Lab WebSocket connection failed: {e}"))?;

    Ok(socket)
}

async fn measure_mlab_download(url: &str) -> Result<MlabTransferStats, String> {
    let mut socket = connect_ndt7(url).await?;
    let started = Instant::now();
    let mut total_bytes = 0u64;
    let mut server_mbps = None;
    let mut min_rtt_ms = None;

    while started.elapsed() < NDT7_DOWNLOAD_WINDOW {
        match tokio::time::timeout(NDT7_MESSAGE_TIMEOUT, socket.next()).await {
            Ok(Some(Ok(Message::Binary(bytes)))) => {
                total_bytes += bytes.len() as u64;
            }
            Ok(Some(Ok(Message::Text(text)))) => {
                let measurement = parse_ndt7_measurement(&text);
                server_mbps = measurement.mbps.or(server_mbps);
                min_rtt_ms = measurement.min_rtt_ms.or(min_rtt_ms);
            }
            Ok(Some(Ok(Message::Close(_)))) | Ok(None) => break,
            Ok(Some(Ok(_))) => {}
            Ok(Some(Err(error))) => {
                if total_bytes == 0 {
                    return Err(error.to_string());
                }
                break;
            }
            Err(_) => break,
        }
    }

    let elapsed = started.elapsed().as_secs_f64();
    if elapsed <= 0.0 || total_bytes == 0 {
        return Err("M-Lab download did not transfer data".to_string());
    }

    let _ = socket.close(None).await;
    Ok(MlabTransferStats {
        mbps: server_mbps.unwrap_or_else(|| bytes_to_mbps(total_bytes, elapsed)),
        bytes: total_bytes,
        min_rtt_ms,
    })
}

async fn measure_mlab_upload(url: &str) -> Result<MlabTransferStats, String> {
    let socket = connect_ndt7(url).await?;
    let (mut write, mut read) = socket.split();
    let payload = vec![0u8; NDT7_UPLOAD_CHUNK_SIZE];
    let started = Instant::now();
    let mut total_bytes = 0u64;
    let mut server_mbps = None;
    let mut min_rtt_ms = None;

    while started.elapsed() < NDT7_UPLOAD_WINDOW {
        tokio::select! {
            send_result = write.send(Message::Binary(payload.clone().into())) => {
                send_result.map_err(|e| format!("M-Lab upload send failed: {e}"))?;
                total_bytes += payload.len() as u64;
            }
            message = read.next() => {
                match message {
                    Some(Ok(Message::Text(text))) => {
                        let measurement = parse_ndt7_measurement(&text);
                        server_mbps = measurement.mbps.or(server_mbps);
                        min_rtt_ms = measurement.min_rtt_ms.or(min_rtt_ms);
                    }
                    Some(Ok(Message::Close(_))) | None => break,
                    Some(Ok(_)) => {}
                    Some(Err(error)) => {
                        if total_bytes == 0 {
                            return Err(error.to_string());
                        }
                        break;
                    }
                }
            }
        }
    }

    let _ = write.close().await;
    let drain_started = Instant::now();
    while drain_started.elapsed() < Duration::from_millis(900) {
        match tokio::time::timeout(Duration::from_millis(200), read.next()).await {
            Ok(Some(Ok(Message::Text(text)))) => {
                let measurement = parse_ndt7_measurement(&text);
                server_mbps = measurement.mbps.or(server_mbps);
                min_rtt_ms = measurement.min_rtt_ms.or(min_rtt_ms);
            }
            Ok(Some(Ok(_))) => {}
            _ => break,
        }
    }

    let elapsed = started
        .elapsed()
        .as_secs_f64()
        .min(NDT7_UPLOAD_WINDOW.as_secs_f64());
    if elapsed <= 0.0 || total_bytes == 0 {
        return Err("M-Lab upload did not transfer data".to_string());
    }

    Ok(MlabTransferStats {
        mbps: server_mbps.unwrap_or_else(|| bytes_to_mbps(total_bytes, elapsed)),
        bytes: total_bytes,
        min_rtt_ms,
    })
}

#[derive(Default)]
struct ParsedNdt7Measurement {
    mbps: Option<f64>,
    min_rtt_ms: Option<u64>,
}

fn parse_ndt7_measurement(text: &str) -> ParsedNdt7Measurement {
    let Ok(value) = serde_json::from_str::<Value>(text) else {
        return ParsedNdt7Measurement::default();
    };

    ParsedNdt7Measurement {
        mbps: find_app_info_rate_mbps(&value),
        min_rtt_ms: find_number_by_key(&value, "MinRTT")
            .or_else(|| find_number_by_key(&value, "RTT"))
            .and_then(normalize_rtt_ms),
    }
}

fn find_app_info_rate_mbps(value: &Value) -> Option<f64> {
    if let Some(app_info) = find_object_by_key(value, "AppInfo") {
        let bytes = find_number_by_key(app_info, "NumBytes")?;
        let elapsed = find_number_by_key(app_info, "ElapsedTime")?;
        if bytes > 0.0 && elapsed > 0.0 {
            return Some(bytes_to_mbps(bytes as u64, elapsed / 1_000_000.0));
        }
    }
    None
}

fn find_object_by_key<'a>(value: &'a Value, key: &str) -> Option<&'a Value> {
    match value {
        Value::Object(map) => {
            if let Some(found) = map.get(key) {
                return Some(found);
            }
            map.values()
                .find_map(|child| find_object_by_key(child, key))
        }
        Value::Array(items) => items
            .iter()
            .find_map(|child| find_object_by_key(child, key)),
        _ => None,
    }
}

fn find_number_by_key(value: &Value, key: &str) -> Option<f64> {
    match value {
        Value::Object(map) => {
            if let Some(found) = map.get(key).and_then(Value::as_f64) {
                return Some(found);
            }
            map.values()
                .find_map(|child| find_number_by_key(child, key))
        }
        Value::Array(items) => items
            .iter()
            .find_map(|child| find_number_by_key(child, key)),
        _ => None,
    }
}

fn bytes_to_mbps(bytes: u64, elapsed_seconds: f64) -> f64 {
    if elapsed_seconds <= 0.0 {
        0.0
    } else {
        (bytes as f64 * 8.0) / elapsed_seconds / 1_000_000.0
    }
}

fn normalize_rtt_ms(raw: f64) -> Option<u64> {
    let rtt = if raw > 1000.0 {
        (raw / 1000.0).round()
    } else {
        raw.round()
    };

    if rtt > 0.0 {
        Some(rtt as u64)
    } else {
        None
    }
}

fn persist_speed_test_result(
    config_state: &tauri::State<'_, Mutex<ConfigState>>,
    result: &SpeedTestResult,
) {
    if let Ok(mut s) = config_state.lock() {
        if let Ok(serialized) = serde_json::to_value(result) {
            s.config.speed_test.last_run = result.timestamp;
            s.config.speed_test.last_server_index = 0;
            s.config.speed_test.results.push(serialized);
            if s.config.speed_test.results.len() > 20 {
                let excess = s.config.speed_test.results.len() - 20;
                s.config.speed_test.results.drain(0..excess);
            }
            save_config_to_path_pub(&s.config_path, &s.config);
        }
    }
}

fn sanitize_speed_test_result(mut value: serde_json::Value) -> serde_json::Value {
    if let Some(obj) = value.as_object_mut() {
        let is_previous_watchman_test =
            !obj.contains_key("source") || !obj.contains_key("downloadMbps");
        if is_previous_watchman_test {
            obj.insert(
                "server".to_string(),
                serde_json::Value::String(String::new()),
            );
            obj.insert(
                "serverLabel".to_string(),
                serde_json::Value::String("Previous Watchman test".to_string()),
            );
            obj.insert(
                "source".to_string(),
                serde_json::Value::String("Previous Watchman test".to_string()),
            );
        }

        obj.entry("source")
            .or_insert_with(|| serde_json::Value::String(MLAB_SOURCE_LABEL.to_string()));
    }
    value
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
