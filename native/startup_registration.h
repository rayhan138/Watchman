#pragma once

#include "startup_registration_policy.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace WatchmanStartup {
// Windows-only implementation. Called exclusively on an owned background
// thread; COM/Task Scheduler/Registry work must never gate the first UI frame.
Result ApplyForCurrentUser(const Request& request) noexcept;

// One serial owner, at most one in-flight request plus the newest pending one.
// No HWND/renderer/settings access crosses into this worker. The UI polls a
// short, independent mailbox and ignores completions older than its request.
class Worker {
public:
    using Work = std::function<Result(const Request&)>;
    explicit Worker(Work work);
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    uint64_t Submit(bool enabled, bool explicit_change, std::wstring executable = {});
    bool TakeResult(Result& result);
    // Drain the latest saved preference, then join. Native COM calls are not
    // forcibly interrupted; this is ownership, not a hard shutdown deadline.
    void Stop();
private:
    void Run();
    Work work_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    std::optional<Result> completed_;
    std::thread worker_;
    uint64_t sequence_ = 0;
    bool stopping_ = false;
};

#ifdef WATCHMAN_STARTUP_REGISTRATION_TESTING
namespace Testing {
// Available only in the native test binary, never a production command-line
// or environment override. Registry/task names are restricted to test scopes.
struct Scope {
    std::wstring task_name;
    std::wstring run_key;
    std::wstring approval_key;
};
Result ApplyInScope(const Request& request, const Scope& scope) noexcept;
}
#endif
} // namespace WatchmanStartup
