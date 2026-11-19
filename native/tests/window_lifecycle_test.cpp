#include "window_lifecycle.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct State {
    bool tray_ready = true;
    unsigned destroyed = 0;
    unsigned timer_ticks = 0;
};
LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state) {
        if (WatchmanWindowLifecycle::HandleMessage(window, message, state->tray_ready)) return 0;
        if (message == WM_TIMER) { ++state->timer_ticks; return 0; }
        if (message == WM_DESTROY) { ++state->destroyed; return 0; }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
struct Window {
    HWND handle = nullptr;
    ~Window() { if (handle && IsWindow(handle)) DestroyWindow(handle); }
};
bool HasVisibleStyle(HWND window) {
    return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
}
}

int main() {
    try {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        cls.lpfnWndProc = TestWindowProc;
        cls.hInstance = instance;
        cls.lpszClassName = L"WatchmanLifecycleTestWindow";
        Check(RegisterClassW(&cls) != 0, "Could not register test window class");
        // A hidden parent keeps all child tests off-screen and noninteractive.
        // No Watchman engine, tray icon, Registry or history database is created.
        Window parent{CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
            0, 0, 10, 10, nullptr, nullptr, instance, nullptr)};
        Check(parent.handle != nullptr, "Could not create hidden test parent");
        State state;
        Window window{CreateWindowExW(0, cls.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 1, 1, parent.handle, nullptr, instance, &state)};
        Check(window.handle != nullptr && HasVisibleStyle(window.handle), "Test child must start logically visible");
        Check(!IsWindowVisible(window.handle), "Test windows must never be visible on the desktop");
        Check(SetTimer(window.handle, 1, 10, nullptr) != 0, "Could not create background test timer");

        SendMessageW(window.handle, WM_CLOSE, 0, 0);
        Check(IsWindow(window.handle) && state.destroyed == 0, "X must not destroy the window");
        Check(!HasVisibleStyle(window.handle), "X must hide the main window");
        SendMessageW(window.handle, WM_CLOSE, 0, 0);
        Check(IsWindow(window.handle) && !HasVisibleStyle(window.handle), "Repeated close must remain a tray hide");
        const ULONGLONG deadline = GetTickCount64() + 1000;
        while (state.timer_ticks == 0 && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, window.handle, WM_TIMER, WM_TIMER, PM_REMOVE)) DispatchMessageW(&message);
            Sleep(5);
        }
        Check(state.timer_ticks > 0, "Background timer must continue after close-to-tray");

        ShowWindow(window.handle, SW_SHOWNOACTIVATE);
        Check(HasVisibleStyle(window.handle), "Tray open must be able to restore a hidden window");
        SendMessageW(window.handle, WM_SYSCOMMAND, SC_CLOSE, 0);
        Check(IsWindow(window.handle) && !HasVisibleStyle(window.handle), "System close / Alt+F4 must also hide");

        state.tray_ready = false;
        SendMessageW(window.handle, WM_CLOSE, 0, 0);
        Check(IsWindow(window.handle) && HasVisibleStyle(window.handle) && state.destroyed == 0,
            "Tray creation failure must keep the window accessible, not quit or strand it");
        state.tray_ready = true;
        SendMessageW(window.handle, WM_CLOSE, 0, 0);
        Check(!HasVisibleStyle(window.handle), "Closing may hide once tray recovery succeeds");
        state.tray_ready = false;
        SendMessageW(window.handle, WatchmanWindowLifecycle::ExitMessage, 0, 0);
        Check(!IsWindow(window.handle) && state.destroyed == 1,
            "Explicit Exit / approved admin restart must destroy even with no tray");
        std::cout << "Window lifecycle checks passed (hidden test windows only; Watchman not launched)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
