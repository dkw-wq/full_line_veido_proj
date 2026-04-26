class RelayServer {
public:
    explicit RelayServer(Config cfg)
        : cfg_(std::move(cfg)),
          validator_(cfg_.require_h264, cfg_.require_audio),
          global_controller_(cfg_) {}
    ~RelayServer() { stop(); }

    bool start() {
        if (srt_startup() != 0) {
            log_line("ERROR", "srt_startup: " + last_srt_error());
            return false;
        }

        pub_listener_ = create_listener(cfg_.publisher_port, true);
        sub_listener_ = create_listener(cfg_.subscriber_port, false);
        if (pub_listener_ == SRT_INVALID_SOCK || sub_listener_ == SRT_INVALID_SOCK) return false;

        if (cfg_.ws_port > 0 && !ws_.start(cfg_.bind_ip, cfg_.ws_port)) {
            log_line("WARN", "WebSocket broadcaster failed to start, continuing without it");
        }

        t_pub_acc_ = std::thread([this]{ accept_publisher_loop(); });
        t_sub_acc_ = std::thread([this]{ accept_subscriber_loop(); });
        t_monitor_ = std::thread([this]{ monitor_loop(); });

        tlmy_relay_.start(cfg_.bind_ip,
            [this](const std::string& peer_ip,
                   double push_to_relay_ms,
                   double relay_to_ack_ms,
                   double push_to_ack_ms) {
                dash_push_to_relay_ms_.store(push_to_relay_ms);
                dash_relay_to_ack_ms_.store(relay_to_ack_ms);
                dash_push_to_ack_ms_.store(push_to_ack_ms);
                dash_last_ack_ms_.store(steady_ms());

                std::lock_guard<std::mutex> lk(sub_mu_);
                for (auto& [id, sess] : subscribers_) {
                    std::string peer = sess->peer();
                    auto colon = peer.rfind(':');
                    std::string ip = (colon == std::string::npos) ? peer : peer.substr(0, colon);
                    if (ip == peer_ip) {
                        sess->updateTelemetryLatency(push_to_relay_ms, relay_to_ack_ms, push_to_ack_ms);
                    }
                }
            }
        );

        log_line("INFO", "relay started  pub=" + std::to_string(cfg_.publisher_port) +
                 "  sub=" + std::to_string(cfg_.subscriber_port) +
                 "  ws=" + std::to_string(cfg_.ws_port));
        return true;
    }

    void stop() {
        tlmy_relay_.stop();

        bool exp = true;
        if (!stopped_.compare_exchange_strong(exp, false)) return;
        g_running.store(false);
        ws_.stop();
        srt_close(pub_listener_); pub_listener_ = SRT_INVALID_SOCK;
        srt_close(sub_listener_); sub_listener_ = SRT_INVALID_SOCK;
        close_publisher();
        close_all_subscribers();
        for (auto* t : {&t_pub_acc_, &t_sub_acc_, &t_monitor_})
            if (t->joinable()) t->join();
        if (t_pub_read_.joinable()) t_pub_read_.join();
        srt_cleanup();
        log_line("INFO", "relay stopped");
    }

private:
    SRTSOCKET create_listener(int port, bool for_pub) {
        SRTSOCKET s = srt_socket(AF_INET, SOCK_DGRAM, 0);
        if (s == SRT_INVALID_SOCK) return SRT_INVALID_SOCK;
        auto sf = [&](SRT_SOCKOPT opt, const void* v, int l, const char* n) {
            if (srt_setsockflag(s, opt, v, l) != 0)
                log_line("WARN", std::string("set ") + n + ": " + last_srt_error());
        };
        int yes=1, lat=cfg_.latency_ms, rb=cfg_.recv_buf_bytes, sb=cfg_.send_buf_bytes;
        int tt=SRTT_LIVE, rto=for_pub?cfg_.publisher_idle_timeout_ms:5000;

        sf(SRTO_TSBPDMODE, &yes, sizeof(yes), "TSBPDMODE");
        sf(SRTO_TRANSTYPE, &tt, sizeof(tt), "TRANSTYPE");

        sf(SRTO_RCVLATENCY, &lat, sizeof(lat), "RCVLATENCY");
        sf(SRTO_PEERLATENCY, &lat, sizeof(lat), "PEERLATENCY");
        sf(SRTO_LATENCY, &lat, sizeof(lat), "LATENCY");

        sf(SRTO_RCVBUF, &rb, sizeof(rb), "RCVBUF");
        sf(SRTO_SNDBUF, &sb, sizeof(sb), "SNDBUF");
        sf(SRTO_RCVTIMEO, &rto, sizeof(rto), "RCVTIMEO");

        if (cfg_.passphrase && !cfg_.passphrase->empty()) {
            int pk = cfg_.pbkeylen;
            sf(SRTO_PASSPHRASE,cfg_.passphrase->c_str(),(int)cfg_.passphrase->size(),"PASSPHRASE");
            sf(SRTO_PBKEYLEN,&pk,sizeof(pk),"PBKEYLEN");
        }
        sockaddr_in addr{}; addr.sin_family=AF_INET;
        addr.sin_port=htons((uint16_t)port);
        inet_pton(AF_INET, cfg_.bind_ip.c_str(), &addr.sin_addr);
        if (srt_bind(s,(sockaddr*)&addr,sizeof(addr))==SRT_ERROR ||
            srt_listen(s,64)==SRT_ERROR) {
            log_line("ERROR","srt bind/listen :"+std::to_string(port)+": "+last_srt_error());
            srt_close(s); return SRT_INVALID_SOCK;
        }
        return s;
    }

    void accept_publisher_loop() {
        while (g_running.load()) {
            sockaddr_in peer{}; int pl=sizeof(peer);
            SRTSOCKET sock = srt_accept(pub_listener_,(sockaddr*)&peer,&pl);
            if (sock == SRT_INVALID_SOCK) {
                if (g_running.load()) log_line("WARN","pub accept: "+last_srt_error());
                break;
            }
            {
                std::lock_guard<std::mutex> lk(pub_mu_);
                if (pub_sock_ != SRT_INVALID_SOCK) {
                    log_line("WARN","reject extra publisher from "+sockaddr_to_string(peer));
                    srt_close(sock); continue;
                }
                pub_sock_ = sock; pub_peer_ = peer;
                status_.publisher_online.store(true);
                status_.publisher_connected_at_ms.store(steady_ms());
                status_.publisher_last_active_at_ms.store(steady_ms());
            }

            log_line("INFO", "publisher connected: " + sockaddr_to_string(pub_peer_));
            dump_srt_latency("[PUB]", pub_sock_);

            t_pub_read_ = std::thread([this]{ publisher_read_loop(); });
            if (t_pub_read_.joinable()) t_pub_read_.join();
            close_publisher();
        }
    }

    void publisher_read_loop() {
        std::vector<char> buf(cfg_.payload_size);
        while (g_running.load()) {
            SRTSOCKET sock;
            { std::lock_guard<std::mutex> lk(pub_mu_); sock = pub_sock_; }
            if (sock == SRT_INVALID_SOCK) break;
            int n = srt_recv(sock, buf.data(), (int)buf.size());
            if (n == SRT_ERROR) {
                int e = srt_getlasterror(nullptr);
                if (e == SRT_ETIMEOUT) {
                    if (steady_ms()-status_.publisher_last_active_at_ms.load() > cfg_.publisher_idle_timeout_ms) {
                        log_line("WARN","publisher idle timeout"); break;
                    }
                    continue;
                }
                log_line("WARN","pub recv: "+last_srt_error()); break;
            }
            if (n == 0) continue;
            status_.publisher_last_active_at_ms.store(steady_ms());
            status_.total_ingress_bytes.fetch_add((uint64_t)n);
            status_.ingress_bytes_window.fetch_add((uint64_t)n);
            status_.ingress_chunks.fetch_add(1);
            tlmy_relay_.observe_chunk((const uint8_t*)buf.data(), (size_t)n);
            if (cfg_.validate_ts) {
                if (!validator_.inspect((const uint8_t*)buf.data(), (size_t)n, status_)) {
                    log_line("WARN","TS: "+validator_.last_error()); continue;
                }
            }
            broadcast((const uint8_t*)buf.data(), (size_t)n);
        }
    }

    void accept_subscriber_loop() {
        while (g_running.load()) {
            sockaddr_in peer{}; int pl=sizeof(peer);
            SRTSOCKET sock = srt_accept(sub_listener_,(sockaddr*)&peer,&pl);
            if (sock == SRT_INVALID_SOCK) {
                if (g_running.load()) log_line("WARN","sub accept: "+last_srt_error());
                break;
            }
            {
                std::lock_guard<std::mutex> lk(sub_mu_);
                if (subscribers_.size() >= (size_t)cfg_.max_subscribers) {
                    log_line("WARN","max_subscribers reached, reject "+sockaddr_to_string(peer));
                    srt_close(sock); continue;
                }
            }
            uint64_t id = next_sub_id_.fetch_add(1);
            auto sess = std::make_shared<SubscriberSession>(sock,peer,status_,cfg_,id);
            { std::lock_guard<std::mutex> lk(sub_mu_); subscribers_[id] = sess; }
            sess->start();
        }
    }

    void broadcast(const uint8_t* data, size_t len) {
        std::vector<uint64_t> dead;
        {
            std::lock_guard<std::mutex> lk(sub_mu_);
            for (auto& [id,s] : subscribers_) {
                if (!s->running()) { dead.push_back(id); continue; }
                if (!s->push_chunk(data,len)) dead.push_back(id);
            }
            for (uint64_t id : dead) {
                auto it = subscribers_.find(id);
                if (it != subscribers_.end()) { it->second->stop(); subscribers_.erase(it); }
            }
        }
    }

    void close_publisher() {
        std::lock_guard<std::mutex> lk(pub_mu_);
        if (pub_sock_ != SRT_INVALID_SOCK) { srt_close(pub_sock_); pub_sock_ = SRT_INVALID_SOCK; }
        status_.publisher_online.store(false);
    }

    void close_all_subscribers() {
        std::lock_guard<std::mutex> lk(sub_mu_);
        for (auto& [_,s] : subscribers_) s->stop();
        subscribers_.clear();
    }

    std::string current_pusher_control_host() {
        if (!cfg_.pusher_control_host.empty()) return cfg_.pusher_control_host;

        std::lock_guard<std::mutex> lk(pub_mu_);
        if (pub_sock_ == SRT_INVALID_SOCK || pub_peer_.sin_addr.s_addr == 0) return {};
        char ip[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &pub_peer_.sin_addr, ip, sizeof(ip)) == nullptr) {
            return {};
        }
        return std::string(ip);
    }

    bool send_pusher_command(const std::string& command) {
        if (!cfg_.auto_control) return false;
        std::string host = current_pusher_control_host();
        if (host.empty()) return false;
        bool ok = pusher_control_.send_command(host, cfg_.pusher_control_port, command);
        if (!ok) {
            log_line("WARN", "pusher control failed: " + host + ":" +
                     std::to_string(cfg_.pusher_control_port) + " cmd=" + command);
        }
        return ok;
    }

    void execute_global_decision(const GlobalDecision& decision) {
        if (decision.action == GlobalControlAction::Noop) return;

        std::ostringstream oss;
        oss << "global controller -> " << global_action_name(decision.action);
        if (!decision.reason.empty()) oss << " (" << decision.reason << ")";
        log_line(decision.action == GlobalControlAction::Idr ? "INFO" : "WARN", oss.str());

        switch (decision.action) {
            case GlobalControlAction::Idr:
                send_pusher_command("IDR");
                return;

            case GlobalControlAction::DegradeBitrate:
                if (decision.bitrate_kbps > 0)
                    send_pusher_command("SET_BITRATE " + std::to_string(decision.bitrate_kbps));
                break;

            case GlobalControlAction::DegradeFps:
                if (decision.fps > 0)
                    send_pusher_command("SET_FPS " + std::to_string(decision.fps));
                break;

            case GlobalControlAction::DegradeResolution:
                if (decision.width > 0 && decision.height > 0)
                    send_pusher_command("SET_RESOLUTION " + std::to_string(decision.width) +
                                        " " + std::to_string(decision.height));
                break;

            case GlobalControlAction::EnterPlaceholder:
                send_pusher_command("VIDEO_MODE BLACK");
                send_pusher_command("AUDIO_MODE SILENT");
                break;

            case GlobalControlAction::RecoverStep:
                if (decision.exit_placeholder) {
                    send_pusher_command("VIDEO_MODE NORMAL");
                    send_pusher_command("AUDIO_MODE NORMAL");
                }
                if (decision.bitrate_kbps > 0)
                    send_pusher_command("SET_BITRATE " + std::to_string(decision.bitrate_kbps));
                if (decision.fps > 0)
                    send_pusher_command("SET_FPS " + std::to_string(decision.fps));
                if (decision.width > 0 && decision.height > 0)
                    send_pusher_command("SET_RESOLUTION " + std::to_string(decision.width) +
                                        " " + std::to_string(decision.height));
                break;

            case GlobalControlAction::Noop:
                return;
        }

        if (decision.send_idr) {
            send_pusher_command("IDR");
        }
    }

    void monitor_loop() {
        static constexpr int HIST = 60;
        std::deque<double> ingress_hist, egress_hist;

        while (g_running.load()) {
            std::this_thread::sleep_for(Ms(cfg_.monitor_interval_ms));
            if (!g_running.load()) break;

            const int64_t now_ms = steady_ms();
            uint64_t ib = status_.ingress_bytes_window.exchange(0);
            uint64_t eb = status_.egress_bytes_window.exchange(0);
            double sec = cfg_.monitor_interval_ms / 1000.0;
            double in_mbps  = (ib * 8.0) / sec / 1e6;
            double out_mbps = (eb * 8.0) / sec / 1e6;

            ingress_hist.push_back(in_mbps);
            egress_hist.push_back(out_mbps);
            if ((int)ingress_hist.size() > HIST) ingress_hist.pop_front();
            if ((int)egress_hist.size()  > HIST) egress_hist.pop_front();

            double dash_push_to_relay_ms = dash_push_to_relay_ms_.load();
            double dash_relay_to_ack_ms  = dash_relay_to_ack_ms_.load();
            double dash_push_to_ack_ms   = dash_push_to_ack_ms_.load();
            const int64_t dash_last_ack_age_ms =
                dash_last_ack_ms_.load() > 0 ? now_ms - dash_last_ack_ms_.load() : -1;
            const int64_t pub_idle_ms =
                status_.publisher_last_active_at_ms.load() > 0
                    ? now_ms - status_.publisher_last_active_at_ms.load()
                    : -1;
            const uint64_t queue_drops = status_.subscriber_queue_drops.load();

            int subscriber_total = 0;
            int bad_subscribers = 0;
            std::vector<uint64_t> disconnect_subscribers;
            {
                std::lock_guard<std::mutex> lk(sub_mu_);
                for (auto& [id, sess] : subscribers_) {
                    SubscriberControlResult r = sess->evaluate_control(now_ms);
                    if (r.changed) {
                        log_line(r.action == SubscriberAction::Normal ? "INFO" : "WARN",
                                 "sub#" + std::to_string(id) + " controller -> " +
                                 subscriber_action_name(r.action));
                    }
                    if (r.disconnect) {
                        disconnect_subscribers.push_back(id);
                        continue;
                    }
                    ++subscriber_total;
                    if (sess->bad_for_global()) ++bad_subscribers;
                }

                for (uint64_t id : disconnect_subscribers) {
                    auto it = subscribers_.find(id);
                    if (it == subscribers_.end()) continue;
                    log_line("WARN", "sub#" + std::to_string(id) +
                             " disconnected after placeholder hold");
                    it->second->stop();
                    subscribers_.erase(it);
                }
            }

            const double bad_subscriber_ratio =
                subscriber_total > 0 ? (double)bad_subscribers / (double)subscriber_total : 0.0;

            GlobalControllerInput global_in;
            global_in.subscriber_count = subscriber_total;
            global_in.bad_subscriber_ratio = bad_subscriber_ratio;
            global_in.dash_push_to_relay_ms = dash_push_to_relay_ms;
            global_in.dash_relay_to_ack_ms = dash_relay_to_ack_ms;
            global_in.dash_push_to_ack_ms = dash_push_to_ack_ms;
            global_in.dash_last_ack_ms = dash_last_ack_age_ms;
            global_in.queue_drops = queue_drops;
            global_in.pub_idle_ms = pub_idle_ms;
            GlobalDecision global_decision = global_controller_.update(global_in, now_ms);
            execute_global_decision(global_decision);

            std::ostringstream log_oss;
            log_oss << std::fixed << std::setprecision(2)
                    << "push->relay=" << dash_push_to_relay_ms << "ms"
                    << " push->ack="  << dash_push_to_ack_ms << "ms"
                    << " relay->ack=" << dash_relay_to_ack_ms << "ms"
                    << " pub=" << (status_.publisher_online.load() ? 1 : 0)
                    << " in=" << in_mbps << "Mbps"
                    << " out=" << out_mbps << "Mbps"
                    << " subs=" << status_.subscriber_count.load()
                    << " disc=" << status_.ts_discontinuities.load()
                    << " drops=" << queue_drops
                    << " bad_ratio=" << bad_subscriber_ratio;
            //log_line("STAT", log_oss.str());

            if (cfg_.ws_port > 0 && ws_.client_count() > 0) {
                std::ostringstream j;
                j << std::fixed << std::setprecision(3);
                j << "{"
                  << "\"ts\":" << now_ms << ","
                  << "\"wall_time\":\"" << now_wall_time() << "\"," 
                  << "\"pub_online\":" << (status_.publisher_online.load() ? "true" : "false") << ","
                  << "\"ingress_mbps\":" << in_mbps << ","
                  << "\"egress_mbps\":" << out_mbps << ","
                  << "\"subscribers\":" << status_.subscriber_count.load() << ","
                  << "\"total_ingress_mb\":" << (status_.total_ingress_bytes.load() / 1048576.0) << ","
                  << "\"total_egress_mb\":" << (status_.total_egress_bytes.load() / 1048576.0) << ","
                  << "\"ingress_chunks\":" << status_.ingress_chunks.load() << ","
                  << "\"ts_disc\":" << status_.ts_discontinuities.load() << ","
                  << "\"queue_drops\":" << queue_drops << ","
                  << "\"pub_idle_ms\":" << pub_idle_ms << ","
                  << "\"ws_clients\":" << ws_.client_count() << ","
                  << "\"dash_push_to_relay_ms\":" << dash_push_to_relay_ms << ","
                  << "\"dash_relay_to_ack_ms\":" << dash_relay_to_ack_ms << ","
                  << "\"dash_push_to_ack_ms\":" << dash_push_to_ack_ms << ","
                  << "\"dash_last_ack_ms\":" << dash_last_ack_age_ms << ","
                  << "\"bad_subscriber_ratio\":" << bad_subscriber_ratio << ","
                  << "\"global_state\":\"" << global_controller_.state_name() << "\","
                  << "\"global_action\":\"" << global_action_name(global_decision.action) << "\","
                  << "\"global_bitrate_kbps\":" << global_controller_.bitrate_kbps() << ","
                  << "\"global_fps\":" << global_controller_.fps() << ","
                  << "\"global_width\":" << global_controller_.width() << ","
                  << "\"global_height\":" << global_controller_.height() << ","
                  << "\"ingress_hist\":[";

                for (size_t i = 0; i < ingress_hist.size(); ++i) {
                    if (i) j << ",";
                    j << ingress_hist[i];
                }
                j << "],\"egress_hist\":[";
                for (size_t i = 0; i < egress_hist.size(); ++i) {
                    if (i) j << ",";
                    j << egress_hist[i];
                }
                j << "],\"sub_list\":[";
                {
                    std::lock_guard<std::mutex> lk(sub_mu_);
                    int idx = 0;
                    for (auto& [id, sess] : subscribers_) {
                        if (idx) j << ",";
                        j << "{"
                          << "\"id\":" << id << ","
                          << "\"peer\":\"" << sess->peer() << "\","
                          << "\"sent_mb\":" << (sess->sent_bytes() / 1048576.0) << ","
                          << "\"q\":" << sess->queue_depth() << ","
                          << "\"latency_ms\":" << sess->latency_ms() << ","
                          << "\"telemetry_push_to_relay_ms\":" << sess->telemetryPushToRelayMs() << ","
                          << "\"telemetry_relay_to_ack_ms\":" << sess->telemetryRelayToAckMs() << ","
                          << "\"telemetry_push_to_ack_ms\":" << sess->telemetryPushToAckMs() << ","
                          << "\"telemetry_last_ack_ms\":" << (sess->telemetryLastAckMs() > 0 ? now_ms - sess->telemetryLastAckMs() : -1) << ","
                          << "\"idle_ms\":" << (now_ms - sess->last_active_ms()) << ","
                          << "\"ctrl\":\"" << sess->control_action_name() << "\""
                          << "}";
                        ++idx;
                    }
                }
                j << "]}";
                ws_.broadcast(j.str());
            }
        }
    }

    Config       cfg_;
    StreamStatus status_;
    TsValidator  validator_;
    GlobalController global_controller_;
    PusherControlClient pusher_control_;
    WsBroadcaster ws_;
    TelemetryRelay tlmy_relay_;

    std::atomic<bool> stopped_{true};
    std::atomic<double> dash_push_to_relay_ms_{-1.0};
    std::atomic<double> dash_relay_to_ack_ms_{-1.0};
    std::atomic<double> dash_push_to_ack_ms_{-1.0};
    std::atomic<int64_t> dash_last_ack_ms_{-1};

    SRTSOCKET pub_listener_ = SRT_INVALID_SOCK;
    SRTSOCKET sub_listener_ = SRT_INVALID_SOCK;

    std::mutex  pub_mu_;
    SRTSOCKET   pub_sock_ = SRT_INVALID_SOCK;
    sockaddr_in pub_peer_{};

    std::mutex  sub_mu_;
    std::map<uint64_t, std::shared_ptr<SubscriberSession>> subscribers_;
    std::atomic<uint64_t> next_sub_id_{1};

    std::thread t_pub_acc_, t_sub_acc_, t_pub_read_, t_monitor_;
};