#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "temperature_sensors.h"

// 1. Dashboard Snapshot
struct DashboardSnapshot {
    uint64_t sample_sequence = 0;
    bool cpu_sample_available = false;
    uint64_t cpu_sampled_at_ms = 0;
    float cpu_usage_percent = 0.0f;
    WatchmanSensors::Snapshot temperatures;
    uint32_t cpu_core_count = 0;

    bool ram_sample_available = false;
    uint64_t ram_sampled_at_ms = 0;
    float ram_usage_percent = 0.0f;
    uint64_t ram_used_bytes = 0;
    uint64_t ram_total_bytes = 0;

    float disk_usage_percent = 0.0f;

    bool gpu_available = false;
    float gpu_usage_percent = 0.0f;
    std::wstring gpu_name;
    std::wstring gpu_active_engine;

    double download_speed_bps = 0.0;
    double upload_speed_bps = 0.0;

    uint64_t session_download_bytes = 0;
    uint64_t session_upload_bytes = 0;

    uint64_t today_download_bytes = 0;
    uint64_t today_upload_bytes = 0;
    std::wstring today_traffic_date;

    // Pending includes normal batching; a nonempty error indicates a failed
    // save retained for retry. Only a successful flush clears that error.
    bool history_save_pending = false;
    std::wstring history_save_error;

    uint64_t uptime_seconds = 0;

    // Static operating-system information is collected once by MetricsManager
    // and kept in the dashboard snapshot so presentation stays renderer-only.
    std::wstring os_name;
    std::wstring architecture;
    std::wstring hostname;
    std::wstring cpu_name;
    float cpu_speed_ghz = 0.0f;
};

// 2. Network Snapshot
struct NetworkSnapshot {
    bool is_connected = false;
    std::wstring active_adapter_name;
    std::wstring active_adapter_type; // "Wi-Fi", "Ethernet", "VPN", or "Other"
    std::wstring wifi_ssid;
    std::wstring local_ip;
    std::wstring subnet_mask;
    std::wstring ipv6_address;
    std::wstring mac_address;
    bool dhcp_enabled = false;
    uint64_t link_speed_bps = 0;
    uint64_t receive_link_speed_bps = 0;
    uint32_t wifi_signal_percent = 0;

    double latency_ms = 0.0;
    double jitter_ms = 0.0;
    float packet_loss_percent = 0.0f;
    uint32_t ping_samples_sent = 0;
    uint32_t ping_samples_received = 0;
    float min_latency_ms = 0.0f;
    float max_latency_ms = 0.0f;
    float quality_score_percent = 100.0f;
};

// 3. App Activity Snapshot
struct AppActivityItem {
    std::wstring name;
    uint32_t pid = 0;
    uint64_t current_download_bps = 0;
    uint64_t current_upload_bps = 0;
    // Persisted plus pending bytes for the current local sample date.
    uint64_t total_bytes_today = 0;
    // Download/upload breakdowns remain Watchman-session totals.
    uint64_t total_download_bytes = 0;
    uint64_t total_upload_bytes = 0;
    uint32_t active_connections = 0;
    bool is_system = false;
    bool is_running = false;
    uint64_t last_active_seconds = 0;
    std::wstring status;
};

struct AppActivitySnapshot {
    std::vector<AppActivityItem> active_apps;
    bool is_admin = false;
    bool bandwidth_available = false;
    bool requires_admin = false;
    std::wstring status_message;
};

// 4. Traffic History Snapshot
struct TrafficHistoryItem {
    std::wstring date;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
};

struct TrafficHistorySnapshot {
    std::vector<TrafficHistoryItem> history;
    std::vector<TrafficHistoryItem> monthly_history;
    std::vector<int> available_years;
    uint64_t all_time_download_bytes = 0;
    uint64_t all_time_upload_bytes = 0;
};
