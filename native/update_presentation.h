#pragma once

#include <algorithm>

namespace WatchmanUpdates::Presentation {
inline constexpr float footer_font_size = 9.0f;
inline constexpr wchar_t footer_text[] = L"Update available  \u00b7  Download now";
inline constexpr wchar_t footer_short_text[] = L"Update  \u00b7  Download now";
inline constexpr wchar_t body_text[] = L"Get the latest Watchman from the official release page.\nNothing is installed automatically.";
struct Rect { float left = 0, top = 0, right = 0, bottom = 0; };
inline bool Contains(Rect r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
struct Layout { Rect card, close, icon, eyebrow, title, version, installed, body, download, later, note; };
inline Layout GetLayout(float width, float height) {
    const float w = (std::min)(500.0f, (std::max)(1.0f, width - 32.0f));
    const float h = (std::min)(460.0f, (std::max)(1.0f, height - 48.0f));
    const float x = (width - w) / 2, y = (height - h) / 2, pad = 28;
    Layout l;
    l.card = {x, y, x + w, y + h};
    l.close = {x + w - 60, y + 16, x + w - 16, y + 60};
    l.icon = {x + pad, y + 30, x + pad + 64, y + 94};
    l.eyebrow = {x + pad, y + 111, x + w - pad, y + 130};
    l.title = {x + pad, y + 138, x + w - pad, y + 174};
    l.version = {x + pad, y + 185, x + w - pad, y + 215};
    l.installed = {x + pad, y + 219, x + w - pad, y + 241};
    l.body = {x + pad, y + 264, x + w - pad, y + h - 116};
    l.download = {x + pad, y + h - 94, x + w - pad - 128, y + h - 50};
    l.later = {l.download.right + 12, l.download.top, x + w - pad, l.download.bottom};
    l.note = {x + pad, y + h - 36, x + w - pad, y + h - 16};
    return l;
}
inline Rect FooterLink(float width, float height) { return {160, height - 28, width - 144, height - 4}; }
inline const wchar_t* FooterText(float width) { return width >= 520 ? footer_text : footer_short_text; }
} // namespace WatchmanUpdates::Presentation
