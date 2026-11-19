#pragma once

#include "network_test_protocol.h"
#include <algorithm>
#include <d2d1helper.h>

namespace SpeedTestPresentation {
inline constexpr wchar_t consent_text[] =
    L"M-Lab  /  Open-source NDT7\n\n"
    L"Transfers substantial download and upload data. Uses Windows' route, not necessarily the monitoring adapter.\n\n"
    L"M-Lab collects and publicly publishes your public IP address, test time and measurement data. It retains this data indefinitely.\n\n"
    L"Choose Agree & run only if you accept this data use.";

struct Status {
    const wchar_t* title = L"";
    const wchar_t* detail = L"";
    const wchar_t* download_label = L"DOWNLOAD";
    const wchar_t* upload_label = L"UPLOAD";
    bool notice = false;
};
inline Status GetStatus(network_test::TransferState download, network_test::TransferState upload,
    bool upload_confirmed, bool error, bool warning) {
    using network_test::TransferState;
    Status status;
    if (download == TransferState::Partial) status.download_label = L"DOWN (partial)";
    if (upload == TransferState::Partial) status.upload_label = upload_confirmed ? L"UP (partial)" : L"UP (estimate)";
    if (error || download == TransferState::Failed || upload == TransferState::Failed) {
        status.title = L"Test incomplete";
        status.detail = L"Some measurements could not finish. Try again.";
        status.notice = true;
    } else if (upload == TransferState::Partial && !upload_confirmed) {
        status.title = L"Upload estimate";
        status.detail = L"Upload was measured locally, not verified by the server.";
        status.notice = true;
    } else if (warning || download == TransferState::Partial || upload == TransferState::Partial) {
        status.title = L"Incomplete result";
        status.detail = L"A transfer ended early. Values may be incomplete.";
        status.notice = true;
    }
    return status;
}
inline D2D1_RECT_F DetailsButton(float left, float right, float card_top) {
    return D2D1::RectF((std::max)(left, right - 96.0f), card_top + 292.0f, right - 28.0f, card_top + 322.0f);
}
struct DialogLayout {
    D2D1_RECT_F card{}, title{}, body{}, privacy{}, cancel{}, confirm{};
};
inline DialogLayout GetDialog(float width, float height) {
    const float w = (std::min)(460.0f, width - 40.0f);
    const float h = (std::min)(430.0f, height - 40.0f);
    const float x = (width - w) * 0.5f;
    const float y = (height - h) * 0.5f;
    DialogLayout d;
    d.card = D2D1::RectF(x, y, x + w, y + h);
    d.title = D2D1::RectF(x + 24, y + 22, x + w - 24, y + 54);
    d.body = D2D1::RectF(x + 24, y + 78, x + w - 24, y + h - 132);
    d.privacy = D2D1::RectF(x + 24, y + h - 114, x + w - 24, y + h - 80);
    d.cancel = D2D1::RectF(x + 24, y + h - 60, x + 124, y + h - 22);
    d.confirm = D2D1::RectF(x + 136, y + h - 60, x + w - 24, y + h - 22);
    return d;
}
} // namespace SpeedTestPresentation
