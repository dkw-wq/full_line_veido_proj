class WsBroadcaster {
public:
    WsBroadcaster() = default;
    ~WsBroadcaster() { stop(); }

    bool start(const std::string& bind_ip, int port) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) { log_line("ERROR", "WS socket failed"); return false; }
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)port);
        if (!bind_ip.empty() && bind_ip != "0.0.0.0")
            inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0 ||
            listen(listen_fd_, 16) < 0) {
            log_line("ERROR", "WS bind/listen failed on :" + std::to_string(port));
            close(listen_fd_); listen_fd_ = -1; return false;
        }
        running_.store(true);
        accept_thread_ = std::thread([this]{ accept_loop(); });
        log_line("INFO", "WebSocket broadcaster listening on :" + std::to_string(port));
        return true;
    }

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
        if (accept_thread_.joinable()) accept_thread_.join();
        std::lock_guard<std::mutex> lk(clients_mu_);
        for (int fd : clients_) close(fd);
        clients_.clear();
    }

    void broadcast(const std::string& json) {
        auto frame = ws_util::ws_frame(json);
        std::lock_guard<std::mutex> lk(clients_mu_);
        std::vector<int> dead;
        for (int fd : clients_) {
            ssize_t n = send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
            if (n <= 0) dead.push_back(fd);
        }
        for (int fd : dead) { close(fd); clients_.erase(fd); }
    }

    int client_count() const {
        std::lock_guard<std::mutex> lk(clients_mu_);
        return (int)clients_.size();
    }

private:
    void accept_loop() {
        while (running_.load()) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_fd_, &rfds);
            timeval tv{1, 0};
            if (select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
            int cfd = accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0) continue;
            std::thread([this, cfd]{ handshake(cfd); }).detach();
        }
    }

    void handshake(int cfd) {
        timeval tv{5, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[4096] = {0};
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(cfd); return; }

        std::string req(buf, n);
        std::string upgrade = ws_util::http_header(req, "Upgrade");
        std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
        if (upgrade != "websocket") {
            std::string resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
            send(cfd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
            close(cfd);
            return;
        }

        std::string key = ws_util::http_header(req, "Sec-WebSocket-Key");
        if (key.empty()) { close(cfd); return; }

        std::string accept_val = ws_util::sha1_b64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_val + "\r\n\r\n";
        if (send(cfd, resp.c_str(), resp.size(), MSG_NOSIGNAL) <= 0) { close(cfd); return; }

        tv = {0, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        {
            std::lock_guard<std::mutex> lk(clients_mu_);
            clients_.insert(cfd);
        }

        while (running_.load()) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(cfd, &rfds);
            timeval stv{2, 0};
            int r = select(cfd + 1, &rfds, nullptr, nullptr, &stv);
            if (r < 0) break;
            if (r == 0) continue;
            std::string frame = ws_util::ws_recv_frame(cfd);
            if (frame.empty()) break;
        }

        {
            std::lock_guard<std::mutex> lk(clients_mu_);
            clients_.erase(cfd);
        }
        close(cfd);
    }

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    mutable std::mutex clients_mu_;
    std::set<int> clients_;
};