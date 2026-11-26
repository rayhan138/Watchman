#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "floating_monitor_layout.h"
#include "taskbar_widget_layout.h"

// A small, offline introduction inside the existing EXE. It is not an
// installer, an elevation prompt, or a set of live monitoring controls.
namespace WatchmanWelcome {
inline constexpr uint32_t version = 1;
inline constexpr int page_count = 3;
inline constexpr int text_count = 5;
inline constexpr float title_font_size = 24.0f;
inline constexpr float button_font_size = 13.0f;
enum Text { Title, Description, TipTitle, TipBody, Footnote };
struct Page { std::array<const wchar_t*, text_count> text; };
inline constexpr Page pages[page_count] = {
    {{{L"Welcome to Watchman",
       L"Double-click the taskbar widget to open Watchman and your recent history.",
       L"A monitor you can move",
       L"Enable Floating Monitor in Settings > Widgets. Drag when unlocked; right-click for options.",
       L"Double-click the floating monitor to open Watchman, too."}}},
    {{{L"Close it. Keep monitoring.",
       L"Closing the window keeps Watchman running in the system tray.",
       L"Right-click the tray icon",
       L"Open Watchman, show or hide either monitor, restart, or choose Exit to quit.",
       L"Can't see the icon? Check the hidden-icons arrow by the clock."}}},
    {{{L"See each app's bandwidth",
       L"Run Watchman as administrator to see each app's upload and download speed.",
       L"How to turn it on",
       L"Open Apps, click Run as administrator, and approve the Windows prompt.",
       L"The taskbar and floating monitors work without administrator access."}}}
};

enum class Control { None, Skip, Back, Next };
inline bool IsInteractiveLaunch(bool startup, uint32_t replace_pid) { return !startup && replace_pid == 0; }

class Tour {
public:
    void LoadSeenVersion(uint32_t value) { seen_version_ = value; }
    uint32_t SeenVersion() const { return seen_version_; }
    bool IsOpen() const { return open_; }
    int PageIndex() const { return page_; }
    Control Focused() const { return focused_; }
    bool CanGoBack() const { return page_ > 0; }
    const wchar_t* NextLabel() const { return page_ + 1 == page_count ? L"Get started" : L"Next"; }

    // Quiet sign-in/replacement launches must not steal focus or consume the
    // welcome. Offer it at the next deliberate, visible opening instead.
    bool Offer(bool interactive) {
        if (!interactive || open_ || offered_ || seen_version_ >= version) return false;
        Open();
        return true;
    }
    void Open() { open_ = true; offered_ = true; page_ = 0; focused_ = Control::Next; }

    // Returns true only when a deliberate Skip/Get started needs persisting.
    bool Activate(Control control) {
        if (!open_) return false;
        if (control == Control::Skip || (control == Control::Next && page_ + 1 == page_count)) {
            open_ = false;
            seen_version_ = (std::max)(seen_version_, version);
            return true;
        }
        if (control == Control::Next) { ++page_; focused_ = Control::Next; }
        else if (control == Control::Back && CanGoBack()) { --page_; focused_ = Control::Back; }
        if (!CanGoBack() && focused_ == Control::Back) focused_ = Control::Next;
        return false;
    }
    void MoveFocus(bool backwards) {
        if (!open_) return;
        const Control controls[] = {Control::Skip, Control::Back, Control::Next};
        int index = focused_ == Control::Skip ? 0 : focused_ == Control::Back ? 1 : 2;
        do { index = (index + (backwards ? 2 : 1)) % 3; }
        while (controls[index] == Control::Back && !CanGoBack());
        focused_ = controls[index];
    }
private:
    uint32_t seen_version_ = 0;
    bool open_ = false, offered_ = false;
    int page_ = 0;
    Control focused_ = Control::Next;
};

struct Rect { float left = 0, top = 0, right = 0, bottom = 0; };
inline float Width(const Rect& rect) { return rect.right - rect.left; }
inline float Height(const Rect& rect) { return rect.bottom - rect.top; }
inline bool Contains(const Rect& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}
struct Layout {
    Rect card, brand, step, illustration, body, skip, back, next;
};
inline Layout GetLayout(float width, float height) {
    const float card_width = (std::min)(480.0f, (std::max)(1.0f, width - 32.0f));
    const float card_height = (std::min)(550.0f, (std::max)(1.0f, height - 32.0f));
    const float x = (width - card_width) / 2, y = (height - card_height) / 2;
    const float pad = card_width < 380 ? 20.0f : 28.0f;
    const float right = x + card_width - pad, left = x + pad;
    const float art_height = card_height >= 500 ? 90.0f : card_height >= 400 ? 48.0f : 0.0f;
    Layout layout;
    layout.card = {x, y, x + card_width, y + card_height};
    layout.brand = {left, y + 22, right - 76, y + 46};
    layout.step = {left, y + 50, right - 76, y + 68};
    layout.skip = {right - 60, y + 16, right, y + 52};
    layout.illustration = {left, y + 80, right, y + 80 + art_height};
    layout.body = {left, layout.illustration.bottom + 20, right, layout.card.bottom - 94};
    layout.back = {left, layout.card.bottom - 66, left + 80, layout.card.bottom - 24};
    layout.next = {right - 132, layout.back.top, right, layout.back.bottom};
    return layout;
}
// Sample readings are explicitly labelled, never presented as live telemetry.
inline constexpr wchar_t sample_upload[] = L"1.2 MB/s";
inline constexpr wchar_t sample_download[] = L"8.6 MB/s";
inline constexpr wchar_t sample_cpu[] = L"24%";
inline constexpr wchar_t sample_memory[] = L"58%";
inline constexpr wchar_t taskbar_caption[] = L"Taskbar (sample)";
inline constexpr wchar_t floating_caption[] = L"Floating (sample)";
struct MonitorPreviews {
    Rect taskbar_label, taskbar, floating_label, floating;
    float scale = 0;
};
inline MonitorPreviews GetMonitorPreviews(const Rect& area) {
    const auto taskbar = TaskbarWidgetLayout::PreferredSize(TaskbarWidgetMode::All, false, 36, 96);
    const int floating_width = FloatingMonitorLayout::Width(FloatingMonitorLayout::default_font_size);
    const float group_width = static_cast<float>(taskbar.width + 20 + floating_width);
    const float group_height = (std::max)(24.0f + taskbar.height, 26.0f + FloatingMonitorLayout::Height());
    const float scale = (std::max)(0.0f, (std::min)({1.0f, (Width(area) - 16) / group_width, Height(area) / (group_height + 8)}));
    const float left = (area.left + area.right - group_width * scale) / 2;
    const float top = (area.top + area.bottom - group_height * scale) / 2;
    const auto bounds = [&](float x, float y, float w, float h) {
        return Rect{left + x * scale, top + y * scale, left + (x + w) * scale, top + (y + h) * scale};
    };
    const float floating_x = static_cast<float>(taskbar.width + 20);
    return {bounds(0, 0, static_cast<float>(taskbar.width), 18),
        bounds(0, 24, static_cast<float>(taskbar.width), static_cast<float>(taskbar.height)),
        bounds(floating_x, 0, static_cast<float>(floating_width), 18),
        bounds(floating_x, 26, static_cast<float>(floating_width), static_cast<float>(FloatingMonitorLayout::Height())), scale};
}
inline Control HitTest(const Layout& layout, int page, float x, float y) {
    if (Contains(layout.skip, x, y)) return Control::Skip;
    if (page > 0 && Contains(layout.back, x, y)) return Control::Back;
    if (Contains(layout.next, x, y)) return Control::Next;
    return Control::None;
}
// Fits the existing Settings header without adding another scrolling card.
inline Rect ReplayButton(float modal_left, float modal_top, float modal_width) {
    return {modal_left + modal_width - 142, modal_top + 18,
            modal_left + modal_width - 56, modal_top + 48};
}
inline float TextWidth(const Layout& layout, int text) {
    return (std::max)(1.0f, Width(layout.body) - ((text == TipTitle || text == TipBody) ? 32.0f : 0.0f));
}
struct Content {
    std::array<Rect, text_count> text{};
    Rect tip_card;
    float scroll = 0, scroll_max = 0;
};
inline Content GetContent(const Layout& layout, const std::array<float, text_count>& heights, float scroll) {
    Content content;
    float y = layout.body.top;
    const auto place = [&](int index, float left, float right) {
        const float h = (std::max)(0.0f, heights[static_cast<size_t>(index)]);
        content.text[static_cast<size_t>(index)] = {left, y, right, y + h};
        y += h;
    };
    place(Title, layout.body.left, layout.body.right); y += 10;
    place(Description, layout.body.left, layout.body.right); y += 18;
    content.tip_card = {layout.body.left, y, layout.body.right, 0}; y += 14;
    place(TipTitle, layout.body.left + 16, layout.body.right - 16); y += 6;
    place(TipBody, layout.body.left + 16, layout.body.right - 16); y += 14;
    content.tip_card.bottom = y; y += 14;
    place(Footnote, layout.body.left, layout.body.right);
    content.scroll_max = (std::max)(0.0f, y - layout.body.bottom);
    content.scroll = std::clamp(scroll, 0.0f, content.scroll_max);
    for (auto& rect : content.text) { rect.top -= content.scroll; rect.bottom -= content.scroll; }
    content.tip_card.top -= content.scroll; content.tip_card.bottom -= content.scroll;
    return content;
}
} // namespace WatchmanWelcome
