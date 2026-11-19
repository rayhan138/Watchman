#pragma once

#include <windows.h>

namespace WatchmanWindowLifecycle {
// Deliberate Exit and an approved administrator restart must not become tray hides.
inline constexpr UINT ExitMessage = WM_APP + 14;

inline bool HandleMessage(HWND window, UINT message, bool tray_ready) {
    switch (message) {
    case WM_CLOSE:
        // Never leave an invisible process without a usable tray recovery path.
        ShowWindow(window, tray_ready ? SW_HIDE : SW_RESTORE);
        return true;
    case ExitMessage:
        DestroyWindow(window);
        return true;
    default:
        return false;
    }
}
} // namespace WatchmanWindowLifecycle
