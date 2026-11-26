#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>

// Pure protocol/result logic: no Windows APIs, sockets, clocks, or storage.
namespace network_test {

class RunningReset {
public:
    explicit RunningReset(std::atomic<bool>& running) : running_(running) {}
    ~RunningReset() { running_.store(false); }
    RunningReset(const RunningReset&) = delete;
    RunningReset& operator=(const RunningReset&) = delete;
private:
    std::atomic<bool>& running_;
};

enum class TransferState { NotRun, Complete, Partial, Failed };

inline bool IsNdt7Subprotocol(const std::wstring& protocol) {
    const size_t first = protocol.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const size_t last = protocol.find_last_not_of(L" \t\r\n");
    return protocol.substr(first, last - first + 1) == L"net.measurementlab.ndt.v7";
}

struct TransferResult {
    double speed_mbps = 0.0;
    TransferState state = TransferState::NotRun;
    bool receiver_confirmed = false;
    std::wstring error;
    std::wstring warning;
};

inline void AppendIssue(std::wstring& issues, const std::wstring& issue) {
    if (issue.empty()) return;
    if (!issues.empty()) issues += L"; ";
    issues += issue;
}

inline double GoodputMbps(double bytes, double elapsed_seconds) {
    if (!std::isfinite(bytes) || !std::isfinite(elapsed_seconds) ||
        bytes <= 0.0 || elapsed_seconds <= 0.0) return 0.0;
    const double speed = (bytes / elapsed_seconds) * (8.0 / 1'000'000.0);
    return std::isfinite(speed) ? speed : 0.0;
}

inline double RttMilliseconds(double microseconds) {
    return std::isfinite(microseconds) && microseconds > 0.0 ? microseconds / 1000.0 : 0.0;
}

inline TransferResult FinishTransfer(bool upload, uint64_t local_bytes,
    double transfer_seconds, double server_mbps, std::wstring issues) {
    TransferResult result;
    const double local_mbps = GoodputMbps(static_cast<double>(local_bytes), transfer_seconds);
    if (local_mbps <= 0.0) {
        result.state = TransferState::Failed;
        result.error = issues.empty() ? L"No application payload was transferred" : std::move(issues);
        return result;
    }
    // Download is measured at the receiving client; upload prefers the server's receiver.
    const bool server_confirmed = std::isfinite(server_mbps) && server_mbps > 0.0;
    result.speed_mbps = upload && server_confirmed ? server_mbps : local_mbps;
    result.receiver_confirmed = !upload || server_confirmed;
    if (upload && !server_confirmed) {
        AppendIssue(issues, L"Upload is a local sending estimate, not confirmed by the server");
    }
    result.warning = std::move(issues);
    result.state = result.warning.empty() ? TransferState::Complete : TransferState::Partial;
    return result;
}

inline bool SuccessfulEcho(uint32_t reply_count, uint32_t reply_status) {
    // Windows IP_SUCCESS is zero. A reply count alone can include ICMP error replies.
    return reply_count > 0 && reply_status == 0;
}

inline const wchar_t* NoPingResponseLabel(uint32_t sent, uint32_t probe_error) {
    if (probe_error != 0) return L"Probe unavailable";
    return sent == 0 ? L"Not measured" : L"No ICMP response";
}

inline bool JsonWhitespace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

// Find only direct, unescaped protocol member names, not similarly named nested fields.
inline size_t MemberValue(const std::wstring& json, const std::wstring& key) {
    const std::wstring quoted_key = L"\"" + key + L"\"";
    int depth = 0;
    for (size_t pos = 0; pos < json.size(); ++pos) {
        const wchar_t ch = json[pos];
        if (ch == L'\"') {
            const size_t start = pos;
            bool escaped = false;
            for (++pos; pos < json.size(); ++pos) {
                if (escaped) escaped = false;
                else if (json[pos] == L'\\') escaped = true;
                else if (json[pos] == L'\"') break;
            }
            if (pos == json.size()) return std::wstring::npos;
            size_t value = pos + 1;
            while (value < json.size() && JsonWhitespace(json[value])) ++value;
            if (depth == 1 && json.compare(start, pos - start + 1, quoted_key) == 0 &&
                value < json.size() && json[value] == L':') {
                ++value;
                while (value < json.size() && JsonWhitespace(json[value])) ++value;
                return value;
            }
        } else if (ch == L'{' || ch == L'[') {
            ++depth;
        } else if (ch == L'}' || ch == L']') {
            --depth;
        }
    }
    return std::wstring::npos;
}

inline bool ExtractJsonObject(const std::wstring& json, const std::wstring& key, std::wstring& value) {
    size_t pos = MemberValue(json, key);
    if (pos == std::wstring::npos || pos >= json.size() || json[pos] != L'{') return false;
    const size_t start = pos;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const wchar_t ch = json[pos];
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == L'\\') escaped = true;
            else if (ch == L'\"') in_string = false;
        } else if (ch == L'\"') {
            in_string = true;
        } else if (ch == L'{') {
            ++depth;
        } else if (ch == L'}' && --depth == 0) {
            value = json.substr(start, pos - start + 1);
            return true;
        }
    }
    return false;
}

inline bool ExtractJsonNumber(const std::wstring& json, const std::wstring& key, double& value) {
    const size_t pos = MemberValue(json, key);
    if (pos == std::wstring::npos || pos >= json.size() ||
        (json[pos] != L'-' && (json[pos] < L'0' || json[pos] > L'9'))) return false;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos || !std::isfinite(parsed)) return false;
    size_t after = static_cast<size_t>(end - json.c_str());
    while (after < json.size() && JsonWhitespace(json[after])) ++after;
    if (after >= json.size() || (json[after] != L',' && json[after] != L'}')) return false;
    value = parsed;
    return true;
}

inline void UpdateMeasurement(const std::wstring& message, double& server_mbps, double& min_rtt_ms,
    bool upload_receiver = false) {
    std::wstring app_info;
    double bytes = 0.0;
    double elapsed_us = 0.0;
    bool application_sample = false;
    if (ExtractJsonObject(message, L"AppInfo", app_info) &&
        ExtractJsonNumber(app_info, L"NumBytes", bytes) &&
        ExtractJsonNumber(app_info, L"ElapsedTime", elapsed_us)) {
        const double speed = GoodputMbps(bytes, elapsed_us / 1'000'000.0);
        if (speed > 0.0) {
            server_mbps = speed; // Latest cumulative sample, never a sum.
            application_sample = true;
        }
    }
    std::wstring tcp_info;
    double raw_rtt = 0.0;
    if (!ExtractJsonObject(message, L"TCPInfo", tcp_info)) return;
    // Current ndt-server publishes receiver TCPInfo, often without AppInfo.
    // Only upload can use the server's BytesReceived; BytesSent/BytesAcked or
    // client-direction download counters must never confirm an upload result.
    if (upload_receiver && !application_sample &&
        ExtractJsonNumber(tcp_info, L"BytesReceived", bytes) &&
        ExtractJsonNumber(tcp_info, L"ElapsedTime", elapsed_us)) {
        const double speed = GoodputMbps(bytes, elapsed_us / 1'000'000.0);
        if (speed > 0.0) server_mbps = speed;
    }
    if (!ExtractJsonNumber(tcp_info, L"MinRTT", raw_rtt) || raw_rtt <= 0.0) {
        ExtractJsonNumber(tcp_info, L"RTT", raw_rtt);
    }
    const double rtt = RttMilliseconds(raw_rtt);
    if (rtt > 0.0 && (min_rtt_ms <= 0.0 || rtt < min_rtt_ms)) min_rtt_ms = rtt;
}

class MeasurementText {
public:
    enum class Result { Pending, Complete, TooLarge };
    // NDT7 implementations must allow messages of at least 2^24 bytes.
    static constexpr size_t kMaxBytes = 1u << 24;
    Result Append(const char* bytes, size_t count, bool final_fragment) {
        if (count > kMaxBytes - text_.size()) {
            text_.clear();
            return Result::TooLarge;
        }
        text_.append(bytes, count);
        return final_fragment ? Result::Complete : Result::Pending;
    }
    const std::string& Text() const { return text_; }
    bool Empty() const { return text_.empty(); }
    void Clear() { text_.clear(); }
private:
    std::string text_;
};

} // namespace network_test
