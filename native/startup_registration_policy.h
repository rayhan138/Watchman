#pragma once

#include <cstdint>
#include <string>

// Platform-independent ordering/safety policy. The only legacy operation is
// removal: normal launches must never rewrite a Windows Run value.
namespace WatchmanStartup {
inline constexpr uint32_t invalid_argument_error = 0x80070057u;
inline constexpr uint32_t conflict_error = 0x800700B7u;
inline constexpr uint32_t invalid_configuration_error = 0x8007000Du;
inline constexpr uint32_t unexpected_error = 0x80004005u;

enum class Status { TaskReady, Disabled, ExternallyDisabled, Failed };
enum class Operation { None, ReadLegacy, ReadTask, RegisterTask, VerifyTask, RemoveLegacy, RemoveTask };

struct Request {
    uint64_t sequence = 0;
    bool enabled = false;
    // Only a saved off -> on change may re-enable externally disabled startup.
    bool explicit_change = false;
    std::wstring executable;
};
struct Result {
    uint64_t sequence = 0;
    bool requested_enabled = false;
    bool explicit_change = false;
    Status status = Status::Failed;
    Operation operation = Operation::None;
    uint32_t error = 0; // HRESULT, including HRESULT_FROM_WIN32 errors.
    uint32_t rollback_error = 0;
    bool changed = false;
    bool legacy_present = false;
    bool task_present = false;
};
struct LegacyState {
    bool present = false;
    bool owned = false;
    bool externally_disabled = false;
};
struct TaskState {
    bool present = false;
    bool owned = false;
    bool enabled = false;
    bool matches = false;
};

inline bool ValidExecutablePath(const std::wstring& path) {
    if (path.size() < 4 || path.size() >= 32768) return false;
    for (const auto character : path)
        if (character < L' ' || character == L'"') return false;
    const bool drive = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
    const auto server_end = path.find(L'\\', 2);
    const auto share_end = server_end == std::wstring::npos ? std::wstring::npos : path.find(L'\\', server_end + 1);
    const bool unc = path[0] == L'\\' && path[1] == L'\\' && server_end != std::wstring::npos &&
        server_end > 2 && share_end != std::wstring::npos && share_end > server_end + 1;
    const auto slash = path.find_last_of(L'\\');
    return (drive || unc) && slash != std::wstring::npos && slash + 1 < path.size();
}
inline std::wstring WorkingDirectory(const std::wstring& path) {
    if (!ValidExecutablePath(path)) return {};
    const auto slash = path.find_last_of(L'\\');
    return path.substr(0, slash == 2 ? 3 : slash);
}

class Backend {
public:
    virtual ~Backend() = default;
    virtual uint32_t ReadLegacy(LegacyState&) = 0;
    // The first read retains an original definition for RollbackTask. Later
    // verification reads must not replace that snapshot.
    virtual uint32_t ReadTask(TaskState&) = 0;
    virtual uint32_t RegisterTask() = 0;
    virtual uint32_t RollbackTask() = 0;
    virtual uint32_t RemoveLegacy() = 0;
    virtual uint32_t RemoveTask() = 0;
};

inline Result Apply(Backend& backend, const Request& request) {
    Result result;
    result.sequence = request.sequence;
    result.requested_enabled = request.enabled;
    result.explicit_change = request.explicit_change;
    const auto fail = [&](Operation operation, uint32_t error) {
        // Disable attempts both independent removals; retain the first failure.
        if (!result.error) { result.operation = operation; result.error = error; }
        result.status = Status::Failed;
    };
    if (request.enabled && !ValidExecutablePath(request.executable)) {
        fail(Operation::RegisterTask, invalid_argument_error);
        return result;
    }

    LegacyState legacy;
    const auto legacy_error = backend.ReadLegacy(legacy);
    result.legacy_present = legacy.present;
    if (legacy_error) fail(Operation::ReadLegacy, legacy_error);
    else if (legacy.present && !legacy.owned) fail(Operation::ReadLegacy, conflict_error);
    // Never change startup on an uncertain enable/migration.
    if (request.enabled && result.error) return result;

    TaskState task;
    const auto task_error = backend.ReadTask(task);
    result.task_present = task.present;
    if (task_error) fail(Operation::ReadTask, task_error);
    else if (task.present && !task.owned) fail(Operation::ReadTask, conflict_error);
    if (request.enabled && result.error) return result;

    if (!request.enabled) {
        // An off preference must never re-enable something to roll it back.
        // Remove every known-owned entry even if the other service is denied.
        if (!task_error && task.present && task.owned) {
            if (const auto error = backend.RemoveTask()) fail(Operation::RemoveTask, error);
            else { result.task_present = false; result.changed = true; }
        }
        if (!legacy_error && legacy.present && legacy.owned) {
            if (const auto error = backend.RemoveLegacy()) fail(Operation::RemoveLegacy, error);
            else { result.legacy_present = false; result.changed = true; }
        }
        if (!result.error) result.status = Status::Disabled;
        return result;
    }

    // A disabled owned task is authoritative once installed. Remove a stale
    // legacy launch route rather than let it bypass that newer Off choice.
    if (!request.explicit_change && task.present && !task.enabled) {
        if (legacy.present) {
            if (const auto error = backend.RemoveLegacy()) {
                fail(Operation::RemoveLegacy, error);
                return result;
            }
            result.legacy_present = false;
            result.changed = true;
        }
        result.status = Status::ExternallyDisabled;
        result.operation = Operation::ReadTask;
        return result;
    }
    // Before initial migration, a Windows-disabled legacy item must not be
    // replaced with an enabled task. A stale approval record is irrelevant
    // once an already-enabled owned task is the selected launch mechanism.
    if (!request.explicit_change && !task.present && legacy.present && legacy.externally_disabled) {
        result.status = Status::ExternallyDisabled;
        result.operation = Operation::ReadLegacy;
        return result;
    }

    const bool update = !task.present || !task.enabled || !task.matches;
    if (update) {
        if (const auto error = backend.RegisterTask()) {
            fail(Operation::RegisterTask, error);
            return result; // In particular, keep the original Run command.
        }
        result.changed = true;
        TaskState verified;
        const auto error = backend.ReadTask(verified);
        if (error || !verified.present || !verified.owned || !verified.enabled || !verified.matches) {
            fail(Operation::VerifyTask, error ? error : invalid_configuration_error);
            result.rollback_error = backend.RollbackTask();
            result.task_present = result.rollback_error ? true : task.present;
            return result;
        }
        result.task_present = true;
    }

    // Do not delete the old launch mechanism until the task was read back and
    // verified. If cleanup fails after an update, restore the previous task (or
    // remove only the task just created) and leave the old Run value intact.
    if (legacy.present) {
        if (const auto error = backend.RemoveLegacy()) {
            fail(Operation::RemoveLegacy, error);
            if (update) {
                result.rollback_error = backend.RollbackTask();
                result.task_present = result.rollback_error ? true : task.present;
            }
            return result;
        }
        result.legacy_present = false;
        result.changed = true;
    }
    result.status = Status::TaskReady;
    return result;
}

inline const char* EventName(Status status) {
    switch (status) {
    case Status::TaskReady: return "task-ready";
    case Status::Disabled: return "disabled";
    case Status::ExternallyDisabled: return "externally-disabled";
    default: return "registration-failed";
    }
}
inline const wchar_t* OperationDescription(Operation operation) {
    switch (operation) {
    case Operation::ReadLegacy: return L"inspect the previous Run entry/Windows approval";
    case Operation::ReadTask: return L"inspect the sign-in task";
    case Operation::RegisterTask: return L"register the sign-in task";
    case Operation::VerifyTask: return L"verify the registered task";
    case Operation::RemoveLegacy: return L"remove the previous Run entry";
    case Operation::RemoveTask: return L"remove the sign-in task";
    default: return L"apply startup preferences";
    }
}
} // namespace WatchmanStartup
