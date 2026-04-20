// srt_relay_server_observer_ready.cpp
// Adapted version for the sidecar observer approach:
// 1) The publisher still sends telemetry private TS sections within the stream
// 2) The relay directly observes the original TS chunks in publisher_read_loop() and records seq -> t_push_us
// 3) An independent observer acts as an additional subscriber, receives the same SRT stream, parses telemetry packets, and sends UDP ACKs back to the relay
// 4) After receiving an ACK, the relay calculates push->relay / relay->ack / push->ack, and displays them in monitor_loop()
//
// Notes:
// - The main player process does not need to be modified and can keep the original smooth-running version.
// - The observer will connect to the relay as an additional subscriber, so one more subscriber entry will appear on the dashboard.
// - The global dashboard metrics use the most recent ACK result and do not depend on subscriber matching always succeeding.

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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cerrno>
#include <limits>

#include <fcntl.h>

#include "telemetry.h"


namespace ws_util {


static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static std::string sha1_b64(const std::string& input) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = msg.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back((bit_len >> (i * 8)) & 0xFF);
    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; ++j)
            w[j] = ((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                   ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for (int j = 16; j < 80; ++j) w[j] = rotl32(w[j-3]^w[j-8]^w[j-14]^w[j-16],1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int j = 0; j < 80; ++j) {
            uint32_t f,k;
            if      (j<20){f=(b&c)|(~b&d);k=0x5A827999;}
            else if (j<40){f=b^c^d;       k=0x6ED9EBA1;}
            else if (j<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else          {f=b^c^d;       k=0xCA62C1D6;}
            uint32_t t=rotl32(a,5)+f+e+k+w[j];
            e=d;d=c;c=rotl32(b,30);b=a;a=t;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    uint8_t digest[20];
    for (int i = 0; i < 5; ++i) {
        digest[i*4]   = (h[i]>>24)&0xFF;
        digest[i*4+1] = (h[i]>>16)&0xFF;
        digest[i*4+2] = (h[i]>>8)&0xFF;
        digest[i*4+3] = h[i]&0xFF;
    }
    static const char* b64t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < 20; i += 3) {
        int rem = std::min(3, 20 - i);
        uint32_t buf = ((uint32_t)digest[i]<<16)
                     | (rem>1?(uint32_t)digest[i+1]<<8:0)
                     | (rem>2?(uint32_t)digest[i+2]:0);
        out += b64t[(buf>>18)&63];
        out += b64t[(buf>>12)&63];
        out += (rem>1) ? b64t[(buf>>6)&63] : '=';
        out += (rem>2) ? b64t[buf&63]      : '=';
    }
    return out;
}

static std::string http_header(const std::string& req, const std::string& key) {
    std::string lo_req = req;
    std::string lo_key = key;
    std::transform(lo_req.begin(), lo_req.end(), lo_req.begin(), ::tolower);
    std::transform(lo_key.begin(), lo_key.end(), lo_key.begin(), ::tolower);
    auto pos = lo_req.find(lo_key + ":");
    if (pos == std::string::npos) return {};
    auto vs = req.find(':', pos) + 1;
    auto ve = req.find("\r\n", vs);
    std::string v = req.substr(vs, ve - vs);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    return v;
}

static std::vector<uint8_t> ws_frame(const std::string& text) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);
    size_t len = text.size();
    if (len <= 125) {
        frame.push_back((uint8_t)len);
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) frame.push_back((len >> (i*8)) & 0xFF);
    }
    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

static std::string ws_recv_frame(int fd) {
    uint8_t hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return "";
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return "";
        len = ((uint64_t)ext[0]<<8)|ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return "";
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len<<8)|ext[i];
    }
    uint8_t mask[4] = {};
    if (masked && recv(fd, mask, 4, MSG_WAITALL) != 4) return "";
    if (len == 0) return "";
    std::vector<uint8_t> payload(len);
    if ((uint64_t)recv(fd, payload.data(), (int)len, MSG_WAITALL) != len) return "";
    if (masked) for (size_t i = 0; i < len; ++i) payload[i] ^= mask[i & 3];
    return std::string(payload.begin(), payload.end());
}

} // namespace ws_util

namespace {

using Clock    = std::chrono::steady_clock;
using SysClock = std::chrono::system_clock;
using Ms       = std::chrono::milliseconds;
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
    std::cerr << "[" << now_wall_time() << "] [" << level << "] " << msg << "\n";
}

std::string last_srt_error() {
    const char* e = srt_getlasterror_str();
    return e ? std::string(e) : "unknown SRT error";
}

std::string sockaddr_to_string(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

int64_t steady_ms() {
    return std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count();
}

static void dump_srt_latency(const char* tag, SRTSOCKET s) {
    int v = 0;
    int l = sizeof(v);

    if (srt_getsockflag(s, SRTO_RCVLATENCY, &v, &l) == 0)
        log_line("INFO", std::string(tag) + " RCVLATENCY=" + std::to_string(v));

    l = sizeof(v);
    if (srt_getsockflag(s, SRTO_PEERLATENCY, &v, &l) == 0)
        log_line("INFO", std::string(tag) + " PEERLATENCY=" + std::to_string(v));

    l = sizeof(v);
    if (srt_getsockflag(s, SRTO_LATENCY, &v, &l) == 0)
        log_line("INFO", std::string(tag) + " LATENCY=" + std::to_string(v));
}

struct Config {
    std::string bind_ip               = "0.0.0.0";
    int publisher_port                = 9000;
    int subscriber_port               = 9001;
    int ws_port                       = 8765;
    std::string pusher_control_host;
    int pusher_control_port           = 10090;
    bool auto_control                 = true;
    int base_bitrate_kbps             = 2000;
    int min_bitrate_kbps              = 350;
    int base_fps                      = 25;
    int min_fps                       = 10;
    int base_width                    = 1280;
    int base_height                   = 720;
    int min_width                     = 426;
    int min_height                    = 240;
    int e2e_target_ms                 = 800;
    int payload_size                  = 4096;
    int latency_ms                    = 20;
    int recv_buf_bytes                = 4 * 1024 * 1024;
    int send_buf_bytes                = 4 * 1024 * 1024;
    int subscriber_queue_max_chunks   = 512;
    int max_subscribers               = 256;
    int publisher_idle_timeout_ms     = 15000;
    bool validate_ts                  = true;
    bool require_h264                 = false;
    bool require_audio                = false;
    int monitor_interval_ms           = 2000;
    std::optional<std::string> passphrase;
    int pbkeylen                      = 16;
};

struct StreamStatus {
    std::atomic<bool>     publisher_online{false};
    std::atomic<uint64_t> total_ingress_bytes{0};
    std::atomic<uint64_t> total_egress_bytes{0};
    std::atomic<uint64_t> ingress_bytes_window{0};
    std::atomic<uint64_t> egress_bytes_window{0};
    std::atomic<uint64_t> ingress_chunks{0};
    std::atomic<uint64_t> ts_discontinuities{0};
    std::atomic<uint64_t> subscriber_queue_drops{0};
    std::atomic<int>      subscriber_count{0};
    std::atomic<int64_t>  publisher_connected_at_ms{0};
    std::atomic<int64_t>  publisher_last_active_at_ms{0};
};

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

class TsValidator {
public:
    explicit TsValidator(bool require_h264, bool require_audio)
        : require_h264_(require_h264), require_audio_(require_audio) {}

    bool inspect(const uint8_t* data, size_t len, StreamStatus& stats) {
        if (len == 0) return true;
        if (len % 188 != 0) { last_error_ = "not aligned to 188"; return false; }
        for (size_t off = 0; off < len; off += 188) {
            const uint8_t* p = data + off;
            if (p[0] != 0x47) { last_error_ = "bad sync byte"; return false; }
            bool pus = (p[1] & 0x40) != 0;
            uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
            uint8_t afc = (p[3] >> 4) & 0x03;
            uint8_t cc  = p[3] & 0x0F;
            bool hp = (afc == 1 || afc == 3);
            bool ha = (afc == 2 || afc == 3);
            if (hp && pid != 0x1FFF) {
                auto it = cc_map_.find(pid);
                if (it != cc_map_.end()) {
                    uint8_t exp = (uint8_t)((it->second + 1) & 0x0F);
                    if (cc != exp && !disc_flag(p, ha))
                        stats.ts_discontinuities.fetch_add(1, std::memory_order_relaxed);
                }
                cc_map_[pid] = cc;
            }
            if (!hp) continue;
            size_t po = 4;
            if (ha) { uint8_t afl = p[4]; po += 1 + afl; if (po > 188) continue; }
            if (po >= 188) continue;
            const uint8_t* pl = p + po;
            size_t pl_len = 188 - po;
            if (pid == 0x0000 && pus) parse_pat(pl, pl_len);
            else if (pmt_pid_ && pid == *pmt_pid_ && pus) parse_pmt(pl, pl_len);
        }
        if (require_h264_ && saw_pmt_ && !saw_h264_) { last_error_ = "no H264 in PMT"; return false; }
        if (require_audio_ && saw_pmt_ && !saw_audio_){ last_error_ = "no audio in PMT"; return false; }
        return true;
    }

    const std::string& last_error() const { return last_error_; }

private:
    static bool disc_flag(const uint8_t* p, bool ha) {
        if (!ha) return false;
        uint8_t afl = p[4];
        if (afl == 0 || 5 + afl > 188) return false;
        return (p[5] & 0x80) != 0;
    }
    void parse_pat(const uint8_t* pl, size_t len) {
        if (len < 1) return;
        uint8_t pf = pl[0];
        if (1 + pf + 8 > len) return;
        const uint8_t* s = pl + 1 + pf;
        if (s[0] != 0x00) return;
        uint16_t sl = (uint16_t)(((s[1] & 0x0F) << 8) | s[2]);
        if (3 + sl > len - 1 - pf || sl < 9) return;
        size_t pil = sl - 9;
        const uint8_t* prog = s + 8;
        for (size_t i = 0; i + 4 <= pil; i += 4) {
            uint16_t pn = (uint16_t)((prog[i]<<8)|prog[i+1]);
            uint16_t pp = (uint16_t)(((prog[i+2]&0x1F)<<8)|prog[i+3]);
            if (pn != 0) { pmt_pid_ = pp; return; }
        }
    }
    void parse_pmt(const uint8_t* pl, size_t len) {
        if (len < 1) return;
        uint8_t pf = pl[0];
        if (1 + pf + 12 > len) return;
        const uint8_t* s = pl + 1 + pf;
        if (s[0] != 0x02) return;
        uint16_t sl = (uint16_t)(((s[1] & 0x0F) << 8) | s[2]);
        if (sl < 13 || 3 + sl > len - 1 - pf) return;
        saw_pmt_ = true;
        uint16_t pil = (uint16_t)(((s[10]&0x0F)<<8)|s[11]);
        size_t pos = 12 + pil, end = 3 + sl - 4;
        while (pos + 5 <= end) {
            uint8_t st = s[pos];
            uint16_t eil = (uint16_t)(((s[pos+3]&0x0F)<<8)|s[pos+4]);
            if (st == 0x1B) saw_h264_ = true;
            if (is_audio(st)) saw_audio_ = true;
            pos += 5 + eil;
        }
    }
    static bool is_audio(uint8_t st) {
        return st==0x0F||st==0x11||st==0x03||st==0x04||st==0x81;
    }

    bool require_h264_, require_audio_;
    std::optional<uint16_t> pmt_pid_;
    bool saw_pmt_ = false, saw_h264_ = false, saw_audio_ = false;
    std::unordered_map<uint16_t, uint8_t> cc_map_;
    std::string last_error_;
};

enum class SubscriberAction {
    Normal,
    Warn,
    Drop,
    Placeholder
};

static const char* subscriber_action_name(SubscriberAction a) {
    switch (a) {
        case SubscriberAction::Normal:      return "NORMAL";
        case SubscriberAction::Warn:        return "WARN";
        case SubscriberAction::Drop:        return "DROP";
        case SubscriberAction::Placeholder: return "PLACEHOLDER";
    }
    return "NORMAL";
}

struct SubscriberSample {
    int queue_depth = 0;
    int queue_max = 0;
    int64_t idle_ms = -1;
    int64_t telemetry_last_ack_age_ms = -1;
    double telemetry_relay_to_ack_ms = -1.0;
};

struct SubscriberControlResult {
    SubscriberAction action = SubscriberAction::Normal;
    bool changed = false;
    bool disconnect = false;
};

class SubscriberController {
public:
    SubscriberControlResult update(const SubscriberSample& s, int64_t now_ms) {
        const int high_q = std::max(8, s.queue_max * 2 / 3);
        const bool queue_rising =
            prev_queue_depth_ >= 0 &&
            s.queue_depth > prev_queue_depth_ &&
            s.queue_depth >= 2;
        rising_queue_cycles_ = queue_rising ? rising_queue_cycles_ + 1 : 0;
        prev_queue_depth_ = s.queue_depth;

        const bool mild_now =
            rising_queue_cycles_ >= 2 ||
            s.idle_ms > 1500 ||
            s.telemetry_last_ack_age_ms > 1500;

        const bool congested_now =
            s.queue_depth >= high_q ||
            s.idle_ms > 3000 ||
            s.telemetry_last_ack_age_ms > 3000 ||
            s.telemetry_relay_to_ack_ms > 3000.0;

        const bool severe_now =
            s.idle_ms > 5000 &&
            s.telemetry_last_ack_age_ms > 5000;

        mild_cycles_ = mild_now ? mild_cycles_ + 1 : 0;
        congested_cycles_ = congested_now ? congested_cycles_ + 1 : 0;

        if (!mild_now && !congested_now && !severe_now) {
            normal_cycles_ += 1;
        } else {
            normal_cycles_ = 0;
        }

        SubscriberControlResult out;
        out.action = action_;

        if (action_ == SubscriberAction::Placeholder &&
            placeholder_started_ms_ > 0 &&
            severe_now &&
            now_ms - placeholder_started_ms_ >= placeholder_disconnect_after_ms_) {
            out.disconnect = true;
            return out;
        }

        if (normal_cycles_ >= 3 && action_ != SubscriberAction::Normal) {
            set_action(SubscriberAction::Normal, now_ms, out);
            return out;
        }

        if (severe_now) {
            set_action(SubscriberAction::Placeholder, now_ms, out);
            return out;
        }

        if (congested_cycles_ >= 2) {
            if (action_ != SubscriberAction::Drop &&
                now_ms - last_action_ms_ >= drop_cooldown_ms_) {
                set_action(SubscriberAction::Drop, now_ms, out);
            }
            return out;
        }

        if (mild_cycles_ >= 2) {
            if (action_ == SubscriberAction::Normal ||
                now_ms - last_action_ms_ >= warn_cooldown_ms_) {
                set_action(SubscriberAction::Warn, now_ms, out);
            }
        }

        return out;
    }

private:
    void set_action(SubscriberAction next, int64_t now_ms, SubscriberControlResult& out) {
        if (action_ == next) {
            out.action = action_;
            return;
        }
        action_ = next;
        last_action_ms_ = now_ms;
        if (next == SubscriberAction::Placeholder) {
            placeholder_started_ms_ = now_ms;
        } else if (next == SubscriberAction::Normal) {
            placeholder_started_ms_ = -1;
            mild_cycles_ = 0;
            congested_cycles_ = 0;
            rising_queue_cycles_ = 0;
        }
        out.action = action_;
        out.changed = true;
    }

    SubscriberAction action_ = SubscriberAction::Normal;
    int prev_queue_depth_ = -1;
    int rising_queue_cycles_ = 0;
    int mild_cycles_ = 0;
    int congested_cycles_ = 0;
    int normal_cycles_ = 0;
    int64_t last_action_ms_ = -100000;
    int64_t placeholder_started_ms_ = -1;
    static constexpr int64_t warn_cooldown_ms_ = 5000;
    static constexpr int64_t drop_cooldown_ms_ = 10000;
    static constexpr int64_t placeholder_disconnect_after_ms_ = 8000;
};

enum class GlobalControlAction {
    Noop,
    Idr,
    DegradeBitrate,
    DegradeFps,
    DegradeResolution,
    EnterPlaceholder,
    RecoverStep
};

static const char* global_action_name(GlobalControlAction a) {
    switch (a) {
        case GlobalControlAction::Noop:              return "NOOP";
        case GlobalControlAction::Idr:               return "IDR";
        case GlobalControlAction::DegradeBitrate:    return "DEGRADE_BITRATE";
        case GlobalControlAction::DegradeFps:        return "DEGRADE_FPS";
        case GlobalControlAction::DegradeResolution: return "DEGRADE_RESOLUTION";
        case GlobalControlAction::EnterPlaceholder:  return "ENTER_PLACEHOLDER";
        case GlobalControlAction::RecoverStep:       return "RECOVER_STEP";
    }
    return "NOOP";
}

struct GlobalControllerInput {
    int subscriber_count = 0;
    double bad_subscriber_ratio = 0.0;
    double dash_push_to_relay_ms = -1.0;
    double dash_relay_to_ack_ms = -1.0;
    double dash_push_to_ack_ms = -1.0;
    int64_t dash_last_ack_ms = -1;
    uint64_t queue_drops = 0;
    int64_t pub_idle_ms = -1;
};

struct GlobalDecision {
    GlobalControlAction action = GlobalControlAction::Noop;
    std::string reason;
    bool send_idr = false;
    bool exit_placeholder = false;
    int bitrate_kbps = 0;
    int fps = 0;
    int width = 0;
    int height = 0;
};

class GlobalController {
public:
    explicit GlobalController(const Config& cfg)
        : target_e2e_ms_(cfg.e2e_target_ms),
          base_bitrate_kbps_(cfg.base_bitrate_kbps),
          min_bitrate_kbps_(cfg.min_bitrate_kbps),
          base_fps_(cfg.base_fps),
          min_fps_(cfg.min_fps),
          base_width_(cfg.base_width),
          base_height_(cfg.base_height),
          min_width_(cfg.min_width),
          min_height_(cfg.min_height),
          bitrate_kbps_(cfg.base_bitrate_kbps),
          fps_(cfg.base_fps),
          width_(cfg.base_width),
          height_(cfg.base_height) {}

    GlobalDecision update(const GlobalControllerInput& in, int64_t now_ms) {
        update_counters(in);

        const bool pub_stalled = in.pub_idle_ms > 4000;
        const bool majority_bad = in.bad_subscriber_ratio > 0.60;
        const bool mid_bad = in.bad_subscriber_ratio >= 0.30 && in.bad_subscriber_ratio <= 0.60;
        const bool ack_stale = in.dash_last_ack_ms > 2500;
        const bool drops_growing = queue_drop_growth_cycles_ >= 2;
        const bool p2r_bad = in.dash_push_to_relay_ms > 500.0;
        const bool r2a_bad = in.dash_relay_to_ack_ms > 800.0;
        const bool p2r_normal = in.dash_push_to_relay_ms >= 0.0 && in.dash_push_to_relay_ms < 300.0;
        const bool relay_side_bad = r2a_bad && p2r_normal;

        if (pub_stalled) {
            if (!placeholder_active_ || now_ms - last_placeholder_ms_ >= placeholder_cooldown_ms_) {
                return enter_placeholder("publisher stalled", now_ms);
            }
            return {};
        }

        const bool normal_window =
            in.bad_subscriber_ratio < 0.10 &&
            in.dash_push_to_ack_ms >= 0.0 &&
            in.dash_push_to_ack_ms < std::max(400, target_e2e_ms_ - 200) &&
            in.dash_last_ack_ms >= 0 &&
            in.dash_last_ack_ms < 1500 &&
            in.pub_idle_ms >= 0 &&
            in.pub_idle_ms < 1500 &&
            !drops_growing;

        recovery_cycles_ = normal_window ? recovery_cycles_ + 1 : 0;
        if (recovery_cycles_ >= 5 && can_recover(now_ms)) {
            GlobalDecision d = recover_step(now_ms);
            if (d.action != GlobalControlAction::Noop) return d;
        }

        const bool severe_global = majority_bad || ack_stale || drops_growing;
        if (severe_global && can_degrade(now_ms)) {
            return degrade("global congestion", now_ms);
        }

        if (p2r_bad && push_to_relay_bad_cycles_ >= 2 && can_degrade(now_ms)) {
            return degrade("push to relay degraded", now_ms);
        }

        if (relay_side_bad && majority_bad && can_degrade(now_ms)) {
            return degrade("relay to subscriber degraded", now_ms);
        }

        if (e2e_high_cycles_ >= 3 && can_degrade(now_ms)) {
            return degrade("end to end latency high", now_ms);
        }

        if (mid_bad && push_to_ack_rising_cycles_ >= 2) {
            if (global_warning_cycles_ >= 3 && can_degrade(now_ms)) {
                return degrade("global warning escalated", now_ms);
            }
            if (can_idr(now_ms, 4000)) {
                return idr("global warning", now_ms);
            }
        }

        if ((e2e_high_cycles_ >= 6 || severe_global) &&
            degrade_actions_since_recovery_ >= 3 &&
            can_enter_placeholder(now_ms)) {
            return enter_placeholder("degrade ineffective", now_ms);
        }

        return {};
    }

    const char* state_name() const {
        if (placeholder_active_) return "GlobalPlaceholder";
        if (degrade_actions_since_recovery_ > 0) return "GlobalDegraded";
        if (global_warning_cycles_ > 0) return "GlobalWarning";
        return "Normal";
    }

    int bitrate_kbps() const { return bitrate_kbps_; }
    int fps() const { return fps_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    void update_counters(const GlobalControllerInput& in) {
        const bool e2e_high =
            in.dash_push_to_ack_ms > (double)target_e2e_ms_;
        e2e_high_cycles_ = e2e_high ? e2e_high_cycles_ + 1 : 0;

        const bool p2r_bad = in.dash_push_to_relay_ms > 500.0;
        push_to_relay_bad_cycles_ = p2r_bad ? push_to_relay_bad_cycles_ + 1 : 0;

        bool rising = false;
        if (prev_push_to_ack_ms_ >= 0.0 && in.dash_push_to_ack_ms >= 0.0) {
            rising = in.dash_push_to_ack_ms > prev_push_to_ack_ms_ + 100.0;
        }
        push_to_ack_rising_cycles_ = rising ? push_to_ack_rising_cycles_ + 1 : 0;
        prev_push_to_ack_ms_ = in.dash_push_to_ack_ms;

        if (prev_queue_drops_ != invalid_drops_ && in.queue_drops > prev_queue_drops_) {
            queue_drop_growth_cycles_ += 1;
        } else {
            queue_drop_growth_cycles_ = 0;
        }
        prev_queue_drops_ = in.queue_drops;

        const bool mid_bad = in.bad_subscriber_ratio >= 0.30 && in.bad_subscriber_ratio <= 0.60;
        global_warning_cycles_ = mid_bad ? global_warning_cycles_ + 1 : 0;
    }

    bool can_idr(int64_t now_ms, int64_t action_cooldown_ms) const {
        return now_ms - last_idr_ms_ >= idr_cooldown_ms_ &&
               now_ms - last_action_ms_ >= action_cooldown_ms;
    }

    bool can_degrade(int64_t now_ms) const {
        return now_ms - last_degrade_ms_ >= degrade_cooldown_ms_;
    }

    bool can_recover(int64_t now_ms) const {
        return now_ms - last_recover_ms_ >= recover_cooldown_ms_;
    }

    bool can_enter_placeholder(int64_t now_ms) const {
        return now_ms - last_placeholder_ms_ >= placeholder_cooldown_ms_;
    }

    GlobalDecision idr(const std::string& reason, int64_t now_ms) {
        GlobalDecision d;
        d.action = GlobalControlAction::Idr;
        d.reason = reason;
        d.send_idr = true;
        last_action_ms_ = now_ms;
        last_idr_ms_ = now_ms;
        return d;
    }

    GlobalDecision degrade(const std::string& reason, int64_t now_ms) {
        GlobalDecision d;
        d.reason = reason;
        d.send_idr = true;
        last_action_ms_ = now_ms;
        last_degrade_ms_ = now_ms;
        last_idr_ms_ = now_ms;
        recovery_cycles_ = 0;
        degrade_actions_since_recovery_ += 1;

        if (bitrate_kbps_ > min_bitrate_kbps_) {
            bitrate_kbps_ = std::max(min_bitrate_kbps_, bitrate_kbps_ * 70 / 100);
            d.action = GlobalControlAction::DegradeBitrate;
            d.bitrate_kbps = bitrate_kbps_;
            return d;
        }

        if (fps_ > min_fps_) {
            fps_ = std::max(min_fps_, fps_ * 70 / 100);
            d.action = GlobalControlAction::DegradeFps;
            d.fps = fps_;
            return d;
        }

        if (width_ > min_width_ || height_ > min_height_) {
            width_ = std::max(min_width_, width_ * 3 / 4);
            height_ = std::max(min_height_, height_ * 3 / 4);
            width_ -= width_ % 2;
            height_ -= height_ % 2;
            d.action = GlobalControlAction::DegradeResolution;
            d.width = width_;
            d.height = height_;
            return d;
        }

        return enter_placeholder(reason + ", already at floor", now_ms);
    }

    GlobalDecision enter_placeholder(const std::string& reason, int64_t now_ms) {
        GlobalDecision d;
        d.action = GlobalControlAction::EnterPlaceholder;
        d.reason = reason;
        d.send_idr = true;
        placeholder_active_ = true;
        last_action_ms_ = now_ms;
        last_placeholder_ms_ = now_ms;
        last_idr_ms_ = now_ms;
        return d;
    }

    GlobalDecision recover_step(int64_t now_ms) {
        GlobalDecision d;

        if (placeholder_active_) {
            if (now_ms - last_placeholder_ms_ < placeholder_min_hold_ms_) {
                return {};
            }
            placeholder_active_ = false;
            d.exit_placeholder = true;
        } else if (bitrate_kbps_ < base_bitrate_kbps_) {
            bitrate_kbps_ = std::min(base_bitrate_kbps_, bitrate_kbps_ * 4 / 3 + 1);
            d.bitrate_kbps = bitrate_kbps_;
        } else if (fps_ < base_fps_) {
            fps_ = std::min(base_fps_, fps_ + std::max(1, base_fps_ / 5));
            d.fps = fps_;
        } else if (width_ < base_width_ || height_ < base_height_) {
            width_ = std::min(base_width_, width_ * 4 / 3);
            height_ = std::min(base_height_, height_ * 4 / 3);
            width_ -= width_ % 2;
            height_ -= height_ % 2;
            d.width = width_;
            d.height = height_;
        } else {
            degrade_actions_since_recovery_ = 0;
            return {};
        }

        d.action = GlobalControlAction::RecoverStep;
        d.reason = "network recovered";
        d.send_idr = true;
        last_action_ms_ = now_ms;
        last_recover_ms_ = now_ms;
        last_idr_ms_ = now_ms;
        return d;
    }

    int target_e2e_ms_;
    int base_bitrate_kbps_;
    int min_bitrate_kbps_;
    int base_fps_;
    int min_fps_;
    int base_width_;
    int base_height_;
    int min_width_;
    int min_height_;

    int bitrate_kbps_;
    int fps_;
    int width_;
    int height_;

    int e2e_high_cycles_ = 0;
    int push_to_relay_bad_cycles_ = 0;
    int push_to_ack_rising_cycles_ = 0;
    int queue_drop_growth_cycles_ = 0;
    int recovery_cycles_ = 0;
    int global_warning_cycles_ = 0;
    int degrade_actions_since_recovery_ = 0;
    bool placeholder_active_ = false;

    double prev_push_to_ack_ms_ = -1.0;
    static constexpr uint64_t invalid_drops_ = std::numeric_limits<uint64_t>::max();
    uint64_t prev_queue_drops_ = invalid_drops_;

    int64_t last_action_ms_ = -100000;
    int64_t last_idr_ms_ = -100000;
    int64_t last_degrade_ms_ = -100000;
    int64_t last_recover_ms_ = -100000;
    int64_t last_placeholder_ms_ = -100000;

    static constexpr int64_t idr_cooldown_ms_ = 2000;
    static constexpr int64_t degrade_cooldown_ms_ = 9000;
    static constexpr int64_t recover_cooldown_ms_ = 15000;
    static constexpr int64_t placeholder_cooldown_ms_ = 10000;
    static constexpr int64_t placeholder_min_hold_ms_ = 5000;
};

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

class SubscriberSession {
public:
    SubscriberSession(SRTSOCKET sock, sockaddr_in peer,
                      StreamStatus& status, const Config& cfg, uint64_t id)
        : sock_(sock), peer_(peer), status_(status), cfg_(cfg), id_(id) {}
    ~SubscriberSession() { stop(); }

    void start() { worker_ = std::thread([this]{ run(); }); }

    void stop() {
        bool first = false;
        if (!stopped_.compare_exchange_strong(first, true)) return;
        running_.store(false);
        { std::lock_guard<std::mutex> lk(mu_); closed_ = true; }
        cv_.notify_all();
        srt_close(sock_);
        if (worker_.joinable()) worker_.join();
    }

    bool push_chunk(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return false;
        if (!synced_) {
            if (!has_rap(data, len)) return true;
            synced_ = true;
        }

        SubscriberAction action = control_action();
        if (action == SubscriberAction::Placeholder) {
            if (!queue_.empty()) {
                status_.subscriber_queue_drops.fetch_add((uint64_t)queue_.size(), std::memory_order_relaxed);
                queue_.clear();
            }
            std::vector<uint8_t> placeholder = make_null_ts_chunk(len);
            queue_.emplace_back(placeholder.data(), placeholder.data() + placeholder.size(), steady_ms());
            cv_.notify_one();
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
        queue_.emplace_back(data, data + len, steady_ms());
        cv_.notify_one();
        return true;
    }

    double telemetryPushToRelayMs() const { return telemetry_push_to_relay_ms_.load(); }
    double telemetryRelayToAckMs() const { return telemetry_relay_to_ack_ms_.load(); }
    double telemetryPushToAckMs()  const { return telemetry_push_to_ack_ms_.load(); }
    int64_t telemetryLastAckMs()   const { return telemetry_last_ack_ms_.load(); }

    void updateTelemetryLatency(double push_to_relay_ms,
                                double relay_to_ack_ms,
                                double push_to_ack_ms) {
        telemetry_push_to_relay_ms_.store(push_to_relay_ms);
        telemetry_relay_to_ack_ms_.store(relay_to_ack_ms);
        telemetry_push_to_ack_ms_.store(push_to_ack_ms);
        telemetry_last_ack_ms_.store(steady_ms());
    }

    bool        running()        const { return running_.load(); }
    uint64_t    id()             const { return id_; }
    std::string peer()           const { return sockaddr_to_string(peer_); }
    uint64_t    sent_bytes()     const { return sent_bytes_.load(); }
    int64_t     last_active_ms() const { return last_active_ms_.load(); }
    int         queue_depth()    const { std::lock_guard<std::mutex> lk(mu_); return (int)queue_.size(); }
    int         latency_ms()     const { return latency_ms_.load(); }
    SubscriberAction control_action() const {
        return (SubscriberAction)control_action_.load(std::memory_order_relaxed);
    }
    const char* control_action_name() const { return subscriber_action_name(control_action()); }
    bool        bad_for_global() const { return control_action() != SubscriberAction::Normal; }

    SubscriberControlResult evaluate_control(int64_t now_ms) {
        SubscriberSample sample;
        sample.queue_depth = queue_depth();
        sample.queue_max = cfg_.subscriber_queue_max_chunks;
        int64_t last_active = last_active_ms_.load();
        sample.idle_ms = last_active > 0 ? now_ms - last_active : -1;
        int64_t last_ack = telemetry_last_ack_ms_.load();
        sample.telemetry_last_ack_age_ms = last_ack > 0 ? now_ms - last_ack : -1;
        sample.telemetry_relay_to_ack_ms = telemetry_relay_to_ack_ms_.load();

        SubscriberControlResult result = controller_.update(sample, now_ms);
        if (result.changed) set_control_action(result.action);
        return result;
    }

private:
    struct Chunk {
        std::vector<uint8_t> data;
        int64_t ts;
        Chunk(const uint8_t* b, const uint8_t* e, int64_t t) : data(b,e), ts(t) {}
    };

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

    void run() {
        status_.subscriber_count.fetch_add(1);
        last_active_ms_.store(steady_ms());
        latency_ms_.store(read_latency(sock_));
        
        log_line("INFO", "subscriber connected: " + sockaddr_to_string(peer_));
        dump_srt_latency("[SUB]", sock_);
        log_line("INFO", "sub#" + std::to_string(id_) + " started: " + peer());

        while (running_.load() && g_running.load()) {
            Chunk chunk(nullptr, nullptr, 0);
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, 500ms, [&]{ return !queue_.empty()||closed_||!running_.load()||!g_running.load(); });
                if (!running_.load() || closed_ || !g_running.load()) break;
                if (queue_.empty()) continue;
                chunk = std::move(queue_.front());
                queue_.pop_front();
            }
            int n = srt_send(sock_, (const char*)chunk.data.data(), (int)chunk.data.size());
            if (n == SRT_ERROR) { log_line("WARN","sub#"+std::to_string(id_)+" send: "+last_srt_error()); break; }
            sent_bytes_.fetch_add((uint64_t)n);
            status_.total_egress_bytes.fetch_add((uint64_t)n);
            status_.egress_bytes_window.fetch_add((uint64_t)n);
            last_active_ms_.store(steady_ms());
        }
        status_.subscriber_count.fetch_sub(1);
        running_.store(false);
        log_line("INFO", "sub#" + std::to_string(id_) + " stopped");
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
    std::condition_variable cv_;
    std::deque<Chunk> queue_;
    bool closed_ = false, synced_ = false;
    uint8_t placeholder_cc_ = 0;

    std::atomic<bool> running_{true}, stopped_{false};
    std::thread worker_;
    std::atomic<uint64_t> sent_bytes_{0};
    std::atomic<int64_t>  last_active_ms_{0};
    std::atomic<int>      latency_ms_{-1};
    std::atomic<double> telemetry_push_to_relay_ms_{-1.0};
    std::atomic<double> telemetry_relay_to_ack_ms_{-1.0};
    std::atomic<double> telemetry_push_to_ack_ms_{-1.0};
    std::atomic<int64_t> telemetry_last_ack_ms_{-1};
    std::atomic<int> control_action_{(int)SubscriberAction::Normal};
    SubscriberController controller_;
};

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

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i+1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if      (a=="--bind")          cfg.bind_ip                    = val();
        else if (a=="--pub-port")      cfg.publisher_port              = std::stoi(val());
        else if (a=="--sub-port")      cfg.subscriber_port             = std::stoi(val());
        else if (a=="--ws-port")       cfg.ws_port                     = std::stoi(val());
        else if (a=="--pusher-ctrl-host") cfg.pusher_control_host       = val();
        else if (a=="--pusher-ctrl-port") cfg.pusher_control_port       = std::stoi(val());
        else if (a=="--no-auto-control")  cfg.auto_control              = false;
        else if (a=="--base-bitrate")  cfg.base_bitrate_kbps           = std::stoi(val());
        else if (a=="--min-bitrate")   cfg.min_bitrate_kbps            = std::stoi(val());
        else if (a=="--base-fps")      cfg.base_fps                    = std::stoi(val());
        else if (a=="--min-fps")       cfg.min_fps                     = std::stoi(val());
        else if (a=="--base-width")    cfg.base_width                  = std::stoi(val());
        else if (a=="--base-height")   cfg.base_height                 = std::stoi(val());
        else if (a=="--min-width")     cfg.min_width                   = std::stoi(val());
        else if (a=="--min-height")    cfg.min_height                  = std::stoi(val());
        else if (a=="--e2e-target-ms") cfg.e2e_target_ms               = std::stoi(val());
        else if (a=="--payload-size")  cfg.payload_size                = std::stoi(val());
        else if (a=="--latency-ms")    cfg.latency_ms                  = std::stoi(val());
        else if (a=="--recv-buf")      cfg.recv_buf_bytes              = std::stoi(val());
        else if (a=="--send-buf")      cfg.send_buf_bytes              = std::stoi(val());
        else if (a=="--sub-queue")     cfg.subscriber_queue_max_chunks = std::stoi(val());
        else if (a=="--max-subs")      cfg.max_subscribers             = std::stoi(val());
        else if (a=="--pub-idle-ms")   cfg.publisher_idle_timeout_ms   = std::stoi(val());
        else if (a=="--no-validate-ts")cfg.validate_ts                 = false;
        else if (a=="--require-h264")  cfg.require_h264                = true;
        else if (a=="--require-audio") cfg.require_audio               = true;
        else if (a=="--passphrase")    cfg.passphrase                  = val();
        else if (a=="--pbkeylen")      cfg.pbkeylen                    = std::stoi(val());
        else if (a=="--monitor-ms")    cfg.monitor_interval_ms         = std::stoi(val());
        else if (a=="--no-ws")         cfg.ws_port                     = 0;
        else if (a=="--help") {
            std::cout <<
                "Usage: srt_relay_server [options]\n"
                "  --pub-port    9000\n"
                "  --sub-port    9001\n"
                "  --ws-port     8765   (0=disable websocket)\n"
                "  --bind        0.0.0.0\n"
                "  --pusher-ctrl-host <ip> (default=publisher peer ip)\n"
                "  --pusher-ctrl-port 10090\n"
                "  --no-auto-control\n"
                "  --base-bitrate 2000 --min-bitrate 350\n"
                "  --base-fps 25 --min-fps 10\n"
                "  --base-width 1280 --base-height 720\n"
                "  --min-width 426 --min-height 240\n"
                "  --e2e-target-ms 800\n"
                "  --latency-ms  40\n"
                "  --monitor-ms  2000\n"
                "  --max-subs    256\n"
                "  --pub-idle-ms 15000\n"
                "  --payload-size 4096\n"
                "  --no-validate-ts\n"
                "  --require-h264\n"
                "  --require-audio\n"
                "  --passphrase <str>\n"
                "  --pbkeylen   16|24|32\n";
            std::exit(0);
        }
        else throw std::runtime_error("unknown arg: " + a);
    }
    if (!(cfg.pbkeylen==16||cfg.pbkeylen==24||cfg.pbkeylen==32))
        throw std::runtime_error("pbkeylen must be 16, 24, or 32");
    if (cfg.min_bitrate_kbps <= 0 || cfg.base_bitrate_kbps < cfg.min_bitrate_kbps)
        throw std::runtime_error("bitrate bounds are invalid");
    if (cfg.min_fps <= 0 || cfg.base_fps < cfg.min_fps)
        throw std::runtime_error("fps bounds are invalid");
    if (cfg.min_width <= 0 || cfg.min_height <= 0 ||
        cfg.base_width < cfg.min_width || cfg.base_height < cfg.min_height)
        throw std::runtime_error("resolution bounds are invalid");
    return cfg;
}

void sig_handler(int) { g_running.store(false); }

} // namespace


int main(int argc, char** argv) {
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);
    try {
        Config cfg = parse_args(argc, argv);
        RelayServer server(cfg);
        if (!server.start()) return 1;
        while (g_running.load()) std::this_thread::sleep_for(200ms);
        server.stop();
        return 0;
    } catch (const std::exception& ex) {
        log_line("ERROR", ex.what());
        return 1;
    }
}
