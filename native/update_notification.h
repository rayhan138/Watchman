#pragma once

#include <windows.h>
#include <shellapi.h>
#include "update_policy.h"

namespace WatchmanUpdates {
inline NOTIFYICONDATAW Notification(HWND owner, UINT icon, const Release& release, const Version& installed) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = icon;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcscpy_s(data.szInfoTitle, L"Watchman update available");
    const std::wstring text = L"Version " + VersionText(release.version) + L" is available.\nInstalled: " +
        VersionText(installed) + L". Open Watchman to download.";
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    return data;
}
// A successful Shell_NotifyIcon call means Windows accepted the request, not
// that the person saw it. Focus Assist/notification settings may suppress it.
inline bool SendNotification(HWND owner, UINT icon, const Release& release, const Version& installed) {
    auto data = Notification(owner, icon, release, installed);
    return Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
}
} // namespace WatchmanUpdates
