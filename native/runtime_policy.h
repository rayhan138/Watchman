#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace WatchmanRuntime {
inline constexpr int default_data_range_index = 1; // Last 7 Days
inline int DataRangeOnTabOpen(bool opening_data, int current_range) {
    return opening_data ? default_data_range_index : current_range;
}

inline std::wstring StartupCommand(const std::wstring& executable) {
    if (executable.empty() || executable.find(L'"') != std::wstring::npos) return {};
    return L"\"" + executable + L"\" --startup";
}

inline std::wstring RestartCommand(const std::wstring& executable, uint32_t current_pid, bool start_hidden) {
    if (executable.empty() || executable.find(L'"') != std::wstring::npos || current_pid == 0) return {};
    return L"\"" + executable + L"\" --replace-pid " + std::to_wstring(current_pid)
        + (start_hidden ? L" --startup" : L"");
}

struct MainUiActivity {
    bool keep_graphics;
    bool animate;
};
inline MainUiActivity GetMainUiActivity(bool visible, bool minimized, bool foreground) {
    const bool keep_graphics = visible && !minimized;
    return {keep_graphics, keep_graphics && foreground};
}

// Idle Network/Tools pages do not need sixty identical full-window paints/s.
// Animate transitions at the native cadence; publish a running speed test at 10Hz.
class MainFrameCadence {
public:
    bool Due(uint64_t now, bool animating, bool live_speed_test) {
        if (animating) { last_live_ = now; return true; }
        if (live_speed_test && now - last_live_ >= 100) { last_live_ = now; return true; }
        return false;
    }
private:
    uint64_t last_live_ = 0;
};

struct LaunchOptions {
    bool startup = false;
    uint32_t replace_pid = 0;
};

// Arguments must already be decoded with CommandLineToArgvW, never substring-matched.
inline LaunchOptions ParseArguments(const std::vector<std::wstring>& arguments) {
    LaunchOptions result;
    for (size_t index = 1; index < arguments.size(); ++index) {
        // Tauri v1.0.5 registered the legacy Run entry with --minimized.
        // Preserve the same quiet-start intent while migrating that exact flag.
        if (arguments[index] == L"--startup" || arguments[index] == L"--minimized") result.startup = true;
        else if (arguments[index] == L"--replace-pid" && index + 1 < arguments.size()) {
            const auto& text = arguments[++index];
            uint64_t value = 0;
            bool valid = !text.empty();
            for (wchar_t character : text) {
                if (character < L'0' || character > L'9') { valid = false; break; }
                value = value * 10 + (character - L'0');
                if (value > (std::numeric_limits<uint32_t>::max)()) { valid = false; break; }
            }
            if (valid) result.replace_pid = static_cast<uint32_t>(value);
        }
    }
    return result;
}

inline bool ExceedsDragThreshold(int dx, int dy, int threshold_x, int threshold_y) {
    return dx > threshold_x || dx < -threshold_x || dy > threshold_y || dy < -threshold_y;
}
} // namespace WatchmanRuntime
