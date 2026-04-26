using Ms       = std::chrono::milliseconds;
using Clock    = std::chrono::steady_clock;
using SysClock = std::chrono::system_clock;
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
