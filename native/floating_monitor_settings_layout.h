#pragma once

#include <algorithm>
#include <array>
#include <d2d1helper.h>

struct FloatingMonitorSettingsLayout {
    static constexpr size_t color_count = 3;
    static constexpr float row_height = 52.0f;

    float view_top = 0.0f;
    float view_bottom = 0.0f;
    float scroll_max = 0.0f;
    float scroll_offset = 0.0f;
    D2D1_RECT_F controls_card{};
    D2D1_RECT_F lock_row{};
    D2D1_RECT_F transparent_row{};
    D2D1_RECT_F opacity_minus{};
    D2D1_RECT_F opacity_value{};
    D2D1_RECT_F opacity_plus{};
    D2D1_RECT_F reset_position{};
    D2D1_RECT_F colors_card{};
    D2D1_RECT_F font_card{};
    std::array<D2D1_RECT_F, color_count> color_rows{};
    std::array<D2D1_RECT_F, color_count> color_controls{};
    D2D1_RECT_F font_description{};
    D2D1_RECT_F font_minus{};
    D2D1_RECT_F font_value{};
    D2D1_RECT_F font_plus{};
    D2D1_RECT_F preview{};
    D2D1_RECT_F preview_caption{};
};

// Drawing and hit testing share geometry. Measure the unscrolled content
// first so scrolling never changes its own maximum or moves the fixed title.
inline FloatingMonitorSettingsLayout GetFloatingMonitorSettingsLayout(
    float left, float right, float modal_top, float modal_height, float requested_scroll
) {
    FloatingMonitorSettingsLayout layout;
    layout.view_top = modal_top + 148.0f;
    layout.view_bottom = modal_top + modal_height - 72.0f;
    const float controls_top = modal_top + 156.0f;
    const float lock_top = controls_top + 44.0f;
    const float transparent_top = lock_top + 48.0f;
    const float opacity_top = transparent_top + 48.0f;
    const float reset_top = opacity_top + 48.0f + 8.0f;
    const float controls_bottom = reset_top + 34.0f + 16.0f;
    const float colors_top = controls_bottom + 16.0f;
    const float rows_top = colors_top + 44.0f;
    const float colors_bottom = rows_top + layout.color_count * layout.row_height + 8.0f;
    const float font_top = colors_bottom + 16.0f;
    const float font_description_top = font_top + 44.0f;
    const float font_size_top = font_description_top + 36.0f;
    const float preview_top = font_size_top + 40.0f + 16.0f;
    const float content_bottom = preview_top + 80.0f + 44.0f + 14.0f;
    layout.scroll_max = std::max(0.0f, content_bottom - layout.view_bottom);
    layout.scroll_offset = std::clamp(requested_scroll, 0.0f, layout.scroll_max);
    const float offset = layout.scroll_offset;
    layout.controls_card = D2D1::RectF(left, controls_top - offset, right, controls_bottom - offset);
    layout.lock_row = D2D1::RectF(left + 18.0f, lock_top - offset, right - 18.0f, lock_top + 48.0f - offset);
    layout.transparent_row = D2D1::RectF(left + 18.0f, transparent_top - offset, right - 18.0f, transparent_top + 48.0f - offset);
    layout.opacity_minus = D2D1::RectF(right - 144.0f, opacity_top + 6.0f - offset, right - 110.0f, opacity_top + 40.0f - offset);
    layout.opacity_value = D2D1::RectF(right - 104.0f, opacity_top + 6.0f - offset, right - 58.0f, opacity_top + 40.0f - offset);
    layout.opacity_plus = D2D1::RectF(right - 52.0f, opacity_top + 6.0f - offset, right - 18.0f, opacity_top + 40.0f - offset);
    layout.reset_position = D2D1::RectF(left + 18.0f, reset_top - offset, right - 18.0f, reset_top + 34.0f - offset);
    layout.colors_card = D2D1::RectF(left, colors_top - offset, right, colors_bottom - offset);
    layout.font_card = D2D1::RectF(left, font_top - offset, right, content_bottom - offset);
    for (size_t index = 0; index < layout.color_count; ++index) {
        const float top = rows_top + static_cast<float>(index) * layout.row_height - offset;
        layout.color_rows[index] = D2D1::RectF(left + 18.0f, top, right - 18.0f, top + layout.row_height);
        layout.color_controls[index] = D2D1::RectF(right - 90.0f, top + 10.0f, right - 18.0f, top + 40.0f);
    }
    layout.font_description = D2D1::RectF(left + 18.0f, font_description_top - offset, right - 18.0f, font_description_top + 28.0f - offset);
    layout.font_minus = D2D1::RectF(right - 174.0f, font_size_top - offset, right - 134.0f, font_size_top + 40.0f - offset);
    layout.font_value = D2D1::RectF(right - 128.0f, font_size_top - offset, right - 64.0f, font_size_top + 40.0f - offset);
    layout.font_plus = D2D1::RectF(right - 58.0f, font_size_top - offset, right - 18.0f, font_size_top + 40.0f - offset);
    layout.preview = D2D1::RectF(left + 18.0f, preview_top - offset, right - 18.0f, preview_top + 80.0f - offset);
    layout.preview_caption = D2D1::RectF(left + 18.0f, preview_top + 88.0f - offset, right - 18.0f, content_bottom - 12.0f - offset);
    return layout;
}
