#pragma once

#include "snapshots.h"
#include "metrics.h"
#include "app_tracker.h"
#include "diagnostics.h"
#include "storage.h"
#include "ping_tester.h"
#include "speed_tester.h"

#include <thread>
#include <atomic>
#include <map>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <functional>

class WatchmanEngine {
public:
    // The constructor is deliberately cheap. Services are constructed on the
    // engine worker, not the window thread. The optional dependency hook is
    // used by in-process tests to stall/fail initialization; it is never fed
    // by command-line flags, environment variables or saved preferences.
    enum class InitializationStage { BeforeServices, MetricsReady, AppTrackerReady, StorageReady, ToolsReady };
    using InitializationHook = std::function<void(InitializationStage, const std::atomic<bool>&)>;
    explicit WatchmanEngine(InitializationHook before_initialize = {});
    ~WatchmanEngine();

    // Starts the background 1-second interval loop
    void Start();
    
    // Stops the background loop gracefully
    void Stop();
    bool IsReady() const { return services_ready_.load(std::memory_order_acquire); }
    std::wstring GetStartupError() const;

    // Snapshot getters for UI
    DashboardSnapshot GetDashboardSnapshot();
    NetworkSnapshot GetNetworkSnapshot();
    AppActivitySnapshot GetAppActivitySnapshot();
    TrafficHistorySnapshot GetTrafficHistorySnapshot();

    // Commands
    void RunSpeedTest();
    void CancelSpeedTest();
    SpeedTestSnapshot GetSpeedTestSnapshot() const;
    SpeedTestResult GetSpeedTestResult();
    int GetSpeedTestProgress();
    double GetSpeedTestLiveSpeed();
    std::wstring GetSpeedTestStatus();
    bool IsSpeedTestRunning();

    void RefreshNetworkDetails();
    void SetNetworkHealthActive(bool active);
    void SetNetworkAdapterId(const std::wstring& adapter_id);
    std::wstring GetNetworkAdapterId() const;
    std::vector<InterfaceInfo> GetNetworkAdapters() const;

    // Tools services
    bool RunDiagnostics();
    DiagnosticsSnapshot GetDiagnosticsSnapshot() const;
    TrafficExportResult ExportTrafficCsv(const TrafficExportOptions& options);

    bool TerminateApp(uint32_t pid);
    void ResetSession();

private:
    bool InitializeServices();
    void WorkerMain();
    void RecordStartupFailure(const char* message, uint32_t error = ERROR_GEN_FAILURE);
    void EngineLoop();
    void FetchSnapshots(bool refresh_persistent_data);
    void FlushPendingTraffic();
    void RefreshAppTotals(const std::wstring& date);

    std::atomic<bool> is_running_{false};
    std::atomic<bool> services_ready_{false};
    InitializationHook before_initialize_;
    mutable std::mutex startup_mutex_;
    std::wstring startup_error_;
    std::wstring requested_adapter_id_;
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> network_health_active_{false};
    std::atomic<bool> network_details_refresh_requested_{false};
    std::atomic<bool> reset_session_requested_{false};
    std::thread engine_thread_;
    std::mutex snapshot_mutex_;

    // Backend Services
    std::unique_ptr<MetricsManager> metrics_;
    std::unique_ptr<AppTracker> app_tracker_;
    std::unique_ptr<TrafficStorage> storage_;
    std::unique_ptr<PingTester> ping_tester_;
    std::unique_ptr<SpeedTester> speed_tester_;
    std::unique_ptr<DiagnosticsRunner> diagnostics_runner_;

    // Current Snapshots
    DashboardSnapshot dashboard_snapshot_;
    NetworkSnapshot network_snapshot_;
    AppActivitySnapshot app_activity_snapshot_;
    TrafficHistorySnapshot traffic_history_snapshot_;
    
    // Session totals accumulated in engine
    uint64_t session_dl_bytes_ = 0;
    uint64_t session_ul_bytes_ = 0;

    uint64_t last_total_rx_ = 0;
    uint64_t last_total_tx_ = 0;
    uint32_t last_network_adapter_index_ = 0;
    bool last_network_counter_active_ = false;
    PendingTrafficHistory pending_traffic_;
    std::wstring app_totals_date_;
    std::map<std::wstring, uint64_t> stored_app_totals_;
    uint32_t persistence_tick_ = 0;
};
