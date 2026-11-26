#pragma once

#include <windows.h>
#include "update_policy.h"

namespace WatchmanUpdates {
inline constexpr wchar_t preference_path[] = L"Software\\Watchman\\Updates";
inline constexpr wchar_t notified_value[] = L"NotifiedVersion";
inline constexpr wchar_t dismissed_value[] = L"DismissedVersion";

// Accept only short, canonical numeric markers; never store remote text/URLs.
// Helpers take an open scoped key so tests need not touch real preferences.
inline std::string ReadMarker(HKEY key, const wchar_t* name) {
    wchar_t buffer[32]{};
    DWORD bytes = sizeof(buffer), type = 0;
    if (!key || RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t) || bytes > sizeof(buffer) || bytes % sizeof(wchar_t) ||
        buffer[bytes / sizeof(wchar_t) - 1] != L'\0') return {};
    std::string text;
    for (DWORD i = 0; i + 1 < bytes / sizeof(wchar_t); ++i) {
        if ((buffer[i] < L'0' || buffer[i] > L'9') && buffer[i] != L'.') return {};
        text += static_cast<char>(buffer[i]);
    }
    const auto version = ParseVersion(text);
    return version && VersionKey(*version) == text ? text : std::string{};
}
inline LONG WriteMarker(HKEY key, const wchar_t* name, const std::string& marker) {
    const auto version = ParseVersion(marker);
    if (!version || VersionKey(*version) != marker) return ERROR_INVALID_DATA;
    if (ReadMarker(key, name) == marker) return ERROR_SUCCESS;
    const std::wstring value(marker.begin(), marker.end());
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}
} // namespace WatchmanUpdates
