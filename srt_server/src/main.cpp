#include <srt/srt.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using SysClock = std::chrono::system_clock;
using Ms = std::chrono::milliseconds;
using namespace std::chrono_literals;

std::atomic<bool> g_running{true};

std::string now_wall_time() {
    auto now = SysClock::now();
    std::time_t t = SysClock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T");
    return oss.str();
}

void log_line(const std::string& level, const std::string& msg) {
    std::cerr << "[" << now_wall_time() << "] [" << level << "] " << msg << std::endl;
}

std::string last_srt_error() {
    const char* err = srt_getlasterror_str();
    return err ? std::string(err) : std::string("unknown SRT error");
}

std::string sockaddr_to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    std::ostringstream oss;
    oss << ip << ":" << ntohs(addr.sin_port);
    return oss.str();
}

struct Config {
    std::string bind_ip = "0.0.0.0";
    int publisher_port = 9000;
    int subscriber_port = 9001;
    int payload_size = 1316;           // 7 * 188, 常见 TS chunk
    int latency_ms = 120;
    int recv_buf_bytes = 4 * 1024 * 1024;
    int send_buf_bytes = 4 * 1024 * 1024;
    int subscriber_queue_max_chunks = 512;
    int max_subscribers = 256;
    int publisher_idle_timeout_ms = 15000; // 超过此时间无数据则断开
    bool validate_ts = true;
    bool require_h264 = false;         // true 时要求 PMT 中出现 H.264 (stream_type 0x1B)
    bool require_audio = false;        // true 时要求 PMT 中出现已知音频流
    int monitor_interval_ms = 2000;
    std::optional<std::string> passphrase; // AES-128/192/256
    int pbkeylen = 16;                        // 16/24/32 -> AES-128/192/256
};

struct StreamStatus {
    std::atomic<bool> publisher_online{false};
    std::atomic<uint64_t> total_ingress_bytes{0};
    std::atomic<uint64_t> total_egress_bytes{0};
    std::atomic<uint64_t> ingress_bytes_window{0};
    std::atomic<uint64_t> egress_bytes_window{0};
    std::atomic<uint64_t> ingress_chunks{0};
    std::atomic<uint64_t> ts_discontinuities{0};
    std::atomic<uint64_t> subscriber_queue_drops{0};
    std::atomic<int> subscriber_count{0};
    std::atomic<int64_t> publisher_connected_at_ms{0};
    std::atomic<int64_t> publisher_last_active_at_ms{0};
};

int64_t steady_ms() {
    return std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count();
}

class TsValidator {
public:
    explicit TsValidator(bool require_h264, bool require_audio)
        : require_h264_(require_h264), require_audio_(require_audio) {}

    bool inspect(const uint8_t* data, size_t len, StreamStatus& stats) {
        if (len == 0) return true;
        if (len % 188 != 0) {
            last_error_ = "payload size is not aligned to 188-byte TS packets";
            return false;
        }

        for (size_t off = 0; off < len; off += 188) {
            const uint8_t* p = data + off;
            if (p[0] != 0x47) {
                last_error_ = "TS sync byte mismatch";
                return false;
            }

            bool payload_unit_start = (p[1] & 0x40) != 0;
            uint16_t pid = static_cast<uint16_t>(((p[1] & 0x1F) << 8) | p[2]);
            uint8_t adaptation_field_control = (p[3] >> 4) & 0x03;
            uint8_t continuity_counter = p[3] & 0x0F;

            bool has_payload = (adaptation_field_control == 1 || adaptation_field_control == 3);
            bool has_adaptation = (adaptation_field_control == 2 || adaptation_field_control == 3);

            if (has_payload && pid != 0x1FFF) {
                auto it = cc_map_.find(pid);
                if (it != cc_map_.end()) {
                    uint8_t expected = static_cast<uint8_t>((it->second + 1) & 0x0F);
                    if (continuity_counter != expected && !discontinuity_flag(p, has_adaptation)) {
                        stats.ts_discontinuities.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                cc_map_[pid] = continuity_counter;
            }

            if (!has_payload) continue;

            size_t payload_offset = 4;
            if (has_adaptation) {
                uint8_t afl = p[4];
                payload_offset += 1 + afl;
                if (payload_offset > 188) continue;
            }
            if (payload_offset >= 188) continue;

            const uint8_t* payload = p + payload_offset;
            size_t payload_len = 188 - payload_offset;

            if (pid == 0x0000) {
                if (payload_unit_start) parse_pat(payload, payload_len);
            } else if (pmt_pid_ && pid == *pmt_pid_) {
                if (payload_unit_start) parse_pmt(payload, payload_len);
            }
        }

        if (require_h264_ && saw_pmt_ && !saw_h264_) {
            last_error_ = "PMT parsed but no H.264 stream_type(0x1B) found";
            return false;
        }
        if (require_audio_ && saw_pmt_ && !saw_audio_) {
            last_error_ = "PMT parsed but no known audio stream found (AAC/Opus/AC3/MPEG-Audio)";
            return false;
        }
        return true;
    }

    const std::string& last_error() const { return last_error_; }
    bool saw_h264() const { return saw_h264_; }
    bool saw_pmt() const { return saw_pmt_; }
    bool saw_audio() const { return saw_audio_; }

private:
    static bool discontinuity_flag(const uint8_t* p, bool has_adaptation) {
        if (!has_adaptation) return false;
        uint8_t afl = p[4];
        if (afl == 0 || 5 + afl > 188) return false;
        return (p[5] & 0x80) != 0;
    }

    void parse_pat(const uint8_t* payload, size_t len) {
        if (len < 1) return;
        uint8_t pointer_field = payload[0];
        if (1 + pointer_field + 8 > len) return;
        const uint8_t* sec = payload + 1 + pointer_field;
        if (sec[0] != 0x00) return;
        uint16_t section_length = static_cast<uint16_t>(((sec[1] & 0x0F) << 8) | sec[2]);
        if (3 + section_length > len - 1 - pointer_field) return;
        if (section_length < 9) return;

        size_t program_info_len = section_length - 9;
        const uint8_t* prog = sec + 8;
        for (size_t i = 0; i + 4 <= program_info_len; i += 4) {
            uint16_t program_number = static_cast<uint16_t>((prog[i] << 8) | prog[i + 1]);
            uint16_t pid = static_cast<uint16_t>(((prog[i + 2] & 0x1F) << 8) | prog[i + 3]);
            if (program_number != 0) {
                pmt_pid_ = pid;
                return;
            }
        }
    }

    void parse_pmt(const uint8_t* payload, size_t len) {
        if (len < 1) return;
        uint8_t pointer_field = payload[0];
        if (1 + pointer_field + 12 > len) return;
        const uint8_t* sec = payload + 1 + pointer_field;
        if (sec[0] != 0x02) return;
        uint16_t section_length = static_cast<uint16_t>(((sec[1] & 0x0F) << 8) | sec[2]);
        if (section_length < 13) return;
        if (3 + section_length > len - 1 - pointer_field) return;
        saw_pmt_ = true;

        uint16_t program_info_length = static_cast<uint16_t>(((sec[10] & 0x0F) << 8) | sec[11]);
        size_t pos = 12 + program_info_length;
        size_t end = 3 + section_length - 4;
        while (pos + 5 <= end) {
            uint8_t stream_type = sec[pos];
            uint16_t es_info_length = static_cast<uint16_t>(((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4]);
            if (stream_type == 0x1B) saw_h264_ = true;
            if (is_audio_stream(stream_type, sec + pos + 5, es_info_length)) {
                saw_audio_ = true;
            }
            pos += 5 + es_info_length;
        }
    }

    static bool is_audio_stream(uint8_t stream_type, const uint8_t* desc, size_t desc_len) {
        switch (stream_type) {
            case 0x0F: // AAC ADTS
            case 0x11: // AAC LATM/LOAS
            case 0x03: // MPEG1 Audio
            case 0x04: // MPEG2 Audio
            case 0x81: // AC3 (common mapping)
                return true;
            default:
                break;
        }
        // Opus usually signaled with registration descriptor "Opus" on private stream_type 0x06
        size_t i = 0;
        while (i + 2 <= desc_len) {
            uint8_t tag = desc[i];
            uint8_t len = desc[i + 1];
            if (i + 2 + len > desc_len) break;
            if (tag == 0x05 && len >= 4) {
                if (desc[i + 2] == 'O' && desc[i + 3] == 'p' && desc[i + 4] == 'u' && desc[i + 5] == 's') {
                    return true;
                }
            }
            i += 2 + len;
        }
        return false;
    }

private:
    bool require_h264_ = false;
    bool require_audio_ = false;
    std::optional<uint16_t> pmt_pid_;
    bool saw_pmt_ = false;
    bool saw_h264_ = false;
    bool saw_audio_ = false;
    std::unordered_map<uint16_t, uint8_t> cc_map_;
    std::string last_error_;
};

class SubscriberSession {
public:
    SubscriberSession(SRTSOCKET sock,
                      sockaddr_in peer_addr,
                      StreamStatus& status,
                      const Config& cfg,
                      uint64_t id)
        : sock_(sock), peer_addr_(peer_addr), status_(status), cfg_(cfg), id_(id) {}

    ~SubscriberSession() {
        stop();
    }

    void start() {
        worker_ = std::thread([this]() { this->run(); });
    }

    void stop() {
        bool first_call = false;
        if (stopped_.compare_exchange_strong(first_call, true)) {
            running_.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mu_);
                closed_ = true;
            }
            cv_.notify_all();
            srt_close(sock_);
        }
        if (worker_.joinable()) worker_.join();
    }

    bool push_chunk(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return false;

        // For a late-joining subscriber, discard data until we see a TS packet
        // that advertises a random access point (usually an IDR keyframe).
        // This prevents the player from starting with delta frames that lack SPS/PPS.
        if (!synced_to_keyframe_) {
            if (!has_random_access_point(data, len)) {
                // Still waiting for the next keyframe, keep dropping quietly.
                return true;
            }
            synced_to_keyframe_ = true;
        }

        if (queue_.size() >= static_cast<size_t>(cfg_.subscriber_queue_max_chunks)) {
            queue_.pop_front();
            status_.subscriber_queue_drops.fetch_add(1, std::memory_order_relaxed);
        }

        queue_.emplace_back(data, data + len, steady_ms());
        cv_.notify_one();
        return true;
    }

    bool running() const { return running_.load(std::memory_order_relaxed); }
    uint64_t id() const { return id_; }
    std::string peer() const { return sockaddr_to_string(peer_addr_); }
    uint64_t dropped_chunks() const { return dropped_chunks_.load(std::memory_order_relaxed); }
    uint64_t sent_bytes() const { return sent_bytes_.load(std::memory_order_relaxed); }
    int64_t last_send_active_ms() const { return last_send_active_ms_.load(std::memory_order_relaxed); }
    int configured_latency_ms() const { return configured_latency_ms_.load(std::memory_order_relaxed); }
    int queue_depth() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<int>(queue_.size());
    }

private:
    struct Chunk {
        std::vector<uint8_t> data;
        int64_t enqueued_at_ms = 0;
        Chunk(const uint8_t* d, const uint8_t* e, int64_t ts)
            : data(d, e), enqueued_at_ms(ts) {}
    };

    void run() {
        status_.subscriber_count.fetch_add(1, std::memory_order_relaxed);
        last_send_active_ms_.store(steady_ms(), std::memory_order_relaxed);
        configured_latency_ms_.store(read_latency_ms(sock_), std::memory_order_relaxed);
        log_line("INFO", "subscriber#" + std::to_string(id_) + " started: " + peer());

        while (running_.load(std::memory_order_relaxed) && g_running.load(std::memory_order_relaxed)) {
            Chunk chunk(nullptr, nullptr, 0);
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, 500ms, [&]() {
                    return !queue_.empty() || closed_ || !running_.load(std::memory_order_relaxed) || !g_running.load(std::memory_order_relaxed);
                });

                if (!running_.load(std::memory_order_relaxed) || !g_running.load(std::memory_order_relaxed) || closed_) {
                    break;
                }
                if (queue_.empty()) continue;
                chunk = std::move(queue_.front());
                queue_.pop_front();
            }

            int sent = srt_send(sock_, reinterpret_cast<const char*>(chunk.data.data()), static_cast<int>(chunk.data.size()));
            if (sent == SRT_ERROR) {
                log_line("WARN", "subscriber#" + std::to_string(id_) + " send failed: " + last_srt_error());
                break;
            }

            sent_bytes_.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
            status_.total_egress_bytes.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
            status_.egress_bytes_window.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
            last_send_active_ms_.store(steady_ms(), std::memory_order_relaxed);
        }

        status_.subscriber_count.fetch_sub(1, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        log_line("INFO", "subscriber#" + std::to_string(id_) + " stopped: " + peer());
    }

    static int read_latency_ms(SRTSOCKET sock) {
        int val = 0;
        int optlen = sizeof(val);
        if (srt_getsockflag(sock, SRTO_RCVLATENCY, &val, &optlen) == 0) return val;
        optlen = sizeof(val);
        if (srt_getsockflag(sock, SRTO_LATENCY, &val, &optlen) == 0) return val;
        return -1;
    }

    // Inspect a TS chunk and return true if any packet carries the
    // random_access_indicator flag in its adaptation field.
    static bool has_random_access_point(const uint8_t* data, size_t len) {
        if (!data || len < 188 || (len % 188) != 0) return false;
        for (size_t off = 0; off + 188 <= len; off += 188) {
            const uint8_t* p = data + off;
            if (p[0] != 0x47) continue; // not a TS sync byte, ignore

            uint8_t afc = static_cast<uint8_t>((p[3] >> 4) & 0x03);
            bool has_adaptation = (afc == 2 || afc == 3);
            if (!has_adaptation) continue;

            uint8_t afl = p[4];
            if (afl == 0 || 5 + afl > 188) continue;

            uint8_t flags = p[5];
            if (flags & 0x40) { // random_access_indicator
                return true;
            }
        }
        return false;
    }

private:
    SRTSOCKET sock_;
    sockaddr_in peer_addr_{};
    StreamStatus& status_;
    const Config& cfg_;
    uint64_t id_ = 0;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Chunk> queue_;
    bool closed_ = false;
    bool synced_to_keyframe_ = false;
    std::atomic<bool> running_{true};
    std::atomic<bool> stopped_{false};
    std::thread worker_;

    std::atomic<uint64_t> dropped_chunks_{0};
    std::atomic<uint64_t> sent_bytes_{0};
    std::atomic<int64_t> last_send_active_ms_{0};
    std::atomic<int> configured_latency_ms_{-1};
};

class RelayServer {
public:
    explicit RelayServer(Config cfg) : cfg_(std::move(cfg)), validator_(cfg_.require_h264, cfg_.require_audio) {}

    ~RelayServer() {
        stop();
    }

    bool start() {
        if (srt_startup() != 0) {
            log_line("ERROR", "srt_startup failed: " + last_srt_error());
            return false;
        }

        publisher_listener_ = create_listener(cfg_.bind_ip, cfg_.publisher_port, true);
        if (publisher_listener_ == SRT_INVALID_SOCK) return false;

        subscriber_listener_ = create_listener(cfg_.bind_ip, cfg_.subscriber_port, false);
        if (subscriber_listener_ == SRT_INVALID_SOCK) return false;

        accept_publisher_thread_ = std::thread([this]() { accept_publisher_loop(); });
        accept_subscriber_thread_ = std::thread([this]() { accept_subscriber_loop(); });
        monitor_thread_ = std::thread([this]() { monitor_loop(); });

        log_line("INFO", "relay server started, publisher port=" + std::to_string(cfg_.publisher_port) +
                         ", subscriber port=" + std::to_string(cfg_.subscriber_port));
        return true;
    }

    void stop() {
        bool expected = true;
        if (!stopped_.compare_exchange_strong(expected, false)) {
            return;
        }

        g_running.store(false, std::memory_order_relaxed);

        if (publisher_listener_ != SRT_INVALID_SOCK) {
            srt_close(publisher_listener_);
            publisher_listener_ = SRT_INVALID_SOCK;
        }
        if (subscriber_listener_ != SRT_INVALID_SOCK) {
            srt_close(subscriber_listener_);
            subscriber_listener_ = SRT_INVALID_SOCK;
        }

        close_publisher();
        close_all_subscribers();

        if (accept_publisher_thread_.joinable()) accept_publisher_thread_.join();
        if (accept_subscriber_thread_.joinable()) accept_subscriber_thread_.join();
        if (monitor_thread_.joinable()) monitor_thread_.join();

        srt_cleanup();
        log_line("INFO", "relay server stopped");
    }

private:
    static bool set_sock_flag(SRTSOCKET sock, SRT_SOCKOPT opt, const void* val, int len, const std::string& name) {
        if (srt_setsockflag(sock, opt, val, len) != 0) {
            log_line("ERROR", "set " + name + " failed: " + last_srt_error());
            return false;
        }
        return true;
    }

    SRTSOCKET create_listener(const std::string& ip, int port, bool sender_side) {
        SRTSOCKET sock = srt_socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == SRT_INVALID_SOCK) {
            log_line("ERROR", "srt_socket failed: " + last_srt_error());
            return SRT_INVALID_SOCK;
        }

        int yes = 1;
        int latency = cfg_.latency_ms;
        int recv_buf = cfg_.recv_buf_bytes;
        int send_buf = cfg_.send_buf_bytes;
        int transtype = SRTT_LIVE;
        int rcv_timeout = sender_side ? cfg_.publisher_idle_timeout_ms : 5000;

        if (!set_sock_flag(sock, SRTO_REUSEADDR, &yes, sizeof(yes), "SRTO_REUSEADDR")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_RCVSYN, &yes, sizeof(yes), "SRTO_RCVSYN")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_SNDSYN, &yes, sizeof(yes), "SRTO_SNDSYN")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_TSBPDMODE, &yes, sizeof(yes), "SRTO_TSBPDMODE")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_LATENCY, &latency, sizeof(latency), "SRTO_LATENCY")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_RCVBUF, &recv_buf, sizeof(recv_buf), "SRTO_RCVBUF")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_SNDBUF, &send_buf, sizeof(send_buf), "SRTO_SNDBUF")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_TRANSTYPE, &transtype, sizeof(transtype), "SRTO_TRANSTYPE")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }
        if (!set_sock_flag(sock, SRTO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout), "SRTO_RCVTIMEO")) {
            srt_close(sock); return SRT_INVALID_SOCK;
        }

        if (cfg_.passphrase && !cfg_.passphrase->empty()) {
            int pbk = cfg_.pbkeylen;
            if (!set_sock_flag(sock, SRTO_PASSPHRASE, cfg_.passphrase->c_str(), static_cast<int>(cfg_.passphrase->size()), "SRTO_PASSPHRASE")) {
                srt_close(sock); return SRT_INVALID_SOCK;
            }
            if (!set_sock_flag(sock, SRTO_PBKEYLEN, &pbk, sizeof(pbk), "SRTO_PBKEYLEN")) {
                srt_close(sock); return SRT_INVALID_SOCK;
            }
        }
        // Some libsrt builds (e.g. on Raspberry Pi distros) reject SRTO_LINGER
        // on listener sockets with "Bad parameters". Treat it as optional.
        int linger_ms = 0;
        if (srt_setsockflag(sock, SRTO_LINGER, &linger_ms, sizeof(linger_ms)) != 0) {
            log_line("WARN", "set SRTO_LINGER skipped: " + last_srt_error());
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            log_line("ERROR", "invalid bind ip: " + ip);
            srt_close(sock);
            return SRT_INVALID_SOCK;
        }

        if (srt_bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SRT_ERROR) {
            log_line("ERROR", "srt_bind failed: " + last_srt_error());
            srt_close(sock);
            return SRT_INVALID_SOCK;
        }
        if (srt_listen(sock, 64) == SRT_ERROR) {
            log_line("ERROR", "srt_listen failed: " + last_srt_error());
            srt_close(sock);
            return SRT_INVALID_SOCK;
        }
        return sock;
    }

    void accept_publisher_loop() {
        while (g_running.load(std::memory_order_relaxed)) {
            sockaddr_in peer{};
            int peer_len = sizeof(peer);
            SRTSOCKET sock = srt_accept(publisher_listener_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (sock == SRT_INVALID_SOCK) {
                if (g_running.load(std::memory_order_relaxed)) {
                    log_line("WARN", "publisher accept failed: " + last_srt_error());
                }
                break;
            }

            std::string peer_str = sockaddr_to_string(peer);
            {
                std::lock_guard<std::mutex> lk(publisher_mu_);
                if (publisher_sock_ != SRT_INVALID_SOCK) {
                    log_line("WARN", "reject extra publisher from " + peer_str + ", only one publisher is allowed");
                    srt_close(sock);
                    continue;
                }
                publisher_sock_ = sock;
                publisher_peer_ = peer;
                status_.publisher_online.store(true, std::memory_order_relaxed);
                status_.publisher_connected_at_ms.store(steady_ms(), std::memory_order_relaxed);
                status_.publisher_last_active_at_ms.store(steady_ms(), std::memory_order_relaxed);
            }

            log_line("INFO", "publisher connected: " + peer_str);
            publisher_read_thread_ = std::thread([this]() { this->publisher_read_loop(); });
            if (publisher_read_thread_.joinable()) publisher_read_thread_.join();
            close_publisher();
        }
    }

    void publisher_read_loop() {
        std::vector<char> buf(static_cast<size_t>(cfg_.payload_size));

        while (g_running.load(std::memory_order_relaxed)) {
            SRTSOCKET sock;
            {
                std::lock_guard<std::mutex> lk(publisher_mu_);
                sock = publisher_sock_;
            }
            if (sock == SRT_INVALID_SOCK) break;

            int n = srt_recv(sock, buf.data(), static_cast<int>(buf.size()));
            if (n == SRT_ERROR) {
                int serr = srt_getlasterror(nullptr);
                if (serr == SRT_ETIMEOUT) {
                    if (steady_ms() - status_.publisher_last_active_at_ms.load(std::memory_order_relaxed) > cfg_.publisher_idle_timeout_ms) {
                        log_line("WARN", "publisher idle timeout, closing");
                        break;
                    }
                    continue;
                }
                log_line("WARN", "publisher recv failed: " + last_srt_error());
                break;
            }
            if (n == 0) continue;

            status_.publisher_last_active_at_ms.store(steady_ms(), std::memory_order_relaxed);
            status_.total_ingress_bytes.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            status_.ingress_bytes_window.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            status_.ingress_chunks.fetch_add(1, std::memory_order_relaxed);

            if (cfg_.validate_ts) {
                bool ok = validator_.inspect(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n), status_);
                if (!ok) {
                    log_line("WARN", std::string("TS validation failed: ") + validator_.last_error());
                    continue;
                }
            }

            broadcast(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n));
        }
    }

    void accept_subscriber_loop() {
        while (g_running.load(std::memory_order_relaxed)) {
            sockaddr_in peer{};
            int peer_len = sizeof(peer);
            SRTSOCKET sock = srt_accept(subscriber_listener_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (sock == SRT_INVALID_SOCK) {
                if (g_running.load(std::memory_order_relaxed)) {
                    log_line("WARN", "subscriber accept failed: " + last_srt_error());
                }
                break;
            }

            {
                std::lock_guard<std::mutex> lk(subscribers_mu_);
                if (subscribers_.size() >= static_cast<size_t>(cfg_.max_subscribers)) {
                    log_line("WARN", "reject subscriber " + sockaddr_to_string(peer) + " reason=max_subscribers");
                    srt_close(sock);
                    continue;
                }
            }

            uint64_t id = next_subscriber_id_.fetch_add(1, std::memory_order_relaxed);
            auto session = std::make_shared<SubscriberSession>(sock, peer, status_, cfg_, id);
            {
                std::lock_guard<std::mutex> lk(subscribers_mu_);
                subscribers_[id] = session;
            }
            session->start();
        }
    }

    void broadcast(const uint8_t* data, size_t len) {
        std::vector<uint64_t> dead;
        {
            std::lock_guard<std::mutex> lk(subscribers_mu_);
            for (auto& [id, sess] : subscribers_) {
                if (!sess->running()) {
                    dead.push_back(id);
                    continue;
                }
                if (!sess->push_chunk(data, len)) {
                    dead.push_back(id);
                }
            }
            for (uint64_t id : dead) {
                auto it = subscribers_.find(id);
                if (it != subscribers_.end()) {
                    it->second->stop();
                    subscribers_.erase(it);
                }
            }
        }
    }

    void close_publisher() {
        std::lock_guard<std::mutex> lk(publisher_mu_);
        if (publisher_sock_ != SRT_INVALID_SOCK) {
            srt_close(publisher_sock_);
            publisher_sock_ = SRT_INVALID_SOCK;
        }
        status_.publisher_online.store(false, std::memory_order_relaxed);
    }

    void close_all_subscribers() {
        std::lock_guard<std::mutex> lk(subscribers_mu_);
        for (auto& [_, sess] : subscribers_) {
            sess->stop();
        }
        subscribers_.clear();
    }

    void monitor_loop() {
        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(Ms(cfg_.monitor_interval_ms));
            if (!g_running.load(std::memory_order_relaxed)) break;

            uint64_t ingress = status_.ingress_bytes_window.exchange(0, std::memory_order_relaxed);
            uint64_t egress = status_.egress_bytes_window.exchange(0, std::memory_order_relaxed);
            double seconds = static_cast<double>(cfg_.monitor_interval_ms) / 1000.0;
            double ingress_mbps = (ingress * 8.0) / seconds / 1000.0 / 1000.0;
            double egress_mbps = (egress * 8.0) / seconds / 1000.0 / 1000.0;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2)
                << "publisher_online=" << (status_.publisher_online.load(std::memory_order_relaxed) ? 1 : 0)
                << " ingress_mbps=" << ingress_mbps
                << " egress_mbps=" << egress_mbps
                << " subscribers=" << status_.subscriber_count.load(std::memory_order_relaxed)
                << " ts_discontinuities=" << status_.ts_discontinuities.load(std::memory_order_relaxed)
                << " queue_drops=" << status_.subscriber_queue_drops.load(std::memory_order_relaxed)
                << " publisher_last_active_ms_ago=" << ms_ago(status_.publisher_last_active_at_ms.load(std::memory_order_relaxed));

            {
                std::lock_guard<std::mutex> lk(subscribers_mu_);
                int idx = 0;
                for (const auto& [id, sess] : subscribers_) {
                    if (idx >= 8) { // 日志里最多展开前 8 个，避免刷屏
                        oss << " subscribers_more=" << (subscribers_.size() - 8);
                        break;
                    }
                    oss << " | sub#" << id
                        << " peer=" << sess->peer()
                        << " q=" << sess->queue_depth()
                        << " latency_ms~=" << sess->configured_latency_ms()
                        << " last_send_ms_ago=" << ms_ago(sess->last_send_active_ms());
                    ++idx;
                }
            }

            log_line("STAT", oss.str());
        }
    }

    static int64_t ms_ago(int64_t t) {
        if (t <= 0) return -1;
        return steady_ms() - t;
    }

private:
    Config cfg_;
    StreamStatus status_;
    TsValidator validator_;

    std::atomic<bool> stopped_{true};

    SRTSOCKET publisher_listener_ = SRT_INVALID_SOCK;
    SRTSOCKET subscriber_listener_ = SRT_INVALID_SOCK;

    std::mutex publisher_mu_;
    SRTSOCKET publisher_sock_ = SRT_INVALID_SOCK;
    sockaddr_in publisher_peer_{};

    std::mutex subscribers_mu_;
    std::map<uint64_t, std::shared_ptr<SubscriberSession>> subscribers_;
    std::atomic<uint64_t> next_subscriber_id_{1};

    std::thread accept_publisher_thread_;
    std::thread accept_subscriber_thread_;
    std::thread publisher_read_thread_;
    std::thread monitor_thread_;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };

        if (a == "--bind") cfg.bind_ip = need_value(a);
        else if (a == "--pub-port") cfg.publisher_port = std::stoi(need_value(a));
        else if (a == "--sub-port") cfg.subscriber_port = std::stoi(need_value(a));
        else if (a == "--payload-size") cfg.payload_size = std::stoi(need_value(a));
        else if (a == "--latency-ms") cfg.latency_ms = std::stoi(need_value(a));
        else if (a == "--recv-buf") cfg.recv_buf_bytes = std::stoi(need_value(a));
        else if (a == "--send-buf") cfg.send_buf_bytes = std::stoi(need_value(a));
        else if (a == "--sub-queue") cfg.subscriber_queue_max_chunks = std::stoi(need_value(a));
        else if (a == "--max-subs") cfg.max_subscribers = std::stoi(need_value(a));
        else if (a == "--pub-idle-ms") cfg.publisher_idle_timeout_ms = std::stoi(need_value(a));
        else if (a == "--no-validate-ts") cfg.validate_ts = false;
        else if (a == "--require-h264") cfg.require_h264 = true;
        else if (a == "--require-audio") cfg.require_audio = true;
        else if (a == "--passphrase") cfg.passphrase = need_value(a);
        else if (a == "--pbkeylen") cfg.pbkeylen = std::stoi(need_value(a));
        else if (a == "--monitor-ms") cfg.monitor_interval_ms = std::stoi(need_value(a));
        else if (a == "--help") {
            std::cout
                << "Usage: srt_relay_server [options]\n"
                << "  --bind <ip>              default: 0.0.0.0\n"
                << "  --pub-port <port>        default: 9000\n"
                << "  --sub-port <port>        default: 9001\n"
                << "  --payload-size <bytes>   default: 1316\n"
                << "  --latency-ms <ms>        default: 120\n"
                << "  --recv-buf <bytes>       default: 4194304\n"
                << "  --send-buf <bytes>       default: 4194304\n"
                << "  --sub-queue <chunks>     default: 512\n"
                << "  --max-subs <n>           default: 256\n"
                << "  --pub-idle-ms <ms>       default: 15000\n"
                << "  --monitor-ms <ms>        default: 2000\n"
                << "  --no-validate-ts         disable TS validation\n"
                << "  --require-h264           require H.264 in PMT\n"
                << "  --require-audio          require audio stream (AAC/Opus/AC3/MPEG-Audio)\n"
                << "  --passphrase <str>       enable AES for SRT sessions\n"
                << "  --pbkeylen <16|24|32>    AES key length, default 16\n";
            std::exit(0);
        }
        else {
            throw std::runtime_error("unknown arg: " + a);
        }
    }
    if (!(cfg.pbkeylen == 16 || cfg.pbkeylen == 24 || cfg.pbkeylen == 32)) {
        throw std::runtime_error("pbkeylen must be 16, 24, or 32");
    }
    return cfg;
}

void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        Config cfg = parse_args(argc, argv);
        RelayServer server(cfg);
        if (!server.start()) {
            return 1;
        }

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(200ms);
        }

        server.stop();
        return 0;
    } catch (const std::exception& ex) {
        log_line("ERROR", ex.what());
        return 1;
    }
}
