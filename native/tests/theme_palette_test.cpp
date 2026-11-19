#include "ui_style.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void CheckColor(const D2D1_COLOR_F& actual, const D2D1_COLOR_F& expected, const char* message) {
    Check(actual.r == expected.r && actual.g == expected.g
        && actual.b == expected.b && actual.a == expected.a, message);
}

double LinearChannel(float channel) {
    const double value = channel;
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double Luminance(const D2D1_COLOR_F& color) {
    return 0.2126 * LinearChannel(color.r)
        + 0.7152 * LinearChannel(color.g) + 0.0722 * LinearChannel(color.b);
}

double Contrast(const D2D1_COLOR_F& first, const D2D1_COLOR_F& second) {
    const double left = Luminance(first);
    const double right = Luminance(second);
    return left >= right ? (left + 0.05) / (right + 0.05)
                         : (right + 0.05) / (left + 0.05);
}

void ThemeNormalization() {
    static_assert(static_cast<int>(ThemeMode::Dark) == 0, "Dark's persisted ID must stay 0");
    static_assert(static_cast<int>(ThemeMode::Light) == 1, "Light's persisted ID must stay 1");
    static_assert(static_cast<int>(ThemeMode::System) == 2, "System's persisted ID must stay 2");
    static_assert(UIStyle::theme_count == 3, "Dark, Light, and System are supported");
    Check(UIStyle::NormalizeTheme(0) == ThemeMode::Dark, "Saved Dark must stay Dark");
    Check(UIStyle::NormalizeTheme(1) == ThemeMode::Light, "Saved Light must stay Light");
    Check(UIStyle::NormalizeTheme(2) == ThemeMode::System, "Saved System must stay System");
    for (int invalid : { -1, 3, 42, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)() }) {
        Check(UIStyle::NormalizeTheme(invalid) == ThemeMode::Dark, "Invalid saved themes must become Dark");
    }
}

void DirectThemeShortcut() {
    for (bool windows_light : {false, true}) {
        for (auto requested : {ThemeMode::Dark, ThemeMode::Light, ThemeMode::System}) {
            auto effective = UIStyle::ResolveTheme(requested, windows_light);
            const auto initial = effective;
            for (int press = 0; press < 100; ++press) {
                const auto next = UIStyle::ThemeOnQuickToggle(effective);
                Check(next != ThemeMode::System, "Titlebar shortcut must not have an invisible Auto stop");
                Check(next != effective, "Every accepted press must change the visible palette");
                Check(next == (effective == ThemeMode::Light ? ThemeMode::Dark : ThemeMode::Light),
                    "The shortcut must use the opposite effective theme, including when leaving Auto");
                effective = UIStyle::ResolveTheme(next, windows_light);
                if (press % 2 == 1) Check(effective == initial, "Two consecutive presses must return to the starting colors");
            }
        }
    }
}

void HistoryPaletteAndSystemTheme() {
    for (bool light : {false, true}) {
        const auto resolved = UIStyle::ResolveTheme(ThemeMode::System, light);
        Check(resolved == (light ? ThemeMode::Light : ThemeMode::Dark), "System must resolve before ALL theme styling");
        Check(UIStyle::ResolveTheme(ThemeMode::Dark, light) == ThemeMode::Dark, "Explicit Dark must ignore Windows theme");
        Check(UIStyle::ResolveTheme(ThemeMode::Light, light) == ThemeMode::Light, "Explicit Light must ignore Windows theme");
        const auto history = UIStyle::GetHistoryPalette(resolved);
        const auto palette = UIStyle::GetPalette(resolved);
        Check(history.Download.b > history.Download.r && history.Upload.b > history.Upload.r,
            "History stacks must use coordinated blues, not an unrelated orange segment");
        Check(Contrast(history.Download, palette.CardBackground) >= 3.0 && Contrast(history.Upload, palette.CardBackground) >= 1.5,
            "Both history segments must stay visible on their theme surface");
        Check(Contrast(history.Download, history.Upload) >= 2.0, "Stacked series need distinct luminance as well as a legend");
        Check(Contrast(history.Tick, palette.CardBackground) >= 4.5, "History labels must be legible");
    }
    const auto light = UIStyle::GetHistoryPalette(ThemeMode::Light);
    CheckColor(light.Download, D2D1::ColorF(0x1E3A8A), "Keep Light history's existing deep-blue download");
    CheckColor(light.Upload, D2D1::ColorF(0x60A5FA), "Keep Light history's existing light-blue upload");
}

void DarkPalette() {
    const auto palette = UIStyle::GetPalette(ThemeMode::Dark);
    CheckColor(palette.Background, D2D1::ColorF(0x000000, 1.0f), "Dark background must be opaque pure black");
    CheckColor(palette.SidebarBackground, D2D1::ColorF(0x000000, 1.0f), "Dark sidebar must be opaque pure black");
    CheckColor(palette.CardBackground, D2D1::ColorF(0x101010, 1.0f), "Dark cards must be opaque neutral #101010");
    CheckColor(palette.CardBorder, D2D1::ColorF(0x383838, 1.0f), "Dark borders must be opaque neutral #383838");
    CheckColor(palette.TextPrimary, D2D1::ColorF(0xFFFFFF, 1.0f), "Dark primary text must be white");
    CheckColor(palette.TextSecondary, D2D1::ColorF(0xCCCCCC, 1.0f), "Dark secondary text must be #CCCCCC");
    CheckColor(palette.ControlHover, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), "Dark hover must be 8% white");
    CheckColor(palette.ControlActive, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), "Dark active must be 14% white");
    CheckColor(palette.AccentDownload, D2D1::ColorF(0x2699FF, 1.0f), "Download must retain a bright blue accent");
    CheckColor(palette.AccentUpload, D2D1::ColorF(0xFF801A, 1.0f), "Upload must retain an orange accent");
    CheckColor(palette.AccentSystem, D2D1::ColorF(0x1AD966, 1.0f), "System must retain a green accent");
    CheckColor(palette.ActionBackground, D2D1::ColorF(0x1D4ED8, 1.0f), "Dark action buttons need a dedicated darker fill");
    Check(Contrast(palette.TextPrimary, palette.ActionBackground) >= 4.5,
        "White action button labels must meet WCAG 4.5:1 contrast");

    for (const auto& background : { palette.Background, palette.SidebarBackground, palette.CardBackground }) {
        for (const auto& text : { palette.TextPrimary, palette.TextSecondary }) {
            Check(Contrast(text, background) >= 4.5, "Dark text must meet WCAG 4.5:1 contrast");
        }
        for (const auto& accent : { palette.AccentDownload, palette.AccentUpload, palette.AccentSystem }) {
            Check(Contrast(accent, background) >= 3.0, "Dark graphic accents must meet WCAG 3:1 contrast");
        }
    }
}

void LightPaletteReference() {
    // D:/watchman c++ windows/Watchman-main/src/styles.css :root and speed arrows.
    const auto palette = UIStyle::GetPalette(ThemeMode::Light);
    CheckColor(palette.Background, D2D1::ColorF(0xFFFFFF, 1.0f), "Light base must be white");
    CheckColor(palette.SidebarBackground, D2D1::ColorF(0xFFFFFF, 0.0f), "Light bars must expose the pastel background");
    CheckColor(palette.CardBackground, D2D1::ColorF(0xFFFFFF, 0.90f), "Default Light cards must be white");
    CheckColor(palette.CardBorder, D2D1::ColorF(0x121212, 1.0f), "Light borders must match CSS");
    CheckColor(palette.TextPrimary, D2D1::ColorF(0x000000, 1.0f), "Light primary text must match CSS");
    CheckColor(palette.TextSecondary, D2D1::ColorF(0x333333, 1.0f), "Light secondary text must match CSS");
    CheckColor(palette.AccentDownload, D2D1::ColorF(0x3B82F6, 1.0f), "Light download must match CSS");
    CheckColor(palette.AccentUpload, D2D1::ColorF(0xF97316, 1.0f), "Light upload must match CSS");
    CheckColor(palette.AccentSystem, D2D1::ColorF(0x10B981, 1.0f), "Light success must match CSS");
    CheckColor(palette.ControlHover, D2D1::ColorF(0xFFFFFF, 0.40f), "Light hover must match CSS");
    CheckColor(palette.ActionBackground, D2D1::ColorF(0x3B82F6, 1.0f), "Light action must match CSS");
    Check(Contrast(palette.TextPrimary, palette.Background) >= 7.0, "Light primary text must have strong contrast");
    Check(UIStyle::TEXT_SMALL >= 11.0f && UIStyle::TEXT_BODY >= 12.0f,
        "Main-window type must not return to tiny labels");
}
} // namespace

int main() {
    try {
        ThemeNormalization();
        DirectThemeShortcut();
        DarkPalette();
        HistoryPaletteAndSystemTheme();
        LightPaletteReference();
        std::cout << "Theme palette tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Theme palette test failed: " << error.what() << '\n';
        return 1;
    }
}
