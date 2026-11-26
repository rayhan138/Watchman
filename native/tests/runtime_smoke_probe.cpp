// Opt-in integration probe, NOT a CTest/unit test. Runs the production engine,
// Windows PDH/ETW/network services and SQLite against disposable CI data.
// windows_smoke_test.ps1 owns the timeout and the disposable-runner safety gate.
#include "engine.h"
#include "sqlite3.h"
#include <winhttp.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
int failures = 0;

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::string Quote(const std::string& value) {
    std::string result = "\"";
    for (unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character < 32 ? '?' : static_cast<char>(character); break;
        }
    }
    return result + "\"";
}
void Check(const char* name, bool pass, const std::string& detail = {}) {
    if (!pass) ++failures;
    std::cout << "{\"event\":\"check\",\"name\":" << Quote(name) << ",\"pass\":" << (pass ? "true" : "false")
              << ",\"detail\":" << Quote(detail) << "}" << std::endl;
}
void Note(const char* name, const std::wstring& detail) {
    std::cout << "{\"event\":\"observation\",\"name\":" << Quote(name) << ",\"detail\":" << Quote(Utf8(detail)) << "}" << std::endl;
}
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Database {
    sqlite3* handle = nullptr;
    explicit Database(const std::filesystem::path& path) {
        Require(sqlite3_open_v2(Utf8(path.wstring()).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
            "Could not open the real application's history database read-only");
        sqlite3_busy_timeout(handle, 6000);
    }
    ~Database() { if (handle) sqlite3_close(handle); }
    std::string Text(const char* sql) {
        sqlite3_stmt* statement = nullptr;
        Require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK, "History inspection query failed");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> scope(statement, sqlite3_finalize);
        Require(sqlite3_step(statement) == SQLITE_ROW, "History inspection returned no row");
        const auto* value = sqlite3_column_text(statement, 0);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
    uint64_t Integer(const char* sql) { return std::stoull(Text(sql)); }
};

void InspectDatabase(const std::filesystem::path& path) {
    Database database(path);
    const auto integrity = database.Text("PRAGMA quick_check;");
    const auto rows = database.Integer("SELECT COUNT(*) FROM daily_traffic;");
    const auto rx = database.Integer("SELECT COALESCE(SUM(download_bytes),0) FROM daily_traffic;");
    const auto tx = database.Integer("SELECT COALESCE(SUM(upload_bytes),0) FROM daily_traffic;");
    const auto apps = database.Integer("SELECT COUNT(*) FROM app_traffic;");
    const auto app_bytes = database.Integer("SELECT COALESCE(SUM(download_bytes + upload_bytes),0) FROM app_traffic;");
    std::cout << "{\"event\":\"database\",\"integrity\":" << Quote(integrity) << ",\"rows\":" << rows
              << ",\"download_bytes\":" << rx << ",\"upload_bytes\":" << tx << ",\"app_rows\":" << apps
              << ",\"app_bytes\":" << app_bytes << "}" << std::endl;
    Check("SQLite integrity", integrity == "ok");
}

struct InternetCloser { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using Internet = std::unique_ptr<void, InternetCloser>;
struct Traffic {
    std::atomic<uint64_t> bytes{0};
    std::atomic<DWORD> error{0};
    std::atomic<bool> done{false};
    std::thread worker;
    ~Traffic() { if (worker.joinable()) worker.join(); }
    void Start() {
        worker = std::thread([this] {
            // Two bounded downloads from a fixed public archive; no credentials,
            // speed-test payloads, synthetic adapter counters or mocked ETW events.
            Internet session(WinHttpOpen(L"Watchman runtime smoke", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (session) WinHttpSetTimeouts(session.get(), 4000, 4000, 4000, 4000);
            Internet connection(session ? WinHttpConnect(session.get(), L"codeload.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr);
            // A fixed public fixture: the Watchman checkout may be private,
            // so its unauthenticated codeload URL can legitimately return 404.
            const std::wstring resource = L"/nlohmann/json/zip/refs/tags/v3.11.3";
            for (int iteration = 0; connection && iteration < 2; ++iteration) {
                Internet request(WinHttpOpenRequest(connection.get(), L"GET", resource.c_str(), nullptr,
                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
                if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
                    error = GetLastError(); break;
                }
                DWORD status = 0, size = sizeof(status);
                if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
                    error = status ? status : GetLastError(); break;
                }
                char buffer[16384];
                const auto deadline = GetTickCount64() + 15000;
                for (;;) {
                    DWORD received = 0;
                    if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &received)) { error = GetLastError(); break; }
                    if (received == 0) break;
                    bytes += received;
                    if (bytes >= 24 * 1024 * 1024 || GetTickCount64() > deadline) break;
                    Sleep(6); // Span several actual one-second sample windows.
                }
                if (error || bytes >= 24 * 1024 * 1024) break;
                Sleep(1000);
            }
            if (!connection && !error) error = GetLastError();
            done = true;
        });
    }
};

template<class Predicate> bool Wait(unsigned timeout_ms, Predicate predicate) {
    const auto deadline = GetTickCount64() + timeout_ms;
    do { if (predicate()) return true; Sleep(100); } while (GetTickCount64() < deadline);
    return predicate();
}

void ExerciseInitialization(const std::filesystem::path& database_path) {
    using Stage = WatchmanEngine::InitializationStage;
    { WatchmanEngine never_started; never_started.Stop(); never_started.Stop(); }
    Check("Unstarted engine supports repeated Stop and destruction", true);
    std::atomic<bool> entered{false};
    std::atomic<bool> worker_mta{false};
    const DWORD ui_thread = GetCurrentThreadId();
    const auto construct_at = GetTickCount64();
    WatchmanEngine delayed([&](Stage stage, const std::atomic<bool>& running) {
        if (stage != Stage::BeforeServices) return;
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        worker_mta = GetCurrentThreadId() != ui_thread &&
            SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && type == APTTYPE_MTA;
        entered = true;
        while (running) Sleep(5); // Stop cooperatively cancels the blocked setup.
    });
    Check("Engine construction does not initialize Windows services", GetTickCount64() - construct_at < 250);
    delayed.SetNetworkAdapterId(L"{EARLY-STARTUP-CHOICE}");
    const auto start_at = GetTickCount64();
    delayed.Start();
    Check("Start returns while initialization is deliberately blocked", GetTickCount64() - start_at < 250 &&
        Wait(3000, [&] { return entered.load(); }) && !delayed.IsReady());
    Check("Backend initialization has an owned MTA worker", worker_mta);
    delayed.RefreshNetworkDetails(); delayed.SetNetworkHealthActive(true); delayed.ResetSession();
    delayed.RunSpeedTest(); delayed.CancelSpeedTest();
    const auto export_result = delayed.ExportTrafficCsv({});
    Check("Early UI getters and commands tolerate unpublished services", delayed.GetDashboardSnapshot().sample_sequence == 0 &&
        delayed.GetNetworkSnapshot().local_ip.empty() && delayed.GetAppActivitySnapshot().active_apps.empty() &&
        delayed.GetTrafficHistorySnapshot().history.empty() && delayed.GetNetworkAdapters().empty() &&
        delayed.GetNetworkAdapterId() == L"{EARLY-STARTUP-CHOICE}" && !delayed.RunDiagnostics() &&
        !delayed.TerminateApp(0) && !delayed.IsSpeedTestRunning() && delayed.GetSpeedTestProgress() == 0 &&
        delayed.GetSpeedTestLiveSpeed() == 0.0 && delayed.GetSpeedTestResult().download_speed_mbps == 0.0 &&
        !delayed.GetSpeedTestStatus().empty() && !delayed.GetSpeedTestSnapshot().available &&
        !delayed.GetDiagnosticsSnapshot().is_running &&
        !export_result.success && !export_result.error.empty());

    // Process actual posted UI messages while setup remains blocked. This is
    // not a boot timing claim; it verifies the window thread is not the worker.
    HWND window = CreateWindowExW(0, L"STATIC", L"startup responsiveness probe", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    bool pumped = window != nullptr;
    for (int index = 0; window && index < 10; ++index) {
        PostMessageW(window, WM_APP + 42, index, 0);
        MSG message{};
        const bool received = PeekMessageW(&message, window, WM_APP + 42, WM_APP + 42, PM_REMOVE) != FALSE;
        pumped = pumped && received;
        if (received) DispatchMessageW(&message);
    }
    if (window) DestroyWindow(window);
    Check("Window messages remain responsive during blocked backend setup", pumped && !delayed.IsReady());
    const auto stop_at = GetTickCount64();
    delayed.Stop(); delayed.Stop();
    Check("Exit during initialization joins without dereferencing partial services", GetTickCount64() - stop_at < 1500 &&
        !delayed.IsReady() && delayed.GetStartupError().empty() && !std::filesystem::exists(database_path));

    // Exceptions at successive real construction boundaries exercise cleanup
    // of partial PDH/temperature/ETW/SQLite ownership. APPDATA is disposable.
    for (const auto stage : {Stage::BeforeServices, Stage::MetricsReady, Stage::AppTrackerReady, Stage::StorageReady, Stage::ToolsReady}) {
        WatchmanEngine failing([stage](Stage current, const std::atomic<bool>&) {
            if (current == stage) throw std::runtime_error("Deliberate startup regression-test failure");
        });
        failing.Start();
        const bool reported = Wait(15000, [&] { return !failing.GetStartupError().empty(); });
        failing.Stop(); failing.Stop();
        Check("Initialization exception is reported and partial ownership joins", reported && !failing.IsReady() &&
            !failing.RunDiagnostics() && failing.GetNetworkAdapters().empty(), "stage=" + std::to_string(static_cast<int>(stage)));
    }
}

void ExerciseOptionalGpuIsolation() {
    std::atomic<bool> entered{false};
    MetricsManager metrics([&](const std::atomic<bool>& stopping) {
        entered = true;
        while (!stopping) Sleep(2);
    });
    Check("Optional GPU discovery is stalled on its separate owner", Wait(3000, [&] { return entered.load(); }));
    const auto before = metrics.GetRam().sampled_at_ms;
    Sleep(1100);
    metrics.Update();
    Check("Core metrics publish while optional GPU discovery is blocked", !metrics.GetGpu().available &&
        metrics.GetRam().sample_available && metrics.GetRam().sampled_at_ms > before &&
        metrics.GetCpu().sample_available && metrics.GetUptimeSeconds() > 0);
}

void SeedHistoryProfiles(const std::filesystem::path& root) {
    wchar_t disposable[8]{};
    GetEnvironmentVariableW(L"WATCHMAN_SMOKE_DISPOSABLE", disposable, 8);
    Require(std::wstring(disposable) == L"1", "Seeding requires the disposable-runner wrapper");
    Require(!std::filesystem::exists(root / L"Watchman") && !std::filesystem::exists(root / L"Watchman.Native"),
        "Refusing to replace any existing history fixture");
    const auto today = Utf8(TrafficStorage::GetCurrentDateString());
    for (bool native : {false, true}) {
        const auto directory = root / (native ? L"Watchman.Native" : L"Watchman");
        std::filesystem::create_directories(directory);
        sqlite3* raw = nullptr;
        Require(sqlite3_open_v2(Utf8((directory / L"watchman.db").wstring()).c_str(), &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK, "Cannot create isolated history fixture");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> close(raw, sqlite3_close);
        const auto sql = std::string("CREATE TABLE daily_traffic(date TEXT PRIMARY KEY,download_bytes INTEGER NOT NULL DEFAULT 0,"
            "upload_bytes INTEGER NOT NULL DEFAULT 0,updated_at TEXT NOT NULL); INSERT INTO daily_traffic VALUES('") +
            (native ? "2001-01-01',100,20,'native-sentinel');" : "2000-01-01',30,4,'legacy-sentinel');") +
            "INSERT INTO daily_traffic VALUES('" + today + (native ? "',5368709120,1073741824,'native-today');" : "',7,3,'legacy-today');");
        Require(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "Cannot seed isolated history rows");
    }
    const auto native = root / L"Watchman.Native";
    std::filesystem::create_directory(native / L"original-v1");
    std::ofstream(native / L"migration-v2.complete") << "2.0.0\n";
    std::ofstream(native / L"original-v1" / L"watchman.db") << "archival fixture must never be opened";
    std::ofstream(root / L"Watchman" / L"history.json") << "archival JSON must not be imported or modified";
    Check("Seeded separate native and stale legacy profiles on disposable storage", true);
}

void VerifyHistoryProfiles(const std::filesystem::path& root) {
    Database native(root / L"Watchman.Native" / L"watchman.db");
    Database legacy(root / L"Watchman" / L"watchman.db");
    Check("Actual EXE retains native-only history", native.Integer("SELECT download_bytes FROM daily_traffic WHERE date='2001-01-01'") == 100 &&
        native.Integer("SELECT COUNT(*) FROM daily_traffic WHERE date='2000-01-01'") == 0);
    const auto today = Utf8(TrafficStorage::GetCurrentDateString());
    const auto download = native.Integer(("SELECT download_bytes FROM daily_traffic WHERE date='" + today + "'").c_str());
    const auto upload = native.Integer(("SELECT upload_bytes FROM daily_traffic WHERE date='" + today + "'").c_str());
    Check("Actual EXE preserves the native current-day baseline", download >= 5368709120ULL && upload >= 1073741824ULL);
    Check("Actual EXE adds newly measured traffic to the native profile", download > 5368709120ULL || upload > 1073741824ULL,
        "saved_rx=" + std::to_string(download) + ", saved_tx=" + std::to_string(upload));
    Check("Legacy test-session history remains separate and unchanged", legacy.Integer("SELECT SUM(download_bytes) FROM daily_traffic") == 37 &&
        legacy.Integer("SELECT SUM(upload_bytes) FROM daily_traffic") == 7);
    Check("Selected native database has an additive per-app schema", native.Integer("SELECT COUNT(*) FROM pragma_table_info('app_traffic')") == 5);
}

int Exercise(const std::filesystem::path& output) {
    wchar_t disposable[8]{};
    GetEnvironmentVariableW(L"WATCHMAN_SMOKE_DISPOSABLE", disposable, static_cast<DWORD>(std::size(disposable)));
    Require(std::wstring(disposable) == L"1", "Runtime smoke requires the disposable-runner wrapper");
    const auto appdata = output / L"engine-appdata";
    const auto database_path = appdata / L"Watchman" / L"watchman.db";
    Require(!std::filesystem::exists(database_path), "Refusing to reuse an existing runtime-smoke database");
    std::filesystem::create_directories(appdata);
    Require(SetEnvironmentVariableW(L"APPDATA", appdata.c_str()) != FALSE, "Could not isolate APPDATA");

    WSADATA winsock{};
    Require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "WinSock initialization failed");
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Cleanup { HRESULT result; ~Cleanup() { if (SUCCEEDED(result)) CoUninitialize(); WSACleanup(); } } cleanup{com};
    ExerciseOptionalGpuIsolation();
    ExerciseInitialization(database_path);
    auto engine = std::make_unique<WatchmanEngine>();
    engine->SetNetworkAdapterId(L"{WATCHMAN-SMOKE-EARLY-MISSING-ADAPTER}");
    engine->Start();
    Check("Engine publishes initial snapshot", Wait(12000, [&] { return engine->GetDashboardSnapshot().sample_sequence > 0; }));
    Check("Saved adapter choice is applied before the first sample", engine->IsReady() &&
        engine->GetNetworkAdapterId() == L"{WATCHMAN-SMOKE-EARLY-MISSING-ADAPTER}" && !engine->GetNetworkSnapshot().is_connected);
    engine->SetNetworkAdapterId(L"");
    Check("Auto adapter selection works after asynchronous readiness", Wait(8000, [&] { return engine->GetNetworkSnapshot().is_connected; }));
    Sleep(3500); // CPU must prime; ETW's real-time consumer must connect.
    Traffic traffic;
    traffic.Start();
    std::set<uint64_t> cpu_times;
    uint64_t previous_sequence = 0, max_gap_ms = 0, previous_at = GetTickCount64(), max_own_rx = 0, max_own_tx = 0;
    uint64_t max_own_rate = 0;
    double maximum_rate = 0;
    bool finite = true, ram_valid = true, sensor_valid = true, saw_sensor = false;
    for (int index = 0; index < 27; ++index) {
        Sleep(1000);
        const auto dash = engine->GetDashboardSnapshot();
        const auto apps = engine->GetAppActivitySnapshot();
        const auto now = GetTickCount64();
        if (dash.sample_sequence != previous_sequence) {
            max_gap_ms = std::max(max_gap_ms, now - previous_at);
            previous_at = now;
            previous_sequence = dash.sample_sequence;
        }
        if (dash.cpu_sample_available && now >= dash.cpu_sampled_at_ms && now - dash.cpu_sampled_at_ms <= 5000)
            cpu_times.insert(dash.cpu_sampled_at_ms);
        finite = finite && std::isfinite(dash.cpu_usage_percent) && dash.cpu_usage_percent >= 0 && dash.cpu_usage_percent <= 100 &&
            std::isfinite(dash.download_speed_bps) && dash.download_speed_bps >= 0 &&
            std::isfinite(dash.upload_speed_bps) && dash.upload_speed_bps >= 0;
        ram_valid = ram_valid && dash.ram_sample_available && dash.ram_total_bytes > 0 &&
            dash.ram_used_bytes <= dash.ram_total_bytes && dash.ram_usage_percent >= 0 && dash.ram_usage_percent <= 100 &&
            now >= dash.ram_sampled_at_ms && now - dash.ram_sampled_at_ms <= 5000;
        for (const auto& sensor : dash.temperatures.readings) {
            if (!sensor.available) continue;
            saw_sensor = true;
            sensor_valid = sensor_valid && WatchmanSensors::Fresh(sensor, now) && WatchmanSensors::ValidCelsius(sensor.celsius);
        }
        for (const auto& app : apps.active_apps) {
            if (app.pid != GetCurrentProcessId()) continue;
            max_own_rx = std::max(max_own_rx, app.total_download_bytes);
            max_own_tx = std::max(max_own_tx, app.total_upload_bytes);
            max_own_rate = std::max(max_own_rate, app.current_download_bps);
        }
        maximum_rate = std::max(maximum_rate, dash.download_speed_bps);
        std::cout << "{\"event\":\"sample\",\"sequence\":" << dash.sample_sequence
                  << ",\"cpu_available\":" << (dash.cpu_sample_available ? "true" : "false") << ",\"cpu\":" << dash.cpu_usage_percent
                  << ",\"cpu_at\":" << dash.cpu_sampled_at_ms << ",\"at\":" << now << ",\"ram\":" << dash.ram_usage_percent
                  << ",\"download_bps\":" << dash.download_speed_bps << ",\"upload_bps\":" << dash.upload_speed_bps
                  << ",\"session_rx\":" << dash.session_download_bytes << ",\"session_tx\":" << dash.session_upload_bytes
                  << ",\"etw_ready\":" << (apps.bandwidth_available ? "true" : "false")
                  << ",\"own_rx\":" << max_own_rx << ",\"own_tx\":" << max_own_tx << ",\"own_peak_download_bps\":" << max_own_rate << "}" << std::endl;
    }
    if (traffic.worker.joinable()) traffic.worker.join();
    const auto dash = engine->GetDashboardSnapshot();
    const auto network = engine->GetNetworkSnapshot();
    const auto apps = engine->GetAppActivitySnapshot();
    Check("Live CPU refreshes and remains finite", finite && cpu_times.size() >= 10,
        "distinct fresh CPU samples=" + std::to_string(cpu_times.size()));
    Check("Live memory is fresh and within physical bounds", ram_valid);
    Check("Engine continues sampling without long stalls", previous_sequence >= 20 && max_gap_ms < 5000,
        "maximum observed gap_ms=" + std::to_string(max_gap_ms));
    Check("Windows route selects a connected adapter", network.is_connected && !network.local_ip.empty() && !network.active_adapter_name.empty(),
        Utf8(network.active_adapter_name + L" / " + network.local_ip));
    Check("Controlled HTTPS traffic actually transferred", traffic.bytes >= 1024 * 1024,
        "payload_bytes=" + std::to_string(traffic.bytes.load()) + ", error=" + std::to_string(traffic.error.load()));
    Check("Real adapter records download AND upload", dash.session_download_bytes >= traffic.bytes && dash.session_upload_bytes > 0 && maximum_rate > 0,
        "rx=" + std::to_string(dash.session_download_bytes) + ", tx=" + std::to_string(dash.session_upload_bytes));
    Note("App tracking status", L"admin=" + std::to_wstring(apps.is_admin) + L", capture=" + std::to_wstring(apps.bandwidth_available) + L", " + apps.status_message);
    if (apps.is_admin) {
        Check("Elevated ETW capture connects", apps.bandwidth_available && !apps.requires_admin);
        Check("ETW attributes real traffic to the generating PID", traffic.bytes >= 1024 * 1024 && max_own_rx > traffic.bytes / 2 && max_own_tx > 0 && max_own_rate > 0,
            "pid=" + std::to_string(GetCurrentProcessId()) + ", rx=" + std::to_string(max_own_rx) + ", tx=" + std::to_string(max_own_tx) + ", peak_download_bps=" + std::to_string(max_own_rate));
    } else {
        Check("Non-admin mode honestly disables per-app bandwidth", !apps.bandwidth_available && apps.requires_admin && !apps.status_message.empty());
        Note("Limitation", L"Runner is not elevated: administrator-only ETW traffic accuracy was not tested.");
    }
    Check("Temperature readings fail closed or remain fresh", sensor_valid);
    if (!saw_sensor) Note("Limitation", L"Cloud VM exposes no temperature sensors. Real CPU/GPU/disk/mainboard temperature accuracy was not tested.");
    Check("History save has no error", dash.history_save_error.empty(), Utf8(dash.history_save_error));

    engine->ResetSession();
    Check("Reset session takes effect on the running worker", Wait(5000, [&] {
        return engine->GetDashboardSnapshot().session_download_bytes < dash.session_download_bytes;
    }));
    const auto original_adapter = engine->GetNetworkAdapterId();
    engine->SetNetworkAdapterId(L"{WATCHMAN-SMOKE-MISSING-ADAPTER}");
    Check("Missing pinned adapter does not fabricate a connection", Wait(7000, [&] {
        const auto snap = engine->GetNetworkSnapshot();
        const auto current = engine->GetDashboardSnapshot();
        return !snap.is_connected && current.download_speed_bps == 0 && current.upload_speed_bps == 0;
    }));
    engine->SetNetworkAdapterId(original_adapter);
    Check("Auto adapter recovers after selection is restored", Wait(8000, [&] { return engine->GetNetworkSnapshot().is_connected; }));

    engine->SetNetworkHealthActive(true);
    engine->RefreshNetworkDetails();
    Sleep(2500);
    engine->SetNetworkHealthActive(false);
    Check("Network details refresh leaves the engine responsive", engine->GetDashboardSnapshot().sample_sequence > dash.sample_sequence);

    Check("Diagnostics start on the production worker", engine->RunDiagnostics());
    const bool diagnostics_done = Wait(45000, [&] { return !engine->GetDiagnosticsSnapshot().is_running; });
    const auto diagnostics = engine->GetDiagnosticsSnapshot();
    Check("Diagnostics complete with explicit probe results", diagnostics_done && diagnostics.has_run && !diagnostics.tests.empty(), Utf8(diagnostics.overall_status));
    for (const auto& test : diagnostics.tests) Note("Diagnostic result", test.name + L" / status=" + std::to_wstring(static_cast<int>(test.status)) + L" / " + test.message + L" / " + test.details);

    // Exercise cancellation early, not a data-intensive completed speed test.
    engine->RunSpeedTest();
    Sleep(100);
    const auto cancel_at = GetTickCount64();
    engine->CancelSpeedTest();
    engine->RunSpeedTest(); // An unfinished cancellation must not join/revive the old worker from this caller.
    Check("Immediate speed cancel/restart returns without a worker join", GetTickCount64() - cancel_at < 250);
    const bool cancelled = Wait(15000, [&] { return !engine->IsSpeedTestRunning(); });
    Check("Speed-test cancellation finishes within 15 seconds", cancelled,
        "cancel_ms=" + std::to_string(GetTickCount64() - cancel_at) + ", status=" + Utf8(engine->GetSpeedTestStatus()));

    const auto stop_at = GetTickCount64();
    engine->Stop();
    Check("Stop joins the engine and flushes pending history", GetTickCount64() - stop_at < 5000 && !engine->GetDashboardSnapshot().history_save_pending,
        "stop_ms=" + std::to_string(GetTickCount64() - stop_at));
    uint64_t saved_rx = 0;
    {
        Database database(database_path);
        saved_rx = database.Integer("SELECT COALESCE(SUM(download_bytes),0) FROM daily_traffic;");
        Check("Measured traffic survives reset and graceful stop", saved_rx >= dash.session_download_bytes,
            "saved_rx=" + std::to_string(saved_rx));
    }
    InspectDatabase(database_path);

    std::vector<TrafficRecord> export_records;
    {
        TrafficStorage persisted(database_path.wstring());
        Check("Export validation reads the persisted byte totals", persisted.LoadHistory(export_records));
    }
    for (const auto& entry : std::vector<std::pair<TrafficExportPeriod, const wchar_t*>>{
            {TrafficExportPeriod::Daily, L"daily"}, {TrafficExportPeriod::Weekly, L"weekly"},
            {TrafficExportPeriod::Monthly, L"monthly"}, {TrafficExportPeriod::Yearly, L"yearly"}}) {
        TrafficExportOptions options;
        options.period = entry.first;
        options.month = TrafficStorage::GetCurrentDateString().substr(0, 7);
        options.year = std::stoi(options.month.substr(0, 4));
        const std::string label = Utf8(entry.second);
        const auto exported = engine->ExportTrafficCsv(options);
        Check(("Real saved history exports to " + label + " CSV").c_str(), exported.success, Utf8(exported.error));
        if (exported.success) {
            std::ifstream input(std::filesystem::path(exported.filepath), std::ios::binary);
            const std::string csv((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            input.close();
            const auto expected = WatchmanExport::BuildCsv(export_records, options);
            Check((label + " export preserves the formatter's exact BOM, five columns, CRLF and saved totals").c_str(),
                !expected.utf8.empty() && csv == expected.utf8 && csv.substr(0, 3) == "\xEF\xBB\xBF" &&
                csv.find(",Summary\r\n") != std::string::npos && csv.find("Bytes") == std::string::npos);
            const auto filename = entry.first == TrafficExportPeriod::Daily ? std::wstring(L"actual-history-export.csv") :
                std::wstring(L"actual-history-export-") + entry.second + L".csv";
            std::filesystem::copy_file(exported.filepath, output / filename, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove(exported.filepath);
        }
    }
    const auto destroy_at = GetTickCount64();
    engine.reset();
    Check("All service destructors shut down promptly", GetTickCount64() - destroy_at < 5000,
        "destruct_ms=" + std::to_string(GetTickCount64() - destroy_at));

    engine = std::make_unique<WatchmanEngine>();
    engine->Start();
    Check("A fresh engine restores persisted history", Wait(10000, [&] {
        return engine->GetTrafficHistorySnapshot().all_time_download_bytes >= saved_rx;
    }));
    engine->Stop();
    engine.reset();
    std::cout << "{\"event\":\"summary\",\"failures\":" << failures << "}" << std::endl;
    return failures ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--seed-profiles") { SeedHistoryProfiles(argv[2]); return failures ? 1 : 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--verify-profiles") { VerifyHistoryProfiles(argv[2]); return failures ? 1 : 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--inspect-db") { InspectDatabase(argv[2]); return failures ? 1 : 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--exercise") return Exercise(std::filesystem::absolute(argv[2]));
        std::cerr << "Use --exercise <disposable output directory> or --inspect-db <history path>\n";
        return 2;
    } catch (const std::exception& error) {
        Check("Runtime smoke probe exception", false, error.what());
        return 1;
    }
}
