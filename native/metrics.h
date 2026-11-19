#pragma once
#include "telemetry_math.h"
#include "temperature_monitor.h"
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <string>
#include <vector>
#include <mutex>

struct CpuMetrics {
    bool sample_available = false;
    uint64_t sampled_at_ms = 0;
    float percent_used = 0.0f;
};

struct RamMetrics {
    bool sample_available = false;
    uint64_t sampled_at_ms = 0;
    float percent_used = 0.0f;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
};

struct DiskMetrics {
    float percent_used = 0.0f;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
};

struct NetworkMetrics {
    double download_speed_bps = 0.0; // bits per second
    double upload_speed_bps = 0.0;   // bits per second
    uint64_t total_received_bytes = 0;
    uint64_t total_sent_bytes = 0;
    uint64_t session_received_bytes = 0;
    uint64_t session_sent_bytes = 0;
    std::wstring adapter_name;
    uint32_t adapter_index = 0;
    std::wstring status = L"NORMAL"; // NORMAL, HEAVY
    bool connected = false;

    // Real adapter info
    std::wstring adapter_type = L"Ethernet";
    std::wstring wifi_ssid = L"";
    uint64_t link_speed_bps = 0;
    uint64_t receive_link_speed_bps = 0;
    uint32_t wifi_signal_percent = 0;
    std::wstring local_ip = L"127.0.0.1";
    std::wstring subnet_mask = L"N/A";
    std::wstring ipv6_address = L"N/A";
    std::wstring mac_address = L"N/A";
    bool dhcp_enabled = false;
};

struct GpuMetrics {
    float percent_used = 0.0f;
    std::wstring label = L"N/A";
    std::wstring active_engine = L"Unavailable";
    bool available = false;
};

struct SystemInfoMetrics {
    std::wstring os_name = L"Windows";
    std::wstring architecture = L"N/A";
    std::wstring hostname = L"N/A";
    std::wstring cpu_name = L"N/A";
    float cpu_speed_ghz = 0.0f;
};

struct InterfaceInfo {
    uint32_t index = 0;
    // A stable Windows interface GUID. Interface indices can change after a
    // reboot, so this is the value persisted for a manual adapter lock.
    std::wstring id;
    std::wstring name;
    std::wstring description;
    std::wstring type;
    bool is_active = false;
    bool is_connected = false;
    uint64_t speed_bps = 0;
};

class MetricsManager {
public:
    MetricsManager();
    ~MetricsManager();

    // Call once per second to update stats
    void Update();

    // Call when Network tab is opened or periodically to refresh location-triggering Wi-Fi details
    void RefreshNetworkDetails();

    void ResetSession();

    CpuMetrics GetCpu() const { return cpu_; }
    RamMetrics GetRam() const { return ram_; }
    DiskMetrics GetDisk() const { return disk_; }
    NetworkMetrics GetNetwork() const { return network_; }
    GpuMetrics GetGpu() const { return gpu_; }
    SystemInfoMetrics GetSystemInfo() const { return system_info_; }
    uint32_t GetCpuCoreCount() const { return cpu_core_count_; }
    uint64_t GetUptimeSeconds() const { return uptime_seconds_; }

    std::vector<InterfaceInfo> GetInterfaces() const;

    // An empty id means Auto: follow Windows' default route. A non-empty id
    // locks monitoring to that exact Windows adapter.
    void SetMonitoredAdapterId(const std::wstring& adapter_id);
    std::wstring GetMonitoredAdapterId() const;

    WatchmanSensors::Snapshot GetTemperatures() const { return temperature_monitor_.GetSnapshot(); }

private:
    CpuMetrics cpu_;
    RamMetrics ram_;
    DiskMetrics disk_;
    NetworkMetrics network_;
    GpuMetrics gpu_;
    SystemInfoMetrics system_info_;

    uint32_t cpu_core_count_ = 0;
    uint64_t uptime_seconds_ = 0;
    
    TemperatureMonitor temperature_monitor_;

    // Each GPU engine is a separate PDH instance. Keep its expanded path so
    // utilization can be grouped the same way as the Tauri implementation.
    struct GpuPdhCounter {
        void* handle = nullptr;
        std::wstring path;
    };

    // PDH state for GPU monitoring
    void* pdh_query_ = nullptr;
    std::vector<GpuPdhCounter> pdh_gpu_counters_;
    bool pdh_gpu_initialized_ = false;
    bool pdh_gpu_primed_ = false;
    ULONGLONG last_gpu_counter_refresh_ms_ = 0;

    // Whole-machine busy time, without boost/frequency scaling. Native times
    // remain warm for a seamless fallback on single-processor-group systems.
    void* pdh_cpu_query_ = nullptr;
    void* pdh_cpu_counter_ = nullptr;
    bool pdh_cpu_initialized_ = false;
    bool pdh_cpu_primed_ = false;
    ULONGLONG last_cpu_pdh_retry_ms_ = 0;
    Telemetry::CpuSampleCadence cpu_sample_cadence_;
    Telemetry::CpuTimeSampler cpu_time_sampler_;

    // Network Tracking state
    uint64_t last_rx_bytes_ = 0;
    uint64_t last_tx_bytes_ = 0;
    uint64_t session_start_rx_ = 0;
    uint64_t session_start_tx_ = 0;
    ULONGLONG last_network_time_ms_ = 0;
    uint32_t active_interface_index_ = 0;
    uint32_t adapter_details_interface_index_ = 0;
    bool network_initialized_ = false;
    ULONGLONG last_adapter_details_refresh_ms_ = 0;
    mutable std::mutex monitored_adapter_mutex_;
    std::wstring monitored_adapter_id_;

    void UpdateCpu();
    void UpdateRam();
    void UpdateDisk();
    void UpdateNetwork();
    void UpdateGpu();
    void UpdateUptime();
    void InitializeSystemInfo();
    void UpdateSystemInfo();
    void RefreshAdapterDetails(uint32_t interface_index);
    
    void InitCpuPdh();
    void InitGpuPdh();
    void RefreshGpuPdhCounters();
    uint32_t FindBestInterfaceIndex();
    uint32_t FindInterfaceIndexById(const std::wstring& adapter_id) const;
};
