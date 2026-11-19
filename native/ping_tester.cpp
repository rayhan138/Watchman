#include <winsock2.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include "ping_tester.h"
#include "network_test_protocol.h"
#include <algorithm>

#include <chrono>
#include <cmath>

#include <memory>



PingTester::PingTester() {
    latency_ms_ = 0.0f;
    jitter_ms_ = 0.0f;
    packet_loss_percent_ = 0.0f;
}

PingTester::~PingTester() {
    Stop();
}

void PingTester::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_) return;
    if (worker_thread_.joinable()) worker_thread_.join();

    probe_error_ = 0;
    recent_samples_.clear();
    latency_ms_ = 0.0f;
    jitter_ms_ = 0.0f;
    packet_loss_percent_ = 0.0f;
    samples_sent_ = 0;
    samples_received_ = 0;
    min_latency_ms_ = 0.0f;
    max_latency_ms_ = 0.0f;

    running_ = true;
    try {
        worker_thread_ = std::thread([this]() {
            network_test::RunningReset reset(running_);
            try {
                RunLoop();
            } catch (...) {
                probe_error_ = ERROR_GEN_FAILURE;
            }
        });
    } catch (...) {
        probe_error_ = ERROR_NOT_ENOUGH_MEMORY;
        running_ = false;
    }
}

void PingTester::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    // A failed worker can already have cleared running_ while remaining joinable.
    running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void PingTester::SetActiveMode(bool active) {
    active_mode_ = active;
}

void PingTester::RunLoop() {
    // Target IP: 8.8.8.8 (Google DNS)
    IPAddr ipAddress = 0x08080808; // 8.8.8.8
    
    char SendData[32] = "Watchman Ping Data Payload";
    const DWORD ReplySize = sizeof(ICMP_ECHO_REPLY) + sizeof(SendData) + 8;
    std::vector<char> ReplyBuffer(ReplySize);
    HANDLE hIcmpFile = IcmpCreateFile();
    if (hIcmpFile == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        probe_error_ = error != NO_ERROR ? error : ERROR_GEN_FAILURE;
        return;
    }

    std::unique_ptr<void, decltype(&IcmpCloseHandle)> icmp_guard(hIcmpFile, &IcmpCloseHandle);
    while (running_) {
        // Send ICMP Echo Request
        DWORD dwRetVal = IcmpSendEcho(
            hIcmpFile,
            ipAddress,
            SendData,
            sizeof(SendData),
            nullptr,
            ReplyBuffer.data(),
            ReplySize,
            1000 // 1 second timeout
        );

        bool was_lost = true;
        float rtt = 0.0f;

        if (dwRetVal != 0) {
            PICMP_ECHO_REPLY pEchoReply = reinterpret_cast<PICMP_ECHO_REPLY>(ReplyBuffer.data());
            if (network_test::SuccessfulEcho(dwRetVal, pEchoReply->Status)) {
                was_lost = false;
                rtt = static_cast<float>(pEchoReply->RoundTripTime);
            }
        }

        CalculateMetrics(rtt, was_lost);

        // Sleep dynamically: 2 seconds if active, 15 seconds if in background
        int sleep_time_ms = active_mode_ ? 2000 : 15000;
        int sleep_chunks = sleep_time_ms / 100;
        for (int i = 0; i < sleep_chunks && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

}

void PingTester::CalculateMetrics(float new_rtt, bool was_lost) {
    constexpr size_t kSampleWindowSize = 5;
    recent_samples_.push_back({ new_rtt, was_lost });
    if (recent_samples_.size() > kSampleWindowSize) {
        recent_samples_.erase(recent_samples_.begin());
    }

    uint32_t received = 0;
    float sum_latency = 0.0f;
    float min_latency = 0.0f;
    float max_latency = 0.0f;
    std::vector<float> successful_rtts;
    successful_rtts.reserve(recent_samples_.size());

    for (const PingSample& sample : recent_samples_) {
        if (sample.lost) {
            continue;
        }

        ++received;
        sum_latency += sample.rtt_ms;
        successful_rtts.push_back(sample.rtt_ms);
        if (received == 1) {
            min_latency = sample.rtt_ms;
            max_latency = sample.rtt_ms;
        } else {
            min_latency = (std::min)(min_latency, sample.rtt_ms);
            max_latency = (std::max)(max_latency, sample.rtt_ms);
        }
    }

    const uint32_t sent = static_cast<uint32_t>(recent_samples_.size());
    samples_sent_ = sent;
    samples_received_ = received;
    packet_loss_percent_ = sent == 0
        ? 0.0f
        : static_cast<float>(sent - received) * 100.0f / static_cast<float>(sent);
    min_latency_ms_ = min_latency;
    max_latency_ms_ = max_latency;

    if (received == 0) {
        latency_ms_ = 999.0f;
        jitter_ms_ = 0.0f;
        return;
    }

    latency_ms_ = sum_latency / static_cast<float>(received);
    if (successful_rtts.size() < 2) {
        jitter_ms_ = 0.0f;
        return;
    }

    float sum_difference = 0.0f;
    for (size_t index = 1; index < successful_rtts.size(); ++index) {
        sum_difference += std::abs(successful_rtts[index] - successful_rtts[index - 1]);
    }
    jitter_ms_ = sum_difference / static_cast<float>(successful_rtts.size() - 1);
}

std::wstring PingTester::GetQualityScoreString() const {
    if (probe_error_.load() != 0 || samples_received_.load() == 0) {
        return network_test::NoPingResponseLabel(samples_sent_.load(), probe_error_.load());
    }
    float loss = packet_loss_percent_.load();
    float rtt = latency_ms_.load();

    if (loss > 15.0f || rtt > 250.0f) return L"Poor";
    if (loss > 5.0f || rtt > 120.0f) return L"Fair";
    if (loss > 1.0f || rtt > 50.0f) return L"Good";
    return L"Excellent";
}

float PingTester::GetQualityScorePercent() const {
    if (samples_received_.load() == 0) return 0.0f;
    float loss = packet_loss_percent_.load();
    float rtt = latency_ms_.load();

    float score = 100.0f;
    score -= (loss * 4.0f);
    
    if (rtt > 20.0f) {
        score -= (rtt - 20.0f) * 0.2f;
    }

    if (score < 0.0f) score = 0.0f;
    if (score > 100.0f) score = 100.0f;
    return score;
}
