#include <winsock2.h>
#include <ws2tcpip.h>
#include <srt/srt.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

volatile std::sig_atomic_t stop_requested = 0;

void on_interrupt(int) { stop_requested = 1; }

struct Options {
    std::string host = "jfznbx.cn";
    std::string stream_id = "cam1";
    std::string csv_path;
    std::string trace_path;
    int port = 9001;
    int clients = 1;
    int ramp_ms = 200;
    int duration_sec = 60;
    int latency_ms = 20;
    int connect_timeout_ms = 5000;
    int idle_ms = 3000;
    int slow_every = 0;
    int slow_read_ms = 500;
    bool help = false;
};

void print_usage() {
    std::cout
        << "SRT receiver load test (Windows)\n"
        << "  --host HOST                 relay hostname or IPv4 (default jfznbx.cn)\n"
        << "  --port N                    subscriber UDP port (default 9001)\n"
        << "  --clients N                 total connections to attempt (default 1)\n"
        << "  --ramp-ms N                 delay between attempts (default 200)\n"
        << "  --duration-sec N            run time after final attempt (default 60)\n"
        << "  --latency-ms N              SRT receive/peer latency (default 20)\n"
        << "  --connect-timeout-ms N      connection deadline (default 5000)\n"
        << "  --idle-ms N                 no-data threshold (default 3000)\n"
        << "  --stream-id TEXT            SRT stream ID; empty string disables it\n"
        << "  --slow-every N              make every Nth receiver slow (default 0)\n"
        << "  --slow-read-ms N            interval for one slow read (default 500)\n"
        << "  --csv FILE                  write one summary row per second\n"
        << "  --trace FILE                write per-client connection events\n"
        << "  --help                      show this help\n";
}

int parse_number(const std::string& text, int min_value, int max_value, const char* name) {
    size_t end = 0;
    long long value = 0;
    try {
        value = std::stoll(text, &end);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid ") + name + ": " + text);
    }
    if (end != text.size() || value < min_value || value > max_value)
        throw std::runtime_error(std::string("invalid ") + name + ": " + text);
    return static_cast<int>(value);
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { o.help = true; return o; }
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
        const std::string value = argv[++i];
        if (arg == "--host") o.host = value;
        else if (arg == "--port") o.port = parse_number(value, 1, 65535, "port");
        else if (arg == "--clients") o.clients = parse_number(value, 1, 4096, "clients");
        else if (arg == "--ramp-ms") o.ramp_ms = parse_number(value, 0, 60000, "ramp-ms");
        else if (arg == "--duration-sec") o.duration_sec = parse_number(value, 1, 86400, "duration-sec");
        else if (arg == "--latency-ms") o.latency_ms = parse_number(value, 1, 60000, "latency-ms");
        else if (arg == "--connect-timeout-ms")
            o.connect_timeout_ms = parse_number(value, 100, 60000, "connect-timeout-ms");
        else if (arg == "--idle-ms") o.idle_ms = parse_number(value, 100, 60000, "idle-ms");
        else if (arg == "--stream-id") o.stream_id = value;
        else if (arg == "--slow-every") o.slow_every = parse_number(value, 0, 4096, "slow-every");
        else if (arg == "--slow-read-ms") o.slow_read_ms = parse_number(value, 1, 60000, "slow-read-ms");
        else if (arg == "--csv") o.csv_path = value;
        else if (arg == "--trace") o.trace_path = value;
        else throw std::runtime_error("unknown option: " + arg);
    }
    if (o.host.empty()) throw std::runtime_error("host must not be empty");
    if (o.stream_id.size() > 480) throw std::runtime_error("stream-id is too long (max 480 with trace tag)");
    return o;
}

class Runtime {
public:
    Runtime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("WSAStartup failed");
        winsock_started_ = true;
        if (srt_startup() != 0) {
            WSACleanup();
            throw std::runtime_error("srt_startup failed");
        }
        srt_started_ = true;
    }
    ~Runtime() {
        if (srt_started_) srt_cleanup();
        if (winsock_started_) WSACleanup();
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

private:
    bool winsock_started_ = false;
    bool srt_started_ = false;
};

sockaddr_in resolve_target(const Options& o) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = nullptr;
    const int result = getaddrinfo(o.host.c_str(), std::to_string(o.port).c_str(), &hints, &list);
    if (result != 0 || !list)
        throw std::runtime_error("cannot resolve host: " + o.host);
    sockaddr_in address = *reinterpret_cast<sockaddr_in*>(list->ai_addr);
    freeaddrinfo(list);
    return address;
}

enum class Phase { Pending, Connecting, Connected, Closed };

struct Client {
    int id = 0;
    SRTSOCKET socket = SRT_INVALID_SOCK;
    Phase phase = Phase::Pending;
    TimePoint started{};
    TimePoint connected{};
    TimePoint last_data{};
    TimePoint next_slow_read{};
    uint64_t bytes = 0;
    uint64_t last_report_bytes = 0;
    int64_t connect_ms = -1;
    int64_t loss = 0;
    int64_t drop = 0;
    double rtt_ms = 0;
    int rcvbuf_packets = -1;
    int avail_rcvbuf_bytes = -1;
    bool slow = false;
    bool slow_paused = false;
    int local_port = -1;
    int64_t launch_lag_ms = 0;
    std::string end_reason;
    std::string stream_id;
};

class LoadTest {
public:
    LoadTest(const Options& options, sockaddr_in target)
        : options_(options), target_(target), clients_(options.clients),
          events_(static_cast<size_t>(options.clients) + 8) {
        epoll_id_ = srt_epoll_create();
        if (epoll_id_ < 0) throw std::runtime_error("srt_epoll_create failed");
        if (!options_.trace_path.empty()) {
            trace_.open(options_.trace_path, std::ios::out | std::ios::trunc);
            if (!trace_) {
                srt_epoll_release(epoll_id_);
                epoll_id_ = -1;
                throw std::runtime_error("cannot open trace: " + options_.trace_path);
            }
            trace_ << "wall_time,elapsed_ms,client,event,local_port,srt_socket,srt_state,connect_ms,launch_lag_ms,loop_gap_max_ms,stream_id,detail\n";
        }
        if (!options_.csv_path.empty()) {
            csv_.open(options_.csv_path, std::ios::out | std::ios::trunc);
            if (!csv_) {
                srt_epoll_release(epoll_id_);
                epoll_id_ = -1;
                throw std::runtime_error("cannot open CSV: " + options_.csv_path);
            }
            csv_ << "elapsed_s,attempted,connected,connect_fail,disconnect,idle,rx_mbps,"
                    "min_client_mbps,p50_client_mbps,p95_client_mbps,p95_connect_ms,"
                    "loss,drop,avg_rtt_ms,loop_gap_max_ms,launch_lag_max_ms,"
                    "start_call_max_ms,event_batch_max_ms,event_batch_interval_max_ms,"
                    "max_rcvbuf_packets,min_avail_rcvbuf_bytes,"
                    "handle_event_interval_max_ms,recv_call_interval_max_ms\n";
        }
    }

    ~LoadTest() {
        for (Client& c : clients_) close_client(c, "finished", true);
        if (epoll_id_ >= 0) srt_epoll_release(epoll_id_);
    }

    int run() {
        const TimePoint start = Clock::now();
        test_start_ = start;
        last_loop_ = start;
        TimePoint next_start = start;
        TimePoint last_report = start;
        TimePoint finish{};
        bool finish_set = false;

        std::cout << "target=" << options_.host << ':' << options_.port
                  << " clients=" << options_.clients << " ramp_ms=" << options_.ramp_ms
                  << " duration_sec=" << options_.duration_sec << '\n';

        while (!stop_requested) {
            const TimePoint loop_at = Clock::now();
            max_loop_gap_ms_ = std::max(max_loop_gap_ms_,
                std::chrono::duration_cast<std::chrono::milliseconds>(loop_at - last_loop_).count());
            last_loop_ = loop_at;
            while (attempted_ < options_.clients && Clock::now() >= next_start) {
                const TimePoint attempt_at = Clock::now();
                const TimePoint planned_start = start +
                    std::chrono::milliseconds(static_cast<int64_t>(attempted_) * options_.ramp_ms);
                clients_[attempted_].launch_lag_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(attempt_at - planned_start).count();
                max_launch_lag_ms_ = std::max(max_launch_lag_ms_, clients_[attempted_].launch_lag_ms);
                start_client(clients_[attempted_], attempted_ + 1, attempt_at);
                max_start_call_ms_ = std::max(max_start_call_ms_,
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - attempt_at).count());
                ++attempted_;
                next_start = attempt_at + std::chrono::milliseconds(options_.ramp_ms);
            }
            if (!finish_set && attempted_ == options_.clients) {
                finish = Clock::now() + std::chrono::seconds(options_.duration_sec);
                finish_set = true;
            }

            const TimePoint now = Clock::now();
            for (Client& c : clients_) {
                if (c.phase == Phase::Connecting &&
                    now - c.started >= std::chrono::milliseconds(options_.connect_timeout_ms)) {
                    close_client(c, "connect timeout");
                } else if (c.phase == Phase::Connected && c.slow_paused && now >= c.next_slow_read) {
                    c.slow_paused = false;
                    update_events(c, SRT_EPOLL_IN | SRT_EPOLL_ERR);
                }
            }

            if (now - last_report >= std::chrono::seconds(1)) {
                report(start, now, last_report);
                last_report = now;
            }
            if (finish_set && now >= finish) break;

            if (by_socket_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
            const int count = srt_epoll_uwait(epoll_id_, events_.data(),
                                               static_cast<int>(events_.size()), 100);
            if (count == SRT_ERROR) {
                throw std::runtime_error(std::string("srt_epoll_uwait: ") + srt_getlasterror_str());
            }
            const TimePoint batch_start = Clock::now();
            for (int i = 0; i < std::min(count, static_cast<int>(events_.size())); ++i) {
                const TimePoint event_start = Clock::now();
                handle_event(events_[i]);
                max_handle_event_interval_ms_ = std::max(max_handle_event_interval_ms_,
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - event_start).count());
            }
            max_event_batch_ms_ = std::max(max_event_batch_ms_,
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - batch_start).count());
            max_event_batch_interval_ms_ = std::max(max_event_batch_interval_ms_,
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - batch_start).count());
        }

        const TimePoint now = Clock::now();
        if (now > last_report) report(start, now, last_report);
        for (Client& c : clients_) close_client(c, "finished", true);
        uint64_t total_bytes = 0;
        for (const Client& c : clients_) total_bytes += c.bytes;
        std::cout << "final: attempted=" << attempted_ << " connected_total=" << connected_total_
                  << " connect_fail=" << connect_fail_ << " disconnect=" << disconnect_
                  << " received_MB=" << std::fixed << std::setprecision(2)
                  << total_bytes / 1048576.0 << '\n';
        return connected_total_ == 0 || total_bytes == 0 ? 2 : 0;
    }

private:
    static const char* state_name(SRT_SOCKSTATUS state) {
        switch (state) {
            case SRTS_INIT: return "INIT";
            case SRTS_OPENED: return "OPENED";
            case SRTS_LISTENING: return "LISTENING";
            case SRTS_CONNECTING: return "CONNECTING";
            case SRTS_CONNECTED: return "CONNECTED";
            case SRTS_BROKEN: return "BROKEN";
            case SRTS_CLOSING: return "CLOSING";
            case SRTS_CLOSED: return "CLOSED";
            case SRTS_NONEXIST: return "NONEXIST";
            default: return "UNKNOWN";
        }
    }

    void refresh_local_port(Client& c) {
        if (c.socket == SRT_INVALID_SOCK) return;
        sockaddr_in local{};
        int len = sizeof(local);
        if (srt_getsockname(c.socket, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            c.local_port = ntohs(local.sin_port);
    }

    void trace_event(Client& c, const char* event, const std::string& detail = {}) {
        refresh_local_port(c);
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &tt);
        auto csv_text = [](const std::string& value) {
            std::string quoted = "\"";
            for (char ch : value) quoted += ch == '"' ? "\"\"" : std::string(1, ch);
            return quoted + '"';
        };
        std::ostringstream line;
        line << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ','
             << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - test_start_).count() << ','
             << c.id << ',' << event << ',' << c.local_port << ',' << c.socket << ','
             << (c.socket == SRT_INVALID_SOCK ? "INVALID" : state_name(srt_getsockstate(c.socket))) << ','
             << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - c.started).count() << ','
             << c.launch_lag_ms << ',' << max_loop_gap_ms_ << ','
             << csv_text(c.stream_id) << ',' << csv_text(detail) << '\n';
        if (trace_) { trace_ << line.str(); trace_.flush(); }
        if (std::string(event) == "failed") std::cerr << line.str();
    }

    void update_events(Client& c, int flags) {
        if (c.socket != SRT_INVALID_SOCK &&
            srt_epoll_update_usock(epoll_id_, c.socket, &flags) == SRT_ERROR)
            close_client(c, "epoll update failed");
    }

    void capture_stats(Client& c) {
        if (c.socket == SRT_INVALID_SOCK || c.phase != Phase::Connected) return;
        SRT_TRACEBSTATS stats{};
        if (srt_bstats(c.socket, &stats, 0) == 0) {
            c.loss = stats.pktRcvLossTotal;
            c.drop = stats.pktRcvDropTotal;
            c.rtt_ms = stats.msRTT;
            c.rcvbuf_packets = stats.pktRcvBuf;
            c.avail_rcvbuf_bytes = stats.byteAvailRcvBuf;
        }
    }

    void close_client(Client& c, const std::string& reason, bool finished = false) {
        if (c.phase == Phase::Pending || c.phase == Phase::Closed) return;
        capture_stats(c);
        trace_event(c, finished ? "finished" : "failed", reason);
        if (!finished) {
            if (c.phase == Phase::Connecting) ++connect_fail_;
            if (c.phase == Phase::Connected) ++disconnect_;
        }
        c.end_reason = reason;
        c.phase = Phase::Closed;
        if (c.socket != SRT_INVALID_SOCK) {
            by_socket_.erase(c.socket);
            srt_epoll_remove_usock(epoll_id_, c.socket);
            srt_close(c.socket);
            c.socket = SRT_INVALID_SOCK;
        }
    }

    void start_client(Client& c, int id, TimePoint now) {
        c.id = id;
        c.phase = Phase::Connecting;
        c.started = now;
        c.slow = options_.slow_every > 0 && id % options_.slow_every == 0;
        if (!options_.stream_id.empty())
            c.stream_id = options_.stream_id + "~lt-" + std::to_string(GetCurrentProcessId()) +
                          "-" + std::to_string(id);
        c.socket = srt_create_socket();
        if (c.socket == SRT_INVALID_SOCK) { close_client(c, "socket creation failed"); return; }

        const int nonblocking = 0;
        const int latency = options_.latency_ms;
        const int timeout = options_.connect_timeout_ms;
        const int live = SRTT_LIVE;
        auto set = [&](SRT_SOCKOPT key, const void* value, int size) {
            return srt_setsockflag(c.socket, key, value, size) != SRT_ERROR;
        };
        if (!set(SRTO_TRANSTYPE, &live, sizeof(live)) ||
            !set(SRTO_RCVLATENCY, &latency, sizeof(latency)) ||
            !set(SRTO_PEERLATENCY, &latency, sizeof(latency)) ||
            !set(SRTO_CONNTIMEO, &timeout, sizeof(timeout)) ||
            !set(SRTO_RCVSYN, &nonblocking, sizeof(nonblocking)) ||
            (!c.stream_id.empty() &&
             !set(SRTO_STREAMID, c.stream_id.data(),
                  static_cast<int>(c.stream_id.size())))) {
            close_client(c, std::string("socket option: ") + srt_getlasterror_str());
            return;
        }
        int flags = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
        if (srt_epoll_add_usock(epoll_id_, c.socket, &flags) == SRT_ERROR) {
            close_client(c, std::string("epoll add: ") + srt_getlasterror_str());
            return;
        }
        by_socket_[c.socket] = static_cast<size_t>(id - 1);
        if (srt_connect(c.socket, reinterpret_cast<const sockaddr*>(&target_), sizeof(target_)) == SRT_ERROR)
            close_client(c, std::string("connect: ") + srt_getlasterror_str());
        else trace_event(c, "connecting", c.stream_id);
    }

    void connected(Client& c, TimePoint now) {
        c.phase = Phase::Connected;
        c.connected = now;
        c.last_data = now;
        c.connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.started).count();
        ++connected_total_;
        trace_event(c, "connected");
        update_events(c, SRT_EPOLL_IN | SRT_EPOLL_ERR);
    }

    void read_client(Client& c) {
        char buffer[4096];
        const int limit = c.slow ? 1 : 64;
        for (int i = 0; i < limit; ++i) {
            const TimePoint recv_start = Clock::now();
            const int n = srt_recv(c.socket, buffer, sizeof(buffer));
            max_recv_call_interval_ms_ = std::max(max_recv_call_interval_ms_,
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - recv_start).count());
            if (n > 0) {
                c.bytes += static_cast<uint64_t>(n);
                c.last_data = Clock::now();
            } else if (n == SRT_ERROR && srt_getlasterror(nullptr) == SRT_EASYNCRCV) {
                break;
            } else {
                close_client(c, n == 0 ? "end of stream" : std::string("receive: ") + srt_getlasterror_str());
                return;
            }
        }
        if (c.slow && c.phase == Phase::Connected) {
            c.slow_paused = true;
            c.next_slow_read = Clock::now() + std::chrono::milliseconds(options_.slow_read_ms);
            update_events(c, SRT_EPOLL_ERR);
        }
    }

    void handle_event(const SRT_EPOLL_EVENT& event) {
        auto it = by_socket_.find(event.fd);
        if (it == by_socket_.end()) return;
        Client& c = clients_[it->second];
        if (event.events & SRT_EPOLL_ERR) {
            close_client(c, std::string("SRT socket error: ") + srt_getlasterror_str());
            return;
        }
        if (c.phase == Phase::Connecting && (event.events & SRT_EPOLL_OUT)) {
            if (srt_getsockstate(c.socket) == SRTS_CONNECTED) connected(c, Clock::now());
            else close_client(c, "connection rejected");
        } else if (c.phase == Phase::Connected && (event.events & SRT_EPOLL_IN)) {
            read_client(c);
        }
    }

    void report(TimePoint start, TimePoint now, TimePoint previous) {
        const double seconds = std::chrono::duration<double>(now - previous).count();
        if (seconds <= 0) return;
        int active = 0;
        int idle = 0;
        int rtt_count = 0;
        int64_t loss = 0;
        int64_t drop = 0;
        int max_rcvbuf_packets = -1;
        int min_avail_rcvbuf_bytes = -1;
        uint64_t delta_bytes = 0;
        double rtt_sum = 0;
        std::vector<double> rates;
        std::vector<int64_t> connect_times;
        for (Client& c : clients_) {
            delta_bytes += c.bytes - c.last_report_bytes;
            const double rate = 8.0 * (c.bytes - c.last_report_bytes) / seconds / 1e6;
            c.last_report_bytes = c.bytes;
            if (c.phase == Phase::Connected) {
                ++active;
                rates.push_back(rate);
                if (now - c.last_data >= std::chrono::milliseconds(options_.idle_ms)) ++idle;
                capture_stats(c);
                rtt_sum += c.rtt_ms;
                ++rtt_count;
                if (c.rcvbuf_packets >= 0)
                    max_rcvbuf_packets = std::max(max_rcvbuf_packets, c.rcvbuf_packets);
                if (c.avail_rcvbuf_bytes >= 0 &&
                    (min_avail_rcvbuf_bytes < 0 || c.avail_rcvbuf_bytes < min_avail_rcvbuf_bytes))
                    min_avail_rcvbuf_bytes = c.avail_rcvbuf_bytes;
            }
            if (c.connect_ms >= 0) connect_times.push_back(c.connect_ms);
            loss += c.loss;
            drop += c.drop;
        }
        std::sort(rates.begin(), rates.end());
        std::sort(connect_times.begin(), connect_times.end());
        auto percentile_index = [](size_t size, size_t percent) {
            return (size * percent + 99) / 100 - 1;
        };
        const double minimum = rates.empty() ? 0 : rates.front();
        const double p50 = rates.empty() ? 0 : rates[percentile_index(rates.size(), 50)];
        const double p95 = rates.empty() ? 0 : rates[percentile_index(rates.size(), 95)];
        const int64_t p95_connect = connect_times.empty() ? 0 :
            connect_times[percentile_index(connect_times.size(), 95)];
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const double total_mbps = 8.0 * delta_bytes / seconds / 1e6;
        const double avg_rtt = rtt_count ? rtt_sum / rtt_count : 0;
        std::cout << std::fixed << std::setprecision(2)
                  << "t=" << elapsed << "s attempted=" << attempted_
                  << " connected=" << active << " connect_fail=" << connect_fail_
                  << " disconnect=" << disconnect_ << " idle=" << idle
                  << " rx=" << total_mbps << "Mbps min/p50/p95="
                  << minimum << '/' << p50 << '/' << p95
                  << "Mbps connect_p95=" << p95_connect << "ms loss=" << loss << " drop=" << drop
                  << " rtt=" << avg_rtt << "ms\n";
        std::cout << "  loop_gap_max=" << max_loop_gap_ms_
                  << "ms launch_lag_max=" << max_launch_lag_ms_
                  << "ms start_call_max=" << max_start_call_ms_
                  << "ms event_batch_max=" << max_event_batch_ms_
                  << "ms event_batch_interval_max=" << max_event_batch_interval_ms_
                  << "ms max_rcvbuf_packets=" << max_rcvbuf_packets
                  << " min_avail_rcvbuf_bytes=" << min_avail_rcvbuf_bytes
                  << " handle_event_interval_max=" << max_handle_event_interval_ms_
                  << "ms recv_call_interval_max=" << max_recv_call_interval_ms_ << "ms\n";
        if (csv_) {
            csv_ << std::fixed << std::setprecision(3)
                 << elapsed << ',' << attempted_ << ',' << active << ',' << connect_fail_ << ','
                 << disconnect_ << ',' << idle << ',' << total_mbps << ',' << minimum << ','
                 << p50 << ',' << p95 << ',' << p95_connect << ',' << loss << ',' << drop << ','
                 << avg_rtt << ',' << max_loop_gap_ms_ << ',' << max_launch_lag_ms_ << ','
                 << max_start_call_ms_ << ',' << max_event_batch_ms_ << ','
                 << max_event_batch_interval_ms_ << ',' << max_rcvbuf_packets << ','
                 << min_avail_rcvbuf_bytes << ',' << max_handle_event_interval_ms_ << ','
                 << max_recv_call_interval_ms_ << '\n';
            csv_.flush();
        }
        max_event_batch_interval_ms_ = 0;
        max_handle_event_interval_ms_ = 0;
        max_recv_call_interval_ms_ = 0;
    }

    const Options& options_;
    sockaddr_in target_{};
    int epoll_id_ = -1;
    std::vector<Client> clients_;
    std::vector<SRT_EPOLL_EVENT> events_;
    std::unordered_map<SRTSOCKET, size_t> by_socket_;
    std::ofstream csv_;
    std::ofstream trace_;
    TimePoint test_start_{};
    TimePoint last_loop_{};
    int64_t max_loop_gap_ms_ = 0;
    int64_t max_launch_lag_ms_ = 0;
    int64_t max_start_call_ms_ = 0;
    int64_t max_event_batch_ms_ = 0;
    int64_t max_event_batch_interval_ms_ = 0;
    int64_t max_handle_event_interval_ms_ = 0;
    int64_t max_recv_call_interval_ms_ = 0;
    int attempted_ = 0;
    int connected_total_ = 0;
    int connect_fail_ = 0;
    int disconnect_ = 0;
};

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) { print_usage(); return 0; }
        std::signal(SIGINT, on_interrupt);
        Runtime runtime;
        const sockaddr_in target = resolve_target(options);
        LoadTest test(options, target);
        return test.run();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
