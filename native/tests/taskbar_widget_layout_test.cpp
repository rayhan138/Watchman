#include "taskbar_widget_layout.h"

#include <array>
#include <cmath>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <dwrite.h>
#include <wrl/client.h>
#include "bundled_fonts.h"
#include "ui_style.h"
#endif

namespace {
using namespace TaskbarWidgetLayout;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Empty(const Rect& rect) {
    return rect.left == rect.right || rect.top == rect.bottom;
}

bool Overlap(const Rect& a, const Rect& b) {
    return !Empty(a) && !Empty(b)
        && a.left < b.right && b.left < a.right
        && a.top < b.bottom && b.top < a.bottom;
}

std::array<Rect, 10> Rectangles(const Layout& layout) {
    return { layout.upload_arrow, layout.upload_value, layout.upload_unit,
        layout.download_arrow, layout.download_value, layout.download_unit,
        layout.cpu_label, layout.cpu_value, layout.memory_label, layout.memory_value };
}

constexpr std::array<TaskbarWidgetMode, 3> Modes{
    TaskbarWidgetMode::All, TaskbarWidgetMode::Network, TaskbarWidgetMode::System
};

void CheckBounds(const Layout& layout, int width, int height, TaskbarWidgetMode mode, unsigned dpi) {
    Check(layout.show_network == TaskbarWidgetModes::ShowsNetwork(mode)
        && layout.show_system == TaskbarWidgetModes::ShowsSystem(mode), "Incorrect mode visibility flags");
    const auto rectangles = Rectangles(layout);
    for (std::size_t i = 0; i < rectangles.size(); ++i) {
        const auto& rect = rectangles[i];
        Check(0 <= rect.left && rect.left <= rect.right && rect.right <= (std::max)(0, width),
            "Rectangle escaped horizontal client bounds");
        Check(0 <= rect.top && rect.top <= rect.bottom && rect.bottom <= (std::max)(0, height),
            "Rectangle escaped vertical client bounds");
        for (std::size_t j = i + 1; j < rectangles.size(); ++j) {
            Check(!Overlap(rect, rectangles[j]), "Text rectangles overlap");
        }
        if ((i < 6 && !layout.show_network) || (i >= 6 && !layout.show_system)) {
            Check(rect.left == 0 && rect.top == 0 && rect.right == 0 && rect.bottom == 0,
                "Hidden metrics must have exactly zero rectangles");
        }
    }
    const float scale = static_cast<float>(dpi == 0 ? 96u : dpi) / 96.0f;
    Check(std::isfinite(layout.font_size)
        && layout.font_size >= 9.0f * scale && layout.font_size <= 12.0f * scale,
        "Font must stay within 9-12 logical pixels");
}

void CheckBounds(const Layout& layout, int width, int height, bool network_only, unsigned dpi) {
    CheckBounds(layout, width, height, TaskbarWidgetModes::FromLegacyNetworkOnly(network_only), dpi);
}

void CheckEqual(const Layout& a, const Layout& b) {
    const auto ar = Rectangles(a);
    const auto br = Rectangles(b);
    for (std::size_t i = 0; i < ar.size(); ++i) {
        Check(ar[i].left == br[i].left && ar[i].top == br[i].top
            && ar[i].right == br[i].right && ar[i].bottom == br[i].bottom,
            "Legacy/mode rectangle mismatch");
    }
    Check(a.split == b.split && a.font_size == b.font_size && a.arrow_on_right == b.arrow_on_right
        && a.show_network == b.show_network && a.show_system == b.show_system,
        "Legacy/mode layout metadata mismatch");
}

void ScalingAndEdges() {
    using namespace TaskbarWidgetModes;
    Check(static_cast<int>(TaskbarWidgetMode::All) == 0
        && static_cast<int>(TaskbarWidgetMode::Network) == 1
        && static_cast<int>(TaskbarWidgetMode::System) == 2, "Persisted mode IDs changed");
    Check(Normalize(0) == TaskbarWidgetMode::All && Normalize(1) == TaskbarWidgetMode::Network
        && Normalize(2) == TaskbarWidgetMode::System && Normalize(-1) == TaskbarWidgetMode::All
        && Normalize(3) == TaskbarWidgetMode::All, "Mode normalization changed");
    Check(FromLegacyNetworkOnly(false) == TaskbarWidgetMode::All
        && FromLegacyNetworkOnly(true) == TaskbarWidgetMode::Network, "Legacy migration changed");
    const auto invalid = static_cast<TaskbarWidgetMode>(-1);
    const auto invalid_size = PreferredSize(invalid, false, 32, 96);
    Check(invalid_size.width == 148 && invalid_size.height == 28, "Invalid mode must size as All");
    CheckEqual(Get(148, 32, invalid, false, 96), Get(148, 32, TaskbarWidgetMode::All, false, 96));
    Check(!IsVertical(0) && !IsVertical(1) && IsVertical(2) && IsVertical(3), "Edge mapping changed");
    Check(!IsVertical(-1) && !IsVertical(4), "Unknown edges must not become vertical");
    Check(Scale(11, 96) == 11 && Scale(11, 120) == 14 && Scale(11, 144) == 17
        && Scale(11, 192) == 22, "DPI rounding changed");
    Check(Scale(-11, 144) == -17 && Scale(0, 144) == 0, "Signed DPI rounding changed");
    Check(Scale(11, 0) == 11, "Unknown DPI must fall back to 96");
    const int maximum = (std::numeric_limits<int>::max)();
    const int minimum = (std::numeric_limits<int>::min)();
    const unsigned dpi = (std::numeric_limits<unsigned>::max)();
    Check(Scale(maximum, dpi) == maximum && Scale(minimum, dpi) == minimum,
        "Extreme scaling must saturate");
}

void PreferredSizes() {
    for (unsigned dpi : { 0u, 96u, 120u, 144u, 192u }) {
        for (int thickness : { -100, 0, 1, 4, 28, 32, 40, 48, 64, 80, 120 }) {
            for (auto mode : Modes) {
                for (int edge : { 0, 1, 2, 3 }) {
                    const bool vertical = IsVertical(edge);
                    const auto size = PreferredSize(mode, vertical, thickness, dpi);
                    const int available = (std::max)(0, thickness - (vertical ? 0 : Scale(4, dpi)));
                    const int expected_width = (std::min)(Scale(84, dpi), available);
                    const bool supported = vertical ? expected_width >= Scale(mode == TaskbarWidgetMode::System ? 40 : 42, dpi)
                                                    : available >= Scale(24, dpi);
                    if (mode != TaskbarWidgetMode::System) {
                        const auto legacy = PreferredSize(mode == TaskbarWidgetMode::Network, vertical, thickness, dpi);
                        Check(size.width == legacy.width && size.height == legacy.height,
                            "Legacy/mode preferred sizes differ");
                    }
                    if (!supported) {
                        Check(size.width == 0 && size.height == 0,
                            "Unreadably thin taskbars must return exactly {0, 0}");
                        continue;
                    }
                    const auto layout = Get(size.width, size.height, mode, vertical, dpi, edge);
                    CheckBounds(layout, size.width, size.height, mode, dpi);
                    if (mode != TaskbarWidgetMode::System) {
                        CheckEqual(layout, Get(size.width, size.height, mode == TaskbarWidgetMode::Network,
                            vertical, dpi, edge));
                    }
                    if (vertical) {
                        Check(size.width == expected_width, "Vertical width must use full thickness, capped at 84");
                        const int usable = size.width - 2 * Scale(1, dpi);
                        Check(layout.split == (usable < Scale(72, dpi)), "Wrong vertical split threshold");
                        const int block = Scale(16, dpi) * (layout.split ? 2 : 1);
                        Check(size.height == (mode == TaskbarWidgetMode::All ? 4 : 2) * block
                            + (mode == TaskbarWidgetMode::All ? Scale(4, dpi) : 0) + 2 * Scale(2, dpi),
                            "Vertical height must reserve only visible metric blocks");
                        if (layout.show_network) {
                            Check(layout.upload_value.top == Scale(2, dpi), "Vertical top inset changed");
                            Check(layout.download_value.top - layout.upload_value.top == block,
                                "Network metrics must occupy consecutive blocks");
                        }
                        if (layout.show_system) {
                            const int system_top = Scale(2, dpi)
                                + (layout.show_network ? 2 * block + Scale(4, dpi) : 0);
                            Check(layout.cpu_label.top == system_top,
                                "System-only must start at top inset, All must retain network gap");
                            Check(layout.cpu_label.left == Scale(1, dpi), "Vertical side padding must be one pixel");
                            Check(layout.memory_label.top - layout.cpu_label.top == block,
                                "System metrics must occupy consecutive blocks");
                        }
                    } else {
                        Check(size.width == Scale(mode == TaskbarWidgetMode::System ? 62
                            : mode == TaskbarWidgetMode::Network ? 84 : 148, dpi), "Wrong horizontal mode width");
                        Check(size.height == (std::min)(Scale(32, dpi), available),
                            "Horizontal height must respect taskbar thickness");
                        Check(!layout.split, "Horizontal layout must not split");
                        if (mode == TaskbarWidgetMode::System) {
                            Check(layout.cpu_label.left == Scale(2, dpi)
                                && layout.memory_label.left == Scale(2, dpi)
                                && layout.cpu_value.right == Scale(60, dpi),
                                "System-only must not reserve the network column");
                            Check(layout.cpu_label.top == 0 && layout.memory_label.top == size.height / 2,
                                "System-only rows must start at the client origin");
                        }
                    }
                    const auto rectangles = Rectangles(layout);
                    for (std::size_t i = 0; i < rectangles.size(); ++i) {
                        if (i < 6 ? layout.show_network : layout.show_system)
                            Check(!Empty(rectangles[i]), "Preferred layout unexpectedly hides a field");
                    }
                }
            }
        }
        for (auto mode : Modes) {
            Check(PreferredSize(mode, true, Scale(48, dpi), dpi).height
                == (mode == TaskbarWidgetMode::All ? 8 : 4) * Scale(16, dpi)
                    + (mode == TaskbarWidgetMode::All ? Scale(4, dpi) : 0) + 2 * Scale(2, dpi),
                "Narrow vertical mode height changed");
            for (bool vertical : { false, true }) {
                const int threshold = vertical ? Scale(mode == TaskbarWidgetMode::System ? 40 : 42, dpi) : Scale(4, dpi) + Scale(24, dpi);
                const auto below = PreferredSize(mode, vertical, threshold - 1, dpi);
                const auto at = PreferredSize(mode, vertical, threshold, dpi);
                const auto above = PreferredSize(mode, vertical, threshold + 1, dpi);
                Check(below.width == 0 && below.height == 0, "Below readability threshold must be unsupported");
                Check(at.width > 0 && at.height > 0, "Exact readability threshold must remain supported");
                Check(above.width > 0 && above.height > 0, "Above readability threshold must remain supported");
                CheckBounds(Get(at.width, at.height, mode, vertical, dpi), at.width, at.height, mode, dpi);
            }
        }
    }
    Check(PreferredSize(false, true, 48, 96).height == 136, "Narrow All must be 136px tall");
    Check(PreferredSize(true, true, 48, 96).height == 68, "Narrow Network must be 68px tall");
    Check(PreferredSize(false, true, 80, 96).height == 72, "Normal All must be 72px tall");
    Check(PreferredSize(true, true, 80, 96).height == 36, "Normal Network must be 36px tall");
    Check(PreferredSize(TaskbarWidgetMode::System, true, 80, 96).height == 36, "Normal System must be 36px tall");
}

void HorizontalRows() {
    for (unsigned dpi : { 96u, 120u, 144u, 192u }) {
        for (int height : { 0, 1, Scale(24, dpi) - 1, Scale(24, dpi), Scale(24, dpi) + 1,
                 Scale(28, dpi), Scale(32, dpi) - 1, Scale(32, dpi), Scale(40, dpi) }) {
            for (auto mode : Modes) {
                const int width = Scale(mode == TaskbarWidgetMode::System ? 62
                    : mode == TaskbarWidgetMode::Network ? 84 : 148, dpi);
                const auto layout = Get(width, height, mode, false, dpi);
                const int row = (std::min)(Scale(16, dpi), height / 2);
                const int top = (height - 2 * row) / 2;
                CheckBounds(layout, width, height, mode, dpi);
                if (layout.show_network) {
                    Check(layout.upload_value.top == top && layout.upload_value.bottom == top + row,
                        "Horizontal upload row must use min(16 scaled pixels, client height / 2)");
                    Check(layout.download_value.top == top + row && layout.download_value.bottom == top + 2 * row,
                        "Horizontal download row must follow without clipping or overlap");
                }
                const float scale = static_cast<float>(dpi) / 96.0f;
                const float expected_font = (std::max)(9.0f * scale,
                    (std::min)(11.0f * scale, static_cast<float>(row - Scale(2, dpi))));
                Check(layout.font_size == expected_font, "Horizontal font must track row height within 9-11 logical pixels");
                if (layout.show_system) {
                    Check(layout.cpu_label.top == top && layout.cpu_value.bottom == top + row
                        && layout.memory_label.top == top + row && layout.memory_value.bottom == top + 2 * row,
                        "System rows must use the same shortened horizontal row height");
                }
            }
        }
    }
}

void FieldPlacement() {
    Check(horizontal_all_width == 148 && horizontal_system_width == 62 && system_label_width == 24 && system_value_gap == 6,
        "Taskbar must reserve readable labels, six-pixel spacing, and enough room for 100%");
    for (unsigned dpi : { 96u, 120u, 144u, 192u }) {
        const float scale = static_cast<float>(dpi) / 96.0f;
        for (int edge : { 0, 1, 2, 3 }) {
            const bool vertical = IsVertical(edge);
            const auto size = PreferredSize(false, vertical, Scale(120, dpi), dpi);
            const auto layout = Get(size.width, size.height, false, vertical, dpi, edge);
            CheckBounds(layout, size.width, size.height, false, dpi);
            Check(!layout.split, "Wide layout must not split");
            Check(layout.arrow_on_right == (vertical && edge == 2), "Arrow must face screen contents");
            Check(layout.upload_arrow.right - layout.upload_arrow.left == Scale(8, dpi),
                "Arrow must use only eight logical pixels");
            const int gap = layout.arrow_on_right
                ? layout.upload_arrow.left - layout.upload_unit.right
                : layout.upload_value.left - layout.upload_arrow.right;
            Check(gap == Scale(4, dpi), "Arrow gap must be four logical pixels on every edge");
            Check(layout.download_value.left == layout.upload_value.left, "Network values must align left");
            Check(layout.upload_value.right - layout.upload_value.left == Scale(36, dpi),
                "Normal numeric allocation must stay 36 logical pixels");
            Check(layout.upload_unit.left == layout.upload_value.right, "Units must follow numbers on both edges");
            Check(layout.cpu_label.right - layout.cpu_label.left == Scale(24, dpi),
                "System labels must reserve 24 horizontal and vertical logical pixels");
            Check(layout.cpu_label.top == layout.cpu_value.top
                && layout.cpu_value.left - layout.cpu_label.right == Scale(6, dpi),
                "Normal CPU fields must share a row with a visible six-pixel gap");
            Check(layout.memory_label.top == layout.memory_value.top
                && layout.memory_value.left - layout.memory_label.right == Scale(6, dpi),
                "Normal memory fields must share a row with the same system gap");
            Check(layout.font_size == (vertical ? 12.0f : 11.0f) * scale, "Wrong normal font size");
            if (vertical) {
                const int left = (std::min)(layout.upload_arrow.left, layout.upload_value.left);
                const int right = (std::max)(layout.upload_arrow.right, layout.upload_unit.right);
                Check(layout.cpu_label.left == left && layout.cpu_value.right == right
                    && layout.memory_label.left == left && layout.memory_value.right == right,
                    "Combined CPU/MEM blocks must use the full centered network span");
                Check(std::abs(left + right - size.width) <= 1, "Vertical content block must be centered");
            } else {
                Check(layout.cpu_label.left == Scale(88, dpi), "Horizontal system column must leave a four-pixel gutter after network");
                Check(layout.cpu_value.right == Scale(146, dpi), "Horizontal system column needs right inset");
                Check(layout.cpu_label.top == layout.upload_value.top
                    && layout.memory_label.top == layout.download_value.top, "Horizontal rows must align");
            }
        }
        for (int edge : { 2, 3 }) {
            const auto narrow = Get(Scale(60, dpi), Scale(136, dpi), false, true, dpi, edge);
            Check(narrow.split, "Narrow vertical layout must split");
            Check(narrow.upload_unit.top == narrow.upload_value.bottom
                && narrow.download_unit.top == narrow.download_value.bottom,
                "Split units must occupy the next subrow");
            for (bool upload : { false, true }) {
                const Rect arrow = upload ? narrow.upload_arrow : narrow.download_arrow;
                const Rect value = upload ? narrow.upload_value : narrow.download_value;
                const Rect unit = upload ? narrow.upload_unit : narrow.download_unit;
                Check(unit.left == value.left && unit.right == value.right,
                    "Split units and numbers must share bounds away from the arrow gutter");
                Check((edge == 2 ? arrow.left - value.right : value.left - arrow.right) == Scale(4, dpi),
                    "Split arrow gap must be four logical pixels on both edges");
                Check(narrow.arrow_on_right == (edge == 2), "Split arrow direction changed");
            }
            Check(narrow.cpu_value.top == narrow.cpu_label.bottom
                && narrow.cpu_value.left == narrow.cpu_label.left
                && narrow.cpu_value.right == narrow.cpu_label.right,
                "Split CPU label/percentage must each use the full centered subrow");
            Check(narrow.memory_value.top == narrow.memory_label.bottom
                && narrow.memory_value.left == narrow.cpu_value.left
                && narrow.memory_value.right == narrow.cpu_value.right,
                "Split memory percentage must use the same centered subrow");
            Check(narrow.font_size == 12.0f * scale, "Moderate split widths must use 12px text");
            const auto smallest = Get(Scale(44, dpi), Scale(136, dpi), false, true, dpi, edge);
            Check(smallest.font_size == 11.0f * scale, "Very narrow widths must use 11px text");
            const int font_threshold = Scale(48, dpi) + 2 * Scale(1, dpi);
            Check(Get(font_threshold - 1, 1000, false, true, dpi, edge).font_size == 11.0f * scale,
                "Below 48 usable pixels must use 11px text");
            Check(Get(font_threshold, 1000, false, true, dpi, edge).font_size == 12.0f * scale,
                "48 usable pixels must use 12px text");
            const int threshold = Scale(72, dpi) + 2 * Scale(1, dpi);
            Check(!Get(threshold, 1000, false, true, dpi, edge).split, "72px usable width must not split");
            Check(Get(threshold - 1, 1000, false, true, dpi, edge).split, "Below 72px usable width must split");
        }
        for (int edge : { 2, 3 }) {
            Check(!Get(Scale(136, dpi), Scale(32, dpi), false, false, dpi, edge).arrow_on_right,
                "An edge argument must not mirror a horizontal layout");
            for (int width : { 44, 60, 76, 84, 120, 121 }) {
                const int client_width = Scale(width, dpi);
                const auto layout = Get(client_width, Scale(136, dpi), false, true, dpi, edge);
                for (bool cpu : { false, true }) {
                    const Rect label = cpu ? layout.cpu_label : layout.memory_label;
                    const Rect value = cpu ? layout.cpu_value : layout.memory_value;
                    Check(std::abs(label.left + value.right - client_width) <= 1,
                        "CPU/MEM full union must stay centered, including clients wider than the content cap");
                    if (layout.split) {
                        Check(std::abs(label.left + label.right - client_width) <= 1
                            && std::abs(value.left + value.right - client_width) <= 1,
                            "Split CPU/MEM labels and percentages must be individually centered");
                    }
                }
            }
        }
        const auto legacy = Get(Scale(84, dpi), Scale(136, dpi), false, true, dpi);
        const auto right = Get(Scale(84, dpi), Scale(136, dpi), false, true, dpi, 3);
        Check(!legacy.arrow_on_right && legacy.upload_arrow.left == right.upload_arrow.left
            && legacy.upload_value.left == right.upload_value.left, "Five-argument callers must default to right edge");
    }
}

void ModeVerticalGeometry() {
    for (unsigned dpi : { 0u, 96u, 120u, 144u, 192u }) {
        const int split_threshold = Scale(72, dpi) + 2 * Scale(1, dpi);
        const int font_threshold = Scale(48, dpi) + 2 * Scale(1, dpi);
        for (auto mode : Modes) {
            for (int width : { Scale(mode == TaskbarWidgetMode::System ? 40 : 42, dpi), Scale(48, dpi), font_threshold - 1, font_threshold,
                     Scale(64, dpi), split_threshold - 1, split_threshold, split_threshold + 1,
                     Scale(84, dpi), Scale(120, dpi) }) {
                for (int edge : { 2, 3 }) {
                    const auto size = PreferredSize(mode, true, width, dpi);
                    const auto layout = Get(size.width, size.height, mode, true, dpi, edge);
                    CheckBounds(layout, size.width, size.height, mode, dpi);
                    Check(size.width == (std::min)(width, Scale(84, dpi)), "Full vertical thickness was lost");
                    Check(layout.split == (size.width < split_threshold), "Get/PreferredSize split mismatch");
                    const float scale = static_cast<float>(dpi == 0 ? 96u : dpi) / 96.0f;
                    Check(layout.font_size == (size.width < font_threshold ? 11.0f : 12.0f) * scale,
                        "Vertical font must remain 11/12px, never shrink to fit text");
                    if (!layout.show_network) continue;
                    for (bool upload : { false, true }) {
                        const Rect arrow = upload ? layout.upload_arrow : layout.download_arrow;
                        const Rect value = upload ? layout.upload_value : layout.download_value;
                        const Rect unit = upload ? layout.upload_unit : layout.download_unit;
                        Check(arrow.right - arrow.left == Scale(8, dpi), "Fixed arrow gutter changed");
                        Check(edge == 2 ? arrow.right == size.width - Scale(1, dpi)
                                        : arrow.left == Scale(1, dpi),
                            "Arrow must stay pinned to its screen-facing edge, not center with text");
                        Check((edge == 2 ? arrow.left - (layout.split ? value.right : unit.right)
                                         : value.left - arrow.right) == Scale(4, dpi),
                            "Fixed gutter must retain four-pixel numeric gap");
                        if (layout.split) {
                            Check(unit.left == value.left && unit.right == value.right,
                                "Split unit must align with the numeric column");
                            Check(value.right - value.left == size.width - 2 * Scale(1, dpi)
                                - Scale(8, dpi) - Scale(4, dpi), "Split numeric capacity changed");
                        }
                    }
                    const auto mirror = Get(size.width, size.height, mode, true, dpi, edge == 2 ? 3 : 2);
                    Check(layout.upload_arrow.left == size.width - mirror.upload_arrow.right
                        && layout.upload_arrow.right == size.width - mirror.upload_arrow.left,
                        "Screen-facing arrow gutters must mirror exactly");
                    Check(layout.upload_value.right - layout.upload_value.left
                        == mirror.upload_value.right - mirror.upload_value.left,
                        "Mirroring must preserve numeric capacity");
                }
            }
        }
    }
    const auto roomier = Get(48, 136, TaskbarWidgetMode::All, true, 96);
    Check(roomier.upload_value.right - roomier.upload_value.left == 34,
        "Full 48px thickness must gain four numeric pixels over the old 30px allocation");
}

void CheckCenteredRow(Rect arrow, Rect value, int arrow_width, int value_width, bool arrow_on_right) {
    const auto centered = CenterNetworkRow(arrow, value, arrow_width, value_width, arrow_on_right);
    const std::int64_t left = (std::min)(arrow.left, value.left);
    const std::int64_t right = (std::max)(arrow.right, value.right);
    for (const Rect rect : { centered.arrow, centered.value }) {
        Check(left <= rect.left && rect.left <= rect.right && rect.right <= right,
            "Centered row must stay inside the original union");
    }
    Check(centered.arrow.top == arrow.top && centered.arrow.bottom == arrow.bottom
        && centered.value.top == value.top && centered.value.bottom == value.bottom,
        "Centering must preserve vertical bounds");
    Check(centered.arrow.right - centered.arrow.left == (std::clamp)(arrow_width, 0, arrow.right - arrow.left)
        && centered.value.right - centered.value.left == (std::clamp)(value_width, 0, value.right - value.left),
        "Measured widths must be capped to their original clipping capacities");
    const std::int64_t gap = arrow_on_right
        ? static_cast<std::int64_t>(arrow.left) - value.right
        : static_cast<std::int64_t>(value.left) - arrow.right;
    Check((arrow_on_right ? centered.arrow.left - centered.value.right
                         : centered.value.left - centered.arrow.right) == gap,
        "Centering must preserve the reserved gap and screen-facing order");
    const auto group_left = (std::min)(centered.arrow.left, centered.value.left);
    const auto group_right = (std::max)(centered.arrow.right, centered.value.right);
    Check(std::abs((static_cast<std::int64_t>(group_left) - left) - (right - group_right)) <= 1,
        "Measured group must be centered to within one physical pixel");
    Check(!Overlap(centered.arrow, centered.value), "Centered arrow and number must never overlap");
}

void MeasuredRowCentering() {
    for (unsigned dpi : { 96u, 120u, 144u, 192u }) {
        for (int edge : { 2, 3 }) {
            for (int width : { 40, 44, 60, 76, 84, 120 }) {
                for (bool network_only : { false, true }) {
                    const auto layout = Get(Scale(width, dpi), Scale(136, dpi), network_only, true, dpi, edge);
                    for (bool upload : { false, true }) {
                        const Rect arrow = upload ? layout.upload_arrow : layout.download_arrow;
                        const Rect original_value = upload ? layout.upload_value : layout.download_value;
                        const Rect unit = upload ? layout.upload_unit : layout.download_unit;
                        for (bool combined : { false, true }) {
                            if (combined && layout.split) continue;
                            Rect value = original_value;
                            if (combined) value.right = unit.right;
                            for (int measured_arrow : { -1, 0, Scale(5, dpi), Scale(8, dpi), 1000 }) {
                                for (int measured_value : { -1, 0, Scale(6, dpi), Scale(18, dpi), Scale(30, dpi), 1000 }) {
                                    CheckCenteredRow(arrow, value, measured_arrow, measured_value, layout.arrow_on_right);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    const auto short_value = CenterNetworkRow({ 2, 2, 10, 18 }, { 12, 2, 42, 18 }, 6, 6, false);
    const auto long_value = CenterNetworkRow({ 2, 2, 10, 18 }, { 12, 2, 42, 18 }, 6, 18, false);
    Check(short_value.arrow.left == 15 && short_value.value.left == 23 && short_value.value.right == 29,
        "Short number must center together with its measured arrow, not in its own slot");
    Check(long_value.arrow.left == 9 && long_value.value.left == 17 && long_value.value.right == 35,
        "Longer number must move the whole group left while retaining its center");
    const auto mirrored = CenterNetworkRow({ 34, 2, 42, 18 }, { 2, 2, 32, 18 }, 6, 18, true);
    Check(mirrored.value.left == 9 && mirrored.value.right == 27
        && mirrored.arrow.left == 29 && mirrored.arrow.right == 35, "Left-edge group must mirror number/arrow order");
    CheckCenteredRow({ 0, 0, 0, 0 }, { 0, 0, 0, 0 }, 100, 100, false);
    const int maximum = (std::numeric_limits<int>::max)();
    CheckCenteredRow({ maximum - 80, 0, maximum - 72, 16 },
        { maximum - 70, 0, maximum, 16 }, maximum, maximum, false);
    CheckCenteredRow({ 0, 0, 8, 16 }, { 10, 0, maximum, 16 }, maximum, maximum, false);
}

void SystemPairCentering() {
    for (unsigned dpi : {0u, 96u, 120u, 144u, 192u}) {
        for (auto mode : {TaskbarWidgetMode::All, TaskbarWidgetMode::System}) {
            for (int edge : {2, 3}) {
                const auto size = PreferredSize(mode, true, Scale(84, dpi), dpi);
                const auto layout = Get(size.width, size.height, mode, true, dpi, edge);
                for (const int measured_value : {Scale(12, dpi), Scale(18, dpi), Scale(24, dpi), Scale(30, dpi)}) {
                    const auto row = CenterSystemRow(layout.cpu_label, layout.cpu_value, Scale(22, dpi), measured_value);
                    Check(row.value.left - row.label.right == Scale(6, dpi),
                        "Centering must retain the visible system label/value gap");
                    Check(row.label.left >= layout.cpu_label.left && row.value.right <= layout.cpu_value.right
                        && std::abs(row.label.left + row.value.right - layout.cpu_label.left - layout.cpu_value.right) <= 1,
                        "Centering must fit the measured pair inside its original centered span");
                    Check(row.label.right - row.label.left == Scale(22, dpi) && row.value.right - row.value.left == measured_value,
                        "Centering must keep the measured label/value widths");
                }
            }
        }
    }
}

void RateFitting() {
    const auto check_fit = [](const std::wstring& text, bool split, int width,
                              const std::wstring& value, const std::wstring& unit,
                              const std::vector<std::wstring>& expected_measurements) {
        std::vector<std::wstring> measurements;
        const auto measure = [&](const std::wstring& candidate) {
            measurements.push_back(candidate);
            return static_cast<float>(candidate.size());
        };
        const auto fitted = FitRate(text, split, width, measure);
        Check(fitted.value == value && fitted.unit == unit, "Rate fitting returned unexpected value/unit");
        Check(measurements == expected_measurements, "Rate fitting changed candidate order or measured unnecessary text");
    };

    // One synthetic pixel per character makes exact boundaries deterministic.
    check_fit(L"12.34 KB/s", false, 10, L"12.34 KB/s", L"", { L"12.34 KB/s" });
    check_fit(L"12.34 KB/s", false, 9, L"12.34KB/s", L"", { L"12.34 KB/s", L"12.34KB/s" });
    check_fit(L"12.34 KB/s", false, 6, L"12KB/s", L"", { L"12.34 KB/s", L"12.34KB/s", L"12KB/s" });
    check_fit(L"12.34 KB/s", false, 5, L"12", L"KB/s", { L"12.34 KB/s", L"12.34KB/s", L"12KB/s" });
    check_fit(L"12.34 KB/s", true, 5, L"12.34", L"KB/s", { L"12.34" });
    check_fit(L"12.34 KB/s", true, 4, L"12", L"KB/s", { L"12.34" });
    check_fit(L"12.34 KB/s", true, 2, L"12", L"KB/s", { L"12.34" });
    for (int width : { -1, 0, 1 }) {
        check_fit(L"12.34 KB/s", false, width, L"12", L"KB/s",
            { L"12.34 KB/s", L"12.34KB/s", L"12KB/s" });
        check_fit(L"12.34 KB/s", true, width, L"12", L"KB/s", { L"12.34" });
    }
    check_fit(L"1234 B/s", true, 1, L"1234", L"B/s", { L"1234" });
    check_fit(L"0.00 B/s", true, 1, L"0", L"B/s", { L"0.00" });
    check_fit(L"", false, 0, L"", L"", {});
    check_fit(L"123.45", true, 0, L"123.45", L"", {});
    check_fit(L"12KB/s", false, 0, L"12KB/s", L"", {});

    const auto fractional_measure = [](const std::wstring& text) {
        return static_cast<float>(text.size()) * 2.5f;
    };
    Check(FitRate(L"12.34 KB/s", false, 25, fractional_measure).value == L"12.34 KB/s",
        "Exact floating-point measurement must fit");
    Check(FitRate(L"12.34 KB/s", false, 24, fractional_measure).value == L"12.34KB/s",
        "Fractional width must not be truncated to an integer");
    Check(FitRate(L"12.34 KB/s", false, 22, fractional_measure).value == L"12KB/s",
        "Fraction must be dropped only after removing the separator");
    for (const std::wstring unit : { L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s", L"KiB/s" }) {
        for (bool split : { false, true }) {
            const auto fitted = FitRate(L"1234.56 " + unit, split, 0, fractional_measure);
            Check(fitted.value == L"1234" && fitted.unit == unit,
                "Final clipped numeric fallback must retain the complete unit and integer");
            Check(fractional_measure(fitted.value) > 0,
                "Zero-width fallback should leave numeric clipping to the caller");
        }
    }
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

class FontMeasurement {
public:
    FontMeasurement(IDWriteFactory* factory, IDWriteFontCollection* fonts,
                    const wchar_t* family, float size) : factory_(factory) {
        Check(SUCCEEDED(factory_->CreateTextFormat(family, fonts, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", format_.GetAddressOf())),
            "DirectWrite CreateTextFormat failed");
        Check(SUCCEEDED(format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)),
            "DirectWrite disabling wrapping failed");
    }

    float operator()(const std::wstring& text) const {
        if (text.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        Check(SUCCEEDED(factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            format_.Get(), 10000.0f, 1000.0f, layout.GetAddressOf())), "DirectWrite CreateTextLayout failed");
        DWRITE_TEXT_METRICS metrics{};
        Check(SUCCEEDED(layout->GetMetrics(&metrics)), "DirectWrite GetMetrics failed");
        return metrics.widthIncludingTrailingWhitespace;
    }

private:
    IDWriteFactory* factory_;
    ComPtr<IDWriteTextFormat> format_;
};

class GdiFontMeasurement {
public:
    GdiFontMeasurement(const wchar_t* family, float size) {
        try {
            dc_ = CreateCompatibleDC(nullptr);
            Check(dc_ != nullptr, "GDI measurement DC creation failed");
            font_ = CreateFontW(-static_cast<int>(std::lround(size)), 0, 0, 0, FW_BOLD,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
            Check(font_ != nullptr, "GDI bold font creation failed");
            previous_ = SelectObject(dc_, font_);
            Check(previous_ != nullptr && previous_ != HGDI_ERROR, "GDI font selection failed");
            wchar_t actual_family[LF_FACESIZE]{};
            Check(GetTextFaceW(dc_, LF_FACESIZE, actual_family) > 0, "GDI selected font lookup failed");
            Check(_wcsicmp(actual_family, family) == 0,
                "GDI silently substituted the requested installed font family");
            TEXTMETRICW metrics{};
            Check(GetTextMetricsW(dc_, &metrics) != FALSE, "GDI font metrics failed");
            Check(metrics.tmWeight >= FW_BOLD, "GDI selected font is not bold");
        } catch (...) {
            Release();
            throw;
        }
    }

    ~GdiFontMeasurement() { Release(); }
    GdiFontMeasurement(const GdiFontMeasurement&) = delete;
    GdiFontMeasurement& operator=(const GdiFontMeasurement&) = delete;

    float operator()(const std::wstring& text) const {
        SIZE extent{};
        Check(GetTextExtentPoint32W(dc_, text.c_str(), static_cast<int>(text.size()), &extent) != FALSE,
            "GDI text measurement failed");
        return static_cast<float>(extent.cx);
    }

private:
    void Release() {
        if (previous_ != nullptr && previous_ != HGDI_ERROR) SelectObject(dc_, previous_);
        if (font_ != nullptr) DeleteObject(font_);
        if (dc_ != nullptr) DeleteDC(dc_);
        previous_ = nullptr;
        font_ = nullptr;
        dc_ = nullptr;
    }

    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

template <typename Measure>
void CheckMeasuredLayout(const Measure& measure, const wchar_t* backend, const wchar_t* family,
                         const Layout& layout, unsigned dpi, int thickness, bool vertical,
                         TaskbarWidgetMode mode, int edge, int& failures, int& clipped_numbers) {
    const auto require = [&](bool condition, const std::wstring& field, float measured, int available) {
        if (condition) return;
        ++failures;
        std::wcerr << L"Font-fit failure: " << backend << L", " << family << L" Bold, dpi=" << dpi
            << L", thickness=" << thickness << L", vertical=" << vertical << L", edge=" << edge
            << L", mode=" << static_cast<int>(mode) << L", split=" << layout.split
            << L", font=" << layout.font_size;
        if (std::wstring(backend) == L"GDI") std::wcerr << L", rounded_font=" << std::lround(layout.font_size);
        std::wcerr << L", " << field << L": measured=" << measured << L", available=" << available << L'\n';
    };
    const auto fits_field = [&](const std::wstring& text, const Rect& rect, const std::wstring& field) {
        const float measured = measure(text);
        require(measured <= static_cast<float>(rect.right - rect.left), field, measured, rect.right - rect.left);
    };
    if (layout.show_system) {
        fits_field(L"CPU", layout.cpu_label, L"CPU label");
        fits_field(L"MEM", layout.memory_label, L"MEM label");
        fits_field(L"100%", layout.cpu_value, L"CPU 100%");
        fits_field(L"100%", layout.memory_value, L"MEM 100%");
        if (!layout.split) {
            Check(layout.cpu_value.left - layout.cpu_label.right == Scale(6, dpi)
                && layout.memory_value.left - layout.memory_label.right == Scale(6, dpi),
                "Normal system fields must use six-logical-pixel gaps");
            if (vertical) {
                for (bool cpu : {false, true}) {
                    const auto label = cpu ? layout.cpu_label : layout.memory_label;
                    const auto value = cpu ? layout.cpu_value : layout.memory_value;
                    for (const auto* percent : {L"0%", L"9%", L"24%", L"58%", L"100%", L"--"}) {
                        const auto text = cpu ? L"CPU" : L"MEM";
                        const auto centered = CenterSystemRow(label, value,
                            static_cast<int>(std::ceil(measure(text))), static_cast<int>(std::ceil(measure(percent))));
                        Check(centered.value.left - centered.label.right == Scale(6, dpi),
                            "Measured centered label/value pair must retain six logical pixels of empty space");
                        fits_field(text, centered.label, L"Centered system label");
                        fits_field(percent, centered.value, L"Centered system percentage");
                        Check(std::abs(centered.label.left + centered.value.right - label.left - value.right) <= 1,
                            "The measured system pair must remain centered inside its original union");
                    }
                }
            }
        }
    }
    if (!layout.show_network) return;
    for (const std::wstring text : { L"1.2 MB/s", L"8.6 MB/s", L"0 B/s", L"0.00 B/s", L"9.99 KB/s", L"12.34 KB/s",
             L"999.99 KB/s", L"1000.00 MB/s", L"9999.99 GB/s" }) {
        for (bool upload : { false, true }) {
            const Rect& arrow = upload ? layout.upload_arrow : layout.download_arrow;
            const Rect& value = upload ? layout.upload_value : layout.download_value;
            const Rect& unit = upload ? layout.upload_unit : layout.download_unit;
            const int available = (layout.split ? value.right : unit.right) - value.left;
            const auto fitted = FitRate(text, layout.split, available, measure);
            const auto separator = text.find_last_of(L' ');
            const std::wstring original_number = text.substr(0, separator);
            const std::wstring original_unit = text.substr(separator + 1);
            const auto decimal = original_number.find(L'.');
            const std::wstring integer = original_number.substr(0, decimal);
            const std::wstring field = std::wstring(upload ? L"UP " : L"DN ") + text;
            if (fitted.unit.empty()) {
                Check(!layout.split, "Split rates must always retain a separate unit");
                Check(fitted.value == text || fitted.value == original_number + original_unit
                    || fitted.value == integer + original_unit, "Rate lost numeric or unit content");
                require(measure(fitted.value) <= available, field + L" combined", measure(fitted.value), available);
                fits_field(upload ? L"\u2191" : L"\u2193", arrow, field + L" arrow");
            } else {
                Check(fitted.unit == original_unit, "Font fitting must never abbreviate/drop units");
                fits_field(fitted.unit, unit, field + L" unit");
                if (layout.split) {
                    Check(unit.left == value.left && unit.right == value.right,
                        "Measured split unit must share the numeric bounds on both edges");
                    Check(NetworkAlignment(true, edge) == (edge == 2 ? TextAlignment::Left : TextAlignment::Right),
                        "Vertical network alignment must follow its taskbar edge");
                    fits_field(upload ? L"\u2191" : L"\u2193", arrow, field + L" arrow");
                }
                const int numeric_width = value.right - value.left;
                if (measure(fitted.value) > numeric_width) {
                    ++clipped_numbers;
                    Check(fitted.value == integer, "Clipped numeric fallback must drop fraction first");
                }
                if (!layout.split) {
                    Check(fitted.value == integer && measure(integer + original_unit) > available,
                        "Separate-unit fallback must occur only after compact integer rate fails");
                } else {
                    Check(fitted.value == (measure(original_number) <= available ? original_number : integer),
                        "Split fitting must preserve fraction whenever it fits");
                }
            }
        }
    }
}

void BundledAndFallbackFontFitting() {
    ComPtr<IDWriteFactory> factory;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "DirectWrite factory creation failed");
    ComPtr<IDWriteFontCollection> fonts;
    Check(SUCCEEDED(factory->GetSystemFontCollection(fonts.GetAddressOf())), "System font enumeration failed");
    WatchmanFonts::Collection bundled;
    Check(SUCCEEDED(bundled.Initialize(factory.Get())), "Production embedded DirectWrite font loader failed");
    WatchmanFonts::GdiRegistration private_fonts;
    Check(SUCCEEDED(private_fonts.Initialize()), "Production in-memory GDI font registration failed");
    int families_tested = 0;
    int failures = 0;
    int clipped_numbers = 0;
    int unsupported_sizes = 0;
    struct FontCase { const wchar_t* family; IDWriteFontCollection* collection; };
    for (const auto& font_case : {FontCase{UIStyle::FONT_MONO, bundled.Get()}, FontCase{UIStyle::FONT_MONO_FALLBACK, fonts.Get()}}) {
        const wchar_t* family = font_case.family;
        UINT32 index = 0;
        BOOL exists = FALSE;
        Check(SUCCEEDED(font_case.collection->FindFamilyName(family, &index, &exists)) && exists,
            "Required bundled/fallback font is absent; never skip bundled JetBrains Mono");
        ++families_tested;
        std::wcout << L"Testing real font in DirectWrite and GDI: " << family << L" Bold\n";
        for (unsigned dpi : { 96u, 120u, 144u, 192u }) {
            for (int thickness : { 32, 40, 48, 64, 80, 120,
                     Scale(40, dpi) - 1, Scale(40, dpi), Scale(40, dpi) + 1,
                     Scale(42, dpi) - 1, Scale(42, dpi), Scale(42, dpi) + 1,
                     Scale(48, dpi) + 2 * Scale(1, dpi) - 1, Scale(48, dpi) + 2 * Scale(1, dpi),
                     Scale(72, dpi) + 2 * Scale(1, dpi) - 1, Scale(72, dpi) + 2 * Scale(1, dpi),
                     Scale(4, dpi) + Scale(24, dpi) - 1, Scale(4, dpi) + Scale(24, dpi),
                     Scale(4, dpi) + Scale(24, dpi) + 1 }) {
                for (int edge : { 0, 1, 2, 3 }) {
                    const bool vertical = IsVertical(edge);
                    for (auto mode : Modes) {
                        const auto size = PreferredSize(mode, vertical, thickness, dpi);
                        const int available = (std::max)(0, thickness - (vertical ? 0 : Scale(4, dpi)));
                        const bool supported = vertical
                            ? (std::min)(Scale(84, dpi), available) >= Scale(mode == TaskbarWidgetMode::System ? 40 : 42, dpi)
                            : available >= Scale(24, dpi);
                        if (!supported) {
                            Check(size.width == 0 && size.height == 0,
                                "Unsupported font-fit matrix entry must return exactly {0, 0}");
                            ++unsupported_sizes;
                            continue;
                        }
                        Check(size.width > 0 && size.height > 0, "Supported font-fit entry must not be skipped");
                        const auto layout = Get(size.width, size.height, mode, vertical, dpi, edge);
                        FontMeasurement directwrite(factory.Get(), font_case.collection, family, layout.font_size);
                        GdiFontMeasurement gdi(family, layout.font_size);
                        CheckMeasuredLayout(directwrite, L"DirectWrite", family, layout, dpi, thickness,
                            vertical, mode, edge, failures, clipped_numbers);
                        CheckMeasuredLayout(gdi, L"GDI", family, layout, dpi, thickness,
                            vertical, mode, edge, failures, clipped_numbers);
                    }
                }
            }
        }
    }
    Check(families_tested == 2, "Both embedded JetBrains Mono and Windows Consolas must run actual font-fit checks");
    std::cout << "Bold font-fit failures: " << failures << "; unsupported preferred sizes: " << unsupported_sizes
        << "; intentional final numeric clipping cases: " << clipped_numbers << '\n';
    Check(failures == 0, "Supported taskbars have insufficient bold text widths; see backend/font/configuration diagnostics above");
}
#endif

void ConstrainedWindows() {
    for (unsigned dpi : { 0u, 96u, 120u, 144u, 192u }) {
        for (int width : { -9, 0, 1, 2, 3, 4, 8, 12, 24, 40, 48, 64, 71, 72, 75, 76, 80, 84, 120, 136, 273 }) {
            for (int height : { -9, 0, 1, 2, 3, 8, 15, 16, 17, 31, 32, 48, 64, 68, 72, 120, 136, 273 }) {
                for (auto mode : Modes) {
                    for (int edge : { 0, 1, 2, 3 }) {
                        const bool vertical = IsVertical(edge);
                        const auto layout = Get(width, height, mode, vertical, dpi, edge);
                        CheckBounds(layout, width, height, mode, dpi);
                        if (mode != TaskbarWidgetMode::System) {
                            CheckEqual(layout, Get(width, height, mode == TaskbarWidgetMode::Network,
                                vertical, dpi, edge));
                        }
                        Check(layout.arrow_on_right == (edge == 2), "Constrained arrow direction changed");
                        for (bool upload : { false, true }) {
                            const Rect arrow = upload ? layout.upload_arrow : layout.download_arrow;
                            Rect value = upload ? layout.upload_value : layout.download_value;
                            if (!layout.split) value.right = upload ? layout.upload_unit.right : layout.download_unit.right;
                            CheckCenteredRow(arrow, value, 1000, 1000, layout.arrow_on_right);
                        }
                    }
                }
            }
        }
    }
    const auto clipped = Get(84, 20, false, true, 96);
    Check(clipped.upload_value.top == 2 && clipped.upload_value.bottom == 18,
        "Constrained height must not shrink the first row");
    Check(clipped.download_value.top == 18 && clipped.download_value.bottom == 20,
        "Second row must clip rather than overlap first row");
    Check(Empty(clipped.cpu_label) && Empty(clipped.memory_value), "Invisible system rows must become empty");
    const int maximum = (std::numeric_limits<int>::max)();
    const int minimum = (std::numeric_limits<int>::min)();
    const unsigned dpi = (std::numeric_limits<unsigned>::max)();
    for (bool vertical : { false, true }) {
        for (auto mode : Modes) {
            CheckBounds(Get(maximum, maximum, mode, vertical, dpi), maximum, maximum, mode, dpi);
            CheckBounds(Get(minimum, minimum, mode, vertical, dpi), minimum, minimum, mode, dpi);
            const auto size = PreferredSize(mode, vertical, maximum, dpi);
            Check(size.width >= 0 && size.height >= 0, "Extreme preferred size must not overflow");
        }
    }
}
} // namespace

int main() {
    try {
        ScalingAndEdges();
        PreferredSizes();
        HorizontalRows();
        FieldPlacement();
        ModeVerticalGeometry();
        MeasuredRowCentering();
        ConstrainedWindows();
        RateFitting();
        SystemPairCentering();
#ifdef _WIN32
        BundledAndFallbackFontFitting();
#endif
        std::cout << "Taskbar widget layout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Taskbar widget layout test failed: " << error.what() << '\n';
        return 1;
    }
}
