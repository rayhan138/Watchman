#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace WatchmanWelcome {
inline constexpr wchar_t seen_value_name[] = L"WelcomeTourVersion";

// The caller supplies an opened key, so tests can use a private volatile test
// key instead of changing a real Watchman profile. Opening/reading never marks
// the tour seen; only an explicit Skip or Get started writes this one value.
inline uint32_t ReadSeenVersion(HKEY key) {
    DWORD type = 0, size = sizeof(DWORD), value = 0;
    if (!key || RegQueryValueExW(key, seen_value_name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS ||
            type != REG_DWORD || size != sizeof(DWORD)) return 0;
    return value;
}
inline LONG WriteSeenVersion(HKEY key, uint32_t version) {
    const DWORD value = (std::max)(ReadSeenVersion(key), version);
    return RegSetValueExW(key, seen_value_name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
}
} // namespace WatchmanWelcome
