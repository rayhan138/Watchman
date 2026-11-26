// Real synchronous WinHTTP against an owned HTTP loopback server only. The
// production binary has no endpoint override and always uses GitHub HTTPS.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "update_checker.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace WatchmanUpdates;
namespace {
unsigned checks = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
class Server {
public:
    explicit Server(std::string response) : response_(std::move(response)) {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) throw std::runtime_error("Loopback socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(socket_, 1)) {
            closesocket(socket_); throw std::runtime_error("Loopback bind/listen failed");
        }
        int size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size)) {
            closesocket(socket_); throw std::runtime_error("Loopback port unavailable");
        }
        port = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            fd_set reads; FD_ZERO(&reads); FD_SET(socket_, &reads);
            timeval timeout{6, 0};
            if (select(0, &reads, nullptr, nullptr, &timeout) <= 0) return;
            const SOCKET client = accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            DWORD receive_timeout = 2000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout));
            char buffer[4096]{};
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout));
            while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
                const int received = recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                request.append(buffer, received);
            }
            std::size_t sent = 0;
            while (sent < response_.size()) {
                const int count = send(client, response_.data() + sent,
                    static_cast<int>((std::min)(response_.size() - sent, std::size_t(8192))), 0);
                if (count <= 0) break;
                sent += static_cast<std::size_t>(count);
            }
            shutdown(client, SD_SEND);
            closesocket(client);
        });
    }
    ~Server() { if (thread_.joinable()) thread_.join(); closesocket(socket_); }
    void Join() { if (thread_.joinable()) thread_.join(); }
    unsigned short port = 0;
    std::string request;
private:
    SOCKET socket_ = INVALID_SOCKET;
    std::string response_;
    std::thread thread_;
};
std::string Response(unsigned status, const std::string& body, const std::string& type = "application/json; charset=utf-8", std::string extra = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " Test\r\nContent-Type: " + type + "\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body;
}
void Test() {
    const std::string feed = "{\"tag_name\":\"v2.0.2\",\"html_url\":\"https://github.com/rayhan138/Watchman/releases/tag/v2.0.2\",\"draft\":false,\"prerelease\":false}";
    std::atomic<bool> stopping{false};
    {
        Server server(Response(200, feed));
        const auto result = CheckLoopbackReleaseForTest(server.port, stopping);
        server.Join();
        Check(result.status == Status::Available && result.release && VersionKey(result.release->version) == "2.0.2", "Actual WinHTTP JSON check failed");
        Check(server.request.find("GET /release-fixture HTTP/1.1") != std::string::npos, "Expected one GET request");
        Check(server.request.find("X-GitHub-Api-Version: 2022-11-28") != std::string::npos, "Use a versioned public API");
        Check(server.request.find("Authorization:") == std::string::npos && server.request.find("Cookie:") == std::string::npos, "Do not send user credentials/cookies");
    }
    for (const auto status : {403u, 404u, 429u, 500u}) {
        Server server(Response(status, "{}"));
        const auto result = CheckLoopbackReleaseForTest(server.port, stopping);
        Check(result.status == (status == 404 ? Status::NoRelease : Status::Unavailable), "HTTP error must not become current/newer");
    }
    {
        Server server(Response(302, "", "application/json", "Location: http://127.0.0.1:1/not-followed\r\n"));
        Check(CheckLoopbackReleaseForTest(server.port, stopping).error == 302, "Must not follow an API redirect to a different endpoint");
    }
    for (const auto& body : {std::string("{truncated"), std::string(max_response_bytes + 8192, 'x')}) {
        Server server(Response(200, body));
        Check(CheckLoopbackReleaseForTest(server.port, stopping).status == Status::Unavailable, "Malformed/oversized transport data must be rejected");
    }
    {
        Server server(Response(200, feed, "text/html"));
        Check(CheckLoopbackReleaseForTest(server.port, stopping).status == Status::Unavailable, "HTML proxy pages are not JSON release metadata");
    }
    stopping = true;
    Check(CheckLoopbackReleaseForTest(1, stopping).status == Status::Unavailable, "Pre-cancelled work must not contact a server");
    std::cout << "Owned-loopback WinHTTP update checks: " << checks << " PASS; no public network requests\n";
}
}
int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    int result = 0;
    try { Test(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    WSACleanup();
    return result;
}
