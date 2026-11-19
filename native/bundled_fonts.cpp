#include "bundled_fonts.h"

#include <dwrite_3.h>
#include <wrl/implements.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace WatchmanFonts {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

HRESULT ReadResource(HMODULE module, UINT id, const void** bytes, DWORD* size) {
    *bytes = nullptr;
    *size = 0;
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!resource) return HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
    const HGLOBAL loaded = LoadResource(module, resource);
    *size = SizeofResource(module, resource);
    *bytes = loaded ? LockResource(loaded) : nullptr;
    return *bytes && *size ? S_OK : HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND);
}

// Resource bytes live for the entire executable lifetime. DirectWrite can ask
// for fragments on its own threads, without disk IO or a temporary-file race.
class ResourceStream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IDWriteFontFileStream> {
public:
    ResourceStream(const void* bytes, DWORD size) : bytes_(static_cast<const std::uint8_t*>(bytes)), size_(size) {}

    IFACEMETHODIMP ReadFileFragment(const void** fragment, UINT64 offset, UINT64 length, void** context) override {
        if (!fragment || !context) return E_POINTER;
        *fragment = nullptr;
        *context = nullptr;
        if (offset > size_ || length > size_ - offset) return E_INVALIDARG;
        *fragment = bytes_ + static_cast<size_t>(offset);
        return S_OK;
    }
    IFACEMETHODIMP_(void) ReleaseFileFragment(void*) override {}
    IFACEMETHODIMP GetFileSize(UINT64* size) override {
        if (!size) return E_POINTER;
        *size = size_;
        return S_OK;
    }
    IFACEMETHODIMP GetLastWriteTime(UINT64* time) override {
        if (!time) return E_POINTER;
        *time = 0; // Immutable embedded resource; no file timestamp is involved.
        return S_OK;
    }
private:
    const std::uint8_t* bytes_;
    UINT64 size_;
};

class ResourceLoader final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IDWriteFontFileLoader> {
public:
    explicit ResourceLoader(HMODULE module) : module_(module) {}
    IFACEMETHODIMP CreateStreamFromKey(const void* key, UINT32 key_size, IDWriteFontFileStream** stream) override {
        if (!stream) return E_POINTER;
        *stream = nullptr;
        if (!key || key_size != sizeof(UINT)) return E_INVALIDARG;
        UINT id = 0;
        std::memcpy(&id, key, sizeof(id));
        const void* bytes = nullptr;
        DWORD size = 0;
        const HRESULT hr = ReadResource(module_, id, &bytes, &size);
        if (FAILED(hr)) return hr;
        auto result = Make<ResourceStream>(bytes, size);
        if (!result) return E_OUTOFMEMORY;
        return result.CopyTo(stream);
    }
private:
    HMODULE module_;
};
} // namespace

Collection::~Collection() { Reset(); }

void Collection::Reset() {
    collection_.Reset();
    if (registered_) factory_->UnregisterFontFileLoader(loader_.Get());
    registered_ = false;
    loader_.Reset();
    factory_.Reset();
}

HRESULT Collection::Initialize(IDWriteFactory* factory, HMODULE module) {
    Reset();
    if (!factory) return E_INVALIDARG;
    factory_ = factory;
    const HMODULE resource_module = module ? module : GetModuleHandleW(nullptr);
    const auto build = [&]() -> HRESULT {
        // Validate resources before handing a custom file reference to DirectWrite.
        // A missing/empty resource must fail here, not enter its lazy font parser.
        for (const UINT id : resource_ids) {
            const void* bytes = nullptr;
            DWORD size = 0;
            const HRESULT present = ReadResource(resource_module, id, &bytes, &size);
            if (FAILED(present)) return present;
        }
        ComPtr<IDWriteFactory3> factory3;
        HRESULT hr = factory_->QueryInterface(IID_PPV_ARGS(factory3.GetAddressOf()));
        if (FAILED(hr)) return hr;
        loader_ = Make<ResourceLoader>(resource_module);
        if (!loader_) return E_OUTOFMEMORY;
        hr = factory_->RegisterFontFileLoader(loader_.Get());
        if (FAILED(hr)) return hr;
        registered_ = true;
        ComPtr<IDWriteFontSetBuilder> builder;
        hr = factory3->CreateFontSetBuilder(builder.GetAddressOf());
        if (FAILED(hr)) return hr;
        for (const UINT id : resource_ids) {
            ComPtr<IDWriteFontFile> file;
            hr = factory_->CreateCustomFontFileReference(&id, sizeof(id), loader_.Get(), file.GetAddressOf());
            if (FAILED(hr)) return hr;
            ComPtr<IDWriteFontFaceReference> face;
            hr = factory3->CreateFontFaceReference(file.Get(), 0, DWRITE_FONT_SIMULATIONS_NONE, face.GetAddressOf());
            if (FAILED(hr)) return hr;
            hr = builder->AddFontFaceReference(face.Get());
            if (FAILED(hr)) return hr;
        }
        ComPtr<IDWriteFontSet> set;
        hr = builder->CreateFontSet(set.GetAddressOf());
        if (FAILED(hr)) return hr;
        ComPtr<IDWriteFontCollection1> collection;
        hr = factory3->CreateFontCollectionFromFontSet(set.Get(), collection.GetAddressOf());
        if (FAILED(hr)) return hr;
        collection_ = collection;
        return S_OK;
    };
    const HRESULT hr = build();
    if (FAILED(hr)) Reset();
    return hr;
}

HRESULT Collection::CreateTextFormat(const wchar_t* family, DWRITE_FONT_WEIGHT weight,
                                     float size, IDWriteTextFormat** format) const {
    if (!format) return E_POINTER;
    *format = nullptr;
    if (!factory_ || !collection_) return E_UNEXPECTED;
    if (!family || !*family || !std::isfinite(size) || size <= 0) return E_INVALIDARG;
    UINT32 index = 0;
    BOOL exists = FALSE;
    HRESULT hr = collection_->FindFamilyName(family, &index, &exists);
    if (FAILED(hr)) return hr;
    if (!exists) return DWRITE_E_NOFONT;
    ComPtr<IDWriteFontFamily> font_family;
    hr = collection_->GetFontFamily(index, font_family.GetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<IDWriteFont> font;
    hr = font_family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf());
    if (FAILED(hr)) return hr;
    if (font->GetWeight() != weight || font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE)
        return DWRITE_E_NOFONT;
    return factory_->CreateTextFormat(family, collection_.Get(), weight,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", format);
}

GdiRegistration::~GdiRegistration() { Reset(); }
void GdiRegistration::Reset() {
    for (auto& handle : handles_) {
        if (handle) RemoveFontMemResourceEx(handle);
        handle = nullptr;
    }
}
HRESULT GdiRegistration::Initialize(HMODULE module) {
    Reset();
    if (!module) module = GetModuleHandleW(nullptr);
    for (size_t index = 0; index < resource_ids.size(); ++index) {
        const void* bytes = nullptr;
        DWORD size = 0;
        HRESULT hr = ReadResource(module, resource_ids[index], &bytes, &size);
        if (SUCCEEDED(hr)) {
            DWORD count = 0;
            handles_[index] = AddFontMemResourceEx(const_cast<void*>(bytes), size, nullptr, &count);
            if (!handles_[index] || count == 0) hr = E_FAIL;
        }
        if (FAILED(hr)) { Reset(); return hr; }
    }
    return S_OK;
}
} // namespace WatchmanFonts
