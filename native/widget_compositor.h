#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <string>
#include "bundled_fonts.h"
#include "taskbar_widget_mode.h"

struct TaskbarWidgetFrame {
    TaskbarWidgetMode mode = TaskbarWidgetMode::All;
    bool light_taskbar = false;
    bool vertical = false;
    int taskbar_edge = 0;
    unsigned dpi = 96;
    std::wstring upload_text;
    std::wstring download_text;
    std::wstring cpu_text;
    std::wstring memory_text;
    COLORREF text_color = CLR_INVALID;
    COLORREF upload_color = CLR_INVALID;
    COLORREF download_color = CLR_INVALID;
    COLORREF cpu_color = CLR_INVALID;
    COLORREF memory_color = CLR_INVALID;
    std::wstring font_family;
};

// Owns the widget's GPU surface separately from the main-window renderer.
// DirectComposition avoids rebuilding a layered GDI window for each metrics refresh.
class TaskbarWidgetCompositor {
public:
    TaskbarWidgetCompositor() = default;
    ~TaskbarWidgetCompositor();

    TaskbarWidgetCompositor(const TaskbarWidgetCompositor&) = delete;
    TaskbarWidgetCompositor& operator=(const TaskbarWidgetCompositor&) = delete;

    bool Render(HWND hwnd, const TaskbarWidgetFrame& frame);
    void Reset();

private:
    bool CreateDeviceResources(HWND hwnd);
    bool EnsureRenderTarget(UINT width, UINT height);
    bool EnsureTextFormat(const std::wstring& font_family, float font_size);
    bool EnsureBrushes(const TaskbarWidgetFrame& frame);
    void ReleaseRenderTarget();
    void ReleaseBrushes();
    void DrawText(
        const std::wstring& text,
        const D2D1_RECT_F& rect,
        DWRITE_TEXT_ALIGNMENT alignment,
        ID2D1Brush* brush,
        ID2D1Brush* shadow_brush
    );

    HWND hwnd_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;

    ID3D11Device* d3d_device_ = nullptr;
    IDXGIDevice* dxgi_device_ = nullptr;
    IDXGIFactory2* dxgi_factory_ = nullptr;
    ID2D1Factory1* d2d_factory_ = nullptr;
    ID2D1Device* d2d_device_ = nullptr;
    ID2D1DeviceContext* d2d_context_ = nullptr;
    IDXGISwapChain1* swap_chain_ = nullptr;
    IDCompositionDevice* composition_device_ = nullptr;
    IDCompositionTarget* composition_target_ = nullptr;
    IDCompositionVisual* composition_visual_ = nullptr;
    ID2D1Bitmap1* target_bitmap_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    WatchmanFonts::Collection bundled_fonts_;
    IDWriteTextFormat* text_format_ = nullptr;

    ID2D1SolidColorBrush* base_overlay_brush_ = nullptr;
    ID2D1SolidColorBrush* guard_overlay_brush_ = nullptr;
    ID2D1SolidColorBrush* shadow_brush_ = nullptr;
    ID2D1SolidColorBrush* value_brush_ = nullptr;
    ID2D1SolidColorBrush* upload_brush_ = nullptr;
    ID2D1SolidColorBrush* download_brush_ = nullptr;
    ID2D1SolidColorBrush* cpu_brush_ = nullptr;
    ID2D1SolidColorBrush* memory_brush_ = nullptr;
    bool brushes_valid_ = false;
    bool brushes_light_taskbar_ = false;
    COLORREF brushes_text_color_ = CLR_INVALID;
    COLORREF brushes_upload_color_ = CLR_INVALID;
    COLORREF brushes_download_color_ = CLR_INVALID;
    COLORREF brushes_cpu_color_ = CLR_INVALID;
    COLORREF brushes_memory_color_ = CLR_INVALID;
    std::wstring text_format_family_;
    float text_format_size_ = 0.0f;

    TaskbarWidgetFrame last_frame_;
    bool has_last_frame_ = false;
};
