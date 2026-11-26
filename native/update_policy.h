#pragma once

#include "app_version.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// Public stable releases only. No updater/installer or arbitrary remote URLs.
namespace WatchmanUpdates {
inline constexpr wchar_t api_host[] = L"api.github.com";
inline constexpr wchar_t api_path[] = L"/repos/rayhan138/Watchman/releases/latest";
inline constexpr char release_prefix[] = "https://github.com/rayhan138/Watchman/releases/tag/";
inline constexpr wchar_t channel_url[] = L"https://github.com/rayhan138/Watchman/releases/latest";
inline constexpr std::size_t max_response_bytes = 256 * 1024;
inline constexpr uint64_t initial_delay_ms = 15000;
inline constexpr uint64_t check_interval_ms = 24ULL * 60 * 60 * 1000;
inline constexpr uint64_t failure_retry_ms = 60ULL * 60 * 1000;
inline constexpr uint64_t manual_cooldown_ms = 60000;

using Version = WatchmanVersion::Number;
inline auto Components(const Version& v) { return std::make_tuple(v.major, v.minor, v.patch, v.revision); }
inline bool Newer(const Version& a, const Version& b) { return Components(a) > Components(b); }
inline std::string VersionKey(const Version& v) {
    std::string text = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    if (v.revision) text += "." + std::to_string(v.revision);
    return text;
}
inline std::wstring VersionText(const Version& v) {
    const auto text = VersionKey(v);
    return {text.begin(), text.end()};
}
inline std::optional<Version> ParseVersion(std::string_view text) {
    if (text.empty() || text.size() > 24) return std::nullopt;
    if (text.front() == 'v' || text.front() == 'V') text.remove_prefix(1);
    std::array<std::uint16_t, 4> parts{};
    std::size_t count = 0;
    while (!text.empty() && count < parts.size()) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (part.empty() || part.size() > 5 || (part.size() > 1 && part.front() == '0')) return std::nullopt;
        unsigned value = 0;
        for (const char ch : part) {
            if (ch < '0' || ch > '9') return std::nullopt;
            value = value * 10 + static_cast<unsigned>(ch - '0');
        }
        if (value > 65535) return std::nullopt;
        parts[count++] = static_cast<std::uint16_t>(value);
        if (dot == std::string_view::npos) { text = {}; break; }
        text.remove_prefix(dot + 1);
        if (text.empty()) return std::nullopt;
    }
    if (!text.empty() || count < 3) return std::nullopt;
    return Version{parts[0], parts[1], parts[2], parts[3]};
}

struct Release {
    Version version{};
    std::string tag;
    std::wstring Url() const {
        // The tag has already passed ParseVersion. Never launch a URL from
        // release notes, an asset, a redirect or an unvalidated API field.
        if (!ParseVersion(tag)) return {};
        const std::string url = std::string(release_prefix) + tag;
        return {url.begin(), url.end()};
    }
};
enum class Status { NotChecked, Checking, Available, UpToDate, NoRelease, Unavailable, UnknownVersion };
struct Result {
    Status status = Status::NotChecked;
    std::optional<Release> release;
    uint32_t error = 0;
};

// Strict, bounded parsing is implemented separately so malformed network
// data exercises the exact same code on Windows and in portable tests.
Result EvaluateResponse(unsigned http_status, std::string_view body, const std::optional<Version>& current);

class Notice {
public:
    void Load(std::string notified, std::string dismissed) {
        notified_ = std::move(notified); dismissed_ = std::move(dismissed);
    }
    void Apply(const Result& result) {
        last_ = result;
        if (result.status == Status::Available && result.release) available_ = result.release;
        else if (result.status == Status::UpToDate || result.status == Status::NoRelease ||
                 result.status == Status::UnknownVersion) available_.reset();
        // A transient offline/rate-limit error must not pretend a previously
        // confirmed update vanished. Tools still reports that the recheck failed.
    }
    const Result& Last() const { return last_; }
    const std::optional<Release>& Available() const { return available_; }
    bool ShouldNotify() const { return available_ && Key() != notified_; }
    bool MayPrompt() const { return available_ && Key() != dismissed_; }
    std::string Key() const { return available_ ? VersionKey(available_->version) : std::string{}; }
    void Notified() { notified_ = Key(); }
    void Dismissed() { dismissed_ = Key(); }
private:
    Result last_;
    std::optional<Release> available_;
    std::string notified_, dismissed_;
};

class Schedule {
public:
    explicit Schedule(uint64_t now = 0) : next_(now + initial_delay_ms) {}
    bool Due(uint64_t now) const { return !pending_ && now >= next_; }
    bool Begin(uint64_t now, bool manual) {
        if (pending_ || (manual ? attempted_ && now - last_attempt_ < manual_cooldown_ms : now < next_)) return false;
        pending_ = true; attempted_ = true; last_attempt_ = now; return true;
    }
    void Complete(uint64_t now, bool success) {
        pending_ = false; next_ = now + (success ? check_interval_ms : failure_retry_ms);
    }
    bool Pending() const { return pending_; }
private:
    uint64_t next_ = 0, last_attempt_ = 0;
    bool pending_ = false, attempted_ = false;
};

inline std::wstring StatusText(const Result& result) {
    switch (result.status) {
    case Status::Checking: return L"Checking the official release channel...";
    case Status::Available: return result.release ? L"Update available: " + VersionText(result.release->version) : L"Could not verify release information.";
    case Status::UpToDate: return L"You're up to date (stable channel).";
    case Status::NoRelease: return L"No published stable release was found.";
    case Status::UnknownVersion: return L"Installed version unavailable.";
    case Status::Unavailable: return L"Couldn't check for updates. Try again later.";
    default: return L"Not checked yet.";
    }
}
} // namespace WatchmanUpdates
