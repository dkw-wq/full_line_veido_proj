// srt_server/src/main.cpp
// Based on the original single-channel relay server, with an embedded lightweight WebSocket server
// Push JSON monitoring data to all connected WebSocket clients every monitor_interval_ms
//
// Build:
//   g++ -std=c++17 -O2 -o srt_relay_server srt_relay_server_ws.cpp \
//       -lsrt -lpthread
//
// Usage:
//   ./srt_relay_server --ws-port 8765 [other parameters same as original]
//
// Frontend: open dashboard.html in browser and connect to ws://SERVER_IP:8765

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
#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <mutex>
#include <iomanip>
#include "telemetry.h" 

// SHA-1 + Base64 for WebSocket handshake (self-contained, no external dependencies)
#include <cstdint>
namespace ws_util {

static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static std::string sha1_b64(const std::string& input) {
    // SHA-1
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
    // Base64
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

// Extract header value from HTTP request
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

// Build RFC6455 text frame (server -> client, no masking)
static std::vector<uint8_t> ws_frame(const std::string& text) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + opcode=text
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

// Read and decode one frame (text frame only, masked, client -> server)
// Return "" if connection closed or error
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

// ============================================================
// Configuration
// ============================================================

struct Config {
    std::string bind_ip               = "0.0.0.0";
    int publisher_port                = 9000;
    int subscriber_port               = 9001;
    int ws_port                       = 8765;   // WebSocket 监控端口
    int payload_size                  = 1316;
    int latency_ms                    = 40;
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

// ============================================================
// Stream state
// ============================================================

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

// ============================================================
// WebSocket broadcaster
// Called by monitor_loop to push JSON to all connected clients
// ============================================================

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

   // Broadcast JSON string to all connected clients
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
            // Use select with timeout to avoid blocking accept indefinitely
            fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_fd_, &rfds);
            timeval tv{1, 0};
            if (select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
            int cfd = accept(listen_fd_, nullptr, nullptr);
            if (cfd < 0) continue;
            // Perform handshake in a separate thread to avoid blocking accept
            std::thread([this, cfd]{ handshake(cfd); }).detach();
        }
    }

    void handshake(int cfd) {
        // Set receive timeout to prevent slow clients
        timeval tv{5, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[4096] = {0};
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(cfd); return; }

        std::string req(buf, n);
        std::string upgrade = ws_util::http_header(req, "Upgrade");
        std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
        if (upgrade != "websocket") {
            // Not a WebSocket handshake, return HTTP 403
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

        // Handshake successful, clear timeout and add to broadcast list
        tv = {0, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        {
            std::lock_guard<std::mutex> lk(clients_mu_);
            clients_.insert(cfd);
        }

        // Keep connection alive until client closes or error occurs
        while (running_.load()) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(cfd, &rfds);
            timeval stv{2, 0};
            int r = select(cfd + 1, &rfds, nullptr, nullptr, &stv);
            if (r < 0) break;
            if (r == 0) continue;
            std::string frame = ws_util::ws_recv_frame(cfd);
            if (frame.empty()) break; // 
        }

        {
            std::lock_guard<std::mutex> lk(clients_mu_);
            clients_.erase(cfd);
        }
        close(cfd);
    }

    int             listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread     accept_thread_;
    mutable std::mutex clients_mu_;
    std::set<int>   clients_;
};

// ============================================================
// TS continuity validation
// ============================================================

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

// ============================================================
// SubscriberSession 
// ============================================================

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
        if (queue_.size() >= (size_t)cfg_.subscriber_queue_max_chunks) {
            queue_.pop_front();
            status_.subscriber_queue_drops.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.emplace_back(data, data + len, steady_ms());
        cv_.notify_one();
        return true;
    }

    bool        running()          const { return running_.load(); }
    uint64_t    id()               const { return id_; }
    std::string peer()             const { return sockaddr_to_string(peer_); }
    uint64_t    sent_bytes()       const { return sent_bytes_.load(); }
    int64_t     last_active_ms()   const { return last_active_ms_.load(); }
    int         queue_depth()      const { std::lock_guard<std::mutex> lk(mu_); return (int)queue_.size(); }
    int         latency_ms()       const { return latency_ms_.load(); }

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

    void run() {
        status_.subscriber_count.fetch_add(1);
        last_active_ms_.store(steady_ms());
        latency_ms_.store(read_latency(sock_));
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

    SRTSOCKET sock_;
    sockaddr_in peer_;
    StreamStatus& status_;
    const Config& cfg_;
    uint64_t id_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Chunk> queue_;
    bool closed_ = false, synced_ = false;

    std::atomic<bool>     running_{true}, stopped_{false};
    std::thread           worker_;
    std::atomic<uint64_t> sent_bytes_{0};
    std::atomic<int64_t>  last_active_ms_{0};
    std::atomic<int>      latency_ms_{-1};
};

// ============================================================
// TelemetryRelay — receive telemetry UDP packets and forward to subscribers
// ============================================================

class TelemetryRelay {
public:
    TelemetryRelay() = default;
    ~TelemetryRelay() { stop(); }

    // subscriber_ips: 回调，每次转发前调用以获取当前订阅者 IP 列表
    using IpListFn = std::function<std::vector<std::string>()>;

    bool start(const std::string& bind_ip, int listen_port,
            int fwd_port, IpListFn ip_fn) {
        fwd_port_ = fwd_port;
        ip_fn_    = std::move(ip_fn);

        recv_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (recv_fd_ < 0) {
            log_line("ERROR", "TLMY recv socket failed");
            return false;
        }

        int yes = 1;
        setsockopt(recv_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)listen_port);
        inet_pton(AF_INET,
                (bind_ip.empty() || bind_ip == "0.0.0.0") ? "0.0.0.0" : bind_ip.c_str(),
                &addr.sin_addr);

        if (bind(recv_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            log_line("ERROR", "TLMY bind :" + std::to_string(listen_port) + " failed");
            close(recv_fd_);
            recv_fd_ = -1;
            return false;
        }

        send_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (send_fd_ < 0) {
            log_line("ERROR", "TLMY send socket failed");
            return false;
        }

        ack_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (ack_fd_ < 0) {
            log_line("ERROR", "TLMY ack socket failed");
            return false;
        }

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
        recv_thread_ = std::thread([this]{ recv_loop(); });
        ack_thread_  = std::thread([this]{ ack_loop(); });

        log_line("INFO", "TelemetryRelay listening :" + std::to_string(listen_port)
                        + " -> fwd :" + std::to_string(fwd_port_)
                        + ", ack :" + std::to_string(TLMY_ACK_PORT));
        return true;
    }

    void stop() {
        running_.store(false);

        if (recv_fd_ >= 0) { close(recv_fd_); recv_fd_ = -1; }
        if (send_fd_ >= 0) { close(send_fd_); send_fd_ = -1; }
        if (ack_fd_  >= 0) { close(ack_fd_);  ack_fd_  = -1; }

        if (recv_thread_.joinable()) recv_thread_.join();
        if (ack_thread_.joinable())  ack_thread_.join();
    }

private:
    void recv_loop() {
        while (running_.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(recv_fd_, &rfds);
            timeval tv{1, 0};
            if (select(recv_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

            TelemetryPkt pkt{};
            ssize_t n = recv(recv_fd_, &pkt, sizeof(pkt), 0);
            if (n != sizeof(TelemetryPkt)) continue;

            TelemetryPkt decoded{};
            if (!tlmy_decode(&pkt, (int)n, &decoded)) continue;

            // 保留现有 push->relay 所需字段
            decoded.t_relay_in_us = wall_us();

            TelemetryPkt to_send{};
            tlmy_encode(&to_send, decoded.seq, decoded.t_push_us, decoded.t_relay_in_us);

            auto ips = ip_fn_();
            if (ips.empty()) continue;

            const uint64_t send_mono = mono_us();

            {
                std::lock_guard<std::mutex> lk(sent_mu_);
                sent_mono_us_[decoded.seq] = send_mono;

                // 防止表无限长，只保留最近少量
                if (sent_mono_us_.size() > 2048) {
                    auto it = sent_mono_us_.begin();
                    sent_mono_us_.erase(it);
                }
            }

            for (const auto& ip : ips) {
                sockaddr_in dst{};
                dst.sin_family = AF_INET;
                dst.sin_port   = htons((uint16_t)fwd_port_);
                inet_pton(AF_INET, ip.c_str(), &dst.sin_addr);

                sendto(send_fd_, &to_send, sizeof(to_send), 0,
                    (sockaddr*)&dst, sizeof(dst));
            }
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
            if (n != sizeof(TelemetryAck)) continue;
            if (!tlmy_ack_decode(&raw, (int)n, &ack)) continue;

            uint64_t sent_us = 0;
            {
                std::lock_guard<std::mutex> lk(sent_mu_);
                auto it = sent_mono_us_.find(ack.seq);
                if (it == sent_mono_us_.end()) continue;
                sent_us = it->second;
                sent_mono_us_.erase(it);
            }

            const uint64_t now_us = mono_us();
            const double rtt_ms = ((double)((int64_t)now_us - (int64_t)sent_us)) / 1000.0;
            const double one_way_est_ms = rtt_ms / 2.0;

            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(3)
                << "[LAT-ACK] seq=" << ack.seq
                << " peer=" << ipbuf
                << " relay<->play_rtt=" << rtt_ms << "ms"
                << " relay->play_est=" << one_way_est_ms << "ms";
            log_line("INFO", oss.str());
        }
    }

    int recv_fd_ = -1;
    int send_fd_ = -1;
    int ack_fd_  = -1;

    int fwd_port_ = TLMY_FWD_PORT;
    IpListFn ip_fn_;

    std::atomic<bool> running_{false};
    std::thread recv_thread_;
    std::thread ack_thread_;

    std::mutex sent_mu_;
    std::unordered_map<uint64_t, uint64_t> sent_mono_us_; // seq -> 转发时刻 mono_us
};

// ============================================================
// RelayServer —  monitor_loop  ws_.broadcast()
// ============================================================

class RelayServer {
public:
    explicit RelayServer(Config cfg)
        : cfg_(std::move(cfg)), validator_(cfg_.require_h264, cfg_.require_audio) {}
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

        
        tlmy_relay_.start(cfg_.bind_ip, TLMY_PORT, TLMY_FWD_PORT, [this]() {

            std::vector<std::string> ips;
            std::lock_guard<std::mutex> lk(sub_mu_);
            for (auto& [id, sess] : subscribers_) {
                if (!sess->running()) continue;
                std::string peer = sess->peer(); // "1.2.3.4:5678"
                auto colon = peer.rfind(':');
                if (colon != std::string::npos)
                    ips.push_back(peer.substr(0, colon));
            }
            
            std::sort(ips.begin(), ips.end());
            ips.erase(std::unique(ips.begin(), ips.end()), ips.end());
            return ips;
        });

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
    TelemetryRelay tlmy_relay_;

    SRTSOCKET create_listener(int port, bool for_pub) {
        SRTSOCKET s = srt_socket(AF_INET, SOCK_DGRAM, 0);
        if (s == SRT_INVALID_SOCK) return SRT_INVALID_SOCK;
        auto sf = [&](SRT_SOCKOPT opt, const void* v, int l, const char* n) {
            if (srt_setsockflag(s, opt, v, l) != 0)
                log_line("WARN", std::string("set ") + n + ": " + last_srt_error());
        };
        int yes=1, lat=cfg_.latency_ms, rb=cfg_.recv_buf_bytes, sb=cfg_.send_buf_bytes;
        int tt=SRTT_LIVE, rto=for_pub?cfg_.publisher_idle_timeout_ms:5000;
        sf(SRTO_REUSEADDR,&yes,sizeof(yes),"REUSEADDR");
        sf(SRTO_RCVSYN,&yes,sizeof(yes),"RCVSYN");
        sf(SRTO_SNDSYN,&yes,sizeof(yes),"SNDSYN");
        sf(SRTO_TSBPDMODE,&yes,sizeof(yes),"TSBPDMODE");
        sf(SRTO_LATENCY,&lat,sizeof(lat),"LATENCY");
        sf(SRTO_RCVBUF,&rb,sizeof(rb),"RCVBUF");
        sf(SRTO_SNDBUF,&sb,sizeof(sb),"SNDBUF");
        sf(SRTO_TRANSTYPE,&tt,sizeof(tt),"TRANSTYPE");
        sf(SRTO_RCVTIMEO,&rto,sizeof(rto),"RCVTIMEO");
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
            log_line("INFO","publisher connected: "+sockaddr_to_string(peer));
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

    
    void monitor_loop() {
        
        static constexpr int HIST = 60;
        std::deque<double> ingress_hist, egress_hist;

        while (g_running.load()) {
            std::this_thread::sleep_for(Ms(cfg_.monitor_interval_ms));
            if (!g_running.load()) break;

            uint64_t ib = status_.ingress_bytes_window.exchange(0);
            uint64_t eb = status_.egress_bytes_window.exchange(0);
            double sec = cfg_.monitor_interval_ms / 1000.0;
            double in_mbps  = (ib * 8.0) / sec / 1e6;
            double out_mbps = (eb * 8.0) / sec / 1e6;

            ingress_hist.push_back(in_mbps);
            egress_hist.push_back(out_mbps);
            if ((int)ingress_hist.size() > HIST) ingress_hist.pop_front();
            if ((int)egress_hist.size()  > HIST) egress_hist.pop_front();

        
            std::ostringstream log_oss;
            log_oss << std::fixed << std::setprecision(2)
                    << "pub=" << (status_.publisher_online.load() ? 1 : 0)
                    << " in=" << in_mbps << "Mbps"
                    << " out=" << out_mbps << "Mbps"
                    << " subs=" << status_.subscriber_count.load()
                    << " disc=" << status_.ts_discontinuities.load()
                    << " drops=" << status_.subscriber_queue_drops.load();
            log_line("STAT", log_oss.str());

       
            if (cfg_.ws_port > 0 && ws_.client_count() > 0) {
                std::ostringstream j;
                j << std::fixed << std::setprecision(3);
                j << "{"
                  << "\"ts\":" << steady_ms() << ","
                  << "\"wall_time\":\"" << now_wall_time() << "\","
                  << "\"pub_online\":" << (status_.publisher_online.load() ? "true" : "false") << ","
                  << "\"ingress_mbps\":" << in_mbps << ","
                  << "\"egress_mbps\":" << out_mbps << ","
                  << "\"subscribers\":" << status_.subscriber_count.load() << ","
                  << "\"total_ingress_mb\":" << (status_.total_ingress_bytes.load() / 1048576.0) << ","
                  << "\"total_egress_mb\":" << (status_.total_egress_bytes.load() / 1048576.0) << ","
                  << "\"ingress_chunks\":" << status_.ingress_chunks.load() << ","
                  << "\"ts_disc\":" << status_.ts_discontinuities.load() << ","
                  << "\"queue_drops\":" << status_.subscriber_queue_drops.load() << ","
                  << "\"pub_idle_ms\":" << (status_.publisher_last_active_at_ms.load() > 0
                                            ? steady_ms() - status_.publisher_last_active_at_ms.load()
                                            : -1) << ","
                  << "\"ws_clients\":" << ws_.client_count() << ","
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
                          << "\"idle_ms\":" << (steady_ms() - sess->last_active_ms())
                          << "}";
                        ++idx;
                    }
                }
                j << "]}";
                ws_.broadcast(j.str());
            }
        }
    }

    static int64_t ms_ago(int64_t t) { return t > 0 ? steady_ms()-t : -1; }

    Config       cfg_;
    StreamStatus status_;
    TsValidator  validator_;
    WsBroadcaster ws_;

    std::atomic<bool> stopped_{true};

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

// ============================================================
// Command line parsing
// ============================================================

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
                "  --ws-port     8765   WebSocket 监控端口 (0=禁用)\n"
                "  --bind        0.0.0.0\n"
                "  --latency-ms  40\n"
                "  --monitor-ms  2000\n"
                "  --max-subs    256\n"
                "  --pub-idle-ms 15000\n"
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