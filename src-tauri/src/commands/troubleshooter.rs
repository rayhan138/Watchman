use serde::{Deserialize, Serialize};
#[cfg(windows)]
use std::os::windows::process::CommandExt;
use std::process::Command;
use std::time::Duration;

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;

fn hidden_cmd(program: &str) -> Command {
    let mut cmd = Command::new(program);
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    cmd
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TestResult {
    pub name: String,
    pub status: String,
    pub message: String,
    pub details: String,
    pub timestamp: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Fix {
    pub problem: String,
    pub suggestion: String,
    pub automated: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiagnosticResults {
    pub tests: Vec<TestResult>,
    #[serde(rename = "overallStatus")]
    pub overall_status: String,
    #[serde(rename = "completedAt")]
    pub completed_at: u64,
    pub duration: u64,
    pub fixes: Vec<Fix>,
}

#[derive(Debug, Clone, Deserialize)]
struct DefaultRouteInfo {
    #[serde(rename = "InterfaceAlias", default)]
    interface_alias: String,
    #[serde(rename = "NextHop", default)]
    next_hop: String,
    #[serde(rename = "RouteMetric", default)]
    route_metric: u32,
    #[serde(rename = "InterfaceMetric", default)]
    interface_metric: u32,
}

#[tauri::command]
pub async fn run_diagnostics() -> DiagnosticResults {
    let start = std::time::Instant::now();
    let dns = test_dns().await;
    let internet = test_internet().await;
    let default_route = test_default_route(internet.status == "pass").await;
    let tests = vec![dns, default_route, internet];

    let overall = if tests.iter().all(|t| t.status == "pass") {
        "pass"
    } else if tests.iter().any(|t| t.status == "fail") {
        "fail"
    } else {
        "warning"
    };

    let fixes: Vec<Fix> = tests
        .iter()
        .filter(|t| t.status == "fail")
        .map(|t| get_fix(t))
        .collect();

    DiagnosticResults {
        tests,
        overall_status: overall.to_string(),
        completed_at: now_ms(),
        duration: start.elapsed().as_millis() as u64,
        fixes,
    }
}

async fn test_dns() -> TestResult {
    let domains = ["google.com", "cloudflare.com", "github.com"];
    for domain in &domains {
        match dns_lookup::lookup_host(domain) {
            Ok(addrs) if !addrs.is_empty() => {
                return TestResult {
                    name: "DNS Resolution".into(),
                    status: "pass".into(),
                    message: "DNS is working correctly".into(),
                    details: format!("Successfully resolved {}", domain),
                    timestamp: now_ms(),
                };
            }
            _ => continue,
        }
    }
    TestResult {
        name: "DNS Resolution".into(),
        status: "fail".into(),
        message: "DNS resolution is not working".into(),
        details: "Unable to resolve any test domains".into(),
        timestamp: now_ms(),
    }
}

async fn test_default_route(internet_ok: bool) -> TestResult {
    match get_default_route() {
        Ok(Some(route)) => {
            let interface_label = if route.interface_alias.is_empty() {
                "active network interface".to_string()
            } else {
                route.interface_alias.clone()
            };
            let next_hop = route.next_hop.trim();

            if next_hop.is_empty() || next_hop == "0.0.0.0" || next_hop.eq_ignore_ascii_case("on-link") {
                return TestResult {
                    name: "Default Route".into(),
                    status: if internet_ok { "pass".into() } else { "warning".into() },
                    message: if internet_ok {
                        "Default route is active".into()
                    } else {
                        "Default route found, but no explicit gateway was exposed".into()
                    },
                    details: format!(
                        "{} is handling the default route without a pingable gateway. This is common on cellular or managed links.",
                        interface_label
                    ),
                    timestamp: now_ms(),
                };
            }

            match hidden_cmd("ping")
                .args(["-n", "1", "-w", "1500", next_hop])
                .output()
            {
                Ok(output) if output.status.success() => TestResult {
                    name: "Default Route".into(),
                    status: "pass".into(),
                    message: "Default route is reachable".into(),
                    details: format!(
                        "{} via {} (route metric {}, interface metric {})",
                        interface_label, next_hop, route.route_metric, route.interface_metric
                    ),
                    timestamp: now_ms(),
                },
                _ if internet_ok => TestResult {
                    name: "Default Route".into(),
                    status: "warning".into(),
                    message: "Default route is active, but the gateway did not answer ping".into(),
                    details: format!(
                        "{} routes through {}, but that gateway may block ICMP. This is common on cellular or filtered networks.",
                        interface_label, next_hop
                    ),
                    timestamp: now_ms(),
                },
                _ => TestResult {
                    name: "Default Route".into(),
                    status: "fail".into(),
                    message: "Default gateway is not reachable".into(),
                    details: format!(
                        "{} routes through {}, but the gateway did not answer.",
                        interface_label, next_hop
                    ),
                    timestamp: now_ms(),
                },
            }
        }
        Ok(None) => TestResult {
            name: "Default Route".into(),
            status: if internet_ok { "warning".into() } else { "fail".into() },
            message: if internet_ok {
                "Internet works, but no default route details were exposed".into()
            } else {
                "No default route found".into()
            },
            details: if internet_ok {
                "Some cellular, VPN, and managed adapters hide gateway details even when the connection works.".into()
            } else {
                "Windows did not report any active IPv4 default route.".into()
            },
            timestamp: now_ms(),
        },
        Err(err) => TestResult {
            name: "Default Route".into(),
            status: if internet_ok { "warning".into() } else { "fail".into() },
            message: if internet_ok {
                "Could not inspect the default route, but internet access is working".into()
            } else {
                "Could not inspect the default route".into()
            },
            details: err,
            timestamp: now_ms(),
        },
    }
}

async fn test_internet() -> TestResult {
    let client = match reqwest::Client::builder()
        .timeout(Duration::from_secs(5))
        .build()
    {
        Ok(c) => c,
        Err(_) => {
            return TestResult {
                name: "Internet Connectivity".into(),
                status: "fail".into(),
                message: "HTTP client error".into(),
                details: "Could not create HTTP client".into(),
                timestamp: now_ms(),
            }
        }
    };

    let urls = ["http://www.google.com", "https://www.cloudflare.com"];
    for url in &urls {
        match client.get(*url).send().await {
            Ok(resp) if resp.status().is_success() || resp.status().is_redirection() => {
                return TestResult {
                    name: "Internet Connectivity".into(),
                    status: "pass".into(),
                    message: "Internet connection is working".into(),
                    details: format!("HTTP request to {} successful", url),
                    timestamp: now_ms(),
                };
            }
            _ => continue,
        }
    }
    TestResult {
        name: "Internet Connectivity".into(),
        status: "fail".into(),
        message: "No internet connection".into(),
        details: "Unable to reach any test websites".into(),
        timestamp: now_ms(),
    }
}

fn get_fix(test: &TestResult) -> Fix {
    let name = test.name.to_lowercase();
    if name.contains("dns") {
        Fix {
            problem: "DNS resolution is not working".into(),
            suggestion:
                "Try changing DNS servers to 8.8.8.8 / 8.8.4.4 (Google) or 1.1.1.1 (Cloudflare)"
                    .into(),
            automated: false,
        }
    } else if name.contains("default route") || name.contains("gateway") {
        Fix {
            problem: "Default route is missing or unreachable".into(),
            suggestion: "Reconnect the current adapter, then check router, modem, or hotspot connection. VPNs and mobile adapters can also change the default route.".into(),
            automated: false,
        }
    } else if name.contains("internet") {
        Fix {
            problem: "No internet connection".into(),
            suggestion: "Check that modem is connected. Contact your ISP if problem persists."
                .into(),
            automated: false,
        }
    } else {
        Fix {
            problem: test.message.clone(),
            suggestion: "Check network settings and restart device.".into(),
            automated: false,
        }
    }
}

fn get_default_route() -> Result<Option<DefaultRouteInfo>, String> {
    let script = r#"
$route = Get-NetRoute -AddressFamily IPv4 -ErrorAction SilentlyContinue |
  Where-Object { $_.DestinationPrefix -eq '0.0.0.0/0' -and $_.State -eq 'Alive' } |
  Sort-Object RouteMetric, InterfaceMetric |
  Select-Object -First 1 InterfaceAlias, NextHop, RouteMetric, InterfaceMetric

if ($route) {
  $route | ConvertTo-Json -Compress
}
"#;

    let output = hidden_cmd("powershell")
        .args(["-NoProfile", "-Command", script])
        .output()
        .map_err(|e| e.to_string())?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let json = stdout.trim();
    if json.is_empty() {
        return Ok(None);
    }

    serde_json::from_str::<DefaultRouteInfo>(json)
        .map(Some)
        .map_err(|e| format!("Unable to parse default route details: {}", e))
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
