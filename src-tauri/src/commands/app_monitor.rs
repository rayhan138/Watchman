use serde::{Deserialize, Serialize};
#[cfg(windows)]
use std::collections::HashMap;
#[cfg(windows)]
use std::ffi::c_void;
#[cfg(windows)]
use std::os::windows::process::CommandExt;
use std::process::Command;
#[cfg(windows)]
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex, OnceLock,
};
#[cfg(windows)]
use std::thread;
#[cfg(windows)]
use std::time::{Duration, SystemTime, UNIX_EPOCH};
#[cfg(windows)]
use sysinfo::{Pid, ProcessesToUpdate, System};
#[cfg(windows)]
use windows::{
    core::{GUID, PCWSTR, PWSTR},
    Win32::System::Diagnostics::Etw::{
        CloseTrace, ControlTraceW, OpenTraceW, ProcessTrace, StartTraceW, EVENT_RECORD,
        EVENT_TRACE_CONTROL_STOP, EVENT_TRACE_FLAG_NETWORK_TCPIP, EVENT_TRACE_FLAG_NO_SYSCONFIG,
        EVENT_TRACE_LOGFILEW, EVENT_TRACE_PROPERTIES, EVENT_TRACE_REAL_TIME_MODE,
        EVENT_TRACE_SYSTEM_LOGGER_MODE, EVENT_TRACE_CONTROL_QUERY, EVENT_TRACE_CONTROL_UPDATE,
        PROCESS_TRACE_MODE_EVENT_RECORD, PROCESS_TRACE_MODE_REAL_TIME, WNODE_FLAG_TRACED_GUID,
    },
};

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;
#[cfg(windows)]
const ERROR_ALREADY_EXISTS_CODE: u32 = 183;
#[cfg(windows)]
const ERROR_INSUFFICIENT_BUFFER_CODE: u32 = 122;
#[cfg(windows)]
const AF_INET_CODE: u32 = 2;
#[cfg(windows)]
const AF_INET6_CODE: u32 = 23;
#[cfg(windows)]
const TCP_TABLE_OWNER_PID_ALL: u32 = 5;
#[cfg(windows)]
const UDP_TABLE_OWNER_PID: u32 = 1;
#[cfg(windows)]
const TCPIP_SEND_IPV4_EVENT: u16 = 10;
#[cfg(windows)]
const TCPIP_RECV_IPV4_EVENT: u16 = 11;
#[cfg(windows)]
const TCPIP_SEND_IPV6_EVENT: u16 = 26;
#[cfg(windows)]
const TCPIP_RECV_IPV6_EVENT: u16 = 27;
#[cfg(windows)]
const TRACE_SESSION_NAME: &str = "TrafficMonitor App Network Session";
#[cfg(windows)]
const TRACE_SESSION_GUID: GUID = GUID::from_u128(0x9d1d28f2_8d75_4b16_b6f7_87e5f9b87af7);
#[cfg(windows)]
const TCPIP_PROVIDER_GUID: GUID = GUID::from_u128(0x9a280ac0_c8e0_11d1_84e2_00c04fb998a2);
#[cfg(windows)]
const UDPIP_PROVIDER_GUID: GUID = GUID::from_u128(0xbf3a50c5_a9c9_4988_a005_2df0b7c80f80);

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppUsage {
    pub pid: u32,
    pub name: String,
    #[serde(rename = "uploadSpeed")]
    pub upload_speed: f64,
    #[serde(rename = "downloadSpeed")]
    pub download_speed: f64,
    #[serde(rename = "totalUpload")]
    pub total_upload: u64,
    #[serde(rename = "totalDownload")]
    pub total_download: u64,
    pub connections: u32,
    pub icon: String,
    pub status: String,
    #[serde(rename = "isRunning")]
    pub is_running: bool,
    #[serde(rename = "lastActiveSeconds")]
    pub last_active_seconds: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TerminationResult {
    pub success: bool,
    pub pid: u32,
    pub error: Option<String>,
    pub message: Option<String>,
}

fn hidden_command(program: &str) -> Command {
    let mut cmd = Command::new(program);
    #[cfg(windows)]
    cmd.creation_flags(CREATE_NO_WINDOW);
    cmd
}

#[cfg(windows)]
#[derive(Default)]
struct TrackedProcess {
    pid: u32,
    name: String,
    total_upload: u64,
    total_download: u64,
    interval_upload: u64,
    interval_download: u64,
    upload_speed: f64,
    download_speed: f64,
    connections: u32,
    last_active_ms: u64,
    is_running: bool,
}

#[cfg(windows)]
#[derive(Default)]
struct TrackerState {
    processes: HashMap<u32, TrackedProcess>,
}

#[cfg(windows)]
struct AppTrackerHandle {
    state: Arc<Mutex<TrackerState>>,
    stop: Arc<AtomicBool>,
    session_state: TraceSessionState,
}

#[cfg(windows)]
static APP_TRACKER: OnceLock<AppTrackerHandle> = OnceLock::new();

#[cfg(windows)]
#[derive(Clone, Copy, PartialEq, Eq)]
enum TraceSessionState {
    Started,
    Attached,
    RequiresAdmin,
    Unavailable,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppMonitorStatus {
    #[serde(rename = "bandwidthAvailable")]
    pub bandwidth_available: bool,
    #[serde(rename = "requiresAdmin")]
    pub requires_admin: bool,
    pub message: Option<String>,
}

#[cfg(windows)]
#[repr(C)]
struct MibTcpRowOwnerPid {
    _state: u32,
    _local_addr: u32,
    _local_port: u32,
    _remote_addr: u32,
    _remote_port: u32,
    owning_pid: u32,
}

#[cfg(windows)]
#[repr(C)]
struct MibTcpTableOwnerPid {
    entry_count: u32,
    table: [MibTcpRowOwnerPid; 1],
}

#[cfg(windows)]
#[repr(C)]
struct MibTcp6RowOwnerPid {
    _local_addr: [u8; 16],
    _local_scope_id: u32,
    _local_port: u32,
    _remote_addr: [u8; 16],
    _remote_scope_id: u32,
    _remote_port: u32,
    _state: u32,
    owning_pid: u32,
}

#[cfg(windows)]
#[repr(C)]
struct MibTcp6TableOwnerPid {
    entry_count: u32,
    table: [MibTcp6RowOwnerPid; 1],
}

#[cfg(windows)]
#[repr(C)]
struct MibUdpRowOwnerPid {
    _local_addr: u32,
    _local_port: u32,
    owning_pid: u32,
}

#[cfg(windows)]
#[repr(C)]
struct MibUdpTableOwnerPid {
    entry_count: u32,
    table: [MibUdpRowOwnerPid; 1],
}

#[cfg(windows)]
#[repr(C)]
struct MibUdp6RowOwnerPid {
    _local_addr: [u8; 16],
    _local_scope_id: u32,
    _local_port: u32,
    owning_pid: u32,
}

#[cfg(windows)]
#[repr(C)]
struct MibUdp6TableOwnerPid {
    entry_count: u32,
    table: [MibUdp6RowOwnerPid; 1],
}

#[cfg(windows)]
#[link(name = "iphlpapi")]
unsafe extern "system" {
    fn GetExtendedTcpTable(
        table: *mut c_void,
        size: *mut u32,
        order: i32,
        af: u32,
        table_class: u32,
        reserved: u32,
    ) -> u32;
    fn GetExtendedUdpTable(
        table: *mut c_void,
        size: *mut u32,
        order: i32,
        af: u32,
        table_class: u32,
        reserved: u32,
    ) -> u32;
}

#[cfg(windows)]
impl AppTrackerHandle {
    fn start() -> Self {
        let state = Arc::new(Mutex::new(TrackerState::default()));
        let stop = Arc::new(AtomicBool::new(false));
        let session_state = ensure_trace_session();

        if session_state != TraceSessionState::Unavailable {
            spawn_etw_consumer(state.clone(), stop.clone());
        }
        spawn_tracker_maintenance(state.clone(), stop.clone());

        Self {
            state,
            stop,
            session_state,
        }
    }

    fn snapshot(&self) -> Vec<AppUsage> {
        build_snapshot(&self.state)
    }

    fn status(&self) -> AppMonitorStatus {
        monitor_status_from_state(self.session_state)
    }

    fn shutdown(&self) {
        self.stop.store(true, Ordering::SeqCst);
        if self.session_state == TraceSessionState::Started {
            stop_trace_session(TRACE_SESSION_NAME);
        }
    }
}

#[cfg(windows)]
fn tracker() -> &'static AppTrackerHandle {
    APP_TRACKER.get_or_init(AppTrackerHandle::start)
}

#[cfg(windows)]
pub fn start_tracker() {
    let _ = tracker();
}

#[cfg(not(windows))]
pub fn start_tracker() {}

#[cfg(windows)]
pub fn shutdown_tracker() {
    if let Some(tracker) = APP_TRACKER.get() {
        tracker.shutdown();
    }
}

#[cfg(not(windows))]
pub fn shutdown_tracker() {}

#[cfg(windows)]
fn current_timestamp_millis() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

#[cfg(windows)]
fn wide_null(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}

#[cfg(windows)]
fn properties_buffer(session_name: &[u16]) -> Vec<u8> {
    let bytes = std::mem::size_of::<EVENT_TRACE_PROPERTIES>() + session_name.len() * 2;
    vec![0u8; bytes]
}

#[cfg(windows)]
unsafe fn configure_properties(
    buffer: &mut [u8],
    session_name: &[u16],
) -> *mut EVENT_TRACE_PROPERTIES {
    let props = buffer.as_mut_ptr() as *mut EVENT_TRACE_PROPERTIES;
    (*props).Wnode.BufferSize = buffer.len() as u32;
    (*props).Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    (*props).Wnode.ClientContext = 1;
    (*props).Wnode.Guid = TRACE_SESSION_GUID;
    (*props).BufferSize = 64;
    (*props).MinimumBuffers = 16;
    (*props).MaximumBuffers = 64;
    (*props).FlushTimer = 1;
    (*props).EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP | EVENT_TRACE_FLAG_NO_SYSCONFIG;
    (*props).LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    (*props).LoggerNameOffset = std::mem::size_of::<EVENT_TRACE_PROPERTIES>() as u32;

    let name_ptr = buffer
        .as_mut_ptr()
        .add((*props).LoggerNameOffset as usize) as *mut u16;
    std::ptr::copy_nonoverlapping(session_name.as_ptr(), name_ptr, session_name.len());

    props
}

#[cfg(windows)]
fn stop_trace_session(session_name: &str) {
    let session_name = wide_null(session_name);
    let mut buffer = properties_buffer(&session_name);
    let props = unsafe { configure_properties(&mut buffer, &session_name) };
    unsafe {
        let _ = ControlTraceW(
            Default::default(),
            PCWSTR(session_name.as_ptr()),
            props,
            EVENT_TRACE_CONTROL_STOP,
        );
    }
}

#[cfg(windows)]
fn ensure_trace_session() -> TraceSessionState {
    let session_name = wide_null(TRACE_SESSION_NAME);
    let desired_flags = EVENT_TRACE_FLAG_NETWORK_TCPIP | EVENT_TRACE_FLAG_NO_SYSCONFIG;

    let mut query_buffer = properties_buffer(&session_name);
    let query_props = unsafe { configure_properties(&mut query_buffer, &session_name) };

    let query_status = unsafe {
        ControlTraceW(
            Default::default(),
            PCWSTR(session_name.as_ptr()),
            query_props,
            EVENT_TRACE_CONTROL_QUERY,
        )
    };

    if query_status.0 == 0 {
        unsafe {
            if ((*query_props).EnableFlags & desired_flags) != desired_flags {
                (*query_props).EnableFlags |= desired_flags;
                let _ = ControlTraceW(
                    Default::default(),
                    PCWSTR(session_name.as_ptr()),
                    query_props,
                    EVENT_TRACE_CONTROL_UPDATE,
                );
            }
        }

        return TraceSessionState::Attached;
    }

    if query_status.0 == 5 {
        return TraceSessionState::RequiresAdmin;
    }

    let mut start_buffer = properties_buffer(&session_name);
    let start_props = unsafe { configure_properties(&mut start_buffer, &session_name) };
    let mut session_handle = Default::default();
    let start_status =
        unsafe { StartTraceW(&mut session_handle, PCWSTR(session_name.as_ptr()), start_props) };

    if start_status.0 == 0 {
        return TraceSessionState::Started;
    }

    if start_status.0 == 5 {
        return TraceSessionState::RequiresAdmin;
    }

    if start_status.0 == ERROR_ALREADY_EXISTS_CODE {
        let mut attach_buffer = properties_buffer(&session_name);
        let attach_props = unsafe { configure_properties(&mut attach_buffer, &session_name) };
        let attach_status = unsafe {
            ControlTraceW(
                Default::default(),
                PCWSTR(session_name.as_ptr()),
                attach_props,
                EVENT_TRACE_CONTROL_QUERY,
            )
        };
        if attach_status.0 == 0 {
            unsafe {
                if ((*attach_props).EnableFlags & desired_flags) != desired_flags {
                    (*attach_props).EnableFlags |= desired_flags;
                    let _ = ControlTraceW(
                        Default::default(),
                        PCWSTR(session_name.as_ptr()),
                        attach_props,
                        EVENT_TRACE_CONTROL_UPDATE,
                    );
                }
            }
            return TraceSessionState::Attached;
        }

        if attach_status.0 == 5 {
            return TraceSessionState::RequiresAdmin;
        }
    }

    TraceSessionState::Unavailable
}

#[cfg(windows)]
fn spawn_etw_consumer(state: Arc<Mutex<TrackerState>>, stop: Arc<AtomicBool>) {
    thread::spawn(move || {
        let session_name = wide_null(TRACE_SESSION_NAME);
        let context_ptr = Arc::into_raw(state.clone()) as *mut c_void;
        let mut logfile = EVENT_TRACE_LOGFILEW::default();
        logfile.LoggerName = PWSTR(session_name.as_ptr() as *mut _);
        logfile.Anonymous1.ProcessTraceMode =
            PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        logfile.Anonymous2.EventRecordCallback = Some(etw_event_callback);
        logfile.Context = context_ptr;

        let trace_handle = unsafe { OpenTraceW(&mut logfile) };
        if trace_handle.Value == u64::MAX {
            let _ = unsafe { Arc::from_raw(context_ptr as *const Mutex<TrackerState>) };
            return;
        }

        let _ = unsafe { ProcessTrace(&[trace_handle], None, None) };
        let _ = unsafe { CloseTrace(trace_handle) };
        let _ = unsafe { Arc::from_raw(context_ptr as *const Mutex<TrackerState>) };

        if !stop.load(Ordering::SeqCst) {
            stop_trace_session(TRACE_SESSION_NAME);
        }
    });
}

#[cfg(windows)]
fn spawn_tracker_maintenance(state: Arc<Mutex<TrackerState>>, stop: Arc<AtomicBool>) {
    thread::spawn(move || {
        let mut system = System::new_all();

        while !stop.load(Ordering::SeqCst) {
            system.refresh_processes(ProcessesToUpdate::All, true);
            let connection_counts = count_connections_by_pid();

            if let Ok(mut tracker_state) = state.lock() {
                for (pid, connections) in &connection_counts {
                    let entry = tracker_state
                        .processes
                        .entry(*pid)
                        .or_insert_with(|| TrackedProcess {
                            pid: *pid,
                            name: format!("Process {}", pid),
                            last_active_ms: current_timestamp_millis(),
                            ..TrackedProcess::default()
                        });

                    entry.connections = *connections;

                    if let Some(sys_process) = system.process(Pid::from_u32(*pid)) {
                        let display_name = resolve_process_name(sys_process);
                        if !display_name.is_empty() {
                            entry.name = display_name;
                        }
                        entry.is_running = true;
                    }
                }

                for (pid, process) in tracker_state.processes.iter_mut() {
                    process.download_speed = process.interval_download as f64;
                    process.upload_speed = process.interval_upload as f64;
                    process.interval_download = 0;
                    process.interval_upload = 0;
                    process.connections = connection_counts.get(pid).copied().unwrap_or(0);

                    if let Some(sys_process) = system.process(Pid::from_u32(*pid)) {
                        let display_name = resolve_process_name(sys_process);
                        if !display_name.is_empty() {
                            process.name = display_name;
                        }
                        process.is_running = true;
                    } else {
                        process.is_running = false;
                    }
                }
            }

            thread::sleep(Duration::from_secs(1));
        }
    });
}

#[cfg(windows)]
fn resolve_process_name(process: &sysinfo::Process) -> String {
    let exe_name = process
        .exe()
        .and_then(|path| path.file_stem())
        .map(|name| name.to_string_lossy().trim().to_string())
        .filter(|name| !name.is_empty());

    if let Some(name) = exe_name {
        return name;
    }

    process.name().to_string_lossy().trim().to_string()
}

#[cfg(windows)]
fn build_snapshot(state: &Arc<Mutex<TrackerState>>) -> Vec<AppUsage> {
    let now_ms = current_timestamp_millis();
    let mut grouped: HashMap<String, AppUsage> = HashMap::new();

    let Ok(tracker_state) = state.lock() else {
        return Vec::new();
    };

    for process in tracker_state.processes.values() {
        if process.total_download == 0
            && process.total_upload == 0
            && process.connections == 0
            && process.download_speed == 0.0
            && process.upload_speed == 0.0
        {
            continue;
        }

        let name = if process.name.is_empty() {
            format!("Process {}", process.pid)
        } else {
            process.name.clone()
        };

        let entry = grouped.entry(name.clone()).or_insert(AppUsage {
            pid: process.pid,
            name,
            upload_speed: 0.0,
            download_speed: 0.0,
            total_upload: 0,
            total_download: 0,
            connections: 0,
            icon: String::new(),
            status: String::new(),
            is_running: false,
            last_active_seconds: 0,
        });

        entry.total_upload = entry.total_upload.saturating_add(process.total_upload);
        entry.total_download = entry.total_download.saturating_add(process.total_download);
        entry.upload_speed += process.upload_speed;
        entry.download_speed += process.download_speed;
        entry.connections = entry.connections.saturating_add(process.connections);
        entry.is_running |= process.is_running;

        let seconds_ago = now_ms
            .saturating_sub(process.last_active_ms)
            .checked_div(1000)
            .unwrap_or(0);
        if entry.last_active_seconds == 0 || seconds_ago < entry.last_active_seconds {
            entry.last_active_seconds = seconds_ago;
            entry.pid = process.pid;
        }
    }

    let mut apps: Vec<AppUsage> = grouped
        .into_values()
        .map(|mut app| {
            app.status = if app.download_speed > 0.0 || app.upload_speed > 0.0 {
                "live".to_string()
            } else if app.is_running {
                "background".to_string()
            } else {
                "recent".to_string()
            };
            app
        })
        .collect();

    apps.sort_by(|a, b| {
        status_rank(&a.status)
            .cmp(&status_rank(&b.status))
            .then_with(|| a.last_active_seconds.cmp(&b.last_active_seconds))
            .then_with(|| {
                (b.total_download + b.total_upload).cmp(&(a.total_download + a.total_upload))
            })
            .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
    });

    apps
}

#[cfg(windows)]
fn status_rank(status: &str) -> u8 {
    match status {
        "live" => 0,
        "background" => 1,
        _ => 2,
    }
}

#[cfg(windows)]
fn count_connections_by_pid() -> HashMap<u32, u32> {
    let mut counts = HashMap::new();

    accumulate_tcp4_counts(&mut counts);
    accumulate_tcp6_counts(&mut counts);
    accumulate_udp4_counts(&mut counts);
    accumulate_udp6_counts(&mut counts);

    counts
}

#[cfg(windows)]
fn accumulate_tcp4_counts(counts: &mut HashMap<u32, u32>) {
    let Some(buffer) = table_buffer_tcp(AF_INET_CODE) else {
        return;
    };

    unsafe {
        let table = &*(buffer.as_ptr() as *const MibTcpTableOwnerPid);
        let rows = std::slice::from_raw_parts(table.table.as_ptr(), table.entry_count as usize);
        for row in rows {
            let pid = row.owning_pid;
            if pid > 0 {
                *counts.entry(pid).or_insert(0) += 1;
            }
        }
    }
}

#[cfg(windows)]
fn accumulate_tcp6_counts(counts: &mut HashMap<u32, u32>) {
    let Some(buffer) = table_buffer_tcp(AF_INET6_CODE) else {
        return;
    };

    unsafe {
        let table = &*(buffer.as_ptr() as *const MibTcp6TableOwnerPid);
        let rows = std::slice::from_raw_parts(table.table.as_ptr(), table.entry_count as usize);
        for row in rows {
            let pid = row.owning_pid;
            if pid > 0 {
                *counts.entry(pid).or_insert(0) += 1;
            }
        }
    }
}

#[cfg(windows)]
fn accumulate_udp4_counts(counts: &mut HashMap<u32, u32>) {
    let Some(buffer) = table_buffer_udp(AF_INET_CODE) else {
        return;
    };

    unsafe {
        let table = &*(buffer.as_ptr() as *const MibUdpTableOwnerPid);
        let rows = std::slice::from_raw_parts(table.table.as_ptr(), table.entry_count as usize);
        for row in rows {
            let pid = row.owning_pid;
            if pid > 0 {
                *counts.entry(pid).or_insert(0) += 1;
            }
        }
    }
}

#[cfg(windows)]
fn accumulate_udp6_counts(counts: &mut HashMap<u32, u32>) {
    let Some(buffer) = table_buffer_udp(AF_INET6_CODE) else {
        return;
    };

    unsafe {
        let table = &*(buffer.as_ptr() as *const MibUdp6TableOwnerPid);
        let rows = std::slice::from_raw_parts(table.table.as_ptr(), table.entry_count as usize);
        for row in rows {
            let pid = row.owning_pid;
            if pid > 0 {
                *counts.entry(pid).or_insert(0) += 1;
            }
        }
    }
}

#[cfg(windows)]
fn table_buffer_tcp(af: u32) -> Option<Vec<u8>> {
    let mut size = 0u32;
    let status = unsafe {
        GetExtendedTcpTable(
            std::ptr::null_mut(),
            &mut size,
            0,
            af,
            TCP_TABLE_OWNER_PID_ALL,
            0,
        )
    };

    if status != ERROR_INSUFFICIENT_BUFFER_CODE || size == 0 {
        return None;
    }

    let mut buffer = vec![0u8; size as usize];
    let status = unsafe {
        GetExtendedTcpTable(
            buffer.as_mut_ptr() as *mut c_void,
            &mut size,
            0,
            af,
            TCP_TABLE_OWNER_PID_ALL,
            0,
        )
    };

    (status == 0).then_some(buffer)
}

#[cfg(windows)]
fn table_buffer_udp(af: u32) -> Option<Vec<u8>> {
    let mut size = 0u32;
    let status = unsafe {
        GetExtendedUdpTable(
            std::ptr::null_mut(),
            &mut size,
            0,
            af,
            UDP_TABLE_OWNER_PID,
            0,
        )
    };

    if status != ERROR_INSUFFICIENT_BUFFER_CODE || size == 0 {
        return None;
    }

    let mut buffer = vec![0u8; size as usize];
    let status = unsafe {
        GetExtendedUdpTable(
            buffer.as_mut_ptr() as *mut c_void,
            &mut size,
            0,
            af,
            UDP_TABLE_OWNER_PID,
            0,
        )
    };

    (status == 0).then_some(buffer)
}

#[cfg(windows)]
unsafe extern "system" fn etw_event_callback(record: *mut EVENT_RECORD) {
    if record.is_null() {
        return;
    }

    let record = &*record;
    if record.EventHeader.ProviderId != TCPIP_PROVIDER_GUID
        && record.EventHeader.ProviderId != UDPIP_PROVIDER_GUID
    {
        return;
    }

    let descriptor = record.EventHeader.EventDescriptor;
    let event_id = if descriptor.Opcode != 0 {
        descriptor.Opcode as u16
    } else {
        descriptor.Id
    };
    let is_send = matches!(event_id, TCPIP_SEND_IPV4_EVENT | TCPIP_SEND_IPV6_EVENT);
    let is_recv = matches!(event_id, TCPIP_RECV_IPV4_EVENT | TCPIP_RECV_IPV6_EVENT);
    if !is_send && !is_recv {
        return;
    }

    if record.UserData.is_null() || record.UserDataLength < 8 || record.UserContext.is_null() {
        return;
    }

    let payload = std::slice::from_raw_parts(record.UserData as *const u8, record.UserDataLength as usize);
    let pid = u32::from_ne_bytes([payload[0], payload[1], payload[2], payload[3]]);
    let size = u32::from_ne_bytes([payload[4], payload[5], payload[6], payload[7]]) as u64;

    if pid <= 4 || size == 0 {
        return;
    }

    let tracker_state = &*(record.UserContext as *const Mutex<TrackerState>);
    if let Ok(mut state) = tracker_state.lock() {
        let now_ms = current_timestamp_millis();
        let entry = state.processes.entry(pid).or_insert_with(|| TrackedProcess {
            pid,
            name: format!("Process {}", pid),
            last_active_ms: now_ms,
            ..TrackedProcess::default()
        });

        if is_send {
            entry.total_upload = entry.total_upload.saturating_add(size);
            entry.interval_upload = entry.interval_upload.saturating_add(size);
        } else {
            entry.total_download = entry.total_download.saturating_add(size);
            entry.interval_download = entry.interval_download.saturating_add(size);
        }

        entry.last_active_ms = now_ms;
    }
}

#[cfg(windows)]
fn monitor_status_from_state(session_state: TraceSessionState) -> AppMonitorStatus {
    match session_state {
        TraceSessionState::Started | TraceSessionState::Attached => AppMonitorStatus {
            bandwidth_available: true,
            requires_admin: false,
            message: None,
        },
        TraceSessionState::RequiresAdmin => AppMonitorStatus {
            bandwidth_available: false,
            requires_admin: true,
            message: Some(
                "Run Watchman as administrator to show per-app bandwidth on Windows."
                    .to_string(),
            ),
        },
        TraceSessionState::Unavailable => AppMonitorStatus {
            bandwidth_available: false,
            requires_admin: false,
            message: Some("Per-app bandwidth is unavailable right now.".to_string()),
        },
    }
}

#[tauri::command]
pub async fn get_active_applications() -> Vec<AppUsage> {
    #[cfg(windows)]
    {
        start_tracker();
        tracker().snapshot()
    }

    #[cfg(not(windows))]
    {
        Vec::new()
    }
}

#[tauri::command]
pub async fn get_app_monitor_status() -> AppMonitorStatus {
    #[cfg(windows)]
    {
        start_tracker();
        tracker().status()
    }

    #[cfg(not(windows))]
    {
        AppMonitorStatus {
            bandwidth_available: false,
            requires_admin: false,
            message: Some("Per-app bandwidth is only available on Windows.".to_string()),
        }
    }
}

#[tauri::command]
pub async fn terminate_application(pid: u32) -> TerminationResult {
    match hidden_command("taskkill")
        .args(["/PID", &pid.to_string(), "/F"])
        .output()
    {
        Ok(output) => {
            if output.status.success() {
                TerminationResult {
                    success: true,
                    pid,
                    error: None,
                    message: None,
                }
            } else {
                let stderr = String::from_utf8_lossy(&output.stderr).to_string();
                let (error, message) = if stderr.contains("Access is denied") {
                    (
                        "permission".to_string(),
                        "Insufficient permissions. Try running as administrator.".to_string(),
                    )
                } else if stderr.contains("not found") {
                    (
                        "not_found".to_string(),
                        "Application has already been closed.".to_string(),
                    )
                } else {
                    (
                        "unknown".to_string(),
                        format!("Unable to terminate (PID: {}).", pid),
                    )
                };
                TerminationResult {
                    success: false,
                    pid,
                    error: Some(error),
                    message: Some(message),
                }
            }
        }
        Err(e) => TerminationResult {
            success: false,
            pid,
            error: Some("unknown".to_string()),
            message: Some(format!("Failed: {}", e)),
        },
    }
}
