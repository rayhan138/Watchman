#pragma once
#include "speed_test_state.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace SpeedTestView {
// Reveal the existing card, without resizing/redesigning it or hiding other
// content permanently. A user wheel/tab action stops automatic following.
inline float RevealOffset(float top, float height, float viewport_top, float viewport_bottom, float current) {
    if (height > viewport_bottom - viewport_top) return (std::max)(0.0f, top - viewport_top);
    if (top < viewport_top + current) current = top - viewport_top;
    if (top + height > viewport_bottom + current) current = top + height - viewport_bottom;
    return (std::max)(0.0f, current);
}
inline std::wstring Mbps(double value, bool completed) {
    std::wostringstream text;
    text << std::fixed << std::setprecision(1) << value << L" Mbps";
    if (completed) text << L" (" << std::setprecision(2) << value / 8.0 << L" MB/s)";
    return text.str();
}
inline std::wstring Progress(const SpeedTestSnapshot& state) {
    const wchar_t* phase = L"Preparing...";
    switch (state.phase) {
    case SpeedTestPhase::Locating: phase = L"Connecting..."; break;
    case SpeedTestPhase::Download: phase = L"Downloading..."; break;
    case SpeedTestPhase::PreparingUpload: phase = L"Preparing upload..."; break;
    case SpeedTestPhase::Upload: phase = L"Uploading..."; break;
    case SpeedTestPhase::Finishing: phase = L"Finishing upload..."; break;
    case SpeedTestPhase::Cancelling: return L"Cancelling...";
    default: break;
    }
    return std::wstring(phase) + L" (" + std::to_wstring(state.progress) + L"%)";
}
inline std::wstring Rate(const SpeedTestSnapshot& state, bool download) {
    if ((download && state.phase == SpeedTestPhase::Download) ||
        (!download && state.phase == SpeedTestPhase::Upload))
        return Mbps(state.live_mbps, false) + L" (Testing...)";
    if (!download && state.phase == SpeedTestPhase::PreparingUpload) return L"Preparing...";
    if (!download && state.phase == SpeedTestPhase::Finishing) return L"Waiting for summary...";
    const auto outcome = download ? state.result.download_state : state.result.upload_state;
    if (outcome == network_test::TransferState::NotRun || outcome == network_test::TransferState::Failed) return L"--";
    return Mbps(download ? state.result.download_speed_mbps : state.result.upload_speed_mbps, !state.Running());
}
} // namespace SpeedTestView
