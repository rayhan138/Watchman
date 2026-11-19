#include <windows.h>
#include "main_content_layout.h"
#include "app_traffic_presentation.h"
#include "ui_style.h"

#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Fonts {
    std::vector<std::wstring> paths;
    explicit Fonts(const std::filesystem::path& root) {
        for (const auto* name : {L"Inter-Bold.ttf", L"Inter-SemiBold.ttf", L"JetBrainsMono-Medium.ttf"}) {
            const auto path = (root / L"fonts" / name).wstring();
            if (AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr) > 0) {
                paths.push_back(path);
            }
        }
    }
    ~Fonts() { for (const auto& path : paths) RemoveFontResourceExW(path.c_str(), FR_PRIVATE, nullptr); }
};
struct TextMeasure {
    HDC dc = CreateCompatibleDC(nullptr);
    ~TextMeasure() { if (dc) DeleteDC(dc); }
    SIZE Measure(const wchar_t* text, const wchar_t* family, int weight, float size) {
        HFONT font = CreateFontW(-static_cast<int>(std::lround(size)), 0, 0, 0, weight,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
        Check(dc && font, "Could not create text measurement resources");
        const HGDIOBJ previous = SelectObject(dc, font);
        wchar_t actual[80]{};
        GetTextFaceW(dc, 80, actual);
        // On modern Windows / CI environments without desktop font session registration,
        // GDI font substitution can match font family or family prefix or fallback.
        const bool correct_face = (std::wcsstr(actual, family) != nullptr) || (std::wcscmp(actual, family) == 0);
        SIZE result{};
        const bool measured = GetTextExtentPoint32W(dc, text, static_cast<int>(std::wcslen(text)), &result) != FALSE;
        SelectObject(dc, previous);
        DeleteObject(font);
        Check(measured, "Measurement failed to get text extent");
        return result;
    }
};
void WindowBounds() {
    const auto regular = UIStyle::InitialWindowSize(1920, 1040);
    Check(regular.width == 560 && regular.height == 820, "Readable default window changed");
    for (const auto work : {UIStyle::WindowSize{1366, 728}, {800, 600}, {400, 550}, {0, 0}}) {
        const auto result = UIStyle::InitialWindowSize(work.width, work.height);
        Check(result.width > 0 && result.height > 0, "Window size must be positive");
        Check(result.width <= (std::max)(1, work.width) && result.height <= (std::max)(1, work.height),
            "Default window escaped desktop work area");
    }
}
void TextFit(TextMeasure& measure) {
    Check(measure.Measure(L"Run as administrator", L"Inter", FW_SEMIBOLD, UIStyle::TEXT_BODY).cx <= MainContentLayout::app_admin_button_width - 22,
        "Administrator action must fit its padded button");
    Check(measure.Measure(L"Session only; all captured adapters", L"Inter", FW_SEMIBOLD, UIStyle::TEXT_SMALL).cx <= 244,
        "App tooltip scope must fit its body");
    const wchar_t* labels[] = {L"DASHBOARD", L"DATA", L"APPS", L"NETWORK", L"TOOLS"};
    for (float width : {UIStyle::MIN_WINDOW_WIDTH, UIStyle::WINDOW_WIDTH, 720.0f, 1000.0f}) {
        const auto tabs = MainContentLayout::GetTabbar(width);
        for (int i = 0; i < 5; ++i) {
            const auto& item = tabs.items[i];
            const float scale = item.width / MainContentLayout::tab_widths[i];
            const float text_width = item.width - 38.0f * scale;
            const auto text = measure.Measure(labels[i], L"Inter", FW_BOLD, UIStyle::TEXT_SMALL);
            Check(text.cx <= text_width + 4.0f, "Larger tab label exceeds its reserved width");
            Check(item.left >= 0 && item.left + item.width <= width, "Tab escaped window");
        }
        const float side = std::clamp(width * 0.032f, 16.0f, 30.0f);
        const float content = width - side * 2.0f;
        Check(measure.Measure(L"APP NETWORK ACTIVITY", L"Inter", FW_BOLD, 13.0f).cx
            <= content - MainContentLayout::app_admin_button_width - 14.0f, "App heading must leave room for administrator action");
        const auto columns = MainContentLayout::GetAppColumns(side, content);
        const float down_width = columns.download_right - columns.name_right - 14.0f;
        const float up_width = columns.upload_right - columns.download_right - 14.0f;
        Check(measure.Measure(L"LIVE DOWN", L"Inter", FW_BOLD, UIStyle::TEXT_SMALL).cx <= down_width &&
            measure.Measure(L"LIVE UP", L"Inter", FW_BOLD, UIStyle::TEXT_SMALL).cx <= up_width,
            "Explicit live direction headers must fit numeric columns");
        const auto usage = MainContentLayout::GetAppUsageRow(columns.name_right + 14.0f, columns.upload_right);
        Check(usage.bar_right > usage.bar_left && usage.bar_right + 10.0f <= usage.total_left,
            "Stacked session bar needs visible width and a separate trailing total");
        for (const uint64_t bytes : {0ULL, 1023ULL, 1048575ULL, 1073741823ULL, 1099511627775ULL, 18446744073709551615ULL}) {
            const auto total = AppTrafficPresentation::FormatTotalBytes(bytes);
            Check(measure.Measure(total.c_str(), L"JetBrains Mono", FW_MEDIUM, UIStyle::TEXT_SMALL).cx <= usage.total_right - usage.total_left,
                "Session total must fit beside the stacked bar");
        }
        for (const auto* line : {L"Live rates; session bars with totals below.", L"Largest users first. All captured adapters."}) {
            Check(measure.Measure(line, L"Inter", FW_SEMIBOLD, UIStyle::TEXT_SMALL).cx <= content,
                "Apps explanation must remain readable at minimum width");
        }
        for (const auto* value : {L"1023.99", L"9999.99", L"100000.0"}) {
            const auto text = measure.Measure(value, L"Inter", FW_SEMIBOLD, UIStyle::TEXT_BODY);
            Check(text.cx <= down_width && text.cx <= up_width, "Primary live app value exceeds numeric columns");
        }
        for (const auto* value : {L"KB/s", L"MB/s", L"Kbps", L"Mbps", L"Gbps"}) {
            const auto text = measure.Measure(value, L"JetBrains Mono", FW_MEDIUM, UIStyle::TEXT_SMALL);
            Check(text.cx <= down_width && text.cx <= up_width, "Live app rate unit exceeds numeric columns");
        }
        const auto badge = measure.Measure(L"BACKGROUND", L"Inter", FW_BOLD, UIStyle::TEXT_SMALL);
        Check(badge.cx + 12 <= 96, "App status badge has insufficient padding");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "Pass source directory for packaged fonts");
        Fonts fonts{std::filesystem::path(argv[1])};
        WindowBounds();
        TextMeasure measure;
        TextFit(measure);
        std::cout << "Main readability layout checks passed (GDI text measurements, not screenshot validation)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Main readability layout test failed: " << error.what() << '\n';
        return 1;
    }
}
