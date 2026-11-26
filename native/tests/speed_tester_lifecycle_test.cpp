#include "ndt7_loopback.h" // winsock2 must precede windows.h in speed_tester.h.
#include "speed_tester.h"
#include "speed_test_view.h"
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Predicate> bool Wait(Predicate predicate, unsigned timeout = 5000) {
    const auto end = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(5); } while (GetTickCount64() < end);
    return predicate();
}
void CancelRestartOwnership() {
    std::atomic<int> starts{0};
    std::atomic<bool> release{false}, entered{false};
    SpeedTester tester; // Destroy/join before the captured test synchronization state.
    // A worker deliberately ignores cancellation while an OS-like call is in
    // flight. A timed release also guarantees the pre-fix UI join fails safely
    // instead of hanging the entire test process forever.
    tester.SetWorkerForTest([&](SpeedTester& running) {
        ++starts; entered = true;
        const auto limit = GetTickCount64() + 1000;
        while (!release && GetTickCount64() < limit) Sleep(5);
        running.PublishForTest(SpeedTestPhase::Upload, 95, 321.0); // Late callback.
    });
    tester.CancelTest();
    Check(tester.GetSnapshot().phase == SpeedTestPhase::Idle, "Cancelling idle must not create a false result");
    tester.StartTest();
    Check(Wait([&] { return entered.load(); }), "Owned worker did not start");
    auto before = GetTickCount64();
    tester.CancelTest();
    tester.StartTest(); // This used to clear cancellation and synchronously join blocked I/O.
    Check(GetTickCount64() - before < 150, "Cancel/restart must never join an unfinished transfer from the UI click");
    auto state = tester.GetSnapshot();
    Check(tester.IsRunning() && state.phase == SpeedTestPhase::Cancelling && starts == 1,
        "Cancelling worker retains sole ownership; immediate restart cannot revive or replace it");
    Sleep(25);
    Check(tester.GetSnapshot().phase == SpeedTestPhase::Cancelling, "Late progress must not erase the Cancelling state");
    release = true;
    Check(Wait([&] { return !tester.IsRunning(); }), "Released cancelled worker failed to retire");
    state = tester.GetSnapshot();
    Check(state.phase == SpeedTestPhase::Cancelled && !state.Running() && state.live_mbps == 0 &&
        !state.result.error_message.empty(), "Cancelled UI result is published only when ownership has retired");
    entered = false;
    tester.StartTest();
    Check(Wait([&] { return entered.load() && !tester.IsRunning(); }) && starts == 2,
        "A later explicit restart must run a clean generation after the old worker retires");
}
void ExceptionalWorker() {
    SpeedTester tester;
    tester.SetWorkerForTest([](SpeedTester&) { throw std::runtime_error("owned worker fault"); });
    tester.StartTest();
    Check(Wait([&] { return !tester.IsRunning(); }), "Exception must retire worker ownership");
    const auto state = tester.GetSnapshot();
    Check(state.phase == SpeedTestPhase::Failed && !state.result.error_message.empty(), "Worker failure must publish a clear failed result");
    tester.StartTest();
    Check(Wait([&] { return !tester.IsRunning(); }), "Failed worker must remain restartable");
}
void RealLoopbackTransfers() {
    WatchmanNdtTest::Server peer;
    SpeedTester tester;
    tester.SetEndpointsForTest(peer.Url(false), peer.Url(true));
    tester.StartTest();
    std::set<SpeedTestPhase> phases;
    bool saw_download_live = false, saw_upload_live = false, download_retained = false;
    const bool completed = Wait([&] {
        const auto state = tester.GetSnapshot(); phases.insert(state.phase);
        if (state.phase == SpeedTestPhase::Download && state.live_mbps > 0) saw_download_live = true;
        if (state.phase == SpeedTestPhase::Upload && state.live_mbps > 0) saw_upload_live = true;
        if (state.phase == SpeedTestPhase::Upload && state.result.download_speed_mbps > 0 &&
            state.result.download_state != network_test::TransferState::NotRun) download_retained = true;
        return !tester.IsRunning();
    }, 15000);
    const auto state = tester.GetSnapshot();
    if (!completed || !state.result.error_message.empty()) {
        std::wcerr << L"Loopback state=" << static_cast<int>(state.phase) << L" error=" << state.result.error_message
            << L" warning=" << state.result.warning_message << L" status=" << state.status << L'\n';
    }
    Check(completed && state.phase == SpeedTestPhase::Completed && !state.Running(), "Actual WinHTTP loopback transfer must complete both phases");
    Check(peer.Healthy() && peer.DownloadConnected() && peer.UploadConnected() && peer.ReceivedBytes() > 0,
        "The test must transfer real application payload over owned sockets, not only simulate a UI phase");
    Check(peer.PongVerified(), "Concurrent WinHTTP upload must return a correctly masked pong with the server's unchanged NDT7 timestamp payload");
    Check(saw_download_live && saw_upload_live && download_retained, "Both transfer phases must publish live progress without erasing the completed download");
    Check(phases.count(SpeedTestPhase::PreparingUpload) && phases.count(SpeedTestPhase::Finishing),
        "Real transfer transitions must expose preparing and final-summary waiting states");
    std::cout << "Loopback final: download=" << state.result.download_speed_mbps << ", upload=" << state.result.upload_speed_mbps
        << ", confirmed=" << state.result.upload_server_confirmed << ", rtt=" << state.result.latency_ms
        << ", completed_at=" << state.result.completed_at_unix_seconds << std::endl;
    Check(state.result.download_speed_mbps > 0 && state.result.upload_speed_mbps == 8.0 && state.result.upload_server_confirmed &&
        state.result.completed_at_unix_seconds > 0 && state.result.latency_ms == 2.5,
        "Final snapshot must coherently retain rates, receiver confirmation, RTT and completion time");
    Check(SpeedTestView::Rate(state, true).find(L"MB/s") != std::wstring::npos &&
        SpeedTestView::Rate(state, false) == L"8.0 Mbps (1.00 MB/s)", "Both completed results must have correct displayed units");
    std::cout << "Real loopback transfer: download/live, preparing, upload/live, finishing, complete; received bytes=" << peer.ReceivedBytes() << '\n';
}
void RealStallCancellation(WatchmanNdtTest::Server::Mode mode) {
    WatchmanNdtTest::Server peer(mode);
    SpeedTester tester;
    tester.SetEndpointsForTest(peer.Url(false), peer.Url(true));
    tester.StartTest();
    Check(Wait([&] { return mode == WatchmanNdtTest::Server::Mode::StallDownload ? peer.DownloadConnected() : peer.UploadConnected(); }),
        "Stall test did not reach its real WinHTTP socket");
    Sleep(150); // Let the real synchronous Receive/Send enter its stalled operation.
    const auto before = GetTickCount64();
    tester.CancelTest(); tester.StartTest();
    Check(GetTickCount64() - before < 150, "Actual stalled I/O must not make cancel/restart block the UI thread");
    Check(Wait([&] { return !tester.IsRunning(); }, 10000), "Cancelled loopback I/O did not retire within the test's bounded stall");
    Check(tester.GetSnapshot().phase == SpeedTestPhase::Cancelled && tester.GetSnapshot().live_mbps == 0,
        "A late real socket completion cannot overwrite the cancellation result");
}
void RealProtocolClose() {
    WatchmanNdtTest::Server peer(WatchmanNdtTest::Server::Mode::ProtocolCloseUpload);
    SpeedTester tester;
    tester.SetEndpointsForTest(peer.Url(false), peer.Url(true));
    tester.StartTest();
    Check(Wait([&] { return !tester.IsRunning(); }, 10000), "Non-normal server close did not retire the real upload worker");
    const auto state = tester.GetSnapshot();
    Check(state.result.download_speed_mbps > 0 && state.result.upload_state == network_test::TransferState::Partial &&
        state.result.warning_message.find(L"1002") != std::wstring::npos &&
        state.result.warning_message.find(L"owned protocol close") != std::wstring::npos,
        "Non-normal close must retain the bounded peer reason in measurement details and mark the upload partial");
    Check(state.result.upload_server_confirmed && state.result.upload_speed_mbps == 8.0,
        "Early peer close may retain receiver-observed data but must never erase its partial-result warning");
}
void RealFailedUpload() {
    WatchmanNdtTest::Server peer(WatchmanNdtTest::Server::Mode::RejectUpload);
    SpeedTester tester;
    tester.SetEndpointsForTest(peer.Url(false), peer.Url(true));
    tester.StartTest();
    Check(Wait([&] { return !tester.IsRunning(); }, 10000), "Rejected upload did not complete failure publication");
    const auto state = tester.GetSnapshot();
    Check(state.phase == SpeedTestPhase::Failed && state.result.download_speed_mbps > 0 &&
        state.result.upload_state == network_test::TransferState::Failed && !state.result.error_message.empty(),
        "A real failed upload handshake retains the successful download but must not claim full success");
    Check(SpeedTestView::Rate(state, false) == L"--", "Failed upload display cannot claim a measured zero");
}
}
int main() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return 2;
    struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
    try {
        CancelRestartOwnership(); ExceptionalWorker(); RealLoopbackTransfers();
        RealStallCancellation(WatchmanNdtTest::Server::Mode::StallDownload);
        RealStallCancellation(WatchmanNdtTest::Server::Mode::StallUpload);
        RealFailedUpload(); RealProtocolClose();
        std::cout << "Speed worker lifecycle/real WinHTTP checks passed: " << checks << '\n'; return 0;
    } catch (const std::exception& error) {
        std::cerr << "Speed lifecycle check " << checks << " failed: " << error.what() << '\n'; return 1;
    }
}
