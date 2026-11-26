#include "startup_registration.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace WatchmanStartup;
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Fake final : Backend {
    LegacyState legacy{};
    TaskState task{}, original{};
    std::vector<std::string> calls;
    uint32_t legacy_error = 0, task_error = 0, register_error = 0, verify_error = 0;
    uint32_t remove_legacy_error = 0, remove_task_error = 0, rollback_error = 0;
    bool verify_mismatch = false;
    unsigned reads = 0;
    uint32_t ReadLegacy(LegacyState& state) override {
        calls.push_back("read-legacy"); state = legacy; return legacy_error;
    }
    uint32_t ReadTask(TaskState& state) override {
        calls.push_back("read-task");
        if (++reads == 1) { original = task; state = task; return task_error; }
        state = task;
        if (verify_mismatch) state.matches = false;
        return verify_error;
    }
    uint32_t RegisterTask() override {
        calls.push_back("register");
        if (register_error) return register_error;
        task = {true, true, true, true};
        return 0;
    }
    uint32_t RollbackTask() override {
        calls.push_back("rollback");
        if (rollback_error) return rollback_error;
        task = original;
        return 0;
    }
    uint32_t RemoveLegacy() override {
        calls.push_back("remove-legacy");
        if (remove_legacy_error) return remove_legacy_error;
        legacy.present = false;
        return 0;
    }
    uint32_t RemoveTask() override {
        calls.push_back("remove-task");
        if (remove_task_error) return remove_task_error;
        task.present = false;
        return 0;
    }
};
const Request enabled{42, true, false, L"E:\\Watchman TEST 02\\11\\Watchman-win64\\dist\\Watchman.exe"};
Request Disabled() { auto request = enabled; request.enabled = false; request.explicit_change = true; return request; }

void Policy() {
    for (const auto* path : {L"", L"Watchman.exe", L"C:Watchman.exe", L"C:\\", L"\"C:\\Watchman.exe\"", L"C:\\bad\nname.exe", L"\\\\server\\share.exe", L"\\\\server\\\\Watchman.exe"}) {
        Check(!ValidExecutablePath(path), "Invalid/relative/quoted task executable accepted");
        auto request = enabled; request.executable = path;
        Fake fake;
        const auto result = Apply(fake, request);
        Check(result.error == invalid_argument_error && fake.calls.empty(), "Invalid paths must fail before touching startup");
    }
    Check(!ValidExecutablePath(std::wstring(L"C:\\a\0b.exe", 10)), "Embedded NUL must not truncate a task path");
    for (const auto* path : {L"C:\\Watchman.exe", L"\\\\server\\share\\Watchman.exe", L"C:\\spaced & \u09AC\u09BE\u0982\u09B2\u09BE\\Watchman.exe"})
        Check(ValidExecutablePath(path), "An absolute spaced/UNC/Unicode path must be supported");
    Check(WorkingDirectory(L"C:\\Watchman.exe") == L"C:\\", "Root working directory lost its separator");
    Check(WorkingDirectory(enabled.executable) == L"E:\\Watchman TEST 02\\11\\Watchman-win64\\dist", "Working directory must match the portable EXE");
    Check(WorkingDirectory(L"relative.exe").empty(), "Relative working directory accepted");
    {
        Fake fake; fake.legacy = {true, true, false};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::TaskReady && !result.error && result.changed, "Legacy migration did not succeed");
        Check(!fake.legacy.present && fake.task.present && result.task_present && !result.legacy_present, "Migration left duplicate/missing registrations");
        Check(fake.calls == std::vector<std::string>{"read-legacy", "read-task", "register", "read-task", "remove-legacy"}, "Run removal must follow successful task registration AND verification");
        Check(result.sequence == 42 && result.requested_enabled && !result.explicit_change, "Request identity lost");
    }
    {
        Fake fake; fake.task = {true, true, true, true};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::TaskReady && !result.changed, "Matching tasks must be a read-only no-op");
        Check(fake.calls == std::vector<std::string>{"read-legacy", "read-task"}, "Normal launch rewrote a matching task");
    }
    {
        Fake fake; fake.task = {true, true, true, true}; fake.legacy = {true, true, false};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::TaskReady && result.changed && fake.task.present && !fake.legacy.present, "An already-verified task must allow stale legacy cleanup");
        Check(fake.calls.back() == "remove-legacy" && fake.calls.size() == 3, "Legacy cleanup needlessly re-registered the task");
    }
    for (bool disabled_legacy : {false, true}) {
        Fake fake;
        if (disabled_legacy) fake.legacy = {true, true, true};
        else fake.task = {true, true, false, false};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::ExternallyDisabled && !result.error && !result.changed, "Automatic launch bypassed external disable");
        Check(fake.calls.size() == 2, "External disable caused a registration write");
        auto explicit_enable = enabled; explicit_enable.explicit_change = true;
        const auto saved = Apply(fake, explicit_enable);
        Check(saved.status == Status::TaskReady && fake.task.enabled && !fake.legacy.present, "An explicit saved On must deliberately enable the task");
    }
    {
        Fake fake; fake.task = {true, true, false, false}; fake.legacy = {true, true, false};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::ExternallyDisabled && !fake.task.enabled && !fake.legacy.present && result.changed,
            "A stale Run entry must not bypass an externally disabled owned task");
        Check(fake.calls.back() == "remove-legacy" && fake.calls.size() == 3, "Honoring task disable re-registered the task");
    }
    {
        Fake fake; fake.task = {true, true, true, true}; fake.legacy = {true, true, true};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::TaskReady && !fake.legacy.present && fake.task.enabled,
            "Stale Run approval must not override an already-enabled owned task");
        Check(fake.calls.size() == 3, "Stale legacy cleanup rewrote an already-correct task");
    }
    {
        Fake fake; fake.task = {true, true, true, false};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::TaskReady && result.changed && fake.task.matches, "A moved EXE must update only its owned task");
        Check(fake.calls == std::vector<std::string>{"read-legacy", "read-task", "register", "read-task"}, "Path update did not verify its result");
    }
    for (unsigned stage = 0; stage != 3; ++stage) {
        Fake fake; fake.legacy = {true, true, false};
        if (stage == 0) fake.legacy_error = 5;
        if (stage == 1) fake.task_error = 6;
        if (stage == 2) fake.register_error = 7;
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::Failed && result.error == 5 + stage, "Enable failure was not retained");
        Check(fake.legacy.present && !fake.task.present && !result.changed, "Failed enable destroyed the original Run startup");
    }
    for (bool verification_read_fails : {false, true}) {
        Fake fake; fake.legacy = {true, true, false};
        if (verification_read_fails) fake.verify_error = 9;
        else fake.verify_mismatch = true;
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::Failed && result.operation == Operation::VerifyTask, "Unverified task was accepted");
        Check(result.error == (verification_read_fails ? 9u : invalid_configuration_error), "Verification failure was hidden");
        Check(fake.legacy.present && !fake.task.present && !result.rollback_error && !result.task_present, "Failed verification did not roll back the new task and preserve Run");
        Check(fake.calls.back() == "rollback", "Unverified task was left installed");
    }
    for (bool existing_task : {false, true}) {
        Fake fake; fake.legacy = {true, true, false}; fake.remove_legacy_error = 11;
        if (existing_task) fake.task = {true, true, true, false};
        const auto result = Apply(fake, enabled);
        Check(result.status == Status::Failed && result.operation == Operation::RemoveLegacy && result.error == 11, "Run cleanup failure was not reported");
        Check(fake.legacy.present && fake.task.present == existing_task && !result.rollback_error, "Run cleanup failure did not restore the original task state");
        if (existing_task) Check(!fake.task.matches, "Rollback overwrote the old definition with the uncommitted new one");
    }
    {
        Fake fake; fake.legacy = {true, true, false}; fake.verify_error = 12; fake.rollback_error = 13;
        const auto result = Apply(fake, enabled);
        Check(result.error == 12 && result.rollback_error == 13 && result.task_present && fake.legacy.present, "A failed rollback must be explicit, not a false all-or-nothing claim");
    }
    for (bool foreign_legacy : {false, true}) {
        Fake fake;
        if (foreign_legacy) fake.legacy = {true, false, false};
        else fake.task = {true, false, true, false};
        const auto result = Apply(fake, enabled);
        Check(result.error == conflict_error && !result.changed, "Foreign startup registration was overwritten");
        Check(fake.calls.size() == (foreign_legacy ? 1u : 2u), "Foreign registration caused a write");
    }
    {
        Fake fake;
        const auto result = Apply(fake, Disabled());
        Check(result.status == Status::Disabled && !result.changed && !result.error, "Off with nothing installed must be idempotent and read-only");
        Check(fake.calls.size() == 2, "Off created/wrote a missing registry key");
    }
    {
        Fake fake; fake.legacy = {true, true, true}; fake.task = {true, true, false, false};
        const auto result = Apply(fake, Disabled());
        Check(result.status == Status::Disabled && !result.task_present && !result.legacy_present, "Off must remove both disabled/enabled old registrations");
        Check(fake.calls == std::vector<std::string>{"read-legacy", "read-task", "remove-task", "remove-legacy"}, "Disable did not clean both methods without re-enabling either");
    }
    {
        Fake fake; fake.legacy = {true, true, false}; fake.task_error = 15;
        const auto result = Apply(fake, Disabled());
        Check(result.error == 15 && !fake.legacy.present && result.changed, "Unavailable Scheduler must not prevent independent Run removal on Off");
    }
    {
        Fake fake; fake.legacy_error = 16; fake.task = {true, true, true, true};
        const auto result = Apply(fake, Disabled());
        Check(result.error == 16 && !fake.task.present && result.changed, "Denied legacy key must not prevent owned task removal on Off");
    }
    {
        Fake fake; fake.legacy = {true, true, false}; fake.task = {true, true, true, true}; fake.remove_task_error = 17;
        const auto result = Apply(fake, Disabled());
        Check(result.error == 17 && fake.task.present && !fake.legacy.present, "Partial disable must remove what is safe and report the remaining task");
        Check(fake.calls.back() == "remove-legacy", "Disable restored an enabled entry after the user requested Off");
    }
    {
        Fake fake; fake.legacy = {true, false, false}; fake.task = {true, true, true, true};
        const auto result = Apply(fake, Disabled());
        Check(result.error == conflict_error && fake.legacy.present && !fake.task.present, "Off removed a foreign value or failed to remove its owned task");
    }
}

void WorkerOwnership() {
    using namespace std::chrono_literals;
    std::promise<void> release, entered;
    auto gate = release.get_future().share();
    auto started = entered.get_future();
    std::mutex mutex;
    std::vector<uint64_t> executed;
    const auto owner = std::this_thread::get_id();
    std::atomic<bool> different_thread{false};
    Worker worker([&](const Request& request) {
        different_thread = std::this_thread::get_id() != owner;
        { std::lock_guard<std::mutex> lock(mutex); executed.push_back(request.sequence); }
        if (request.sequence == 1) { entered.set_value(); gate.wait_for(5s); }
        Result result; result.status = request.enabled ? Status::TaskReady : Status::Disabled; return result;
    });
    const auto submitted_at = std::chrono::steady_clock::now();
    Check(worker.Submit(true, false, enabled.executable) == 1, "First worker request lost");
    Check(started.wait_for(2s) == std::future_status::ready, "Worker did not own its operation");
    Check(std::chrono::steady_clock::now() - submitted_at < 1s && different_thread, "Startup registration blocked the calling/UI thread");
    Result result;
    const auto read_at = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != 1000; ++i) Check(!worker.TakeResult(result), "Blocked registration invented a ready result");
    Check(std::chrono::steady_clock::now() - read_at < 1s, "Result reads waited behind COM/registration");
    Check(worker.Submit(true, true, enabled.executable) == 2, "Second generation lost");
    Check(worker.Submit(false, true, enabled.executable) == 3, "Newest disable generation lost");
    release.set_value();
    worker.Stop(); // Must drain generation 3, not leave saved Off unprocessed.
    Check(executed == std::vector<uint64_t>{1, 3}, "Worker overlapped operations or failed to coalesce superseded pending requests");
    Check(worker.TakeResult(result) && result.sequence == 3 && !result.requested_enabled && result.explicit_change &&
        result.status == Status::Disabled, "Newest completion/intent lost during joined shutdown");
    Check(!worker.TakeResult(result), "Result mailbox replayed an old completion");
    worker.Stop();
    Check(worker.Submit(true, true) == 0, "Stopped worker accepted an orphan operation");
    {
        Worker failing([](const Request&) -> Result { throw std::runtime_error("Injected registration failure"); });
        failing.Submit(true, true, enabled.executable);
        failing.Stop();
        Check(failing.TakeResult(result) && result.status == Status::Failed && result.error == unexpected_error &&
            result.sequence == 1 && result.requested_enabled, "Worker exception escaped or lost the saved intent");
    }
    { Worker idle([](const Request&) { return Result{}; }); idle.Stop(); }
}
} // namespace

int main() {
    try {
        Policy();
        WorkerOwnership();
        std::cout << "StartupRegistrationPolicy: " << checks << " checks passed (fake backend; no OS startup changes)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "StartupRegistrationPolicy: " << error.what() << '\n';
        return 1;
    }
}
