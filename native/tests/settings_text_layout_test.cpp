#include "settings_text_layout.h"
#include "theme_picker_layout.h"
#include "warnings_settings_layout.h"
#include "ui_style.h"
#include "welcome_tour.h"
#include "speed_test_presentation.h"
#include "bundled_fonts.h"

#include <dwrite.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

ComPtr<IDWriteTextFormat> Format(IDWriteFactory* factory, IDWriteFontCollection* fonts,
                                const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD) {
    ComPtr<IDWriteTextFormat> format;
    Check(SUCCEEDED(factory->CreateTextFormat(family, fonts, weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", format.GetAddressOf())),
        "Cannot create settings text format");
    return format;
}

void FullParagraph(const SettingsText::Paragraph& paragraph, const wchar_t* text, float width, bool must_wrap = true) {
    Check(paragraph.layout && paragraph.height > 0, "Wrapped paragraph must own a layout and measured height");
    Check(paragraph.layout->GetWordWrapping() == DWRITE_WORD_WRAPPING_WRAP &&
          paragraph.layout->GetTextAlignment() == DWRITE_TEXT_ALIGNMENT_LEADING &&
          paragraph.layout->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
        "Warning notes must override inherited compact no-wrap and vertical alignment");
    DWRITE_TEXT_METRICS metrics{};
    Check(SUCCEEDED(paragraph.layout->GetMetrics(&metrics)), "Cannot measure wrapped paragraph");
    Check(metrics.lineCount > 0 && (!must_wrap || metrics.lineCount > 1), "Long notes must actually wrap, not just clip a single line");
    Check(metrics.width <= width + 0.05f && metrics.left >= -0.05f,
        "Wrapped warning text must stay inside the card's horizontal padding");
    Check(metrics.height <= paragraph.height && paragraph.layout->GetMaxHeight() == paragraph.height,
        "Measured paragraph height must cover every line and match the drawing clip");
    std::vector<DWRITE_LINE_METRICS> lines(metrics.lineCount);
    UINT32 line_count = 0;
    Check(SUCCEEDED(paragraph.layout->GetLineMetrics(lines.data(), static_cast<UINT32>(lines.size()), &line_count)),
        "Cannot inspect wrapped lines");
    UINT32 characters = 0;
    float line_height = 0;
    for (const auto& line : lines) {
        Check(!line.isTrimmed, "Warning instructions must not be ellipsized or trimmed");
        characters += line.length;
        line_height += line.height;
    }
    Check(line_count == metrics.lineCount && characters == std::wcslen(text),
        "Wrapped layout must retain the entire explanation, including its final sentence");
    Check(line_height <= paragraph.height + 0.05f, "Last wrapped line must not be hidden below the card");
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS last{};
    Check(SUCCEEDED(paragraph.layout->HitTestTextPosition(characters - 1, FALSE, &x, &y, &last)),
        "Cannot locate final paragraph character");
    Check(last.top + last.height <= paragraph.height + 0.05f && last.left + last.width <= width + 0.05f,
        "Final punctuation must be visible inside the text area");
}

void SingleLine(IDWriteFactory* factory, IDWriteTextFormat* format, const wchar_t* text, float width, float height) {
    ComPtr<IDWriteTextLayout> layout;
    Check(SUCCEEDED(factory->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)),
        format, width, height, layout.GetAddressOf())), "Cannot lay out theme label");
    DWRITE_TEXT_METRICS metrics{};
    Check(SUCCEEDED(layout->GetMetrics(&metrics)), "Cannot measure theme label");
    Check(metrics.lineCount == 1 && metrics.widthIncludingTrailingWhitespace <= width + 0.05f,
        "Theme labels and Auto explanation must fit fully on one line at minimum width");
    Check(metrics.height <= height && std::abs(metrics.top + metrics.height / 2 - height / 2) < 0.1f,
        "Theme text line box must fit and share the radio's vertical center");
}

void FontCases(IDWriteFactory* factory, IDWriteFontCollection* fonts, const wchar_t* family) {
    const auto note = Format(factory, fonts, family, UIStyle::TEXT_SMALL);
    Check(SUCCEEDED(note->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) &&
          SUCCEEDED(note->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)), "Cannot prepare compact base format");
    const auto choice = Format(factory, fonts, family, UIStyle::TEXT_BODY);
    Check(SUCCEEDED(SettingsText::ConfigureThemeChoice(choice.Get())), "Cannot configure one-line theme labels");
    Check(choice->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP &&
          choice->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
        "Theme label format must prevent wrapping and vertically center its line box");
    SettingsText::Paragraph setup, details;
    float previous_setup_height = (std::numeric_limits<float>::max)();
    for (float card_width : {272.0f, 300.0f, 332.0f, 440.0f}) {
        const float text_width = card_width - 28;
        Check(SUCCEEDED(SettingsText::CreateParagraph(factory, note.Get(), WarningsSettingsText::sensor_setup,
            text_width, setup)), "Cannot create wrapped sensor setup instructions");
        Check(SUCCEEDED(SettingsText::CreateParagraph(factory, note.Get(), WarningsSettingsText::alert_details,
            text_width, details)), "Cannot create wrapped warning explanation");
        FullParagraph(setup, WarningsSettingsText::sensor_setup, text_width);
        FullParagraph(details, WarningsSettingsText::alert_details, text_width);
        Check(setup.height <= previous_setup_height + 0.05f, "Widening the card must remeasure rather than retain a stale height");
        previous_setup_height = setup.height;
        Check(note->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP &&
              note->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            "Wrapping a note must not mutate the shared dashboard format");
        const auto page = GetWarningsSettingsLayout(0, card_width, 10, 0, setup.height, details.height);
        Check(page.sensors_text_top + setup.height <= page.sensors_bot - 14 + 0.05f &&
              page.info_text_top + details.height <= page.info_bot - 14 + 0.05f,
            "Measured notes must have bottom padding inside their respective cards");
        const auto picker = ThemePickerLayout::Get(0, card_width, 100);
        for (int index = 0; index < ThemePickerLayout::option_count; ++index) {
            const auto& text = picker.options[static_cast<size_t>(index)].text;
            SingleLine(factory, choice.Get(), ThemePickerLayout::labels[index], text.right - text.left, text.bottom - text.top);
        }
        SingleLine(factory, note.Get(), ThemePickerLayout::auto_note,
            picker.note.right - picker.note.left, picker.note.bottom - picker.note.top);
    }
    // Also prove that copying an already-mutated shared format cannot reintroduce
    // centered/right-aligned or bottom-aligned warning paragraphs.
    note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    note->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    Check(SUCCEEDED(SettingsText::CreateParagraph(factory, note.Get(), WarningsSettingsText::sensor_setup, 244, setup)),
        "Cannot create a paragraph from a non-default base alignment");
    FullParagraph(setup, WarningsSettingsText::sensor_setup, 244);
    Check(note->GetTextAlignment() == DWRITE_TEXT_ALIGNMENT_TRAILING &&
          note->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_FAR, "Base-format alignment must be left unchanged");
    for (float invalid_width : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        Check(FAILED(SettingsText::CreateParagraph(factory, note.Get(), L"Text", invalid_width, setup)) &&
            !setup.layout && setup.height == 0, "Invalid dimensions must fail without retaining stale text");
    }
    std::wcout << L"Measured wrapped notes and theme choices: " << family << L'\n';
}
void WelcomeCases(IDWriteFactory* factory, IDWriteFontCollection* fonts, const wchar_t* family) {
    using namespace WatchmanWelcome;
    std::array<ComPtr<IDWriteTextFormat>, text_count> formats = {
        Format(factory, fonts, family, title_font_size, DWRITE_FONT_WEIGHT_BOLD),
        Format(factory, fonts, family, 14, DWRITE_FONT_WEIGHT_NORMAL),
        Format(factory, fonts, family, 13, DWRITE_FONT_WEIGHT_BOLD),
        Format(factory, fonts, family, 14, DWRITE_FONT_WEIGHT_NORMAL),
        Format(factory, fonts, family, UIStyle::TEXT_SMALL)
    };
    const auto button = Format(factory, fonts, family, button_font_size);
    Check(SUCCEEDED(SettingsText::ConfigureThemeChoice(button.Get())), "Cannot configure welcome buttons");
    button->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    const auto counter = Format(factory, fonts, family, UIStyle::TEXT_SMALL);
    Check(SUCCEEDED(SettingsText::ConfigureThemeChoice(counter.Get())), "Cannot configure welcome counter");
    struct Size { float width, height; };
    for (const auto size : {Size{320, 400}, {400, 460}, {400, 550}, {460, 650}, {560, 820}, {720, 728}}) {
        const auto layout = GetLayout(size.width, size.height);
        for (int page = 0; page < page_count; ++page) {
            std::array<float, text_count> heights{};
            for (int index = 0; index < text_count; ++index) {
                SettingsText::Paragraph paragraph;
                const auto text = pages[page].text[static_cast<size_t>(index)];
                Check(SUCCEEDED(SettingsText::CreateParagraph(factory, formats[static_cast<size_t>(index)].Get(),
                    text, TextWidth(layout, index), paragraph)), "Cannot lay out welcome copy");
                FullParagraph(paragraph, text, TextWidth(layout, index), false);
                heights[static_cast<size_t>(index)] = paragraph.height;
            }
            const auto content = GetContent(layout, heights, 100000);
            Check(content.text[Footnote].bottom <= layout.body.bottom + 0.05f,
                "The complete welcome, including the floating right-click and administrator instructions, must remain reachable");
            if (size.width >= UIStyle::MIN_WINDOW_WIDTH && size.height >= UIStyle::MIN_WINDOW_HEIGHT) {
                Check(content.scroll_max == 0, "The simple welcome must fit without scrolling at normal supported window sizes");
            }
        }
        for (const auto* label : {L"Next", L"Get started"}) SingleLine(factory, button.Get(), label, Width(layout.next), Height(layout.next));
        SingleLine(factory, button.Get(), L"Back", Width(layout.back), Height(layout.back));
        SingleLine(factory, button.Get(), L"Skip", Width(layout.skip), Height(layout.skip));
        SingleLine(factory, counter.Get(), L"Quick start  /  3 of 3", Width(layout.step), Height(layout.step));
    }
    std::wcout << L"Measured all welcome screens, navigation and compact-window scrolling: " << family << L'\n';
}

void SpeedDialogCases(IDWriteFactory* factory, IDWriteFontCollection* fonts, const wchar_t* family) {
    const auto compact = Format(factory, fonts, family, UIStyle::TEXT_SMALL, DWRITE_FONT_WEIGHT_MEDIUM);
    compact->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); // Actual app-row base format.
    for (float width : {460.0f, 560.0f, 720.0f}) {
        for (float height : {650.0f, 720.0f, 820.0f}) {
            const auto dialog = SpeedTestPresentation::GetDialog(width, height);
            const float body_width = dialog.body.right - dialog.body.left;
            const float body_height = dialog.body.bottom - dialog.body.top;
            SettingsText::Paragraph disclosure;
            Check(SUCCEEDED(SettingsText::CreateParagraph(factory, compact.Get(),
                SpeedTestPresentation::consent_text, body_width, disclosure)), "Cannot lay out speed-test privacy disclosure");
            FullParagraph(disclosure, SpeedTestPresentation::consent_text, body_width);
            Check(disclosure.height <= body_height,
                "Entire disclosure, including public IP/data retention and consent, must be visible at supported sizes");
            Check(compact->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP,
                "Wrapping the speed dialog must not alter compact app-row text");

            std::wstring details;
            for (int index = 0; index < 20; ++index) details += L"Measurement notes: connection failed after a partial transfer. Try again.\n\n";
            SettingsText::Paragraph long_details;
            Check(SUCCEEDED(SettingsText::CreateParagraph(factory, compact.Get(), details.c_str(), body_width, long_details)),
                "Cannot lay out long measurement details");
            FullParagraph(long_details, details.c_str(), body_width);
            const float viewport = dialog.cancel.top - 18.0f - dialog.body.top;
            const float scroll_max = (std::max)(0.0f, long_details.height - viewport);
            Check(scroll_max > 0 && long_details.height - scroll_max <= viewport + 0.05f,
                "The final line of long measurement details must remain reachable by scrolling");
        }
    }
    std::wcout << L"Full speed-test disclosure and wrapped/scrollable measurement details: " << family << L'\n';
}

void MonitorPreviewTextFits(IDWriteFactory* factory, IDWriteFontCollection* fonts) {
    using namespace WatchmanWelcome;
    const auto mono = Format(factory, fonts, UIStyle::FONT_MONO, 11, DWRITE_FONT_WEIGHT_BOLD);
    const auto caption = Format(factory, fonts, UIStyle::FONT_MAIN, UIStyle::TEXT_SMALL);
    const auto floating_font = Format(factory, nullptr, L"Segoe UI", 12, DWRITE_FONT_WEIGHT_BOLD);
    for (auto* format : {mono.Get(), caption.Get(), floating_font.Get()}) {
        Check(SUCCEEDED(SettingsText::ConfigureThemeChoice(format)), "Cannot configure monitor sample text");
    }
    const auto taskbar_size = TaskbarWidgetLayout::PreferredSize(TaskbarWidgetMode::All, false, 36, 96);
    const auto taskbar = TaskbarWidgetLayout::Get(taskbar_size.width, taskbar_size.height, TaskbarWidgetMode::All, false, 96);
    const auto fit = [&](const wchar_t* text, const auto& rect, IDWriteTextFormat* format) {
        SingleLine(factory, format, text, static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    };
    fit(L"\u2191", taskbar.upload_arrow, mono.Get());
    fit(L"\u2193", taskbar.download_arrow, mono.Get());
    auto up = taskbar.upload_value;
    up.right = taskbar.upload_unit.right;
    auto down = taskbar.download_value;
    down.right = taskbar.download_unit.right;
    fit(sample_upload, up, mono.Get());
    fit(sample_download, down, mono.Get());
    fit(L"CPU", taskbar.cpu_label, mono.Get());
    fit(sample_cpu, taskbar.cpu_value, mono.Get());
    fit(L"MEM", taskbar.memory_label, mono.Get());
    fit(sample_memory, taskbar.memory_value, mono.Get());
    const auto floating = FloatingMonitorLayout::Get(FloatingMonitorLayout::Width(12), FloatingMonitorLayout::Height());
    fit(L"UP", floating.upload_label, floating_font.Get());
    fit(sample_upload, floating.upload_value, floating_font.Get());
    fit(L"DN", floating.download_label, floating_font.Get());
    fit(sample_download, floating.download_value, floating_font.Get());
    SingleLine(factory, caption.Get(), taskbar_caption, static_cast<float>(taskbar_size.width), 18);
    SingleLine(factory, caption.Get(), floating_caption, static_cast<float>(FloatingMonitorLayout::Width(12)), 18);
    std::cout << "Real welcome monitor samples, labels and numbers fit their actual widget geometry\n";
}
} // namespace

int main() {
    try {
        ComPtr<IDWriteFactory> factory;
        Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "Cannot create DirectWrite factory");
        WatchmanFonts::Collection bundled;
        Check(SUCCEEDED(bundled.Initialize(factory.Get())), "Cannot load the production embedded font collection");
        FontCases(factory.Get(), bundled.Get(), UIStyle::FONT_MAIN);
        WelcomeCases(factory.Get(), bundled.Get(), UIStyle::FONT_MAIN);
        SpeedDialogCases(factory.Get(), bundled.Get(), UIStyle::FONT_MAIN);
        MonitorPreviewTextFits(factory.Get(), bundled.Get());
        ComPtr<IDWriteFontCollection> system;
        Check(SUCCEEDED(factory->GetSystemFontCollection(system.GetAddressOf())),
            "Cannot enumerate fallback fonts");
        UINT32 index = 0;
        BOOL exists = FALSE;
        Check(SUCCEEDED(system->FindFamilyName(UIStyle::FONT_MAIN_FALLBACK, &index, &exists)) && exists,
            "Segoe UI fallback must be available for real font measurement");
        FontCases(factory.Get(), system.Get(), UIStyle::FONT_MAIN_FALLBACK);
        WelcomeCases(factory.Get(), system.Get(), UIStyle::FONT_MAIN_FALLBACK);
        SpeedDialogCases(factory.Get(), system.Get(), UIStyle::FONT_MAIN_FALLBACK);
        std::cout << "Settings text layout tests passed (DirectWrite measurements, not screenshot validation)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
