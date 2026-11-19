#pragma once
#include <d2d1.h>
#include <string>

enum class ThemeMode {
    Dark = 0,
    Light = 1,
    System = 2
};

enum class ActiveTab {
    Dashboard,
    Data,
    Apps,
    Network,
    Tools
};

namespace UIStyle {
    inline constexpr int theme_count = 3;

    inline constexpr ThemeMode NormalizeTheme(int saved_value) noexcept {
        if (saved_value == static_cast<int>(ThemeMode::Light)) return ThemeMode::Light;
        if (saved_value == static_cast<int>(ThemeMode::System)) return ThemeMode::System;
        return ThemeMode::Dark;
    }

    inline constexpr ThemeMode ResolveTheme(ThemeMode requested, bool windows_light) noexcept {
        return requested == ThemeMode::System
            ? (windows_light ? ThemeMode::Light : ThemeMode::Dark) : requested;
    }

    // The sun/moon shortcut always changes the VISIBLE theme on this press.
    // Auto remains an explicit Settings choice, not an invisible third stop.
    inline constexpr ThemeMode ThemeOnQuickToggle(ThemeMode effective_theme) noexcept {
        return effective_theme == ThemeMode::Light ? ThemeMode::Dark : ThemeMode::Light;
    }

    // These are the same three families used by the Tauri app. Watchman
    // uses explicit embedded DirectWrite collections and memory-private GDI
    // registration, so this look does not depend on installed fonts or a cache.
    const wchar_t FONT_MAIN[] = L"Inter";
    const wchar_t FONT_MAIN_FALLBACK[] = L"Segoe UI";
    const wchar_t FONT_VALUES[] = L"Nunito Sans";
    const wchar_t FONT_VALUES_FALLBACK[] = L"Segoe UI";
    const wchar_t FONT_MONO[] = L"JetBrains Mono";
    const wchar_t FONT_MONO_FALLBACK[] = L"Consolas";
    // Dimensions
    const float WINDOW_WIDTH = 560.0f;
    const float WINDOW_HEIGHT = 820.0f;
    const float MIN_WINDOW_WIDTH = 460.0f;
    const float MIN_WINDOW_HEIGHT = 650.0f;
    // Main-window text only; compact taskbar/floating typography is independent.
    inline constexpr float TEXT_SMALL = 11.0f;
    inline constexpr float TEXT_BODY = 12.0f;

        struct WindowSize { int width; int height; };
        inline WindowSize InitialWindowSize(int work_width, int work_height) {
            const int width = work_width > 0 ? work_width : 1;
            const int height = work_height > 0 ? work_height : 1;
            return { width < static_cast<int>(WINDOW_WIDTH) ? width : static_cast<int>(WINDOW_WIDTH),
                     height < static_cast<int>(WINDOW_HEIGHT) ? height : static_cast<int>(WINDOW_HEIGHT) };
        }
    // The title bar matches Tauri's 48px flex surface. Its tab strip has an
    // intrinsic 37px height at the compact window size (14px icon plus the
    // source padding and its bottom rule).
    const float TITLEBAR_HEIGHT = 47.0f;
    const float TABBAR_HEIGHT = 37.0f;
    const float STATUSBAR_HEIGHT = 32.0f;
    
    const float CORNER_RADIUS_LARGE = 18.0f;
    const float CORNER_RADIUS_SMALL = 8.0f;

    // Fluent Colors - Theme Dependent
    struct ColorPalette {
        D2D1_COLOR_F Background;
        D2D1_COLOR_F SidebarBackground;    // Used for titlebar/tabbar backgrounds
        D2D1_COLOR_F CardBackground;
        D2D1_COLOR_F CardBorder;
        D2D1_COLOR_F TextPrimary;
        D2D1_COLOR_F TextSecondary;
        D2D1_COLOR_F AccentDownload;       // Download/chart accent
        D2D1_COLOR_F AccentUpload;         // Upload/chart accent
        D2D1_COLOR_F AccentSystem;         // System/status accent
        D2D1_COLOR_F ControlHover;
        D2D1_COLOR_F ControlActive;
        D2D1_COLOR_F ActionBackground;     // White-label button fill, separate from chart accents
    };

    // History has its own coordinated blue pair. Do not reuse the unrelated
    // blue/orange live-traffic accents for these stacked comparison bars.
    struct HistoryPalette {
        D2D1_COLOR_F Download, Upload, Grid, Tick;
    };
    inline HistoryPalette GetHistoryPalette(ThemeMode resolved_mode) {
        if (resolved_mode == ThemeMode::Light) {
            return { D2D1::ColorF(0x1E3A8A), D2D1::ColorF(0x60A5FA),
                     D2D1::ColorF(0.42f, 0.40f, 0.37f, 0.32f), D2D1::ColorF(0x666666) };
        }
        return { D2D1::ColorF(0x4D8FD6), D2D1::ColorF(0xAACFF2),
                 D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), D2D1::ColorF(0xCCCCCC) };
    }

    // Light retains its pastel surfaces; Dark uses opaque neutral surfaces.
    inline ColorPalette GetPalette(ThemeMode mode) {
        ColorPalette palette;
        if (mode == ThemeMode::Light) {
            // Original Tauri :root colors; the renderer supplies the three
            // blurred pastel disks over this clean white base.
            palette.Background = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            palette.SidebarBackground = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f);
            // Session/snapshot warm gradients are separate renderer resources.
            // General filled surfaces use the reference's translucent white.
            palette.CardBackground = D2D1::ColorF(0xFFFFFF, 0.90f);
            palette.CardBorder = D2D1::ColorF(0x121212, 1.0f);
            palette.TextPrimary = D2D1::ColorF(0x000000, 1.0f);
            // The source app uses #333 for muted copy. The previous lighter
            // gray made Network diagnostics look faded against the surface.
            palette.TextSecondary = D2D1::ColorF(0.20f, 0.20f, 0.20f, 1.0f);
            palette.AccentDownload = D2D1::ColorF(0x3B82F6, 1.0f);
            palette.AccentUpload = D2D1::ColorF(0xF97316, 1.0f);
            palette.AccentSystem = D2D1::ColorF(0x10B981, 1.0f);
            palette.ControlHover = D2D1::ColorF(0xFFFFFF, 0.40f);
            palette.ControlActive = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f);
            palette.ActionBackground = D2D1::ColorF(0x3B82F6, 1.0f);
        } else { // Dark Mode (Default) or fallback for System before resolution
            palette.Background = D2D1::ColorF(0x000000, 1.0f);
            palette.SidebarBackground = D2D1::ColorF(0x000000, 1.0f);
            palette.CardBackground = D2D1::ColorF(0x101010, 1.0f);
            palette.CardBorder = D2D1::ColorF(0x383838, 1.0f);
            palette.TextPrimary = D2D1::ColorF(0xFFFFFF, 1.0f);
            palette.TextSecondary = D2D1::ColorF(0xCCCCCC, 1.0f);
            palette.AccentDownload = D2D1::ColorF(0x2699FF, 1.0f);
            palette.AccentUpload = D2D1::ColorF(0xFF801A, 1.0f);
            palette.AccentSystem = D2D1::ColorF(0x1AD966, 1.0f);
            palette.ControlHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
            palette.ControlActive = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
            palette.ActionBackground = D2D1::ColorF(0x1D4ED8, 1.0f);
        }
        return palette;
    }
}
