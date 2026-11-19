#include "diagnostics.h"
#include "network_test_protocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <winhttp.h>

#include <array>
#include <chrono>
#include <ctime>
#include <cstring>
#include <iterator>
#include <sstream>

namespace {

constexpr wchar_t kDiagnosticsUserAgent[] = L"Watchman Native/2.0";
constexpr DWORD kConnectTimeoutMs = 5'000;
constexpr DWORD kGatewayPingTimeoutMs = 1'500;

DiagnosticTestResult MakeResult(
    const wchar_t* name,
    DiagnosticStatus status,
    std::wstring message,
    std::wstring details
) {
    DiagnosticTestResult result;
    result.name = name;
    result.status = status;
    result.message = std::move(message);
    result.details = std::move(details);
    return result;
}

std::wstring Win32ErrorMessage(const wchar_t* operation, DWORD error) {
    std::wstringstream message;
    message << operation << L" (Windows error " << error << L")";
    return message.str();
}

DiagnosticTestResult TestDns() {
    constexpr const wchar_t* kDomains[] = {
        L"www.google.com",
        L"www.cloudflare.com",
        L"github.com",
    };

    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    for (const wchar_t* domain : kDomains) {
        ADDRINFOW* addresses = nullptr;
        const int result = GetAddrInfoW(domain, nullptr, &hints, &addresses);
        if (result == 0 && addresses != nullptr) {
            FreeAddrInfoW(addresses);
            return MakeResult(
                L"DNS Resolution",
                DiagnosticStatus::Passed,
                L"A test hostname resolved successfully",
                std::wstring(L"Successfully resolved ") + domain
            );
        }
        if (addresses != nullptr) {
            FreeAddrInfoW(addresses);
        }
    }

    DiagnosticTestResult result = MakeResult(
        L"DNS Resolution",
        DiagnosticStatus::Failed,
        L"None of the test hostnames resolved",
        L"Unable to resolve any test domains"
    );
    result.suggestion = L"Check the configured resolver, VPN, and network policy. Test-domain failures do not prove all DNS is unavailable.";
    return result;
}

bool ProbeHttps(const wchar_t* host, DWORD& http_status, std::wstring& error) {
    HINTERNET session = WinHttpOpen(
        kDiagnosticsUserAgent,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!session) {
        error = Win32ErrorMessage(L"Opening the HTTP client failed", GetLastError());
        return false;
    }

    WinHttpSetTimeouts(session, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        error = Win32ErrorMessage(L"Connecting to the test site failed", GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"HEAD",
        L"/",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!request) {
        error = Win32ErrorMessage(L"Opening the connectivity request failed", GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    const bool sent = WinHttpSendRequest(
        request,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    ) == TRUE;
    const bool received = sent && WinHttpReceiveResponse(request, nullptr) == TRUE;
    if (!received) {
        error = Win32ErrorMessage(L"The connectivity request failed", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status_size = sizeof(http_status);
    const bool got_status = WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &http_status,
        &status_size,
        WINHTTP_NO_HEADER_INDEX
    ) == TRUE;
    if (!got_status) {
        error = Win32ErrorMessage(L"Reading the test-site status failed", GetLastError());
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return got_status;
}

DiagnosticTestResult TestInternet() {
    constexpr const wchar_t* kHosts[] = {
        L"www.google.com",
        L"www.cloudflare.com",
    };

    std::wstring last_error;
    for (const wchar_t* host : kHosts) {
        DWORD status = 0;
        std::wstring error;
        if (ProbeHttps(host, status, error) && status >= 200 && status < 400) {
            std::wstringstream details;
            details << L"HTTPS request to " << host << L" succeeded (HTTP " << status << L")";
            return MakeResult(
                L"HTTPS Connectivity",
                DiagnosticStatus::Passed,
                L"A test website answered HTTPS",
                details.str()
            );
        }
        if (!error.empty()) {
            last_error = std::move(error);
        } else if (status != 0) {
            last_error = std::wstring(host) + L" returned HTTP " + std::to_wstring(status);
        }
    }

    DiagnosticTestResult result = MakeResult(
        L"HTTPS Connectivity",
        DiagnosticStatus::Failed,
        L"HTTPS probes to the test websites failed",
        last_error.empty() ? L"Unable to reach any test websites" : last_error
    );
    result.suggestion = L"Check connectivity, proxy/VPN settings, and filtering. These sites may be blocked even when other Internet access works.";
    return result;
}

bool IsZeroIpv4(const IN_ADDR& address) {
    return address.S_un.S_addr == INADDR_ANY;
}

std::wstring GetInterfaceAlias(const NET_LUID& luid) {
    wchar_t alias[IF_MAX_STRING_SIZE + 1]{};
    if (ConvertInterfaceLuidToAlias(&luid, alias, static_cast<ULONG>(std::size(alias))) == NO_ERROR && alias[0] != L'\0') {
        return alias;
    }
    return L"active network interface";
}

DiagnosticTestResult TestIpv4Route(bool internet_ok) {
    SOCKADDR_INET destination{};
    destination.si_family = AF_INET;
    InetPtonW(AF_INET, L"1.1.1.1", &destination.Ipv4.sin_addr);

    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET best_source{};
    const DWORD route_result = GetBestRoute2(
        nullptr,
        0,
        nullptr,
        &destination,
        0,
        &route,
        &best_source
    );
    if (route_result != NO_ERROR) {
        DiagnosticTestResult result = MakeResult(
            L"IPv4 Route (1.1.1.1)",
            DiagnosticStatus::Warning,
            L"No IPv4 route to 1.1.1.1 was returned",
            Win32ErrorMessage(L"IPv6 connectivity was not tested; IPv4 route lookup failed", route_result)
        );
        if (!internet_ok) {
            result.suggestion = L"Reconnect the current adapter, then check the router, modem, hotspot, or VPN.";
        }
        return result;
    }

    const std::wstring alias = GetInterfaceAlias(route.InterfaceLuid);
    if (route.NextHop.si_family != AF_INET || IsZeroIpv4(route.NextHop.Ipv4.sin_addr)) {
        return MakeResult(
            L"IPv4 Route (1.1.1.1)",
            DiagnosticStatus::Passed,
            L"IPv4 route found without an exposed gateway",
            alias + L" routes to 1.1.1.1 without an exposed gateway; no gateway ping was attempted."
        );
    }

    wchar_t gateway[INET_ADDRSTRLEN]{};
    InetNtopW(AF_INET, &route.NextHop.Ipv4.sin_addr, gateway, static_cast<DWORD>(std::size(gateway)));

    HANDLE icmp = IcmpCreateFile();
    DWORD replies = 0;
    DWORD echo_status = icmp == INVALID_HANDLE_VALUE ? GetLastError() : IP_GENERAL_FAILURE;
    if (icmp != INVALID_HANDLE_VALUE) {
        const char payload[] = "Watchman";
        std::array<unsigned char, sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8> reply{};
        replies = IcmpSendEcho(
            icmp,
            route.NextHop.Ipv4.sin_addr.S_un.S_addr,
            const_cast<char*>(payload),
            static_cast<WORD>(sizeof(payload) - 1),
            nullptr,
            reply.data(),
            static_cast<DWORD>(reply.size()),
            kGatewayPingTimeoutMs
        );
        if (replies > 0) {
            ICMP_ECHO_REPLY first_reply{};
            std::memcpy(&first_reply, reply.data(), sizeof(first_reply));
            echo_status = first_reply.Status;
        } else {
            echo_status = GetLastError();
        }
        IcmpCloseHandle(icmp);
    }

    std::wstringstream details;
    details << L"Route to 1.1.1.1: " << alias << L" via " << (gateway[0] ? gateway : L"the gateway")
            << L" (route metric " << route.Metric << L", ICMP status/error " << echo_status << L")";
    if (network_test::SuccessfulEcho(replies, echo_status)) {
        return MakeResult(
            L"IPv4 Route (1.1.1.1)",
            DiagnosticStatus::Passed,
            L"The IPv4 route's gateway answered ICMP",
            details.str()
        );
    }

    DiagnosticTestResult result = MakeResult(
        L"IPv4 Route (1.1.1.1)",
        DiagnosticStatus::Warning,
        L"IPv4 route found; no successful gateway echo reply",
        details.str()
    );
    if (!internet_ok) {
        result.suggestion = L"Reconnect the current adapter, then check the router, modem, hotspot, or VPN.";
    }
    return result;
}

std::wstring OverallStatus(const std::vector<DiagnosticTestResult>& tests) {
    bool has_warning = false;
    for (const auto& test : tests) {
        if (test.status == DiagnosticStatus::Failed) {
            return L"failed";
        }
        has_warning = has_warning || test.status == DiagnosticStatus::Warning;
    }
    return has_warning ? L"warning" : L"pass";
}

} // namespace

DiagnosticsRunner::~DiagnosticsRunner() {
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool DiagnosticsRunner::Start() {
    bool expected = false;
    if (!is_running_.compare_exchange_strong(expected, true)) {
        return false;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_ = {};
        snapshot_.is_running = true;
        snapshot_.overall_status = L"running";
    }

    try {
        worker_thread_ = std::thread([this]() {
            network_test::RunningReset reset(is_running_);
            try {
                Run();
            } catch (...) {
                std::lock_guard<std::mutex> lock(snapshot_mutex_);
                snapshot_.is_running = false;
                snapshot_.has_run = true;
                snapshot_.overall_status = L"failed";
                snapshot_.completed_at_unix_seconds = static_cast<uint64_t>(std::time(nullptr));
                snapshot_.tests = {MakeResult(L"Diagnostics", DiagnosticStatus::Failed,
                    L"Diagnostics could not finish", L"An unexpected worker error occurred")};
            }
        });
    } catch (...) {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.is_running = false;
        snapshot_.has_run = true;
        snapshot_.overall_status = L"failed";
        snapshot_.completed_at_unix_seconds = static_cast<uint64_t>(std::time(nullptr));
        snapshot_.tests = {MakeResult(L"Diagnostics", DiagnosticStatus::Failed,
            L"Diagnostics could not start", L"Unable to create the worker thread")};
        is_running_ = false;
        return false;
    }
    return true;
}

DiagnosticsSnapshot DiagnosticsRunner::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

void DiagnosticsRunner::Run() {
    const auto start = std::chrono::steady_clock::now();
    const DiagnosticTestResult dns = TestDns();
    const DiagnosticTestResult internet = TestInternet();
    const DiagnosticTestResult route = TestIpv4Route(internet.status == DiagnosticStatus::Passed);
    std::vector<DiagnosticTestResult> tests{dns, route, internet};

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );

    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_.is_running = false;
        snapshot_.has_run = true;
        snapshot_.overall_status = OverallStatus(tests);
        snapshot_.duration_ms = static_cast<uint64_t>(duration.count());
        snapshot_.completed_at_unix_seconds = static_cast<uint64_t>(std::time(nullptr));
        snapshot_.tests = std::move(tests);
    }
}
