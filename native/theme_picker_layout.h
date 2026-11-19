#pragma once

#include <array>

// Shared drawing/hit geometry. Short, single-line choices stay on one baseline;
// the full meaning of Auto is explained below the entire row, not wrapped in a
// narrow third column. All dimensions are logical pixels.
namespace ThemePickerLayout {
inline constexpr int option_count = 3;
inline constexpr float card_height = 116.0f;
inline constexpr float padding = 20.0f;
inline constexpr float gap = 8.0f;
inline constexpr float choice_height = 36.0f;
inline constexpr const wchar_t* labels[option_count] = {L"Dark", L"Light", L"Auto"};
inline constexpr wchar_t auto_note[] = L"Auto follows your Windows theme.";

struct Rect { float left, top, right, bottom; };
struct Option {
    Rect bounds{};
    Rect text{};
    float radio_x = 0, radio_y = 0;
};
struct Layout {
    std::array<Option, option_count> options{};
    Rect note{};
    float bottom = 0;
};

inline Layout Get(float left, float right, float top) {
    Layout layout;
    const float inner_left = left + padding;
    const float inner_right = right - padding;
    const float width = (inner_right - inner_left - gap * (option_count - 1)) / option_count;
    for (int index = 0; index < option_count; ++index) {
        auto& option = layout.options[index];
        const float x = inner_left + index * (width + gap);
        option.bounds = {x, top + 42.0f, x + width, top + 42.0f + choice_height};
        option.radio_x = x + 14.0f;
        option.radio_y = (option.bounds.top + option.bounds.bottom) * 0.5f;
        option.text = {x + 26.0f, option.bounds.top, option.bounds.right - 6.0f, option.bounds.bottom};
    }
    layout.note = {inner_left, top + 86.0f, inner_right, top + 102.0f};
    layout.bottom = top + card_height;
    return layout;
}

inline int HitTest(const Layout& layout, float x, float y) {
    for (int index = 0; index < option_count; ++index) {
        const auto& bounds = layout.options[index].bounds;
        if (x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom) return index;
    }
    return -1;
}
} // namespace ThemePickerLayout
