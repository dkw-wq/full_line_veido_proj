struct StreamChunk {
    std::vector<uint8_t> data;
    StreamChunk(const uint8_t* bytes, size_t len) : data(bytes, bytes + len) {}
    explicit StreamChunk(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}
};

class SubscriberSession {
public:
    enum class SendResult { Empty, Sent, Blocked, Failed };

    SubscriberSession(SRTSOCKET sock, sockaddr_in peer,
                      StreamStatus& status, const Config& cfg, uint64_t id)
        : sock_(sock), peer_(peer), status_(status), cfg_(cfg), id_(id) {
        status_.subscriber_count.fetch_add(1);
        last_active_ms_.store(steady_ms());
        latency_ms_.store(read_latency(sock_));
        log_line("INFO", "subscriber connected: " + sockaddr_to_string(peer_));
        dump_srt_latency("[SUB]", sock_);
        log_line("INFO", "sub#" + std::to_string(id_) + " started: " + this->peer());
    }
    ~SubscriberSession() { stop(); }

    void stop() {
        if (stopped_.exchange(true)) return;
        running_.store(false);
        { std::lock_guard<std::mutex> lk(mu_); queue_.clear(); }
        srt_close(sock_);
        status_.subscriber_count.fetch_sub(1);
        log_line("INFO", "sub#" + std::to_string(id_) + " stopped");
    }

    void request_close() { closing_.store(true); }
    bool closing() const { return closing_.load(); }
    SRTSOCKET socket() const { return sock_; }

    bool push_chunk(const std::shared_ptr<const StreamChunk>& chunk, bool rap) {
        std::lock_guard<std::mutex> lk(mu_);
        if (closing_.load() || !running_.load()) return false;
        if (!synced_) {
            if (!rap) return true;
            synced_ = true;
        }
        SubscriberAction action = control_action();
        if (action == SubscriberAction::Placeholder) {
            if (!queue_.empty()) {
                status_.subscriber_queue_drops.fetch_add((uint64_t)queue_.size(), std::memory_order_relaxed);
                queue_.clear();
            }
            queue_.push_back(std::make_shared<StreamChunk>(make_null_ts_chunk(chunk->data.size())));
            status_.subscriber_queue_drops.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (action == SubscriberAction::Drop && !queue_.empty()) {
            status_.subscriber_queue_drops.fetch_add((uint64_t)queue_.size(), std::memory_order_relaxed);
            queue_.clear();
        }
        if (queue_.size() >= (size_t)cfg_.subscriber_queue_max_chunks) {
            queue_.pop_front();
            status_.subscriber_queue_drops.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(chunk);
        return true;
    }

    SendResult send_one() {
        std::shared_ptr<const StreamChunk> chunk;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.empty() || closing_.load()) return SendResult::Empty;
            chunk = queue_.front();
        }
        const int n = srt_send(sock_, (const char*)chunk->data.data(), (int)chunk->data.size());
        if (n == SRT_ERROR) {
            if (srt_getlasterror(nullptr) == SRT_EASYNCSND) return SendResult::Blocked;
            log_line("WARN", "sub#" + std::to_string(id_) + " send: " + last_srt_error());
            request_close();
            return SendResult::Failed;
        }
        if ((size_t)n != chunk->data.size()) {
            log_line("WARN", "sub#" + std::to_string(id_) + " short send");
            request_close();
            return SendResult::Failed;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!queue_.empty() && queue_.front() == chunk) queue_.pop_front();
        }
        sent_bytes_.fetch_add((uint64_t)n);
        status_.total_egress_bytes.fetch_add((uint64_t)n);
        status_.egress_bytes_window.fetch_add((uint64_t)n);
        last_active_ms_.store(steady_ms());
        return SendResult::Sent;
    }

    bool running() const { return running_.load() && !closing_.load(); }
    uint64_t id() const { return id_; }
    std::string peer() const { return sockaddr_to_string(peer_); }
    uint64_t sent_bytes() const { return sent_bytes_.load(); }
    int64_t last_active_ms() const { return last_active_ms_.load(); }
    int queue_depth() const { std::lock_guard<std::mutex> lk(mu_); return (int)queue_.size(); }
    int latency_ms() const { return latency_ms_.load(); }
    SubscriberAction control_action() const {
        return (SubscriberAction)control_action_.load(std::memory_order_relaxed);
    }
    const char* control_action_name() const { return subscriber_action_name(control_action()); }
    bool bad_for_global() const { return control_action() != SubscriberAction::Normal; }

    SubscriberControlResult evaluate_control(int64_t now_ms) {
        SubscriberSample sample;
        sample.queue_depth = queue_depth();
        sample.queue_max = cfg_.subscriber_queue_max_chunks;
        int64_t last_active = last_active_ms_.load();
        sample.idle_ms = last_active > 0 ? now_ms - last_active : -1;
        SubscriberControlResult result = controller_.update(sample, now_ms);
        if (result.changed) set_control_action(result.action);
        return result;
    }

    static bool has_rap(const uint8_t* d, size_t l) {
        if (!d || l < 188 || l % 188) return false;
        for (size_t o = 0; o + 188 <= l; o += 188) {
            const uint8_t* p = d + o;
            if (p[0] != 0x47) continue;
            uint8_t afc = (p[3] >> 4) & 0x03;
            if (afc != 2 && afc != 3) continue;
            if (p[4] == 0 || 5 + p[4] > 188) continue;
            if (p[5] & 0x40) return true;
        }
        return false;
    }

private:
    std::vector<uint8_t> make_null_ts_chunk(size_t desired_len) {
        size_t len = desired_len >= 188 ? desired_len - (desired_len % 188) : 188;
        if (len == 0) len = 188;
        std::vector<uint8_t> out(len, 0xFF);
        for (size_t off = 0; off + 188 <= len; off += 188) {
            out[off + 0] = 0x47;
            out[off + 1] = 0x1F;
            out[off + 2] = 0xFF;
            out[off + 3] = (uint8_t)(0x10 | (placeholder_cc_ & 0x0F));
            placeholder_cc_ = (uint8_t)((placeholder_cc_ + 1) & 0x0F);
        }
        return out;
    }
    static int read_latency(SRTSOCKET s) {
        int v = 0, l = sizeof(v);
        if (srt_getsockflag(s, SRTO_RCVLATENCY, &v, &l) == 0) return v;
        l = sizeof(v);
        if (srt_getsockflag(s, SRTO_LATENCY, &v, &l) == 0) return v;
        return -1;
    }
    void set_control_action(SubscriberAction action) {
        SubscriberAction old = (SubscriberAction)control_action_.exchange((int)action, std::memory_order_relaxed);
        if (old == action) return;
        if (action == SubscriberAction::Drop || action == SubscriberAction::Placeholder) {
            std::lock_guard<std::mutex> lk(mu_);
            if (!queue_.empty()) {
                status_.subscriber_queue_drops.fetch_add((uint64_t)queue_.size(), std::memory_order_relaxed);
                queue_.clear();
            }
        }
    }

    SRTSOCKET sock_;
    sockaddr_in peer_;
    StreamStatus& status_;
    const Config& cfg_;
    uint64_t id_;
    mutable std::mutex mu_;
    std::deque<std::shared_ptr<const StreamChunk>> queue_;
    bool synced_ = false;
    uint8_t placeholder_cc_ = 0;
    std::atomic<bool> running_{true}, stopped_{false}, closing_{false};
    std::atomic<uint64_t> sent_bytes_{0};
    std::atomic<int64_t> last_active_ms_{0};
    std::atomic<int> latency_ms_{-1};
    std::atomic<int> control_action_{(int)SubscriberAction::Normal};
    SubscriberController controller_;
};
