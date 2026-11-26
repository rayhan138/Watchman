#include "floating_monitor_settings_layout.h"
#include "floating_monitor_layout.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.01f;
}

void CheckLayout(float width, float height, float requested_scroll) {
    const float modal_top = 10.0f;
    const float left = 154.0f;
    const float right = width - 8.0f;
    const auto page = GetFloatingMonitorSettingsLayout(left, right, modal_top, height, requested_scroll);
    Check(page.view_top < page.view_bottom, "Content viewport must have positive height");
    Check(page.scroll_offset >= 0.0f && page.scroll_offset <= page.scroll_max, "Scroll offset must be clamped");
    Check(page.readings_card.bottom + 16.0f <= page.font_card.top, "Text style follows Readings with a readable gap");
    Check(page.cpu_row.top >= page.readings_card.top + 44.0f && page.cpu_row.bottom <= page.memory_row.top,
        "CPU and memory toggles need their own rows below the Readings title");
    Check(page.memory_row.bottom < page.readings_card.bottom, "Readings need bottom padding");
    for (const auto& row : {page.cpu_row, page.memory_row}) {
        Check(row.left > page.readings_card.left && row.right < page.readings_card.right && row.bottom - row.top >= 48.0f,
            "Independent reading toggles need usable hit targets inside their card");
    }
    Check(page.colors_card.top - page.font_card.bottom >= 16.0f, "Colors follow Text style with a readable gap");
    Check(page.controls_card.top - page.colors_card.bottom >= 16.0f, "Behavior controls follow Colors with a readable gap");
    Check(page.lock_row.bottom <= page.transparent_row.top, "Lock and transparency rows must not overlap");
    Check(page.opacity_minus.right < page.opacity_value.left && page.opacity_value.right < page.opacity_plus.left,
        "Opacity controls must have separate hit targets");
    Check(page.opacity_plus.bottom < page.reset_position.top, "Reset position needs its own row");
    Check(page.opacity_minus.left - page.lock_row.left >= 70.0f, "Opacity label needs room at minimum width");
    Check(page.reset_position.bottom < page.controls_card.bottom, "Reset control must remain inside its card");
    Check(page.weight_controls.size() == FloatingMonitorText::weights.size(), "Every supported weight has a visible choice");
    Check(page.weight_label.top >= page.font_description.bottom && page.weight_label.bottom < page.weight_controls.front().top,
        "Weight label needs its own row below the family/default description");
    for (size_t index = 0; index < page.weight_controls.size(); ++index) {
        const auto& control = page.weight_controls[index];
        Check(control.left >= page.font_description.left && control.right <= page.font_description.right + 0.01f,
            "Weight choices must stay inside the Text style card");
        Check(control.bottom - control.top >= 44.0f && control.right - control.left >= 60.0f,
            "Weight choices need full-size targets and readable labels at minimum width");
        Check(control.bottom + 16.0f <= page.font_minus.top, "Weight and size are separate, nonoverlapping rows");
        if (index) Check(control.left >= page.weight_controls[index - 1].right + 7.99f,
            "Weight choices need separate hit targets, not overlapping click regions");
    }
    Check(page.colors_card.left == left && page.font_card.right == right, "Cards must share their edges");
    Check(page.font_description.top >= page.font_card.top + 44.0f,
        "Font description must leave room for the Text style title");
    Check(page.font_description.bottom < page.font_minus.top,
        "Font description and size controls need separate rows");
    Check(page.font_description.right - page.font_description.left >= 200.0f,
        "Fixed font description needs readable space at minimum modal width");
    Check(page.font_minus.right < page.font_value.left && page.font_value.right < page.font_plus.left,
        "Text size controls must have separate hit targets");
    Check(Near(page.font_minus.top, page.font_value.top) && Near(page.font_value.top, page.font_plus.top)
        && Near(page.font_minus.bottom, page.font_value.bottom) && Near(page.font_value.bottom, page.font_plus.bottom),
        "Text size controls must share one row");
    Check(page.font_minus.left - page.font_description.left >= 40.0f,
        "Text size row label needs space at minimum modal width");
    for (const auto& control : { page.font_description, page.font_minus, page.font_value, page.font_plus }) {
        Check(control.left > page.font_card.left && control.right < page.font_card.right,
            "Text size controls must stay inside their card");
        Check(control.top > page.font_card.top && control.bottom < page.font_card.bottom,
            "Text size controls need vertical padding");
        Check(control.left < control.right && control.top < control.bottom,
            "Text size controls must have positive bounds");
    }
    Check(Near(page.font_minus.right - page.font_minus.left, 40.0f)
        && Near(page.font_plus.right - page.font_plus.left, 40.0f)
        && Near(page.font_plus.bottom - page.font_plus.top, 40.0f),
        "Text size buttons need full-size hit targets");
    Check(page.preview.top >= page.font_plus.bottom + 16.0f && page.preview.bottom < page.preview_caption.top,
        "Preview must have its own space below the size controls");
    Check(page.preview_caption.bottom < page.font_card.bottom, "Preview caption needs bottom padding");
    Check(page.preview.right > page.preview.left && page.preview.bottom - page.preview.top >= 80.0f,
        "Preview needs room for two draft-font sample lines without shrinking");
    for (size_t index = 0; index < page.color_count; ++index) {
        const auto& row = page.color_rows[index];
        const auto& control = page.color_controls[index];
        Check(control.left > row.left && control.right <= row.right, "Color controls must stay inside their rows");
        Check(control.top >= row.top && control.bottom <= row.bottom, "Color controls must match their row hit targets");
        Check(control.left - row.left >= 115.0f, "Color labels need space before the swatch at minimum app width");
        Check(row.top > page.colors_card.top && row.bottom < page.colors_card.bottom, "Color rows must stay inside their card");
        if (index > 0) {
            Check(Near(row.top, page.color_rows[index - 1].bottom), "Color rows must not overlap");
        }
    }
    if (Near(page.scroll_offset, page.scroll_max)) {
        Check(page.controls_card.bottom <= page.view_bottom + 0.01f, "Bottom of page must be reachable above the footer");
    }
}

void DefaultControlsFit() {
    const auto page = GetFloatingMonitorSettingsLayout(154.0f, 492.0f, 10.0f, 740.0f, 0.0f);
    Check(page.font_plus.bottom < page.view_bottom && page.weight_controls.front().top > page.view_top,
        "Weight and size controls must be available without scrolling at the normal height");
    Check(page.scroll_max <= 700.0f, "Readings, typography, five colors and behavior have a bounded normal-size scroll");
    const auto text = GetFloatingMonitorSettingsLayout(154.0f, 492.0f, 10.0f, 740.0f, 104.0f);
    Check(text.font_card.top >= text.view_top && text.preview_caption.bottom <= text.view_bottom,
        "After two wheel detents, the entire Text style card and its preview fit together");
    const auto bottom = GetFloatingMonitorSettingsLayout(154.0f, 492.0f, 10.0f, 740.0f, page.scroll_max);
    Check(bottom.controls_card.top >= bottom.view_top && bottom.reset_position.bottom <= bottom.view_bottom,
        "All position/transparency controls must be reachable together above the footer");
    Check(page.readings_card.top > page.view_top, "Cards must not touch the fixed title");
}

void ScrollRangeIsStable() {
    const auto initial = GetFloatingMonitorSettingsLayout(154.0f, 432.0f, 10.0f, 460.0f, 0.0f);
    Check(initial.scroll_max > 0.0f, "Short dialogs must scroll rather than compress controls");
    const auto middle = GetFloatingMonitorSettingsLayout(154.0f, 432.0f, 10.0f, 460.0f, initial.scroll_max / 2.0f);
    const auto bottom = GetFloatingMonitorSettingsLayout(154.0f, 432.0f, 10.0f, 460.0f, initial.scroll_max);
    Check(Near(initial.scroll_max, middle.scroll_max) && Near(initial.scroll_max, bottom.scroll_max),
        "Scroll maximum must not shrink as the page scrolls");
    Check(initial.view_top == bottom.view_top && initial.view_bottom == bottom.view_bottom,
        "Title and footer viewport boundaries must not scroll");
    Check(Near(initial.font_description.top - bottom.font_description.top, initial.scroll_max)
        && Near(initial.font_minus.top - bottom.font_minus.top, initial.scroll_max)
        && Near(initial.font_value.top - bottom.font_value.top, initial.scroll_max)
        && Near(initial.font_plus.top - bottom.font_plus.top, initial.scroll_max),
        "Description and size hit targets must move by the same scroll offset");
}

void ResizeAndClamp() {
    const auto negative = GetFloatingMonitorSettingsLayout(154.0f, 432.0f, 10.0f, 460.0f, -500.0f);
    Check(Near(negative.scroll_offset, 0.0f), "Negative scroll must clamp to the top");
    const auto bottom = GetFloatingMonitorSettingsLayout(154.0f, 432.0f, 10.0f, 460.0f, 10000.0f);
    Check(Near(bottom.scroll_offset, bottom.scroll_max), "Excessive scroll must clamp to the bottom");
    const auto enlarged = GetFloatingMonitorSettingsLayout(154.0f, 492.0f, 10.0f, 740.0f, bottom.scroll_offset);
    Check(Near(enlarged.scroll_offset, enlarged.scroll_max) && enlarged.scroll_max < bottom.scroll_max,
        "Growing the dialog must clamp obsolete scrolling to its new maximum");
    const auto very_tall = GetFloatingMonitorSettingsLayout(154.0f, 492.0f, 10.0f, 1400.0f, enlarged.scroll_offset);
    Check(Near(very_tall.scroll_offset, 0.0f), "A page that fits must clear obsolete scrolling");
}
void MonitorFontSizesAndBounds() {
    using namespace FloatingMonitorLayout;
    Check(minimum_font_size == 11 && maximum_font_size == 16 && default_font_size == 12,
        "Monitor text must use the shared 11..16px range and 12px default");
    Check(width == 188 && Width(default_font_size) == 188 && Height() == 56 && Height(12, false, false) == 28,
        "Default 188x56 monitor includes CPU/memory; network-only remains 188x28");
    Check(default_text_color == 0x00FFFFFFu && panel_background_color == 0,
        "Default text must stay white on a black panel, independent of app theme");
    static_assert(sizeof(Layout) == 8 * sizeof(Rect), "Floating monitor includes CPU/memory label/value fields");
    Check(FloatingMonitorSettingsLayout::color_count == 5,
        "Values, upload, download, CPU and memory need independent color controls");
    for (const int size : { std::numeric_limits<int>::min(), -1, 0, 10 }) {
        Check(ClampFontSize(size) == 11 && Width(size) == Width(11) && Height(size) == Height(11),
            "Font sizes below the minimum must clamp before dimension calculations");
    }
    for (const int size : { 17, 100, std::numeric_limits<int>::max() }) {
        Check(ClampFontSize(size) == 16 && Width(size) == Width(16) && Height(size) == Height(16),
            "Font sizes above the maximum must clamp before dimension calculations");
    }
    const auto check_bounds = [](const Rect& rect, int surface_width, int surface_height) {
        Check(rect.left >= 0 && rect.left <= rect.right && rect.right <= surface_width,
            "Monitor text must have ordered horizontal bounds inside the surface");
        Check(rect.top >= 0 && rect.top <= rect.bottom && rect.bottom <= surface_height,
            "Monitor text must have ordered vertical bounds inside the surface");
    };
    for (int size = minimum_font_size; size <= maximum_font_size; ++size) {
        Check(ClampFontSize(size) == size, "Valid font sizes must be retained");
        const int surface_width = Width(size);
        const int height = Height(size);
        Check(surface_width == static_cast<int>(std::ceil(188.0 * size / 12.0)),
            "Monitor width must scale from 188px at 12px, rounded up");
        Check(height == 2 * std::max(28, size + 12), "Enabled CPU/memory share one additional row");
        const auto layout = Get(surface_width, height, size);
        for (const auto& rect : { layout.upload_label, layout.upload_value, layout.download_label, layout.download_value }) {
            check_bounds(rect, surface_width, height);
        }
        Check(layout.upload_label.right < layout.upload_value.left
            && layout.upload_value.right < layout.download_label.left
            && layout.download_label.right < layout.download_value.left,
            "Network labels and values must have nonoverlapping columns and gaps");
        for (const auto& label : { layout.upload_label, layout.download_label }) {
            Check(label.right - label.left >= 20 * surface_width / 188,
                "UP/DN labels need scaled 20px columns");
            Check(label.bottom - label.top >= size, "Network text must fit vertically");
        }
        for (const auto& value : { layout.upload_value, layout.download_value }) {
            Check(value.right - value.left >= 60 * surface_width / 188,
                "Network rates need scaled 60px value columns");
        }
        Check(layout.upload_value.left - layout.upload_label.right >= 4 * surface_width / 188
            && layout.download_value.left - layout.download_label.right >= 4 * surface_width / 188,
            "UP/DN labels need a scaled 4px gap before values");
    }
    const auto defaults = Get(width, Height());
    Check(defaults.upload_label.right - defaults.upload_label.left == 20
        && defaults.download_label.right - defaults.download_label.left == 20
        && defaults.upload_value.left - defaults.upload_label.right == 4
        && defaults.download_value.left - defaults.download_label.right == 4,
        "Two-argument Get must preserve default UP/DN column sizes");
    for (const int surface_width : { 0, 1, 100, 200, 267, 400 }) {
        for (const int surface_height : { 0, 1, 10, 28, 52 }) {
            const auto layout = Get(surface_width, surface_height, maximum_font_size);
            for (const auto& rect : { layout.upload_label, layout.upload_value, layout.download_label, layout.download_value }) {
                check_bounds(rect, surface_width, surface_height);
            }
        }
    }
}

class GdiFontMeasurement {
public:
    explicit GdiFontMeasurement(int size, FloatingMonitorText::Weight weight) : dc_(CreateCompatibleDC(nullptr)), weight_(weight) {
        Check(dc_ != nullptr, "CreateCompatibleDC failed for font measurement");
        font_ = CreateFontW(-size, 0, 0, 0, FloatingMonitorText::Value(weight), FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, FloatingMonitorText::GdiFamily(weight));
        if (!font_) {
            DeleteDC(dc_);
            throw std::runtime_error("CreateFontW failed for chosen Segoe UI weight");
        }
        previous_font_ = SelectObject(dc_, font_);
        if (!previous_font_ || previous_font_ == HGDI_ERROR) {
            DeleteObject(font_);
            DeleteDC(dc_);
            throw std::runtime_error("SelectObject failed for chosen Segoe UI weight");
        }
    }

    ~GdiFontMeasurement() {
        SelectObject(dc_, previous_font_);
        DeleteObject(font_);
        DeleteDC(dc_);
    }

    GdiFontMeasurement(const GdiFontMeasurement&) = delete;
    GdiFontMeasurement& operator=(const GdiFontMeasurement&) = delete;

    void CheckSelectedFont() const {
        wchar_t face[LF_FACESIZE]{};
        Check(GetTextFaceW(dc_, LF_FACESIZE, face) > 0, "GetTextFaceW failed");
        Check(std::wstring(face).rfind(L"Segoe UI", 0) == 0, "Font measurement must not silently substitute another family");
        TEXTMETRICW metrics{};
        Check(GetTextMetricsW(dc_, &metrics) != FALSE, "GetTextMetricsW failed");
        if (metrics.tmWeight != FloatingMonitorText::Value(weight_)) {
            std::wcerr << L"Requested GDI face: " << FloatingMonitorText::GdiFamily(weight_) << L"; selected: " << face << L'\n';
            throw std::runtime_error("Actual GDI weight " + std::to_string(metrics.tmWeight) +
                " differs from requested " + std::to_string(FloatingMonitorText::Value(weight_)));
        }
    }

    SIZE Measure(const std::wstring& text) const {
        SIZE extent{};
        Check(GetTextExtentPoint32W(dc_, text.c_str(), static_cast<int>(text.size()), &extent) != FALSE,
            "GetTextExtentPoint32W failed");
        return extent;
    }

private:
    HDC dc_;
    FloatingMonitorText::Weight weight_;
    HFONT font_ = nullptr;
    HGDIOBJ previous_font_ = nullptr;
};

void GdiMonitorTextFits() {
    using namespace FloatingMonitorLayout;
    for (const auto weight : FloatingMonitorText::weights) for (int size = minimum_font_size; size <= maximum_font_size; ++size) {
        const GdiFontMeasurement font(size, weight);
        font.CheckSelectedFont();
        const auto fits = [&font, size](const std::wstring& text, const Rect& column) {
            const SIZE extent = font.Measure(text);
            if (extent.cy > column.bottom - column.top) {
                throw std::runtime_error("GDI text height exceeds column at " + std::to_string(size) + "px");
            }
            return extent.cx <= column.right - column.left;
        };
        const auto require_fit = [&fits, size](const std::wstring& text, const Rect& column) {
            if (!fits(text, column)) {
                throw std::runtime_error("GDI text width exceeds column at " + std::to_string(size) + "px");
            }
        };
        const auto layout = Get(Width(size), Height(size), size);
        require_fit(L"UP", layout.upload_label);
        require_fit(L"DN", layout.download_label);
        require_fit(L"CPU", layout.cpu_label);
        require_fit(L"MEM", layout.memory_label);
        for (const auto& column : {layout.cpu_value, layout.memory_value}) {
            for (const auto* percentage : {L"0%", L"24%", L"58%", L"100%", L"--"}) require_fit(percentage, column);
        }
        for (const bool cpu : {false, true}) {
            for (const bool memory : {false, true}) {
                const auto selected = Get(Width(size), Height(size, cpu, memory), size, cpu, memory);
                if (cpu) { require_fit(L"CPU", selected.cpu_label); require_fit(L"100%", selected.cpu_value); }
                if (memory) { require_fit(L"MEM", selected.memory_label); require_fit(L"100%", selected.memory_value); }
            }
        }
        for (const auto& column : { layout.upload_value, layout.download_value }) {
            for (const auto* rate : { L"1.2 MB/s", L"8.6 MB/s", L"0.0KB/s", L"999GB/s", L"999Gbps" }) {
                require_fit(rate, column);
            }
            for (const auto* rate : { L"999.9 KB/s", L"999.9 GB/s", L"999.9 Gbps", L"999.999 GB/s" }) {
                std::wstring compact(rate);
                if (fits(compact, column)) continue;
                // Match the renderer's fallback order without shrinking the font.
                compact.erase(std::remove(compact.begin(), compact.end(), L' '), compact.end());
                if (fits(compact, column)) continue;
                const auto decimal = compact.find(L'.');
                const auto unit = compact.find_first_not_of(L"0123456789", decimal + 1);
                Check(decimal != std::wstring::npos && unit != std::wstring::npos,
                    "Long-rate fixture must allow dropping its decimal portion");
                compact.erase(decimal, unit - decimal);
                require_fit(compact, column);
            }
        }
    }
}

void OpacityAndAlpha() {
    Check(FloatingMonitorLayout::ClampOpacity(-1) == 20 && FloatingMonitorLayout::ClampOpacity(101) == 100,
        "Menu/settings opacity must share safe limits");
    Check(FloatingMonitorLayout::ClampOpacity(65) == 65, "Valid opacity must be retained");
    Check(FloatingMonitorLayout::PremultipliedPixel(200, 100, 50, 0) == 0, "Zero alpha must have zero color channels");
    Check(FloatingMonitorLayout::PremultipliedPixel(0, 0, 0, 1) == 0x01000000u, "Transparent hit surface must not contain visible RGB");
    for (unsigned alpha : { 1u, 64u, 128u, 255u }) {
        const auto pixel = FloatingMonitorLayout::PremultipliedPixel(255, 170, 85, alpha);
        Check((pixel >> 24u) == alpha, "Per-pixel alpha must be preserved");
        Check((pixel & 255u) <= alpha && ((pixel >> 8u) & 255u) <= alpha && ((pixel >> 16u) & 255u) <= alpha,
            "Layered surface color channels must be premultiplied");
    }
}
} // namespace

int main() {
    try {
        DefaultControlsFit();
        MonitorFontSizesAndBounds();
        GdiMonitorTextFits();
        OpacityAndAlpha();
        ScrollRangeIsStable();
        ResizeAndClamp();
        for (const float width : { 400.0f, 420.0f, 440.0f, 460.0f, 500.0f }) {
            for (const float height : { 460.0f, 630.0f, 740.0f }) {
                for (const float offset : { -50.0f, 0.0f, 52.0f, 10000.0f }) {
                    CheckLayout(width, height, offset);
                }
            }
        }
        std::cout << "Floating monitor: Light/Regular/Bold font measurements, size bounds, scaled layout, alpha, opacity, settings scrolling and control bounds passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
