#include "speed_tester.h"

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kUserAgent[] = L"Watchman Native/1.0";
constexpr wchar_t kMlabLocateHost[] = L"locate.measurementlab.net";
constexpr wchar_t kMlabLocatePath[] = L"/v2/nearest/ndt/ndt7";
constexpr wchar_t kNdt7ProtocolHeader[] = L"Sec-WebSocket-Protocol: net.measurementlab.ndt.v7\r\n";
constexpr double kTestWindowSeconds = 10.0;
constexpr DWORD kConnectTimeoutMs = 10'000;
constexpr DWORD kReceiveTimeoutMs = 2'000;
constexpr size_t kTransferChunkBytes = 64 * 1024;

struct WebSocketUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

std::wstring MakeWinHttpError(const wchar_t* operation, DWORD error) {
    std::wstringstream message;
    message << operation << L" (WinHTTP " << error << L")";
    return message.str();
}

bool IsJsonWhitespace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

int HexValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return 10 + (ch - L'a');
    if (ch >= L'A' && ch <= L'F') return 10 + (ch - L'A');
    return -1;
}

bool ExtractJsonString(
    const std::wstring& json,
    const std::wstring& key,
    std::wstring& value,
    size_t start_at = 0
) {
    const std::wstring needle = L"\"" + key + L"\"";
    const size_t key_pos = json.find(needle, start_at);
    if (key_pos == std::wstring::npos) return false;

    size_t pos = json.find(L':', key_pos + needle.size());
    if (pos == std::wstring::npos) return false;
    ++pos;
    while (pos < json.size() && IsJsonWhitespace(json[pos])) ++pos;
    if (pos >= json.size() || json[pos] != L'\"') return false;
    ++pos;

    std::wstring decoded;
    while (pos < json.size()) {
        const wchar_t ch = json[pos++];
        if (ch == L'\"') {
            value = std::move(decoded);
            return true;
        }
        if (ch != L'\\') {
            decoded.push_back(ch);
            continue;
        }
        if (pos >= json.size()) return false;

        const wchar_t escaped = json[pos++];
        switch (escaped) {
        case L'\"': decoded.push_back(L'\"'); break;
        case L'\\': decoded.push_back(L'\\'); break;
        case L'/': decoded.push_back(L'/'); break;
        case L'b': decoded.push_back(L'\b'); break;
        case L'f': decoded.push_back(L'\f'); break;
        case L'n': decoded.push_back(L'\n'); break;
        case L'r': decoded.push_back(L'\r'); break;
        case L't': decoded.push_back(L'\t'); break;
        case L'u': {
            if (pos + 4 > json.size()) return false;
            unsigned int codepoint = 0;
            for (size_t index = 0; index < 4; ++index) {
                const int hex = HexValue(json[pos + index]);
                if (hex < 0) return false;
                codepoint = (codepoint << 4) | static_cast<unsigned int>(hex);
            }
            decoded.push_back(static_cast<wchar_t>(codepoint));
            pos += 4;
            break;
        }
        default:
            return false;
        }
    }

    return false;
}


std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};

    const int source_size = static_cast<int>(value.size());
    int converted_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_size, nullptr, 0);
    UINT code_page = CP_UTF8;
    if (converted_size == 0) {
        code_page = CP_ACP;
        converted_size = MultiByteToWideChar(code_page, 0, value.data(), source_size, nullptr, 0);
    }
    if (converted_size == 0) return {};

    std::wstring converted(static_cast<size_t>(converted_size), L'\0');
    if (MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
            value.data(), source_size, converted.data(), converted_size) == 0) {
        return {};
    }
    return converted;
}

bool GetHttpStatus(HINTERNET request, DWORD& status_code) {
    DWORD status_size = sizeof(status_code);
    return WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code,
        &status_size,
        WINHTTP_NO_HEADER_INDEX
    ) == TRUE;
}

bool ReadHttpResponse(HINTERNET request, std::string& response, std::wstring& error) {
    response.clear();
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = MakeWinHttpError(L"Reading M-Lab response failed", GetLastError());
            return false;
        }
        if (available == 0) return true;

        std::vector<char> buffer(static_cast<size_t>(available));
        DWORD downloaded = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &downloaded)) {
            error = MakeWinHttpError(L"Reading M-Lab response failed", GetLastError());
            return false;
        }
        response.append(buffer.data(), downloaded);
    }
}

bool ParseWebSocketUrl(const std::wstring& url, WebSocketUrl& parsed, std::wstring& error) {
    constexpr std::wstring_view kSecurePrefix = L"wss://";
    constexpr std::wstring_view kPlainPrefix = L"ws://";

    size_t host_start = 0;
    if (url.rfind(kSecurePrefix.data(), 0) == 0) {
        parsed.secure = true;
        parsed.port = INTERNET_DEFAULT_HTTPS_PORT;
        host_start = kSecurePrefix.size();
    } else if (url.rfind(kPlainPrefix.data(), 0) == 0) {
        parsed.secure = false;
        parsed.port = INTERNET_DEFAULT_HTTP_PORT;
        host_start = kPlainPrefix.size();
    } else {
        error = L"M-Lab returned an invalid NDT7 WebSocket address";
        return false;
    }

    const size_t path_start = url.find_first_of(L"/?", host_start);
    const std::wstring authority = path_start == std::wstring::npos
        ? url.substr(host_start)
        : url.substr(host_start, path_start - host_start);
    if (authority.empty()) {
        error = L"M-Lab returned an empty NDT7 host";
        return false;
    }

    parsed.path = path_start == std::wstring::npos ? L"/" : url.substr(path_start);
    if (!parsed.path.empty() && parsed.path.front() == L'?') {
        parsed.path.insert(parsed.path.begin(), L'/');
    }

    const size_t colon = authority.rfind(L':');
    const bool has_port = colon != std::wstring::npos && authority.find(L']') == std::wstring::npos;
    if (!has_port) {
        parsed.host = authority;
        return true;
    }

    const std::wstring port_text = authority.substr(colon + 1);
    wchar_t* end = nullptr;
    const unsigned long parsed_port = std::wcstoul(port_text.c_str(), &end, 10);
    if (end == port_text.c_str() || *end != L'\0' || parsed_port == 0 || parsed_port > 65535) {
        error = L"M-Lab returned an invalid NDT7 port";
        return false;
    }

    parsed.host = authority.substr(0, colon);
    parsed.port = static_cast<INTERNET_PORT>(parsed_port);
    return true;
}

class Ndt7WebSocket {
public:
    Ndt7WebSocket() = default;
    ~Ndt7WebSocket() { Close(); }

    Ndt7WebSocket(const Ndt7WebSocket&) = delete;
    Ndt7WebSocket& operator=(const Ndt7WebSocket&) = delete;

    bool Connect(const std::wstring& url, std::wstring& error) {
        WebSocketUrl parsed;
        if (!ParseWebSocketUrl(url, parsed, error)) return false;

        session_ = WinHttpOpen(
            kUserAgent,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (!session_) {
            error = MakeWinHttpError(L"Opening M-Lab connection failed", GetLastError());
            return false;
        }
        WinHttpSetTimeouts(session_, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs, kReceiveTimeoutMs);

        connection_ = WinHttpConnect(session_, parsed.host.c_str(), parsed.port, 0);
        if (!connection_) {
            error = MakeWinHttpError(L"Connecting to M-Lab failed", GetLastError());
            return false;
        }

        const DWORD request_flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
        request_ = WinHttpOpenRequest(
            connection_,
            L"GET",
            parsed.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            request_flags
        );
        if (!request_) {
            error = MakeWinHttpError(L"Opening M-Lab WebSocket request failed", GetLastError());
            return false;
        }

        if (!WinHttpSetOption(request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            error = MakeWinHttpError(L"Preparing M-Lab WebSocket upgrade failed", GetLastError());
            return false;
        }
        if (!WinHttpAddRequestHeaders(
                request_,
                kNdt7ProtocolHeader,
                static_cast<DWORD>(wcslen(kNdt7ProtocolHeader)),
                WINHTTP_ADDREQ_FLAG_ADD
            )) {
            error = MakeWinHttpError(L"Adding NDT7 protocol header failed", GetLastError());
            return false;
        }
        if (!WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            error = MakeWinHttpError(L"Starting M-Lab WebSocket failed", GetLastError());
            return false;
        }
        if (!WinHttpReceiveResponse(request_, nullptr)) {
            error = MakeWinHttpError(L"Completing M-Lab WebSocket handshake failed", GetLastError());
            return false;
        }

        DWORD status_code = 0;
        if (!GetHttpStatus(request_, status_code)) {
            error = MakeWinHttpError(L"Reading M-Lab handshake status failed", GetLastError());
            return false;
        }
        if (status_code != 101) {
            std::wstringstream message;
            message << L"M-Lab rejected the NDT7 connection (HTTP " << status_code << L")";
            error = message.str();
            return false;
        }

        wchar_t selected_protocol[128]{};
        DWORD protocol_bytes = sizeof(selected_protocol);
        if (!WinHttpQueryHeaders(request_, WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol",
                selected_protocol, &protocol_bytes, WINHTTP_NO_HEADER_INDEX) ||
            !network_test::IsNdt7Subprotocol(selected_protocol)) {
            error = L"The server did not select the required NDT7 WebSocket protocol";
            return false;
        }

        socket_ = WinHttpWebSocketCompleteUpgrade(request_, 0);
        if (!socket_) {
            error = MakeWinHttpError(L"Opening M-Lab WebSocket failed", GetLastError());
            return false;
        }
        WinHttpCloseHandle(request_);
        request_ = nullptr;
        return true;
    }

    DWORD Receive(void* buffer, DWORD buffer_size, DWORD* bytes_read, WINHTTP_WEB_SOCKET_BUFFER_TYPE* buffer_type) const {
        return WinHttpWebSocketReceive(socket_, buffer, buffer_size, bytes_read, buffer_type);
    }

    DWORD ShutdownSend() {
        if (send_shutdown_) return NO_ERROR;
        send_shutdown_ = true;
        return WinHttpWebSocketShutdown(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }

    std::wstring CloseIssue() const {
        USHORT status = 0;
        DWORD reason_size = 0;
        std::array<char, 123> reason{};
        const DWORD result = WinHttpWebSocketQueryCloseStatus(socket_, &status,
            reason.data(), static_cast<DWORD>(reason.size()), &reason_size);
        if (result != NO_ERROR) return MakeWinHttpError(L"Reading NDT7 close status failed", result);
        if (status != WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS) {
            return L"NDT7 peer closed with status " + std::to_wstring(status);
        }
        return {};
    }

    DWORD SendBinary(const void* buffer, DWORD buffer_size) const {
        return WinHttpWebSocketSend(
            socket_,
            WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
            const_cast<void*>(buffer),
            buffer_size
        );
    }

private:
    void Close() {
        if (socket_) {
            ShutdownSend();
            WinHttpCloseHandle(socket_);
            socket_ = nullptr;
        }
        if (request_) {
            WinHttpCloseHandle(request_);
            request_ = nullptr;
        }
        if (connection_) {
            WinHttpCloseHandle(connection_);
            connection_ = nullptr;
        }
        if (session_) {
            WinHttpCloseHandle(session_);
            session_ = nullptr;
        }
    }

    HINTERNET session_ = nullptr;
    HINTERNET connection_ = nullptr;
    HINTERNET request_ = nullptr;
    HINTERNET socket_ = nullptr;
    bool send_shutdown_ = false;
};


std::wstring BuildServerLabel(const std::wstring& city, const std::wstring& country, const std::wstring& machine) {
    if (!city.empty() && !country.empty()) return city + L", " + country;
    if (!city.empty()) return city;
    if (!country.empty()) return country;
    return machine.empty() ? L"M-Lab" : machine;
}

void AppendError(std::wstring& combined, const wchar_t* phase, const std::wstring& detail) {
    if (detail.empty()) return;
    if (!combined.empty()) combined += L"; ";
    combined += phase;
    combined += L": ";
    combined += detail;
}

} // namespace

SpeedTester::SpeedTester() {
    result_.server_name = L"";
    result_.server_city = L"";
}

SpeedTester::~SpeedTester() {
    is_running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

std::wstring SpeedTester::GetStatusString() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_string_;
}

SpeedTestResult SpeedTester::GetResult() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return result_;
}

void SpeedTester::SetStatus(const std::wstring& status) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_string_ = status;
}

void SpeedTester::SetResult(const SpeedTestResult& res) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    result_ = res;
}

void SpeedTester::CancelTest() {
    if (is_running_.load()) {
        is_running_.store(false);
        SetStatus(L"Speed test cancelled by user");
        progress_ = 0;
        live_speed_ = 0.0;
        SpeedTestResult res = GetResult();
        res.error_message = L"Test cancelled by user";
        SetResult(res);
    }
}

void SpeedTester::StartTest() {
    bool expected = false;
    if (!is_running_.compare_exchange_strong(expected, true)) return;

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    progress_ = 0;
    live_speed_ = 0.0;
    SetStatus(L"Preparing M-Lab test...");
    SetResult(SpeedTestResult());

    try {
        worker_thread_ = std::thread([this]() {
            try {
                RunSpeedTest();
            } catch (...) {
                SpeedTestResult failed = GetResult();
                failed.error_message = L"Unexpected error while running the M-Lab test";
                SetResult(failed);
                SetStatus(L"M-Lab test failed");
                progress_ = 100;
                live_speed_ = 0.0;
                is_running_ = false;
            }
        });
    } catch (...) {
        SpeedTestResult failed;
        failed.error_message = L"Unable to start the M-Lab worker thread";
        SetResult(failed);
        SetStatus(L"M-Lab test could not start");
        is_running_ = false;
    }
}

bool SpeedTester::QueryMLabServer(
    std::wstring& dl_url,
    std::wstring& ul_url,
    std::wstring& server_name,
    std::wstring& server_city,
    std::wstring& error
) {
    HINTERNET session = WinHttpOpen(
        kUserAgent,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!session) {
        error = MakeWinHttpError(L"Opening M-Lab locator failed", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs);

    HINTERNET connection = WinHttpConnect(session, kMlabLocateHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        error = MakeWinHttpError(L"Connecting to M-Lab locator failed", GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        kMlabLocatePath,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!request) {
        error = MakeWinHttpError(L"Opening M-Lab locator request failed", GetLastError());
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == TRUE &&
        WinHttpReceiveResponse(request, nullptr) == TRUE;
    if (!ok) {
        error = MakeWinHttpError(L"M-Lab locator request failed", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status_code = 0;
    if (!GetHttpStatus(request, status_code)) {
        error = MakeWinHttpError(L"Reading M-Lab locator status failed", GetLastError());
    } else if (status_code < 200 || status_code >= 300) {
        std::wstringstream message;
        message << L"M-Lab locator returned HTTP " << status_code;
        error = message.str();
    }

    std::string response;
    if (error.empty() && !ReadHttpResponse(request, response, error)) {
        // ReadHttpResponse provides the error message.
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!error.empty()) return false;

    const std::wstring json = Utf8ToWide(response);
    if (json.empty()) {
        error = L"M-Lab locator returned an unreadable response";
        return false;
    }

    if (!ExtractJsonString(json, L"wss:///ndt/v7/download", dl_url) ||
        !ExtractJsonString(json, L"wss:///ndt/v7/upload", ul_url)) {
        error = L"M-Lab did not return secure NDT7 test endpoints";
        return false;
    }

    std::wstring machine;
    std::wstring hostname;
    std::wstring city;
    std::wstring country;
    ExtractJsonString(json, L"machine", machine);
    ExtractJsonString(json, L"hostname", hostname);
    ExtractJsonString(json, L"city", city);
    ExtractJsonString(json, L"country", country);

    server_name = BuildServerLabel(city, country, hostname.empty() ? machine : hostname);
    server_city = L"M-Lab NDT7";
    return true;
}

network_test::TransferResult SpeedTester::RunDownloadTest(const std::wstring& url, double& min_rtt_ms) {
    Ndt7WebSocket socket;
    std::wstring issues;
    if (!socket.Connect(url, issues)) return network_test::FinishTransfer(false, 0, 0.0, 0.0, issues);

    const auto started = std::chrono::steady_clock::now();
    uint64_t total_bytes = 0;
    double server_mbps = 0.0;
    network_test::MeasurementText text_message;
    std::array<char, kTransferChunkBytes> buffer{};

    while (is_running_) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (elapsed >= kTestWindowSeconds) break;

        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD receive_result = socket.Receive(buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, &buffer_type);
        if (receive_result != NO_ERROR) {
            network_test::AppendIssue(issues, MakeWinHttpError(L"M-Lab download stream ended with an error", receive_result));
            break;
        }
        if (buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            network_test::AppendIssue(issues, socket.CloseIssue());
            break;
        }
        if (buffer_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            buffer_type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            total_bytes += bytes_read;
        } else if (buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                   buffer_type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            const auto text_result = text_message.Append(buffer.data(), bytes_read,
                buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE);
            if (text_result == network_test::MeasurementText::Result::TooLarge) {
                network_test::AppendIssue(issues, L"NDT7 measurement message exceeded the size limit");
                break;
            }
            if (text_result == network_test::MeasurementText::Result::Complete) {
                network_test::UpdateMeasurement(Utf8ToWide(text_message.Text()), server_mbps, min_rtt_ms);
                text_message.Clear();
            }
        }

        const double current_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (current_elapsed > 0.05) {
            live_speed_ = network_test::GoodputMbps(static_cast<double>(total_bytes), current_elapsed);
            progress_ = 10 + static_cast<int>((std::min)(40.0, (current_elapsed / kTestWindowSeconds) * 40.0));
        }
    }

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!text_message.Empty()) network_test::AppendIssue(issues, L"Incomplete NDT7 measurement message");
    return network_test::FinishTransfer(false, total_bytes, elapsed, server_mbps, issues);
}

network_test::TransferResult SpeedTester::RunUploadTest(const std::wstring& url, double& min_rtt_ms) {
    Ndt7WebSocket socket;
    std::wstring issues;
    if (!socket.Connect(url, issues)) return network_test::FinishTransfer(true, 0, 0.0, 0.0, issues);

    const auto started = std::chrono::steady_clock::now();
    std::array<char, kTransferChunkBytes> payload{};
    uint64_t total_bytes = 0;
    double server_mbps = 0.0;
    min_rtt_ms = 0.0;

    // One sender and one receiver may run concurrently. Reader-owned measurements
    // and issues are consumed only after join; reader_done also stops the sender.
    std::atomic<bool> reader_done{ false };
    std::wstring receive_issues;
    std::thread response_reader([&]() {
        try {
            std::array<char, kTransferChunkBytes> receive_buffer{};
            network_test::MeasurementText text_message;
            bool peer_closed = false;
            while (is_running_) {
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                if (elapsed >= kTestWindowSeconds + 3.0) break;
                DWORD bytes_read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
                const DWORD receive_result = socket.Receive(receive_buffer.data(),
                    static_cast<DWORD>(receive_buffer.size()), &bytes_read, &buffer_type);
                if (receive_result != NO_ERROR) {
                    network_test::AppendIssue(receive_issues,
                        MakeWinHttpError(L"M-Lab upload measurement stream ended with an error", receive_result));
                    break;
                }
                if (buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                    peer_closed = true;
                    network_test::AppendIssue(receive_issues, socket.CloseIssue());
                    break;
                }
                if (buffer_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                    buffer_type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
                    network_test::AppendIssue(receive_issues, L"Unexpected binary data from the upload server");
                    break;
                }
                if (buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                    buffer_type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
                    const auto text_result = text_message.Append(receive_buffer.data(), bytes_read,
                        buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE);
                    if (text_result == network_test::MeasurementText::Result::TooLarge) {
                        network_test::AppendIssue(receive_issues, L"NDT7 measurement message exceeded the size limit");
                        break;
                    }
                    if (text_result == network_test::MeasurementText::Result::Complete) {
                        network_test::UpdateMeasurement(Utf8ToWide(text_message.Text()), server_mbps, min_rtt_ms);
                        text_message.Clear();
                    }
                }
            }
            if (!peer_closed) network_test::AppendIssue(receive_issues, L"Upload ended without a server close; summary may be incomplete");
            if (!text_message.Empty()) network_test::AppendIssue(receive_issues, L"Incomplete NDT7 measurement message");
        } catch (...) {
            receive_issues = L"Unexpected error while reading upload measurements";
        }
        reader_done = true;
    });

    // Ensure no exceptional exit can destroy a joinable std::thread. Shutdown
    // leaves Receive open so the reader can consume the final summary and close.
    double send_elapsed = 0.0;
    try {
        while (is_running_ && !reader_done.load()) {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (elapsed >= kTestWindowSeconds) break;
            const DWORD send_result = socket.SendBinary(payload.data(), static_cast<DWORD>(payload.size()));
            if (send_result != NO_ERROR) {
                network_test::AppendIssue(issues, MakeWinHttpError(L"M-Lab upload stream ended with an error", send_result));
                break;
            }
            total_bytes += payload.size();
            const double current_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (current_elapsed > 0.05) {
                live_speed_ = network_test::GoodputMbps(static_cast<double>(total_bytes), current_elapsed);
                progress_ = 55 + static_cast<int>((std::min)(40.0, (current_elapsed / kTestWindowSeconds) * 40.0));
            }
        }
        // Capture before shutdown/join: waiting for the reader is not sending time.
        send_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const DWORD shutdown_result = socket.ShutdownSend();
        if (shutdown_result != NO_ERROR) {
            network_test::AppendIssue(issues, MakeWinHttpError(L"Closing the upload send channel failed", shutdown_result));
        }
    } catch (...) {
        socket.ShutdownSend();
        response_reader.join();
        throw;
    }
    // This remains synchronous, not a hard deadline: ShutdownSend requests a peer
    // close but cannot guarantee a blocked Receive returns. Do not advertise cancel.
    response_reader.join();
    network_test::AppendIssue(issues, receive_issues);
    return network_test::FinishTransfer(true, total_bytes, send_elapsed, server_mbps, issues);
}

void SpeedTester::RunSpeedTest() {
    SetStatus(L"Locating M-Lab server...");
    progress_ = 5;

    std::wstring download_url;
    std::wstring upload_url;
    std::wstring server_name;
    std::wstring server_city;
    std::wstring locate_error;
    if (!QueryMLabServer(download_url, upload_url, server_name, server_city, locate_error)) {
        SpeedTestResult failed;
        failed.server_name = L"M-Lab unavailable";
        failed.error_message = locate_error;
        failed.completed_at_unix_seconds = static_cast<uint64_t>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
        );
        SetResult(failed);
        SetStatus(L"M-Lab server location failed");
        progress_ = 100;
        live_speed_ = 0.0;
        is_running_ = false;
        return;
    }

    SpeedTestResult result = GetResult();
    result.server_name = server_name;
    result.server_city = server_city;
    SetResult(result);

    SetStatus(L"Testing download speed...");
    progress_ = 10;
    double download_rtt = 0.0;
    const auto download = RunDownloadTest(download_url, download_rtt);
    if (!is_running_) return;

    result = GetResult();
    result.download_speed_mbps = download.speed_mbps;
    result.download_state = download.state;
    AppendError(result.error_message, L"Download", download.error);
    AppendError(result.warning_message, L"Download", download.warning);
    result.latency_ms = download_rtt;
    SetResult(result);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!is_running_) return;

    SetStatus(L"Testing upload speed...");
    progress_ = 55;
    double upload_rtt = 0.0;
    const auto upload = RunUploadTest(upload_url, upload_rtt);
    if (!is_running_) return;

    result = GetResult();
    result.upload_speed_mbps = upload.speed_mbps;
    result.upload_state = upload.state;
    result.upload_server_confirmed = upload.receiver_confirmed;
    if (result.latency_ms <= 0.0 && upload_rtt > 0.0) {
        result.latency_ms = upload_rtt;
    }
    AppendError(result.error_message, L"Upload", upload.error);
    AppendError(result.warning_message, L"Upload", upload.warning);
    result.completed_at_unix_seconds = static_cast<uint64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
    );
    SetResult(result);

    SetStatus(!result.error_message.empty() ? L"M-Lab test completed with a failed phase" :
        (!result.warning_message.empty() ? L"M-Lab test completed with warnings" : L"Test completed"));
    progress_ = 100;
    live_speed_ = 0.0;
    is_running_ = false;
}
