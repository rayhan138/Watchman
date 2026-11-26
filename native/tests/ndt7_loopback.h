#pragma once
// Owned RFC6455 test peer. Bound ONLY to loopback; no DNS, M-Lab/public traffic,
// profile changes, detached threads or production runtime switches.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>
#include <iostream>

namespace WatchmanNdtTest {
class Server {
public:
    enum class Mode { Complete, StallDownload, StallUpload, RejectUpload, ProtocolCloseUpload };
    explicit Server(Mode mode = Mode::Complete) : mode_(mode) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("Test listen socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener_, 4)) {
            closesocket(listener_); throw std::runtime_error("Test loopback bind/listen failed");
        }
        int size = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { Run(); });
    }
    ~Server() {
        stopping_ = true;
        // The owner closes sockets after their bounded I/O returns. No raw
        // handle is concurrently closed/recycled from the test/UI thread.
        if (worker_.joinable()) worker_.join();
        closesocket(listener_);
    }
    std::wstring Url(bool upload) const {
        return L"ws://127.0.0.1:" + std::to_wstring(port_) + (upload ? L"/upload" : L"/download");
    }
    bool DownloadConnected() const { return download_connected_; }
    bool UploadConnected() const { return upload_connected_; }
    bool Healthy() const { return !failed_; }
    bool PongVerified() const { return pong_verified_; }
    uint64_t ReceivedBytes() const { return received_bytes_; }
private:
    SOCKET listener_ = INVALID_SOCKET;
    std::thread worker_;
    Mode mode_;
    unsigned short port_ = 0;
    std::atomic<bool> stopping_{false}, failed_{false}, download_connected_{false}, upload_connected_{false};
    std::atomic<uint64_t> received_bytes_{0};
    std::atomic<bool> pong_verified_{false};
    bool fragmented_message_ = false;
    unsigned frames_ = 0;
    inline static constexpr char kPingPayload[] = "1795473025000000000";

    bool ProtocolViolation(const char* reason) {
        failed_ = true;
        std::cerr << "Owned peer rejected a client RFC6455 frame: " << reason << '\n';
        return false;
    }
    bool Receive(SOCKET socket, void* data, size_t length) {
        auto* bytes = static_cast<char*>(data);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (length && !stopping_ && GetTickCount64() < deadline) {
            const int n = recv(socket, bytes, static_cast<int>((std::min)(length, size_t{65536})), 0);
            if (n > 0) { bytes += n; length -= static_cast<size_t>(n); }
            else if (n == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) continue;
            else return false;
        }
        return length == 0;
    }
    bool Send(SOCKET socket, const void* data, size_t length) {
        const auto* bytes = static_cast<const char*>(data);
        while (length && !stopping_) {
            const int n = send(socket, bytes, static_cast<int>(length), 0);
            if (n <= 0) return false;
            bytes += n; length -= static_cast<size_t>(n);
        }
        return length == 0;
    }
    bool Frame(SOCKET socket, unsigned char opcode, const std::string& payload, bool final = true) {
        std::string header(1, static_cast<char>((final ? 0x80 : 0) | opcode));
        if (payload.size() < 126) header += static_cast<char>(payload.size());
        else {
            header += static_cast<char>(126);
            header += static_cast<char>(payload.size() >> 8);
            header += static_cast<char>(payload.size());
        }
        return Send(socket, header.data(), header.size()) && Send(socket, payload.data(), payload.size());
    }
    bool ReadFrame(SOCKET socket, unsigned char& opcode, std::string& payload) {
        unsigned char header[2]{};
        if (!Receive(socket, header, 2)) return false;
        opcode = header[0] & 15;
        ++frames_;
        if (frames_ <= 4 || (opcode != 0 && opcode != 1 && opcode != 2) || (header[0] & 0x70) || !(header[1] & 128)) {
            std::cout << "Owned client frame " << frames_ << ": header=" << static_cast<unsigned>(header[0])
                << ',' << static_cast<unsigned>(header[1]) << ", received_payload=" << received_bytes_.load() << '\n';
        }
        const bool final = (header[0] & 128) != 0;
        if (header[0] & 0x70) return ProtocolViolation("reserved bits without a negotiated extension");
        if (!(header[1] & 128)) return ProtocolViolation("unmasked client frame");
        if (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)
            return ProtocolViolation("reserved opcode");
        const auto encoded_length = header[1] & 127;
        if (opcode >= 8 && (!final || encoded_length > 125)) return ProtocolViolation("invalid control frame");
        uint64_t length = encoded_length;
        if (length == 126 || length == 127) {
            const size_t count = length == 126 ? 2 : 8;
            unsigned char extended[8]{};
            if (!Receive(socket, extended, count)) return false;
            if (count == 8 && (extended[0] & 128)) return ProtocolViolation("negative frame length");
            length = 0;
            for (size_t i = 0; i < count; ++i) length = (length << 8) | extended[i];
            if ((count == 2 && length < 126) || (count == 8 && length <= 65535))
                return ProtocolViolation("noncanonical frame length");
        }
        if (length > 1024 * 1024) return ProtocolViolation("oversized owned-test message");
        if (opcode == 0) {
            if (!fragmented_message_) return ProtocolViolation("continuation without an open message");
            fragmented_message_ = !final;
        } else if (opcode == 1 || opcode == 2) {
            if (fragmented_message_) return ProtocolViolation("new data message inside a fragmented message");
            fragmented_message_ = !final;
        } else if (opcode == 8 && length == 1) return ProtocolViolation("one-byte close status");
        unsigned char mask[4]{};
        if (!Receive(socket, mask, sizeof(mask))) return false;
        payload.resize(static_cast<size_t>(length));
        if (!Receive(socket, payload.data(), payload.size())) return false;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
        return true;
    }
    bool Upgrade(SOCKET socket, bool& upload) {
        std::string request;
        while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
            char ch = 0;
            if (!Receive(socket, &ch, 1)) return false;
            request += ch;
        }
        upload = request.find("GET /upload ") == 0;
        if (upload && mode_ == Mode::RejectUpload) {
            const std::string rejected = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            Send(socket, rejected.data(), rejected.size()); return false;
        }
        const auto key_start = request.find("Sec-WebSocket-Key: ");
        if (key_start == std::string::npos) return false;
        const auto first = key_start + std::string("Sec-WebSocket-Key: ").size();
        const auto end = request.find("\r\n", first);
        const std::string input = request.substr(first, end - first) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::array<BYTE, 20> sha1{};
        DWORD size = static_cast<DWORD>(sha1.size());
        if (!CryptHashCertificate(0, CALG_SHA1, 0, reinterpret_cast<const BYTE*>(input.data()),
            static_cast<DWORD>(input.size()), sha1.data(), &size)) return false;
        char encoded[64]{}; size = sizeof(encoded);
        if (!CryptBinaryToStringA(sha1.data(), static_cast<DWORD>(sha1.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded, &size)) return false;
        const std::string response = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: net.measurementlab.ndt.v7\r\nSec-WebSocket-Accept: ") + encoded + "\r\n\r\n";
        return Send(socket, response.data(), response.size());
    }
    void Transfer(SOCKET socket, bool upload) {
        if (upload) upload_connected_ = true; else download_connected_ = true;
        if ((upload && mode_ == Mode::StallUpload) || (!upload && mode_ == Mode::StallDownload)) {
            const auto end = GetTickCount64() + 3500;
            while (!stopping_ && GetTickCount64() < end) Sleep(10);
            return;
        }
        if (!upload) {
            // Send the first RTT sample before payload. Even a deliberately
            // shortened client transfer window must exercise fragmented UTF8
            // parsing; VM scheduling must not turn that into a missing fixture.
            if (!Frame(socket, 1, "{\"AppInfo\":{\"NumBytes\":0,", false) ||
                !Frame(socket, 0, "\"ElapsedTime\":1},\"TCPInfo\":{\"MinRTT\":2500}}")) return;
            const std::string data(16384, 'x');
            for (int i = 0; i < 40 && !stopping_; ++i) {
                if (!Frame(socket, 2, data)) return;
                Sleep(20);
            }
            // Fragmented UTF8 measurement exercises WinHTTP's message assembly.
            if (!Frame(socket, 1, "{\"AppInfo\":{\"NumBytes\":655360,", false) ||
                !Frame(socket, 0, "\"ElapsedTime\":800000},\"TCPInfo\":{\"MinRTT\":2500}}")) return;
            Frame(socket, 8, std::string("\x03\xe8", 2));
            unsigned char opcode{}; std::string payload;
            ReadFrame(socket, opcode, payload);
        } else {
            unsigned char opcode{}; std::string payload;
            const auto started = GetTickCount64();
            bool measured = false;
            unsigned pings = 0;
            while (ReadFrame(socket, opcode, payload)) {
                if (opcode == 8) break;
                if (opcode == 2 || opcode == 0) received_bytes_ += payload.size();
                if (opcode == 10) {
                    if (payload != kPingPayload) { ProtocolViolation("pong did not echo the server ping"); return; }
                    pong_verified_ = true;
                }
                // Repeated control traffic before the shortened 1s send
                // window ends; coalesced pongs are legal, malformed frames not.
                if (mode_ == Mode::Complete && pings < 3 && GetTickCount64() - started >= 200 * (pings + 1)) {
                    if (!Frame(socket, 9, kPingPayload)) return;
                    ++pings;
                }
                if (!measured && GetTickCount64() - started >= 600) {
                    // RFC6455 forbids data after receiving Close. NDT receiver
                    // samples must be sent while the upload is still open.
                    if (!Frame(socket, 1, "{\"TCPInfo\":{\"BytesReceived\":1000000,\"ElapsedTime\":1000000,\"MinRTT\":3000}}")) return;
                    measured = true;
                    if (mode_ == Mode::ProtocolCloseUpload) {
                        Frame(socket, 8, std::string("\x03\xea", 2) + "owned protocol close");
                        while (ReadFrame(socket, opcode, payload) && opcode != 8) {}
                        return;
                    }
                }
                Sleep(2); // Bound loopback traffic instead of benchmarking a CI VM.
            }
            Sleep(180); // Make the finishing/server-close wait observable.
            Frame(socket, 8, std::string("\x03\xe8", 2));
        }
    }
    void Run() noexcept {
        try {
            while (!stopping_) {
                fd_set set; FD_ZERO(&set); FD_SET(listener_, &set);
                timeval timeout{0, 200000};
                if (select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
                const SOCKET client = accept(listener_, nullptr, nullptr);
                if (client == INVALID_SOCKET) continue;
                const int no_delay = 1;
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
                DWORD io_timeout = 200;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout));
                io_timeout = 1000;
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout));
                bool upload = false;
                fragmented_message_ = false;
                frames_ = 0;
                if (Upgrade(client, upload)) Transfer(client, upload);
                shutdown(client, SD_BOTH);
                closesocket(client);
            }
        } catch (...) { failed_ = true; }
    }
};
} // namespace WatchmanNdtTest
