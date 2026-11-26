#include "update_checker.h"
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <memory>

namespace WatchmanUpdates {
namespace {
struct InternetCloser { void operator()(void* handle) const { if (handle) WinHttpCloseHandle(handle); } };
using Internet = std::unique_ptr<void, InternetCloser>;
Result Failed(DWORD error = GetLastError()) { return {Status::Unavailable, {}, error}; }
Result FetchRelease(const std::atomic<bool>& stopping, const std::optional<Version>& current,
                    const wchar_t* host, INTERNET_PORT port, const wchar_t* path, bool secure) {
    if (!current) return {Status::UnknownVersion, {}, 0};
    if (stopping) return Failed(ERROR_CANCELLED);
    const std::wstring agent = L"Watchman/" + VersionText(*current) + L" (release-check)";
    Internet session(WinHttpOpen(agent.c_str(), secure ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return Failed();
    // Ordinary synchronous HTTP on this worker only, not the monitoring or UI
    // thread. Finite per-operation waits + response/deadline limits. No TLS
    // bypass, credentials, cookies, auto-downloads or cross-origin redirects.
    if (!WinHttpSetTimeouts(session.get(), 2500, 2500, 2500, 2500)) return Failed();
    const uint64_t deadline = GetTickCount64() + 12000;
    Internet connection(WinHttpConnect(session.get(), host, port, 0));
    if (!connection) return Failed();
    Internet request(WinHttpOpenRequest(connection.get(), L"GET", path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, (secure ? WINHTTP_FLAG_SECURE : 0) | WINHTTP_FLAG_REFRESH));
    if (!request) return Failed();
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) return Failed();
    const auto cancelled = [&] { return stopping || GetTickCount64() >= deadline; };
    if (cancelled()) return Failed(ERROR_CANCELLED);
    constexpr wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return Failed();
    if (cancelled()) return Failed(ERROR_TIMEOUT);
    if (!WinHttpReceiveResponse(request.get(), nullptr)) return Failed();
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) return Failed();
    if (status != 200) return EvaluateResponse(status, {}, current);
    // Reject surprising document types instead of parsing an HTML proxy page.
    wchar_t type[128]{};
    size = sizeof(type);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
        type, &size, WINHTTP_NO_HEADER_INDEX)) return Failed();
    const std::wstring_view content_type(type);
    if (content_type != L"application/json" && content_type.substr(0, 17) != L"application/json;") return Failed(ERROR_INVALID_DATA);
    std::string body;
    std::array<char, 8192> chunk{};
    for (;;) {
        if (cancelled()) return Failed(ERROR_TIMEOUT);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read)) return Failed();
        if (read == 0) break;
        if (body.size() + read > max_response_bytes) return Failed(ERROR_FILE_TOO_LARGE);
        body.append(chunk.data(), read);
    }
    if (cancelled()) return Failed(ERROR_TIMEOUT);
    return EvaluateResponse(status, body, current);
}
} // namespace

Result CheckOfficialRelease(const std::atomic<bool>& stopping) {
    return FetchRelease(stopping, WatchmanVersion::Current(), api_host, INTERNET_DEFAULT_HTTPS_PORT, api_path, true);
}
#ifdef WATCHMAN_UPDATE_CHECKING_TESTING
Result CheckLoopbackReleaseForTest(unsigned short port, const std::atomic<bool>& stopping) {
    return FetchRelease(stopping, Version{1, 0, 0, 0}, L"127.0.0.1", port, L"/release-fixture", false);
}
#endif
} // namespace WatchmanUpdates
