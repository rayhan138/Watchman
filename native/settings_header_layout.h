#pragma once

#include <algorithm>
#include <cmath>

namespace SettingsHeaderLayout {

inline constexpr float height = 62.0f;
inline constexpr float version_font_size = 17.0f;
inline constexpr float horizontal_padding = 12.0f;
inline constexpr float gap = 12.0f;

struct Rect {
    float left = 0, top = 0, right = 0, bottom = 0;
};

struct Layout {
    Rect title, version, version_text;
    float version_font_scale = 1.0f;
    bool stacked = false;
};

// actions_left is the existing Quick tour button's left edge. Do not move its
// draw/hit target or the close button when making room for the version badge.
// Both widths are actual DirectWrite measurements with the embedded fonts.
inline Layout Get(float modal_left, float modal_top, float actions_left,
                  float title_text_width, float version_text_width) {
    Layout result;
    const float left = modal_left + 22.0f;
    const float right = actions_left - gap;
    const float wanted_width = (std::max)(132.0f,
        std::ceil((std::max)(0.0f, version_text_width)) + 2 * horizontal_padding);
    result.stacked = right - wanted_width - gap - left < std::ceil(title_text_width);
    if (result.stacked) {
        // Narrow dialogs / longer future versions use the left column's two
        // rows, still inside the existing 62-DIP header (no body displacement).
        result.title = {left, modal_top, right, modal_top + 33.0f};
        result.version = {left, modal_top + 34.0f,
            left + (std::min)(wanted_width, right - left), modal_top + 59.0f};
    } else {
        result.version = {right - wanted_width, modal_top + 16.0f, right, modal_top + 50.0f};
        result.title = {left, modal_top + 14.0f, result.version.left - gap, modal_top + 51.0f};
    }
    result.version_text = {result.version.left + horizontal_padding, result.version.top,
        result.version.right - horizontal_padding, result.version.bottom};
    const float available = result.version_text.right - result.version_text.left;
    if (version_text_width > available) {
        // Keep even unusually long four-component resource versions complete;
        // ordinary versions retain the full-size, bold 17-DIP type.
        result.version_font_scale = (std::max)(0.01f, (available - 0.5f) / version_text_width);
    }
    return result;
}

} // namespace SettingsHeaderLayout
