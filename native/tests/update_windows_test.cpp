// No real notifications, release downloads or personal preferences. Payload
// construction, isolated Registry markers and real bundled-font measurement.
#include "update_preferences.h"
#include "update_notification.h"
#include "update_presentation.h"
#include "bundled_fonts.h"
#include "scoped_handle.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>

using namespace WatchmanUpdates;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
struct RegistryScope {
    std::wstring path = L"Software\\Watchman.Tests\\Updates-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Watchman::ScopedHKey key;
    RegistryScope() { Check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, key.Put(), nullptr) == ERROR_SUCCESS, "Could not create isolated update preferences"); }
    ~RegistryScope() { key.Reset(); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
};
void Fits(IDWriteFactory* factory, IDWriteTextFormat* format, const wchar_t* text, float width, float height) {
    ComPtr<IDWriteTextLayout> layout;
    Check(SUCCEEDED(factory->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)), format, width, 2000, &layout)), "Could not measure update text");
    DWRITE_TEXT_METRICS m{};
    Check(SUCCEEDED(layout->GetMetrics(&m)), "Could not read text metrics");
    if (m.widthIncludingTrailingWhitespace > width + 0.5f || m.height > height + 0.5f) {
        std::wcerr << L"Update text overflow: '" << text << L"', actual=" << m.widthIncludingTrailingWhitespace << L"x" << m.height << L", area=" << width << L"x" << height << L'\n';
        Check(false, "Update text must fit without clipping");
    }
    Check(true, "Measured text fits");
}
void Test() {
    RegistryScope scope;
    Check(ReadMarker(scope.key.Get(), notified_value).empty(), "Missing marker must not suppress a new notice");
    Check(WriteMarker(scope.key.Get(), notified_value, "2.0.2") == ERROR_SUCCESS, "Could not persist notification marker");
    Check(ReadMarker(scope.key.Get(), notified_value) == "2.0.2", "Could not read accepted notification marker");
    Check(ReadMarker(scope.key.Get(), dismissed_value).empty(), "Windows notification acceptance must not dismiss the in-app prompt");
    Check(WriteMarker(scope.key.Get(), dismissed_value, "2.0.2") == ERROR_SUCCESS, "Could not persist prompt dismissal");
    Check(WriteMarker(scope.key.Get(), dismissed_value, "2.0.2") == ERROR_SUCCESS, "Repeated marker write must be idempotent");
    for (const auto* bad : {"v2.0.2", "2.00.2", "2.0.2.0", "garbage", "1.2.3-beta", "https://evil.example"})
        Check(WriteMarker(scope.key.Get(), dismissed_value, bad) == ERROR_INVALID_DATA, "Do not persist arbitrary/noncanonical feed data");
    DWORD dword = 42;
    Check(RegSetValueExW(scope.key.Get(), notified_value, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dword), sizeof(dword)) == ERROR_SUCCESS, "Could not seed corrupt type");
    Check(ReadMarker(scope.key.Get(), notified_value).empty(), "Wrong-type marker must be ignored");
    const wchar_t embedded[] = L"2.0.2\0evil";
    RegSetValueExW(scope.key.Get(), notified_value, 0, REG_SZ, reinterpret_cast<const BYTE*>(embedded), sizeof(embedded));
    Check(ReadMarker(scope.key.Get(), notified_value).empty(), "Embedded NUL must not be accepted as a canonical marker");
    Check(WriteMarker(scope.key.Get(), notified_value, "65535.65535.65535.65535") == ERROR_SUCCESS &&
        ReadMarker(scope.key.Get(), notified_value) == "65535.65535.65535.65535", "Full resource-range marker must round trip");
    const auto payload = Notification(nullptr, 17, {{2, 10, 0, 1}, "v2.10.0.1"}, {1, 0, 5, 0});
    Check(payload.uID == 17 && payload.uFlags == NIF_INFO, "Use the existing app tray identity, not a new elevated notification source");
    Check((payload.dwInfoFlags & NIIF_RESPECT_QUIET_TIME) != 0, "Respect Windows quiet time");
    Check(std::wstring(payload.szInfoTitle) == L"Watchman update available", "Native update title incorrect");
    Check(std::wstring(payload.szInfo).find(L"Version 2.10.0.1") != std::wstring::npos && std::wstring(payload.szInfo).find(L"1.0.5") != std::wstring::npos,
        "Native Windows payload must contain genuine available and installed versions");
    ComPtr<IDWriteFactory> factory;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf()))), "DirectWrite initialization failed");
    WatchmanFonts::Collection fonts;
    Check(SUCCEEDED(fonts.Initialize(factory.Get())), "Bundled update fonts failed");
    ComPtr<IDWriteTextFormat> footer, title, version, body;
    Check(SUCCEEDED(fonts.CreateTextFormat(L"Inter", DWRITE_FONT_WEIGHT_SEMI_BOLD, Presentation::footer_font_size, &footer)), "Footer font failed");
    Check(SUCCEEDED(fonts.CreateTextFormat(L"Inter", DWRITE_FONT_WEIGHT_BOLD, 24, &title)), "Title font failed");
    Check(SUCCEEDED(fonts.CreateTextFormat(L"Inter", DWRITE_FONT_WEIGHT_SEMI_BOLD, 18, &version)), "Version font failed");
    Check(SUCCEEDED(fonts.CreateTextFormat(L"Inter", DWRITE_FONT_WEIGHT_NORMAL, 14, &body)), "Body font failed");
    footer->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    version->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    for (const float width : {460.0f, 480.0f, 520.0f, 560.0f, 960.0f}) {
        const auto l = Presentation::GetLayout(width, 650);
        const auto f = Presentation::FooterLink(width, 650);
        Fits(factory.Get(), footer.Get(), Presentation::FooterText(width), f.right - f.left, f.bottom - f.top);
        Fits(factory.Get(), title.Get(), L"Update available", l.title.right - l.title.left, l.title.bottom - l.title.top);
        Fits(factory.Get(), version.Get(), L"Version 65535.65535.65535.65535", l.version.right - l.version.left, l.version.bottom - l.version.top);
        Fits(factory.Get(), body.Get(), Presentation::body_text, l.body.right - l.body.left, l.body.bottom - l.body.top);
        Fits(factory.Get(), body.Get(), L"Couldn't open your browser. Visit github.com/rayhan138/Watchman/releases to download the update.", l.body.right - l.body.left, l.body.bottom - l.body.top);
    }
    std::cout << "Isolated update preferences/native payload/bundled font checks: " << checks << " PASS; no notification sent\n";
}
}
int main() {
    try { Test(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
