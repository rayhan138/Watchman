#pragma once

#include <algorithm>

// Main-window geometry only. Compact monitor layouts have their own type scale.
namespace MainContentLayout {
inline constexpr float tab_widths[] = {116.0f, 72.0f, 78.0f, 100.0f, 82.0f};
inline constexpr float tab_side = 10.0f;
inline constexpr float tab_gap = 4.0f;
struct TabItem { float left = 0.0f; float width = 0.0f; };
struct Tabbar { TabItem items[5]; };

inline Tabbar GetTabbar(float width) {
    float nominal = 0.0f;
    for (float item : tab_widths) nominal += item;
    const float available = (std::max)(1.0f, width - 2.0f * tab_side - 4.0f * tab_gap);
    const float scale = (std::min)(1.0f, available / nominal);
    Tabbar result{};
    float left = tab_side;
    for (int i = 0; i < 5; ++i) {
        result.items[i] = {left, tab_widths[i] * scale};
        left += result.items[i].width + tab_gap;
    }
    return result;
}

// Titlebar drawing, hit testing, and rapid-click eligibility share this
// contract. In particular, only the theme action repeats on a double-click;
// browser links, close/minimize and Settings must not fire twice.
struct Titlebar {
    float feedback_left = 0, feedback_right = 0;
    float globe_left = 0, gear_left = 0, theme_left = 0;
    float minimize_left = 0, close_left = 0;
    float action_size = 32.0f;
};
inline Titlebar GetTitlebar(float render_width) {
    Titlebar layout;
    constexpr float action_gap = 8.0f;
    layout.close_left = render_width - 16.0f - layout.action_size;
    layout.minimize_left = layout.close_left - action_gap - layout.action_size;
    layout.theme_left = layout.minimize_left - action_gap - layout.action_size;
    layout.gear_left = layout.theme_left - action_gap - layout.action_size;
    layout.globe_left = layout.gear_left - action_gap - layout.action_size;
    layout.feedback_right = layout.globe_left - (action_gap + 4.0f);
    layout.feedback_left = (std::max)(126.0f, layout.feedback_right - 84.0f);
    return layout;
}
inline int TitlebarButtonAt(float render_width, float x, float y) {
    const auto layout = GetTitlebar(render_width);
    if (x >= layout.feedback_left && x < layout.feedback_right && y >= 12.0f && y < 36.0f) return 0;
    if (y < 8.0f || y >= 40.0f) return -1;
    const float actions[] = {layout.globe_left, layout.gear_left, layout.theme_left,
        layout.minimize_left, layout.close_left};
    for (int index = 0; index < 5; ++index) {
        if (x >= actions[index] && x < actions[index] + layout.action_size) return index + 1;
    }
    return -1;
}
inline bool IsRepeatableTitlebarButton(int button) { return button == 3; }

inline constexpr float app_admin_button_width = 174.0f;

struct AppUsageRow { float bar_left, bar_right, total_left, total_right; };
inline AppUsageRow GetAppUsageRow(float left, float right) {
    const float total_width = (std::min)(88.0f, (std::max)(0.0f, right - left) * 0.48f);
    const float total_left = right - total_width;
    return {left, (std::max)(left, total_left - 10.0f), total_left, right};
}

struct AppColumns {
    float name_right;
    float download_right;
    float upload_right;
    float right;
};
inline AppColumns GetAppColumns(float left, float width) {
    // Status badges sit under names, leaving wider numeric columns for 11/12px text.
    return {left + width * 0.44f, left + width * 0.67f,
            left + width * 0.90f, left + width};
}
} // namespace MainContentLayout
