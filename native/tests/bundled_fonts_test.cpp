#include "bundled_fonts.h"
#include "ui_style.h"

#include <wrl/implements.h>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// Preserve a precise phase even when a native font API raises an SEH exception.
// This is test-only: no crash hooks or diagnostic windows are added to Watchman.
std::atomic<const char*> phase{"start"};
std::atomic<UINT> active_resource{0};
void Phase(const char* text, bool announce = false) {
    phase.store(text, std::memory_order_relaxed);
    if (announce) std::cerr << "Font check: " << text << " (resource "
        << active_resource.load(std::memory_order_relaxed) << ")" << std::endl;
}
LONG WINAPI ReportNativeFailure(EXCEPTION_POINTERS* exception) {
    char message[512]{};
    std::snprintf(message, sizeof(message),
        "Font check crash: phase=%s; resource=%u; exception=0x%08lX; address=%p\n",
        phase.load(std::memory_order_relaxed), active_resource.load(std::memory_order_relaxed),
        exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

struct FaceSource {
    ComPtr<IDWriteFontFileLoader> loader;
    UINT id = 0;
};
HRESULT ReadFaceSource(IDWriteFontFace* face, FaceSource& result) {
    if (!face || face->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) return E_FAIL;
    UINT32 count = 0;
    HRESULT hr = face->GetFiles(&count, nullptr);
    if (FAILED(hr) || count != 1) return E_FAIL;
    ComPtr<IDWriteFontFile> file;
    hr = face->GetFiles(&count, file.GetAddressOf());
    if (FAILED(hr)) return hr;
    const void* key = nullptr;
    UINT32 size = 0;
    hr = file->GetReferenceKey(&key, &size);
    if (FAILED(hr) || !key || size != sizeof(UINT)) return E_FAIL;
    std::memcpy(&result.id, key, sizeof(result.id));
    return file->GetLoader(result.loader.GetAddressOf());
}

// Inspect the font faces actually delivered to the text renderer, not just the
// requested family name. A silent system substitution would fail these checks,
// even on a developer PC that has another copy of the same family installed.
class GlyphInspector final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDWriteTextRenderer> {
public:
    explicit GlyphInspector(FaceSource expected) : expected_(std::move(expected)) {}
    unsigned runs = 0;
    IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = TRUE; return S_OK; }
    IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override {
        *matrix = {1, 0, 0, 1, 0, 0}; return S_OK;
    }
    IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* pixels) override { *pixels = 1; return S_OK; }
    IFACEMETHODIMP DrawGlyphRun(void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* glyphs, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        if (!glyphs || glyphs->glyphCount == 0) return E_FAIL;
        FaceSource actual;
        const HRESULT hr = ReadFaceSource(glyphs->fontFace, actual);
        if (FAILED(hr) || actual.id != expected_.id || actual.loader.Get() != expected_.loader.Get()) return E_FAIL;
        for (UINT32 index = 0; index < glyphs->glyphCount; ++index) {
            if (glyphs->glyphIndices[index] == 0) return E_FAIL;
        }
        ++runs;
        return S_OK;
    }
    IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return E_NOTIMPL; }
    IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return E_NOTIMPL; }
    IFACEMETHODIMP DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return E_NOTIMPL; }
private:
    FaceSource expected_;
};

std::string FamilyNames(IDWriteFontCollection* fonts) {
    std::string result;
    for (UINT32 index = 0; index < fonts->GetFontFamilyCount(); ++index) {
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> names;
        Check(SUCCEEDED(fonts->GetFontFamily(index, family.GetAddressOf())) &&
            SUCCEEDED(family->GetFamilyNames(names.GetAddressOf())), "Cannot enumerate embedded family names");
        UINT32 length = 0;
        Check(SUCCEEDED(names->GetStringLength(0, &length)), "Cannot measure embedded family name");
        std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
        Check(SUCCEEDED(names->GetString(0, text.data(), length + 1)), "Cannot read embedded family name");
        if (!result.empty()) result += "; ";
        for (UINT32 position = 0; position < length; ++position) {
            result += text[position] <= 127 ? static_cast<char>(text[position]) : '?';
        }
    }
    return result;
}

FaceSource MatchingFace(WatchmanFonts::Collection& fonts, const wchar_t* name, DWRITE_FONT_WEIGHT weight, UINT resource_id) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(fonts.Get()->FindFamilyName(name, &index, &exists)) || !exists) {
        throw std::runtime_error("Embedded family missing for resource " + std::to_string(resource_id) +
            "; actual WWS families: " + FamilyNames(fonts.Get()));
    }
    ComPtr<IDWriteFontFamily> family;
    Check(SUCCEEDED(fonts.Get()->GetFontFamily(index, family.GetAddressOf())), "Cannot inspect embedded family");
    ComPtr<IDWriteFont> font;
    Check(SUCCEEDED(family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())), "Cannot select bundled font weight");
    Check(font->GetWeight() == weight && font->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE,
        "Must select the exact embedded weight, never a synthetic bold or variable ExtraLight default");
    ComPtr<IDWriteFontFace> face;
    Check(SUCCEEDED(font->CreateFontFace(face.GetAddressOf())), "Cannot create embedded font face");
    FaceSource source;
    Check(SUCCEEDED(ReadFaceSource(face.Get(), source)) && source.id == resource_id,
        "Font face must come from the expected executable RCDATA resource");
    return source;
}

void StreamBounds(const FaceSource& source) {
    ComPtr<IDWriteFontFileStream> stream;
    Check(SUCCEEDED(source.loader->CreateStreamFromKey(&source.id, sizeof(source.id), stream.GetAddressOf())),
        "Cannot open production resource font stream");
    UINT64 size = 0;
    Check(SUCCEEDED(stream->GetFileSize(&size)) && size > 0, "Embedded stream size must be nonzero");
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(source.id), MAKEINTRESOURCEW(10));
    Check(resource && size == SizeofResource(nullptr, resource), "Font stream must expose the exact embedded byte length");
    const void* fragment = nullptr;
    void* context = nullptr;
    Check(SUCCEEDED(stream->ReadFileFragment(&fragment, 0, size, &context)) && fragment,
        "Entire embedded stream must be readable");
    const auto memory = LoadResource(nullptr, resource);
    Check(memory && std::memcmp(fragment, LockResource(memory), static_cast<size_t>(size)) == 0,
        "Font stream bytes must match this executable, not a stale cache or another installed font");
    stream->ReleaseFileFragment(context);
    Check(FAILED(stream->ReadFileFragment(&fragment, size + 1, 0, &context)) && !fragment,
        "Out-of-bounds offset must fail without a dangling fragment");
    Check(FAILED(stream->ReadFileFragment(&fragment, 1, (std::numeric_limits<UINT64>::max)(), &context)) && !fragment,
        "Fragment-length overflow must be rejected");
    Check(FAILED(source.loader->CreateStreamFromKey(&source.id, sizeof(source.id) - 1, stream.ReleaseAndGetAddressOf())) && !stream,
        "Invalid resource keys must fail cleanly");
}

void FormatsAndActualGlyphs(IDWriteFactory* factory, WatchmanFonts::Collection& fonts) {
    struct Case { const wchar_t* family; DWRITE_FONT_WEIGHT weight; UINT id; };
    const Case cases[] = {
        {UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_NORMAL, IDR_FONT_INTER_REGULAR},
        {UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_MEDIUM, IDR_FONT_INTER_MEDIUM},
        {UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_SEMI_BOLD, IDR_FONT_INTER_SEMIBOLD},
        {UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_BOLD, IDR_FONT_INTER_BOLD},
        {UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_EXTRA_BOLD, IDR_FONT_INTER_EXTRABOLD},
        {UIStyle::FONT_VALUES, DWRITE_FONT_WEIGHT_EXTRA_BOLD, IDR_FONT_NUNITO_SANS},
        {UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_NORMAL, IDR_FONT_JETBRAINS_REGULAR},
        {UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_MEDIUM, IDR_FONT_JETBRAINS_MEDIUM},
        {UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_SEMI_BOLD, IDR_FONT_JETBRAINS_SEMIBOLD},
        {UIStyle::FONT_MONO, DWRITE_FONT_WEIGHT_BOLD, IDR_FONT_JETBRAINS_BOLD},
    };
    Check(fonts.Get()->GetFontFamilyCount() == 3, "Resource collection must contain exactly Watchman's three families");
    for (const auto& item : cases) {
        active_resource.store(item.id, std::memory_order_relaxed);
        Phase("matching embedded family and weight", true);
        const auto source = MatchingFace(fonts, item.family, item.weight, item.id);
        Phase("embedded stream bounds");
        StreamBounds(source);
        for (const float size : {11.0f, 12.0f, 18.0f, 22.0f, 26.0f}) {
            ComPtr<IDWriteTextFormat> format;
            Check(SUCCEEDED(fonts.CreateTextFormat(item.family, item.weight, size, format.GetAddressOf())),
                "Production font-format creation failed");
            ComPtr<IDWriteFontCollection> selected;
            Check(SUCCEEDED(format->GetFontCollection(selected.GetAddressOf())) && selected.Get() == fonts.Get(),
                "Text formats must use the explicit embedded collection, never nullptr/system fonts");
            std::wstring text = L"Watchman Settings 1.2 MB/s 8.6 MB/s 24% 58% 100% -- \u00b0C";
            if (std::wcscmp(item.family, UIStyle::FONT_MONO) == 0) text += L" \u2191 \u2193";
            ComPtr<IDWriteTextLayout> layout;
            Check(SUCCEEDED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                format.Get(), 2000, 200, layout.GetAddressOf())), "Cannot shape embedded-font sample");
            auto renderer = Microsoft::WRL::Make<GlyphInspector>(source);
            Phase("drawing actual resource glyphs");
            Check(renderer && SUCCEEDED(layout->Draw(nullptr, renderer.Get(), 0, 0)) && renderer->runs > 0,
                "Actual shaped glyphs silently substituted a font, weight, file, or missing glyph");
        }
    }
    Phase("rejecting unsupported families, weights and sizes", true);
    ComPtr<IDWriteTextFormat> invalid;
    Check(FAILED(fonts.CreateTextFormat(L"Missing Watchman family", DWRITE_FONT_WEIGHT_BOLD, 12, invalid.GetAddressOf())) && !invalid,
        "A missing family must be detected even though raw CreateTextFormat could succeed");
    Check(FAILED(fonts.CreateTextFormat(UIStyle::FONT_VALUES, DWRITE_FONT_WEIGHT_NORMAL, 12, invalid.GetAddressOf())) && !invalid,
        "An unbundled Nunito weight must not fall back to ExtraLight or synthetic bold");
    for (const float size : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        Check(FAILED(fonts.CreateTextFormat(UIStyle::FONT_MAIN, DWRITE_FONT_WEIGHT_NORMAL, size, invalid.GetAddressOf())) && !invalid,
            "Invalid text sizes must fail cleanly");
    }
}

void GdiFallbackUsesEmbeddedBytes() {
    Phase("GDI memory registration", true);
    WatchmanFonts::GdiRegistration fonts;
    Check(SUCCEEDED(fonts.Initialize()), "Production GDI memory registration failed");
    const HDC dc = CreateCompatibleDC(nullptr);
    Check(dc != nullptr, "Cannot create GDI measurement surface");
    const HFONT font = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, UIStyle::FONT_MONO);
    Check(font != nullptr, "Cannot create private taskbar font");
    const auto previous = SelectObject(dc, font);
    Check(previous && previous != HGDI_ERROR, "Cannot select private taskbar font");
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT_JETBRAINS_BOLD), MAKEINTRESOURCEW(10));
    const auto resource_size = SizeofResource(nullptr, resource);
    const auto resource_data = LockResource(LoadResource(nullptr, resource));
    Phase("reading exact GDI font bytes", true);
    const DWORD gdi_size = GetFontData(dc, 0, 0, nullptr, 0);
    std::vector<BYTE> bytes(gdi_size == GDI_ERROR ? 0 : gdi_size);
    const bool embedded = resource_data && gdi_size == resource_size && !bytes.empty() &&
        GetFontData(dc, 0, 0, bytes.data(), gdi_size) == gdi_size &&
        std::memcmp(bytes.data(), resource_data, resource_size) == 0;
    SelectObject(dc, previous);
    DeleteObject(font);
    DeleteDC(dc);
    Check(embedded, "GDI must select the exact embedded JetBrains Mono Bold bytes without installation or temp files");
    Phase("GDI unregister and re-register", true);
    fonts.Reset();
    Check(SUCCEEDED(fonts.Initialize()), "GDI private fonts must support cleanup and re-registration");
}

void LifecycleAndCleanPcPath() {
    for (const auto type : {DWRITE_FACTORY_TYPE_ISOLATED, DWRITE_FACTORY_TYPE_SHARED}) {
        Phase(type == DWRITE_FACTORY_TYPE_ISOLATED ? "isolated DirectWrite factory" : "shared DirectWrite factory", true);
        ComPtr<IDWriteFactory> factory;
        Check(SUCCEEDED(DWriteCreateFactory(type, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "Cannot create DirectWrite test factory");
        ComPtr<IDWriteFontCollection> system;
        Check(SUCCEEDED(factory->GetSystemFontCollection(system.GetAddressOf())), "Cannot enumerate test system fonts");
        for (const auto* family : {UIStyle::FONT_MAIN, UIStyle::FONT_VALUES, UIStyle::FONT_MONO}) {
            UINT32 index = 0;
            BOOL installed = FALSE;
            Check(SUCCEEDED(system->FindFamilyName(family, &index, &installed)), "System font lookup failed");
            std::wcout << family << L" installed system-wide: " << (installed ? L"yes" : L"no")
                << L"; test always requires EXE resource glyphs\n";
        }
        WatchmanFonts::Collection main, widget;
        Check(SUCCEEDED(main.Initialize(factory.Get())) && SUCCEEDED(widget.Initialize(factory.Get())),
            "Main and taskbar renderers must have independent live embedded collections");
        FormatsAndActualGlyphs(factory.Get(), main);
        Phase("taskbar collection reset with main still alive", true);
        widget.Reset();
        FormatsAndActualGlyphs(factory.Get(), main);
        Phase("taskbar collection recreation", true);
        Check(SUCCEEDED(widget.Initialize(factory.Get())), "Taskbar resource recreation must re-register its loader");
        FormatsAndActualGlyphs(factory.Get(), widget);
        Phase("main collection reset", true);
        main.Reset();
        Phase("missing-resource module must fail cleanly", true);
        Check(FAILED(main.Initialize(factory.Get(), GetModuleHandleW(L"kernel32.dll"))) && !main.Get(),
            "Missing resources must roll back loader registration without retaining a partial collection");
        Phase("main collection recovery", true);
        Check(SUCCEEDED(main.Initialize(factory.Get())), "Initialization must recover after missing resources");
        Phase("factory scope cleanup", true);
    }
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(ReportNativeFailure);
    try {
        LifecycleAndCleanPcPath();
        GdiFallbackUsesEmbeddedBytes();
        std::cout << "Embedded fonts: exact resource glyphs/weights, no installation/cache, two renderers, stream bounds and GDI fallback passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Font check failed: " << error.what() << '\n'; return 1; }
}
