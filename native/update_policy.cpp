#include "update_policy.h"

namespace WatchmanUpdates {
namespace {
bool ValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        const unsigned n = first >= 0xC2 && first <= 0xDF ? 1 : first >= 0xE0 && first <= 0xEF ? 2 :
            first >= 0xF0 && first <= 0xF4 ? 3 : 0;
        if (!n || i + n > text.size()) return false;
        const auto second = static_cast<unsigned char>(text[i]);
        if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0) ||
            (first == 0xF0 && second < 0x90) || (first == 0xF4 && second >= 0x90)) return false;
        for (unsigned j = 0; j < n; ++j) if ((static_cast<unsigned char>(text[i++]) & 0xC0) != 0x80) return false;
    }
    return true;
}
void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
// A bounded JSON reader, not regex/string searching. Unknown release fields
// (including nested assets and notes) are validated/skipped, never displayed or
// executed. Only four uniquely named root fields are consumed.
class ReleaseJson {
public:
    explicit ReleaseJson(std::string_view text) : text_(text) {}
    bool Parse() {
        if (!Take('{')) return false;
        unsigned seen = 0;
        if (Take('}')) return false;
        do {
            std::string key;
            if (!String(key) || !Take(':')) return false;
            const unsigned bit = key == "tag_name" ? 1 : key == "html_url" ? 2 : key == "draft" ? 4 : key == "prerelease" ? 8 : 0;
            if (bit && (seen & bit)) return false;
            seen |= bit;
            if (bit == 1) { if (!String(tag)) return false; }
            else if (bit == 2) { if (!String(url)) return false; }
            else if (bit == 4) { if (!Boolean(draft)) return false; }
            else if (bit == 8) { if (!Boolean(prerelease)) return false; }
            else if (!Skip(0)) return false;
            if (Take('}')) { Space(); return pos_ == text_.size() && seen == 15; }
        } while (Take(','));
        return false;
    }
    std::string tag, url;
    bool draft = false, prerelease = false;
private:
    void Space() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_; }
    bool Take(char ch) { Space(); if (pos_ == text_.size() || text_[pos_] != ch) return false; ++pos_; return true; }
    bool Literal(std::string_view value) {
        Space(); if (text_.substr(pos_, value.size()) != value) return false; pos_ += value.size(); return true;
    }
    bool Boolean(bool& value) {
        if (Literal("true")) { value = true; return true; }
        if (Literal("false")) { value = false; return true; }
        return false;
    }
    bool Hex(unsigned& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == text_.size()) return false;
            const char ch = text_[pos_++];
            const unsigned digit = ch >= '0' && ch <= '9' ? static_cast<unsigned>(ch - '0') :
                ch >= 'a' && ch <= 'f' ? static_cast<unsigned>(ch - 'a') + 10 :
                ch >= 'A' && ch <= 'F' ? static_cast<unsigned>(ch - 'A') + 10 : 16;
            if (digit == 16) return false;
            value = value * 16 + digit;
        }
        return true;
    }
    bool String(std::string& out) {
        if (!Take('"')) return false;
        out.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') return true;
            if (static_cast<unsigned char>(ch) < 0x20) return false;
            if (ch != '\\') { out += ch; continue; }
            if (pos_ == text_.size()) return false;
            const char escape = text_[pos_++];
            if (escape == '"' || escape == '\\' || escape == '/') out += escape;
            else if (escape == 'b') out += '\b';
            else if (escape == 'f') out += '\f';
            else if (escape == 'n') out += '\n';
            else if (escape == 'r') out += '\r';
            else if (escape == 't') out += '\t';
            else if (escape == 'u') {
                unsigned cp = 0;
                if (!Hex(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (text_.substr(pos_, 2) != "\\u") return false;
                    pos_ += 2;
                    unsigned low = 0;
                    if (!Hex(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                AppendUtf8(out, cp);
            } else return false;
        }
        return false;
    }
    bool Skip(unsigned depth) {
        if (depth > 24) return false;
        Space();
        if (pos_ == text_.size()) return false;
        if (text_[pos_] == '"') { std::string unused; return String(unused); }
        if (Take('{')) {
            if (Take('}')) return true;
            do {
                std::string key;
                if (!String(key) || !Take(':') || !Skip(depth + 1)) return false;
                if (Take('}')) return true;
            } while (Take(','));
            return false;
        }
        if (Take('[')) {
            if (Take(']')) return true;
            do { if (!Skip(depth + 1)) return false; if (Take(']')) return true; } while (Take(','));
            return false;
        }
        bool unused = false;
        if (Boolean(unused) || Literal("null")) return true;
        // JSON numbers: no leading zero, mandatory digits after ./exponent.
        Take('-');
        if (pos_ == text_.size()) return false;
        if (text_[pos_] == '0') ++pos_;
        else {
            if (text_[pos_] < '1' || text_[pos_] > '9') return false;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const auto digits = [&] {
            const auto start = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            return pos_ > start;
        };
        if (pos_ < text_.size() && text_[pos_] == '.') { ++pos_; if (!digits()) return false; }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            if (!digits()) return false;
        }
        return true;
    }
    std::string_view text_;
    std::size_t pos_ = 0;
};
} // namespace

Result EvaluateResponse(unsigned http_status, std::string_view body, const std::optional<Version>& current) {
    if (!current) return {Status::UnknownVersion, {}, 0};
    if (http_status == 404) return {Status::NoRelease, {}, 404};
    if (http_status != 200) return {Status::Unavailable, {}, http_status};
    if (body.size() > max_response_bytes || !ValidUtf8(body)) return {Status::Unavailable, {}, 13};
    ReleaseJson json(body);
    if (!json.Parse()) return {Status::Unavailable, {}, 13};
    const auto version = ParseVersion(json.tag);
    if (!version || json.url != std::string(release_prefix) + json.tag) return {Status::Unavailable, {}, 13};
    if (json.draft || json.prerelease) return {Status::NoRelease, {}, 0};
    Release release{*version, json.tag};
    return {Newer(*version, *current) ? Status::Available : Status::UpToDate, release, 0};
}
} // namespace WatchmanUpdates
