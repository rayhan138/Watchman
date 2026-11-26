#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace WatchmanVersion {

struct Number {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
    std::uint16_t revision = 0;
};

inline std::wstring FormatLabel(const Number& version) {
    std::wstring label = L"Version " + std::to_wstring(version.major) + L"." +
        std::to_wstring(version.minor) + L"." + std::to_wstring(version.patch);
    if (version.revision != 0) label += L"." + std::to_wstring(version.revision);
    return label;
}

inline constexpr wchar_t unavailable_label[] = L"Version unavailable";

// Cached from this process's executable ProductVersion, never from an update
// feed, installer filename, another Watchman.exe, or a hard-coded UI label.
const std::optional<Number>& Current();
const std::wstring& Label();

} // namespace WatchmanVersion
