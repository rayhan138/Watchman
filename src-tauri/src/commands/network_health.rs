use serde::{Deserialize, Serialize};
use std::process::Command;
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tokio::task::JoinSet;

use super::config::{save_config_to_path_pub, ConfigState};

#[cfg(windows)]
use std::os::windows::process::CommandExt;

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;

const DEFAULT_LATENCY_TARGET: &str = "8.8.8.8";
const LATENCY_SAMPLE_COUNT: u32 = 5;
const LATENCY_TIMEOUT_MS: u64 = 1000;
const FRIENDLY_SPEED_TEST_SERVER_LABEL: &str = "USA · more coming soon";
const SPEED_TEST_SERVERS: [&str; 2] = [
    "https://traffic-monitor-speedtest-production.up.railway.app",
    "https://traffic-monitor-speedtest.onrender.com",
];

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
    pub ping: u64,
    pub timestamp: u64,
    pub server: String,
    #[serde(rename = "serverLabel")]
    pub server_label: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct AdapterSnapshot {
    name: String,
    description: String,
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
    let selected_server = match select_speed_test_server().await {
        Ok(server) => server,
        Err(error) => {
            return Ok(SpeedTestResult {
                download_speed: 0.0,
                upload_speed: 0.0,
                ping: 0,
                timestamp: now_ms(),
                server: String::new(),
                server_label: "Unavailable".to_string(),
                error: Some(error),
            });
        }
    };

    let (server_index, server, ping) = selected_server;

    // Give the chosen server a tiny pause before the sustained transfer tests.
    tokio::time::sleep(Duration::from_millis(300)).await;

    let download_speed = measure_download(&server).await.unwrap_or(0.0);
    let upload_speed = measure_upload(&server).await.unwrap_or(0.0);

    let result = SpeedTestResult {
        download_speed,
        upload_speed,
        ping,
        timestamp: now_ms(),
        server: server.clone(),
        server_label: speed_test_server_label(&server),
        error: None,
    };

    persist_speed_test_result(&config_state, server_index, &result);
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
        link_speed: String::new(),
        local_ip: String::new(),
    });

    let connection_type = classify_connection_type(&adapter.name, &adapter.description);
    let wifi_details = fetch_wifi_details().unwrap_or_default();

    // Prefer an actively connected Wi-Fi interface. Some PCs keep a mobile
    // broadband provider visible even while traffic is actually on Wi-Fi.
    if let Some(wifi) = wifi_details {
        return build_wifi_signal(adapter, wifi);
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
    Select-Object -First 1 Name, InterfaceDescription, LinkSpeed, ifIndex
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
    Select-Object -First 1 Name, InterfaceDescription, LinkSpeed, ifIndex
}

if ($adapter) {
  $ip = Get-NetIPAddress -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -and $_.IPAddress -notlike '169.254*' } |
    Select-Object -First 1 -ExpandProperty IPAddress

  [pscustomobject]@{
    name = $adapter.Name
    description = $adapter.InterfaceDescription
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

fn classify_connection_type(name: &str, description: &str) -> &'static str {
    let combined = format!("{} {}", name, description).to_lowercase();
    if combined.contains("wi-fi")
        || combined.contains("wifi")
        || combined.contains("wireless")
        || combined.contains("wlan")
        || combined.contains("802.11")
    {
        "wifi"
    } else if combined.contains("wwan")
        || combined.contains("cellular")
        || combined.contains("mobile broadband")
        || combined.contains("mbim")
        || combined.contains("lte")
        || combined.contains("5g")
        || combined.contains("4g")
    {
        "cellular"
    } else if combined.contains("ethernet")
        || combined.contains("gbe")
        || combined.contains("lan")
        || combined.contains("pcie")
    {
        "ethernet"
    } else {
        "unknown"
    }
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

async fn select_speed_test_server() -> Result<(usize, String, u64), String> {
    let mut best: Option<(usize, String, u64)> = None;

    for (index, server) in SPEED_TEST_SERVERS.iter().enumerate() {
        if let Ok(ping) = measure_http_ping(server, 3).await {
            match &best {
                Some((_, _, best_ping)) if *best_ping <= ping => {}
                _ => best = Some((index, (*server).to_string(), ping)),
            }
        }
    }

    best.ok_or_else(|| "No speed test server responded".to_string())
}

async fn measure_http_ping(server: &str, samples: usize) -> Result<u64, String> {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .build()
        .map_err(|e| e.to_string())?;

    let mut values = Vec::new();
    for _ in 0..samples {
        let start = Instant::now();
        let response = client
            .get(format!("{}/ping", server))
            .send()
            .await
            .map_err(|e| e.to_string())?;
        if response.status().is_success() {
            values.push(start.elapsed().as_millis() as u64);
        }
    }

    if values.is_empty() {
        return Err("Server ping failed".to_string());
    }

    values.sort_unstable();
    Ok(values[values.len() / 2])
}

async fn measure_download(server: &str) -> Result<f64, String> {
    const WORKERS: usize = 2;
    const CHUNK_SIZE: usize = 4 * 1024 * 1024;
    const TEST_WINDOW: Duration = Duration::from_secs(6);

    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(25))
        .build()
        .map_err(|e| e.to_string())?;
    let url = format!("{}/download?size={}", server, CHUNK_SIZE);
    let started = Instant::now();
    let mut tasks = JoinSet::new();

    for _ in 0..WORKERS {
        let client = client.clone();
        let url = url.clone();
        tasks.spawn(async move {
            let worker_start = Instant::now();
            let mut total_bytes = 0u64;

            while worker_start.elapsed() < TEST_WINDOW {
                let response = client.get(&url).send().await.map_err(|e| e.to_string())?;
                let bytes = response.bytes().await.map_err(|e| e.to_string())?;
                total_bytes += bytes.len() as u64;
            }

            Ok::<u64, String>(total_bytes)
        });
    }

    let mut total_bytes = 0u64;
    while let Some(result) = tasks.join_next().await {
        match result.map_err(|e| e.to_string())? {
            Ok(bytes) => total_bytes += bytes,
            Err(error) if total_bytes == 0 => return Err(error),
            Err(_) => {}
        }
    }

    let elapsed = started.elapsed().as_secs_f64();
    if elapsed <= 0.0 || total_bytes == 0 {
        return Err("Download measurement did not transfer data".to_string());
    }

    Ok(total_bytes as f64 / (1024.0 * 1024.0) / elapsed)
}

async fn measure_upload(server: &str) -> Result<f64, String> {
    const WORKERS: usize = 2;
    const CHUNK_SIZE: usize = 3 * 1024 * 1024;
    const TEST_WINDOW: Duration = Duration::from_secs(5);

    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(25))
        .build()
        .map_err(|e| e.to_string())?;
    let payload = vec![0u8; CHUNK_SIZE];
    let url = format!("{}/upload", server);
    let started = Instant::now();
    let mut tasks = JoinSet::new();

    for _ in 0..WORKERS {
        let client = client.clone();
        let url = url.clone();
        let payload = payload.clone();
        tasks.spawn(async move {
            let worker_start = Instant::now();
            let mut total_bytes = 0u64;

            while worker_start.elapsed() < TEST_WINDOW {
                client
                    .post(&url)
                    .header("Content-Type", "application/octet-stream")
                    .body(payload.clone())
                    .send()
                    .await
                    .map_err(|e| e.to_string())?;
                total_bytes += CHUNK_SIZE as u64;
            }

            Ok::<u64, String>(total_bytes)
        });
    }

    let mut total_bytes = 0u64;
    while let Some(result) = tasks.join_next().await {
        match result.map_err(|e| e.to_string())? {
            Ok(bytes) => total_bytes += bytes,
            Err(error) if total_bytes == 0 => return Err(error),
            Err(_) => {}
        }
    }

    let elapsed = started.elapsed().as_secs_f64();
    if elapsed <= 0.0 || total_bytes == 0 {
        return Err("Upload measurement did not transfer data".to_string());
    }

    Ok(total_bytes as f64 / (1024.0 * 1024.0) / elapsed)
}

fn persist_speed_test_result(
    config_state: &tauri::State<'_, Mutex<ConfigState>>,
    server_index: usize,
    result: &SpeedTestResult,
) {
    if let Ok(mut s) = config_state.lock() {
        if let Ok(serialized) = serde_json::to_value(result) {
            s.config.speed_test.last_run = result.timestamp;
            s.config.speed_test.last_server_index = server_index;
            s.config.speed_test.results.push(serialized);
            if s.config.speed_test.results.len() > 20 {
                let excess = s.config.speed_test.results.len() - 20;
                s.config.speed_test.results.drain(0..excess);
            }
            save_config_to_path_pub(&s.config_path, &s.config);
        }
    }
}

fn speed_test_server_label(server: &str) -> String {
    let _ = server;
    FRIENDLY_SPEED_TEST_SERVER_LABEL.to_string()
}

fn sanitize_speed_test_result(mut value: serde_json::Value) -> serde_json::Value {
    if let Some(obj) = value.as_object_mut() {
        obj.insert(
            "serverLabel".to_string(),
            serde_json::Value::String(FRIENDLY_SPEED_TEST_SERVER_LABEL.to_string()),
        );

        if let Some(raw_server) = obj.get("server").and_then(|value| value.as_str()) {
            let normalized = raw_server.to_lowercase();
            if normalized.contains("traffic-monitor-speedtest")
                || normalized.contains("railway.app")
                || normalized.contains("onrender.com")
            {
                obj.insert(
                    "server".to_string(),
                    serde_json::Value::String(FRIENDLY_SPEED_TEST_SERVER_LABEL.to_string()),
                );
            }
        }
    }
    value
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
