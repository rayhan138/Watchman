#include "network_test_protocol.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Near(double actual, double expected, const char* message) {
    Check(std::abs(actual - expected) < 0.000001, message);
}

void TestUnitsAndSources() {
    using namespace network_test;
    Near(GoodputMbps(125'000'000, 10), 100, "bytes/seconds to decimal Mbps");
    Near(RttMilliseconds(500), 0.5, "submillisecond RTT");
    Near(RttMilliseconds(1000), 1, "one millisecond RTT");
    Near(RttMilliseconds(2000), 2, "two millisecond RTT");
    Near(GoodputMbps(1, 0), 0, "zero elapsed");
    Near(GoodputMbps(-1, 1), 0, "negative bytes");
    Near(GoodputMbps(1, std::numeric_limits<double>::infinity()), 0, "infinite elapsed");
    Near(RttMilliseconds(std::numeric_limits<double>::quiet_NaN()), 0, "invalid RTT");

    const auto download = FinishTransfer(false, 125'000'000, 10, 250, {});
    Near(download.speed_mbps, 100, "download must use client received goodput, not server sent rate");
    Check(download.state == TransferState::Complete, "normal download completion");
    const auto upload = FinishTransfer(true, 125'000'000, 10, 80, {});
    Near(upload.speed_mbps, 80, "upload prefers receiver measurement");
    Check(upload.state == TransferState::Complete, "confirmed upload completion");
    // The caller supplies the send interval, never the later join/shutdown time.
    const auto estimate = FinishTransfer(true, 125'000'000, 10, 0, {});
    Near(estimate.speed_mbps, 100, "local upload uses send interval");
    Check(estimate.state == TransferState::Partial && !estimate.warning.empty(), "unconfirmed upload warning");
    Check(estimate.error.empty(), "an estimate is not a fatal error");
}

void TestPartialAndFailedResults() {
    using namespace network_test;
    for (bool upload : {false, true}) {
        const auto partial = FinishTransfer(upload, 65'536, 0.1, 20, L"Stream reset");
        Check(partial.state == TransferState::Partial, "payload then reset is not clean completion");
        Check(partial.warning == L"Stream reset", "preserve partial-transfer warning");
        Check(partial.speed_mbps > 0 && partial.error.empty(), "retain usable partial measurement");
        const auto failed = FinishTransfer(upload, 0, 1, 20, L"Handshake failed");
        Check(failed.state == TransferState::Failed, "zero local payload must fail");
        Check(failed.speed_mbps == 0 && failed.error == L"Handshake failed", "preserve fatal error");
    }
    const auto invalid = FinishTransfer(false, 100, 0, 100, {});
    Check(invalid.state == TransferState::Failed && !invalid.error.empty(), "invalid interval fails");
}

void TestMeasurementScopeAndAggregation() {
    using namespace network_test;
    double speed = 0;
    double rtt = 0;
    UpdateMeasurement(LR"({"TCPInfo":{"NumBytes":999999999,"ElapsedTime":1,"MinRTT":500},"AppInfo":{"NumBytes":125000000,"ElapsedTime":10000000}})", speed, rtt);
    Near(speed, 100, "AppInfo scoped bytes/time");
    Near(rtt, 0.5, "TCPInfo scoped RTT");
    UpdateMeasurement(LR"({"AppInfo":{"NumBytes":300000000,"ElapsedTime":20000000},"TCPInfo":{"MinRTT":1000}})", speed, rtt);
    Near(speed, 120, "latest cumulative sample replaces, not sums or averages");
    Near(rtt, 0.5, "minimum RTT retained");
    UpdateMeasurement(LR"({"RTT":1,"AppInfo":{"NumBytes":0,"ElapsedTime":0},"TCPInfo":{"RTT":250}})", speed, rtt);
    Near(speed, 120, "invalid AppInfo does not erase prior measurement");
    Near(rtt, 0.25, "fallback RTT still microseconds, ignore top-level RTT");
    UpdateMeasurement(LR"({"Other":{"AppInfo":{"NumBytes":9,"ElapsedTime":1},"MinRTT":1}})", speed, rtt);
    Near(speed, 120, "ignore nested impostor AppInfo");
    Near(rtt, 0.25, "ignore RTT outside TCPInfo");
    double number = 0;
    Check(!ExtractJsonNumber(LR"({"NumBytes":123garbage})", L"NumBytes", number), "reject trailing numeric garbage");
    Check(!ExtractJsonNumber(LR"({"NumBytes":"123"})", L"NumBytes", number), "reject quoted number");
    std::wstring object;
    Check(ExtractJsonObject(LR"({"AppInfo":{"note":"brace } and escaped \" quote","NumBytes":1}})", L"AppInfo", object), "braces inside strings");
}

void TestFragmentAssembly() {
    using namespace network_test;
    MeasurementText text;
    const std::string first = R"({"AppInfo":{"NumBytes":125000000,)";
    const std::string last = R"("ElapsedTime":10000000}})";
    Check(text.Append(first.data(), first.size(), false) == MeasurementText::Result::Pending, "nonfinal UTF8 fragment");
    Check(text.Append(last.data(), last.size(), true) == MeasurementText::Result::Complete, "final UTF8 fragment");
    double speed = 0;
    double rtt = 0;
    UpdateMeasurement(std::wstring(text.Text().begin(), text.Text().end()), speed, rtt);
    Near(speed, 100, "parse assembled measurement once");
    text.Clear();
    Check(text.Empty(), "clear completed text before next message");
    const std::string maximum(MeasurementText::kMaxBytes, 'x');
    Check(text.Append(maximum.data(), maximum.size(), false) == MeasurementText::Result::Pending, "allow NDT7 minimum maximum message size");
    Check(text.Append("x", 1, false) == MeasurementText::Result::TooLarge, "bound accumulated message");
    Check(text.Empty(), "release oversized message");
}

void TestIcmpAndWorkerCleanup() {
    using namespace network_test;
    Check(SuccessfulEcho(1, 0), "IP_SUCCESS echo passes");
    Check(!SuccessfulEcho(1, 11003), "ICMP unreachable reply is not success");
    Check(!SuccessfulEcho(0, 0), "zero replies cannot pass even with stale success status");
    Check(std::wstring(NoPingResponseLabel(0, 0)) == L"Not measured", "initial ping state is not offline");
    Check(std::wstring(NoPingResponseLabel(5, 0)) == L"No ICMP response", "ICMP blocking is not offline");
    Check(std::wstring(NoPingResponseLabel(0, 5)) == L"Probe unavailable", "report handle/worker error");
    std::atomic<bool> running{true};
    { RunningReset reset(running); }
    Check(!running.load(), "worker normal/early-return cleanup");
    running = true;
    try {
        RunningReset reset(running);
        throw 1;
    } catch (int) {}
    Check(!running.load(), "worker exceptional-exit cleanup");
}
} // namespace

int main() {
    try {
        TestUnitsAndSources();
        TestPartialAndFailedResults();
        TestMeasurementScopeAndAggregation();
        TestFragmentAssembly();
        TestIcmpAndWorkerCleanup();
        std::cout << "Network protocol/result tests passed (no network or database access)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
