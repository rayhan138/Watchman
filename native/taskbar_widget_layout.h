#pragma once

#include "taskbar_widget_mode.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace TaskbarWidgetLayout {
inline constexpr int horizontal_all_width = 148;
inline constexpr int horizontal_system_width = 62;
inline constexpr int system_label_width = 24;
inline constexpr int system_value_gap = 6;

struct Size {
    int width, height;
};

struct Rect {
    int left, top, right, bottom;
};

enum class TextAlignment { Left, Right };

inline TextAlignment NetworkAlignment(bool vertical, int edge) {
    return vertical && edge == 3 ? TextAlignment::Right : TextAlignment::Left;
}

struct Layout {
    Rect upload_arrow, upload_value, upload_unit;
    Rect download_arrow, download_value, download_unit;
    Rect cpu_label, cpu_value, memory_label, memory_value;
    bool split;
    float font_size;
    bool arrow_on_right;
    bool show_network, show_system;
};

struct NetworkRow {
    Rect arrow, value;
};

// Network rendering uses Get's fixed edge gutter, not a centered pair.
// Center the measured arrow/value group in the input rectangles' horizontal union.
// Preserve each rectangle's vertical bounds and the nonnegative reserved gap in
// the requested order. Negative measurements become zero; widths are capped to
// the original capacities (and union span), so oversized text must still be clipped.
// For a non-split combined rate, extend value.right to unit.right before calling.
inline NetworkRow CenterNetworkRow(Rect arrow, Rect value, int measured_arrow_width,
                                   int measured_value_width, bool arrow_on_right) {
    using Coordinate = std::int64_t;
    const Coordinate left = (std::min)(arrow.left, value.left);
    const Coordinate right = (std::max)(arrow.right, value.right);
    const Coordinate span = (std::max)(Coordinate{0}, right - left);
    const Coordinate gap = (std::clamp)(arrow_on_right
        ? static_cast<Coordinate>(arrow.left) - value.right
        : static_cast<Coordinate>(value.left) - arrow.right, Coordinate{0}, span);
    const Coordinate arrow_width = (std::clamp)(static_cast<Coordinate>(measured_arrow_width),
        Coordinate{0}, (std::min)(span - gap,
            (std::max)(Coordinate{0}, static_cast<Coordinate>(arrow.right) - arrow.left)));
    const Coordinate value_width = (std::clamp)(static_cast<Coordinate>(measured_value_width),
        Coordinate{0}, (std::min)(span - gap - arrow_width,
            (std::max)(Coordinate{0}, static_cast<Coordinate>(value.right) - value.left)));
    const Coordinate start = left + (span - arrow_width - gap - value_width) / 2;
    const Coordinate arrow_left = arrow_on_right ? start + value_width + gap : start;
    const Coordinate value_left = arrow_on_right ? start : start + arrow_width + gap;
    arrow.left = static_cast<int>(arrow_left);
    arrow.right = static_cast<int>(arrow_left + arrow_width);
    value.left = static_cast<int>(value_left);
    value.right = static_cast<int>(value_left + value_width);
    return { arrow, value };
}

struct SystemRow { Rect label, value; };

// Wide vertical taskbars center the measured pair, but still keep the exact
// same visible label/value gap as horizontal rows. Do not concatenate text:
// an ordinary space can become nearly invisible in a compact bold font.
inline SystemRow CenterSystemRow(Rect label, Rect value, int label_width, int value_width) {
    const auto pair = CenterNetworkRow(label, value, label_width, value_width, false);
    return { pair.arrow, pair.value };
}

inline bool IsVertical(int edge) {
    return edge == 2 || edge == 3; // Bottom = 0, top = 1, left = 2, right = 3.
}

namespace Detail {
inline unsigned Dpi(unsigned dpi) { return dpi == 0 ? 96u : dpi; }

inline int Saturate(std::int64_t value) {
    return static_cast<int>((std::clamp)(value,
        static_cast<std::int64_t>((std::numeric_limits<int>::min)()),
        static_cast<std::int64_t>((std::numeric_limits<int>::max)())));
}
} // namespace Detail

// Logical pixels to physical pixels, rounded to nearest (ties away from zero).
// A missing DPI uses 96; extreme inputs saturate rather than overflow.
inline int Scale(int logical, unsigned dpi) {
    const std::int64_t product = static_cast<std::int64_t>(logical) * Detail::Dpi(dpi);
    return Detail::Saturate((product + (product < 0 ? -48 : 48)) / 96);
}

namespace Detail {
inline bool Split(int width, unsigned dpi) {
    const auto usable = (std::max)(std::int64_t{0},
        static_cast<std::int64_t>(width) - std::int64_t{2} * Scale(1, dpi));
    return usable < Scale(72, dpi);
}
} // namespace Detail

// Thickness is physical pixels. The long axis is not constrained by thickness.
inline Size PreferredSize(TaskbarWidgetMode mode, bool vertical, int taskbar_thickness, unsigned dpi) {
    const bool show_network = TaskbarWidgetModes::ShowsNetwork(mode);
    const bool show_system = TaskbarWidgetModes::ShowsSystem(mode);
    const auto available = (std::max)(std::int64_t{0},
        static_cast<std::int64_t>(taskbar_thickness) - (vertical ? 0 : Scale(4, dpi)));
    if (!vertical) {
        if (available < Scale(24, dpi)) return { 0, 0 };
        return { Scale(show_network ? (show_system ? horizontal_all_width : 84) : horizontal_system_width, dpi),
            static_cast<int>((std::min)(static_cast<std::int64_t>(Scale(32, dpi)), available)) };
    }
    const int width = static_cast<int>((std::min)(static_cast<std::int64_t>(Scale(84, dpi)), available));
    // Shared number/unit bounds must fit four bold unit glyphs without moving the arrow gutter.
    if (width < Scale(show_network ? 42 : 40, dpi)) return { 0, 0 };
    const bool split = Detail::Split(width, dpi);
    const int rows = (show_network && show_system ? 4 : 2) * (split ? 2 : 1);
    return { width, Detail::Saturate(static_cast<std::int64_t>(rows) * Scale(16, dpi)
        + (show_network && show_system ? Scale(4, dpi) : 0) + std::int64_t{2} * Scale(2, dpi)) };
}

inline Size PreferredSize(bool network_only, bool vertical, int taskbar_thickness, unsigned dpi) {
    return PreferredSize(TaskbarWidgetModes::FromLegacyNetworkOnly(network_only),
        vertical, taskbar_thickness, dpi);
}

// Rectangles are half-open physical-pixel bounds, clipped to the nonnegative
// client size. Empty rectangles mean hidden/clipped text. Vertical block heights
// stay fixed; horizontal rows adapt to thickness. Renderers must clip and fit text.
inline Layout Get(int width, int height, TaskbarWidgetMode mode, bool vertical, unsigned dpi, int edge = 3) {
    using Coordinate = std::int64_t;
    const Coordinate safe_width = (std::max)(0, width);
    const Coordinate safe_height = (std::max)(0, height);
    const Coordinate padding = Scale(2, dpi);
    const Coordinate side_padding = Scale(vertical ? 1 : 2, dpi);
    const Coordinate row = vertical ? Scale(16, dpi)
        : (std::min)(static_cast<Coordinate>(Scale(16, dpi)), safe_height / 2);
    const Coordinate gap = Scale(4, dpi);
    const Coordinate content_width = (std::min)(safe_width, static_cast<Coordinate>(Scale(84, dpi)));
    Layout result{};
    result.show_network = TaskbarWidgetModes::ShowsNetwork(mode);
    result.show_system = TaskbarWidgetModes::ShowsSystem(mode);
    result.arrow_on_right = vertical && edge == 2;
    result.split = vertical && Detail::Split(static_cast<int>(content_width), dpi);
    const Coordinate usable = (std::max)(Coordinate{0}, content_width - 2 * side_padding);
    const int font = vertical ? (usable < Scale(48, dpi) ? 11 : 12) : 11;
    const float scale = static_cast<float>(Detail::Dpi(dpi)) / 96.0f;
    result.font_size = static_cast<float>(font) * scale;
    if (!vertical) result.font_size = (std::max)(9.0f * scale,
        (std::min)(result.font_size, static_cast<float>(row) - Scale(2, dpi)));

    const auto rect = [safe_width, safe_height](Coordinate left, Coordinate top,
                                               Coordinate right, Coordinate bottom) {
        const auto x = [safe_width](Coordinate value) {
            return static_cast<int>((std::clamp)(value, Coordinate{0}, safe_width));
        };
        const auto y = [safe_height](Coordinate value) {
            return static_cast<int>((std::clamp)(value, Coordinate{0}, safe_height));
        };
        return Rect{ x(left), y(top), x((std::max)(left, right)), y((std::max)(top, bottom)) };
    };
    const auto network = [&](Coordinate left, Coordinate right, Coordinate top,
                             Rect& arrow, Rect& value, Rect& unit) {
        const Coordinate arrow_left = result.arrow_on_right
            ? (std::max)(left, right - Scale(8, dpi)) : left;
        const Coordinate arrow_right = (std::min)(right, arrow_left + Scale(8, dpi));
        const Coordinate value_left = result.arrow_on_right ? left : (std::min)(right, arrow_right + gap);
        const Coordinate value_right = result.arrow_on_right ? (std::max)(left, arrow_left - gap) : right;
        arrow = rect(arrow_left, top, arrow_right, top + row);
        if (result.split) {
            value = rect(value_left, top, value_right, top + row);
            unit = rect(value_left, top + row, value_right, top + 2 * row);
        } else {
            const Coordinate unit_left = (std::min)(value_right, value_left + Scale(36, dpi));
            value = rect(value_left, top, unit_left, top + row);
            unit = rect(unit_left, top, value_right, top + row);
        }
    };
    const auto system = [&](Coordinate left, Coordinate right, Coordinate top,
                            Rect& label, Rect& value) {
        if (result.split) {
            label = rect(left, top, right, top + row);
            value = rect(left, top + row, right, top + 2 * row);
        } else {
            const Coordinate label_right = (std::min)(right, left + Scale(system_label_width, dpi));
            const Coordinate value_left = (std::min)(right, label_right + Scale(system_value_gap, dpi));
            label = rect(left, top, label_right, top + row);
            value = rect(value_left, top, right, top + row);
        }
    };

    if (vertical) {
        const Coordinate left = (safe_width - content_width) / 2 + (std::min)(side_padding, content_width);
        const Coordinate right = (std::max)(left, (safe_width + content_width) / 2 - side_padding);
        const Coordinate block = result.split ? 2 * row : row;
        if (result.show_network) {
            network(left, right, padding, result.upload_arrow, result.upload_value, result.upload_unit);
            network(left, right, padding + block, result.download_arrow, result.download_value, result.download_unit);
        }
        if (result.show_system) {
            const Coordinate system_top = padding + (result.show_network ? 2 * block + Scale(4, dpi) : 0);
            system(left, right, system_top, result.cpu_label, result.cpu_value);
            system(left, right, system_top + block, result.memory_label, result.memory_value);
        }
    } else {
        const Coordinate top = (std::max)(Coordinate{0}, (safe_height - 2 * row) / 2);
        if (result.show_network) {
            network(padding, (std::max)(padding, static_cast<Coordinate>(Scale(84, dpi)) - padding),
                top, result.upload_arrow, result.upload_value, result.upload_unit);
            network(padding, (std::max)(padding, static_cast<Coordinate>(Scale(84, dpi)) - padding),
                top + row, result.download_arrow, result.download_value, result.download_unit);
        }
        if (result.show_system) {
            const Coordinate left = result.show_network ? Scale(88, dpi) : padding;
            const Coordinate right = Scale(result.show_network ? horizontal_all_width - 2 : horizontal_system_width - 2, dpi);
            system(left, right, top, result.cpu_label, result.cpu_value);
            system(left, right, top + row, result.memory_label, result.memory_value);
        }
    }
    return result;
}

inline Layout Get(int width, int height, bool network_only, bool vertical, unsigned dpi, int edge = 3) {
    return Get(width, height, TaskbarWidgetModes::FromLegacyNetworkOnly(network_only), vertical, dpi, edge);
}

struct RateText {
    std::wstring value;
    std::wstring unit;
};

// Both rendering paths use the same compacting order and always retain units.
// A separate unit in a non-split row is the final clipped-number fallback.
template <typename Measure>
inline RateText FitRate(const std::wstring& text, bool split, int available_width, const Measure& measure) {
    const auto separator = text.find_last_of(L' ');
    if (separator == std::wstring::npos) return { text, L"" };
    std::wstring number = text.substr(0, separator);
    const std::wstring unit = text.substr(separator + 1);
    const auto fits = [&](const std::wstring& value) { return measure(value) <= available_width; };
    const auto drop_fraction = [&]() {
        const auto decimal = number.find(L'.');
        if (decimal != std::wstring::npos) number.erase(decimal);
    };
    if (split) {
        if (!fits(number)) drop_fraction();
        return { number, unit };
    }
    if (fits(text)) return { text, L"" };
    if (fits(number + unit)) return { number + unit, L"" };
    drop_fraction();
    if (fits(number + unit)) return { number + unit, L"" };
    return { number, unit };
}
} // namespace TaskbarWidgetLayout
