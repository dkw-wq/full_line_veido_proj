#include "ControlClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <mutex>
#include <string>

ControlClient::ControlClient(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

bool ControlClient::pause() {
    return sendCommand("PAUSE");
}

bool ControlClient::resume() {
    return sendCommand("RESUME");
}

bool ControlClient::sendCommand(const char* cmd) {
    if (!ensureWinsock()) return false;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        closesocket(sock);
        return false;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return false;
    }

    std::string msg = std::string(cmd) + "\n";
    int sent = send(sock, msg.c_str(), static_cast<int>(msg.size()), 0);
    closesocket(sock);
    return sent == static_cast<int>(msg.size());
}

bool ControlClient::ensureWinsock() {
    static std::atomic<bool> inited{false};
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
            inited.store(true, std::memory_order_relaxed);
        }
    });
    return inited.load(std::memory_order_relaxed);
}

