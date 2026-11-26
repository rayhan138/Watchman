#include "startup_diagnostics.h"

#include <windows.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace WatchmanStartupDiagnostics {
namespace {
class Log {
public:
    Log() : worker_([this] { Run(); }) {}
    ~Log() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    void Push(const char* component, const char* event, uint32_t error, uint64_t timestamp_ms) {
        const std::string line = std::to_string(timestamp_ms) + " pid=" + std::to_string(GetCurrentProcessId())
            + " " + component + " " + event
            + " error=" + std::to_string(error) + "\n";
        OutputDebugStringA(line.c_str());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            // Only diagnostics may be dropped. This queue never owns telemetry
            // or history. Bound memory even if profile storage is unavailable.
            if (queue_.size() == 64) queue_.pop_front();
            queue_.push_back(line);
        }
        wake_.notify_one();
    }
private:
    void Run() noexcept {
        try {
            const wchar_t* pattern = L"%LOCALAPPDATA%\\Watchman\\startup-recovery.log";
            const DWORD needed = ExpandEnvironmentStringsW(pattern, nullptr, 0);
            std::vector<wchar_t> buffer(needed);
            std::filesystem::path path;
            if (needed && ExpandEnvironmentStringsW(pattern, buffer.data(), needed) == needed &&
                std::wstring(buffer.data()).find(L'%') == std::wstring::npos) {
                path = buffer.data();
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                if (error) path.clear();
            }
            std::ofstream output;
            size_t bytes = 0;
            const auto open = [&] {
                output.close();
                output.clear();
                std::error_code error;
                const auto size = path.empty() ? 0 : std::filesystem::file_size(path, error);
                bytes = error ? 0 : static_cast<size_t>(size);
                if (path.empty()) return;
                if (bytes >= 65536) bytes = 0;
                output.open(path, std::ios::binary | (bytes ? std::ios::app : std::ios::trunc));
            };
            open();
            for (;;) {
                std::string line;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                    if (queue_.empty() && stopping_) break;
                    line = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (bytes + line.size() > 65536 && output.is_open()) {
                    output.close();
                    output.clear();
                    output.open(path, std::ios::binary | std::ios::trunc);
                    bytes = 0;
                }
                if (output) {
                    output << line;
                    output.flush();
                    bytes += line.size();
                }
            }
        } catch (...) {
            // A missing/read-only profile or disk error is not an app failure.
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    bool stopping_ = false;
    std::thread worker_;
};
} // namespace

void RecordAt(const char* component, const char* event, uint64_t timestamp_ms, uint32_t error) noexcept {
    try {
        static Log log;
        log.Push(component, event, error, timestamp_ms);
    } catch (...) {
        OutputDebugStringA("Watchman: startup diagnostics unavailable.\n");
    }
}

void Record(const char* component, const char* event, uint32_t error) noexcept {
    RecordAt(component, event, GetTickCount64(), error);
}
} // namespace WatchmanStartupDiagnostics
