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

        epoll_id_ = srt_epoll_create();
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        int wake_events = SRT_EPOLL_IN;
        if (epoll_id_ < 0 || wake_fd_ < 0 ||
            srt_epoll_add_ssock(epoll_id_, wake_fd_, &wake_events) == SRT_ERROR) {
            log_line("ERROR", "cannot initialize subscriber SRT epoll");
            if (wake_fd_ >= 0) close(wake_fd_);
            if (epoll_id_ >= 0) srt_epoll_release(epoll_id_);
            srt_close(pub_listener_);
            srt_close(sub_listener_);
            pub_listener_ = sub_listener_ = SRT_INVALID_SOCK;
            srt_cleanup();
            return false;
        }

        if (cfg_.ws_port > 0 && !ws_.start(cfg_.bind_ip, cfg_.ws_port)) {
            log_line("WARN", "WebSocket broadcaster failed to start, continuing without it");
        }

        t_pub_acc_ = std::thread([this]{ accept_publisher_loop(); });
        t_sub_acc_ = std::thread([this]{ accept_subscriber_loop(); });
        t_sub_send_ = std::thread([this]{ subscriber_send_loop(); });
        t_monitor_ = std::thread([this]{ monitor_loop(); });

        log_line("INFO", "relay started  pub=" + std::to_string(cfg_.publisher_port) +
                 "  sub=" + std::to_string(cfg_.subscriber_port) +
                 "  ws=" + std::to_string(cfg_.ws_port));
        return true;
    }

    void stop() {
        

        bool exp = true;
        if (!stopped_.compare_exchange_strong(exp, false)) return;
        g_running.store(false);
        ws_.stop();
        wake_sender();
        srt_close(pub_listener_); pub_listener_ = SRT_INVALID_SOCK;
        srt_close(sub_listener_); sub_listener_ = SRT_INVALID_SOCK;
        close_publisher();
        for (auto* t : {&t_pub_acc_, &t_sub_acc_, &t_monitor_})
            if (t->joinable()) t->join();
        if (t_pub_read_.joinable()) t_pub_read_.join();
        if (t_sub_send_.joinable()) t_sub_send_.join();
        close_all_subscribers();
        if (wake_fd_ >= 0) { close(wake_fd_); wake_fd_ = -1; }
        if (epoll_id_ >= 0) { srt_epoll_release(epoll_id_); epoll_id_ = -1; }
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
            int nonblocking = 0;
            if (srt_setsockflag(sock, SRTO_SNDSYN, &nonblocking, sizeof(nonblocking)) == SRT_ERROR) {
                log_line("WARN", "subscriber nonblocking mode: " + last_srt_error());
                srt_close(sock);
                continue;
            }
            uint64_t id = next_sub_id_.fetch_add(1);
            auto sess = std::make_shared<SubscriberSession>(sock,peer,status_,cfg_,id);
            { std::lock_guard<std::mutex> lk(sub_mu_); subscribers_[id] = sess; }
            wake_sender();
        }
    }

    void broadcast(const uint8_t* data, size_t len) {
        const bool rap = SubscriberSession::has_rap(data, len);
        auto chunk = std::make_shared<const StreamChunk>(data, len);
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(sub_mu_);
            for (auto& [id,s] : subscribers_) {
                if (!s->running()) continue;
                if (!s->push_chunk(chunk, rap)) s->request_close();
                else queued = true;
            }
        }
        if (queued) wake_sender();
    }

    void wake_sender() {
        if (wake_fd_ < 0) return;
        uint64_t one = 1;
        const ssize_t ignored = write(wake_fd_, &one, sizeof(one));
        (void)ignored;
    }

    void subscriber_send_loop() {
        struct Registered {
            std::shared_ptr<SubscriberSession> session;
            bool writable = false;
        };
        std::unordered_map<SRTSOCKET, Registered> registered;
        const int capacity = std::max(8, cfg_.max_subscribers + 8);
        std::vector<SRTSOCKET> readfds(capacity), writefds(capacity);
        SYSSOCKET system_readfds[1];
        bool needs_sync = true;

        while (g_running.load()) {
            if (needs_sync) {
                needs_sync = false;
                std::vector<std::shared_ptr<SubscriberSession>> sessions;
                {
                    std::lock_guard<std::mutex> lk(sub_mu_);
                    for (auto& [_, session] : subscribers_) sessions.push_back(session);
                }
                for (const auto& session : sessions) {
                    const SRTSOCKET sock = session->socket();
                    const SRT_SOCKSTATUS state = srt_getsockstate(sock);
                    if (session->closing() || state == SRTS_BROKEN || state == SRTS_CLOSING ||
                        state == SRTS_CLOSED || state == SRTS_NONEXIST) {
                        if (registered.erase(sock)) srt_epoll_remove_usock(epoll_id_, sock);
                        {
                            std::lock_guard<std::mutex> lk(sub_mu_);
                            auto it = subscribers_.find(session->id());
                            if (it != subscribers_.end() && it->second == session) subscribers_.erase(it);
                        }
                        session->stop();
                        continue;
                    }

                    bool wants_write = session->queue_depth() > 0;
                    int events = SRT_EPOLL_ERR | (wants_write ? SRT_EPOLL_OUT : 0);
                    auto it = registered.find(sock);
                    if (it == registered.end()) {
                        if (srt_epoll_add_usock(epoll_id_, sock, &events) == SRT_ERROR) {
                            log_line("WARN", "sub#" + std::to_string(session->id()) +
                                     " epoll add: " + last_srt_error());
                            session->request_close();
                        } else {
                            registered.emplace(sock, Registered{session, wants_write});
                        }
                    } else if (it->second.writable != wants_write) {
                        if (srt_epoll_update_usock(epoll_id_, sock, &events) == SRT_ERROR) {
                            log_line("WARN", "sub#" + std::to_string(session->id()) +
                                     " epoll update: " + last_srt_error());
                            session->request_close();
                        } else {
                            it->second.writable = wants_write;
                        }
                    }
                }
            }

            int rnum = capacity, wnum = capacity, srnum = 1;
            int rc = srt_epoll_wait(epoll_id_, readfds.data(), &rnum,
                                    writefds.data(), &wnum, 500,
                                    system_readfds, &srnum, nullptr, nullptr);
            if (rc == SRT_ERROR) {
                if (srt_getlasterror(nullptr) != SRT_ETIMEOUT && g_running.load())
                    log_line("WARN", "subscriber epoll: " + last_srt_error());
                needs_sync = true;
                continue;
            }
            if (rc == 0) needs_sync = true;
            if (srnum > 0) {
                uint64_t value;
                while (read(wake_fd_, &value, sizeof(value)) == sizeof(value)) {}
                needs_sync = true;
            }
            for (int i = 0; i < rnum; ++i) {
                auto it = registered.find(readfds[i]);
                if (it != registered.end()) {
                    it->second.session->request_close();
                    needs_sync = true;
                }
            }
            for (int i = 0; i < wnum; ++i) {
                auto it = registered.find(writefds[i]);
                if (it == registered.end() || it->second.session->closing()) continue;
                if (srt_getsockstate(writefds[i]) != SRTS_CONNECTED) {
                    it->second.session->request_close();
                    needs_sync = true;
                    continue;
                }
                // Bound work per socket so a busy client cannot starve the others.
                for (int sent = 0; sent < 32; ++sent) {
                    auto result = it->second.session->send_one();
                    if (result != SubscriberSession::SendResult::Sent) break;
                }
                if (it->second.session->closing()) {
                    needs_sync = true;
                } else if (it->second.writable && it->second.session->queue_depth() == 0) {
                    int events = SRT_EPOLL_ERR;
                    if (srt_epoll_update_usock(epoll_id_, writefds[i], &events) == SRT_ERROR) {
                        it->second.session->request_close();
                        needs_sync = true;
                    } else {
                        it->second.writable = false;
                    }
                }
            }
        }
        for (auto& [sock, _] : registered) srt_epoll_remove_usock(epoll_id_, sock);
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

            const int64_t pub_idle_ms =
                status_.publisher_last_active_at_ms.load() > 0
                    ? now_ms - status_.publisher_last_active_at_ms.load()
                    : -1;
            const uint64_t queue_drops = status_.subscriber_queue_drops.load();
            const HostMetrics host = host_monitor_.sample();

            int subscriber_total = 0;
            int bad_subscribers = 0;
            std::vector<uint64_t> disconnect_subscribers;
            struct SubSample {
                uint64_t id;
                std::string peer;
                double sent_mb;
                int queue_depth;
                int latency_ms;
                int64_t idle_ms;
                std::string control;
                SubscriberSession::SrtMetrics srt;
            };
            std::vector<SubSample> sub_samples;
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
                    sub_samples.push_back({id, sess->peer(), sess->sent_bytes() / 1048576.0,
                                           sess->queue_depth(), sess->latency_ms(),
                                           now_ms - sess->last_active_ms(),
                                           sess->control_action_name(), sess->srt_metrics()});
                }

                for (uint64_t id : disconnect_subscribers) {
                    auto it = subscribers_.find(id);
                    if (it == subscribers_.end()) continue;
                    log_line("WARN", "sub#" + std::to_string(id) +
                             " disconnected after placeholder hold");
                    it->second->request_close();
                }
            }
            if (!disconnect_subscribers.empty()) wake_sender();

            const double bad_subscriber_ratio =
                subscriber_total > 0 ? (double)bad_subscribers / (double)subscriber_total : 0.0;

            GlobalControllerInput global_in;
            global_in.subscriber_count = subscriber_total;
            global_in.bad_subscriber_ratio = bad_subscriber_ratio;
            global_in.queue_drops = queue_drops;
            global_in.pub_idle_ms = pub_idle_ms;
            GlobalDecision global_decision = global_controller_.update(global_in, now_ms);
            execute_global_decision(global_decision);

            std::ostringstream log_oss;
            log_oss << std::fixed << std::setprecision(2)
                    << " pub=" << (status_.publisher_online.load() ? 1 : 0)
                    << " in=" << in_mbps << "Mbps"
                    << " out=" << out_mbps << "Mbps"
                    << " subs=" << status_.subscriber_count.load()
                    << " disc=" << status_.ts_discontinuities.load()
                    << " drops=" << queue_drops
                    << " bad_ratio=" << bad_subscriber_ratio
                    << " nic=" << (host.nic.empty() ? "unknown" : host.nic)
                    << " nic_rx=" << host.nic_rx_mbps << "Mbps"
                    << " nic_tx=" << host.nic_tx_mbps << "Mbps"
                    << " cpu=" << host.cpu_percent << "%"
                    << " mem_avail=" << host.mem_available_mb << "MB"
                    << " rss=" << host.process_rss_mb << "MB";
            log_line("STAT", log_oss.str());
            for (const auto& sub : sub_samples) {
                std::ostringstream sub_log;
                sub_log << std::fixed << std::setprecision(2)
                        << "sub#" << sub.id << " peer=" << sub.peer
                        << " q=" << sub.queue_depth << " idle_ms=" << sub.idle_ms
                        << " sndbuf_pkts=" << sub.srt.send_buf_packets
                        << " avail_sndbuf_bytes=" << sub.srt.avail_send_buf_bytes
                        << " retrans=" << sub.srt.retrans_total
                        << " snd_loss=" << sub.srt.send_loss_total
                        << " rtt_ms=" << sub.srt.rtt_ms;
                log_line("SUBSTAT", sub_log.str());
            }

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
                  << "\"bad_subscriber_ratio\":" << bad_subscriber_ratio << ","
                  << "\"host_nic\":\"" << host.nic << "\","
                  << "\"host_nic_rx_mbps\":" << host.nic_rx_mbps << ","
                  << "\"host_nic_tx_mbps\":" << host.nic_tx_mbps << ","
                  << "\"host_cpu_percent\":" << host.cpu_percent << ","
                  << "\"host_mem_available_mb\":" << host.mem_available_mb << ","
                  << "\"relay_rss_mb\":" << host.process_rss_mb << ","
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
                    for (size_t idx = 0; idx < sub_samples.size(); ++idx) {
                        const auto& sub = sub_samples[idx];
                        if (idx) j << ",";
                        j << "{"
                          << "\"id\":" << sub.id << ","
                          << "\"peer\":\"" << sub.peer << "\","
                          << "\"sent_mb\":" << sub.sent_mb << ","
                          << "\"q\":" << sub.queue_depth << ","
                          << "\"latency_ms\":" << sub.latency_ms << ","
                          << "\"idle_ms\":" << sub.idle_ms << ","
                          << "\"ctrl\":\"" << sub.control << "\","
                          << "\"srt_sndbuf_pkts\":" << sub.srt.send_buf_packets << ","
                          << "\"srt_avail_sndbuf_bytes\":" << sub.srt.avail_send_buf_bytes << ","
                          << "\"srt_retrans_total\":" << sub.srt.retrans_total << ","
                          << "\"srt_send_loss_total\":" << sub.srt.send_loss_total << ","
                          << "\"srt_rtt_ms\":" << sub.srt.rtt_ms
                          << "}";
                    }
                }
                j << "]}";
                ws_.broadcast(j.str());
            }
        }
    }

    Config       cfg_;
    StreamStatus status_;
    HostMonitor host_monitor_;
    TsValidator  validator_;
    GlobalController global_controller_;
    PusherControlClient pusher_control_;
    WsBroadcaster ws_;

    std::atomic<bool> stopped_{true};

    SRTSOCKET pub_listener_ = SRT_INVALID_SOCK;
    SRTSOCKET sub_listener_ = SRT_INVALID_SOCK;
    int epoll_id_ = -1;
    int wake_fd_ = -1;

    std::mutex  pub_mu_;
    SRTSOCKET   pub_sock_ = SRT_INVALID_SOCK;
    sockaddr_in pub_peer_{};

    std::mutex  sub_mu_;
    std::map<uint64_t, std::shared_ptr<SubscriberSession>> subscribers_;
    std::atomic<uint64_t> next_sub_id_{1};

    std::thread t_pub_acc_, t_sub_acc_, t_pub_read_, t_sub_send_, t_monitor_;
};
