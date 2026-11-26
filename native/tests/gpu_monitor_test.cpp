// Owned-worker lifetime tests. Hooks stop before DXGI/PDH, so these need no
// GPU/driver and do not inspect the user's metrics, preferences or history.
#include "gpu_monitor.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto logs = std::filesystem::path(temp) / (L"Watchman-gpu-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(logs);
    SetEnvironmentVariableW(L"LOCALAPPDATA", logs.c_str());
    try {
        std::atomic<bool> entered{false};
        std::atomic<DWORD> owner{0};
        const auto start = GetTickCount64();
        auto monitor = std::make_unique<GpuMonitor>([&](const std::atomic<bool>& stopping) {
            owner = GetCurrentThreadId();
            entered = true;
            while (!stopping) Sleep(1);
        });
        if (GetTickCount64() - start >= 250) throw std::runtime_error("GPU construction blocked the caller");
        const auto deadline = GetTickCount64() + 3000;
        while (!entered && GetTickCount64() < deadline) Sleep(1);
        if (!entered || owner == GetCurrentThreadId()) throw std::runtime_error("GPU initialization did not use its owner worker");
        const auto read_at = GetTickCount64();
        for (int sample = 0; sample < 1000; ++sample)
            if (monitor->GetSnapshot().available) throw std::runtime_error("Uninitialized GPU exposed invented measurements");
        if (GetTickCount64() - read_at >= 250) throw std::runtime_error("GPU snapshot reads waited for blocked discovery");
        const auto stop_at = GetTickCount64();
        monitor.reset();
        if (GetTickCount64() - stop_at >= 1000) throw std::runtime_error("GPU Stop failed to join cooperative discovery");
        {
            GpuMonitor failed([](const std::atomic<bool>&) { throw std::runtime_error("Owned optional discovery fault"); });
            Sleep(20);
            if (failed.GetSnapshot().available) throw std::runtime_error("Optional discovery fault was reported as valid data");
        }
        std::cout << "GpuMonitor: owned blocked/failed startup, nonblocking reads, and joined shutdown passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GpuMonitor failure: " << error.what() << '\n';
        return 1;
    }
}
