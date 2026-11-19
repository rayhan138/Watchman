#include "update.h"
#include "window_lifecycle.h"
#include "third_party/nlohmann/json.hpp"
#include <winhttp.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <atomic>
#include <regex>
#include <memory>
#include <thread>

namespace
{
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Internet = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
struct Close
{
    void operator()(HANDLE h) const
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, Close>;
void check(bool ok, const char *what)
{
    if (!ok)
        throw std::runtime_error(what);
}
std::atomic_bool checking{false};
#ifdef WATCHMAN_PROTOCOL_PROBE
// Separate CTest/tooling target, never linked into or distributed with Watchman.
const std::wstring base = L"https://localhost:45443/releases/";
constexpr INTERNET_PORT port = 45443;
constexpr std::array<unsigned, 3> current{1, 0, 5};
bool allowed(const std::wstring &host)
{
    return host == L"localhost";
}
#else
const std::wstring base = L"https://github.com/rayhan138/Watchman/releases/";
constexpr INTERNET_PORT port = 443;
constexpr std::array<unsigned, 3> current{2, 0, 0};
bool allowed(const std::wstring &host)
{
    return host == L"github.com" || host == L"release-assets.githubusercontent.com" ||
           host == L"objects.githubusercontent.com";
}
#endif
std::wstring wide(const std::string &ascii)
{
    return {ascii.begin(), ascii.end()};
}
// Explicit redirect loop. Never follow HTTP, a non-GitHub host, credentials or a custom port.
Bytes download(std::wstring url, size_t limit)
{
    Internet session(WinHttpOpen(L"Watchman/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0),
                     WinHttpCloseHandle);
    check(!!session, "Cannot initialize HTTPS");
    WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 30000);
    for (int hop = 0; hop < 6; ++hop)
    {
        URL_COMPONENTS u{sizeof(u)};
        u.dwHostNameLength = u.dwUrlPathLength = u.dwExtraInfoLength = u.dwUserNameLength =
            u.dwPasswordLength = DWORD(-1);
        check(WinHttpCrackUrl(url.c_str(), 0, 0, &u) && u.nScheme == INTERNET_SCHEME_HTTPS &&
                  u.nPort == port && !u.dwUserNameLength && !u.dwPasswordLength,
              "Invalid HTTPS destination");
        std::wstring host(u.lpszHostName, u.dwHostNameLength), path(u.lpszUrlPath, u.dwUrlPathLength);
        check(allowed(host), "Unexpected update redirect host");
        if (u.dwExtraInfoLength)
            path.append(u.lpszExtraInfo, u.dwExtraInfoLength);
        Internet conn(WinHttpConnect(session.get(), host.c_str(), port, 0), WinHttpCloseHandle);
        Internet req(WinHttpOpenRequest(conn.get(), L"GET", path.c_str(), nullptr, nullptr, nullptr,
                                        WINHTTP_FLAG_SECURE),
                     WinHttpCloseHandle);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        check(req && WinHttpSetOption(req.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) &&
                  WinHttpSendRequest(req.get(), nullptr, 0, nullptr, 0, 0, 0) &&
                  WinHttpReceiveResponse(req.get(), nullptr),
              "Could not retrieve update (HTTPS)");
        DWORD status = 0, len = sizeof(status);
        check(WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                  &status, &len, nullptr),
              "Invalid HTTP response");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            wchar_t loc[8192];
            len = sizeof(loc);
            check(WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_LOCATION, nullptr, loc, &len, nullptr),
                  "Invalid redirect");
            url = loc;
            if (url.rfind(L"/", 0) == 0 && url.rfind(L"//", 0) != 0)
                url = L"https://" + host + url;
            continue;
        }
        check(status == 200, "Update server did not return the requested file");
        Bytes bytes;
        unsigned char chunk[32768];
        DWORD n;
        for (;;)
        {
            check(WinHttpReadData(req.get(), chunk, sizeof(chunk), &n), "Incomplete update transfer");
            if (!n)
                break;
            check(bytes.size() + n <= limit, "Update exceeds size limit");
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
        return bytes;
    }
    throw std::runtime_error("Too many update redirects");
}
std::array<unsigned, 3> parse_version(const std::string &v)
{
    static const std::regex pattern(R"(^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$)");
    std::smatch m;
    check(std::regex_match(v, m, pattern), "Invalid update version");
    std::array<unsigned, 3> a{};
    for (int i = 0; i < 3; ++i)
    {
        a[i] = std::stoul(m[i + 1].str());
        check(a[i] <= 65535, "Invalid update version");
    }
    return a;
}
struct Stage
{
    fs::path dir;
    bool launched = false;
    Stage()
    {
        GUID id;
        wchar_t s[40];
        check(SUCCEEDED(CoCreateGuid(&id)) && StringFromGUID2(id, s, 40), "Cannot create update workspace");
        dir = fs::temp_directory_path() / (std::wstring(L"Watchman-") + s);
        check(fs::create_directory(dir), "Cannot create update workspace");
    }
    ~Stage()
    {
        if (!launched)
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    }
};
void write(const fs::path &p, const Bytes &b)
{
    std::ofstream out(p, std::ios::binary);
    out.write((const char *)b.data(), b.size());
    out.close();
    check(out.good(), "Cannot save update");
}
void verify(const fs::path &setup, const fs::path &sig, const std::array<unsigned, 3> &expected)
{
    wchar_t exe[32768];
    auto n = GetModuleFileNameW(nullptr, exe, 32768);
    check(n && n < 32768, "Cannot locate installed verifier");
    auto tool = fs::path(exe).parent_path() / L"watchman-verify-release.exe";
    auto cmd = L"\"" + tool.wstring() + L"\" \"" + setup.wstring() + L"\" \"" + sig.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    check(CreateProcessW(tool.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         tool.parent_path().c_str(), &si, &pi),
          "Cannot start installed signature verifier");
    Handle proc(pi.hProcess), thr(pi.hThread);
    DWORD code = 1;
    check(WaitForSingleObject(proc.get(), 60000) == WAIT_OBJECT_0 && GetExitCodeProcess(proc.get(), &code) &&
              !code,
          "Installer signature is not trusted");
    // Do not trust an unsigned manifest's version claim about a valid older installer.
    DWORD unused;
    DWORD size = GetFileVersionInfoSizeW(setup.c_str(), &unused);
    Bytes info(size);
    VS_FIXEDFILEINFO *v = nullptr;
    UINT len = 0;
    check(size && GetFileVersionInfoW(setup.c_str(), 0, size, info.data()) &&
              VerQueryValueW(info.data(), L"\\", (void **)&v, &len) && len >= sizeof(*v) &&
              v->dwSignature == 0xfeef04bd && v->dwProductVersionMS == MAKELONG(expected[1], expected[0]) &&
              v->dwProductVersionLS == MAKELONG(0, expected[2]),
          "Signed installer version does not match manifest");
}
int run(HWND owner, bool install)
{
    auto feed = download(base + L"latest/download/latest.json", 64 * 1024);
    auto j = nlohmann::json::parse(feed);
    auto version = j.at("version").get<std::string>();
    auto v = parse_version(version);
    if (v <= current)
    {
#ifndef WATCHMAN_PROTOCOL_PROBE
        MessageBoxW(owner, L"No newer stable version is available.", L"Watchman update", MB_OK);
#endif
        return 3;
    }
    auto &entry = j.at("platforms").at("windows-x86_64");
    auto filename = L"Watchman_" + wide(version) + L"_x64-setup.exe";
    auto url = base + L"download/v" + wide(version) + L"/" + filename;
    check(wide(entry.at("url").get<std::string>()) == url, "Unexpected update asset URL");
#ifndef WATCHMAN_PROTOCOL_PROBE
    if (MessageBoxW(owner,
                    (L"Download and install Watchman " + wide(version) + L"? Watchman will restart.").c_str(),
                    L"Watchman update", MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return 3;
#endif
    auto encoded = entry.at("signature").get<std::string>();
    DWORD count = 0;
    check(!encoded.empty() && encoded.size() < 16384 &&
              CryptStringToBinaryA(encoded.c_str(), (DWORD)encoded.size(), CRYPT_STRING_BASE64, nullptr,
                                   &count, nullptr, nullptr),
          "Invalid signature encoding");
    Bytes signature(count);
    check(CryptStringToBinaryA(encoded.c_str(), (DWORD)encoded.size(), CRYPT_STRING_BASE64, signature.data(),
                               &count, nullptr, nullptr),
          "Invalid signature encoding");
    Stage stage;
    auto setup = stage.dir / filename, sig = stage.dir / L"updater.minisig";
    write(setup, download(url, 128 * 1024 * 1024));
    write(sig, signature);
    // Deny writers, deletion and directory renaming through verification and handoff.
    Handle directory(CreateFileW(stage.dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    Handle lock(
        CreateFileW(setup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    check(directory.get() != INVALID_HANDLE_VALUE && lock.get() != INVALID_HANDLE_VALUE,
          "Cannot lock staged installer");
    verify(setup, sig, v);
    if (!install)
        return 0;
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpVerb = L"open";
    launch.lpFile = setup.c_str();
    launch.lpParameters = L"/P /UPDATE /R";
    launch.nShow = SW_SHOWNORMAL;
    check(ShellExecuteExW(&launch), "Installer was not started");
    Handle child(launch.hProcess);
    stage.launched = true;
#ifdef WATCHMAN_PROTOCOL_PROBE
    DWORD result = 1;
    check(WaitForSingleObject(child.get(), 120000) == WAIT_OBJECT_0 &&
              GetExitCodeProcess(child.get(), &result) && result == 0,
          "Installer failed");
#else
    PostMessageW(owner, WatchmanWindowLifecycle::ExitMessage, 0, 0);
#endif
    return 0;
}
} // namespace
void CheckForUpdate(HWND owner)
{
    if (checking.exchange(true))
        return;
    std::thread([owner] {
        try
        {
            if (IsUserAnAdmin())
                throw std::runtime_error(
                    "Restart Watchman without administrator privileges to check for updates.");
            run(owner, true);
        }
        catch (const std::exception &e)
        {
            MessageBoxA(owner, e.what(), "Watchman update", MB_ICONWARNING);
        }
        checking = false;
    }).detach();
}

#ifdef WATCHMAN_PROTOCOL_PROBE
int main(int argc, char **argv)
{
    try
    {
        return run(nullptr, argc == 2 && std::string(argv[1]) == "--install");
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
#endif
