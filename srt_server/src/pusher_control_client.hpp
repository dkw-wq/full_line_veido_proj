class PusherControlClient {
public:
    bool send_command(const std::string& host, int port, const std::string& command, int timeout_ms = 1200) {
        if (host.empty() || port <= 0) return false;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            close(fd);
            return false;
        }

        int rc = connect(fd, (sockaddr*)&addr, sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            return false;
        }

        if (rc < 0) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            rc = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (rc <= 0) {
                close(fd);
                return false;
            }
            int err = 0;
            socklen_t err_len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
                close(fd);
                return false;
            }
        }

        if (flags >= 0) fcntl(fd, F_SETFL, flags);

        std::string wire = command + "\n";
        const char* p = wire.data();
        size_t left = wire.size();
        while (left > 0) {
            ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
            if (n <= 0) {
                close(fd);
                return false;
            }
            p += n;
            left -= (size_t)n;
        }

        char ack[32];
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recv(fd, ack, sizeof(ack), 0);
        close(fd);
        return n >= 2 && ack[0] == 'O' && ack[1] == 'K';
    }
};