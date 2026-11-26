#include "update_policy.h"
#include "update_presentation.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace WatchmanUpdates;
namespace {
unsigned checks = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
std::string Feed(std::string tag = "v2.0.2", std::string extra = {}) {
    return "{\"tag_name\":\"" + tag + "\",\"html_url\":\"" + release_prefix + tag +
        "\",\"draft\":false,\"prerelease\":false" + extra + "}";
}
void Test() {
    const Version current{2, 0, 1, 0};
    for (const auto* valid : {"0.0.0", "v2.0.2", "V2.0.2", "12.34.56.7", "65535.65535.65535.65535"})
        Check(ParseVersion(valid).has_value(), "Valid bounded numeric version rejected");
    for (const auto* invalid : {"", "v", "1", "1.2", "1.2.3.4.5", "1.2.3.", "1..3", ".1.2.3", "01.2.3", "1.02.3", "1.2.003", "-1.2.3", "+1.2.3", "1.2.3-beta", "v1.2.3+build", "1.2.65536", "99999999999.1.1", " 1.2.3", "1.2.3 ", "1.2.3\n", "vv1.2.3", "1.2.3/evil", "1.2.3?x=1"})
        Check(!ParseVersion(invalid), "Unsafe/non-release tag accepted");
    Check(Newer({2, 10, 0, 0}, {2, 9, 99, 0}), "Versions must be compared numerically");
    Check(Newer({2, 0, 1, 1}, current), "Fourth numeric component must be compared");
    Check(!Newer(current, current), "An equal version must never be an update");
    Check(!Newer({1, 99, 99, 0}, current), "Never offer a downgrade");
    Check(VersionKey(*ParseVersion("v2.0.1.0")) == "2.0.1", "Normalize equivalent version identities");
    auto result = EvaluateResponse(200, Feed(), current);
    Check(result.status == Status::Available && result.release, "New stable release not offered");
    Check(result.release->Url() == L"https://github.com/rayhan138/Watchman/releases/tag/v2.0.2", "Download must target the pinned official release page");
    Check(EvaluateResponse(200, Feed("v2.0.1"), current).status == Status::UpToDate, "Equal stable version must not notify");
    Check(EvaluateResponse(200, Feed("v1.0.5"), current).status == Status::UpToDate, "Older stable version must not notify");
    Check(EvaluateResponse(200, Feed(), {}).status == Status::UnknownVersion, "Missing resource must not become a fabricated installed version");
    Check(EvaluateResponse(404, "", current).status == Status::NoRelease, "404 is no published release, not up to date");
    for (unsigned status : {0u, 301u, 302u, 401u, 403u, 429u, 500u, 503u})
        Check(EvaluateResponse(status, Feed(), current).status == Status::Unavailable, "HTTP errors/redirects must not become update/current results");
    Check(EvaluateResponse(200, Feed("v2.0.2", ",\"assets\":[{\"name\":\"a\\uD83D\\uDE80\",\"size\":-1.25e+4,\"ignored\":[null,true,false,{}]}],\"body\":\"notes\\n\\\"tag_name\\\":\\\"v999.0.0\\\"\""), current).status == Status::Available,
        "Valid unknown nested fields/escaped Unicode must parse without being searched as metadata");
    auto draft = Feed(); draft.replace(draft.find("false"), 5, "true");
    Check(EvaluateResponse(200, draft, current).status == Status::NoRelease, "Draft releases must not notify");
    draft = Feed(); draft.replace(draft.rfind("false"), 5, "true");
    Check(EvaluateResponse(200, draft, current).status == Status::NoRelease, "Prereleases must not notify");
    const std::vector<std::string> malformed = {
        "", "null", "[]", "{}", "{broken}", Feed() + "x", "[" + Feed() + "]", "{\"tag_name\":null}",
        Feed("v2.0.2", ",\"tag_name\":\"v9.0.0\""), Feed("v2.0.2", ",\"draft\":false"),
        Feed("v2.0.2", ",\"x\":01"), Feed("v2.0.2", ",\"x\":1."), Feed("v2.0.2", ",\"x\":1e"),
        Feed("v2.0.2", ",\"x\":[1,]"), Feed("v2.0.2", ",\"x\":{\"a\":1,}"),
        Feed("v2.0.2", ",\"x\":\"\\uD800\""), Feed("v2.0.2", ",\"x\":\"\\uDC00\""),
        Feed("v2.0.2", ",\"x\":\"\\q\""), Feed("v2.0.2", ",\"x\":\"\n\""),
        Feed("v2.0.2", ",\"x\":\"\xc0\xaf\""), Feed("v2.0.2", ",\"x\":\"\xed\xa0\x80\""),
        Feed("v2.0.2", ",\"x\":\"\xf4\x90\x80\x80\""),
        Feed("v2.0.2", ",\"x\":" + std::string(30, '[') + "0" + std::string(30, ']')),
        std::string(max_response_bytes + 1, ' ')
    };
    for (const auto& body : malformed) Check(EvaluateResponse(200, body, current).status == Status::Unavailable, "Malformed/unbounded JSON must fail closed");
    auto spoof = Feed(); spoof.replace(spoof.find(release_prefix), std::string(release_prefix).size(), "https://evil.example/");
    Check(EvaluateResponse(200, spoof, current).status == Status::Unavailable, "A foreign URL must not be launchable");
    for (const auto& tag : {"v2.0.2-beta", "v2.0.2/../../../evil", "v2.0.2?download=1"})
        Check(EvaluateResponse(200, Feed(tag), current).status == Status::Unavailable, "Unsafe URL/tag accepted");
    Check(Release{{}, "file:///evil"}.Url().empty(), "Even a bad internal release must not launch a file URI");
    auto changed_type = Feed(); changed_type.replace(changed_type.find("false"), 5, "\"false\"");
    Check(EvaluateResponse(200, changed_type, current).status == Status::Unavailable, "Booleans cannot be JSON strings");
    // Every truncation must be an error, never an invented update/current state.
    const auto full = Feed();
    for (std::size_t i = 0; i < full.size(); ++i)
        Check(EvaluateResponse(200, full.substr(0, i), current).status == Status::Unavailable, "Truncated response accepted");
    Notice notice;
    notice.Apply(result);
    Check(notice.ShouldNotify() && notice.MayPrompt(), "A new version should enable all three surfaces");
    notice.Notified(); notice.Dismissed();
    Check(!notice.ShouldNotify() && !notice.MayPrompt() && notice.Available(), "Dismissal must not remove the footer/download information");
    Notice reloaded; reloaded.Load("2.0.2", "2.0.2"); reloaded.Apply(result);
    Check(!reloaded.ShouldNotify() && !reloaded.MayPrompt(), "Restart must not nag about a dismissed/notified version");
    notice.Apply({Status::Unavailable, {}, 403});
    Check(notice.Available() && notice.Last().status == Status::Unavailable, "Offline recheck must retain last confirmed availability but show the error");
    notice.Apply(EvaluateResponse(200, Feed("v2.0.3"), current));
    Check(notice.ShouldNotify() && notice.MayPrompt(), "The next version must not inherit the old version's dismissal");
    notice.Apply(EvaluateResponse(200, Feed("v2.0.1"), current));
    Check(!notice.Available(), "A confirmed withdrawn/current release must clear stale update UI");
    Schedule schedule(100);
    Check(!schedule.Due(100 + initial_delay_ms - 1) && schedule.Due(100 + initial_delay_ms), "Defer automatic network work until after startup");
    Check(schedule.Begin(101, true) && schedule.Pending(), "Manual check may run before automatic delay");
    Check(!schedule.Begin(102, true), "Do not start concurrent checks");
    schedule.Complete(102, true);
    Check(!schedule.Begin(103, true), "Rate-limit repeated clicks");
    Check(schedule.Begin(101 + manual_cooldown_ms, true), "Manual retry should be possible after cooldown");
    schedule.Complete(200000, false);
    Check(!schedule.Due(200000 + failure_retry_ms - 1) && schedule.Due(200000 + failure_retry_ms), "Failed checks need a bounded backoff");
    Check(schedule.Begin(200000 + failure_retry_ms, false), "Retry failed automatic check");
    schedule.Complete(4000000, true);
    Check(!schedule.Due(4000000 + check_interval_ms - 1) && schedule.Due(4000000 + check_interval_ms), "Successful checks should be daily");
    for (const float width : {460.0f, 480.0f, 560.0f, 960.0f, 1920.0f}) {
        for (const float height : {650.0f, 720.0f, 820.0f, 1080.0f}) {
            const auto l = Presentation::GetLayout(width, height);
            Check(l.card.left >= 0 && l.card.top >= 0 && l.card.right <= width && l.card.bottom <= height, "Prompt must fit the supported client area");
            Check(l.close.right - l.close.left >= 44 && l.download.bottom - l.download.top >= 44, "Prompt actions need real hit areas");
            Check(l.title.bottom < l.version.top && l.version.bottom < l.installed.top && l.body.bottom < l.download.top, "Update text/actions must not overlap");
            Check(l.download.right < l.later.left && l.later.right <= l.card.right - 20, "Download and dismiss must remain separate");
            const auto footer = Presentation::FooterLink(width, height);
            Check(footer.left >= 160 && footer.right <= width - 140 && footer.right - footer.left >= 120, "Tiny footer must not cover Connected/Uptime");
        }
    }
}
}
int main() {
    try { Test(); std::cout << "Update policy/parser/layout: " << checks << " checks passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "Update check " << checks << ": " << e.what() << '\n'; return 1; }
}
