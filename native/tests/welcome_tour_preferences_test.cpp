#include "welcome_tour.h"
#include "welcome_tour_preferences.h"
#include "scoped_handle.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct TestKey {
    std::wstring path = L"Software\\Watchman-WelcomeTour-Test-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64());
    Watchman::ScopedHKey key;
    TestKey() {
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
            KEY_ALL_ACCESS, nullptr, key.Put(), nullptr) == ERROR_SUCCESS, "Cannot create isolated volatile test key");
    }
    ~TestKey() { key.Close(); RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()); }
};
}

int main() {
    try {
        using namespace WatchmanWelcome;
        TestKey isolated;
        const auto key = isolated.key.Get();
        Check(ReadSeenVersion(nullptr) == 0 && ReadSeenVersion(key) == 0, "Missing welcome marker must mean unseen");
        Check(RegQueryValueExW(key, seen_value_name, nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND,
            "Reading a missing marker must not consume the welcome");
        const DWORD sentinel = 731;
        Check(RegSetValueExW(key, L"UnrelatedSetting", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&sentinel),
            sizeof(sentinel)) == ERROR_SUCCESS, "Cannot prepare isolated sentinel");
        for (DWORD type : {REG_BINARY, REG_SZ, REG_QWORD}) {
            const ULONGLONG malformed = 42;
            Check(RegSetValueExW(key, seen_value_name, 0, type, reinterpret_cast<const BYTE*>(&malformed),
                sizeof(malformed)) == ERROR_SUCCESS, "Cannot prepare invalid marker type");
            Check(ReadSeenVersion(key) == 0, "A malformed or differently typed value must not suppress onboarding");
        }
        Check(WriteSeenVersion(key, version) == ERROR_SUCCESS && ReadSeenVersion(key) == version,
            "Skip/Get started marker must round-trip as a DWORD");
        Tour next_launch;
        next_launch.LoadSeenVersion(ReadSeenVersion(key));
        Check(!next_launch.Offer(true), "Saved completion must suppress the next launch's automatic tour");
        Check(WriteSeenVersion(key, version + 5) == ERROR_SUCCESS && WriteSeenVersion(key, version) == ERROR_SUCCESS &&
            ReadSeenVersion(key) == version + 5, "Older EXEs must preserve a newer completion marker");
        DWORD actual = 0, size = sizeof(actual), type = 0;
        Check(RegQueryValueExW(key, L"UnrelatedSetting", nullptr, &type, reinterpret_cast<BYTE*>(&actual), &size) == ERROR_SUCCESS
            && actual == sentinel, "Welcome persistence must not overwrite any monitoring preference");
        Watchman::ScopedHKey read_only;
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, isolated.path.c_str(), 0, KEY_QUERY_VALUE, read_only.Put()) == ERROR_SUCCESS,
            "Cannot open isolated read-only key");
        Check(WriteSeenVersion(read_only.Get(), version + 10) == ERROR_ACCESS_DENIED && ReadSeenVersion(key) == version + 5,
            "Failed writes must be reported without changing the previous marker");
        Tour in_memory;
        in_memory.Offer(true); in_memory.Activate(Control::Skip);
        Check(!in_memory.Offer(true), "A persistence failure must not reopen a skipped tour repeatedly within the session");
        std::cout << "Welcome persistence checks passed (isolated volatile key only; no real Watchman settings touched)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
