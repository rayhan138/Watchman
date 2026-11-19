#include "engine.h"
#include "app_traffic_presentation.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>

namespace {

uint64_t AddTrafficBytes(uint64_t left, uint64_t right) {
    const uint64_t limit = (std::numeric_limits<uint64_t>::max)();
    return limit - left < right ? limit : left + right;
}

std::wstring HistorySaveError(const std::string& error) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), nullptr, 0);
    if (length <= 0) return L"Traffic history could not be saved; pending data will be retried.";
    std::wstring message(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, error.data(), static_cast<int>(error.size()), message.data(), length);
    return message;
}

} // namespace

WatchmanEngine::WatchmanEngine() {
    metrics_ = std::make_unique<MetricsManager>();
    app_tracker_ = std::make_unique<AppTracker>();
    storage_ = std::make_unique<TrafficStorage>();
    ping_tester_ = std::make_unique<PingTester>();
    speed_tester_ = std::make_unique<SpeedTester>();
    diagnostics_runner_ = std::make_unique<DiagnosticsRunner>();
}

WatchmanEngine::~WatchmanEngine() {
    Stop();
}

void WatchmanEngine::Start() {
    if (is_running_) return;
    is_running_ = true;
    engine_thread_ = std::thread([this]() {
        EngineLoop();
    });
}

void WatchmanEngine::Stop() {
    is_running_ = false;
    if (engine_thread_.joinable()) {
        engine_thread_.join();
    }
    FlushPendingTraffic();
}

void WatchmanEngine::EngineLoop() {
    constexpr uint32_t kPersistenceIntervalTicks = 5;

    auto next_tick = std::chrono::steady_clock::now();
    while (is_running_) {
        // 1. Update Core Metrics
        metrics_->Update();
        // Attribute this sample to one date, not the later persistence time.
        const std::wstring sample_date = TrafficStorage::GetCurrentDateString();
        const uint64_t sample_time = static_cast<uint64_t>(time(nullptr));
        if (sample_date != app_totals_date_) RefreshAppTotals(sample_date);

        // Wi-Fi details are opt-in from the Network tab. Keep this work on
        // the engine thread so the renderer never races the metrics service.
        if (network_health_active_.load() &&
            network_details_refresh_requested_.exchange(false)) {
            metrics_->RefreshNetworkDetails();
        }

        // 2. Refresh Ping only while the Network tab is visible. ICMP does
        // not need to keep the machine active when the health screen is away.
        std::wstring target_ip = metrics_->GetNetwork().local_ip;
        if (!network_health_active_.load() ||
            target_ip == L"127.0.0.1" || target_ip.empty()) {
            ping_tester_->Stop();
        } else {
            ping_tester_->SetActiveMode(true);
            ping_tester_->Start();
        }

        // 3. Track App Traffic
        const NetworkMetrics network_metrics = metrics_->GetNetwork();
        double dl_speed = network_metrics.download_speed_bps;
        double ul_speed = network_metrics.upload_speed_bps;
        app_tracker_->Refresh(dl_speed, ul_speed);

        // 4. Accumulate app traffic. Persisting every sample is needlessly
        // expensive for a taskbar monitor, but current app speeds stay live.
        auto active_apps = app_tracker_->GetActiveApps();
        for (const auto& app : active_apps) {
            if (app.interval_download_bytes > 0 || app.interval_upload_bytes > 0) {
                pending_traffic_.AddApp(sample_date, AppTrafficRecord{
                    app.name, app.interval_download_bytes, app.interval_upload_bytes, sample_time});
            }
        }

        // 5. Update Session Totals
        const uint64_t current_rx = network_metrics.total_received_bytes;
        const uint64_t current_tx = network_metrics.total_sent_bytes;
        const uint32_t current_adapter = network_metrics.adapter_index;
        const bool has_active_counter = network_metrics.connected && current_adapter != 0;
        const bool adapter_changed = current_adapter != last_network_adapter_index_;

        // Interface byte counters are not globally monotonic. Preserve the
        // session across a default-route switch, but do not combine counters
        // from Wi-Fi, Ethernet, or a VPN into a false traffic spike.
        if (has_active_counter && last_network_counter_active_ && !adapter_changed) {
            if (current_rx >= last_total_rx_) {
                const uint64_t delta_rx = current_rx - last_total_rx_;
                session_dl_bytes_ += delta_rx;
                pending_traffic_.AddDaily(sample_date, delta_rx, 0);
            }
            if (current_tx >= last_total_tx_) {
                const uint64_t delta_tx = current_tx - last_total_tx_;
                session_ul_bytes_ += delta_tx;
                pending_traffic_.AddDaily(sample_date, 0, delta_tx);
            }
        }

        last_total_rx_ = has_active_counter ? current_rx : 0;
        last_total_tx_ = has_active_counter ? current_tx : 0;
        last_network_adapter_index_ = current_adapter;
        last_network_counter_active_ = has_active_counter;

        // Reset only the displayed session. Persistence baselines and pending
        // history must include all measured bytes, including this sample.
        if (reset_session_requested_.exchange(false)) {
            session_dl_bytes_ = 0;
            session_ul_bytes_ = 0;
        }

        // 6. Batch SQLite work. Metrics and taskbar values still update every
        // second; history and stored totals refresh every five seconds.
        const bool refresh_persistent_data =
            (persistence_tick_++ % kPersistenceIntervalTicks) == 0;
        if (refresh_persistent_data) {
            FlushPendingTraffic();
            RefreshAppTotals(sample_date);
        }

        // 7. Update Snapshots
        FetchSnapshots(refresh_persistent_data);

        // 8. Target one-second sample windows, rather than one second PLUS
        // work duration. Never run catch-up bursts after a slow API or resume.
        next_tick += std::chrono::seconds(1);
        const auto now = std::chrono::steady_clock::now();
        if (next_tick < now) next_tick = now + std::chrono::seconds(1);
        std::this_thread::sleep_until(next_tick);
    }
}

void WatchmanEngine::FlushPendingTraffic() {
    const auto before = pending_traffic_.GetAppTotals(app_totals_date_);
    const bool success = pending_traffic_.Flush(*storage_);
    const std::string error = success ? std::string{} : storage_->GetLastError();
    const auto after = pending_traffic_.GetAppTotals(app_totals_date_);
    // Transfer acknowledged bytes into the cache even if the subsequent bulk
    // read fails, or an earlier date committed before a later date failed.
    for (const auto& [name, bytes] : before) {
        const auto remaining = after.find(name);
        const uint64_t committed = bytes - (remaining == after.end() ? 0 : remaining->second);
        auto& stored = stored_app_totals_[name];
        stored = AddTrafficBytes(stored, committed);
    }
    if (!success) {
        OutputDebugStringA(("Watchman history flush failed; pending traffic retained: " + error + "\n").c_str());
    }
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    dashboard_snapshot_.history_save_pending = !pending_traffic_.Empty();
    if (success) dashboard_snapshot_.history_save_error.clear();
    else dashboard_snapshot_.history_save_error = HistorySaveError(error);
}

void WatchmanEngine::RefreshAppTotals(const std::wstring& date) {
    if (app_totals_date_ != date) {
        app_totals_date_ = date;
        stored_app_totals_.clear();
    }
    std::vector<AppTrafficRecord> records;
    if (!storage_->LoadAppTrafficForDate(date, records)) return;
    std::map<std::wstring, uint64_t> totals;
    for (const auto& app : records) {
        totals[app.app_name] = AddTrafficBytes(app.rx_bytes, app.tx_bytes);
    }
    stored_app_totals_.swap(totals);
}

void WatchmanEngine::FetchSnapshots(bool refresh_persistent_data) {
    std::vector<TrafficRecord> history;
    std::vector<TrafficRecord> monthly_history;
    std::vector<int> available_years;
    if (refresh_persistent_data) {
        history = storage_->LoadHistory();
        monthly_history = storage_->LoadMonthlyHistory();
        available_years = storage_->LoadAvailableHistoryYears();
    }

    const auto pending_app_totals = pending_traffic_.GetAppTotals(app_totals_date_);
    std::lock_guard<std::mutex> lock(snapshot_mutex_);

    // Dashboard
    ++dashboard_snapshot_.sample_sequence;
    dashboard_snapshot_.history_save_pending = !pending_traffic_.Empty();
    const auto cpu = metrics_->GetCpu();
    dashboard_snapshot_.cpu_sample_available = cpu.sample_available;
    dashboard_snapshot_.cpu_sampled_at_ms = cpu.sampled_at_ms;
    dashboard_snapshot_.cpu_usage_percent = cpu.percent_used;
    dashboard_snapshot_.temperatures = metrics_->GetTemperatures();
    dashboard_snapshot_.cpu_core_count = metrics_->GetCpuCoreCount();

    const auto ram = metrics_->GetRam();
    dashboard_snapshot_.ram_sample_available = ram.sample_available;
    dashboard_snapshot_.ram_sampled_at_ms = ram.sampled_at_ms;
    dashboard_snapshot_.ram_usage_percent = ram.percent_used;
    dashboard_snapshot_.ram_used_bytes = ram.used_bytes;
    dashboard_snapshot_.ram_total_bytes = ram.total_bytes;

    dashboard_snapshot_.disk_usage_percent = metrics_->GetDisk().percent_used;

    dashboard_snapshot_.gpu_available = metrics_->GetGpu().available;
    dashboard_snapshot_.gpu_usage_percent = metrics_->GetGpu().percent_used;
    dashboard_snapshot_.gpu_name = metrics_->GetGpu().label;
    dashboard_snapshot_.gpu_active_engine = metrics_->GetGpu().active_engine;

    dashboard_snapshot_.download_speed_bps = metrics_->GetNetwork().download_speed_bps;
    dashboard_snapshot_.upload_speed_bps = metrics_->GetNetwork().upload_speed_bps;

    dashboard_snapshot_.session_download_bytes = session_dl_bytes_;
    dashboard_snapshot_.session_upload_bytes = session_ul_bytes_;

    if (refresh_persistent_data) {
        dashboard_snapshot_.today_traffic_date = app_totals_date_;
        dashboard_snapshot_.today_download_bytes = 0;
        dashboard_snapshot_.today_upload_bytes = 0;
        if (!history.empty()) {
            for (const auto& rec : history) {
                if (rec.date == app_totals_date_) {
                    dashboard_snapshot_.today_download_bytes = rec.rx_bytes;
                    dashboard_snapshot_.today_upload_bytes = rec.tx_bytes;
                    break;
                }
            }
        }
    }

    dashboard_snapshot_.uptime_seconds = metrics_->GetUptimeSeconds();
    const SystemInfoMetrics system_info = metrics_->GetSystemInfo();
    dashboard_snapshot_.os_name = system_info.os_name;
    dashboard_snapshot_.architecture = system_info.architecture;
    dashboard_snapshot_.hostname = system_info.hostname;
    dashboard_snapshot_.cpu_name = system_info.cpu_name;
    dashboard_snapshot_.cpu_speed_ghz = system_info.cpu_speed_ghz;

    // Network
    network_snapshot_.is_connected = metrics_->GetNetwork().connected;
    network_snapshot_.active_adapter_name = metrics_->GetNetwork().adapter_name;
    network_snapshot_.active_adapter_type = metrics_->GetNetwork().adapter_type;
    network_snapshot_.wifi_ssid = metrics_->GetNetwork().wifi_ssid;
    network_snapshot_.local_ip = metrics_->GetNetwork().local_ip;
    network_snapshot_.subnet_mask = metrics_->GetNetwork().subnet_mask;
    network_snapshot_.ipv6_address = metrics_->GetNetwork().ipv6_address;
    network_snapshot_.mac_address = metrics_->GetNetwork().mac_address;
    network_snapshot_.dhcp_enabled = metrics_->GetNetwork().dhcp_enabled;
    network_snapshot_.link_speed_bps = metrics_->GetNetwork().link_speed_bps;
    network_snapshot_.receive_link_speed_bps = metrics_->GetNetwork().receive_link_speed_bps;
    network_snapshot_.wifi_signal_percent = metrics_->GetNetwork().wifi_signal_percent;
    network_snapshot_.latency_ms = ping_tester_->GetLatencyMs();
    network_snapshot_.jitter_ms = ping_tester_->GetJitterMs();
    network_snapshot_.packet_loss_percent = ping_tester_->GetPacketLoss();
    network_snapshot_.ping_samples_sent = ping_tester_->GetSamplesSent();
    network_snapshot_.ping_samples_received = ping_tester_->GetSamplesReceived();
    network_snapshot_.min_latency_ms = ping_tester_->GetMinLatencyMs();
    network_snapshot_.max_latency_ms = ping_tester_->GetMaxLatencyMs();
    network_snapshot_.quality_score_percent = ping_tester_->GetQualityScorePercent();

    // Apps
    app_activity_snapshot_.is_admin = app_tracker_->IsAdmin();
    app_activity_snapshot_.bandwidth_available = app_tracker_->IsBandwidthAvailable();
    app_activity_snapshot_.requires_admin = app_tracker_->RequiresAdmin();
    app_activity_snapshot_.status_message = app_tracker_->GetStatusMessage();
    app_activity_snapshot_.active_apps.clear();
    for (const auto& app : app_tracker_->GetActiveApps()) {
        AppActivityItem item;
        item.name = app.name;
        item.pid = app.pid;
        item.current_download_bps = static_cast<uint64_t>(Telemetry::KibibytesToBitsPerSecond(app.download_speed_kib_per_sec));
        item.current_upload_bps = static_cast<uint64_t>(Telemetry::KibibytesToBitsPerSecond(app.upload_speed_kib_per_sec));
        const auto stored = stored_app_totals_.find(app.name);
        const auto pending = pending_app_totals.find(app.name);
        item.total_bytes_today = AddTrafficBytes(
            stored == stored_app_totals_.end() ? 0 : stored->second,
            pending == pending_app_totals.end() ? 0 : pending->second);
        item.total_download_bytes = app.total_download_bytes;
        item.total_upload_bytes = app.total_upload_bytes;
        item.active_connections = app.connection_count;
        item.is_system = app.is_system;
        item.is_running = app.is_running;
        item.last_active_seconds = app.last_active_seconds;
        item.status = app.status;
        app_activity_snapshot_.active_apps.push_back(item);
    }
    // Pre-sort active apps once per telemetry tick so UI frames do not re-sort on 60 FPS paints.
    std::sort(app_activity_snapshot_.active_apps.begin(),
              app_activity_snapshot_.active_apps.end(),
              AppTrafficPresentation::MostUsageFirst);

    if (refresh_persistent_data) {
        // Traffic History
        traffic_history_snapshot_.history.clear();
        traffic_history_snapshot_.monthly_history.clear();
        uint64_t all_dl = 0;
        uint64_t all_ul = 0;
        for (const auto& rec : history) {
            TrafficHistoryItem item;
            item.date = rec.date;
            item.rx_bytes = rec.rx_bytes;
            item.tx_bytes = rec.tx_bytes;
            traffic_history_snapshot_.history.push_back(item);
            all_dl += rec.rx_bytes;
            all_ul += rec.tx_bytes;
        }

        for (const auto& rec : monthly_history) {
            TrafficHistoryItem item;
            item.date = rec.date;
            item.rx_bytes = rec.rx_bytes;
            item.tx_bytes = rec.tx_bytes;
            traffic_history_snapshot_.monthly_history.push_back(item);
        }

        traffic_history_snapshot_.available_years = available_years;
        traffic_history_snapshot_.all_time_download_bytes = all_dl;
        traffic_history_snapshot_.all_time_upload_bytes = all_ul;
    }
}

DashboardSnapshot WatchmanEngine::GetDashboardSnapshot() {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return dashboard_snapshot_;
}

NetworkSnapshot WatchmanEngine::GetNetworkSnapshot() {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return network_snapshot_;
}

AppActivitySnapshot WatchmanEngine::GetAppActivitySnapshot() {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return app_activity_snapshot_;
}

TrafficHistorySnapshot WatchmanEngine::GetTrafficHistorySnapshot() {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return traffic_history_snapshot_;
}

void WatchmanEngine::RunSpeedTest() {
    speed_tester_->StartTest();
}

void WatchmanEngine::CancelSpeedTest() {
    speed_tester_->CancelTest();
}

SpeedTestResult WatchmanEngine::GetSpeedTestResult() {
    return speed_tester_->GetResult();
}

int WatchmanEngine::GetSpeedTestProgress() {
    return speed_tester_->GetProgress();
}

double WatchmanEngine::GetSpeedTestLiveSpeed() {
    return speed_tester_->GetLiveSpeed();
}

std::wstring WatchmanEngine::GetSpeedTestStatus() {
    return speed_tester_->GetStatusString();
}

bool WatchmanEngine::IsSpeedTestRunning() {
    return speed_tester_->IsRunning();
}

void WatchmanEngine::RefreshNetworkDetails() {
    network_details_refresh_requested_ = true;
}

void WatchmanEngine::SetNetworkHealthActive(bool active) {
    network_health_active_ = active;
}

void WatchmanEngine::SetNetworkAdapterId(const std::wstring& adapter_id) {
    metrics_->SetMonitoredAdapterId(adapter_id);
}

std::wstring WatchmanEngine::GetNetworkAdapterId() const {
    return metrics_->GetMonitoredAdapterId();
}

std::vector<InterfaceInfo> WatchmanEngine::GetNetworkAdapters() const {
    return metrics_->GetInterfaces();
}

bool WatchmanEngine::TerminateApp(uint32_t pid) {
    return app_tracker_->TerminateApp(pid);
}

bool WatchmanEngine::RunDiagnostics() {
    return diagnostics_runner_ && diagnostics_runner_->Start();
}

DiagnosticsSnapshot WatchmanEngine::GetDiagnosticsSnapshot() const {
    return diagnostics_runner_ ? diagnostics_runner_->GetSnapshot() : DiagnosticsSnapshot{};
}

TrafficExportResult WatchmanEngine::ExportTrafficCsv(const TrafficExportOptions& options) {
    return storage_ ? storage_->ExportTrafficCsv(options) : TrafficExportResult{
        false,
        {},
        L"Saved traffic history is unavailable."
    };
}

void WatchmanEngine::ResetSession() {
    reset_session_requested_ = true;
}
