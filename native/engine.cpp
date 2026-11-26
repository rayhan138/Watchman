#include "engine.h"
#include "app_traffic_presentation.h"
#include "startup_diagnostics.h"
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <utility>

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

WatchmanEngine::WatchmanEngine(InitializationHook before_initialize)
    : before_initialize_(std::move(before_initialize)) {}

WatchmanEngine::~WatchmanEngine() {
    Stop();
}

bool WatchmanEngine::InitializeServices() {
    const auto keep_starting = [this](InitializationStage stage) {
        if (before_initialize_) before_initialize_(stage, is_running_);
        return is_running_.load();
    };
    // Stop may be requested during any slow constructor. Never publish a
    // half-initialized set of services, and do not start more work after Stop.
    if (!keep_starting(InitializationStage::BeforeServices)) return false;
    metrics_ = std::make_unique<MetricsManager>();
    if (!keep_starting(InitializationStage::MetricsReady)) return false;
    app_tracker_ = std::make_unique<AppTracker>();
    if (!keep_starting(InitializationStage::AppTrackerReady)) return false;
    storage_ = std::make_unique<TrafficStorage>();
    WatchmanStartupDiagnostics::Record("history", storage_->GetProfileKind() == WatchmanHistory::Profile::Native
        ? "native-profile" : "legacy-profile");
    if (!keep_starting(InitializationStage::StorageReady)) return false;
    ping_tester_ = std::make_unique<PingTester>();
    speed_tester_ = std::make_unique<SpeedTester>();
    diagnostics_runner_ = std::make_unique<DiagnosticsRunner>();
    if (!keep_starting(InitializationStage::ToolsReady)) return false;
    {
        // Serialize pre-ready adapter choices with publication so an option
        // changed during initialization cannot be silently lost.
        std::lock_guard<std::mutex> lock(startup_mutex_);
        if (!is_running_) return false;
        metrics_->SetMonitoredAdapterId(requested_adapter_id_);
        services_ready_.store(true, std::memory_order_release);
    }
    WatchmanStartupDiagnostics::Record("backend", "ready");
    return true;
}

void WatchmanEngine::RecordStartupFailure(const char* message, uint32_t error) {
    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        startup_error_ = L"Monitoring could not start or stopped unexpectedly. Restart Watchman and check startup-recovery.log.";
    }
    OutputDebugStringA(message);
    WatchmanStartupDiagnostics::Record("backend", "failed", error);
    is_running_ = false;
}

std::wstring WatchmanEngine::GetStartupError() const {
    std::lock_guard<std::mutex> lock(startup_mutex_);
    return startup_error_;
}

void WatchmanEngine::Start() {
    if (is_running_) return;
    // Start/Stop are owned by the UI thread. Join before setting running so a
    // finishing worker cannot clear the new worker's cancellation flag.
    if (engine_thread_.joinable()) engine_thread_.join();
    is_running_ = true;
    { std::lock_guard<std::mutex> lock(startup_mutex_); startup_error_.clear(); }
    try {
        engine_thread_ = std::thread([this] { WorkerMain(); });
    } catch (const std::exception& error) {
        RecordStartupFailure(error.what());
    }
}

void WatchmanEngine::WorkerMain() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope {
        HRESULT result;
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } scope{com};
    try {
        if (FAILED(com)) {
            RecordStartupFailure("Watchman engine COM initialization failed.", static_cast<uint32_t>(com));
        } else {
            WatchmanStartupDiagnostics::Record("backend", "starting");
            const bool ready = IsReady() || InitializeServices();
            if (ready && is_running_) EngineLoop();
        }
    } catch (const std::exception& error) {
        RecordStartupFailure(error.what());
    } catch (...) {
        RecordStartupFailure("Watchman engine initialization/acquisition failed.");
    }
    // Flush on the owning worker, after acquisition stops. Stop on an engine
    // that never reached Ready must not dereference unpublished services.
    if (IsReady()) {
        try { FlushPendingTraffic(); }
        catch (...) { RecordStartupFailure("Watchman could not finish its pending history flush."); }
    } else {
        // Nothing was published: release partial services on their creating
        // worker, before its COM apartment ends. A later Stop is a no-op.
        diagnostics_runner_.reset();
        speed_tester_.reset();
        ping_tester_.reset();
        storage_.reset();
        app_tracker_.reset();
        metrics_.reset();
    }
    is_running_ = false;
    WatchmanStartupDiagnostics::Record("backend", "stopped");
}

void WatchmanEngine::Stop() {
    is_running_ = false;
    if (IsReady()) speed_tester_->CancelTest();
    wake_.notify_all();
    if (engine_thread_.joinable()) engine_thread_.join();
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
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wake_.wait_until(lock, next_tick, [this] { return !is_running_.load(); });
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
    DashboardSnapshot dashboard;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        dashboard = dashboard_snapshot_; // Last good today's totals/save status.
    }
    NetworkSnapshot network;
    AppActivitySnapshot apps;
    TrafficHistorySnapshot next_history;
    std::vector<TrafficRecord> history;
    const bool history_loaded = refresh_persistent_data && storage_->LoadHistory(history);
    if (refresh_persistent_data) {
        dashboard.history_read_error = history_loaded ? L"" : HistorySaveError(storage_->GetLastError());
    }
    dashboard.history_profile_description = storage_->GetProfileDescription();
    const auto pending_app_totals = pending_traffic_.GetAppTotals(app_totals_date_);

    // Dashboard
    ++dashboard.sample_sequence;
    dashboard.history_save_pending = !pending_traffic_.Empty();
    const auto cpu = metrics_->GetCpu();
    dashboard.cpu_sample_available = cpu.sample_available;
    dashboard.cpu_sampled_at_ms = cpu.sampled_at_ms;
    dashboard.cpu_usage_percent = cpu.percent_used;
    dashboard.temperatures = metrics_->GetTemperatures();
    dashboard.cpu_core_count = metrics_->GetCpuCoreCount();

    const auto ram = metrics_->GetRam();
    dashboard.ram_sample_available = ram.sample_available;
    dashboard.ram_sampled_at_ms = ram.sampled_at_ms;
    dashboard.ram_usage_percent = ram.percent_used;
    dashboard.ram_used_bytes = ram.used_bytes;
    dashboard.ram_total_bytes = ram.total_bytes;

    dashboard.disk_usage_percent = metrics_->GetDisk().percent_used;

    const auto gpu = metrics_->GetGpu();
    dashboard.gpu_available = gpu.available;
    dashboard.gpu_usage_percent = gpu.percent_used;
    dashboard.gpu_name = gpu.label;
    dashboard.gpu_active_engine = gpu.active_engine;

    dashboard.download_speed_bps = metrics_->GetNetwork().download_speed_bps;
    dashboard.upload_speed_bps = metrics_->GetNetwork().upload_speed_bps;

    dashboard.session_download_bytes = session_dl_bytes_;
    dashboard.session_upload_bytes = session_ul_bytes_;

    if (history_loaded) {
        dashboard.today_traffic_date = app_totals_date_;
        dashboard.today_download_bytes = 0;
        dashboard.today_upload_bytes = 0;
        if (!history.empty()) {
            for (const auto& rec : history) {
                if (rec.date == app_totals_date_) {
                    dashboard.today_download_bytes = rec.rx_bytes;
                    dashboard.today_upload_bytes = rec.tx_bytes;
                    break;
                }
            }
        }
    }

    dashboard.uptime_seconds = metrics_->GetUptimeSeconds();
    const SystemInfoMetrics system_info = metrics_->GetSystemInfo();
    dashboard.os_name = system_info.os_name;
    dashboard.architecture = system_info.architecture;
    dashboard.hostname = system_info.hostname;
    dashboard.cpu_name = system_info.cpu_name;
    dashboard.cpu_speed_ghz = system_info.cpu_speed_ghz;

    // Network
    network.is_connected = metrics_->GetNetwork().connected;
    network.active_adapter_name = metrics_->GetNetwork().adapter_name;
    network.active_adapter_type = metrics_->GetNetwork().adapter_type;
    network.wifi_ssid = metrics_->GetNetwork().wifi_ssid;
    network.local_ip = metrics_->GetNetwork().local_ip;
    network.subnet_mask = metrics_->GetNetwork().subnet_mask;
    network.ipv6_address = metrics_->GetNetwork().ipv6_address;
    network.mac_address = metrics_->GetNetwork().mac_address;
    network.dhcp_enabled = metrics_->GetNetwork().dhcp_enabled;
    network.link_speed_bps = metrics_->GetNetwork().link_speed_bps;
    network.receive_link_speed_bps = metrics_->GetNetwork().receive_link_speed_bps;
    network.wifi_signal_percent = metrics_->GetNetwork().wifi_signal_percent;
    network.latency_ms = ping_tester_->GetLatencyMs();
    network.jitter_ms = ping_tester_->GetJitterMs();
    network.packet_loss_percent = ping_tester_->GetPacketLoss();
    network.ping_samples_sent = ping_tester_->GetSamplesSent();
    network.ping_samples_received = ping_tester_->GetSamplesReceived();
    network.min_latency_ms = ping_tester_->GetMinLatencyMs();
    network.max_latency_ms = ping_tester_->GetMaxLatencyMs();
    network.quality_score_percent = ping_tester_->GetQualityScorePercent();

    // Apps
    apps.is_admin = app_tracker_->IsAdmin();
    apps.bandwidth_available = app_tracker_->IsBandwidthAvailable();
    apps.requires_admin = app_tracker_->RequiresAdmin();
    apps.status_message = app_tracker_->GetStatusMessage();
    apps.active_apps.clear();
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
        apps.active_apps.push_back(item);
    }
    // Pre-sort active apps once per telemetry tick so UI frames do not re-sort on 60 FPS paints.
    std::sort(apps.active_apps.begin(),
              apps.active_apps.end(),
              AppTrafficPresentation::MostUsageFirst);

    if (history_loaded) {
        // Derive all aggregates from one checked SQLite statement/snapshot.
        // Three independent reads could disagree or disguise a partial read.
        std::map<std::wstring, std::pair<uint64_t, uint64_t>> months;
        std::vector<int> years;
        for (const auto& record : history) {
            next_history.history.push_back({record.date, record.rx_bytes, record.tx_bytes});
            next_history.all_time_download_bytes = AddTrafficBytes(next_history.all_time_download_bytes, record.rx_bytes);
            next_history.all_time_upload_bytes = AddTrafficBytes(next_history.all_time_upload_bytes, record.tx_bytes);
            auto& month = months[record.date.substr(0, 7)];
            month.first = AddTrafficBytes(month.first, record.rx_bytes);
            month.second = AddTrafficBytes(month.second, record.tx_bytes);
            const int year = std::stoi(record.date.substr(0, 4));
            if (years.empty() || years.back() != year) years.push_back(year);
        }
        for (const auto& [month, bytes] : months)
            next_history.monthly_history.push_back({month, bytes.first, bytes.second});
        next_history.available_years.assign(years.rbegin(), years.rend());
    }
    // No SQLite, ETW, sensor reads, allocations/sorting of app rows or history
    // aggregation while the UI is waiting to acquire the publication mutex.
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    dashboard_snapshot_ = std::move(dashboard);
    network_snapshot_ = std::move(network);
    app_activity_snapshot_ = std::move(apps);
    if (history_loaded) traffic_history_snapshot_ = std::move(next_history);
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
    if (IsReady() && is_running_) speed_tester_->StartTest();
}

void WatchmanEngine::CancelSpeedTest() {
    if (IsReady()) speed_tester_->CancelTest();
}

SpeedTestSnapshot WatchmanEngine::GetSpeedTestSnapshot() const {
    if (IsReady()) return speed_tester_->GetSnapshot();
    SpeedTestSnapshot starting;
    starting.available = false;
    starting.status = L"Monitoring is starting...";
    return starting;
}

SpeedTestResult WatchmanEngine::GetSpeedTestResult() {
    return IsReady() ? speed_tester_->GetResult() : SpeedTestResult{};
}

int WatchmanEngine::GetSpeedTestProgress() {
    return IsReady() ? speed_tester_->GetProgress() : 0;
}

double WatchmanEngine::GetSpeedTestLiveSpeed() {
    return IsReady() ? speed_tester_->GetLiveSpeed() : 0.0;
}

std::wstring WatchmanEngine::GetSpeedTestStatus() {
    return IsReady() ? speed_tester_->GetStatusString() : L"Monitoring is starting...";
}

bool WatchmanEngine::IsSpeedTestRunning() {
    return IsReady() && speed_tester_->IsRunning();
}

void WatchmanEngine::RefreshNetworkDetails() {
    network_details_refresh_requested_ = true;
}

void WatchmanEngine::SetNetworkHealthActive(bool active) {
    network_health_active_ = active;
}

void WatchmanEngine::SetNetworkAdapterId(const std::wstring& adapter_id) {
    std::lock_guard<std::mutex> lock(startup_mutex_);
    requested_adapter_id_ = adapter_id;
    if (IsReady()) metrics_->SetMonitoredAdapterId(adapter_id);
}

std::wstring WatchmanEngine::GetNetworkAdapterId() const {
    std::lock_guard<std::mutex> lock(startup_mutex_);
    return requested_adapter_id_;
}

std::vector<InterfaceInfo> WatchmanEngine::GetNetworkAdapters() const {
    return IsReady() ? metrics_->GetInterfaces() : std::vector<InterfaceInfo>{};
}

bool WatchmanEngine::TerminateApp(uint32_t pid) {
    return IsReady() && is_running_ && app_tracker_->TerminateApp(pid);
}

bool WatchmanEngine::RunDiagnostics() {
    return IsReady() && is_running_ && diagnostics_runner_->Start();
}

DiagnosticsSnapshot WatchmanEngine::GetDiagnosticsSnapshot() const {
    return IsReady() ? diagnostics_runner_->GetSnapshot() : DiagnosticsSnapshot{};
}

TrafficExportResult WatchmanEngine::ExportTrafficCsv(const TrafficExportOptions& options) {
    return IsReady() ? storage_->ExportTrafficCsv(options) : TrafficExportResult{
        false,
        {},
        L"Monitoring is still starting; saved history is not ready yet."
    };
}

void WatchmanEngine::ResetSession() {
    reset_session_requested_ = true;
}
