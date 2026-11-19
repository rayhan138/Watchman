#pragma once

#include <algorithm>

namespace WarningsSettingsText {
inline constexpr wchar_t sensor_setup[] =
    L"GPU: Windows driver, when supported. For CPU, disk and mainboard, run LibreHardwareMonitor "
    L"with its web server bound to 127.0.0.1:8085. Keep it running. No LAN access is needed. "
    L"Missing sensors show Unavailable; Watchman installs no driver.";
inline constexpr wchar_t alert_details[] =
    L"Hottest device per category. CPU prefers package/die, then hottest core. "
    L"Alerts use fresh readings only; re-arm 5 C/points below, with a 5-minute cooldown. "
    L"Traffic uses saved MiB/GiB. Windows may suppress notifications.";
} // namespace WarningsSettingsText

// Shared draw/hit geometry. Titles stay fixed; only cards scroll.
struct WarningsSettingsLayout {
    float card_left = 0, card_right = 0;
    float pad = 14, toggle_w = 40, toggle_h = 22;
    float row_h = 96, control_h = 32;
    float title_y = 0, subtitle_y = 0;
    float alert_top = 0, alert_bot = 0, section_label = 0;
    float threshold_top = 0, threshold_bot = 0;
    static constexpr int rule_count = 6;
    float row[rule_count]{};
    float sensors_top = 0, sensors_bot = 0, info_top = 0, info_bot = 0;
    float sensors_text_top = 0, info_text_top = 0;
    float toggle_left = 0, toggle_right = 0;
    float input_left = 0, input_right = 0, unit_left = 0, unit_right = 0;
};
inline WarningsSettingsLayout GetWarningsSettingsLayout(float left, float right, float modal_top, float scroll = 0,
                                                       float setup_text_height = 134, float info_text_height = 110) {
    WarningsSettingsLayout w;
    w.card_left = left;
    w.card_right = right;
    w.title_y = modal_top + 84;
    w.subtitle_y = modal_top + 117;
    w.alert_top = modal_top + 156 - scroll;
    w.alert_bot = w.alert_top + 90;
    w.section_label = w.alert_bot + 16;
    w.threshold_top = w.section_label + 28;
    for (int i = 0; i < w.rule_count; ++i) w.row[i] = w.threshold_top + 12 + i * w.row_h;
    w.threshold_bot = w.row[w.rule_count - 1] + w.row_h + 4;
    w.sensors_top = w.threshold_bot + 16;
    w.sensors_text_top = w.sensors_top + 42;
    w.sensors_bot = w.sensors_text_top + (std::max)(0.0f, setup_text_height) + 14;
    w.info_top = w.sensors_bot + 16;
    w.info_text_top = w.info_top + 14;
    w.info_bot = w.info_text_top + (std::max)(0.0f, info_text_height) + 14;
    w.toggle_right = right - w.pad;
    w.toggle_left = w.toggle_right - w.toggle_w;
    w.input_left = left + w.pad;
    w.input_right = w.input_left + 72;
    w.unit_left = w.input_right + 8;
    w.unit_right = w.unit_left + 46;
    return w;
}
