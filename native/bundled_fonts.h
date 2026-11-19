#pragma once

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>

#include "resource.h"

// The EXE is the source of truth. Neither renderer uses the system collection
// for Watchman's own families, and the GDI fallback needs no writable font cache.
namespace WatchmanFonts {
inline constexpr std::array<UINT, 10> resource_ids{
    IDR_FONT_INTER_REGULAR, IDR_FONT_INTER_MEDIUM, IDR_FONT_INTER_SEMIBOLD,
    IDR_FONT_INTER_BOLD, IDR_FONT_INTER_EXTRABOLD, IDR_FONT_NUNITO_SANS,
    IDR_FONT_JETBRAINS_REGULAR, IDR_FONT_JETBRAINS_MEDIUM,
    IDR_FONT_JETBRAINS_SEMIBOLD, IDR_FONT_JETBRAINS_BOLD
};

class Collection {
public:
    Collection() = default;
    ~Collection();
    Collection(const Collection&) = delete;
    Collection& operator=(const Collection&) = delete;

    HRESULT Initialize(IDWriteFactory* factory, HMODULE module = nullptr);
    // Release all layouts/formats made from this collection before Reset.
    void Reset();
    IDWriteFontCollection* Get() const { return collection_.Get(); }

    // A missing family/weight is an error, not a successful implicit substitute.
    HRESULT CreateTextFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight,
                             float size, IDWriteTextFormat** format) const;

private:
    Microsoft::WRL::ComPtr<IDWriteFactory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFontFileLoader> loader_;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection_;
    bool registered_ = false;
};

class GdiRegistration {
public:
    GdiRegistration() = default;
    ~GdiRegistration();
    GdiRegistration(const GdiRegistration&) = delete;
    GdiRegistration& operator=(const GdiRegistration&) = delete;
    HRESULT Initialize(HMODULE module = nullptr);
    void Reset();

private:
    std::array<HANDLE, resource_ids.size()> handles_{};
};
} // namespace WatchmanFonts
