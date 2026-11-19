// Direct2D_Gradient_Demo.cpp
// Pixel-accurate C++ Direct2D recreation of the Watchman TSX Dashboard mockup.
// Uses the exact fonts: Inter, Nunito Sans, JetBrains Mono (falls back to Segoe UI if not installed).

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <cmath>

using Microsoft::WRL::ComPtr;

// ─── Brush & Font Collection ───
struct Demo {
    // Radial blob brushes
    ComPtr<ID2D1RadialGradientBrush> blobPink, blobYellow, blobOrange;
    // Linear gradient card brushes
    ComPtr<ID2D1LinearGradientBrush> gradSession, gradUsage, gradDial;
    // Solid brushes
    ComPtr<ID2D1SolidColorBrush> brBorder;     // light gray border (~rgba(0,0,0,0.08))
    ComPtr<ID2D1SolidColorBrush> brText;       // near-black text
    ComPtr<ID2D1SolidColorBrush> brTextSec;    // secondary gray text
    ComPtr<ID2D1SolidColorBrush> brBlue;       // accent blue
    ComPtr<ID2D1SolidColorBrush> brOrange;     // accent orange
    ComPtr<ID2D1SolidColorBrush> brGreen;      // status green
    ComPtr<ID2D1SolidColorBrush> brGreenText;  // green text for CONNECTED
    ComPtr<ID2D1SolidColorBrush> brWhite;
    ComPtr<ID2D1SolidColorBrush> brBlack;
    ComPtr<ID2D1SolidColorBrush> brDarkBorder; // for speed card borders (slightly darker)

    // Fonts
    ComPtr<IDWriteTextFormat> fTitle;       // Inter Bold 15 (WATCHMAN)
    ComPtr<IDWriteTextFormat> fTabs;        // Inter SemiBold 11 (tab labels)
    ComPtr<IDWriteTextFormat> fCardTitle;   // Inter ExtraBold 12 (CURRENT SESSION, USAGE SNAPSHOT)
    ComPtr<IDWriteTextFormat> fCardSub;     // Inter SemiBold 9 (subtitle text)
    ComPtr<IDWriteTextFormat> fStatLabel;   // Inter Bold 9 (DOWNLOAD/UPLOAD/TOTAL labels)
    ComPtr<IDWriteTextFormat> fSpeedNum;    // Nunito Sans ExtraBold 38 (speed numbers)
    ComPtr<IDWriteTextFormat> fSpeedLabel;  // Inter Bold 12 (DOWNLOAD/UPLOAD in speed card)
    ComPtr<IDWriteTextFormat> fStatNum;     // Nunito Sans Black 22 (stat numbers like 3.39 GB)
    ComPtr<IDWriteTextFormat> fUnit;        // Inter SemiBold 13 (KB/s)
    ComPtr<IDWriteTextFormat> fStatusTitle; // Inter ExtraBold 16 (NORMAL)
    ComPtr<IDWriteTextFormat> fStatusSub;   // Inter SemiBold 10 (status subtitle)
    ComPtr<IDWriteTextFormat> fDialPct;     // Nunito Sans ExtraBold 20 (dial %)
    ComPtr<IDWriteTextFormat> fDialPctSm;   // Inter Bold 11 (the % sign)
    ComPtr<IDWriteTextFormat> fDialLabel;   // Inter Bold 12 (CPU, RAM, GPU)
    ComPtr<IDWriteTextFormat> fDialSub;     // Inter SemiBold 9 (12 cores)
    ComPtr<IDWriteTextFormat> fFooter;      // Inter Bold 10 (CONNECTED)
    ComPtr<IDWriteTextFormat> fMono;        // JetBrains Mono Bold 10 (UPTIME)
    ComPtr<IDWriteTextFormat> fBtnLabel;    // Inter Bold 9 (RESET SESSION)
    ComPtr<IDWriteTextFormat> fArrow;       // Segoe UI Symbol 20 (arrow icons)
    ComPtr<IDWriteTextFormat> fFeedback;    // Inter SemiBold 11 (Feedback)
    ComPtr<IDWriteTextFormat> fIcons;       // Segoe UI Symbol 14 (header icons)
    ComPtr<IDWriteTextFormat> fTodayLabel;  // Inter SemiBold 10 (Today)
};

ComPtr<ID2D1Factory> g_factory;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<ID2D1HwndRenderTarget> g_rt;
Demo g;

// ─── Helper: try font with fallback ───
void MakeFont(const wchar_t* family, const wchar_t* fallback, DWRITE_FONT_WEIGHT weight, float size, ComPtr<IDWriteTextFormat>& out) {
    HRESULT hr = g_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
    if (FAILED(hr))
        g_dwrite->CreateTextFormat(fallback, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
}

// ─── Helper: draw text ───
void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, ID2D1Brush* b,
    DWRITE_TEXT_ALIGNMENT ha = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT va = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
    f->SetTextAlignment(ha);
    f->SetParagraphAlignment(va);
    g_rt->DrawText(s.c_str(), (UINT32)s.length(), f, r, b);
}

// ─── Helper: rounded rect fill+stroke ───
void Card(D2D1_RECT_F r, float radius, ID2D1Brush* fill, ID2D1Brush* stroke, float strokeW = 1.0f) {
    auto rr = D2D1::RoundedRect(r, radius, radius);
    if (fill) g_rt->FillRoundedRectangle(rr, fill);
    if (stroke) g_rt->DrawRoundedRectangle(rr, stroke, strokeW);
}

// ─── Helper: draw arc (partial circle for dial percentage) ───
void DrawDialArc(float cx, float cy, float radius, float pct, ID2D1Brush* brush, float width) {
    if (pct <= 0) return;
    float angle = pct / 100.0f * 360.0f;
    // For simplicity, draw full circle at low percentages, partial via geometry at higher
    // We'll use a path geometry approach
    ComPtr<ID2D1PathGeometry> path;
    g_factory->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);

    float startAngle = -90.0f; // top of circle
    float endAngle = startAngle + angle;
    float startRad = startAngle * 3.14159265f / 180.0f;
    float endRad = endAngle * 3.14159265f / 180.0f;

    D2D1_POINT_2F startPt = D2D1::Point2F(cx + radius * cosf(startRad), cy + radius * sinf(startRad));
    D2D1_POINT_2F endPt = D2D1::Point2F(cx + radius * cosf(endRad), cy + radius * sinf(endRad));

    sink->BeginFigure(startPt, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(endPt, D2D1::SizeF(radius, radius), 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE,
        angle > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    g_rt->DrawGeometry(path.Get(), brush, width);
}

// ─── Create Resources ───
HRESULT CreateRes(HWND hwnd) {
    if (g_rt) return S_OK;
    RECT rc; GetClientRect(hwnd, &rc);
    HRESULT hr = g_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
        &g_rt);
    if (FAILED(hr)) return hr;

    // ── Solid brushes ──
    // Border: very light gray, matching the subtle borders in the real app
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f), &g.brBorder);
    // Darker border for speed cards (slightly more visible)
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f), &g.brDarkBorder);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x111827), &g.brText);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x6b7280), &g.brTextSec);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x3b82f6), &g.brBlue);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0xf97316), &g.brOrange);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x10b981), &g.brGreen);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x059669), &g.brGreenText);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), &g.brWhite);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0x111827), &g.brBlack);

    // ── Gradient: Current Session card ──
    // linear-gradient(135deg, rgba(255,247,237,0.78), rgba(255,255,255,0.9))
    D2D1_GRADIENT_STOP gs1[2] = {
        { 0.0f, D2D1::ColorF(1.0f, 0.969f, 0.929f, 0.85f) },
        { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f) }
    };
    ComPtr<ID2D1GradientStopCollection> sc1;
    g_rt->CreateGradientStopCollection(gs1, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc1);
    g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0), D2D1::Point2F(0,0)), sc1.Get(), &g.gradSession);

    // ── Gradient: Usage Snapshot card ──
    D2D1_GRADIENT_STOP gs2[2] = {
        { 0.0f, D2D1::ColorF(1.0f, 0.969f, 0.929f, 0.88f) },
        { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f) }
    };
    ComPtr<ID2D1GradientStopCollection> sc2;
    g_rt->CreateGradientStopCollection(gs2, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc2);
    g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0), D2D1::Point2F(0,0)), sc2.Get(), &g.gradUsage);

    // ── Gradient: Dial cards (cream/yellow tint) ──
    D2D1_GRADIENT_STOP gs3[2] = {
        { 0.0f, D2D1::ColorF(0.996f, 0.957f, 0.886f, 0.70f) },  // warm cream
        { 1.0f, D2D1::ColorF(0.996f, 0.976f, 0.929f, 0.50f) }   // lighter cream
    };
    ComPtr<ID2D1GradientStopCollection> sc3;
    g_rt->CreateGradientStopCollection(gs3, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc3);
    g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0), D2D1::Point2F(0,0)), sc3.Get(), &g.gradDial);

    // ── Radial blob: Pink (top-left corner) ──
    D2D1_GRADIENT_STOP gp[2] = {
        { 0.0f, D2D1::ColorF(0.957f, 0.447f, 0.714f, 0.45f) },
        { 1.0f, D2D1::ColorF(0.957f, 0.447f, 0.714f, 0.0f) }
    };
    ComPtr<ID2D1GradientStopCollection> sp;
    g_rt->CreateGradientStopCollection(gp, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sp);
    g_rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0,0), D2D1::Point2F(0,0), 300, 300), sp.Get(), &g.blobPink);

    // ── Radial blob: Yellow (bottom-right) ──
    D2D1_GRADIENT_STOP gy[2] = {
        { 0.0f, D2D1::ColorF(0.996f, 0.882f, 0.278f, 0.40f) },
        { 1.0f, D2D1::ColorF(0.996f, 0.882f, 0.278f, 0.0f) }
    };
    ComPtr<ID2D1GradientStopCollection> sy;
    g_rt->CreateGradientStopCollection(gy, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sy);
    g_rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0,0), D2D1::Point2F(0,0), 350, 350), sy.Get(), &g.blobYellow);

    // ── Radial blob: Orange (mid-left) ──
    D2D1_GRADIENT_STOP go[2] = {
        { 0.0f, D2D1::ColorF(0.984f, 0.573f, 0.235f, 0.35f) },
        { 1.0f, D2D1::ColorF(0.984f, 0.573f, 0.235f, 0.0f) }
    };
    ComPtr<ID2D1GradientStopCollection> so;
    g_rt->CreateGradientStopCollection(go, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &so);
    g_rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0,0), D2D1::Point2F(0,0), 200, 200), so.Get(), &g.blobOrange);

    // ── Fonts ──
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 15.0f, g.fTitle);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, g.fTabs);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_EXTRA_BOLD, 12.0f, g.fCardTitle);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 9.0f, g.fCardSub);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 9.0f, g.fStatLabel);
    MakeFont(L"Nunito Sans", L"Segoe UI", DWRITE_FONT_WEIGHT_EXTRA_BOLD, 38.0f, g.fSpeedNum);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_EXTRA_BOLD, 12.0f, g.fSpeedLabel);
    MakeFont(L"Nunito Sans", L"Segoe UI", DWRITE_FONT_WEIGHT_BLACK, 22.0f, g.fStatNum);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f, g.fUnit);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_EXTRA_BOLD, 16.0f, g.fStatusTitle);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.0f, g.fStatusSub);
    MakeFont(L"Nunito Sans", L"Segoe UI", DWRITE_FONT_WEIGHT_EXTRA_BOLD, 20.0f, g.fDialPct);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 11.0f, g.fDialPctSm);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 12.0f, g.fDialLabel);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 9.0f, g.fDialSub);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 10.0f, g.fFooter);
    MakeFont(L"JetBrains Mono", L"Consolas", DWRITE_FONT_WEIGHT_BOLD, 10.0f, g.fMono);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 9.0f, g.fBtnLabel);
    MakeFont(L"Segoe UI Symbol", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 20.0f, g.fArrow);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, g.fFeedback);
    MakeFont(L"Segoe MDL2 Assets", L"Segoe UI Symbol", DWRITE_FONT_WEIGHT_NORMAL, 14.0f, g.fIcons);
    MakeFont(L"Inter", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.0f, g.fTodayLabel);

    return S_OK;
}

void DiscardRes() { g_rt.Reset(); }

// ─── Main Render ───
void Render(HWND hwnd) {
    if (FAILED(CreateRes(hwnd))) return;
    D2D1_SIZE_F sz = g_rt->GetSize();
    float W = sz.width;
    float H = sz.height;

    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(D2D1::ColorF::White));

    // ══════════════════════════════════════════
    // ── BACKGROUND BLOBS (high opacity, large) ──
    // ══════════════════════════════════════════
    // Pink blob: top-right area
    g.blobPink->SetCenter(D2D1::Point2F(W * 0.8f, -20.0f));
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(W * 0.8f, -20.0f), 350, 350), g.blobPink.Get());

    // Orange blob: mid-left
    g.blobOrange->SetCenter(D2D1::Point2F(-30.0f, H * 0.35f));
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(-30.0f, H * 0.35f), 280, 280), g.blobOrange.Get());

    // Yellow blob: bottom-right
    g.blobYellow->SetCenter(D2D1::Point2F(W * 0.9f, H * 0.85f));
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(W * 0.9f, H * 0.85f), 400, 400), g.blobYellow.Get());

    float px = 20.0f;    // horizontal padding
    float cw = W - 2*px; // content width

    // ══════════════════════════════════════════
    // ── TOP BAR (h=48) ──
    // ══════════════════════════════════════════
    // Logo placeholder circle
    g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(28, 24), 12, 12), g.brBorder.Get(), 1.5f);
    // Title: WATCHMAN
    Text(L"WATCHMAN", g.fTitle.Get(), D2D1::RectF(48, 0, 210, 48), g.brText.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // "Feedback" button (rounded pill with border)
    D2D1_RECT_F fbBtn = D2D1::RectF(W - 260, 11, W - 178, 37);
    Card(fbBtn, 13, nullptr, g.brDarkBorder.Get());
    Text(L"Feedback", g.fFeedback.Get(), fbBtn, g.brText.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Header icons: globe, gear, moon, minimize, close
    Text(L"\u2295  \u2699  \u263E  \u2014  \u2715", g.fIcons.Get(), D2D1::RectF(W - 166, 0, W - 10, 48), g.brText.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Bottom border of header
    g_rt->DrawLine(D2D1::Point2F(0, 48), D2D1::Point2F(W, 48), g.brBorder.Get(), 1);

    // ══════════════════════════════════════════
    // ── TAB BAR (y=48..96) ──
    // ══════════════════════════════════════════
    // Active tab: DASHBOARD (dark pill)
    D2D1_RECT_F tabActive = D2D1::RectF(12, 58, 132, 86);
    Card(tabActive, 14, g.brBlack.Get(), nullptr);
    Text(L"\u25A3  DASHBOARD", g.fTabs.Get(), tabActive, g.brWhite.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Inactive tabs with subtle icon prefixes
    Text(L"\u2502  DATA", g.fTabs.Get(), D2D1::RectF(142, 58, 212, 86), g.brTextSec.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"\u25A1  APPS", g.fTabs.Get(), D2D1::RectF(218, 58, 290, 86), g.brTextSec.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"\u223F  NETWORK", g.fTabs.Get(), D2D1::RectF(296, 58, 392, 86), g.brTextSec.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"\u2692  TOOLS", g.fTabs.Get(), D2D1::RectF(398, 58, 480, 86), g.brTextSec.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Bottom border
    g_rt->DrawLine(D2D1::Point2F(0, 96), D2D1::Point2F(W, 96), g.brBorder.Get(), 1);

    // ══════════════════════════════════════════
    // ── CONTENT AREA ──
    // ══════════════════════════════════════════
    float y = 110; // below tabs

    // ── DOWNLOAD speed card (rounded-full pill, h=58) ──
    D2D1_RECT_F dlR = D2D1::RectF(px, y, W - px, y + 58);
    Card(dlR, 29, nullptr, g.brDarkBorder.Get());
    // Blue down arrow
    Text(L"\u2193", g.fArrow.Get(), D2D1::RectF(px + 18, y, px + 48, y + 58), g.brBlue.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Label
    Text(L"DOWNLOAD", g.fSpeedLabel.Get(), D2D1::RectF(px + 50, y, px + 200, y + 58), g.brText.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Speed number + unit (right-aligned)
    Text(L"1.5", g.fSpeedNum.Get(), D2D1::RectF(W - px - 150, y - 2, W - px - 52, y + 60), g.brText.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"KB/s", g.fUnit.Get(), D2D1::RectF(W - px - 50, y, W - px - 8, y + 58), g.brTextSec.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    y += 70; // card height + gap

    // ── UPLOAD speed card ──
    D2D1_RECT_F ulR = D2D1::RectF(px, y, W - px, y + 58);
    Card(ulR, 29, nullptr, g.brDarkBorder.Get());
    // Orange up arrow
    Text(L"\u2191", g.fArrow.Get(), D2D1::RectF(px + 18, y, px + 48, y + 58), g.brOrange.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"UPLOAD", g.fSpeedLabel.Get(), D2D1::RectF(px + 50, y, px + 200, y + 58), g.brText.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"0.1", g.fSpeedNum.Get(), D2D1::RectF(W - px - 150, y - 2, W - px - 52, y + 60), g.brText.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(L"KB/s", g.fUnit.Get(), D2D1::RectF(W - px - 50, y, W - px - 8, y + 58), g.brTextSec.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    y += 74;

    // ══════════════════════════════════════════
    // ── CURRENT SESSION card (gradient fill, rounded-24, ~140h) ──
    // ══════════════════════════════════════════
    float sesH = 140;
    D2D1_RECT_F sesR = D2D1::RectF(px, y, W - px, y + sesH);
    // Set 135-degree gradient direction
    g.gradSession->SetStartPoint(D2D1::Point2F(sesR.left, sesR.top));
    g.gradSession->SetEndPoint(D2D1::Point2F(sesR.right, sesR.bottom));
    Card(sesR, 24, g.gradSession.Get(), g.brBorder.Get());

    // Title + subtitle
    Text(L"CURRENT SESSION", g.fCardTitle.Get(), D2D1::RectF(px + 20, y + 16, px + 250, y + 32), g.brText.Get());
    Text(L"Since app opened or last reset", g.fCardSub.Get(), D2D1::RectF(px + 20, y + 34, px + 280, y + 48), g.brTextSec.Get());

    // RESET SESSION button (dark pill)
    D2D1_RECT_F rstBtn = D2D1::RectF(W - px - 134, y + 16, W - px - 20, y + 44);
    Card(rstBtn, 14, g.brBlack.Get(), nullptr);
    Text(L"RESET SESSION", g.fBtnLabel.Get(), rstBtn, g.brWhite.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Divider
    float divY = y + 58;
    g_rt->DrawLine(D2D1::Point2F(px + 20, divY), D2D1::Point2F(W - px - 20, divY), g.brBorder.Get(), 1.0f);

    // 3-column stats
    float col = (cw - 40) / 3.0f;
    float statY = divY + 10;

    Text(L"DOWNLOAD", g.fStatLabel.Get(), D2D1::RectF(px + 20, statY, px + 20 + col, statY + 14), g.brTextSec.Get());
    Text(L"315.75 MB", g.fStatNum.Get(), D2D1::RectF(px + 20, statY + 16, px + 20 + col, statY + 50), g.brText.Get());

    Text(L"UPLOAD", g.fStatLabel.Get(), D2D1::RectF(px + 20 + col, statY, px + 20 + col * 2, statY + 14), g.brTextSec.Get());
    Text(L"54.20 MB", g.fStatNum.Get(), D2D1::RectF(px + 20 + col, statY + 16, px + 20 + col * 2, statY + 50), g.brText.Get());

    Text(L"TOTAL", g.fStatLabel.Get(), D2D1::RectF(px + 20 + col * 2, statY, W - px - 20, statY + 14), g.brTextSec.Get());
    Text(L"369.96 MB", g.fStatNum.Get(), D2D1::RectF(px + 20 + col * 2, statY + 16, W - px - 20, statY + 50), g.brBlue.Get());

    y += sesH + 14;

    // ══════════════════════════════════════════
    // ── USAGE SNAPSHOT card (gradient fill, rounded-24, ~130h) ──
    // ══════════════════════════════════════════
    float usH = 130;
    D2D1_RECT_F usR = D2D1::RectF(px, y, W - px, y + usH);
    g.gradUsage->SetStartPoint(D2D1::Point2F(usR.left, usR.top));
    g.gradUsage->SetEndPoint(D2D1::Point2F(usR.right, usR.bottom));
    Card(usR, 24, g.gradUsage.Get(), g.brBorder.Get());

    Text(L"USAGE SNAPSHOT", g.fCardTitle.Get(), D2D1::RectF(px + 20, y + 16, px + 250, y + 34), g.brText.Get());

    // "Today" capsule (right side)
    D2D1_RECT_F todayR = D2D1::RectF(W - px - 80, y + 14, W - px - 20, y + 38);
    Card(todayR, 12, nullptr, g.brDarkBorder.Get());
    Text(L"Today", g.fTodayLabel.Get(), todayR, g.brText.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 3-column usage stats
    float usStatY = y + 48;
    Text(L"TOTAL", g.fStatLabel.Get(), D2D1::RectF(px + 20, usStatY, px + 20 + col, usStatY + 14), g.brTextSec.Get());
    Text(L"4.95 GB", g.fStatNum.Get(), D2D1::RectF(px + 20, usStatY + 16, px + 20 + col, usStatY + 48), g.brText.Get());

    Text(L"\u2193 DOWNLOAD", g.fStatLabel.Get(), D2D1::RectF(px + 20 + col, usStatY, px + 20 + col * 2, usStatY + 14), g.brTextSec.Get());
    Text(L"3.87 GB", g.fStatNum.Get(), D2D1::RectF(px + 20 + col, usStatY + 16, px + 20 + col * 2, usStatY + 48), g.brText.Get());

    Text(L"\u2191 UPLOAD", g.fStatLabel.Get(), D2D1::RectF(px + 20 + col * 2, usStatY, W - px - 20, usStatY + 14), g.brTextSec.Get());
    Text(L"1.07 GB", g.fStatNum.Get(), D2D1::RectF(px + 20 + col * 2, usStatY + 16, W - px - 20, usStatY + 48), g.brText.Get());

    Text(L"Updated just now", g.fCardSub.Get(), D2D1::RectF(px + 20, y + usH - 22, px + 200, y + usH - 4), g.brTextSec.Get());

    y += usH + 14;

    // ══════════════════════════════════════════
    // ── NORMAL STATUS banner (rounded, h=52) ──
    // ══════════════════════════════════════════
    D2D1_RECT_F stR = D2D1::RectF(px, y, W - px, y + 52);
    Card(stR, 26, nullptr, g.brBorder.Get());
    // Green dot
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px + 32, y + 26), 10, 10), g.brGreen.Get());
    Text(L"NORMAL", g.fStatusTitle.Get(), D2D1::RectF(px + 54, y + 8, W - px, y + 28), g.brText.Get());
    Text(L"Network usage is within normal range", g.fStatusSub.Get(), D2D1::RectF(px + 54, y + 28, W - px, y + 44), g.brTextSec.Get());

    y += 64;

    // ══════════════════════════════════════════
    // ── HARDWARE DIALS (3 cards with cream background) ──
    // ══════════════════════════════════════════
    float dialGap = 10;
    float dialW = (cw - dialGap * 2) / 3.0f;
    float dialH = 115;

    struct DialInfo { const wchar_t* pct; float pctVal; const wchar_t* label; const wchar_t* sub; };
    DialInfo dials[3] = {
        { L"6", 6.0f, L"CPU", L"12 cores" },
        { L"70", 70.0f, L"RAM", L"9.8 / 13.9 GB" },
        { L"6", 6.0f, L"GPU", L"3D" }
    };

    for (int i = 0; i < 3; i++) {
        float dx = px + i * (dialW + dialGap);
        D2D1_RECT_F dR = D2D1::RectF(dx, y, dx + dialW, y + dialH);

        // Cream/yellow gradient fill for dial cards
        g.gradDial->SetStartPoint(D2D1::Point2F(dR.left, dR.top));
        g.gradDial->SetEndPoint(D2D1::Point2F(dR.right, dR.bottom));
        Card(dR, 24, g.gradDial.Get(), g.brBorder.Get());

        // Circle (radius 26) centered in card upper area
        float cx = dx + dialW / 2.0f;
        float cy = y + 44;
        float cr = 26.0f;

        // Background ring (light gray)
        g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cr, cr), g.brBorder.Get(), 3.0f);
        // Foreground arc (blue, showing percentage)
        DrawDialArc(cx, cy, cr, dials[i].pctVal, g.brBlue.Get(), 3.5f);

        // Percentage text inside circle
        D2D1_RECT_F pctR = D2D1::RectF(cx - 22, cy - 12, cx + 3, cy + 12);
        Text(dials[i].pct, g.fDialPct.Get(), pctR, g.brText.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F pctSR = D2D1::RectF(cx + 3, cy - 8, cx + 22, cy + 12);
        Text(L"%", g.fDialPctSm.Get(), pctSR, g.brText.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Label below circle
        Text(dials[i].label, g.fDialLabel.Get(), D2D1::RectF(dx, y + 76, dx + dialW, y + 92), g.brText.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Sublabel
        Text(dials[i].sub, g.fDialSub.Get(), D2D1::RectF(dx, y + 92, dx + dialW, y + 108), g.brTextSec.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // ══════════════════════════════════════════
    // ── FOOTER (h=32, pinned to bottom) ──
    // ══════════════════════════════════════════
    float fy = H - 32;
    g_rt->DrawLine(D2D1::Point2F(0, fy), D2D1::Point2F(W, fy), g.brBorder.Get(), 1);
    // Green dot + CONNECTED in green
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(24, fy + 16), 4, 4), g.brGreen.Get());
    Text(L"CONNECTED", g.fFooter.Get(), D2D1::RectF(36, fy, 160, H), g.brGreenText.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Uptime in orange mono font
    Text(L"UPTIME: 4H 20M", g.fMono.Get(), D2D1::RectF(W - 180, fy, W - 16, H), g.brOrange.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DiscardRes();
}

// ─── WndProc ───
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: Render(h); ValidateRect(h, nullptr); return 0;
    case WM_SIZE:
        if (g_rt) { g_rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l))); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_DESTROY: DiscardRes(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ─── Entry ───
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmd) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, &g_factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf()));

    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW|CS_VREDRAW, WndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"WatchmanDemo", nullptr };
    RegisterClassExW(&wc);

    // Window size: 522x782 client area
    RECT rc = { 0, 0, 522, 782 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX, FALSE);

    HWND hwnd = CreateWindowW(L"WatchmanDemo", L"Watchman Dashboard UI Preview",
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    // Rounded corners on Windows 11
    DWORD round = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));

    ShowWindow(hwnd, nCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return (int)msg.wParam;
}
