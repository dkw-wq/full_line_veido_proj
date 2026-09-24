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

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), service.c_str(), &hints, &addresses) != 0) {
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        SOCKET candidate = socket(address->ai_family, address->ai_socktype,
                                  address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        if (connect(candidate, address->ai_addr,
                    static_cast<int>(address->ai_addrlen)) != SOCKET_ERROR) {
            sock = candidate;
            break;
        }
        closesocket(candidate);
    }
    freeaddrinfo(addresses);
    if (sock == INVALID_SOCKET) return false;

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

