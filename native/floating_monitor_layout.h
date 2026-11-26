#pragma once

#include <algorithm>
#include <cstdint>

namespace FloatingMonitorLayout {
inline constexpr int minimum_font_size = 11;
inline constexpr int maximum_font_size = 16;
inline constexpr int default_font_size = 12;
inline constexpr int width = 188;
inline constexpr int network_height = 28;
inline constexpr bool default_show_cpu = true;
inline constexpr bool default_show_memory = true;
// The floating monitor's white-on-black defaults are independent of app theme.
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

inline int RowHeight(int font_size) {
    return std::max(network_height, ClampFontSize(font_size) + 12);
}

inline int Height(int font_size = default_font_size,
                  bool show_cpu = default_show_cpu, bool show_memory = default_show_memory) {
    return RowHeight(font_size) * ((show_cpu || show_memory) ? 2 : 1);
}

inline int ClampOpacity(int value) {
    return std::clamp(value, minimum_opacity, maximum_opacity);
}

struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
};

struct Layout {
    Rect upload_label, upload_value, download_label, download_value;
    // Disabled readings have empty bounds. A single system reading is centered.
    Rect cpu_label, cpu_value, memory_label, memory_value;
};

inline Layout Get(int surface_width, int surface_height, int font_size = default_font_size,
                  bool show_cpu = default_show_cpu, bool show_memory = default_show_memory) {
    const int safe_width = std::max(0, surface_width);
    const int safe_height = std::max(0, surface_height);
    const auto x = [safe_width](int coordinate) {
        return static_cast<int>(static_cast<std::int64_t>(coordinate) * safe_width / width);
    };
    const int row_height = RowHeight(font_size);
    const int network_bottom = std::min(row_height, safe_height);
    const int network_top = std::min(2, network_bottom);
    const int text_bottom = std::max(network_top, network_bottom - 2);
    Layout layout{
        {x(6), network_top, x(26), text_bottom}, {x(30), network_top, x(90), text_bottom},
        {x(98), network_top, x(118), text_bottom}, {x(122), network_top, x(182), text_bottom},
        {}, {}, {}, {}
    };
    const int system_bottom = std::min(row_height * 2, safe_height);
    const int system_top = std::min(row_height + 2, system_bottom);
    const int bottom = std::max(system_top, system_bottom - 2);
    if (show_cpu) {
        layout.cpu_label = {x(show_memory ? 6 : 48), system_top, x(show_memory ? 34 : 80), bottom};
        layout.cpu_value = {x(show_memory ? 38 : 84), system_top, x(show_memory ? 90 : 140), bottom};
    }
    if (show_memory) {
        layout.memory_label = {x(show_cpu ? 98 : 48), system_top, x(show_cpu ? 130 : 80), bottom};
        layout.memory_value = {x(show_cpu ? 134 : 84), system_top, x(show_cpu ? 182 : 140), bottom};
    }
    return layout;
}

// UpdateLayeredWindow requires premultiplied BGRA, not an ordinary GDI RGB pixel.
inline std::uint32_t PremultipliedPixel(unsigned red, unsigned green, unsigned blue, unsigned alpha) {
    const auto channel = [alpha](unsigned value) { return (value * alpha + 127u) / 255u; };
    return channel(blue) | (channel(green) << 8u) | (channel(red) << 16u) | (alpha << 24u);
}
} // namespace FloatingMonitorLayout
