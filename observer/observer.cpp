#include <srt/srt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

extern "C" {
#include "telemetry.h"
}

namespace {

std::atomic<bool> g_running{true};

void on_signal(int) {
    g_running.store(false);
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#ifdef _WIN32
void close_fd(SOCKET fd) {
    if (fd != INVALID_SOCKET) closesocket(fd);
}
#else
void close_fd(int fd) {
    if (fd >= 0) close(fd);
}
#endif

struct Config {
    std::string url;
    std::string ack_host;
    int ack_port = TLMY_ACK_PORT;
    int latency_ms = 40;
    int recv_timeout_ms = 2000;
    int reconnect_ms = 500;
    std::string streamid;
    std::optional<std::string> passphrase;
    int pbkeylen = 16;
};

struct ParsedUrl {
    std::string host;
    int port = 0;
    int latency_ms = 40;
    std::string streamid;
    std::optional<std::string> passphrase;
    int pbkeylen = 16;
};

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out.push_back((char)strtol(hex, nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

bool parse_srt_url(const std::string& url, ParsedUrl* out) {
    const std::string prefix = "srt://";
    if (!out || url.rfind(prefix, 0) != 0) return false;

    size_t host_begin = prefix.size();
    size_t qpos = url.find('?', host_begin);
    std::string host_port = (qpos == std::string::npos)
        ? url.substr(host_begin)
        : url.substr(host_begin, qpos - host_begin);

    size_t colon = host_port.rfind(':');
    if (colon == std::string::npos) return false;

    out->host = host_port.substr(0, colon);
    out->port = std::stoi(host_port.substr(colon + 1));

    if (qpos == std::string::npos) return true;

    std::string query = url.substr(qpos + 1);
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
        std::string val = (eq == std::string::npos) ? "" : url_decode(kv.substr(eq + 1));

        if (key == "latency") out->latency_ms = std::stoi(val);
        else if (key == "streamid") out->streamid = val;
        else if (key == "passphrase") out->passphrase = val;
        else if (key == "pbkeylen") out->pbkeylen = std::stoi(val);

        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return true;
}

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };

        if (a == "--url") cfg.url = val();
        else if (a == "--ack-host") cfg.ack_host = val();
        else if (a == "--ack-port") cfg.ack_port = std::stoi(val());
        else if (a == "--latency-ms") cfg.latency_ms = std::stoi(val());
        else if (a == "--recv-timeout-ms") cfg.recv_timeout_ms = std::stoi(val());
        else if (a == "--reconnect-ms") cfg.reconnect_ms = std::stoi(val());
        else if (a == "--streamid") cfg.streamid = val();
        else if (a == "--passphrase") cfg.passphrase = val();
        else if (a == "--pbkeylen") cfg.pbkeylen = std::stoi(val());
        else if (a == "--help") {
            std::cout
                << "Usage: telemetry_observer --url <srt://host:port?...> [options]\n"
                << "  --ack-host         <ip>   default: same as url host\n"
                << "  --ack-port         19902\n"
                << "  --latency-ms       40     override url latency\n"
                << "  --recv-timeout-ms  2000\n"
                << "  --reconnect-ms     500\n"
                << "  --streamid         <str>  override url streamid\n"
                << "  --passphrase       <str>\n"
                << "  --pbkeylen         16|24|32\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown arg: " + a);
        }
    }

    if (cfg.url.empty()) {
        throw std::runtime_error("--url is required");
    }
    return cfg;
}

class AckSender {
public:
    AckSender() = default;
    ~AckSender() { stop(); }

    bool start(const std::string& host, int port) {
        stop();

#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::cerr << "[OBS] WSAStartup failed\n";
            return false;
        }
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) {
            WSACleanup();
            return false;
        }
#else
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            return false;
        }
#endif

        memset(&dst_, 0, sizeof(dst_));
        dst_.sin_family = AF_INET;
        dst_.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host.c_str(), &dst_.sin_addr) != 1) {
            std::cerr << "[OBS] bad ack host: " << host << "\n";
            stop();
            return false;
        }

        return true;
    }

    void stop() {
#ifdef _WIN32
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
            WSACleanup();
        }
#else
        if (sock_ >= 0) {
            close(sock_);
            sock_ = -1;
        }
#endif
    }

    void send_ack(uint64_t seq) {
#ifdef _WIN32
        if (sock_ == INVALID_SOCKET) return;
#else
        if (sock_ < 0) return;
#endif
        TelemetryAck ack{};
        tlmy_ack_encode(&ack, seq);
        sendto(sock_, (const char*)&ack, sizeof(ack), 0,
               (sockaddr*)&dst_, sizeof(dst_));
    }

private:
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif
    sockaddr_in dst_{};
};

class TsObserver {
public:
    explicit TsObserver(AckSender& sender) : sender_(sender) {}

    void process_chunk(const uint8_t* data, size_t len) {
        if (!data || len == 0) return;

        if (!tail_.empty()) {
            std::vector<uint8_t> merged;
            merged.reserve(tail_.size() + len);
            merged.insert(merged.end(), tail_.begin(), tail_.end());
            merged.insert(merged.end(), data, data + len);
            tail_.clear();
            process_buffer(merged.data(), merged.size());
            return;
        }

        process_buffer(data, len);
    }

private:
    void process_buffer(const uint8_t* data, size_t len) {
        if (len < 188) {
            tail_.assign(data, data + len);
            return;
        }

        if ((len % 188) == 0) {
            for (size_t off = 0; off + 188 <= len; off += 188) {
                parse_one(data + off);
            }
            return;
        }

        size_t off = 0;
        while (off + 188 <= len) {
            if (data[off] == 0x47u) {
                uint64_t t_push_us = 0;
                uint64_t seq = 0;
                if (tlmy_ts_parse(data + off, &t_push_us, &seq)) {
                    ack_once(seq);
                    off += 188;
                    continue;
                }
            }
            ++off;
        }

        size_t remain = len - off;
        if (remain > 0) {
            tail_.assign(data + off, data + len);
            if (tail_.size() > 187) {
                tail_.erase(tail_.begin(), tail_.end() - 187);
            }
        }
    }

    void parse_one(const uint8_t* pkt) {
        uint64_t t_push_us = 0;
        uint64_t seq = 0;
        if (tlmy_ts_parse(pkt, &t_push_us, &seq)) {
            ack_once(seq);
        }
    }

    void ack_once(uint64_t seq) {
        if (recent_.find(seq) != recent_.end()) return;
        recent_.insert(seq);
        order_.push_back(seq);
        while (order_.size() > 8192) {
            recent_.erase(order_.front());
            order_.pop_front();
        }
        sender_.send_ack(seq);
        std::cout << "[OBS] ack seq=" << seq << "\n";
    }

private:
    AckSender& sender_;
    std::vector<uint8_t> tail_;
    std::unordered_set<uint64_t> recent_;
    std::deque<uint64_t> order_;
};

class SrtObserverClient {
public:
    explicit SrtObserverClient(Config cfg)
        : cfg_(std::move(cfg)) {}

    int run() {
        ParsedUrl pu{};
        if (!parse_srt_url(cfg_.url, &pu)) {
            std::cerr << "[OBS] invalid srt url: " << cfg_.url << "\n";
            return 1;
        }

        if (cfg_.ack_host.empty()) cfg_.ack_host = pu.host;
        if (!cfg_.streamid.empty()) pu.streamid = cfg_.streamid;
        if (cfg_.passphrase.has_value()) pu.passphrase = cfg_.passphrase;
        if (cfg_.latency_ms > 0) pu.latency_ms = cfg_.latency_ms;
        pu.pbkeylen = cfg_.pbkeylen;

        if (!ack_sender_.start(cfg_.ack_host, cfg_.ack_port)) {
            std::cerr << "[OBS] failed to init ack sender\n";
            return 1;
        }

        if (srt_startup() != 0) {
            std::cerr << "[OBS] srt_startup failed\n";
            return 1;
        }

        TsObserver observer(ack_sender_);

        while (g_running.load()) {
            SRTSOCKET sock = connect_once(pu);
            if (sock == SRT_INVALID_SOCK) {
                sleep_ms(cfg_.reconnect_ms);
                continue;
            }

            std::cout << "[OBS] connected to " << pu.host << ":" << pu.port << "\n";
            std::vector<uint8_t> buf(8192);
            while (g_running.load()) {
                int n = srt_recv(sock, (char*)buf.data(), (int)buf.size());
                if (n == SRT_ERROR) {
                    int err = srt_getlasterror(nullptr);
                    if (err == SRT_ETIMEOUT) continue;
                    std::cerr << "[OBS] recv failed: " << srt_getlasterror_str() << "\n";
                    break;
                }
                if (n <= 0) continue;
                observer.process_chunk(buf.data(), (size_t)n);
            }

            srt_close(sock);
            if (g_running.load()) {
                std::cout << "[OBS] reconnecting...\n";
                sleep_ms(cfg_.reconnect_ms);
            }
        }

        srt_cleanup();
        return 0;
    }

private:
    SRTSOCKET connect_once(const ParsedUrl& pu) {
        SRTSOCKET s = srt_socket(AF_INET, SOCK_DGRAM, 0);
        if (s == SRT_INVALID_SOCK) {
            std::cerr << "[OBS] srt_socket failed\n";
            return SRT_INVALID_SOCK;
        }

        auto sf = [&](SRT_SOCKOPT opt, const void* val, int len, const char* name) -> bool {
            if (srt_setsockflag(s, opt, val, len) != 0) {
                std::cerr << "[OBS] set " << name << " failed: " << srt_getlasterror_str() << "\n";
                return false;
            }
            return true;
        };

        int yes = 1;
        int live = SRTT_LIVE;
        int latency = pu.latency_ms;
        int rcv_timeout = cfg_.recv_timeout_ms;
        if (!sf(SRTO_RCVSYN, &yes, sizeof(yes), "RCVSYN")) goto fail;
        if (!sf(SRTO_SNDSYN, &yes, sizeof(yes), "SNDSYN")) goto fail;
        if (!sf(SRTO_TSBPDMODE, &yes, sizeof(yes), "TSBPDMODE")) goto fail;
        if (!sf(SRTO_TRANSTYPE, &live, sizeof(live), "TRANSTYPE")) goto fail;
        if (!sf(SRTO_LATENCY, &latency, sizeof(latency), "LATENCY")) goto fail;
        if (!sf(SRTO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout), "RCVTIMEO")) goto fail;
        if (!pu.streamid.empty()) {
            if (!sf(SRTO_STREAMID, pu.streamid.c_str(), (int)pu.streamid.size(), "STREAMID")) goto fail;
        }
        if (pu.passphrase.has_value() && !pu.passphrase->empty()) {
            int pk = pu.pbkeylen;
            if (!sf(SRTO_PASSPHRASE, pu.passphrase->c_str(), (int)pu.passphrase->size(), "PASSPHRASE")) goto fail;
            if (!sf(SRTO_PBKEYLEN, &pk, sizeof(pk), "PBKEYLEN")) goto fail;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)pu.port);
        if (inet_pton(AF_INET, pu.host.c_str(), &addr.sin_addr) != 1) {
            std::cerr << "[OBS] bad host: " << pu.host << "\n";
            goto fail;
        }

        if (srt_connect(s, (sockaddr*)&addr, sizeof(addr)) == SRT_ERROR) {
            std::cerr << "[OBS] connect failed: " << srt_getlasterror_str() << "\n";
            goto fail;
        }
        return s;

    fail:
        srt_close(s);
        return SRT_INVALID_SOCK;
    }

private:
    Config cfg_;
    AckSender ack_sender_;
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    std::signal(SIGINT, on_signal);
#else
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#endif

    try {
        Config cfg = parse_args(argc, argv);
        SrtObserverClient client(std::move(cfg));
        return client.run();
    } catch (const std::exception& ex) {
        std::cerr << "[OBS] " << ex.what() << "\n";
        return 1;
    }
}
