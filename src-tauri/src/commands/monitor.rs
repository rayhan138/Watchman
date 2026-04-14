use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Instant;
use sysinfo::{CpuRefreshKind, Disks, Networks, System};

#[cfg(target_os = "windows")]
mod windows_cpu {
    use std::ffi::OsStr;
    use std::mem::MaybeUninit;
    use std::os::windows::ffi::OsStrExt;

    type PdhHCounter = isize;
    type PdhHQuery = isize;
    type PdhStatus = i32;

    const ERROR_SUCCESS: PdhStatus = 0;
    const PDH_FMT_DOUBLE: u32 = 0x0000_0200;
    const CPU_COUNTER_PATHS: [&str; 2] = [
        r"\Processor Information(_Total)\% Processor Utility",
        r"\Processor Information(_Total)\% Processor Time",
    ];

    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct FileTime {
        dw_low_date_time: u32,
        dw_high_date_time: u32,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetSystemTimes(
            idle_time: *mut FileTime,
            kernel_time: *mut FileTime,
            user_time: *mut FileTime,
        ) -> i32;
    }

    #[repr(C)]
    union PdhFmtCounterValueUnion {
        long_value: i32,
        double_value: f64,
        large_value: i64,
        ansi_string_value: *const u8,
        wide_string_value: *const u16,
    }

    #[repr(C)]
    struct PdhFmtCounterValue {
        c_status: u32,
        value: PdhFmtCounterValueUnion,
    }

    #[link(name = "pdh")]
    unsafe extern "system" {
        fn PdhOpenQueryW(
            data_source: *const u16,
            user_data: usize,
            query: *mut PdhHQuery,
        ) -> PdhStatus;
        fn PdhAddEnglishCounterW(
            query: PdhHQuery,
            full_counter_path: *const u16,
            user_data: usize,
            counter: *mut PdhHCounter,
        ) -> PdhStatus;
        fn PdhCollectQueryData(query: PdhHQuery) -> PdhStatus;
        fn PdhGetFormattedCounterValue(
            counter: PdhHCounter,
            format: u32,
            counter_type: *mut u32,
            value: *mut PdhFmtCounterValue,
        ) -> PdhStatus;
        fn PdhCloseQuery(query: PdhHQuery) -> PdhStatus;
    }

    #[derive(Clone, Copy, Debug)]
    pub struct CpuTimesSnapshot {
        pub idle: u64,
        pub kernel: u64,
        pub user: u64,
    }

    pub struct PdhCpuQuery {
        query: PdhHQuery,
        counter: PdhHCounter,
        primed: bool,
    }

    impl PdhCpuQuery {
        pub fn new() -> Result<Self, ()> {
            let mut query = 0isize;
            let open_status = unsafe { PdhOpenQueryW(std::ptr::null(), 0, &mut query) };
            if open_status != ERROR_SUCCESS || query == 0 {
                return Err(());
            }

            for path in CPU_COUNTER_PATHS {
                let mut counter = 0isize;
                let wide_path = wide_string(path);
                let add_status =
                    unsafe { PdhAddEnglishCounterW(query, wide_path.as_ptr(), 0, &mut counter) };
                if add_status == ERROR_SUCCESS && counter != 0 {
                    return Ok(Self {
                        query,
                        counter,
                        primed: false,
                    });
                }
            }

            unsafe {
                let _ = PdhCloseQuery(query);
            }
            Err(())
        }

        pub fn next_value(&mut self) -> Result<Option<f64>, ()> {
            let collect_status = unsafe { PdhCollectQueryData(self.query) };
            if collect_status != ERROR_SUCCESS {
                return Err(());
            }

            if !self.primed {
                self.primed = true;
                return Ok(None);
            }

            let mut counter_type = 0u32;
            let mut value = MaybeUninit::<PdhFmtCounterValue>::zeroed();
            let status = unsafe {
                PdhGetFormattedCounterValue(
                    self.counter,
                    PDH_FMT_DOUBLE,
                    &mut counter_type,
                    value.as_mut_ptr(),
                )
            };
            if status != ERROR_SUCCESS {
                return Err(());
            }

            let value = unsafe { value.assume_init() };
            if value.c_status != ERROR_SUCCESS as u32 {
                return Err(());
            }

            let usage = unsafe { value.value.double_value }.clamp(0.0, 100.0);
            Ok(Some((usage * 10.0).round() / 10.0))
        }
    }

    impl Drop for PdhCpuQuery {
        fn drop(&mut self) {
            if self.query != 0 {
                unsafe {
                    let _ = PdhCloseQuery(self.query);
                }
            }
        }
    }

    pub fn get_system_cpu_times() -> Option<CpuTimesSnapshot> {
        let mut idle = FileTime::default();
        let mut kernel = FileTime::default();
        let mut user = FileTime::default();

        let ok = unsafe { GetSystemTimes(&mut idle, &mut kernel, &mut user) };
        if ok == 0 {
            return None;
        }

        Some(CpuTimesSnapshot {
            idle: filetime_to_u64(idle),
            kernel: filetime_to_u64(kernel),
            user: filetime_to_u64(user),
        })
    }

    fn filetime_to_u64(value: FileTime) -> u64 {
        ((value.dw_high_date_time as u64) << 32) | value.dw_low_date_time as u64
    }

    fn wide_string(value: &str) -> Vec<u16> {
        OsStr::new(value)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }
}

#[cfg(target_os = "windows")]
mod windows_network {
    use std::slice;

    const MAX_INTERFACE_NAME_LEN: usize = 256;
    const MAXLEN_PHYSADDR: usize = 8;
    const MAXLEN_IFDESCR: usize = 256;

    const ERROR_INSUFFICIENT_BUFFER: u32 = 122;
    const NO_ERROR: u32 = 0;
    const OCTET_COUNTER_RANGE: u64 = u32::MAX as u64 + 1;

    pub const IF_TYPE_SOFTWARE_LOOPBACK: u32 = 24;
    pub const IF_TYPE_TUNNEL: u32 = 131;
    pub const IF_OPER_STATUS_CONNECTED: u32 = 4;
    pub const IF_OPER_STATUS_OPERATIONAL: u32 = 5;

    #[repr(C)]
    pub struct MibIfRow {
        pub wsz_name: [u16; MAX_INTERFACE_NAME_LEN],
        pub dw_index: u32,
        pub dw_type: u32,
        pub dw_mtu: u32,
        pub dw_speed: u32,
        pub dw_phys_addr_len: u32,
        pub b_phys_addr: [u8; MAXLEN_PHYSADDR],
        pub dw_admin_status: u32,
        pub dw_oper_status: u32,
        pub dw_last_change: u32,
        pub dw_in_octets: u32,
        pub dw_in_ucast_pkts: u32,
        pub dw_in_n_ucast_pkts: u32,
        pub dw_in_discards: u32,
        pub dw_in_errors: u32,
        pub dw_in_unknown_protos: u32,
        pub dw_out_octets: u32,
        pub dw_out_ucast_pkts: u32,
        pub dw_out_n_ucast_pkts: u32,
        pub dw_out_discards: u32,
        pub dw_out_errors: u32,
        pub dw_out_qlen: u32,
        pub dw_descr_len: u32,
        pub b_descr: [u8; MAXLEN_IFDESCR],
    }

    #[repr(C)]
    struct MibIfTable {
        dw_num_entries: u32,
        table: [MibIfRow; 1],
    }

    #[link(name = "iphlpapi")]
    unsafe extern "system" {
        fn GetIfTable(table: *mut MibIfTable, size: *mut u32, order: i32) -> u32;
    }

    #[derive(Clone, Debug)]
    pub struct InterfaceSnapshot {
        pub index: u32,
        pub name: String,
        pub description: String,
        pub rx_bytes: u64,
        pub tx_bytes: u64,
    }

    pub fn get_interfaces() -> Result<Vec<InterfaceSnapshot>, String> {
        let mut buffer_len = 0u32;
        let status = unsafe { GetIfTable(std::ptr::null_mut(), &mut buffer_len, 0) };
        if status != ERROR_INSUFFICIENT_BUFFER || buffer_len == 0 {
            return Err(format!(
                "GetIfTable size query failed with status {}",
                status
            ));
        }

        let mut buffer = vec![0u8; buffer_len as usize];
        let table_ptr = buffer.as_mut_ptr() as *mut MibIfTable;
        let status = unsafe { GetIfTable(table_ptr, &mut buffer_len, 0) };
        if status != NO_ERROR {
            return Err(format!("GetIfTable failed with status {}", status));
        }

        let table = unsafe { &*table_ptr };
        let rows =
            unsafe { slice::from_raw_parts(table.table.as_ptr(), table.dw_num_entries as usize) };

        Ok(rows
            .iter()
            .filter(|row| is_active_interface(row))
            .map(|row| InterfaceSnapshot {
                index: row.dw_index,
                name: get_interface_name(row),
                description: get_interface_description(row),
                rx_bytes: row.dw_in_octets as u64,
                tx_bytes: row.dw_out_octets as u64,
            })
            .collect())
    }

    fn is_active_interface(row: &MibIfRow) -> bool {
        if matches!(row.dw_type, IF_TYPE_SOFTWARE_LOOPBACK | IF_TYPE_TUNNEL) {
            return false;
        }

        if is_filter_driver_interface(&get_interface_description(row)) {
            return false;
        }

        matches!(
            row.dw_oper_status,
            IF_OPER_STATUS_CONNECTED | IF_OPER_STATUS_OPERATIONAL
        )
    }

    fn get_interface_description(row: &MibIfRow) -> String {
        let desc_len = (row.dw_descr_len as usize).min(row.b_descr.len());
        if desc_len == 0 {
            return String::new();
        }

        let desc = &row.b_descr[..desc_len];
        let end = desc.iter().position(|b| *b == 0).unwrap_or(desc_len);
        String::from_utf8_lossy(&desc[..end]).trim().to_string()
    }

    fn is_filter_driver_interface(description: &str) -> bool {
        let desc = description.to_ascii_lowercase();

        desc.contains("wfp native mac layer lightweight filter")
            || desc.contains("wfp 802.3 mac layer lightweight filter")
            || desc.contains("qos packet scheduler")
    }

    fn get_interface_name(row: &MibIfRow) -> String {
        let description = get_interface_description(row);
        if !description.is_empty() {
            return description;
        }

        let end = row
            .wsz_name
            .iter()
            .position(|c| *c == 0)
            .unwrap_or(row.wsz_name.len());
        let name = String::from_utf16_lossy(&row.wsz_name[..end])
            .trim()
            .to_string();
        if !name.is_empty() {
            return name;
        }

        format!("Interface {}", row.dw_index)
    }

    pub fn diff_counter(current: u64, previous: u64) -> u64 {
        if current >= previous {
            current - previous
        } else {
            (OCTET_COUNTER_RANGE - previous) + current
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CpuUsage {
    pub overall: f64,
    pub cores: Vec<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MemoryUsage {
    pub total: u64,
    pub used: u64,
    pub active: u64,
    pub available: u64,
    #[serde(rename = "percentUsed")]
    pub percent_used: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkStats {
    #[serde(rename = "downloadSpeed")]
    pub download_speed: f64,
    #[serde(rename = "uploadSpeed")]
    pub upload_speed: f64,
    #[serde(rename = "downloadedBytes")]
    pub downloaded_bytes: u64,
    #[serde(rename = "uploadedBytes")]
    pub uploaded_bytes: u64,
    #[serde(rename = "totalDownloaded")]
    pub total_downloaded: u64,
    #[serde(rename = "totalUploaded")]
    pub total_uploaded: u64,
    pub interfaces: Vec<InterfaceStat>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InterfaceStat {
    pub iface: String,
    pub rx_bytes: u64,
    pub tx_bytes: u64,
}

#[derive(Debug, Clone)]
pub struct CounterSnapshot {
    pub rx_bytes: u64,
    pub tx_bytes: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiskInfo {
    pub fs: String,
    pub mount: String,
    #[serde(rename = "type")]
    pub disk_type: String,
    pub size: u64,
    pub used: u64,
    pub available: u64,
    #[serde(rename = "percentUsed")]
    pub percent_used: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetworkInterface {
    pub iface: String,
    #[serde(rename = "ifaceName")]
    pub iface_name: String,
    pub ip4: String,
    pub mac: String,
    #[serde(rename = "type")]
    pub iface_type: String,
    pub speed: u64,
    pub operstate: String,
    pub internal: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemInfo {
    pub platform: String,
    pub distro: String,
    pub release: String,
    pub arch: String,
    pub hostname: String,
    #[serde(rename = "cpuBrand")]
    pub cpu_brand: String,
    #[serde(rename = "cpuManufacturer")]
    pub cpu_manufacturer: String,
    #[serde(rename = "cpuSpeed")]
    pub cpu_speed: f64,
    #[serde(rename = "cpuCores")]
    pub cpu_cores: usize,
    pub uptime: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MetricsPayload {
    pub network: NetworkStats,
    pub cpu: CpuUsage,
    pub memory: MemoryUsage,
}

pub struct MonitorState {
    pub system: System,
    pub networks: Networks,
    pub prev_rx: u64,
    pub prev_tx: u64,
    pub initial_rx: Option<u64>,
    pub initial_tx: Option<u64>,
    pub prev_time: Instant,
    pub is_connected: bool,
    pub prev_interfaces: HashMap<u32, CounterSnapshot>,
    pub runtime_downloaded: u64,
    pub runtime_uploaded: u64,
    #[cfg(target_os = "windows")]
    pub pdh_cpu_query: Option<windows_cpu::PdhCpuQuery>,
    #[cfg(target_os = "windows")]
    pub prev_cpu_times: Option<windows_cpu::CpuTimesSnapshot>,
}

impl MonitorState {
    pub fn new() -> Self {
        let system = System::new_all();
        let networks = Networks::new_with_refreshed_list();
        Self {
            system,
            networks,
            prev_rx: 0,
            prev_tx: 0,
            initial_rx: None,
            initial_tx: None,
            prev_time: Instant::now(),
            is_connected: true,
            prev_interfaces: HashMap::new(),
            runtime_downloaded: 0,
            runtime_uploaded: 0,
            #[cfg(target_os = "windows")]
            pdh_cpu_query: None,
            #[cfg(target_os = "windows")]
            prev_cpu_times: None,
        }
    }
}

pub fn reset_session_counters(state: &mut MonitorState) {
    state.runtime_downloaded = 0;
    state.runtime_uploaded = 0;
    state.initial_rx = Some(state.prev_rx);
    state.initial_tx = Some(state.prev_tx);
}

pub fn get_cpu_usage(state: &mut MonitorState) -> CpuUsage {
    #[cfg(target_os = "windows")]
    if let Some(cpu) = get_windows_cpu_usage(state) {
        return cpu;
    }

    state
        .system
        .refresh_cpu_specifics(CpuRefreshKind::nothing().with_cpu_usage());
    std::thread::sleep(std::time::Duration::from_millis(100));
    state
        .system
        .refresh_cpu_specifics(CpuRefreshKind::nothing().with_cpu_usage());

    let cpus = state.system.cpus();
    let overall = if cpus.is_empty() {
        0.0
    } else {
        let sum: f64 = cpus.iter().map(|c| c.cpu_usage() as f64).sum();
        (sum / cpus.len() as f64 * 10.0).round() / 10.0
    };
    let cores: Vec<f64> = cpus
        .iter()
        .map(|c| (c.cpu_usage() as f64 * 10.0).round() / 10.0)
        .collect();
    CpuUsage { overall, cores }
}

#[cfg(target_os = "windows")]
fn get_windows_cpu_usage(state: &mut MonitorState) -> Option<CpuUsage> {
    use windows_cpu::{get_system_cpu_times, PdhCpuQuery};

    if state.pdh_cpu_query.is_none() {
        state.pdh_cpu_query = PdhCpuQuery::new().ok();
    }

    if let Some(query) = state.pdh_cpu_query.as_mut() {
        match query.next_value() {
            Ok(Some(overall)) => return Some(build_windows_cpu_usage(state, overall)),
            Ok(None) => {}
            Err(_) => state.pdh_cpu_query = None,
        }
    }

    let current = get_system_cpu_times()?;

    let overall = if let Some(previous) = state.prev_cpu_times {
        let idle_delta = current.idle.saturating_sub(previous.idle);
        let kernel_delta = current.kernel.saturating_sub(previous.kernel);
        let user_delta = current.user.saturating_sub(previous.user);
        let total_delta = kernel_delta.saturating_add(user_delta);

        if total_delta > 0 {
            let busy_delta = total_delta.saturating_sub(idle_delta);
            ((busy_delta as f64 / total_delta as f64) * 1000.0).round() / 10.0
        } else {
            0.0
        }
    } else {
        0.0
    };

    state.prev_cpu_times = Some(current);

    Some(build_windows_cpu_usage(state, overall))
}

#[cfg(target_os = "windows")]
fn build_windows_cpu_usage(state: &mut MonitorState, overall: f64) -> CpuUsage {
    state
        .system
        .refresh_cpu_specifics(CpuRefreshKind::nothing().with_cpu_usage());

    let cpu_count = state.system.cpus().len().max(
        std::thread::available_parallelism()
            .map(|v| v.get())
            .unwrap_or(1),
    );

    let mut cores: Vec<f64> = state
        .system
        .cpus()
        .iter()
        .map(|cpu| (cpu.cpu_usage() as f64 * 10.0).round() / 10.0)
        .collect();

    if cores.is_empty() {
        cores = vec![overall; cpu_count];
    }

    CpuUsage { overall, cores }
}

pub fn get_memory_usage(state: &mut MonitorState) -> MemoryUsage {
    state.system.refresh_memory();
    let total = state.system.total_memory();
    let used = state.system.used_memory();
    let available = state.system.available_memory();
    let active = used;
    let percent_used = if total > 0 {
        ((active as f64 / total as f64) * 1000.0).round() / 10.0
    } else {
        0.0
    };
    MemoryUsage {
        total,
        used,
        active,
        available,
        percent_used,
    }
}

pub fn get_network_stats(state: &mut MonitorState) -> NetworkStats {
    #[cfg(target_os = "windows")]
    if let Some(stats) = get_windows_network_stats(state) {
        return stats;
    }

    state.networks.refresh(true);

    let mut current_rx: u64 = 0;
    let mut current_tx: u64 = 0;
    let mut interfaces = Vec::new();

    for (name, data) in state.networks.iter() {
        // Skip loopback
        let name_lower = name.to_lowercase();
        if name_lower.contains("loopback") || name_lower == "lo" {
            continue;
        }
        let rx = data.total_received();
        let tx = data.total_transmitted();
        current_rx += rx;
        current_tx += tx;
        interfaces.push(InterfaceStat {
            iface: name.clone(),
            rx_bytes: rx,
            tx_bytes: tx,
        });
    }

    let now = Instant::now();
    let elapsed = now.duration_since(state.prev_time).as_secs_f64();

    let mut download_speed = 0.0;
    let mut upload_speed = 0.0;

    if state.prev_rx > 0
        && elapsed > 0.0
        && current_rx >= state.prev_rx
        && current_tx >= state.prev_tx
    {
        download_speed = (current_rx - state.prev_rx) as f64 / elapsed;
        upload_speed = (current_tx - state.prev_tx) as f64 / elapsed;
    }

    let downloaded_bytes = if state.prev_rx > 0 && current_rx >= state.prev_rx {
        current_rx - state.prev_rx
    } else {
        0
    };
    let uploaded_bytes = if state.prev_tx > 0 && current_tx >= state.prev_tx {
        current_tx - state.prev_tx
    } else {
        0
    };

    if state.initial_rx.is_none() {
        state.initial_rx = Some(current_rx);
        state.initial_tx = Some(current_tx);
    }

    let total_downloaded = current_rx.saturating_sub(state.initial_rx.unwrap_or(current_rx));
    let total_uploaded = current_tx.saturating_sub(state.initial_tx.unwrap_or(current_tx));

    state.prev_rx = current_rx;
    state.prev_tx = current_tx;
    state.prev_time = now;

    NetworkStats {
        download_speed: download_speed.max(0.0),
        upload_speed: upload_speed.max(0.0),
        downloaded_bytes,
        uploaded_bytes,
        total_downloaded,
        total_uploaded,
        interfaces,
    }
}

#[cfg(target_os = "windows")]
fn get_windows_network_stats(state: &mut MonitorState) -> Option<NetworkStats> {
    use windows_network::{diff_counter, get_interfaces};

    let samples = match get_interfaces() {
        Ok(samples) => samples,
        Err(_) => return None,
    };

    let now = Instant::now();
    let elapsed_ms = now.duration_since(state.prev_time).as_millis() as f64;

    let mut next_prev = HashMap::new();
    let mut downloaded_bytes = 0u64;
    let mut uploaded_bytes = 0u64;
    let mut interfaces = Vec::with_capacity(samples.len());

    for sample in samples {
        if let Some(previous) = state.prev_interfaces.get(&sample.index) {
            downloaded_bytes += diff_counter(sample.rx_bytes, previous.rx_bytes);
            uploaded_bytes += diff_counter(sample.tx_bytes, previous.tx_bytes);
        }

        interfaces.push(InterfaceStat {
            iface: if sample.description.is_empty() {
                sample.name.clone()
            } else {
                sample.description.clone()
            },
            rx_bytes: sample.rx_bytes,
            tx_bytes: sample.tx_bytes,
        });

        next_prev.insert(
            sample.index,
            CounterSnapshot {
                rx_bytes: sample.rx_bytes,
                tx_bytes: sample.tx_bytes,
            },
        );
    }

    state.prev_interfaces = next_prev;
    state.runtime_downloaded = state.runtime_downloaded.saturating_add(downloaded_bytes);
    state.runtime_uploaded = state.runtime_uploaded.saturating_add(uploaded_bytes);
    state.prev_time = now;

    let (download_speed, upload_speed) = if elapsed_ms > 0.0 {
        (
            (downloaded_bytes as f64 * 1000.0) / elapsed_ms,
            (uploaded_bytes as f64 * 1000.0) / elapsed_ms,
        )
    } else {
        (0.0, 0.0)
    };

    Some(NetworkStats {
        download_speed,
        upload_speed,
        downloaded_bytes,
        uploaded_bytes,
        total_downloaded: state.runtime_downloaded,
        total_uploaded: state.runtime_uploaded,
        interfaces,
    })
}

pub fn get_disk_usage() -> Vec<DiskInfo> {
    let disks = Disks::new_with_refreshed_list();
    disks
        .iter()
        .map(|d| {
            let total = d.total_space();
            let available = d.available_space();
            let used = total.saturating_sub(available);
            let percent = if total > 0 {
                ((used as f64 / total as f64) * 1000.0).round() / 10.0
            } else {
                0.0
            };
            DiskInfo {
                fs: d.name().to_string_lossy().to_string(),
                mount: d.mount_point().to_string_lossy().to_string(),
                disk_type: format!("{:?}", d.kind()),
                size: total,
                used,
                available,
                percent_used: percent,
            }
        })
        .collect()
}

pub fn get_network_interfaces() -> Vec<NetworkInterface> {
    let networks = Networks::new_with_refreshed_list();
    networks
        .iter()
        .map(|(name, _data)| NetworkInterface {
            iface: name.clone(),
            iface_name: name.clone(),
            ip4: String::new(),
            mac: String::new(),
            iface_type: "unknown".to_string(),
            speed: 0,
            operstate: "up".to_string(),
            internal: name.to_lowercase().contains("loopback") || name.to_lowercase() == "lo",
        })
        .collect()
}

pub fn get_system_info(state: &mut MonitorState) -> SystemInfo {
    state.system.refresh_all();
    let cpus = state.system.cpus();
    let cpu_brand = cpus
        .first()
        .map(|c| c.brand().to_string())
        .unwrap_or_default();
    let cpu_vendor = cpus
        .first()
        .map(|c| c.vendor_id().to_string())
        .unwrap_or_default();
    let cpu_speed = cpus
        .first()
        .map(|c| c.frequency() as f64 / 1000.0)
        .unwrap_or(0.0);
    let cpu_cores = cpus.len();

    SystemInfo {
        platform: "win32".to_string(),
        distro: System::long_os_version().unwrap_or_default(),
        release: System::os_version().unwrap_or_default(),
        arch: std::env::consts::ARCH.to_string(),
        hostname: System::host_name().unwrap_or_default(),
        cpu_brand,
        cpu_manufacturer: cpu_vendor,
        cpu_speed,
        cpu_cores,
        uptime: System::uptime(),
    }
}

// Tauri commands

#[tauri::command]
pub fn cmd_get_cpu_usage(state: tauri::State<'_, Mutex<MonitorState>>) -> CpuUsage {
    let mut s = state.lock().unwrap();
    get_cpu_usage(&mut s)
}

#[tauri::command]
pub fn cmd_get_memory_usage(state: tauri::State<'_, Mutex<MonitorState>>) -> MemoryUsage {
    let mut s = state.lock().unwrap();
    get_memory_usage(&mut s)
}

#[tauri::command]
pub fn cmd_get_network_stats(state: tauri::State<'_, Mutex<MonitorState>>) -> NetworkStats {
    let mut s = state.lock().unwrap();
    get_network_stats(&mut s)
}

#[tauri::command]
pub fn cmd_get_disk_usage() -> Vec<DiskInfo> {
    get_disk_usage()
}

#[tauri::command]
pub fn cmd_get_network_interfaces() -> Vec<NetworkInterface> {
    get_network_interfaces()
}

#[tauri::command]
pub fn cmd_get_system_info(state: tauri::State<'_, Mutex<MonitorState>>) -> SystemInfo {
    let mut s = state.lock().unwrap();
    get_system_info(&mut s)
}
