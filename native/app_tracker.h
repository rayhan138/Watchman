#pragma once
#include "telemetry_math.h"
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>

struct AppNetworkInfo {
    uint32_t pid = 0;
    std::wstring name;
    uint32_t connection_count = 0;
    // Network transfer rates are KiB/s (kibibytes per second).
    double download_speed_kib_per_sec = 0.0;
    double upload_speed_kib_per_sec = 0.0;
    double activity_score = 0.0;

    // Backward-compatibility properties for legacy field names
    double download_speed_kbps = 0.0;
    double upload_speed_kbps = 0.0;
    double activity_score_kbps = 0.0;

    uint64_t total_bytes = 0;
    uint64_t total_download_bytes = 0;
    uint64_t total_upload_bytes = 0;
    uint64_t interval_download_bytes = 0;
    uint64_t interval_upload_bytes = 0;
    uint64_t last_active_seconds = 0;
    bool is_system = false;
    bool is_running = false;
    std::wstring status = L"recent";

    // Group processes by app name without dropping the canonical KiB/s fields
    // consumed by WatchmanEngine. Legacy aliases must mirror those values,
    // never become a second, independently populated source of truth.
    void AddTrafficCounters(const AppNetworkInfo& other) {
        download_speed_kib_per_sec += other.download_speed_kib_per_sec;
        upload_speed_kib_per_sec += other.upload_speed_kib_per_sec;
        activity_score += other.activity_score;
        download_speed_kbps = download_speed_kib_per_sec;
        upload_speed_kbps = upload_speed_kib_per_sec;
        activity_score_kbps = activity_score;
        total_download_bytes += other.total_download_bytes;
        total_upload_bytes += other.total_upload_bytes;
        interval_download_bytes += other.interval_download_bytes;
        interval_upload_bytes += other.interval_upload_bytes;
        total_bytes = total_download_bytes + total_upload_bytes;
    }
};


enum class AppTraceSessionState {
    Started,
    Attached,
    RequiresAdmin,
    Unavailable
};

struct EtwTrafficDelta {
    uint64_t download_bytes = 0;
    uint64_t upload_bytes = 0;
};

class AppTracker {
public:
    AppTracker();
    ~AppTracker();

    // Refresh the list of active network apps
    void Refresh(double total_system_download_bps, double total_system_upload_bps);

    std::vector<AppNetworkInfo>& GetActiveAppsMutable() { return active_apps_; }
    std::vector<AppNetworkInfo> GetActiveApps() const { return active_apps_; }

    // Check the process token. Per-app bandwidth is available only after the
    // dedicated ETW session has started or been attached.
    bool IsAdmin() const { return is_admin_; }
    bool IsBandwidthAvailable() const;
    bool RequiresAdmin() const;
    std::wstring GetStatusMessage() const;

    // Terminate a process by PID (Watchman admin feature)
    bool TerminateApp(uint32_t pid);

private:
    std::vector<AppNetworkInfo> active_apps_;
    std::unordered_map<uint32_t, AppNetworkInfo> tracked_apps_;
    std::unordered_map<uint32_t, uint64_t> last_active_tick_;
    std::unordered_map<uint32_t, std::wstring> process_name_cache_;

    std::unordered_map<uint32_t, EtwTrafficDelta> etw_pending_;
    std::mutex etw_pending_mutex_;
    Telemetry::SampleInterval etw_sample_interval_;
    std::mutex etw_trace_mutex_;
    std::atomic<bool> etw_stop_{ false };
    std::atomic<bool> etw_consumer_connected_{ false };
    std::atomic<ULONG> etw_consumer_last_error_{ ERROR_SUCCESS };
    std::thread etw_thread_;
    TRACEHANDLE etw_session_handle_ = 0;
    TRACEHANDLE etw_consumer_handle_ = INVALID_PROCESSTRACE_HANDLE;
    AppTraceSessionState trace_session_state_ = AppTraceSessionState::Unavailable;
    bool owns_trace_session_ = false;

    bool is_admin_ = false;

    void StartEtwMonitoring();
    void StopEtwMonitoring();
    AppTraceSessionState EnsureTraceSession();
    ULONG EnableKernelNetworkProvider(TRACEHANDLE session_handle);
    void ConsumeEtwEvents();
    void CloseEtwConsumerTrace();
    static VOID WINAPI EtwEventRecordCallback(PEVENT_RECORD record);
    void AccumulateEtwTraffic(uint32_t pid, bool upload, uint64_t bytes);

    std::wstring GetProcessName(uint32_t pid);
    void CleanCache();
};
