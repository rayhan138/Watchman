// Real Task Scheduler/Registry integration, confined to unique test task names
// and HKCU\Software\Watchman.Tests. Never launches Watchman or a task action.
#include "startup_registration.h"
#include "scoped_handle.h"
#include <windows.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace WatchmanStartup;
using Microsoft::WRL::ComPtr;
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void Hr(HRESULT value) {
    if (FAILED(value)) { std::cerr << "Task Scheduler HRESULT=0x" << std::hex << static_cast<uint32_t>(value) << std::dec << '\n'; throw std::runtime_error("Native Scheduler API failed"); }
}
struct Text {
    BSTR value = nullptr;
    explicit Text(const std::wstring& text) { value = SysAllocStringLen(text.data(), static_cast<UINT>(text.size())); if (!value) throw std::bad_alloc(); }
    ~Text() { SysFreeString(value); }
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
};
VARIANT StringVariant(const Text& text) { VARIANT value{}; value.vt = VT_BSTR; value.bstrVal = text.value; return value; }
std::wstring Xml(IRegisteredTask* task) {
    BSTR value = nullptr;
    Hr(task->get_Xml(&value));
    const std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}
bool StandardToken() {
    Watchman::ScopedHandle token;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.Put()) != FALSE, "Cannot query test token");
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    Check(GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size) != FALSE, "Cannot query token elevation");
    BYTE administrators[SECURITY_MAX_SID_SIZE]{};
    DWORD sid_size = sizeof(administrators);
    Check(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &sid_size) != FALSE, "Cannot form Administrators SID");
    BOOL member = FALSE;
    Check(CheckTokenMembership(nullptr, administrators, &member) != FALSE, "Cannot query admin membership");
    return !elevation.TokenIsElevated && !member;
}

class Fixture {
public:
    Fixture() {
        const auto id = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        base_ = L"Software\\Watchman.Tests\\Startup-" + id;
        scope = {L"Watchman Startup Test " + id, base_ + L"\\Run", base_ + L"\\Approval"};
        Hr(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service_.GetAddressOf())));
        Hr(service_->Connect(VARIANT{}, VARIANT{}, VARIANT{}, VARIANT{}));
        Hr(service_->GetFolder(Text(L"\\").value, folder_.GetAddressOf()));
        Check(!Task(), "Unique test task already exists; refusing to overwrite it");
        HKEY existing = nullptr;
        const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, base_.c_str(), 0, KEY_READ, &existing);
        if (existing) RegCloseKey(existing);
        Check(opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND, "Unique test registry scope already exists");
        safe_ = true;
    }
    ~Fixture() { Cleanup(); }
    ComPtr<IRegisteredTask> Task() {
        ComPtr<IRegisteredTask> task;
        const HRESULT found = folder_->GetTask(Text(scope.task_name).value, task.GetAddressOf());
        if (found != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) Hr(found);
        return task;
    }
    Result ApplyRequest(bool enabled, bool explicit_change = false,
                        const std::wstring& path = L"C:\\Watchman Startup Test Folder\\Watchman.exe") {
        const auto result = Testing::ApplyInScope({1, enabled, explicit_change, path}, scope);
        if (result.error) std::cout << "Observed result error=0x" << std::hex << result.error << std::dec << ", operation=" << static_cast<unsigned>(result.operation) << '\n';
        return result;
    }
    void WriteLegacy(const std::wstring& command = L"\"C:\\Previous Watchman Folder\\Watchman.exe\" --startup") {
        Write(scope.run_key, L"Watchman", REG_SZ, command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    bool HasLegacy() {
        Watchman::ScopedHKey key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, scope.run_key.c_str(), 0, KEY_QUERY_VALUE, key.Put()) != ERROR_SUCCESS) return false;
        return RegQueryValueExW(key.Get(), L"Watchman", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    }
    void Approval(DWORD state) {
        BYTE record[12]{};
        std::memcpy(record, &state, sizeof(state));
        Write(scope.approval_key, L"Watchman", REG_BINARY, record, sizeof(record));
    }
    DWORD ApprovalState() {
        Watchman::ScopedHKey key;
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, scope.approval_key.c_str(), 0, KEY_QUERY_VALUE, key.Put()) == ERROR_SUCCESS, "Approval record was deleted");
        BYTE record[12]{};
        DWORD size = sizeof(record), type = 0, state = 0;
        Check(RegQueryValueExW(key.Get(), L"Watchman", nullptr, &type, record, &size) == ERROR_SUCCESS && type == REG_BINARY && size == 12, "Approval record was changed/malformed");
        std::memcpy(&state, record, sizeof(state));
        return state;
    }
    void PutForeignSource() {
        const auto task = Task();
        ComPtr<ITaskDefinition> definition;
        Hr(task->get_Definition(definition.GetAddressOf()));
        ComPtr<IRegistrationInfo> information;
        Hr(definition->get_RegistrationInfo(information.GetAddressOf()));
        Hr(information->put_Source(Text(L"not-owned-by-watchman").value));
        ComPtr<IPrincipal> principal;
        Hr(definition->get_Principal(principal.GetAddressOf()));
        BSTR user_id = nullptr;
        Hr(principal->get_UserId(&user_id));
        const std::wstring user(user_id, SysStringLen(user_id));
        SysFreeString(user_id);
        const Text identity(user);
        ComPtr<IRegisteredTask> saved;
        Hr(folder_->RegisterTaskDefinition(Text(scope.task_name).value, definition.Get(),
            TASK_UPDATE | TASK_IGNORE_REGISTRATION_TRIGGERS, StringVariant(identity), VARIANT{},
            TASK_LOGON_INTERACTIVE_TOKEN, VARIANT{}, saved.GetAddressOf()));
    }
    void CleanupChecked() {
        const auto removed = folder_->DeleteTask(Text(scope.task_name).value, 0);
        Check(SUCCEEDED(removed) || removed == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "Cannot clean unique test task");
        const auto registry = RegDeleteTreeW(HKEY_CURRENT_USER, base_.c_str());
        Check(registry == ERROR_SUCCESS || registry == ERROR_FILE_NOT_FOUND, "Cannot clean unique test registry scope");
        safe_ = false;
    }
    Testing::Scope scope;
private:
    static void Write(const std::wstring& path, const wchar_t* name, DWORD type, const void* data, DWORD size) {
        Watchman::ScopedHKey key;
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.Put(), nullptr) == ERROR_SUCCESS, "Cannot create unique test registry key");
        Check(RegSetValueExW(key.Get(), name, 0, type, static_cast<const BYTE*>(data), size) == ERROR_SUCCESS, "Cannot write isolated test value");
    }
    void Cleanup() noexcept {
        if (!safe_) return;
        try { folder_->DeleteTask(Text(scope.task_name).value, 0); } catch (...) {}
        RegDeleteTreeW(HKEY_CURRENT_USER, base_.c_str());
    }
    std::wstring base_;
    ComPtr<ITaskService> service_;
    ComPtr<ITaskFolder> folder_;
    bool safe_ = false;
};

void Exercise() {
    Fixture fixture;
    auto result = fixture.ApplyRequest(false);
    Check(result.status == Status::Disabled && !result.error && !result.changed, "Absent Off must not create a task/Run key");
    Check(!fixture.Task() && !fixture.HasLegacy(), "Off created a registration");

    fixture.WriteLegacy();
    result = fixture.ApplyRequest(true);
    Check(result.status == Status::TaskReady && !result.error && !result.rollback_error && result.changed, "Real current-user migration failed");
    Check(fixture.Task() && !fixture.HasLegacy(), "Migration did not leave exactly one task and no Run entry");
    const auto first_xml = Xml(fixture.Task().Get());
    // LUA is the schema default, so Scheduler may omit <RunLevel> entirely.
    // Test the actual COM principal, not the spelling of an optional XML node.
    ComPtr<ITaskDefinition> registered_definition;
    ComPtr<IPrincipal> registered_principal;
    Hr(fixture.Task()->get_Definition(registered_definition.GetAddressOf()));
    Hr(registered_definition->get_Principal(registered_principal.GetAddressOf()));
    TASK_RUNLEVEL_TYPE level{};
    TASK_LOGON_TYPE logon_type{};
    Hr(registered_principal->get_RunLevel(&level));
    Hr(registered_principal->get_LogonType(&logon_type));
    Check(level == TASK_RUNLEVEL_LUA && logon_type == TASK_LOGON_INTERACTIVE_TOKEN, "Task principal must actually be least-privileged and interactive");
    Check(first_xml.find(L"HighestAvailable") == std::wstring::npos, "Task XML requested highest privileges");
    std::cout << "Verified Scheduler principal: run_level=" << level << ", logon_type=" << logon_type << '\n';
    Check(first_xml.find(L"<Command>C:\\Watchman Startup Test Folder\\Watchman.exe</Command>") != std::wstring::npos &&
          first_xml.find(L"<Arguments>--startup</Arguments>") != std::wstring::npos, "Task did not preserve separate unquoted executable/quiet arguments");
    result = fixture.ApplyRequest(true);
    Check(result.status == Status::TaskReady && !result.changed && !result.error && Xml(fixture.Task().Get()) == first_xml, "Repeated real launch rewrote a matching task");

    result = fixture.ApplyRequest(false, true);
    Check(result.status == Status::Disabled && !result.error && !fixture.Task(), "Could not clear startup fixture before v1 migration case");
    fixture.WriteLegacy(L"\"C:\\Previous Watchman Folder\\Watchman.exe\" --minimized");
    result = fixture.ApplyRequest(true);
    Check(result.status == Status::TaskReady && !result.error && fixture.Task() && !fixture.HasLegacy(),
          "Tauri v1 --minimized Run command was not migrated to the native task");

    result = fixture.ApplyRequest(true, false, L"E:\\Moved & \u09AC\u09BE\u0982\u09B2\u09BE Watchman\\Watchman.exe");
    Check(result.status == Status::TaskReady && result.changed && !result.error, "Moved spaced/Unicode portable path failed to update");
    const auto moved_xml = Xml(fixture.Task().Get());
    Check(moved_xml != first_xml && moved_xml.find(L"&amp;") != std::wstring::npos, "Scheduler did not encode the actual executable path safely");
    result = fixture.ApplyRequest(true, false, L"E:\\Moved & \u09AC\u09BE\u0982\u09B2\u09BE Watchman\\Watchman.exe");
    Check(!result.changed && !result.error, "Moved executable caused repeated registration writes");

    Hr(fixture.Task()->put_Enabled(VARIANT_FALSE));
    const auto disabled_xml = Xml(fixture.Task().Get());
    result = fixture.ApplyRequest(true);
    Check(result.status == Status::ExternallyDisabled && !result.changed && !result.error &&
          Xml(fixture.Task().Get()) == disabled_xml, "Automatic launch re-enabled an externally disabled task");
    result = fixture.ApplyRequest(true, true);
    Check(result.status == Status::TaskReady && result.changed && !result.error, "Explicit On did not re-enable the task");
    fixture.WriteLegacy();
    result = fixture.ApplyRequest(false, true);
    Check(result.status == Status::Disabled && !result.error && !fixture.Task() && !fixture.HasLegacy(), "Off did not remove both real registrations");
    result = fixture.ApplyRequest(false);
    Check(result.status == Status::Disabled && !result.changed && !result.error, "Repeated Off was not read-only");

    fixture.WriteLegacy(); fixture.Approval(3);
    result = fixture.ApplyRequest(true);
    Check(result.status == Status::ExternallyDisabled && !result.changed && !fixture.Task() && fixture.HasLegacy(), "Windows-disabled Run was bypassed with a new task");
    Check(fixture.ApprovalState() == 3, "Windows approval was rewritten");
    fixture.Approval(0x7FFF);
    result = fixture.ApplyRequest(true);
    Check(result.status == Status::ExternallyDisabled && !fixture.Task() && fixture.HasLegacy(), "Unknown approval record was treated as permission to bypass Windows");
    result = fixture.ApplyRequest(true, true);
    Check(result.status == Status::TaskReady && !result.error && !fixture.HasLegacy(), "Explicit saved On did not migrate a disabled/unknown legacy record");
    Check(fixture.ApprovalState() == 0x7FFF, "Explicit On altered Windows' global/approval settings");

    fixture.PutForeignSource();
    const auto foreign_xml = Xml(fixture.Task().Get());
    result = fixture.ApplyRequest(true);
    Check(result.error == conflict_error && Xml(fixture.Task().Get()) == foreign_xml, "Foreign named task was overwritten");
    result = fixture.ApplyRequest(false, true);
    Check(result.error == conflict_error && Xml(fixture.Task().Get()) == foreign_xml, "Foreign named task was deleted");
    fixture.WriteLegacy(L"\"C:\\Other Tool\\Other.exe\" --startup");
    result = fixture.ApplyRequest(true, true);
    Check(result.error == conflict_error && fixture.HasLegacy(), "Unrelated named Run value was overwritten/removed");
    fixture.CleanupChecked();

    const auto production_scope_rejected = Testing::ApplyInScope({1, true, true, L"C:\\Watchman.exe"},
        {L"Watchman Sign-in not-a-test", L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Software\\Watchman.Tests\\unused"});
    Check(production_scope_rejected.error == invalid_argument_error, "Native tests allowed production startup locations");
}
} // namespace

int main(int argc, char** argv) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 2;
    int exit_code = 0;
    try {
        const bool standard = StandardToken();
        if (argc == 2 && std::string(argv[1]) == "--require-standard-user")
            Check(standard, "Standard-user proof was accidentally run as an administrator/elevated token");
        std::cout << "Startup registration token: " << (standard ? "STANDARD (non-admin, non-elevated)" : "ADMIN/ELEVATED (not standard-user proof)") << '\n';
        Exercise();
        std::cout << "StartupRegistration: " << checks << " real isolated Scheduler/Registry checks passed; no actions launched\n";
    } catch (const std::exception& error) {
        std::cerr << "StartupRegistration: " << error.what() << '\n';
        exit_code = 1;
    }
    CoUninitialize();
    return exit_code;
}
