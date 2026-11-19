#pragma once

#include <algorithm>
#include <cstdint>

namespace FloatingMonitorLayout {
inline constexpr int minimum_font_size = 11;
inline constexpr int maximum_font_size = 16;
inline constexpr int default_font_size = 12;
inline constexpr int width = 188;
inline constexpr int network_height = 28;
// The compact monitor is network-only, independent of the app theme.
inline constexpr std::uint32_t default_text_color = 0x00FFFFFFu;
inline constexpr std::uint32_t panel_background_color = 0x00000000u;
inline constexpr int minimum_opacity = 20;
inline constexpr int maximum_opacity = 100;

inline int ClampFontSize(int value) {
    return std::clamp(value, minimum_font_size, maximum_font_size);
}

inline int Width(int font_size) {
    return (width * ClampFontSize(font_size) + default_font_size - 1) / default_font_size;
}

inline int Height(int font_size = default_font_size) {
    return std::max(network_height, ClampFontSize(font_size) + 12);
}

inline int ClampOpacity(int value) {
    return std::clamp(value, minimum_opacity, maximum_opacity);
}

struct Rect {
    int left, top, right, bottom;
};

struct Layout {
    Rect upload_label, upload_value, download_label, download_value;
};

inline Layout Get(int surface_width, int surface_height, int font_size = default_font_size) {
    const int safe_width = std::max(0, surface_width);
    const int safe_height = std::max(0, surface_height);
    const auto x = [safe_width](int coordinate) {
        return static_cast<int>(static_cast<std::int64_t>(coordinate) * safe_width / width);
    };
    const int network_bottom = std::min(Height(font_size), safe_height);
    const int network_top = std::min(2, network_bottom);
    const int text_bottom = std::max(network_top, network_bottom - 2);
    // Keep full-size rate fields; compact labels and group spacing, not the font.
    return {
        { x(6), network_top, x(26), text_bottom }, { x(30), network_top, x(90), text_bottom },
        { x(98), network_top, x(118), text_bottom }, { x(122), network_top, x(182), text_bottom }
    };
}

// UpdateLayeredWindow requires premultiplied BGRA, not an ordinary GDI RGB pixel.
inline std::uint32_t PremultipliedPixel(unsigned red, unsigned green, unsigned blue, unsigned alpha) {
    const auto channel = [alpha](unsigned value) { return (value * alpha + 127u) / 255u; };
    return channel(blue) | (channel(green) << 8u) | (channel(red) << 16u) | (alpha << 24u);
}
} // namespace FloatingMonitorLayout
