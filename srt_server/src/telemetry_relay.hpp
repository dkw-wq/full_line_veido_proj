class TelemetryRelay {
public:
    TelemetryRelay() = default;
    ~TelemetryRelay() { stop(); }

    using AckUpdateFn = std::function<void(const std::string&, double, double, double)>;

    bool start(const std::string& bind_ip, AckUpdateFn ack_update_fn) {
        ack_update_fn_ = std::move(ack_update_fn);

        ack_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (ack_fd_ < 0) {
            log_line("ERROR", "TLMY ack socket failed");
            return false;
        }

        int yes = 1;
        setsockopt(ack_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in ack_addr{};
        ack_addr.sin_family = AF_INET;
        ack_addr.sin_port   = htons((uint16_t)TLMY_ACK_PORT);
        inet_pton(AF_INET,
                  (bind_ip.empty() || bind_ip == "0.0.0.0") ? "0.0.0.0" : bind_ip.c_str(),
                  &ack_addr.sin_addr);

        if (bind(ack_fd_, (sockaddr*)&ack_addr, sizeof(ack_addr)) < 0) {
            log_line("ERROR", "TLMY ACK bind :" + std::to_string(TLMY_ACK_PORT) + " failed");
            close(ack_fd_);
            ack_fd_ = -1;
            return false;
        }

        running_.store(true);
        ack_thread_ = std::thread([this]{ ack_loop(); });

        log_line("INFO", "TelemetryRelay tracking TS private section, ack :" + std::to_string(TLMY_ACK_PORT));
        return true;
    }

    void stop() {
        running_.store(false);
        if (ack_fd_ >= 0) {
            close(ack_fd_);
            ack_fd_ = -1;
        }
        if (ack_thread_.joinable()) {
            ack_thread_.join();
        }

        std::lock_guard<std::mutex> lk(seen_mu_);
        seen_.clear();
        order_.clear();
    }

    void observe_chunk(const uint8_t* data, size_t len) {
        if (!data || len < 188) return;

        if (len % 188 == 0) {
            for (size_t off = 0; off + 188 <= len; off += 188) {
                uint64_t t_push_us = 0;
                uint64_t seq = 0;
                if (tlmy_ts_parse(data + off, &t_push_us, &seq)) {
                    remember(seq, t_push_us);
                }
            }
            return;
        }

        for (size_t off = 0; off + 188 <= len; ++off) {
            if (data[off] != 0x47u) continue;
            uint64_t t_push_us = 0;
            uint64_t seq = 0;
            if (tlmy_ts_parse(data + off, &t_push_us, &seq)) {
                remember(seq, t_push_us);
                off += 187;
            }
        }
    }

private:
    struct SeenEntry {
        uint64_t t_push_us = 0;
        uint64_t relay_seen_wall_us = 0;
        uint64_t relay_seen_mono_us = 0;
    };

    void remember(uint64_t seq, uint64_t t_push_us) {
        SeenEntry e;
        e.t_push_us          = t_push_us;
        e.relay_seen_wall_us = wall_us();
        e.relay_seen_mono_us = mono_us();

        std::lock_guard<std::mutex> lk(seen_mu_);
        auto it = seen_.find(seq);
        if (it == seen_.end()) {
            seen_[seq] = e;
            order_.push_back(seq);
        } else {
            it->second = e;
        }

        while (order_.size() > 8192) {
            uint64_t old_seq = order_.front();
            order_.pop_front();
            seen_.erase(old_seq);
        }
    }

    void ack_loop() {
        while (running_.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ack_fd_, &rfds);
            timeval tv{1, 0};
            if (select(ack_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

            TelemetryAck raw{}, ack{};
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(ack_fd_, &raw, sizeof(raw), 0,
                                 (sockaddr*)&from, &from_len);
            if (n != (ssize_t)sizeof(TelemetryAck)) continue;
            if (!tlmy_ack_decode(&raw, (int)n, &ack)) continue;

            SeenEntry entry;
            {
                std::lock_guard<std::mutex> lk(seen_mu_);
                auto it = seen_.find(ack.seq);
                if (it == seen_.end()) continue;
                entry = it->second;
            }

            const uint64_t now_wall_us = wall_us();
            const uint64_t now_mono_us = mono_us();

            const double push_to_relay_ms =
                ((double)((int64_t)entry.relay_seen_wall_us - (int64_t)entry.t_push_us)) / 1000.0;
            const double relay_to_ack_ms =
                ((double)((int64_t)now_mono_us - (int64_t)entry.relay_seen_mono_us)) / 1000.0;
            const double push_to_ack_ms =
                ((double)((int64_t)now_wall_us - (int64_t)entry.t_push_us)) / 1000.0;

            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));

            std::ostringstream oss;
            /*
            oss << std::fixed << std::setprecision(3)
                << "[LAT-ACK] seq=" << ack.seq
                << " peer=" << ipbuf
                << " push->relay=" << push_to_relay_ms << "ms"
                << " relay->ack=" << relay_to_ack_ms << "ms"
                << " push->ack=" << push_to_ack_ms << "ms";
            log_line("INFO", oss.str());
            */
            if (ack_update_fn_) {
                ack_update_fn_(std::string(ipbuf), push_to_relay_ms, relay_to_ack_ms, push_to_ack_ms);
            }
        }
    }

    int ack_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread ack_thread_;

    AckUpdateFn ack_update_fn_;

    std::mutex seen_mu_;
    std::unordered_map<uint64_t, SeenEntry> seen_;
    std::deque<uint64_t> order_;
};