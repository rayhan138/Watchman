#include "main_content_layout.h"
#include "theme_picker_layout.h"
#include "warnings_settings_layout.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a - b) < 0.01f; }

void ThemeChoices() {
    for (float width : {272.0f, 300.0f, 332.0f, 440.0f}) {
        for (float top : {0.0f, 480.0f, -100.0f}) {
            const float left = 150.0f;
            const auto picker = ThemePickerLayout::Get(left, left + width, top);
            const float choice_width = picker.options[0].bounds.right - picker.options[0].bounds.left;
            for (int i = 0; i < ThemePickerLayout::option_count; ++i) {
                const auto& choice = picker.options[static_cast<size_t>(i)];
                const auto& bounds = choice.bounds;
                Check(Near(bounds.right - bounds.left, choice_width), "All three theme choices need equal width");
                Check(Near(bounds.top, picker.options[0].bounds.top) && Near(bounds.bottom, picker.options[0].bounds.bottom),
                    "Theme choice edges must share a baseline even after scrolling");
                Check(Near(choice.radio_y, (choice.text.top + choice.text.bottom) / 2),
                    "Radio circle must be vertically centered with its label");
                Check(bounds.left >= left + 20 && bounds.right <= left + width - 19.99f,
                    "Theme choices must retain the card's shared horizontal padding");
                Check(choice.radio_x - 6 > bounds.left && choice.radio_x + 6 < choice.text.left,
                    "Radio glyph needs internal padding and a real gap before text");
                Check(choice.text.left < choice.text.right && choice.text.right < bounds.right,
                    "Theme text must fit inside its own tile, not the next column");
                Check(ThemePickerLayout::HitTest(picker, choice.radio_x, choice.radio_y) == i,
                    "Clicking a drawn radio must select exactly that mode");
                Check(ThemePickerLayout::HitTest(picker, choice.text.right - 1, choice.radio_y) == i,
                    "The label and radio must share the same hit target");
                if (i + 1 < ThemePickerLayout::option_count) {
                    const auto& next = picker.options[static_cast<size_t>(i + 1)].bounds;
                    Check(Near(next.left - bounds.right, ThemePickerLayout::gap), "All theme tile gaps must match");
                    Check(ThemePickerLayout::HitTest(picker, (bounds.right + next.left) / 2, choice.radio_y) == -1,
                        "An inter-tile gap must not accidentally change the theme");
                }
            }
            Check(picker.note.top > picker.options[2].bounds.bottom && picker.note.bottom + 13.99f <= picker.bottom,
                "Auto explanation must have a separate row inside the card");
            Check(ThemePickerLayout::HitTest(picker, left + 25, picker.note.top + 1) == -1,
                "The Auto description must not be mistaken for another radio row");
            Check(ThemePickerLayout::HitTest(picker, left - 1, top + 55) == -1,
                "Outside-card clicks must not change the theme");
        }
    }
}

void RapidTitlebarClicks() {
    using namespace MainContentLayout;
    for (float width : {460.0f, 500.0f, 560.0f, 720.0f, 1000.0f}) {
        const auto bar = GetTitlebar(width);
        const float action_left[] = {bar.globe_left, bar.gear_left, bar.theme_left, bar.minimize_left, bar.close_left};
        for (int i = 0; i < 5; ++i) {
            const int button = TitlebarButtonAt(width, action_left[i] + bar.action_size / 2, 24);
            Check(button == i + 1, "Rendered titlebar action and hit target must agree");
            Check(IsRepeatableTitlebarButton(button) == (button == 3),
                "Only the theme action should treat the double-click's second press as another activation");
        }
        for (int press = 0; press < 100; ++press) {
            // WM_LBUTTONDOWN and WM_LBUTTONDBLCLK use this same classification.
            // No time or cooldown participates in accepting a theme press.
            Check(IsRepeatableTitlebarButton(TitlebarButtonAt(width, bar.theme_left + 16, 24)),
                "Every rapid theme press must remain eligible, without waiting");
        }
        Check(!IsRepeatableTitlebarButton(TitlebarButtonAt(width, bar.theme_left - 4, 24)),
            "Gaps between titlebar buttons must not activate the shortcut");
        Check(TitlebarButtonAt(width, bar.theme_left + 16, 4) == -1 &&
              TitlebarButtonAt(width, bar.theme_left + 16, 44) == -1,
            "Theme button hit target must not cover the resize border or tab bar");
    }
    for (int action : {-1, 0, 1, 2, 4, 5, 42}) Check(!IsRepeatableTitlebarButton(action),
        "Other actions must retain their double-activation protection");
}

void MeasuredWarningCards() {
    for (float setup_height : {75.0f, 134.0f, 240.0f}) {
        for (float detail_height : {60.0f, 110.0f, 210.0f}) {
            const auto page = GetWarningsSettingsLayout(144, 482, 10, 0, setup_height, detail_height);
            Check(Near(page.sensors_bot - page.sensors_text_top, setup_height + 14),
                "Sensor setup card must size to its measured wrapped paragraph plus padding");
            Check(Near(page.info_bot - page.info_text_top, detail_height + 14),
                "Warning notes card must size to its measured wrapped paragraph plus padding");
            Check(page.info_top >= page.sensors_bot + 16, "Wrapped note cards must not overlap");
            for (float modal_height : {460.0f, 630.0f, 740.0f}) {
                const float viewport_bottom = 10 + modal_height - 72;
                const float scroll_max = (std::max)(0.0f, page.info_bot - viewport_bottom);
                const auto scrolled = GetWarningsSettingsLayout(144, 482, 10, scroll_max, setup_height, detail_height);
                Check(scrolled.info_bot <= viewport_bottom + 0.01f,
                    "Entire warning text must remain reachable above the fixed Save footer");
                Check(Near(scrolled.sensors_text_top + scroll_max, page.sensors_text_top),
                    "Paragraph and its card must scroll together");
                Check(scrolled.title_y == page.title_y, "Wrapping must not move the fixed page title");
            }
        }
    }
}
} // namespace

int main() {
    try {
        ThemeChoices(); RapidTitlebarClicks(); MeasuredWarningCards();
        std::cout << "Settings geometry and rapid-click policy tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
