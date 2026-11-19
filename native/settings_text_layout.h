#pragma once

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace SettingsText {
struct Paragraph {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    float height = 0.0f;

    void Reset() { layout.Reset(); height = 0.0f; }
};

// A text layout owns its wrapping/alignment. Never mutate the shared compact
// dashboard format: gauges, units and one-line notes still depend on it.
inline HRESULT CreateParagraph(IDWriteFactory* factory, IDWriteTextFormat* base_format,
                               const wchar_t* text, float width, Paragraph& paragraph) {
    paragraph.Reset();
    if (!factory || !base_format || !text || !std::isfinite(width) || width <= 0) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = factory->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)), base_format,
        width, 16384.0f, layout.GetAddressOf());
    if (SUCCEEDED(hr)) hr = layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (SUCCEEDED(hr)) hr = layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (SUCCEEDED(hr)) hr = layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(hr)) hr = layout->GetMetrics(&metrics);
    if (FAILED(hr)) return hr;
    // Round up with a small glyph guard; the card will add its own padding.
    const float height = (std::max)(1.0f, std::ceil(metrics.height) + 2.0f);
    hr = layout->SetMaxHeight(height);
    if (SUCCEEDED(hr)) {
        paragraph.layout = std::move(layout);
        paragraph.height = height;
    }
    return hr;
}

inline HRESULT ConfigureThemeChoice(IDWriteTextFormat* format) {
    if (!format) return E_INVALIDARG;
    HRESULT hr = format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (SUCCEEDED(hr)) hr = format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (SUCCEEDED(hr)) hr = format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return hr;
}
} // namespace SettingsText
