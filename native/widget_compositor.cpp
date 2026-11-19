#include "widget_compositor.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>


#include "ui_style.h"
#include "taskbar_widget_layout.h"

namespace {

template <typename T>
void SafeRelease(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

D2D1_COLOR_F ToD2DColor(COLORREF color, float alpha = 1.0f) {
    return D2D1::ColorF(
        static_cast<float>(GetRValue(color)) / 255.0f,
        static_cast<float>(GetGValue(color)) / 255.0f,
        static_cast<float>(GetBValue(color)) / 255.0f,
        alpha
    );
}

bool FramesMatch(const TaskbarWidgetFrame& left, const TaskbarWidgetFrame& right) {
    return left.mode == right.mode &&
           left.light_taskbar == right.light_taskbar &&
           left.vertical == right.vertical && left.taskbar_edge == right.taskbar_edge && left.dpi == right.dpi &&
           left.upload_text == right.upload_text &&
           left.download_text == right.download_text &&
           left.cpu_text == right.cpu_text &&
           left.memory_text == right.memory_text &&
           left.text_color == right.text_color &&
           left.upload_color == right.upload_color &&
           left.download_color == right.download_color &&
           left.cpu_color == right.cpu_color &&
           left.memory_color == right.memory_color &&
           left.font_family == right.font_family;
}

}  // namespace

TaskbarWidgetCompositor::~TaskbarWidgetCompositor() {
    Reset();
}

void TaskbarWidgetCompositor::ReleaseRenderTarget() {
    if (d2d_context_) {
        d2d_context_->SetTarget(nullptr);
    }
    SafeRelease(target_bitmap_);

    if (composition_visual_) {
        composition_visual_->SetContent(nullptr);
    }
    SafeRelease(swap_chain_);
    width_ = 0;
    height_ = 0;
    has_last_frame_ = false;
}

void TaskbarWidgetCompositor::ReleaseBrushes() {
    SafeRelease(base_overlay_brush_);
    SafeRelease(guard_overlay_brush_);
    SafeRelease(shadow_brush_);
    SafeRelease(value_brush_);
    SafeRelease(upload_brush_);
    SafeRelease(download_brush_);
    SafeRelease(cpu_brush_);
    SafeRelease(memory_brush_);
    brushes_valid_ = false;
    brushes_text_color_ = CLR_INVALID;
    brushes_upload_color_ = CLR_INVALID;
    brushes_download_color_ = CLR_INVALID;
    brushes_cpu_color_ = CLR_INVALID;
    brushes_memory_color_ = CLR_INVALID;
}

void TaskbarWidgetCompositor::Reset() {
    ReleaseRenderTarget();
    ReleaseBrushes();

    if (composition_target_) {
        composition_target_->SetRoot(nullptr);
    }

    SafeRelease(composition_visual_);
    SafeRelease(composition_target_);
    SafeRelease(composition_device_);
    SafeRelease(text_format_);
    bundled_fonts_.Reset();
    SafeRelease(dwrite_factory_);
    SafeRelease(d2d_context_);
    SafeRelease(d2d_device_);
    SafeRelease(d2d_factory_);
    SafeRelease(dxgi_factory_);
    SafeRelease(dxgi_device_);
    SafeRelease(d3d_device_);

    text_format_family_.clear();
    text_format_size_ = 0.0f;
    hwnd_ = nullptr;
}

bool TaskbarWidgetCompositor::CreateDeviceResources(HWND hwnd) {
    hwnd_ = hwnd;

    D3D_FEATURE_LEVEL feature_level{};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3d_device_,
        &feature_level,
        nullptr
    );
    if (FAILED(hr)) {
        Reset();
        return false;
    }

    hr = d3d_device_->QueryInterface(IID_PPV_ARGS(&dxgi_device_));
    if (FAILED(hr)) {
        Reset();
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_device_->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) {
        hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory_));
    }
    SafeRelease(adapter);
    if (FAILED(hr)) {
        Reset();
        return false;
    }

    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        nullptr,
        reinterpret_cast<void**>(&d2d_factory_)
    );
    if (SUCCEEDED(hr)) {
        hr = d2d_factory_->CreateDevice(dxgi_device_, &d2d_device_);
    }
    if (SUCCEEDED(hr)) {
        hr = d2d_device_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &d2d_context_
        );
    }
    if (FAILED(hr)) {
        Reset();
        return false;
    }

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwrite_factory_)
    );
    if (FAILED(hr)) {
        Reset();
        return false;
    }

    if (FAILED(bundled_fonts_.Initialize(dwrite_factory_)) || !EnsureTextFormat(L"", 11.0f)) {
        Reset();
        return false;
    }

    hr = DCompositionCreateDevice(
        dxgi_device_,
        IID_PPV_ARGS(&composition_device_)
    );
    if (SUCCEEDED(hr)) {
        const BOOL topmost =
            (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        hr = composition_device_->CreateTargetForHwnd(
            hwnd,
            topmost,
            &composition_target_
        );
    }
    if (SUCCEEDED(hr)) {
        hr = composition_device_->CreateVisual(&composition_visual_);
    }
    if (FAILED(hr)) {
        Reset();
        return false;
    }

    d2d_context_->SetDpi(96.0f, 96.0f);
    d2d_context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

bool TaskbarWidgetCompositor::EnsureRenderTarget(UINT width, UINT height) {
    if (swap_chain_ && width_ == width && height_ == height) {
        return true;
    }

    ReleaseRenderTarget();

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = dxgi_factory_->CreateSwapChainForComposition(
        d3d_device_,
        &description,
        nullptr,
        &swap_chain_
    );
    if (FAILED(hr)) {
        return false;
    }

    hr = composition_visual_->SetContent(swap_chain_);
    if (SUCCEEDED(hr)) {
        hr = composition_target_->SetRoot(composition_visual_);
    }
    if (FAILED(hr)) {
        ReleaseRenderTarget();
        return false;
    }

    IDXGISurface* surface = nullptr;
    hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (SUCCEEDED(hr)) {
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED
            ),
            96.0f,
            96.0f
        );
        hr = d2d_context_->CreateBitmapFromDxgiSurface(
            surface,
            &properties,
            &target_bitmap_
        );
    }
    SafeRelease(surface);
    if (FAILED(hr)) {
        ReleaseRenderTarget();
        return false;
    }

    d2d_context_->SetTarget(target_bitmap_);
    width_ = width;
    height_ = height;
    return SUCCEEDED(composition_device_->Commit());
}

bool TaskbarWidgetCompositor::EnsureTextFormat(const std::wstring& font_family, float font_size) {
    const std::wstring requested = font_family.empty() ? std::wstring(UIStyle::FONT_MONO) : font_family;
    if (text_format_ && text_format_size_ == font_size && _wcsicmp(text_format_family_.c_str(), requested.c_str()) == 0) {
        return true;
    }

    SafeRelease(text_format_);
    const HRESULT hr = bundled_fonts_.CreateTextFormat(requested.c_str(), DWRITE_FONT_WEIGHT_BOLD,
        font_size, &text_format_);
    if (FAILED(hr)) return false;
    text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_format_family_ = requested;
    text_format_size_ = font_size;
    return true;
}

bool TaskbarWidgetCompositor::EnsureBrushes(const TaskbarWidgetFrame& frame) {
    if (brushes_valid_ && brushes_light_taskbar_ == frame.light_taskbar &&
        brushes_text_color_ == frame.text_color && brushes_upload_color_ == frame.upload_color &&
        brushes_download_color_ == frame.download_color && brushes_cpu_color_ == frame.cpu_color &&
        brushes_memory_color_ == frame.memory_color) {
        return true;
    }

    ReleaseBrushes();

    const COLORREF shadow = frame.light_taskbar ? RGB(255, 255, 255) : RGB(0, 0, 0);
    const COLORREF value = frame.text_color == CLR_INVALID
        ? (frame.light_taskbar ? RGB(38, 49, 58) : RGB(255, 255, 255)) : frame.text_color;
    const COLORREF upload = frame.upload_color == CLR_INVALID
        ? (frame.light_taskbar ? RGB(209, 63, 131) : RGB(240, 125, 172)) : frame.upload_color;
    const COLORREF download = frame.download_color == CLR_INVALID
        ? (frame.light_taskbar ? RGB(0, 120, 212) : RGB(99, 214, 255)) : frame.download_color;
    const COLORREF cpu = frame.cpu_color == CLR_INVALID
        ? (frame.light_taskbar ? RGB(164, 95, 0) : RGB(243, 182, 74)) : frame.cpu_color;
    const COLORREF memory = frame.memory_color == CLR_INVALID
        ? (frame.light_taskbar ? RGB(22, 122, 56) : RGB(134, 217, 108)) : frame.memory_color;

    HRESULT hr = d2d_context_->CreateSolidColorBrush(
        frame.light_taskbar ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.01f)
                      : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f),
        &base_overlay_brush_
    );
    if (SUCCEEDED(hr)) {
        hr = d2d_context_->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.001f),
            &guard_overlay_brush_
        );
    }
    if (SUCCEEDED(hr)) hr = d2d_context_->CreateSolidColorBrush(ToD2DColor(shadow, 0.90f), &shadow_brush_);
    if (SUCCEEDED(hr)) hr = d2d_context_->CreateSolidColorBrush(ToD2DColor(value), &value_brush_);
    if (SUCCEEDED(hr)) hr = d2d_context_->CreateSolidColorBrush(ToD2DColor(upload), &upload_brush_);
    if (SUCCEEDED(hr)) hr = d2d_context_->CreateSolidColorBrush(ToD2DColor(download), &download_brush_);
    if (SUCCEEDED(hr)) hr = d2d_context_->CreateSolidColorBrush(ToD2DColor(cpu), &cpu_brush_);
    if (SUCCEEDED(hr)) hr = d2d_context_->CreateSolidColorBrush(ToD2DColor(memory), &memory_brush_);

    if (FAILED(hr)) {
        ReleaseBrushes();
        return false;
    }

    brushes_light_taskbar_ = frame.light_taskbar;
    brushes_text_color_ = frame.text_color;
    brushes_upload_color_ = frame.upload_color;
    brushes_download_color_ = frame.download_color;
    brushes_cpu_color_ = frame.cpu_color;
    brushes_memory_color_ = frame.memory_color;
    brushes_valid_ = true;
    return true;
}

void TaskbarWidgetCompositor::DrawText(
    const std::wstring& text,
    const D2D1_RECT_F& rect,
    DWRITE_TEXT_ALIGNMENT alignment,
    ID2D1Brush* brush,
    ID2D1Brush* shadow_brush
) {
    if (text.empty() || !brush || !shadow_brush || !text_format_) {
        return;
    }

    text_format_->SetTextAlignment(alignment);
    D2D1_RECT_F shadow_rect = rect;
    shadow_rect.top += 1.0f;
    shadow_rect.bottom += 1.0f;

    d2d_context_->DrawText(
        text.c_str(),
        static_cast<UINT32>(text.length()),
        text_format_,
        shadow_rect,
        shadow_brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP
    );
    d2d_context_->DrawText(
        text.c_str(),
        static_cast<UINT32>(text.length()),
        text_format_,
        rect,
        brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP
    );
}

bool TaskbarWidgetCompositor::Render(HWND hwnd, const TaskbarWidgetFrame& frame) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));

    if (hwnd_ && hwnd_ != hwnd) {
        Reset();
    }
    if (!d3d_device_ && !CreateDeviceResources(hwnd)) {
        return false;
    }
    if (!EnsureRenderTarget(width, height)) {
        Reset();
        return false;
    }

    if (has_last_frame_ && FramesMatch(last_frame_, frame)) {
        return true;
    }
    const auto layout = TaskbarWidgetLayout::Get(static_cast<int>(width), static_cast<int>(height), frame.mode, frame.vertical, frame.dpi, frame.taskbar_edge);
    if (!EnsureTextFormat(frame.font_family, layout.font_size) || !EnsureBrushes(frame)) {
        Reset();
        return false;
    }

    d2d_context_->BeginDraw();
    const D2D1_COLOR_F transparent = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    d2d_context_->Clear(&transparent);

    const auto to_rect = [](const TaskbarWidgetLayout::Rect& rect) {
        return D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right), static_cast<float>(rect.bottom));
    };
    const auto measure = [&](const std::wstring& text) {
        IDWriteTextLayout* text_layout = nullptr;
        DWRITE_TEXT_METRICS metrics{};
        const HRESULT result = dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), text_format_, 100000.0f, 1000.0f, &text_layout);
        if (FAILED(result)) return 100000.0f;
        const HRESULT measured = text_layout->GetMetrics(&metrics);
        text_layout->Release();
        return SUCCEEDED(measured) ? metrics.widthIncludingTrailingWhitespace : 100000.0f;
    };
    const auto draw = [&](const std::wstring& text, const TaskbarWidgetLayout::Rect& rect, ID2D1Brush* brush, bool centered = false) {
        if (rect.right <= rect.left || rect.bottom <= rect.top) return;
        DrawText(text, to_rect(rect), centered ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING, brush, shadow_brush_);
    };
    const auto draw_rate = [&](const std::wstring& text, const wchar_t* arrow_text,
                               const TaskbarWidgetLayout::Rect& arrow, const TaskbarWidgetLayout::Rect& value,
                               const TaskbarWidgetLayout::Rect& unit, ID2D1Brush* brush) {
        auto combined = value;
        if (!layout.split) combined.right = unit.right;
        const auto fitted = TaskbarWidgetLayout::FitRate(text, layout.split, combined.right - combined.left, measure);
        draw(arrow_text, arrow, brush, frame.vertical);
        const auto alignment = TaskbarWidgetLayout::NetworkAlignment(frame.vertical, frame.taskbar_edge)
            == TaskbarWidgetLayout::TextAlignment::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
        DrawText(fitted.value, to_rect(fitted.unit.empty() ? combined : value), alignment, value_brush_, shadow_brush_);
        if (!fitted.unit.empty()) DrawText(fitted.unit, to_rect(unit), alignment, value_brush_, shadow_brush_);
    };
    const D2D1_RECT_F full_rect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    d2d_context_->FillRectangle(full_rect, base_overlay_brush_);
    d2d_context_->FillRectangle(full_rect, guard_overlay_brush_);

    if (layout.show_network) {
        draw_rate(frame.upload_text, L"\u2191", layout.upload_arrow, layout.upload_value, layout.upload_unit, upload_brush_);
        draw_rate(frame.download_text, L"\u2193", layout.download_arrow, layout.download_value, layout.download_unit, download_brush_);
    }
    if (layout.show_system) {
        const auto draw_system = [&](const wchar_t* label, const std::wstring& value,
                                     TaskbarWidgetLayout::Rect label_area, const TaskbarWidgetLayout::Rect& value_area, ID2D1Brush* brush) {
            if (frame.vertical && !layout.split) {
                const auto row = TaskbarWidgetLayout::CenterSystemRow(label_area, value_area,
                    static_cast<int>(std::ceil(measure(label))), static_cast<int>(std::ceil(measure(value))));
                draw(label, row.label, brush);
                draw(value, row.value, brush);
            } else {
                draw(label, label_area, brush, frame.vertical);
                draw(value, value_area, brush, frame.vertical);
            }
        };
        draw_system(L"CPU", frame.cpu_text, layout.cpu_label, layout.cpu_value, cpu_brush_);
        draw_system(L"MEM", frame.memory_text, layout.memory_label, layout.memory_value, memory_brush_);
    }

    HRESULT hr = d2d_context_->EndDraw();

    if (FAILED(hr)) {
        Reset();
        return false;
    }

    hr = swap_chain_->Present(1, 0);
    if (SUCCEEDED(hr)) {
        hr = composition_device_->Commit();
    }
    if (FAILED(hr)) {
        Reset();
        return false;
    }

    last_frame_ = frame;
    has_last_frame_ = true;
    return true;
}
