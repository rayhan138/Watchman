#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include "app_tracker.h"

#include <iphlpapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <unordered_set>
#include <vector>

namespace {

constexpr wchar_t kTraceSessionName[] = L"Watchman Native App Network";
constexpr ULONGLONG kKernelNetworkKeywords = 0x30ULL; // IPv4 | IPv6

const GUID kTraceSessionGuid = {
    0x91a60d7d, 0xefbb, 0x4ce9, { 0xa4, 0x6b, 0x24, 0x61, 0x9f, 0x8a, 0x92, 0x4c }
};
const GUID kTcpIpProviderGuid = {
    0x9a280ac0, 0xc8e0, 0x11d1, { 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0x2a }
};
const GUID kUdpIpProviderGuid = {
    0xbf3a50c5, 0xa9c9, 0x4988, { 0xa0, 0x05, 0x2d, 0xf0, 0x7c, 0x80, 0xf8, 0xf0 }
};
const GUID kKernelNetworkProviderGuid = {
    0x7dd42a49, 0x5329, 0x4832, { 0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88 }
};

constexpr USHORT kSendIpv4Event = 10;
constexpr USHORT kReceiveIpv4Event = 11;
constexpr USHORT kSendIpv6Event = 26;
constexpr USHORT kReceiveIpv6Event = 27;
constexpr USHORT kSendUdpIpv4Event = 42;
constexpr USHORT kReceiveUdpIpv4Event = 43;
constexpr USHORT kSendUdpIpv6Event = 58;
constexpr USHORT kReceiveUdpIpv6Event = 59;
constexpr uint64_t kLiveGraceSeconds = 4;
constexpr double kActivityScoreBlend = 0.40;
constexpr double kActivityScoreIdleDecay = 0.78;

bool IsRunningAsAdmin() {
    BOOL running_as_admin = FALSE;
    PSID administrators_group = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(
            &authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &administrators_group)) {
        CheckTokenMembership(nullptr, administrators_group, &running_as_admin);
        FreeSid(administrators_group);
    }
    return running_as_admin != FALSE;
}

bool IsProcessRunning(uint32_t pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }

    DWORD exit_code = 0;
    const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

std::vector<BYTE> CreateTracePropertiesBuffer() {
    const size_t name_chars = wcslen(kTraceSessionName) + 1;
    const size_t bytes = sizeof(EVENT_TRACE_PROPERTIES) + name_chars * sizeof(wchar_t);
    std::vector<BYTE> buffer(bytes, 0);

    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(bytes);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->Wnode.Guid = kTraceSessionGuid;
    properties->BufferSize = 64;
    properties->MinimumBuffers = 16;
    properties->MaximumBuffers = 64;
    properties->FlushTimer = 1;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    auto* name = reinterpret_cast<wchar_t*>(buffer.data() + properties->LoggerNameOffset);
    memcpy(name, kTraceSessionName, name_chars * sizeof(wchar_t));
    return buffer;
}

EVENT_TRACE_PROPERTIES* TraceProperties(std::vector<BYTE>& buffer) {
    return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
}

void StopTraceSession() {
    auto properties_buffer = CreateTracePropertiesBuffer();
    ControlTraceW(0, kTraceSessionName, TraceProperties(properties_buffer), EVENT_TRACE_CONTROL_STOP);
}

void AddTcp4ConnectionCounts(std::unordered_map<uint32_t, uint32_t>& counts) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return;
    }

    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const uint32_t pid = table->table[index].dwOwningPid;
        if (pid > 0) {
            ++counts[pid];
        }
    }
}

void AddTcp6ConnectionCounts(std::unordered_map<uint32_t, uint32_t>& counts) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return;
    }

    const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const uint32_t pid = table->table[index].dwOwningPid;
        if (pid > 0) {
            ++counts[pid];
        }
    }
}

void AddUdp4ConnectionCounts(std::unordered_map<uint32_t, uint32_t>& counts) {
    ULONG size = 0;
    if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) {
        return;
    }

    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const uint32_t pid = table->table[index].dwOwningPid;
        if (pid > 0) {
            ++counts[pid];
        }
    }
}

void AddUdp6ConnectionCounts(std::unordered_map<uint32_t, uint32_t>& counts) {
    ULONG size = 0;
    if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return;
    }

    std::vector<BYTE> buffer(size);
    if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) {
        return;
    }

    const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const uint32_t pid = table->table[index].dwOwningPid;
        if (pid > 0) {
            ++counts[pid];
        }
    }
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring RemoveExecutableExtension(std::wstring value) {
    const size_t extension = value.find_last_of(L'.');
    if (extension != std::wstring::npos && extension > 0) {
        value.resize(extension);
    }
    return value;
}

std::wstring FindProcessNameInSnapshot(uint32_t pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return L"";
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring name;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                name = RemoveExecutableExtension(entry.szExeFile);
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return name;
}

} // namespace

AppTracker::AppTracker() {
    process_name_cache_[0] = L"Idle";
    process_name_cache_[4] = L"System";
    is_admin_ = IsRunningAsAdmin();
    StartEtwMonitoring();
}

AppTracker::~AppTracker() {
    StopEtwMonitoring();
}

bool AppTracker::IsBandwidthAvailable() const {
    const bool trace_ready = trace_session_state_ == AppTraceSessionState::Started ||
        trace_session_state_ == AppTraceSessionState::Attached;
    return is_admin_ && trace_ready && etw_consumer_connected_.load();
}

bool AppTracker::RequiresAdmin() const {
    return !is_admin_ || trace_session_state_ == AppTraceSessionState::RequiresAdmin;
}

std::wstring AppTracker::GetStatusMessage() const {
    if (RequiresAdmin()) {
        return L"Run Watchman as administrator to show per-app bandwidth on Windows.";
    }
    if (trace_session_state_ == AppTraceSessionState::Unavailable) {
        return L"Per-app bandwidth is unavailable right now.";
    }
    if (!etw_consumer_connected_.load()) {
        return L"Connecting to Windows per-app network tracking...";
    }
    return L"";
}

void AppTracker::StartEtwMonitoring() {
    trace_session_state_ = EnsureTraceSession();
    const bool trace_ready = trace_session_state_ == AppTraceSessionState::Started ||
        trace_session_state_ == AppTraceSessionState::Attached;
    if (is_admin_ && trace_ready) {
        etw_sample_interval_.Reset(GetTickCount64());
        etw_thread_ = std::thread(&AppTracker::ConsumeEtwEvents, this);
    }
}

void AppTracker::StopEtwMonitoring() {
    etw_stop_.store(true);
    etw_consumer_connected_.store(false);

    if (owns_trace_session_) {
        StopTraceSession();
    }
    etw_session_handle_ = 0;
    CloseEtwConsumerTrace();

    if (etw_thread_.joinable()) {
        etw_thread_.join();
    }
}

AppTraceSessionState AppTracker::EnsureTraceSession() {
    auto query_buffer = CreateTracePropertiesBuffer();
    auto* query_properties = TraceProperties(query_buffer);
    ULONG status = ControlTraceW(0, kTraceSessionName, query_properties, EVENT_TRACE_CONTROL_QUERY);
    if (status == ERROR_SUCCESS) {
        etw_session_handle_ = static_cast<TRACEHANDLE>(query_properties->Wnode.HistoricalContext);
        status = EnableKernelNetworkProvider(etw_session_handle_);
        if (status != ERROR_SUCCESS) {
            return status == ERROR_ACCESS_DENIED
                ? AppTraceSessionState::RequiresAdmin
                : AppTraceSessionState::Unavailable;
        }
        owns_trace_session_ = false;
        return AppTraceSessionState::Attached;
    }
    if (status == ERROR_ACCESS_DENIED) {
        return AppTraceSessionState::RequiresAdmin;
    }

    auto start_buffer = CreateTracePropertiesBuffer();
    TRACEHANDLE session_handle = 0;
    status = StartTraceW(&session_handle, kTraceSessionName, TraceProperties(start_buffer));
    if (status == ERROR_SUCCESS) {
        etw_session_handle_ = session_handle;
        status = EnableKernelNetworkProvider(session_handle);
        if (status != ERROR_SUCCESS) {
            StopTraceSession();
            etw_session_handle_ = 0;
            return status == ERROR_ACCESS_DENIED
                ? AppTraceSessionState::RequiresAdmin
                : AppTraceSessionState::Unavailable;
        }
        owns_trace_session_ = true;
        return AppTraceSessionState::Started;
    }
    if (status == ERROR_ACCESS_DENIED) {
        return AppTraceSessionState::RequiresAdmin;
    }
    if (status == ERROR_ALREADY_EXISTS) {
        auto attach_buffer = CreateTracePropertiesBuffer();
        auto* attach_properties = TraceProperties(attach_buffer);
        status = ControlTraceW(0, kTraceSessionName, attach_properties, EVENT_TRACE_CONTROL_QUERY);
        if (status == ERROR_SUCCESS) {
            etw_session_handle_ = static_cast<TRACEHANDLE>(attach_properties->Wnode.HistoricalContext);
            status = EnableKernelNetworkProvider(etw_session_handle_);
            if (status != ERROR_SUCCESS) {
                return status == ERROR_ACCESS_DENIED
                    ? AppTraceSessionState::RequiresAdmin
                    : AppTraceSessionState::Unavailable;
            }
            owns_trace_session_ = false;
            return AppTraceSessionState::Attached;
        }
        if (status == ERROR_ACCESS_DENIED) {
            return AppTraceSessionState::RequiresAdmin;
        }
    }

    return AppTraceSessionState::Unavailable;
}

ULONG AppTracker::EnableKernelNetworkProvider(TRACEHANDLE session_handle) {
    if (session_handle == 0) {
        return ERROR_INVALID_HANDLE;
    }

    return EnableTraceEx2(
        session_handle,
        &kKernelNetworkProviderGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        kKernelNetworkKeywords,
        0,
        0,
        nullptr
    );
}

void AppTracker::ConsumeEtwEvents() {
    while (!etw_stop_.load()) {
        // Use an explicitly terminated buffer, matching the Rust monitor's
        // wide_null() call. Some ETW consumers keep this pointer while opening.
        std::wstring session_name = kTraceSessionName;
        session_name.push_back(L'\0');
        EVENT_TRACE_LOGFILEW logfile{};
        logfile.LoggerName = session_name.data();
        logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        logfile.EventRecordCallback = &AppTracker::EtwEventRecordCallback;
        logfile.Context = this;

        const TRACEHANDLE trace_handle = OpenTraceW(&logfile);
        if (trace_handle == INVALID_PROCESSTRACE_HANDLE) {
            etw_consumer_connected_.store(false);
            etw_consumer_last_error_.store(GetLastError());
            if (!etw_stop_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        etw_consumer_last_error_.store(ERROR_SUCCESS);
        etw_consumer_connected_.store(true);
        {
            std::lock_guard<std::mutex> lock(etw_trace_mutex_);
            etw_consumer_handle_ = trace_handle;
        }

        if (etw_stop_.load()) {
            CloseEtwConsumerTrace();
            break;
        }

        ProcessTrace(const_cast<PTRACEHANDLE>(&trace_handle), 1, nullptr, nullptr);
        etw_consumer_connected_.store(false);

        bool close_handle = false;
        {
            std::lock_guard<std::mutex> lock(etw_trace_mutex_);
            if (etw_consumer_handle_ == trace_handle) {
                etw_consumer_handle_ = INVALID_PROCESSTRACE_HANDLE;
                close_handle = true;
            }
        }
        if (close_handle) {
            CloseTrace(trace_handle);
        }

        if (!etw_stop_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

void AppTracker::CloseEtwConsumerTrace() {
    TRACEHANDLE trace_handle = INVALID_PROCESSTRACE_HANDLE;
    {
        std::lock_guard<std::mutex> lock(etw_trace_mutex_);
        trace_handle = etw_consumer_handle_;
        etw_consumer_handle_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (trace_handle != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(trace_handle);
    }
}

VOID WINAPI AppTracker::EtwEventRecordCallback(PEVENT_RECORD record) {
    if (record == nullptr || record->UserContext == nullptr ||
        record->UserData == nullptr || record->UserDataLength < 8) {
        return;
    }

    const GUID& provider = record->EventHeader.ProviderId;
    if (!InlineIsEqualGUID(provider, kTcpIpProviderGuid) &&
        !InlineIsEqualGUID(provider, kUdpIpProviderGuid) &&
        !InlineIsEqualGUID(provider, kKernelNetworkProviderGuid)) {
        return;
    }

    const EVENT_DESCRIPTOR& descriptor = record->EventHeader.EventDescriptor;
    const auto is_matching_event = [&](USHORT event) {
        return event == kSendIpv4Event || event == kSendIpv6Event ||
            event == kReceiveIpv4Event || event == kReceiveIpv6Event ||
            event == kSendUdpIpv4Event || event == kSendUdpIpv6Event ||
            event == kReceiveUdpIpv4Event || event == kReceiveUdpIpv6Event;
    };
    const USHORT event_id = is_matching_event(descriptor.Opcode)
        ? descriptor.Opcode
        : descriptor.Id;
    const bool is_upload = event_id == kSendIpv4Event || event_id == kSendIpv6Event ||
        event_id == kSendUdpIpv4Event || event_id == kSendUdpIpv6Event;
    const bool is_download = event_id == kReceiveIpv4Event || event_id == kReceiveIpv6Event ||
        event_id == kReceiveUdpIpv4Event || event_id == kReceiveUdpIpv6Event;
    if (!is_upload && !is_download) {
        return;
    }

    const auto* payload = static_cast<const BYTE*>(record->UserData);
    uint32_t pid = 0;
    uint32_t size = 0;
    memcpy(&pid, payload, sizeof(pid));
    memcpy(&size, payload + sizeof(pid), sizeof(size));
    if (pid <= 4 || size == 0) {
        return;
    }

    auto* tracker = static_cast<AppTracker*>(record->UserContext);
    tracker->AccumulateEtwTraffic(pid, is_upload, size);
}

void AppTracker::AccumulateEtwTraffic(uint32_t pid, bool upload, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(etw_pending_mutex_);
    auto& delta = etw_pending_[pid];
    if (upload) {
        delta.upload_bytes += bytes;
    } else {
        delta.download_bytes += bytes;
    }
}

void AppTracker::Refresh(double total_system_download_bps, double total_system_upload_bps) {
    UNREFERENCED_PARAMETER(total_system_download_bps);
    UNREFERENCED_PARAMETER(total_system_upload_bps);

    std::unordered_map<uint32_t, uint32_t> connection_counts;
    AddTcp4ConnectionCounts(connection_counts);
    AddTcp6ConnectionCounts(connection_counts);
    AddUdp4ConnectionCounts(connection_counts);
    AddUdp6ConnectionCounts(connection_counts);

    std::unordered_map<uint32_t, EtwTrafficDelta> pending_traffic;
    const bool bandwidth_available = IsBandwidthAvailable();
    uint64_t elapsed_ms = 0;
    if (bandwidth_available) {
        std::lock_guard<std::mutex> lock(etw_pending_mutex_);
        pending_traffic.swap(etw_pending_);
        // Keep the clock and byte-drain boundaries together. If the consumer
        // disconnects, retained bytes keep their original elapsed interval.
        elapsed_ms = etw_sample_interval_.Advance(GetTickCount64());
    }

    const uint64_t now_ms = GetTickCount64();
    std::unordered_set<uint32_t> observed_pids;
    for (const auto& [pid, count] : connection_counts) {
        UNREFERENCED_PARAMETER(count);
        if (pid > 4) {
            observed_pids.insert(pid);
        }
    }
    for (const auto& [pid, traffic] : pending_traffic) {
        UNREFERENCED_PARAMETER(traffic);
        if (pid > 4) {
            observed_pids.insert(pid);
        }
    }

    for (uint32_t pid : observed_pids) {
        auto [tracked, inserted] = tracked_apps_.try_emplace(pid);
        AppNetworkInfo& info = tracked->second;
        if (inserted) {
            info.pid = pid;
            info.name = GetProcessName(pid);
            last_active_tick_[pid] = now_ms;
        } else if (info.name.empty() || info.name == L"Unknown") {
            process_name_cache_.erase(pid);
            info.name = GetProcessName(pid);
        }

        const auto connection_it = connection_counts.find(pid);
        info.connection_count = connection_it == connection_counts.end() ? 0 : connection_it->second;
        info.is_system = pid == 0 || pid == 4;
        info.is_running = info.connection_count > 0 || IsProcessRunning(pid);

        const auto traffic_it = pending_traffic.find(pid);
        const EtwTrafficDelta delta = traffic_it == pending_traffic.end() ? EtwTrafficDelta{} : traffic_it->second;
        if (bandwidth_available) {
            info.interval_download_bytes = delta.download_bytes;
            info.interval_upload_bytes = delta.upload_bytes;
            info.download_speed_kib_per_sec = Telemetry::KibibytesPerSecond(delta.download_bytes, elapsed_ms);
            info.upload_speed_kib_per_sec = Telemetry::KibibytesPerSecond(delta.upload_bytes, elapsed_ms);
            info.download_speed_kbps = info.download_speed_kib_per_sec;
            info.upload_speed_kbps = info.upload_speed_kib_per_sec;
            const double instant_bandwidth_kib = info.download_speed_kib_per_sec + info.upload_speed_kib_per_sec;
            if (instant_bandwidth_kib > 0.0) {
                info.activity_score +=
                    (instant_bandwidth_kib - info.activity_score) * kActivityScoreBlend;
            } else {
                info.activity_score *= kActivityScoreIdleDecay;
                if (info.activity_score < 0.01) {
                    info.activity_score = 0.0;
                }
            }
            info.activity_score_kbps = info.activity_score;
            info.total_download_bytes += delta.download_bytes;
            info.total_upload_bytes += delta.upload_bytes;
            info.total_bytes = info.total_download_bytes + info.total_upload_bytes;
            if (delta.download_bytes > 0 || delta.upload_bytes > 0) {
                last_active_tick_[pid] = now_ms;
            }
        } else {
            info.interval_download_bytes = 0;
            info.interval_upload_bytes = 0;
            info.download_speed_kib_per_sec = 0.0;
            info.upload_speed_kib_per_sec = 0.0;
            info.download_speed_kbps = 0.0;
            info.upload_speed_kbps = 0.0;
        }
    }

    for (auto& [pid, info] : tracked_apps_) {
        if (observed_pids.find(pid) == observed_pids.end()) {
            info.connection_count = 0;
            info.interval_download_bytes = 0;
            info.interval_upload_bytes = 0;
            info.download_speed_kib_per_sec = 0.0;
            info.upload_speed_kib_per_sec = 0.0;
            info.download_speed_kbps = 0.0;
            info.upload_speed_kbps = 0.0;
            info.activity_score *= kActivityScoreIdleDecay;
            if (info.activity_score < 0.01) {
                info.activity_score = 0.0;
            }
            info.activity_score_kbps = info.activity_score;
            info.is_running = IsProcessRunning(pid);
        }

        const auto last_active = last_active_tick_.find(pid);
        info.last_active_seconds = last_active != last_active_tick_.end() && now_ms >= last_active->second
            ? (now_ms - last_active->second) / 1000
            : 0;
    }

    std::unordered_map<std::wstring, AppNetworkInfo> grouped_apps;
    for (const auto& [pid, info] : tracked_apps_) {
        UNREFERENCED_PARAMETER(pid);
        if (info.total_download_bytes == 0 && info.total_upload_bytes == 0 &&
            info.connection_count == 0 && info.interval_download_bytes == 0 &&
            info.interval_upload_bytes == 0) {
            continue;
        }

        const std::wstring name = info.name.empty() ? L"Process " + std::to_wstring(info.pid) : info.name;
        auto [entry, inserted] = grouped_apps.try_emplace(name);
        AppNetworkInfo& grouped = entry->second;
        if (inserted) {
            grouped.pid = info.pid;
            grouped.name = name;
            grouped.last_active_seconds = info.last_active_seconds;
        } else if (info.last_active_seconds < grouped.last_active_seconds) {
            grouped.pid = info.pid;
            grouped.last_active_seconds = info.last_active_seconds;
        }

        grouped.connection_count += info.connection_count;
        grouped.AddTrafficCounters(info);
        grouped.is_system = grouped.is_system || info.is_system;
        grouped.is_running = grouped.is_running || info.is_running;
    }

    active_apps_.clear();
    active_apps_.reserve(grouped_apps.size());
    for (auto& [name, app] : grouped_apps) {
        UNREFERENCED_PARAMETER(name);
        const bool recently_live = app.is_running &&
            app.last_active_seconds <= kLiveGraceSeconds &&
            (app.total_download_bytes > 0 || app.total_upload_bytes > 0);
        if (recently_live) {
            app.status = L"live";
        } else if (app.is_running) {
            app.status = L"background";
        } else {
            app.status = L"recent";
        }
        active_apps_.push_back(std::move(app));
    }

    const auto status_rank = [](const std::wstring& status) {
        if (status == L"live") return 0;
        if (status == L"background") return 1;
        return 2;
    };
    std::sort(active_apps_.begin(), active_apps_.end(), [&](const AppNetworkInfo& left, const AppNetworkInfo& right) {
        const int left_rank = status_rank(left.status);
        const int right_rank = status_rank(right.status);
        if (left_rank != right_rank) {
            return left_rank < right_rank;
        }
        if (left.activity_score_kbps > right.activity_score_kbps + 0.05) {
            return true;
        }
        if (right.activity_score_kbps > left.activity_score_kbps + 0.05) {
            return false;
        }
        const uint64_t left_total = left.total_download_bytes + left.total_upload_bytes;
        const uint64_t right_total = right.total_download_bytes + right.total_upload_bytes;
        if (left_total != right_total) {
            return left_total > right_total;
        }
        return ToLower(left.name) < ToLower(right.name);
    });

    CleanCache();
}

std::wstring AppTracker::GetProcessName(uint32_t pid) {
    if (pid == 0) return L"Idle";
    if (pid == 4) return L"System";

    const auto cached = process_name_cache_.find(pid);
    if (cached != process_name_cache_.end()) {
        return cached->second;
    }

    std::wstring name;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t path[MAX_PATH]{};
        DWORD path_size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &path_size)) {
            const wchar_t* filename = PathFindFileNameW(path);
            name = filename != nullptr && *filename != L'\0' ? filename : path;
            name = RemoveExecutableExtension(std::move(name));
        }
        CloseHandle(process);
    }

    if (name.empty()) {
        name = FindProcessNameInSnapshot(pid);
    }
    if (name.empty()) {
        // Never merge unrelated protected processes into one "Unknown" row.
        name = L"Process " + std::to_wstring(pid);
    }

    process_name_cache_[pid] = name;
    return name;
}

void AppTracker::CleanCache() {
    if (process_name_cache_.size() > 500) {
        process_name_cache_.clear();
        process_name_cache_[0] = L"Idle";
        process_name_cache_[4] = L"System";
    }
}

bool AppTracker::TerminateApp(uint32_t pid) {
    if (pid <= 4) return false;
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == nullptr) {
        return false;
    }

    const BOOL terminated = TerminateProcess(process, 1);
    CloseHandle(process);
    return terminated != FALSE;
}
