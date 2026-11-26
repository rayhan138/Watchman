#include "startup_registration.h"

#include <windows.h>
#include <taskschd.h>
#include <sddl.h>
#include <wrl/client.h>
#include "scoped_handle.h"
#include <cstring>
#include <cwchar>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace WatchmanStartup {
namespace {
using Microsoft::WRL::ComPtr;
constexpr const wchar_t* task_source = L"app.blinkeye.watchman.startup.v1";
constexpr const wchar_t* task_prefix = L"Watchman Sign-in ";
constexpr const wchar_t* run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* approval_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

struct Failure { HRESULT code; };
void Check(HRESULT code) { if (FAILED(code)) throw Failure{code}; }
void CheckWin32(LONG code) { if (code != ERROR_SUCCESS) throw Failure{HRESULT_FROM_WIN32(code)}; }
template<class Function> uint32_t Guard(Function&& function) noexcept {
    try { function(); return 0; }
    catch (const Failure& failure) { return static_cast<uint32_t>(failure.code); }
    catch (const std::bad_alloc&) { return static_cast<uint32_t>(E_OUTOFMEMORY); }
    catch (...) { return unexpected_error; }
}
class Text {
public:
    Text() = default;
    explicit Text(const wchar_t* text) : Text(std::wstring(text)) {}
    explicit Text(const std::wstring& text) {
        value_ = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (!value_) throw std::bad_alloc();
    }
    ~Text() { SysFreeString(value_); }
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
    BSTR Get() const { return value_; }
    BSTR* Put() { SysFreeString(value_); value_ = nullptr; return &value_; }
    std::wstring String() const { return value_ ? std::wstring(value_, SysStringLen(value_)) : std::wstring(); }
private:
    BSTR value_ = nullptr;
};
VARIANT StringVariant(const Text& text) {
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = text.Get(); // Borrowed only for the duration of a COM call.
    return value;
}
bool Missing(HRESULT error) {
    return error == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || error == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}
bool Equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring CurrentUserSid() {
    Watchman::ScopedHandle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.Put())) CheckWin32(GetLastError());
    DWORD size = 0;
    GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
    if (!size) CheckWin32(GetLastError());
    std::vector<BYTE> bytes(size);
    if (!GetTokenInformation(token.Get(), TokenUser, bytes.data(), size, &size)) CheckWin32(GetLastError());
    const PSID user = reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid;
    // This is an interactive user's preference, never service-account/SYSTEM
    // persistence even if somebody starts Watchman from such a context.
    if (IsWellKnownSid(user, WinLocalSystemSid) || IsWellKnownSid(user, WinLocalServiceSid) ||
        IsWellKnownSid(user, WinNetworkServiceSid)) Check(E_ACCESSDENIED);
    LPWSTR raw = nullptr;
    if (!ConvertSidToStringSidW(user, &raw)) CheckWin32(GetLastError());
    const std::unique_ptr<wchar_t, decltype(&LocalFree)> sid(raw, LocalFree);
    return std::wstring(sid.get());
}
bool SameUser(const std::wstring& user, const std::wstring& sid) {
    if (Equal(user, sid)) return true;
    if (user.empty()) return false;
    // Some Scheduler versions return the resolved account name rather than the
    // SID supplied at registration. Compare identities, never an account suffix.
    DWORD sid_size = 0, domain_size = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, user.c_str(), nullptr, &sid_size, nullptr, &domain_size, &use);
    if (!sid_size) return false;
    std::vector<BYTE> bytes(sid_size);
    std::vector<wchar_t> domain(domain_size);
    if (!LookupAccountNameW(nullptr, user.c_str(), bytes.data(), &sid_size,
                            domain.data(), &domain_size, &use)) return false;
    LPWSTR raw = nullptr;
    if (!ConvertSidToStringSidW(bytes.data(), &raw)) return false;
    const std::unique_ptr<wchar_t, decltype(&LocalFree)> resolved(raw, LocalFree);
    return Equal(resolved.get(), sid);
}

struct RawValue {
    bool present = false;
    DWORD type = REG_NONE;
    std::vector<BYTE> bytes;
    bool operator==(const RawValue& other) const {
        return present == other.present && type == other.type && bytes == other.bytes;
    }
};
RawValue QueryValue(HKEY key) {
    RawValue result;
    DWORD size = 0;
    const LONG queried = RegQueryValueExW(key, L"Watchman", nullptr, &result.type, nullptr, &size);
    if (queried == ERROR_FILE_NOT_FOUND) { result.type = REG_NONE; return result; }
    CheckWin32(queried);
    if (size > 65536) CheckWin32(ERROR_INVALID_DATA);
    result.bytes.resize(size);
    CheckWin32(RegQueryValueExW(key, L"Watchman", nullptr, &result.type, result.bytes.data(), &size));
    result.bytes.resize(size);
    result.present = true;
    return result;
}
RawValue QueryValue(const std::wstring& path) {
    Watchman::ScopedHKey key;
    const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, key.Put());
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return {};
    CheckWin32(opened);
    return QueryValue(key.Get());
}
bool LegacyCommandIsOwned(const RawValue& value, const std::wstring& expected_executable) {
    if ((value.type != REG_SZ && value.type != REG_EXPAND_SZ) || value.bytes.size() < sizeof(wchar_t) ||
        value.bytes.size() % sizeof(wchar_t) != 0) return false;
    std::wstring command(value.bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(command.data(), value.bytes.data(), value.bytes.size());
    if (command.back() != L'\0') return false;
    command.pop_back();
    if (command.find(L'\0') != std::wstring::npos) return false;
    if (value.type == REG_EXPAND_SZ) {
        const DWORD needed = ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
        if (!needed || needed > 32768) return false;
        std::vector<wchar_t> expanded(needed);
        if (ExpandEnvironmentStringsW(command.c_str(), expanded.data(), needed) != needed) return false;
        command = expanded.data();
    }
    // Match the exact installed executable, not merely a basename. Tauri v1
    // wrote paths without quotes even when Program Files contained spaces, so
    // CommandLineToArgvW would split that known legacy value incorrectly.
    std::wstring executable;
    if (!ExtractLegacyStartupExecutable(command, executable) ||
        !ValidExecutablePath(executable) || !ValidExecutablePath(expected_executable) ||
        !Equal(executable, expected_executable)) return false;
    const auto slash = executable.find_last_of(L'\\');
    return slash != std::wstring::npos && Equal(executable.substr(slash + 1), L"Watchman.exe");
}
bool ApprovalDisallowsAutomaticMigration(const RawValue& value) {
    if (!value.present) return false;
    // Windows uses 12-byte StartupApproved records (2/6 enabled, 3/7
    // disabled). An unrecognized record is conservative: leave it alone and
    // require an explicit saved off/on change, rather than bypassing a choice.
    if (value.type != REG_BINARY || value.bytes.size() != 12) return true;
    DWORD state = 0;
    std::memcpy(&state, value.bytes.data(), sizeof(state));
    return state != 2 && state != 6;
}

struct Configuration {
    std::wstring sid;
    std::wstring name;
    std::wstring legacy_key;
    std::wstring approval;
    std::wstring source;
    std::wstring executable;
};

class WindowsBackend final : public Backend {
public:
    WindowsBackend(Configuration configuration, const Request& request)
        : config_(std::move(configuration)), request_(request) {}

    uint32_t ReadLegacy(LegacyState& state) override {
        return Guard([&] {
            legacy_ = QueryValue(config_.legacy_key);
            state.present = legacy_.present;
            state.owned = legacy_.present && LegacyCommandIsOwned(legacy_, config_.executable);
            if (state.owned && request_.enabled && !request_.explicit_change)
                state.externally_disabled = ApprovalDisallowsAutomaticMigration(QueryValue(config_.approval));
        });
    }
    uint32_t ReadTask(TaskState& state) override {
        return Guard([&] { InspectTask(state); });
    }
    uint32_t RegisterTask() override {
        return Guard([&] {
            Connect();
            CheckUnchangedTask();
            ComPtr<ITaskDefinition> definition;
            Check(service_->NewTask(0, definition.GetAddressOf()));
            ComPtr<IRegistrationInfo> information;
            Check(definition->get_RegistrationInfo(information.GetAddressOf()));
            Check(information->put_Author(Text(L"Watchman").Get()));
            Check(information->put_Source(Text(config_.source).Get()));
            Check(information->put_URI(Text(L"\\" + config_.name).Get()));
            Check(information->put_Description(Text(L"Start Watchman quietly at this user's sign-in, with normal privileges and no added trigger delay. Managed by Watchman's startup toggle.").Get()));

            ComPtr<IPrincipal> principal;
            Check(definition->get_Principal(principal.GetAddressOf()));
            Check(principal->put_Id(Text(L"WatchmanUser").Get()));
            Check(principal->put_UserId(Text(config_.sid).Get()));
            Check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
            Check(principal->put_RunLevel(TASK_RUNLEVEL_LUA));

            ComPtr<ITaskSettings> settings;
            Check(definition->get_Settings(settings.GetAddressOf()));
            Check(settings->put_Compatibility(TASK_COMPATIBILITY_V2));
            Check(settings->put_Enabled(VARIANT_TRUE));
            Check(settings->put_Hidden(VARIANT_FALSE));
            Check(settings->put_AllowDemandStart(VARIANT_TRUE));
            Check(settings->put_AllowHardTerminate(VARIANT_FALSE));
            Check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
            Check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
            Check(settings->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE));
            Check(settings->put_RunOnlyIfIdle(VARIANT_FALSE));
            Check(settings->put_StartWhenAvailable(VARIANT_FALSE));
            Check(settings->put_WakeToRun(VARIANT_FALSE));
            Check(settings->put_ExecutionTimeLimit(Text(L"PT0S").Get()));
            Check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
            Check(settings->put_Priority(6)); // Normal priority, not elevated.
            ComPtr<IIdleSettings> idle;
            Check(settings->get_IdleSettings(idle.GetAddressOf()));
            Check(idle->put_StopOnIdleEnd(VARIANT_FALSE));
            Check(idle->put_RestartOnIdle(VARIANT_FALSE));

            ComPtr<ITriggerCollection> triggers;
            ComPtr<ITrigger> trigger;
            ComPtr<ILogonTrigger> logon;
            Check(definition->get_Triggers(triggers.GetAddressOf()));
            Check(triggers->Create(TASK_TRIGGER_LOGON, trigger.GetAddressOf()));
            Check(trigger.As(&logon));
            Check(logon->put_Id(Text(L"WatchmanSignIn").Get()));
            Check(logon->put_Enabled(VARIANT_TRUE));
            Check(logon->put_UserId(Text(config_.sid).Get()));
            Check(logon->put_Delay(Text(L"PT0S").Get()));

            ComPtr<IActionCollection> actions;
            ComPtr<IAction> action;
            ComPtr<IExecAction> executable;
            Check(definition->get_Actions(actions.GetAddressOf()));
            Check(actions->put_Context(Text(L"WatchmanUser").Get()));
            Check(actions->Create(TASK_ACTION_EXEC, action.GetAddressOf()));
            Check(action.As(&executable));
            // Task Scheduler has separate path/argument fields: no command
            // shell and no quotes surrounding the executable Path property.
            Check(executable->put_Path(Text(config_.executable).Get()));
            Check(executable->put_Arguments(Text(L"--startup").Get()));
            Check(executable->put_WorkingDirectory(Text(WorkingDirectory(config_.executable)).Get()));

            const Text user(config_.sid);
            // Explicit current-user access also permits normal launches to
            // manage a task first installed from an elevated Watchman instance.
            const Text security(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + config_.sid + L")");
            ComPtr<IRegisteredTask> registered;
            Check(folder_->RegisterTaskDefinition(Text(config_.name).Get(), definition.Get(),
                (original_present_ ? TASK_UPDATE : TASK_CREATE) | TASK_IGNORE_REGISTRATION_TRIGGERS,
                StringVariant(user), VARIANT{}, TASK_LOGON_INTERACTIVE_TOKEN, StringVariant(security), registered.GetAddressOf()));
        });
    }
    uint32_t RollbackTask() override {
        if (!original_present_) return RemoveTask();
        return Guard([&] {
            TaskState current;
            InspectTask(current, false);
            if (current.present && !current.owned) Check(static_cast<HRESULT>(conflict_error));
            const Text user(config_.sid), security(original_security_);
            ComPtr<IRegisteredTask> restored;
            Check(folder_->RegisterTask(Text(config_.name).Get(), Text(original_xml_).Get(),
                TASK_CREATE_OR_UPDATE | TASK_IGNORE_REGISTRATION_TRIGGERS, StringVariant(user), VARIANT{},
                TASK_LOGON_INTERACTIVE_TOKEN, StringVariant(security), restored.GetAddressOf()));
        });
    }
    uint32_t RemoveLegacy() override {
        return Guard([&] {
            Watchman::ScopedHKey key;
            const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER, config_.legacy_key.c_str(), 0,
                                              KEY_QUERY_VALUE | KEY_SET_VALUE, key.Put());
            if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return;
            CheckWin32(opened);
            const auto current = QueryValue(key.Get());
            if (!current.present) return;
            if (!(current == legacy_) || !LegacyCommandIsOwned(current, config_.executable)) CheckWin32(ERROR_RETRY);
            const LONG removed = RegDeleteValueW(key.Get(), L"Watchman");
            if (removed != ERROR_FILE_NOT_FOUND) CheckWin32(removed);
        });
    }
    uint32_t RemoveTask() override {
        return Guard([&] {
            TaskState current;
            InspectTask(current, false);
            if (!current.present) return;
            if (!current.owned) Check(static_cast<HRESULT>(conflict_error));
            const HRESULT removed = folder_->DeleteTask(Text(config_.name).Get(), 0);
            if (!Missing(removed)) Check(removed);
            // Deleting a registration never calls IRegisteredTask::Stop and
            // must not terminate the running Watchman or its history workers.
        });
    }
private:
    void Connect() {
        if (folder_) return;
        Check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(service_.ReleaseAndGetAddressOf())));
        // Local current-token authentication only. No credentials, alternate
        // account, highest-privilege context or global COM-security override.
        Check(service_->Connect(VARIANT{}, VARIANT{}, VARIANT{}, VARIANT{}));
        Check(service_->GetFolder(Text(L"\\").Get(), folder_.ReleaseAndGetAddressOf()));
    }
    ComPtr<IRegisteredTask> FindTask() {
        Connect();
        ComPtr<IRegisteredTask> task;
        const HRESULT found = folder_->GetTask(Text(config_.name).Get(), task.GetAddressOf());
        if (!Missing(found)) Check(found);
        return task;
    }
    bool Owned(ITaskDefinition* definition) {
        ComPtr<IRegistrationInfo> information;
        Check(definition->get_RegistrationInfo(information.GetAddressOf()));
        Text source;
        Check(information->get_Source(source.Put()));
        if (source.String() != config_.source) return false;
        ComPtr<IPrincipal> principal;
        Check(definition->get_Principal(principal.GetAddressOf()));
        Text user;
        Check(principal->get_UserId(user.Put()));
        return SameUser(user.String(), config_.sid);
    }
    bool Matches(ITaskDefinition* definition) {
        ComPtr<IPrincipal> principal;
        Check(definition->get_Principal(principal.GetAddressOf()));
        TASK_RUNLEVEL_TYPE level{};
        TASK_LOGON_TYPE logon_type{};
        Check(principal->get_RunLevel(&level));
        Check(principal->get_LogonType(&logon_type));
        if (level != TASK_RUNLEVEL_LUA || logon_type != TASK_LOGON_INTERACTIVE_TOKEN) return false;
        Text group;
        Check(principal->get_GroupId(group.Put()));
        if (!group.String().empty()) return false;

        ComPtr<ITaskSettings> settings;
        Check(definition->get_Settings(settings.GetAddressOf()));
        VARIANT_BOOL value = VARIANT_FALSE;
        Check(settings->get_Enabled(&value)); if (value != VARIANT_TRUE) return false;
        Check(settings->get_Hidden(&value)); if (value != VARIANT_FALSE) return false;
        Check(settings->get_AllowDemandStart(&value)); if (value != VARIANT_TRUE) return false;
        Check(settings->get_AllowHardTerminate(&value)); if (value != VARIANT_FALSE) return false;
        Check(settings->get_DisallowStartIfOnBatteries(&value)); if (value != VARIANT_FALSE) return false;
        Check(settings->get_StopIfGoingOnBatteries(&value)); if (value != VARIANT_FALSE) return false;
        Check(settings->get_RunOnlyIfNetworkAvailable(&value)); if (value != VARIANT_FALSE) return false;
        Check(settings->get_RunOnlyIfIdle(&value)); if (value != VARIANT_FALSE) return false;
        Check(settings->get_StartWhenAvailable(&value)); if (value != VARIANT_FALSE) return false;
        Check(settings->get_WakeToRun(&value)); if (value != VARIANT_FALSE) return false;
        TASK_INSTANCES_POLICY multiple{};
        Check(settings->get_MultipleInstances(&multiple)); if (multiple != TASK_INSTANCES_IGNORE_NEW) return false;
        int priority = 0;
        Check(settings->get_Priority(&priority)); if (priority != 6) return false;
        Text limit;
        Check(settings->get_ExecutionTimeLimit(limit.Put())); if (limit.String() != L"PT0S") return false;
        ComPtr<IIdleSettings> idle;
        Check(settings->get_IdleSettings(idle.GetAddressOf()));
        Check(idle->get_StopOnIdleEnd(&value)); if (value != VARIANT_FALSE) return false;
        Check(idle->get_RestartOnIdle(&value)); if (value != VARIANT_FALSE) return false;

        ComPtr<ITriggerCollection> triggers;
        Check(definition->get_Triggers(triggers.GetAddressOf()));
        LONG count = 0;
        Check(triggers->get_Count(&count)); if (count != 1) return false;
        ComPtr<ITrigger> trigger;
        Check(triggers->get_Item(1, trigger.GetAddressOf()));
        TASK_TRIGGER_TYPE2 type{};
        Check(trigger->get_Type(&type)); if (type != TASK_TRIGGER_LOGON) return false;
        Check(trigger->get_Enabled(&value)); if (value != VARIANT_TRUE) return false;
        Text boundary;
        Check(trigger->get_StartBoundary(boundary.Put())); if (!boundary.String().empty()) return false;
        Check(trigger->get_EndBoundary(boundary.Put())); if (!boundary.String().empty()) return false;
        ComPtr<IRepetitionPattern> repetition;
        Check(trigger->get_Repetition(repetition.GetAddressOf()));
        Text interval, duration;
        Check(repetition->get_Interval(interval.Put()));
        Check(repetition->get_Duration(duration.Put()));
        if ((!interval.String().empty() && interval.String() != L"PT0S") ||
            (!duration.String().empty() && duration.String() != L"PT0S")) return false;
        ComPtr<ILogonTrigger> logon;
        Check(trigger.As(&logon));
        Text user, delay;
        Check(logon->get_UserId(user.Put())); if (!SameUser(user.String(), config_.sid)) return false;
        Check(logon->get_Delay(delay.Put())); if (!delay.String().empty() && delay.String() != L"PT0S") return false;

        ComPtr<IActionCollection> actions;
        Check(definition->get_Actions(actions.GetAddressOf()));
        Check(actions->get_Count(&count)); if (count != 1) return false;
        ComPtr<IAction> action;
        Check(actions->get_Item(1, action.GetAddressOf()));
        TASK_ACTION_TYPE action_type{};
        Check(action->get_Type(&action_type)); if (action_type != TASK_ACTION_EXEC) return false;
        ComPtr<IExecAction> executable;
        Check(action.As(&executable));
        Text path, arguments, directory;
        Check(executable->get_Path(path.Put()));
        Check(executable->get_Arguments(arguments.Put()));
        Check(executable->get_WorkingDirectory(directory.Put()));
        return Equal(path.String(), config_.executable) && arguments.String() == L"--startup" &&
            Equal(directory.String(), WorkingDirectory(config_.executable));
    }
    void InspectTask(TaskState& state, bool check_configuration = true) {
        state = {};
        const auto task = FindTask();
        if (task) {
            state.present = true;
            ComPtr<ITaskDefinition> definition;
            Check(task->get_Definition(definition.GetAddressOf()));
            state.owned = Owned(definition.Get());
            VARIANT_BOOL enabled = VARIANT_FALSE;
            Check(task->get_Enabled(&enabled));
            state.enabled = enabled == VARIANT_TRUE;
            if (state.owned && request_.enabled && check_configuration) state.matches = Matches(definition.Get());
            if (!snapshot_taken_ && state.owned) {
                Text xml, security;
                Check(task->get_Xml(xml.Put()));
                Check(task->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, security.Put()));
                original_xml_ = xml.String();
                original_security_ = security.String();
            }
        }
        if (!snapshot_taken_) {
            original_present_ = state.present;
            snapshot_taken_ = true;
        }
    }
    void CheckUnchangedTask() {
        const auto task = FindTask();
        if (!original_present_) {
            if (task) Check(static_cast<HRESULT>(conflict_error));
        } else {
            if (!task) CheckWin32(ERROR_RETRY);
            Text xml;
            Check(task->get_Xml(xml.Put()));
            if (xml.String() != original_xml_) CheckWin32(ERROR_RETRY);
        }
    }
    Configuration config_;
    Request request_;
    RawValue legacy_;
    ComPtr<ITaskService> service_;
    ComPtr<ITaskFolder> folder_;
    bool snapshot_taken_ = false;
    bool original_present_ = false;
    std::wstring original_xml_;
    std::wstring original_security_;
};

Result ApplyWithConfiguration(Request request, Configuration configuration) noexcept {
    Result result;
    result.sequence = request.sequence;
    result.requested_enabled = request.enabled;
    result.explicit_change = request.explicit_change;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { result.error = static_cast<uint32_t>(initialized); return result; }
    {
        const auto error = Guard([&] {
            configuration.sid = CurrentUserSid();
            if (configuration.name.empty()) configuration.name = task_prefix + configuration.sid;
            if (request.executable.empty()) {
                std::vector<wchar_t> path(32768);
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (!length) CheckWin32(GetLastError());
                if (length >= path.size()) CheckWin32(ERROR_INSUFFICIENT_BUFFER);
                request.executable.assign(path.data(), length);
            }
            configuration.executable = request.executable;
            WindowsBackend backend(std::move(configuration), request);
            result = Apply(backend, request);
        });
        if (error) { result.status = Status::Failed; result.error = error; }
    } // Release all COM interfaces before balancing this apartment.
    CoUninitialize();
    return result;
}
} // namespace

Result ApplyForCurrentUser(const Request& request) noexcept {
    // This can allocate before the guarded implementation is entered.
    try { return ApplyWithConfiguration(request, {{}, {}, run_key, approval_key, task_source, {}}); }
    catch (...) {
        Result result;
        result.sequence = request.sequence;
        result.requested_enabled = request.enabled;
        result.explicit_change = request.explicit_change;
        result.error = unexpected_error;
        return result;
    }
}

#ifdef WATCHMAN_STARTUP_REGISTRATION_TESTING
namespace Testing {
Result ApplyInScope(const Request& request, const Scope& scope) noexcept {
    try {
        // Refuse production locations even in a test binary.
        if (scope.task_name.rfind(L"Watchman Startup Test ", 0) != 0 ||
            scope.run_key.rfind(L"Software\\Watchman.Tests\\", 0) != 0 ||
            scope.approval_key.rfind(L"Software\\Watchman.Tests\\", 0) != 0) {
            Result result;
            result.error = invalid_argument_error;
            return result;
        }
        return ApplyWithConfiguration(request, {{}, scope.task_name, scope.run_key, scope.approval_key,
                                               L"app.blinkeye.watchman.startup.test.v1", {}});
    } catch (...) { Result result; result.error = unexpected_error; return result; }
}
} // namespace Testing
#endif
} // namespace WatchmanStartup
