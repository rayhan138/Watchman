#include "app_version.h"

#ifdef _WIN32
#include <windows.h>
#include <winver.h>
#include <vector>
#endif

namespace WatchmanVersion {
namespace {
std::optional<Number> ReadNumber() {
#ifdef _WIN32
    // Installed/custom paths can exceed MAX_PATH and contain non-ASCII names.
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::nullopt;

    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.data(), &unused);
    if (size == 0) return std::nullopt;
    std::vector<BYTE> resource(size);
    if (!GetFileVersionInfoW(path.data(), 0, size, resource.data())) return std::nullopt;

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(resource.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        !info || info_size < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD) {
        return std::nullopt;
    }
    return Number{HIWORD(info->dwProductVersionMS), LOWORD(info->dwProductVersionMS),
        HIWORD(info->dwProductVersionLS), LOWORD(info->dwProductVersionLS)};
#else
    // Portable tests exercise formatting and the honest unversioned fallback.
    return std::nullopt;
#endif
}
} // namespace

const std::optional<Number>& Current() {
    static const auto version = ReadNumber();
    return version;
}

const std::wstring& Label() {
    static const std::wstring label = Current() ? FormatLabel(*Current()) : unavailable_label;
    return label;
}

} // namespace WatchmanVersion
