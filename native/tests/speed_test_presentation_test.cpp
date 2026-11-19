#include "speed_test_presentation.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool Inside(const D2D1_RECT_F& r, const D2D1_RECT_F& outer) {
    return r.left >= outer.left && r.right <= outer.right && r.top >= outer.top && r.bottom <= outer.bottom && r.left < r.right && r.top < r.bottom;
}
}
int main() {
    try {
        using namespace SpeedTestPresentation;
        using network_test::TransferState;
        const auto estimate = GetStatus(TransferState::Complete, TransferState::Partial, false, false, true);
        Check(estimate.notice && std::wstring(estimate.title) == L"Upload estimate", "unconfirmed upload needs visible estimate status");
        Check(std::wstring(estimate.upload_label) == L"UP (estimate)", "estimate label must remain next to upload number");
        const auto partial = GetStatus(TransferState::Complete, TransferState::Partial, true, false, true);
        Check(partial.notice && std::wstring(partial.title) == L"Incomplete result" && std::wstring(partial.upload_label) == L"UP (partial)",
            "receiver sample plus abnormal closure is partial, not an unconfirmed local estimate");
        const auto failed = GetStatus(TransferState::Complete, TransferState::Failed, false, true, false);
        Check(failed.notice && std::wstring(failed.title) == L"Test incomplete", "failed phase must never appear complete");
        Check(!GetStatus(TransferState::Complete, TransferState::Complete, true, false, false).notice, "clean test needs no warning");
        Check(!GetStatus(TransferState::NotRun, TransferState::NotRun, false, false, false).notice, "initial state is not failure");
        for (float width : {460.0f, 560.0f, 720.0f}) {
            for (float height : {650.0f, 820.0f}) {
                const auto d = GetDialog(width, height);
                Check(Inside(d.card, D2D1::RectF(0, 0, width, height)), "dialog must fit native minimum window");
                for (const auto& r : {d.title, d.body, d.privacy, d.cancel, d.confirm}) Check(Inside(r, d.card), "dialog controls must fit card");
                Check(d.title.bottom < d.body.top && d.body.bottom < d.privacy.top && d.privacy.bottom < d.confirm.top,
                    "disclosure and actions must not overlap");
                Check(d.cancel.right + 12 <= d.confirm.left, "cancel must be separate from explicit consent");
                const auto button = DetailsButton(16, width - 16, 700);
                Check(button.left >= 16 && button.right <= width - 16 && button.bottom > button.top, "details button must fit card");
            }
        }
        Check(network_test::IsNdt7Subprotocol(L"net.measurementlab.ndt.v7"), "required NDT7 protocol");
        Check(network_test::IsNdt7Subprotocol(L" net.measurementlab.ndt.v7\t"), "HTTP header whitespace");
        for (const auto* invalid : {L"", L"other", L"net.measurementlab.ndt.v7, other", L"NET.measurementlab.ndt.v7"})
            Check(!network_test::IsNdt7Subprotocol(invalid), "missing or mismatched selected protocol must fail");
        const auto local = network_test::FinishTransfer(true, 1000000, 1, 0, {});
        Check(!local.receiver_confirmed && local.state == TransferState::Partial, "local sender measurement cannot be called server-confirmed");
        const auto receiver = network_test::FinishTransfer(true, 1000000, 1, 6, L"Peer closed early");
        Check(receiver.receiver_confirmed && receiver.state == TransferState::Partial, "confirmation and clean completion are different facts");
        std::cout << "Speed test presentation and protocol checks passed (offline)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
