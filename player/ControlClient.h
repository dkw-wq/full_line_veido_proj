#pragma once

#include <string>

// Minimal TCP client to send PAUSE/RESUME commands to the pusher.
class ControlClient {
public:
    ControlClient(std::string host, uint16_t port);

    bool pause();
    bool resume();

private:
    bool sendCommand(const char* cmd);
    static bool ensureWinsock();

    std::string host_;
    uint16_t port_;
};

