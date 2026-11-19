#include "temperature_monitor.h"
#include "temperature_provider.h"
#include <windows.h>
#include <winternl.h> // NTSTATUS must be defined before the graphics thunk header.
#include <dxgi.h>
#include <d3dkmthk.h>
#include <winhttp.h>
#include <wrl/client.h>
#include <roapi.h>
#include <chrono>
#include <memory>

namespace {
using namespace WatchmanSensors;

struct InternetCloser {
    void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetCloser>;

// Read-only, fixed IPv4 loopback endpoint. Never use the user's HTTP proxy,
// follow redirects to another host, send credentials, or fetch code/drivers.
std::wstring ReadLocalHardwareMonitor() {
    InternetHandle session(WinHttpOpen(L"Watchman/2.0 sensors", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session || !WinHttpSetTimeouts(session.get(), 250, 250, 250, 250)) return {};
    InternetHandle connection(WinHttpConnect(session.get(), L"127.0.0.1", 8085, 0));
    if (!connection) return {};
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", L"/data.json", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) return {};
    DWORD no_auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &no_auth, sizeof(no_auth))) return {};
    const ULONGLONG deadline = GetTickCount64() + 1500;
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) return {};
    DWORD status = 0, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200) return {};
    std::string response;
    char buffer[8192];
    for (;;) {
        if (GetTickCount64() >= deadline) return {};
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &received)) return {};
        if (received == 0) break;
        if (response.size() + received > 1024 * 1024) return {};
        response.append(buffer, received);
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        response.data(), static_cast<int>(response.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring json(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, response.data(),
            static_cast<int>(response.size()), json.data(), length) != length) return {};
    return json;
}

Reading ReadWindowsGpuTemperature() {
    // Windows' WDDM performance data is the same driver family used for the
    // GPU temperature in Task Manager. Unsupported drivers return no value.
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&factory)))) return {};
    Reading hottest;
    for (UINT index = 0; index < 32; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (factory->EnumAdapters(index, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC description{};
        if (FAILED(adapter->GetDesc(&description))) continue;
        D3DKMT_OPENADAPTERFROMLUID open{};
        open.AdapterLuid = description.AdapterLuid;
        if (D3DKMTOpenAdapterFromLuid(&open) < 0) continue;
        struct AdapterScope {
            D3DKMT_HANDLE handle;
            ~AdapterScope() { D3DKMT_CLOSEADAPTER close{}; close.hAdapter = handle; D3DKMTCloseAdapter(&close); }
        } scope{open.hAdapter};
        D3DKMT_PHYSICAL_ADAPTER_COUNT physical{};
        D3DKMT_QUERYADAPTERINFO query{};
        query.hAdapter = open.hAdapter;
        query.Type = KMTQAITYPE_PHYSICALADAPTERCOUNT;
        query.pPrivateDriverData = &physical;
        query.PrivateDriverDataSize = sizeof(physical);
        const UINT physical_count = D3DKMTQueryAdapterInfo(&query) >= 0
            ? std::clamp<UINT>(physical.Count, 1, 16) : 1;
        for (UINT device = 0; device < physical_count; ++device) {
            D3DKMT_ADAPTER_PERFDATA data{};
            data.PhysicalAdapterIndex = device;
            query.Type = KMTQAITYPE_ADAPTERPERFDATA;
            query.pPrivateDriverData = &data;
            query.PrivateDriverDataSize = sizeof(data);
            if (D3DKMTQueryAdapterInfo(&query) < 0) continue;
            const double celsius = data.Temperature / 10.0; // Driver units are tenths of Celsius.
            if (ValidCelsius(celsius) && (!hottest.available || celsius > hottest.celsius)) {
                hottest = {true, celsius, GetTickCount64(), L"GPU temperature", description.Description, L"Windows GPU driver"};
            }
        }
    }
    return hottest;
}
} // namespace

TemperatureMonitor::TemperatureMonitor() : worker_([this] { Run(); }) {}

TemperatureMonitor::~TemperatureMonitor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

WatchmanSensors::Snapshot TemperatureMonitor::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto snapshot = snapshot_;
    const auto now = GetTickCount64();
    for (auto& reading : snapshot.readings) {
        if (!WatchmanSensors::Fresh(reading, now)) reading = {};
    }
    return snapshot;
}

void TemperatureMonitor::Run() {
    // COM/WinRT objects stay on this worker and are released before uninitializing.
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    struct RuntimeScope {
        HRESULT result;
        ~RuntimeScope() { if (SUCCEEDED(result)) RoUninitialize(); }
    } runtime{initialized};
    while (true) {
        Snapshot next;
        next.scanning = false;
        try {
            if (SUCCEEDED(initialized)) {
                std::vector<Candidate> sensors;
                const auto json = ReadLocalHardwareMonitor();
                next.provider_connected = ParseLibreHardwareMonitorJson(json, sensors);
                if (next.provider_connected) next.readings = Select(sensors, GetTickCount64(), L"LibreHardwareMonitor");
            }
            auto& gpu = next.readings[static_cast<size_t>(Kind::Gpu)];
            if (!gpu.available) gpu = ReadWindowsGpuTemperature();
        } catch (...) {
            // Optional telemetry must never terminate the application. A failed
            // scan clears readings instead of preserving a frozen high/low temp.
            next = {};
            next.scanning = false;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        snapshot_ = std::move(next);
        if (wake_.wait_for(lock, std::chrono::seconds(3), [this] { return stopping_; })) break;
    }
}
