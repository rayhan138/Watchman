#include "welcome_tour.h"
#include "runtime_policy.h"

#include <cmath>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace WatchmanWelcome;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a - b) < 0.02f; }
bool Inside(const Rect& inner, const Rect& outer) {
    return inner.left >= outer.left && inner.right <= outer.right && inner.top >= outer.top && inner.bottom <= outer.bottom;
}

void FirstRun() {
    for (const auto& args : {std::vector<std::wstring>{L"Watchman.exe"},
            {L"Watchman.exe", L"--startup"}, {L"Watchman.exe", L"--replace-pid", L"123"},
            {L"Watchman.exe", L"--replace-pid", L"123", L"--startup"}}) {
        const auto launch = WatchmanRuntime::ParseArguments(args);
        Tour tour;
        const bool interactive = IsInteractiveLaunch(launch.startup, launch.replace_pid);
        Check(tour.Offer(interactive) == (args.size() == 1),
            "Only an ordinary manual launch should immediately offer a first-run tour");
        if (!interactive) {
            Check(!tour.IsOpen() && tour.SeenVersion() == 0, "Quiet launch must not consume or finish the welcome");
            Check(tour.Offer(true), "An unseen tour must be offered on the next deliberate restore");
        }
        Check(tour.IsOpen() && tour.PageIndex() == 0, "A first offer must start at the first screen");
        Check(!tour.Offer(true) && tour.PageIndex() == 0, "Activation messages must not restart an open tour");
        Check(tour.Activate(Control::Skip), "Skip must explicitly acknowledge the welcome");
        Check(tour.SeenVersion() == version && !tour.IsOpen(), "Skip must remember completion without further steps");
        Check(!tour.Offer(true), "Skip must not be undone by a later focus or restore event");
        Tour next_process;
        next_process.LoadSeenVersion(tour.SeenVersion());
        Check(!next_process.Offer(true), "The persisted marker must suppress the next manual launch's automatic tour");
        next_process.Open();
        Check(next_process.IsOpen() && next_process.PageIndex() == 0, "Manual replay must work for an existing user");
    }
    Tour future;
    future.LoadSeenVersion(version + 5);
    Check(!future.Offer(true), "An older EXE must not nag a user with a newer completion marker");
    future.Open(); future.Activate(Control::Skip);
    Check(future.SeenVersion() == version + 5, "Replay must not downgrade a future completion marker");
}

void Navigation() {
    for (int skip_page = 0; skip_page < page_count; ++skip_page) {
        Tour tour; tour.Open();
        Check(!tour.CanGoBack(), "Back must be unavailable on the first page");
        tour.Activate(Control::Back); tour.Activate(Control::None);
        Check(tour.PageIndex() == 0 && tour.SeenVersion() == 0, "Inert actions must not finish or move the tour");
        for (int page = 0; page < skip_page; ++page) {
            Check(!tour.Activate(Control::Next) && tour.IsOpen(), "Next must stay inside the same welcome window");
        }
        Check(tour.PageIndex() == skip_page && tour.Activate(Control::Skip), "Skip must work from every screen");
        Check(!tour.Activate(Control::Next), "A stale input after closing must not reactivate a tour");
    }
    Tour tour; tour.Open();
    for (int page = 0; page < page_count; ++page) {
        Check(tour.PageIndex() == page, "Pages must be visited in order");
        Check(std::wcscmp(tour.NextLabel(), page + 1 == page_count ? L"Get started" : L"Next") == 0,
            "Only the final page should offer Get started");
        if (page == page_count - 1) {
            tour.Activate(Control::Back);
            Check(tour.PageIndex() == page - 1 && tour.SeenVersion() == 0, "Back must not acknowledge completion");
            tour.Activate(Control::Next);
        }
        Check(tour.Activate(Control::Next) == (page == page_count - 1),
            "Only Get started, not an intermediate Next, may write the seen marker");
    }
    Check(!tour.IsOpen() && tour.SeenVersion() == version, "Get started must finish on the current page count");
    tour.Open();
    for (int cycle = 0; cycle < 10; ++cycle) {
        Check(tour.Focused() == Control::Next, "First page's default keyboard target must be Next");
        tour.MoveFocus(false); Check(tour.Focused() == Control::Skip, "Tab must reach Skip");
        tour.MoveFocus(false); Check(tour.Focused() == Control::Next, "Tab must skip the absent Back button");
    }
    tour.MoveFocus(true); Check(tour.Focused() == Control::Skip, "Shift+Tab must cycle backwards");
    tour.Activate(Control::Next); tour.MoveFocus(true);
    Check(tour.Focused() == Control::Back, "Keyboard navigation must reach Back on subsequent pages");
    tour.Activate(tour.Focused());
    Check(tour.PageIndex() == 0 && tour.Focused() == Control::Next, "Returning to page one must not leave focus on a hidden control");
}

void PlainCopyAndRealMonitorSamples() {
    const std::wstring admin = pages[2].text[Description];
    Check(admin.find(L"as administrator") != std::wstring::npos && admin.find(L"each app") != std::wstring::npos
        && admin.find(L"upload") != std::wstring::npos && admin.find(L"download") != std::wstring::npos,
        "Apps page must plainly explain administrator access for each app's upload and download");
    const std::wstring instruction = pages[2].text[TipBody];
    Check(instruction.find(L"Apps") != std::wstring::npos && instruction.find(L"Run as administrator") != std::wstring::npos
        && instruction.find(L"If capture") == std::wstring::npos,
        "Apps instructions must name the button, not give a vague conditional permission tip");
    Check(std::wstring(taskbar_caption).find(L"sample") != std::wstring::npos
        && std::wstring(floating_caption).find(L"sample") != std::wstring::npos,
        "The realistic previews must be explicitly labelled as sample, not live, readings");
    Check(std::wstring(sample_upload).find(L"MB/s") != std::wstring::npos
        && std::wstring(sample_download).find(L"MB/s") != std::wstring::npos
        && std::wstring(sample_cpu).find(L"%") != std::wstring::npos
        && std::wstring(sample_memory).find(L"%") != std::wstring::npos,
        "Preview numbers must show real rate and percentage units");
    Check(FloatingMonitorLayout::default_text_color == 0x00FFFFFFu && FloatingMonitorLayout::panel_background_color == 0,
        "Floating samples and live defaults must remain white-on-black in Light and Dark");
    static_assert(sizeof(FloatingMonitorLayout::Layout) == 8 * sizeof(FloatingMonitorLayout::Rect),
        "Floating monitor supports upload/download and independent CPU/memory fields");
    for (int font = FloatingMonitorLayout::minimum_font_size; font <= FloatingMonitorLayout::maximum_font_size; ++font) {
        Check(FloatingMonitorLayout::Height(font) == 56, "Default sample must include CPU/memory height");
        Check(FloatingMonitorLayout::Height(font, false, false) == 28, "Network-only mode remains compact");
    }
}

void Geometry() {
    for (float width : {320.0f, 360.0f, 400.0f, 460.0f, 560.0f, 720.0f, 1000.0f}) {
        for (float height : {400.0f, 460.0f, 550.0f, 650.0f, 820.0f}) {
            const auto layout = GetLayout(width, height);
            const Rect window{0, 0, width, height};
            Check(Inside(layout.card, window), "Welcome must remain inside the available app window");
            for (const auto& bounds : {layout.brand, layout.step, layout.body, layout.skip, layout.back, layout.next}) {
                Check(Inside(bounds, layout.card) && Width(bounds) > 0 && Height(bounds) > 0,
                    "Welcome controls and copy must retain positive dimensions inside the card");
            }
            Check(layout.body.bottom + 28 <= layout.next.top && layout.back.right < layout.next.left,
                "Scrolling copy, Back and Next must not overlap");
            Check(Height(layout.next) >= 40 && Height(layout.back) >= 40, "Navigation buttons need comfortable click targets");
            Check(layout.brand.right < layout.skip.left && layout.step.right < layout.skip.left,
                "Branding and the page counter must leave space for Skip");
            Check(HitTest(layout, 0, layout.back.left + 2, layout.back.top + 2) == Control::None,
                "An invisible Back button must not be clickable");
            for (int page = 0; page < page_count; ++page) {
                Check(HitTest(layout, page, layout.skip.left + 1, layout.skip.top + 1) == Control::Skip &&
                      HitTest(layout, page, layout.next.left + 1, layout.next.top + 1) == Control::Next,
                    "Drawn Skip and Next controls must share their hit geometry");
                Check(HitTest(layout, page, 0, 0) == Control::None &&
                      HitTest(layout, page, layout.illustration.left, layout.illustration.top) == Control::None,
                    "Outside clicks and decorative illustrations must not activate anything");
            }
            if (Height(layout.illustration) > 0) {
                const auto previews = GetMonitorPreviews(layout.illustration);
                Check(previews.scale > 0 && previews.scale <= 1, "Monitor samples must scale together without enlarging text");
                for (const auto& bounds : {previews.taskbar_label, previews.taskbar, previews.floating_label, previews.floating}) {
                    Check(Inside(bounds, layout.illustration), "Both sample monitors and their captions must stay in the illustration");
                }
                const auto taskbar = TaskbarWidgetLayout::PreferredSize(TaskbarWidgetMode::All, false, 36, 96);
                Check(Near(Width(previews.taskbar), taskbar.width * previews.scale)
                    && Near(Height(previews.taskbar), taskbar.height * previews.scale),
                    "Taskbar sample must use the real preferred dimensions, not a placeholder diagram");
                Check(Near(Width(previews.floating), FloatingMonitorLayout::Width(FloatingMonitorLayout::default_font_size) * previews.scale)
                    && Near(Height(previews.floating), FloatingMonitorLayout::Height() * previews.scale),
                    "Floating sample must use the real default network/CPU/memory dimensions");
                Check(previews.taskbar_label.bottom < previews.taskbar.top && previews.floating_label.bottom < previews.floating.top
                    && previews.taskbar.right < previews.floating.left,
                    "Monitor samples and captions must remain clearly separated");
                if (width >= 460 && height >= 650) Check(Near(previews.scale, 1),
                    "Both monitors must be shown at their real text size on supported windows");
            }
            for (const auto heights : {std::array<float, text_count>{32, 36, 18, 52, 28},
                                      std::array<float, text_count>{64, 72, 36, 144, 56}}) {
                const auto top = GetContent(layout, heights, -100);
                const auto end = GetContent(layout, heights, 100000);
                Check(top.scroll == 0 && Near(top.text[Title].top, layout.body.top), "Tour copy must begin at the top of the viewport");
                Check(Near(end.scroll, end.scroll_max) && end.text[Footnote].bottom <= layout.body.bottom + 0.02f,
                    "All wrapped copy must be reachable above the fixed navigation buttons");
                Check(Inside(top.text[TipTitle], top.tip_card) && Inside(top.text[TipBody], top.tip_card),
                    "Wrapped floating/tray/admin tip must remain inside its own callout");
                for (int index = 1; index < text_count; ++index) Check(
                    top.text[static_cast<size_t>(index)].top > top.text[static_cast<size_t>(index - 1)].bottom,
                    "Measured welcome paragraphs must not overlap");
            }
        }
    }
    for (float modal_width : {360.0f, 400.0f, 440.0f, 500.0f}) {
        const auto replay = ReplayButton(10, 10, modal_width);
        Check(replay.right < 10 + modal_width - 40 && replay.left > 10 + 150,
            "Replay button must not overlap the Settings title or close button");
    }
}
} // namespace
int main() {
    try { FirstRun(); Navigation(); PlainCopyAndRealMonitorSamples(); Geometry(); std::cout << "Welcome tour state/layout tests passed (no app launch or preferences changed)\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
