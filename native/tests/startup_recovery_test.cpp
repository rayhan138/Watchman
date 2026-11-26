// Native owned-window regression tests. The production host, attachment and
// renderer code are linked with compile-time faults; no production EXE flags,
// real Explorer modifications, monitoring services or saved settings are used.
#include <winsock2.h>
#include "renderer.h"
#include "engine.h"
#include "taskbar_embed.h"
#include "bundled_fonts.h"
#include "window_lifecycle.h"
#include <objbase.h>
#include <VersionHelpers.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

extern AppRenderer* g_renderer;
extern WatchmanEngine* g_engine;
extern HWND g_main_hwnd;
extern HWND g_widget_hwnd;
extern HWND g_floating_monitor_hwnd;
extern DashboardSnapshot g_display_dashboard_snapshot;
extern bool g_has_display_dashboard_snapshot;
extern LRESULT CALLBACK FloatingMonitorWndProc(HWND, UINT, WPARAM, LPARAM);
void UpdateFloatingMonitorState(bool reset = false);
void WatchmanTrayReadyForTest(bool ready);
void WatchmanBlockTrayForTest(HANDLE entered, HANDLE release);
bool WatchmanTrayRegisteredForTest();
bool WatchmanTrayPendingForTest();
uint64_t WatchmanTrayRetryAtForTest();
void WatchmanTickTrayForTest(uint64_t now);
void WatchmanJoinTrayForTest();
void PresentMainWindowForLaunch(const WatchmanRuntime::LaunchOptions& launch, int show_command);
extern LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
extern LRESULT CALLBACK WidgetWndProc(HWND, UINT, WPARAM, LPARAM);
void UpdateWidgetState(bool force_reembed = false);
WatchmanWidgetRuntime::Result WatchmanWidgetStatusForTest();

namespace {
using namespace WatchmanWidgetRuntime;
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        const auto result = WatchmanWidgetStatusForTest();
        std::cerr << "Widget status=" << EventName(result.status) << ", error=" << result.error << '\n';
        throw std::runtime_error(message);
    }
}
void Register(const wchar_t* name, WNDPROC proc = DefWindowProcW) {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = proc;
    wc.lpszClassName = name;
    Check(RegisterClassW(&wc) != 0, "Could not register owned test window class");
}
HWND Window(const wchar_t* name, HWND parent, int x, int y, int width, int height, bool visible = true) {
    const HWND result = CreateWindowExW(0, name, L"Watchman test window",
        (parent ? WS_CHILD : WS_POPUP) | (visible ? WS_VISIBLE : 0),
        x, y, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(result != nullptr, "Could not create owned test window");
    return result;
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message != WM_QUIT) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
}
template<class Predicate> bool Wait(Predicate predicate) {
    const auto until = GetTickCount64() + 5000;
    do {
        Pump();
        if (predicate()) return true;
        Sleep(1);
    } while (GetTickCount64() < until);
    return predicate();
}
void ShellEvent() {
    SendMessageW(g_main_hwnd, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    Pump();
}
void Expect(Status status, const char* message) { Check(WatchmanWidgetStatusForTest().status == status, message); }
void ExpectFrame(const char* message) {
    Check(g_renderer->GetWidgetPaintResult().success, message);
    Expect(Status::Ready, "A successful frame must publish Ready");
}
int Width(HWND window) {
    RECT rect{};
    Check(GetWindowRect(window, &rect) != FALSE, "Could not read owned test bounds");
    return rect.right - rect.left;
}
struct FakeShell {
    HWND bar = nullptr, list = nullptr, tray = nullptr, rebar = nullptr;
    explicit FakeShell(bool legacy = false, bool children = true) {
        bar = Window(L"Shell_TrayWnd", nullptr, 20, 300, 1000, 48);
        if (legacy) {
            rebar = Window(L"ReBarWindow32", bar, 0, 0, 1000, 48);
            list = Window(L"MSTaskListWClass", rebar, 0, 0, 900, 48);
        } else if (children) {
            list = Window(L"MSTaskListWClass", bar, 40, 0, 670, 48);
            tray = Window(L"TrayNotifyWnd", bar, 920, 0, 80, 48);
        }
        tm_use_taskbar_for_test(bar);
    }
    ~FakeShell() { if (IsWindow(bar)) DestroyWindow(bar); }
};

void QuietStartupWithUnavailableTray() {
    std::cout << "Native recovery: quiet startup while tray registration is blocked/failed" << std::endl;
    struct Gate {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool finished = false;
        ~Gate() {
            WatchmanBlockTrayForTest(nullptr, nullptr);
            if (release) SetEvent(release);
            if (!finished) WatchmanJoinTrayForTest();
            if (entered) CloseHandle(entered);
            if (release) CloseHandle(release);
        }
    } gate;
    Check(gate.entered && gate.release, "Could not create the owned tray-call gate");
    WatchmanTrayReadyForTest(false);
    g_renderer->SetWidgetTimeForTest(1000); // Deterministic first-GDI policy even on a slow runner.
    WatchmanBlockTrayForTest(gate.entered, gate.release);
    tm_use_taskbar_for_test(nullptr);
    const auto launch = WatchmanRuntime::ParseArguments({L"C:\\Watchman\\Watchman.exe", L"--startup"});
    PresentMainWindowForLaunch(launch, SW_SHOWNORMAL);
    Check(WaitForSingleObject(gate.entered, 5000) == WAIT_OBJECT_0,
        "The production startup path must submit the shell call on its worker");
    Check(!IsWindowVisible(g_main_hwnd) && GetForegroundWindow() != g_main_hwnd && !g_renderer->IsWelcomeTourOpen(),
        "Quiet startup must not show/focus a dashboard or welcome while the tray is blocked");
    Check(g_renderer->Initialize(g_main_hwnd) == S_FALSE && !g_renderer->MainResourcesInitializedForTest() &&
        g_renderer->MainInitializationAttemptsForTest() == 0,
        "Even an explicit hidden initialize request must not load dashboard resources");
    SendMessageW(g_main_hwnd, WM_PAINT, 0, 0);
    Check(g_renderer->MainInitializationAttemptsForTest() == 0, "Hidden paint messages must not initialize dashboard graphics");
    auto shell = std::make_unique<FakeShell>();
    ShellEvent(); // A replacement generation queues behind the blocked call.
    ExpectFrame("Widget must submit an independent first frame while tray registration is blocked");
    Check(!IsWindowVisible(g_main_hwnd) && GetForegroundWindow() != g_main_hwnd &&
        !g_renderer->MainResourcesInitializedForTest() &&
        g_renderer->GetWidgetPaintResult().backend == Backend::Gdi && g_renderer->CompositionAttemptsForTest() == 0,
        "TaskbarCreated must not turn a blocked tray call into a visible dashboard or GPU startup dependency");
    WatchmanBlockTrayForTest(nullptr, nullptr);
    SetEvent(gate.release);
    Check(Wait([&] { WatchmanTickTrayForTest(0); return !WatchmanTrayPendingForTest(); }),
        "Failed shell calls must publish a result without another broadcast");
    Check(!WatchmanTrayRegisteredForTest() && WatchmanTrayRetryAtForTest() == 100 && !IsWindowVisible(g_main_hwnd),
        "Failed quiet tray registration must schedule a retry, never show a fallback dashboard");
    WatchmanTrayReadyForTest(true);
    WatchmanTickTrayForTest(99);
    Check(!WatchmanTrayPendingForTest() && !WatchmanTrayRegisteredForTest(), "Tray retry must not run before its cooldown");
    WatchmanTickTrayForTest(100);
    Check(Wait([&] { WatchmanTickTrayForTest(100); return WatchmanTrayRegisteredForTest(); }),
        "Tray registration must recover through polling without requiring a new TaskbarCreated");
    Check(!IsWindowVisible(g_main_hwnd) && !g_renderer->MainResourcesInitializedForTest(),
        "Successful delayed tray recovery must remain quiet and leave the dashboard unloaded");
    gate.finished = true;
    // Restore a pristine independent-widget first-frame state for the rest of
    // this suite; no monitoring services or preferences have been started.
    g_renderer->SetWidgetEnabled(false);
    UpdateWidgetState();
    if (IsWindow(g_widget_hwnd)) DestroyWindow(g_widget_hwnd);
    Pump();
    Check(tm_reset_taskbar_binding() != 0, "Could not restore the owned startup shell layout");
    shell.reset(); tm_use_taskbar_for_test(nullptr);
    g_renderer->SetWidgetEnabled(true);
    g_renderer->ResetWidgetHost();
    g_renderer->SetWidgetTimeForTest(0);
}

// Optional disposable-runner screenshots of production rendering with an
// explicitly synthetic release fixture. Never injects a fake update into the
// shipped EXE and never reads/captures a personal desktop outside this window.
void SaveUpdateFixture(const wchar_t* name, bool footer_only = false) {
    wchar_t folder[32768]{}, actions[16]{};
    if (!GetEnvironmentVariableW(L"WATCHMAN_UPDATE_UI_EVIDENCE", folder, static_cast<DWORD>(std::size(folder))) ||
        !GetEnvironmentVariableW(L"GITHUB_ACTIONS", actions, static_cast<DWORD>(std::size(actions))) || wcscmp(actions, L"true") != 0) return;
    std::filesystem::create_directories(folder);
    RECT client{};
    Check(GetClientRect(g_main_hwnd, &client) != FALSE, "Could not capture update fixture bounds");
    const int width = client.right, height = footer_only ? 32 : client.bottom;
    const int top = footer_only ? client.bottom - 32 : 0;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC window = GetDC(g_main_hwnd), memory = CreateCompatibleDC(window);
    const auto close_bitmap = [](void* object) { if (object) DeleteObject(static_cast<HBITMAP>(object)); };
    std::unique_ptr<void, decltype(close_bitmap)> bitmap(CreateDIBSection(window, &info, DIB_RGB_COLORS, &bits, nullptr, 0), close_bitmap);
    const auto old = bitmap ? SelectObject(memory, bitmap.get()) : nullptr;
    DwmFlush();
    const bool copied = old && old != HGDI_ERROR && BitBlt(memory, 0, 0, width, height, window, 0, top, SRCCOPY);
    if (old && old != HGDI_ERROR) SelectObject(memory, old);
    DeleteDC(memory); ReleaseDC(g_main_hwnd, window);
    Check(copied && bits, "Could not capture owned update fixture pixels");
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "Could not initialize PNG fixture encoder");
    Check(SUCCEEDED(factory->CreateStream(&stream)), "Could not create PNG fixture stream");
    const auto path = std::filesystem::path(folder) / name;
    Check(SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)), "Could not open PNG fixture file");
    Check(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(width, height)), "Could not start PNG fixture frame");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(SUCCEEDED(frame->SetPixelFormat(&format)), "Could not choose PNG fixture pixel format");
    // GDI fills BGR, not alpha. Make the captured app client opaque.
    auto* pixels = static_cast<BYTE*>(bits);
    for (int i = 3; i < width * height * 4; i += 4) pixels[i] = 255;
    Check(SUCCEEDED(frame->WritePixels(height, width * 4, width * height * 4, pixels)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "Could not write PNG update fixture");
}

void FloatingTypographyPixels() {
    std::cout << "Native floating typography: actual GDI pixels, no color-dependent outline" << std::endl;
    const HWND floating = g_floating_monitor_hwnd;
    const auto engine = g_engine;
    const bool was_transparent = g_renderer->IsFloatingMonitorBackgroundTransparent();
    const int old_opacity = g_renderer->GetFloatingMonitorOpacity();
    DashboardSnapshot sample{};
    sample.sample_sequence = 42;
    sample.upload_speed_bps = 4096000;
    sample.download_speed_bps = 8192000;
    sample.cpu_sample_available = sample.ram_sample_available = true;
    sample.cpu_sampled_at_ms = sample.ram_sampled_at_ms = GetTickCount64();
    sample.cpu_usage_percent = 24;
    sample.ram_usage_percent = 58;
    sample.ram_total_bytes = 8ULL * 1024 * 1024 * 1024;
    g_renderer->SetFloatingColorsForTest(RGB(255, 0, 0));
    uint64_t light_ink = 0, bold_ink = 0;
    for (const auto weight : FloatingMonitorText::weights) {
        g_renderer->SetFloatingMonitorTextWeight(weight);
        for (int font = FloatingMonitorLayout::minimum_font_size; font <= FloatingMonitorLayout::maximum_font_size; ++font) {
            g_renderer->SetFloatingMonitorFontSize(font);
            for (bool cpu : {false, true}) for (bool memory : {false, true}) {
                g_renderer->SetFloatingMonitorCpuShown(cpu);
                g_renderer->SetFloatingMonitorMemoryShown(memory);
                UpdateFloatingMonitorState();
                RECT bounds{}; GetWindowRect(floating, &bounds);
                Check(g_floating_monitor_hwnd == floating && g_engine == engine && bounds.left == 100 && bounds.top == 100,
                    "Typography must retain the HWND, position, readings and engine");
                for (bool transparent : {false, true}) {
                    uint64_t previous_ink = 0;
                    for (int opacity : {20, 92, 100}) {
                        g_renderer->SetFloatingMonitorBackgroundTransparent(transparent);
                        g_renderer->SetFloatingMonitorOpacity(opacity);
                        const auto submissions = g_renderer->FloatingSubmissionsForTest();
                        const HDC dc = GetDC(floating);
                        Check(dc != nullptr, "Could not paint the actual floating typography fixture");
                        sample.cpu_sampled_at_ms = sample.ram_sampled_at_ms = GetTickCount64();
                        g_renderer->RenderFloatingMonitor(floating, dc, sample);
                        ReleaseDC(floating, dc);
                        const auto& pixels = g_renderer->FloatingPixelsForTest();
                        Check(g_renderer->FloatingSubmissionsForTest() > submissions &&
                            pixels.size() == static_cast<size_t>(bounds.right - bounds.left) * (bounds.bottom - bounds.top),
                            "Each weight/background/opacity needs an accepted actual floating surface");
                        bool tinted_once = true, premultiplied = true;
                        uint64_t ink = 0;
                        for (const auto pixel : pixels) {
                            const DWORD red = (pixel >> 16) & 255u, alpha = pixel >> 24;
                            tinted_once = tinted_once && (pixel & 0xFFFFu) == 0;
                            premultiplied = premultiplied && red <= alpha;
                            ink += red;
                        }
                        Check(tinted_once && ink > 0,
                            "Pure red text must have no white halo or colored antialiasing fringe");
                        Check(premultiplied, "Every actual floating text pixel must remain premultiplied");
                        if (previous_ink) Check(ink == previous_ink, "Panel opacity must not fade or thicken the text");
                        previous_ink = ink;
                        if (font == 12 && cpu && memory && transparent && opacity == 100) {
                            if (weight == FloatingMonitorText::Weight::Light) light_ink = ink;
                            if (weight == FloatingMonitorText::Weight::Bold) bold_ink = ink;
                        }
                        if (transparent && opacity == 100) {
                            g_renderer->SetFloatingColorsForTest(RGB(255, 255, 255));
                            const HDC white_dc = GetDC(floating);
                            Check(white_dc != nullptr, "Could not inspect white floating text");
                            g_renderer->RenderFloatingMonitor(floating, white_dc, sample);
                            ReleaseDC(floating, white_dc);
                            bool no_dark_stroke = true;
                            for (const auto pixel : g_renderer->FloatingPixelsForTest()) {
                                const DWORD red = (pixel >> 16) & 255u, alpha = pixel >> 24;
                                // Transparent hit pixels add at most one alpha
                                // level. A black halo would add alpha, not ink.
                                no_dark_stroke = no_dark_stroke && red == ((pixel >> 8) & 255u) &&
                                    red == (pixel & 255u) && alpha >= red && alpha - red <= 1;
                            }
                            Check(no_dark_stroke, "White text must not contain a hidden dark outline on transparent desktops");
                            g_renderer->SetFloatingColorsForTest(RGB(255, 0, 0));
                        }
                    }
                }
            }
        }
    }
    Check(light_ink > 0 && bold_ink > light_ink,
        "The genuine Light glyphs must use less ink than Bold; do not fake weight with a stroke");
    g_renderer->SetFloatingColorsForTest(CLR_INVALID);
    g_renderer->SetFloatingMonitorBackgroundTransparent(was_transparent);
    g_renderer->SetFloatingMonitorOpacity(old_opacity);
    g_renderer->SetFloatingMonitorTextWeight(FloatingMonitorText::default_weight);
    g_renderer->SetFloatingMonitorFontSize(FloatingMonitorLayout::default_font_size);
    g_renderer->SetFloatingMonitorCpuShown(true);
    g_renderer->SetFloatingMonitorMemoryShown(true);
    UpdateFloatingMonitorState();
}

void DashboardGraphicsRecovery(WatchmanEngine& engine) {
    std::cout << "Native recovery: deliberate dashboard graphics failure/retry without widget dependency" << std::endl;
    SetWindowPos(g_main_hwnd, nullptr, 50, 50, 480, 650, SWP_NOZORDER | SWP_NOACTIVATE);
    g_renderer->SetMainTimeForTest(10000);
    g_renderer->SetMainGraphicsFaultsForTest(E_FAIL);
    const auto initialization = g_renderer->MainInitializationAttemptsForTest();
    const auto frames = g_renderer->WidgetSubmissionsForTest();
    // The actual second-launch activation path still opens the same dashboard.
    SendMessageW(g_main_hwnd, WM_APP + 13, 0, 0);
    UpdateWindow(g_main_hwnd);
    Check(IsWindowVisible(g_main_hwnd) && g_renderer->MainGraphicsUnavailable() &&
        !g_renderer->MainResourcesInitializedForTest() &&
        g_renderer->MainInitializationAttemptsForTest() == initialization + 1 && g_renderer->MainFallbackPaintsForTest() > 0,
        "A deliberate opening with a partial font/factory failure must paint a readable GDI fallback");
    SaveUpdateFixture(L"dashboard-graphics-unavailable-FIXTURE.png");
    const auto tab = g_renderer->GetActiveTab();
    SendMessageW(g_main_hwnd, WM_KEYDOWN, VK_RETURN, 1);
    g_renderer->OnLButtonDown(g_main_hwnd, 140, 65, engine);
    Check(g_renderer->GetActiveTab() == tab && g_renderer->IsWelcomeTourOpen(),
        "Fallback must consume keyboard/page actions instead of operating an unseen modal or tab");
    for (int paint = 0; paint < 20; ++paint)
        g_renderer->Render(g_main_hwnd, {}, {}, {}, {}, {}, {});
    Check(g_renderer->MainInitializationAttemptsForTest() == initialization + 1 && !g_renderer->NeedsAnimationFrame(),
        "Failed dashboard initialization must not hot-loop on paint or animation ticks");
    UpdateWidgetState(true);
    Check(g_renderer->WidgetSubmissionsForTest() > frames && g_renderer->GetWidgetPaintResult().success,
        "Failed dashboard setup and partial cleanup must leave independent widget presentation alive");
    g_renderer->OnLButtonDown(g_main_hwnd, 456, 24, engine); // Fallback's real close control.
    Pump();
    Check(!IsWindowVisible(g_main_hwnd), "The fallback close control must remain usable");
    g_renderer->SetMainGraphicsFaultsForTest(S_OK);
    g_renderer->SetMainTimeForTest(g_renderer->MainGraphicsRetryAtForTest());
    Check(g_renderer->Initialize(g_main_hwnd) == S_FALSE &&
        g_renderer->MainInitializationAttemptsForTest() == initialization + 1,
        "A due retry must not initialize graphics while the dashboard is hidden");
    SendMessageW(g_main_hwnd, WM_APP + 13, 0, 0);
    UpdateWindow(g_main_hwnd);
    Check(!g_renderer->MainGraphicsUnavailable() && g_renderer->MainResourcesInitializedForTest() &&
        g_renderer->MainInitializationAttemptsForTest() == initialization + 2 && g_renderer->MainPaintsForTest() > 0,
        "A transient main initialization failure must recover in the same process after its cooldown");
    g_renderer->CompleteWelcomeForTest(); // In-memory only: never write an owned fixture into HKCU preferences.
    const auto targets = g_renderer->MainTargetAttemptsForTest();
    g_renderer->ReleaseMainWindowResources();
    g_renderer->SetMainGraphicsFaultsForTest(S_OK, E_OUTOFMEMORY);
    g_renderer->SetMainTimeForTest(20000);
    g_renderer->Render(g_main_hwnd, {}, {}, {}, {}, {}, {});
    Check(g_renderer->MainGraphicsUnavailable() && g_renderer->MainResourcesInitializedForTest() &&
        g_renderer->MainTargetAttemptsForTest() == targets + 1,
        "HWND target failure must use the fallback without discarding healthy independent resources");
    for (int paint = 0; paint < 10; ++paint) g_renderer->Render(g_main_hwnd, {}, {}, {}, {}, {}, {});
    Check(g_renderer->MainTargetAttemptsForTest() == targets + 1, "A failed main target must also respect retry backoff");
    g_renderer->SetMainGraphicsFaultsForTest(S_OK);
    g_renderer->SetMainTimeForTest(g_renderer->MainGraphicsRetryAtForTest());
    g_renderer->Render(g_main_hwnd, {}, {}, {}, {}, {}, {});
    Check(!g_renderer->MainGraphicsUnavailable() && g_renderer->MainTargetAttemptsForTest() == targets + 2 &&
        g_renderer->MainInitializationAttemptsForTest() == initialization + 2,
        "HWND target recovery must reuse healthy factories and resume actual dashboard drawing");
    WatchmanTrayReadyForTest(false);
    ShowWindow(g_main_hwnd, SW_MINIMIZE);
    ShellEvent();
    Check(IsIconic(g_main_hwnd), "A failed tray recreation must not unminimize an intentionally minimized dashboard");
    // A normal interactive launch remains visible even when the tray fails.
    PresentMainWindowForLaunch({}, SW_RESTORE);
    Check(IsWindowVisible(g_main_hwnd) && !IsIconic(g_main_hwnd), "Interactive launch must still open without a usable tray");
    WatchmanTrayReadyForTest(true);
    SendMessageW(g_main_hwnd, WM_CLOSE, 0, 0);
    g_renderer->SetMainTimeForTest(0);
}

void UpdateNoticeUiFixture(WatchmanEngine& engine) {
    std::cout << "Native recovery: three-surface update UI fixture (not a published release)" << std::endl;
    g_renderer->DismissSettings();
    g_renderer->SetActiveTab(ActiveTab::Dashboard);
    const auto saved = g_renderer->SettingsWritesForTest();
    const auto result = WatchmanUpdates::EvaluateResponse(200,
        R"({"tag_name":"v9.8.7","html_url":"https://github.com/rayhan138/Watchman/releases/tag/v9.8.7","draft":false,"prerelease":false})",
        WatchmanUpdates::Version{2, 0, 1, 0});
    Check(result.status == WatchmanUpdates::Status::Available, "Update UI needs an explicit newer-version fixture");
    g_renderer->SetUpdateResult(result);
    g_renderer->OfferUpdatePrompt(false);
    Check(!g_renderer->IsUpdatePromptOpen(), "Quiet startup must not open/consume the update prompt");
    g_renderer->OfferUpdatePrompt(true);
    Check(g_renderer->IsUpdatePromptOpen(), "A deliberate app opening should offer the update prompt");
    NetworkSnapshot network; network.is_connected = true;
    const auto paint = [&] {
        g_renderer->SetRuntimeStatus(L"");
        g_renderer->Render(g_main_hwnd, g_display_dashboard_snapshot, network, {}, {}, {}, {});
    };
    SetWindowPos(g_main_hwnd, HWND_TOP, 20, 20, 560, 700, SWP_SHOWWINDOW);
    g_renderer->SetTheme(ThemeMode::Light); paint();
    SaveUpdateFixture(L"update-ui-light-FIXTURE.png");
    const auto tab = g_renderer->GetActiveTab();
    g_renderer->OnLButtonDown(g_main_hwnd, 150, 65, engine);
    Check(g_renderer->GetActiveTab() == tab && g_renderer->IsUpdatePromptOpen(), "Modal must consume clicks instead of activating the page behind it");
    g_renderer->SetTheme(ThemeMode::Dark); paint();
    SaveUpdateFixture(L"update-ui-dark-FIXTURE.png");
    SetWindowPos(g_main_hwnd, HWND_TOP, 20, 20, 460, 650, 0); paint();
    SaveUpdateFixture(L"update-ui-compact-FIXTURE.png");
    SendMessageW(g_main_hwnd, WM_KEYDOWN, VK_ESCAPE, 1);
    Check(!g_renderer->IsUpdatePromptOpen() && !g_renderer->Updates().MayPrompt() && g_renderer->Updates().Available(),
        "Escape must dismiss only this version's prompt, preserving the footer and monitoring");
    paint();
    SaveUpdateFixture(L"update-footer-dark-FIXTURE.png", true);
    g_renderer->SetTheme(ThemeMode::Light); paint();
    SaveUpdateFixture(L"update-footer-light-FIXTURE.png", true);
    g_renderer->OfferUpdatePrompt(true);
    Check(!g_renderer->IsUpdatePromptOpen(), "App activation must not nag after a version was dismissed");
    g_renderer->OfferUpdatePrompt(true, true);
    Check(g_renderer->IsUpdatePromptOpen(), "A deliberate update notification click can reopen a dismissed prompt");
    g_renderer->HandleUpdatePromptKeyDown(g_main_hwnd, VK_TAB, false); // Download -> Not now
    g_renderer->HandleUpdatePromptKeyDown(g_main_hwnd, VK_RETURN, false);
    Check(!g_renderer->IsUpdatePromptOpen(), "Keyboard focus must reach the non-download action");
    g_renderer->OfferUpdatePrompt(true, true);
    g_renderer->HandleUpdatePromptKeyDown(g_main_hwnd, VK_TAB, true); // Download -> cross
    g_renderer->HandleUpdatePromptKeyDown(g_main_hwnd, VK_SPACE, false);
    Check(!g_renderer->IsUpdatePromptOpen(), "Shift-Tab must reach the dismiss cross");
    g_renderer->OfferUpdatePrompt(true, true);
    const auto l = WatchmanUpdates::Presentation::GetLayout(460, 650);
    const LPARAM cross = MAKELPARAM(static_cast<int>((l.close.left + l.close.right) / 2), static_cast<int>((l.close.top + l.close.bottom) / 2));
    SendMessageW(g_main_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, cross);
    SendMessageW(g_main_hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, cross);
    Check(!g_renderer->IsUpdatePromptOpen() && g_renderer->GetActiveTab() == tab, "Dismissal double-click must not reach a newly exposed page control");
    Check(g_renderer->SettingsWritesForTest() == saved, "Update notice actions must not rewrite unrelated settings");
    g_renderer->SetUpdateResult({WatchmanUpdates::Status::Unavailable, {}, 403});
    Check(g_renderer->Updates().Available() && !g_renderer->IsUpdatePromptOpen(), "Failed recheck must keep the last confirmed update without reopening it");
    g_renderer->SetUpdateResult({WatchmanUpdates::Status::UpToDate, {}, 0});
    Check(!g_renderer->Updates().Available(), "A genuinely current/withdrawn result must clear update UI");
    g_renderer->OpenWelcomeTour();
    g_renderer->SetUpdateResult(result);
    g_renderer->OfferUpdatePrompt(true, true);
    Check(g_renderer->IsWelcomeTourOpen() && !g_renderer->IsUpdatePromptOpen(), "An update prompt must queue behind the welcome tour, not stack on it");
}

void Exercise() {
    Register(L"WatchmanWindowClass", WndProc);
    Register(L"WatchmanWidgetClass", WidgetWndProc);
    Register(L"WatchmanFloatingMonitorClass", FloatingMonitorWndProc);
    for (const auto* name : {L"Shell_TrayWnd", L"MSTaskListWClass", L"TrayNotifyWnd", L"ReBarWindow32", L"Progman", L"WorkerW"}) Register(name);
    auto renderer = std::make_unique<AppRenderer>();
    WatchmanEngine engine; // Cheap construction; deliberately never Start.
    g_renderer = renderer.get();
    g_engine = &engine;
    g_main_hwnd = Window(L"WatchmanWindowClass", nullptr, 100, 100, 480, 200, false);
    Check(!g_renderer->MainResourcesInitializedForTest(), "A hidden host must not initialize dashboard fonts/graphics");
    QuietStartupWithUnavailableTray();
    g_renderer->SaveSettings();
    const auto saved = g_renderer->SettingsWritesForTest();
    g_renderer->SetActiveTab(ActiveTab::Data);
    g_renderer->OnMouseMove(20, 65);
    g_renderer->SaveSettings();
    Check(g_renderer->SettingsWritesForTest() == saved, "Tab/hover-only interactions must not rewrite Registry preferences");
    g_renderer->SetActiveTab(ActiveTab::Dashboard);
    g_renderer->SetWidgetEnabled(false); g_renderer->SaveSettings(); g_renderer->SaveSettings();
    Check(g_renderer->SettingsWritesForTest() == saved + 1, "A real preference change needs one persisted batch, not repeated batches");
    g_renderer->SetWidgetEnabled(true);
    const auto engine_identity = g_engine;

    tm_use_taskbar_for_test(nullptr);
    const HWND unrelated = Window(L"STATIC", nullptr, 0, 0, 10, 10, false);
    g_widget_hwnd = unrelated; // A recycled non-null HWND with the wrong class.
    UpdateWidgetState(true);
    Check(IsWindow(unrelated) && g_widget_hwnd != unrelated,
        "Stale host recovery must replace, not re-style or destroy, an unrelated owned window");
    DestroyWindow(unrelated);
    Expect(Status::WaitingForShell, "Missing taskbar must be distinguishable from an API failure");
    Check(IsWindow(g_widget_hwnd) && !IsWindowVisible(g_widget_hwnd), "Host may exist but cannot float over the desktop");

    auto shell = std::make_unique<FakeShell>(false, false);
    ShellEvent();
    Expect(Status::WaitingForLayout, "Incomplete shell hierarchy must wait for layout");
    shell->list = Window(L"MSTaskListWClass", shell->bar, 40, 0, 850, 48);
    shell->tray = Window(L"TrayNotifyWnd", shell->bar, 920, 0, 80, 48);
    UpdateWidgetState();
    Expect(Status::NoSpace, "An insufficient known gap must be a no-space outcome");
    Check(!IsWindowVisible(g_widget_hwnd) && GetParent(g_widget_hwnd) == nullptr && tm_input_widget_for_test() == nullptr,
        "No-space recovery must keep the popup hidden and detached");
    SetWindowPos(shell->list, nullptr, 40, 0, 1660, 48, SWP_NOZORDER);
    UpdateWidgetState();
    Expect(Status::UnsupportedLayout, "Out-of-taskbar bounds must be reported as unsupported, not empty");

    SetWindowPos(shell->list, nullptr, 40, 0, 670, 48, SWP_NOZORDER);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    Check(GetMonitorInfoW(MonitorFromWindow(shell->bar, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
        "Could not read the owned test monitor for fullscreen classification");
    const RECT screen = monitor.rcMonitor;
    for (const auto* desktop_class : {L"Progman", L"WorkerW"}) {
        const HWND desktop = Window(desktop_class, nullptr, screen.left, screen.top,
            screen.right - screen.left, screen.bottom - screen.top);
        tm_use_foreground_for_test(desktop);
        int visible = 0;
        Check(tm_should_widget_be_visible(g_widget_hwnd, g_main_hwnd, &visible) && visible,
            "Explorer wallpaper must not suppress recovery as if it were a fullscreen app");
        DestroyWindow(desktop);
    }
    const HWND fullscreen = Window(L"STATIC", nullptr, screen.left, screen.top,
        screen.right - screen.left, screen.bottom - screen.top);
    tm_use_foreground_for_test(fullscreen);
    int visible = 1;
    Check(tm_should_widget_be_visible(g_widget_hwnd, g_main_hwnd, &visible) && !visible,
        "A real fullscreen application must still suppress an unattached widget");
    UpdateWidgetState();
    Expect(Status::HiddenFullscreen, "Fullscreen app hiding must remain a distinct outcome");
    Check(!IsWindowVisible(g_widget_hwnd), "Fullscreen suppression must not create a visible top-level overlay");
    tm_use_foreground_for_test(nullptr);
    DestroyWindow(fullscreen);

    tm_fail_next_parent_for_test(ERROR_ACCESS_DENIED);
    UpdateWidgetState();
    Expect(Status::AttachFailed, "SetParent failure must be an actual attachment error");
    Check(WatchmanWidgetStatusForTest().error == ERROR_ACCESS_DENIED && !IsWindowVisible(g_widget_hwnd),
        "SetParent error must survive style rollback and remain hidden");
    tm_fail_next_position_for_test(ERROR_INVALID_PARAMETER);
    UpdateWidgetState();
    Expect(Status::AttachFailed, "Position failure must not count as successful attachment");
    Check(WatchmanWidgetStatusForTest().error == ERROR_INVALID_PARAMETER && GetParent(g_widget_hwnd) == nullptr,
        "A failed position must retain its error and detach the widget");
    UpdateWidgetState();
    ExpectFrame("Retry must submit a real frame without restarting the process");
    Check(GetParent(g_widget_hwnd) == shell->bar && IsWindowVisible(g_widget_hwnd), "Widget must be a visible taskbar child");
    Check(tm_input_widget_for_test() == g_widget_hwnd, "Mouse input must bind only to the successfully placed current host");
    Check(!g_renderer->MainResourcesInitializedForTest() &&
        g_renderer->GetWidgetPaintResult().backend == Backend::Gdi && g_renderer->CompositionAttemptsForTest() == 0,
        "First accepted widget frame must need neither dashboard resources nor Direct3D initialization");
    g_renderer->SetWidgetTimeForTest(GetTickCount64() + 2000);
    UpdateWidgetState(true); // Deferred, quiet optional GPU trial; no sleep needed.
    ExpectFrame("A deferred GPU trial must preserve a usable frame");
    const auto first = g_renderer->WidgetSubmissionsForTest();
    Check(first > 0, "At least one successful native presentation is required");

    const bool gpu = g_renderer->GetWidgetPaintResult().backend == Backend::Composition;
    g_renderer->RenderWidget(g_widget_hwnd, {});
    if (gpu) Check(g_renderer->WidgetSubmissionsForTest() == first && !g_renderer->GetWidgetPaintResult().submitted,
        "An unchanged GPU frame may be reused only while the surface remains valid");
    else std::cout << "NOTE: native GPU unavailable; real GDI success required, GPU cache assertion skipped.\n";
    const auto before_force = g_renderer->WidgetSubmissionsForTest();
    UpdateWidgetState(true);
    Check(g_renderer->WidgetSubmissionsForTest() > before_force,
        "Forced shell/DPI refresh must submit even with identical readings");

    const auto before_destroy = g_renderer->WidgetSubmissionsForTest();
    Check(DestroyWindow(g_widget_hwnd) != FALSE, "Could not destroy the owned widget host");
    Pump();
    ExpectFrame("Destroyed host must be recreated, rebound and painted");
    Check(g_renderer->WidgetSubmissionsForTest() > before_destroy && g_engine == engine_identity,
        "Host recovery must repaint without replacing the engine");

    const auto before_shell = g_renderer->WidgetSubmissionsForTest();
    shell.reset(); // Own parent destruction really destroys the child HWND.
    tm_use_taskbar_for_test(nullptr);
    Pump();
    UpdateWidgetState();
    Expect(Status::WaitingForShell, "A missing replacement Explorer must wait safely");
    shell = std::make_unique<FakeShell>();
    ShellEvent();
    ExpectFrame("Replacement shell must receive a newly bound frame");
    Check(g_renderer->WidgetSubmissionsForTest() > before_shell && g_engine == engine_identity,
        "Explorer-like parent loss must not restart backend monitoring");

    // Also cover a surviving HWND across shell replacement/reparent.
    const HWND survivor = g_widget_hwnd;
    Check(tm_reset_taskbar_binding() != 0, "Could not detach surviving owned host");
    shell.reset();
    shell = std::make_unique<FakeShell>();
    const auto before_reparent = g_renderer->WidgetSubmissionsForTest();
    ShellEvent();
    ExpectFrame("Surviving host needs fresh graphics after shell replacement");
    Check(g_widget_hwnd == survivor && g_renderer->WidgetSubmissionsForTest() > before_reparent,
        "Same-HWND shell recovery must bypass stale-value frame caching");

    DashboardGraphicsRecovery(engine);
    std::cout << "Native recovery: production close/minimize/restore and independent widget frames" << std::endl;
    // Production close (not just the isolated lifecycle helper) must retain
    // independent HWNDs, resources and future accepted readings on both widgets.
    g_renderer->SetFloatingMonitorEnabled(true);
    g_renderer->SetFloatingMonitorPosition(100, 100);
    g_floating_monitor_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"WatchmanFloatingMonitorClass", L"Owned floating monitor", WS_POPUP,
        100, 100, 220, 54, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(IsWindow(g_floating_monitor_hwnd), "Could not create the independent floating monitor");
    UpdateFloatingMonitorState();
    UpdateWindow(g_floating_monitor_hwnd);
    Check(IsWindowVisible(g_floating_monitor_hwnd) && GetWindow(g_floating_monitor_hwnd, GW_OWNER) == nullptr,
        "The floating monitor must be unowned, not an auto-hidden dashboard popup");
    const HWND floating = g_floating_monitor_hwnd;
    // All optional modes use the same independent window and existing samples.
    // Changing its height must retain position, text size, and engine identity.
    for (bool cpu : {false, true}) for (bool memory : {false, true}) {
        g_renderer->SetFloatingMonitorCpuShown(cpu);
        g_renderer->SetFloatingMonitorMemoryShown(memory);
        for (int font : {11, 12, 16}) {
            g_renderer->SetFloatingMonitorFontSize(font);
            UpdateFloatingMonitorState();
            UpdateWindow(floating);
            RECT bounds{}; GetWindowRect(floating, &bounds);
            Check(g_floating_monitor_hwnd == floating && bounds.left == 100 && bounds.top == 100 &&
                bounds.right - bounds.left == FloatingMonitorLayout::Width(font) &&
                bounds.bottom - bounds.top == FloatingMonitorLayout::Height(font, cpu, memory) && g_engine == engine_identity,
                "CPU/memory modes must resize in place without another window, font shrink, or backend restart");
        }
    }
    g_renderer->SetFloatingMonitorFontSize(FloatingMonitorLayout::default_font_size);
    g_renderer->SetFloatingMonitorCpuShown(true);
    g_renderer->SetFloatingMonitorMemoryShown(true);
    UpdateFloatingMonitorState();
    FloatingTypographyPixels();
    const HWND taskbar_widget = g_widget_hwnd;
    ShowWindow(g_main_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_main_hwnd); // Real dashboard target/brushes must exist before close.
    Check(g_renderer->MainResourcesInitializedForTest(), "Interactive restore must lazily initialize the real dashboard");
    SendMessageW(g_main_hwnd, WM_CLOSE, 0, 0);
    Pump();
    Check(IsWindow(g_main_hwnd) && !IsWindowVisible(g_main_hwnd) &&
        g_widget_hwnd == taskbar_widget && IsWindowVisible(taskbar_widget) &&
        g_floating_monitor_hwnd == floating && IsWindowVisible(floating),
        "Close must hide only the dashboard and retain both enabled widget windows");
    const auto taskbar_before_sample = g_renderer->WidgetSubmissionsForTest();
    const auto floating_before_sample = g_renderer->FloatingSubmissionsForTest();
    g_has_display_dashboard_snapshot = true;
    g_display_dashboard_snapshot.sample_sequence = 42;
    g_display_dashboard_snapshot.download_speed_bps = 8192000;
    g_display_dashboard_snapshot.upload_speed_bps = 4096000;
    g_display_dashboard_snapshot.cpu_sample_available = true;
    g_display_dashboard_snapshot.cpu_sampled_at_ms = GetTickCount64();
    g_display_dashboard_snapshot.cpu_usage_percent = 24;
    g_display_dashboard_snapshot.ram_sample_available = true;
    g_display_dashboard_snapshot.ram_sampled_at_ms = GetTickCount64();
    g_display_dashboard_snapshot.ram_usage_percent = 58;
    g_display_dashboard_snapshot.ram_total_bytes = 8ULL * 1024 * 1024 * 1024;
    InvalidateRect(taskbar_widget, nullptr, FALSE); UpdateWindow(taskbar_widget);
    InvalidateRect(floating, nullptr, FALSE); UpdateWindow(floating);
    Check(g_renderer->WidgetSubmissionsForTest() > taskbar_before_sample &&
        g_renderer->FloatingSubmissionsForTest() > floating_before_sample && g_engine == engine_identity,
        "Both widgets must accept new readings after main-window graphics are released");
    ShowWindow(g_main_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_main_hwnd);
    ShowWindow(g_main_hwnd, SW_MINIMIZE);
    Check(IsWindowVisible(floating) && IsWindowVisible(taskbar_widget),
        "Minimizing the dashboard must not minimize either independent widget");
    ShowWindow(g_main_hwnd, SW_RESTORE);
    UpdateWindow(g_main_hwnd);
    Check(IsWindowVisible(g_main_hwnd) && g_renderer->MainPaintsForTest() >= 2,
        "Restoring the same dashboard must recreate main graphics without replacing widgets");
    // Paint the production speed card at minimum supported size, not just its
    // formatting helper. Running/upload-95% and expanded failure states fit;
    // ordinary user scrolling must still override following the result card.
    std::cout << "Native recovery: speed card phase rendering and full result visibility" << std::endl;
    SetWindowPos(g_main_hwnd, nullptr, 50, 50, 460, 650, SWP_NOZORDER | SWP_NOACTIVATE);
    g_renderer->SetActiveTab(ActiveTab::Network);
    g_renderer->FollowSpeedCardForTest();
    SpeedTestSnapshot speed;
    NetworkSnapshot network;
    for (const auto phase : {SpeedTestPhase::Download, SpeedTestPhase::Upload, SpeedTestPhase::Finishing, SpeedTestPhase::Failed}) {
        speed.phase = phase; speed.progress = 95; speed.live_mbps = 123.4;
        if (phase != SpeedTestPhase::Download) {
            speed.result.download_state = network_test::TransferState::Complete;
            speed.result.download_speed_mbps = 81.6;
        }
        if (phase == SpeedTestPhase::Failed) {
            speed.result.upload_state = network_test::TransferState::Failed;
            speed.result.error_message = L"Owned regression failure";
        }
        g_renderer->Render(g_main_hwnd, g_display_dashboard_snapshot, network, {}, {}, speed, {});
        Check(g_renderer->SpeedCardVisibleForTest(), "Live speed card and expanded final notice must be fully inside the real content clip");
    }
    g_renderer->FollowSpeedCardForTest();
    speed.phase = SpeedTestPhase::Upload;
    g_renderer->OnMouseWheel(30 * WHEEL_DELTA);
    g_renderer->Render(g_main_hwnd, g_display_dashboard_snapshot, network, {}, {}, speed, {});
    Check(!g_renderer->SpeedCardVisibleForTest(), "Automatic result reveal must not fight explicit user wheel navigation");
    Sleep(200);
    g_renderer->Render(g_main_hwnd, g_display_dashboard_snapshot, network, {}, {}, speed, {});
    Check(!g_renderer->NeedsAnimationFrame(), "Settled Network content must stop full-cadence animation paints");
    g_renderer->SetActiveTab(ActiveTab::Dashboard);

    WatchmanTrayReadyForTest(false);
    SendMessageW(g_main_hwnd, WM_CLOSE, 0, 0);
    Check(IsWindowVisible(g_main_hwnd) && IsWindowVisible(floating) && IsWindowVisible(taskbar_widget),
        "Tray failure must keep a usable dashboard, without stopping or hiding widgets");
    WatchmanTrayReadyForTest(true);
    SendMessageW(g_main_hwnd, WM_CLOSE, 0, 0);

    std::cout << "Native recovery: display/DPI/resume burst handling" << std::endl;
    // Bursts are delayed/coalesced by the actual production handler. A retained
    // HWND still needs a fresh target/frame after DPI/display/sleep recovery.
    const auto before_display = g_renderer->WidgetSubmissionsForTest();
    for (int notification = 0; notification < 20; ++notification)
        SendMessageW(g_main_hwnd, WM_DISPLAYCHANGE, 32, MAKELPARAM(1280, 720));
    Check(g_renderer->WidgetSubmissionsForTest() == before_display,
        "Display notification bursts must not synchronously rebuild the renderer per message");
    Sleep(250);
    SendMessageW(g_main_hwnd, WM_TIMER, 3, 0);
    Check(g_renderer->WidgetSubmissionsForTest() > before_display && g_engine == engine_identity &&
        !IsWindowVisible(g_main_hwnd) && IsWindowVisible(floating),
        "Coalesced hidden-dashboard display recovery must repaint widgets without restarting monitoring");
    const RECT suggested{120, 120, 600, 340};
    SendMessageW(g_main_hwnd, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&suggested));
    RECT actual{}; GetWindowRect(g_main_hwnd, &actual);
    Check(EqualRect(&actual, &suggested), "DPI handler must apply Windows' suggested main-window rectangle");
    const auto before_resume = g_renderer->WidgetSubmissionsForTest();
    SendMessageW(g_main_hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    Sleep(250); SendMessageW(g_main_hwnd, WM_TIMER, 3, 0);
    Check(g_renderer->WidgetSubmissionsForTest() > before_resume,
        "Resume recovery must invalidate even a byte-for-byte identical cached widget frame");

    g_renderer->SetWidgetEnabled(false);
    UpdateWidgetState();
    Expect(Status::Disabled, "Disabled status must be explicit");
    DestroyWindow(g_widget_hwnd);
    Pump();
    Check(g_widget_hwnd == nullptr, "Disabled widgets must not be recreated by queued destruction recovery");
    g_renderer->SetWidgetEnabled(true);
    UpdateWidgetState();
    ExpectFrame("Re-enabling the widget must create an independent new host");

    // Faults exist only in this test binary. Test real GDI UpdateLayeredWindow
    // success as well as a captured API failure and a latched GPU fallback.
    g_renderer->ResetWidgetHost();
    g_renderer->SetWidgetFaultsForTest(true, ERROR_ACCESS_DENIED);
    const auto attempts = g_renderer->CompositionAttemptsForTest();
    UpdateWidgetState(true);
    Expect(Status::RenderFailed, "Failure of both paths must not be reported as a displayed frame");
    auto paint = g_renderer->GetWidgetPaintResult();
    Check(!paint.success && !paint.submitted && paint.error == ERROR_ACCESS_DENIED && paint.backend == Backend::Gdi,
        "Rejected GDI presentation must keep the immediate API error");
    for (int retry = 0; retry < 3; ++retry) UpdateWidgetState(true);
    Check(g_renderer->CompositionAttemptsForTest() == attempts + 1, "Repeated GDI failures must not thrash D3D initialization");
    g_renderer->SetWidgetFaultsForTest(true, 0);
    UpdateWidgetState(true);
    ExpectFrame("GDI retries must recover when Windows accepts frames again");
    Check(g_renderer->GetWidgetPaintResult().backend == Backend::Gdi,
        "Successful fallback must be distinguished from DirectComposition success");
    Check(g_renderer->CompositionAttemptsForTest() == attempts + 1, "GDI recovery must retain the fallback latch");
    const auto before_selection_failure = g_renderer->WidgetSubmissionsForTest();
    g_renderer->SetWidgetFaultsForTest(true, 0, ERROR_NOT_ENOUGH_MEMORY);
    UpdateWidgetState(true);
    Expect(Status::RenderFailed, "Failed GDI bitmap selection must not submit an empty or stale frame");
    Check(g_renderer->GetWidgetPaintResult().error == ERROR_NOT_ENOUGH_MEMORY &&
        g_renderer->WidgetSubmissionsForTest() == before_selection_failure,
        "Selection failure must keep its immediate error without counting a presentation");
    g_renderer->SetWidgetFaultsForTest(true);
    UpdateWidgetState(true);
    ExpectFrame("GDI selection cleanup must permit a later successful frame");
    Check(g_renderer->CompositionAttemptsForTest() == attempts + 1, "Resource failure must not clear the fallback latch");
    std::cout << "Native recovery: controlled real GDI-to-composition retry" << std::endl;
    // A healthy fallback gets a bounded retry without needing Explorer restart.
    for (int frame = 0; frame < 3; ++frame) UpdateWidgetState(true);
    const uint64_t retry_at = g_renderer->WidgetGpuRetryAtForTest();
    g_renderer->SetWidgetTimeForTest(retry_at - 1);
    UpdateWidgetState(true);
    Check(g_renderer->CompositionAttemptsForTest() == attempts + 1, "GPU retry must respect its initial cooldown");
    g_renderer->AllowWidgetGpuRetry(false);
    g_renderer->SetWidgetTimeForTest(retry_at);
    // Direct render: production blocks a retry during speed tests; the owned
    // unstarted engine in this test otherwise always reports a quiet UI.
    g_renderer->RenderWidget(g_widget_hwnd, g_display_dashboard_snapshot);
    Check(g_renderer->CompositionAttemptsForTest() == attempts + 1, "Busy transfer UI must defer a GPU retry");
    g_renderer->AllowWidgetGpuRetry(true);
    UpdateWidgetState(true);
    ExpectFrame("Failed scheduled GPU probe must restore and submit GDI within the same paint");
    Check(g_renderer->CompositionAttemptsForTest() == attempts + 2 &&
        g_renderer->GetWidgetPaintResult().backend == Backend::Gdi &&
        g_renderer->WidgetGpuRetryAtForTest() >= retry_at + 60000,
        "Repeated GPU failures need exponential cooldown, not per-frame initialization");
    const auto next_gpu = g_renderer->WidgetGpuRetryAtForTest();
    for (int frame = 0; frame < 3; ++frame) UpdateWidgetState(true);
    g_renderer->SetWidgetFaultsForTest(false);
    g_renderer->SetWidgetTimeForTest(next_gpu);
    UpdateWidgetState(true);
    ExpectFrame("Scheduled retry with the real compositor restored must preserve a usable frame");
    if (gpu) Check(g_renderer->GetWidgetPaintResult().backend == Backend::Composition,
        "When a real GPU path worked earlier, a later successful trial must return from GDI to it");
    g_renderer->SetWidgetTimeForTest(0);

    // The legacy reservation/rollback path must not double-shrink or overlap.
    Check(tm_reset_taskbar_binding() != 0, "Could not finish modern shell cleanup");
    shell.reset();
    shell = std::make_unique<FakeShell>(true);
    ShellEvent();
    ExpectFrame("Legacy placement must also render");
    const int reserved_width = Width(shell->list);
    Check(reserved_width < 900 && reserved_width > 0, "Legacy task list must reserve exactly one widget slot");
    for (int retry = 0; retry < 4; ++retry) UpdateWidgetState();
    Check(Width(shell->list) == reserved_width, "Repeated checks must not shorten the task list again");
    g_renderer->SetWidgetEnabled(false);
    UpdateWidgetState();
    Check(Width(shell->list) == 900, "Disabling must restore the original task-list extent");
    g_renderer->SetWidgetEnabled(true);
    tm_fail_next_position_for_test(ERROR_INVALID_PARAMETER, 1); // Fail after reservation, while moving widget.
    UpdateWidgetState();
    Expect(Status::AttachFailed, "Failed widget position after reservation must report attachment failure");
    Check(Width(shell->list) == 900 && !IsWindowVisible(g_widget_hwnd), "Partial reservation must roll back without overlap");
    UpdateWidgetState();
    ExpectFrame("Legacy reservation failure must be retryable");
    const int reserved = Width(shell->list);
    SetWindowPos(shell->list, nullptr, 0, 0, 603, 48, SWP_NOZORDER);
    g_renderer->SetWidgetEnabled(false);
    UpdateWidgetState();
    Expect(Status::CleanupPending, "Unknown Explorer geometry must remain cleanup-pending, never become a new baseline");
    Check(Width(shell->list) == 603, "Ambiguous geometry must not be guessed or expanded");
    SetWindowPos(shell->list, nullptr, 0, 0, reserved, 48, SWP_NOZORDER);
    UpdateWidgetState();
    Check(Width(shell->list) == 900, "Known reservation geometry must finish pending cleanup");

    g_renderer->SetWidgetEnabled(true);
    UpdateWidgetState();
    ExpectFrame("Widget should recover after delayed reservation cleanup");
    Check(g_engine == engine_identity && !engine.IsReady(), "Owned-window tests must not start or replace monitoring services");
    UpdateNoticeUiFixture(engine);
    g_renderer->ReleaseMainWindowResources();
    g_renderer->SetMainTimeForTest(30000);
    g_renderer->SetMainGraphicsFaultsForTest(S_OK, E_FAIL);
    g_renderer->Render(g_main_hwnd, {}, {}, {}, {}, {}, {});
    Check(g_renderer->MainGraphicsUnavailable(), "Explicit Exit fixture needs a real fallback surface");
    WatchmanTrayReadyForTest(false);
    g_renderer->OnLButtonDown(g_main_hwnd, 100, 257, engine); // Actual fallback Exit, even with no tray.
    Pump();
    WatchmanJoinTrayForTest();
    UpdateWidgetState(true);
    Check(g_widget_hwnd == nullptr && tm_input_widget_for_test() == nullptr && !IsWindow(floating),
        "Explicit Exit must destroy both widgets and suppress their recreation/input interception");
    Check(Width(shell->list) == 900, "Shutdown must restore the taskbar reservation");
    g_main_hwnd = nullptr;
    g_engine = nullptr;
    g_renderer = nullptr;
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto log_root = std::filesystem::path(temp) / (L"Watchman-recovery-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(log_root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", log_root.c_str());
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComScope { HRESULT result; ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } scope{com};
    WatchmanFonts::GdiRegistration fonts;
    try {
        Check(SUCCEEDED(com), "COM initialization failed");
        Check(IsWindows10OrGreater(), "The app and native test need the shared Windows 10/11 compatibility manifest for layered child windows");
        Check(SUCCEEDED(fonts.Initialize(GetModuleHandleW(nullptr))), "Bundled GDI fonts must load for native fallback tests");
        Exercise();
        std::cout << "Native startup/recovery checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        // Do not dispatch more messages with pointers to unwound test objects.
        g_renderer = nullptr;
        g_engine = nullptr;
        WatchmanJoinTrayForTest();
        std::cerr << "Native recovery check " << checks << " failed: " << error.what() << '\n';
        return 1;
    }
}
