#include "gpu_monitor.h"
#include "startup_diagnostics.h"
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <utility>

namespace {
constexpr wchar_t kGpuCounterWildcard[] = L"\\GPU Engine(*)\\Utilization Percentage";
constexpr ULONGLONG kGpuCounterRefreshIntervalMs = 8000;
constexpr PDH_STATUS kPdhMoreData = static_cast<PDH_STATUS>(0x800007D2u);

static std::vector<std::wstring> ExpandGpuCounterPaths() {
    DWORD required_chars = 0;
    const PDH_STATUS first_status = PdhExpandWildCardPathW(
        nullptr, kGpuCounterWildcard, nullptr, &required_chars, 0
    );
    if (first_status != kPdhMoreData || required_chars == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(required_chars);
    DWORD buffer_chars = required_chars;
    if (PdhExpandWildCardPathW(nullptr, kGpuCounterWildcard, buffer.data(), &buffer_chars, 0) != ERROR_SUCCESS) {
        return {};
    }

    std::vector<std::wstring> paths;
    for (const wchar_t* current = buffer.data(); current != nullptr && *current != L'\0';) {
        std::wstring path(current);
        if (path.find(L"_Total") == std::wstring::npos) {
            paths.push_back(std::move(path));
        }
        current += wcslen(current) + 1;
    }
    return paths;
}

static std::wstring ExtractGpuInstanceName(const std::wstring& counter_path) {
    const size_t start_marker = counter_path.find(L'(');
    const size_t start = start_marker == std::wstring::npos ? 0 : start_marker + 1;
    const size_t end = counter_path.find(L')', start);
    return counter_path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

static std::wstring ExtractGpuKey(const std::wstring& counter_path) {
    const std::wstring instance = ExtractGpuInstanceName(counter_path);
    const size_t marker = instance.find(L"_phys_");
    if (marker != std::wstring::npos) {
        size_t end = marker + 6;
        while (end < instance.size() && std::iswdigit(instance[end])) {
            ++end;
        }
        if (end > marker + 6) {
            return L"phys_" + instance.substr(marker + 6, end - (marker + 6));
        }
    }
    return instance;
}

static std::wstring PrettifyGpuEngineName(const std::wstring& raw_name) {
    std::wstring pretty;
    pretty.reserve(raw_name.size() + 4);
    bool previous_lowercase = false;
    for (wchar_t character : raw_name) {
        if (character == L'_') {
            character = L' ';
        }
        if (previous_lowercase && std::iswupper(character)) {
            pretty.push_back(L' ');
        }
        pretty.push_back(character);
        previous_lowercase = std::iswlower(character) != 0;
    }
    while (!pretty.empty() && pretty.back() == L' ') {
        pretty.pop_back();
    }
    return pretty.empty() ? L"Active engine" : pretty;
}

static std::wstring ExtractGpuEngineName(const std::wstring& counter_path) {
    const std::wstring instance = ExtractGpuInstanceName(counter_path);
    const size_t marker = instance.find(L"_engtype_");
    return marker == std::wstring::npos
        ? L"Active engine"
        : PrettifyGpuEngineName(instance.substr(marker + 9));
}

} // namespace

GpuMonitor::GpuMonitor(InitializationHook before_initialize)
    : before_initialize_(std::move(before_initialize)) {
    try { worker_ = std::thread([this] { Run(); }); }
    catch (...) { WatchmanStartupDiagnostics::Record("gpu-counters", "worker-unavailable"); }
}

GpuMonitor::~GpuMonitor() {
    stopping_ = true;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

GpuMetrics GpuMonitor::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = snapshot_;
    const auto now = GetTickCount64();
    if (!sampled_at_ms_ || now < sampled_at_ms_ || now - sampled_at_ms_ > 5000) {
        result.available = false;
        result.active_engine = L"Unavailable";
    }
    return result;
}

void GpuMonitor::CloseQuery() {
    if (pdh_query_) PdhCloseQuery(static_cast<HQUERY>(pdh_query_));
    pdh_query_ = nullptr;
    pdh_gpu_counters_.clear();
    pdh_gpu_initialized_ = pdh_gpu_primed_ = false;
}

void GpuMonitor::InitializeLabel() {
    // Get GPU Name/Label from DXGI
    IDXGIFactory* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) {
        IDXGIAdapter* pAdapter = nullptr;
        if (SUCCEEDED(pFactory->EnumAdapters(0, &pAdapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(pAdapter->GetDesc(&desc))) {
                gpu_.label = desc.Description;
                gpu_.available = true;
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }

    if (!gpu_.available) {
        gpu_.label = L"N/A";
        gpu_.available = false;
    }

}

void GpuMonitor::Run() noexcept {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Scope { HRESULT result; ~Scope() { if (SUCCEEDED(result)) CoUninitialize(); } } scope{com};
    try {
        WatchmanStartupDiagnostics::Record("gpu-counters", "starting");
        if (before_initialize_) before_initialize_(stopping_);
        if (!stopping_) InitializeLabel();
        while (!stopping_) {
            UpdateGpu();
            std::unique_lock<std::mutex> lock(mutex_);
            snapshot_ = gpu_;
            sampled_at_ms_ = GetTickCount64();
            if (wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_.load(); })) break;
        }
    } catch (...) {
        // Optional hardware discovery failure must not stop network accounting.
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.available = false;
        sampled_at_ms_ = 0;
        WatchmanStartupDiagnostics::Record("gpu-counters", "unavailable");
    }
    CloseQuery(); // Release native query on its owner before COM uninitializes.
}

void GpuMonitor::UpdateGpu() {
    gpu_.available = false;
    const auto now = GetTickCount64();
    if (!pdh_gpu_initialized_ && (last_retry_ms_ == 0 || now - last_retry_ms_ >= 30000)) InitGpuPdh();

    if (!pdh_gpu_initialized_ || !pdh_query_ || pdh_gpu_counters_.empty()) {
        gpu_.percent_used = 0.0f;
        gpu_.active_engine = L"Unavailable";
        return;
    }

    RefreshGpuPdhCounters();
    if (!pdh_gpu_initialized_ || !pdh_query_ || pdh_gpu_counters_.empty()) {
        gpu_.percent_used = 0.0f;
        gpu_.active_engine = L"Unavailable";
        return;
    }

    const HQUERY query = static_cast<HQUERY>(pdh_query_);
    if (PdhCollectQueryData(query) != ERROR_SUCCESS) {
        gpu_.percent_used = 0.0f;
        gpu_.active_engine = L"Unavailable";
        CloseQuery();
        last_retry_ms_ = now;
        return;
    }

    // PDH needs a first sample before formatted utilization values are valid.
    if (!pdh_gpu_primed_) {
        pdh_gpu_primed_ = true;
        gpu_.percent_used = 0.0f;
        gpu_.active_engine = L"Starting...";
        return;
    }

    bool valid_sample = false;
    std::map<std::pair<std::wstring, std::wstring>, double> per_gpu_engine;
    for (const GpuPdhCounter& entry : pdh_gpu_counters_) {
        DWORD counter_type = 0;
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(
                static_cast<HCOUNTER>(entry.handle), PDH_FMT_DOUBLE, &counter_type, &value
            ) != ERROR_SUCCESS || (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) ||
            !std::isfinite(value.doubleValue) || value.doubleValue < 0.0) {
            continue;
        }

        valid_sample = true; // A measured idle zero is valid, unlike a missing counter.
        per_gpu_engine[{ExtractGpuKey(entry.path), ExtractGpuEngineName(entry.path)}] += value.doubleValue;
    }

    std::map<std::wstring, double> per_gpu_overall;
    std::wstring busiest_engine = L"Idle";
    double busiest_value = 0.0;
    for (const auto& [key, engine_total] : per_gpu_engine) {
        const double clamped_total = std::min(engine_total, 100.0);
        double& gpu_total = per_gpu_overall[key.first];
        gpu_total = std::max(gpu_total, clamped_total);
        if (clamped_total > busiest_value) {
            busiest_value = clamped_total;
            busiest_engine = key.second;
        }
    }

    double overall = 0.0;
    for (const auto& [gpu_key, gpu_total] : per_gpu_overall) {
        overall = std::max(overall, gpu_total);
    }

    gpu_.percent_used = static_cast<float>(std::round(std::clamp(overall, 0.0, 100.0) * 10.0) / 10.0);
    gpu_.available = valid_sample;
    gpu_.active_engine = !valid_sample ? L"Unavailable" : overall > 0.0 ? busiest_engine : L"Idle";
}

void GpuMonitor::InitGpuPdh() {
    CloseQuery();
    last_retry_ms_ = GetTickCount64();

    const std::vector<std::wstring> paths = ExpandGpuCounterPaths();
    if (paths.empty()) {
        return;
    }

    HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || !query) {
        return;
    }

    for (const std::wstring& path : paths) {
        HCOUNTER counter = nullptr;
        if (PdhAddEnglishCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS && counter) {
            pdh_gpu_counters_.push_back({ counter, path });
        }
    }

    if (pdh_gpu_counters_.empty()) {
        PdhCloseQuery(query);
        return;
    }

    pdh_query_ = query;
    pdh_gpu_initialized_ = true;
    pdh_gpu_primed_ = false;
    last_gpu_counter_refresh_ms_ = GetTickCount64();
    gpu_.active_engine = L"Starting...";
}

void GpuMonitor::RefreshGpuPdhCounters() {
    if (!pdh_gpu_initialized_) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - last_gpu_counter_refresh_ms_ < kGpuCounterRefreshIntervalMs) {
        return;
    }
    last_gpu_counter_refresh_ms_ = now;

    std::vector<std::wstring> refreshed_paths = ExpandGpuCounterPaths();
    if (refreshed_paths.empty()) {
        return;
    }
    std::sort(refreshed_paths.begin(), refreshed_paths.end());

    std::vector<std::wstring> current_paths;
    current_paths.reserve(pdh_gpu_counters_.size());
    for (const GpuPdhCounter& entry : pdh_gpu_counters_) {
        current_paths.push_back(entry.path);
    }
    std::sort(current_paths.begin(), current_paths.end());

    if (refreshed_paths != current_paths) {
        InitGpuPdh();
    }
}

