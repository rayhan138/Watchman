#include <windows.h>
#include <shellapi.h>
#include "runtime_policy.h"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main() {
    try {
        const std::wstring path = L"E:\\Watchman Craft\\Watchman\\build\\Release\\Watchman.exe";
        const auto command = WatchmanRuntime::StartupCommand(path);
        Check(command == L"\"" + path + L"\" --startup", "Startup path must be quoted with a separate startup flag");
        int count = 0;
        LPWSTR* argv = CommandLineToArgvW(command.c_str(), &count);
        Check(argv != nullptr, "Windows argument decoding failed");
        std::vector<std::wstring> arguments;
        for (int index = 0; index < count; ++index) arguments.emplace_back(argv[index]);
        LocalFree(argv);
        Check(arguments.size() == 2 && arguments[0] == path, "Startup command must round-trip a spaced path");
        Check(WatchmanRuntime::ParseArguments(arguments).startup, "Decoded startup launch must hide the main window");
        Check(WatchmanRuntime::ParseArguments({path,L"--minimized"}).startup, "v1 autostart argument must stay quiet until Run is rewritten");
        Check(!WatchmanRuntime::ParseArguments({path}).startup, "Normal launch must show the main window");
        Check(!WatchmanRuntime::ParseArguments({L"C:\\--startup\\Watchman.exe", L"--startup-other"}).startup,
            "Flags must not match substrings in paths or other flags");
        Check(WatchmanRuntime::StartupCommand(L"").empty() && WatchmanRuntime::StartupCommand(L"bad\"path").empty(),
            "Invalid executable paths must be rejected");
        const auto replacement = WatchmanRuntime::ParseArguments({path, L"--replace-pid", L"12345"});
        Check(replacement.replace_pid == 12345 && !replacement.startup, "Elevated replacement must wait for old PID and show UI");
        for (const auto* invalid : {L"", L"-1", L"1junk", L"4294967296", L"99999999999999999999999999999"}) {
            Check(WatchmanRuntime::ParseArguments({path, L"--replace-pid", invalid}).replace_pid == 0,
                "Invalid or overflowing replacement PID must be rejected");
        }
        Check(WatchmanRuntime::ParseArguments({path, L"--replace-pid"}).replace_pid == 0, "Missing PID must be safe");
        for (bool hidden : {false, true}) {
            const auto restart_command = WatchmanRuntime::RestartCommand(path, 12345, hidden);
            argv = CommandLineToArgvW(restart_command.c_str(), &count);
            Check(argv != nullptr, "Windows restart argument decoding failed");
            arguments.clear();
            for (int index = 0; index < count; ++index) arguments.emplace_back(argv[index]);
            LocalFree(argv);
            Check(arguments.size() == (hidden ? 4u : 3u) && arguments[0] == path,
                "Restart command must round-trip a spaced executable path");
            const auto restart = WatchmanRuntime::ParseArguments(arguments);
            Check(restart.replace_pid == 12345 && restart.startup == hidden,
                "Restart must wait for the current PID and preserve quiet/visible launch intent");
        }
        Check(WatchmanRuntime::RestartCommand(L"", 12345, false).empty()
            && WatchmanRuntime::RestartCommand(L"bad\"path", 12345, true).empty()
            && WatchmanRuntime::RestartCommand(path, 0, false).empty(),
            "Restart must reject empty/quoted paths and a missing current PID");
        for (bool visible : {false, true}) {
            for (bool minimized : {false, true}) {
                for (bool foreground : {false, true}) {
                    const auto activity = WatchmanRuntime::GetMainUiActivity(visible, minimized, foreground);
                    Check(activity.keep_graphics == (visible && !minimized),
                        "Main graphics must be released only when hidden or minimized, not on alt-tab");
                    Check(activity.animate == (visible && !minimized && foreground),
                        "Animation must stop when hidden, minimized or inactive");
                }
            }
        }
        Check(!WatchmanRuntime::ExceedsDragThreshold(3, -4, 4, 4), "Click jitter must not start dragging");
        Check(WatchmanRuntime::ExceedsDragThreshold(5, 0, 4, 4)
            && WatchmanRuntime::ExceedsDragThreshold(-5, 0, 4, 4)
            && WatchmanRuntime::ExceedsDragThreshold(0, 5, 4, 4)
            && WatchmanRuntime::ExceedsDragThreshold(0, -5, 4, 4), "Real movement must drag in all directions");
        Check(WatchmanRuntime::default_data_range_index == 1, "Data must initially default to Last 7 Days");
        for (int previous_range : {0, 1, 2, 3}) {
            Check(WatchmanRuntime::DataRangeOnTabOpen(true, previous_range) == 1,
                "Opening/reopening Data must reset Today, Monthly or Yearly to Last 7 Days");
            Check(WatchmanRuntime::DataRangeOnTabOpen(false, previous_range) == previous_range,
                "Unrelated navigation must not alter an explicit range selection");
        }
        std::cout << "Runtime policy checks passed (no Registry writes or Watchman launch)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
