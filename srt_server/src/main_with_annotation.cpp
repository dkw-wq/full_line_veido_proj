#include <srt/srt.h>          // SRT 库头文件，提供 SRT socket、收发、选项设置等 API

#include <arpa/inet.h>        // inet_ntop / inet_pton 等 IP 地址转换函数
#include <netinet/in.h>       // sockaddr_in、htons 等网络地址结构
#include <sys/socket.h>       // socket 相关基础定义
#include <unistd.h>           // close 等 Unix 接口（这里基本没直接用到）

#include <atomic>             // 原子变量，跨线程安全读写
#include <chrono>             // 时间相关类型和函数
#include <condition_variable> // 条件变量，用于线程间等待/通知
#include <csignal>            // signal / std::signal，处理 SIGINT、SIGTERM
#include <cstdint>            // uint8_t / uint16_t / uint64_t 等固定宽度整数
#include <cstring>            // C 风格字符串操作（这里基本没直接用到）
#include <deque>              // 双端队列，订阅者发送队列
#include <iomanip>            // put_time / setprecision 等格式化输出
#include <iostream>           // std::cout / std::cerr
#include <map>                // 有序 map，用于保存订阅者
#include <memory>             // shared_ptr / make_shared
#include <mutex>              // mutex / lock_guard / unique_lock
#include <optional>           // std::optional，可选值
#include <sstream>            // 字符串流，用于拼日志
#include <string>             // std::string
#include <thread>             // std::thread
#include <unordered_map>      // 哈希表，用于 PID -> continuity counter
#include <vector>             // 动态数组

namespace {                   // 匿名命名空间：只在当前编译单元可见，避免符号外泄

using Clock = std::chrono::steady_clock;      // 单调时钟：适合计算时间间隔，不受系统时间调整影响
using SysClock = std::chrono::system_clock;   // 系统墙上时钟：适合打印当前日期时间
using Ms = std::chrono::milliseconds;         // 毫秒类型别名
using namespace std::chrono_literals;         // 允许写 500ms、2s 这种字面量

std::atomic<bool> g_running{true};            // 全局运行标志，所有线程通过它判断是否退出

std::string now_wall_time() {                 // 获取当前本地时间，格式化为字符串
    auto now = SysClock::now();               // 取当前系统时间
    std::time_t t = SysClock::to_time_t(now); // 转成 time_t
    std::tm tm{};                             // 本地时间结构体
    localtime_r(&t, &tm);                     // 线程安全地转成本地时间
    std::ostringstream oss;                   // 用字符串流拼接输出
    oss << std::put_time(&tm, "%F %T");       // 格式：YYYY-MM-DD HH:MM:SS
    return oss.str();                         // 返回格式化后的时间字符串
}

void log_line(const std::string& level, const std::string& msg) { // 统一日志输出函数
    std::cerr << "[" << now_wall_time() << "] "                  // 打印当前时间
              << "[" << level << "] "                            // 打印日志级别
              << msg                                             // 打印消息正文
              << std::endl;                                      // 换行并刷新
}

std::string last_srt_error() {                // 获取最近一次 SRT 错误字符串
    const char* err = srt_getlasterror_str(); // SRT 库返回最近错误信息
    return err ? std::string(err)             // 如果非空，转换成 std::string
               : std::string("unknown SRT error"); // 否则返回默认文案
}

std::string sockaddr_to_string(const sockaddr_in& addr) { // 把 IPv4 地址结构转成 ip:port 字符串
    char ip[INET_ADDRSTRLEN] = {0};                       // 存放文本 IP 的缓冲区
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));  // 二进制 IP -> 字符串 IP
    std::ostringstream oss;                              // 字符串流
    oss << ip << ":" << ntohs(addr.sin_port);            // 拼接 "IP:PORT"
    return oss.str();                                    // 返回结果
}

struct Config {                              // 服务配置项
    std::string bind_ip = "0.0.0.0";         // 监听绑定地址，默认所有网卡
    int publisher_port = 9000;               // 发布者接入端口
    int subscriber_port = 9001;              // 订阅者接入端口
    int payload_size = 1316;                 // 每次收发负载大小，1316 = 7 * 188，常见 TS 分片大小
    int latency_ms = 120;                    // SRT latency 配置，单位毫秒
    int recv_buf_bytes = 4 * 1024 * 1024;    // SRT 接收缓冲区大小
    int send_buf_bytes = 4 * 1024 * 1024;    // SRT 发送缓冲区大小
    int subscriber_queue_max_chunks = 512;   // 每个订阅者最大排队 chunk 数
    bool validate_ts = true;                 // 是否做 TS 校验
    bool require_h264 = false;               // 如果为 true，则要求 PMT 里出现 H.264 (stream_type 0x1B)
    int monitor_interval_ms = 2000;          // 监控日志输出间隔
};

struct StreamStatus {                                        // 跨线程共享的运行状态/统计
    std::atomic<bool> publisher_online{false};               // 发布者是否在线
    std::atomic<uint64_t> total_ingress_bytes{0};            // 总入流字节数
    std::atomic<uint64_t> total_egress_bytes{0};             // 总出流字节数
    std::atomic<uint64_t> ingress_bytes_window{0};           // 本监控窗口内入流字节数
    std::atomic<uint64_t> egress_bytes_window{0};            // 本监控窗口内出流字节数
    std::atomic<uint64_t> ingress_chunks{0};                 // 收到的 chunk 总数
    std::atomic<uint64_t> ts_discontinuities{0};             // TS continuity counter 不连续次数
    std::atomic<uint64_t> subscriber_queue_drops{0};         // 订阅者队列满导致丢弃 chunk 次数
    std::atomic<int> subscriber_count{0};                    // 当前订阅者数量
    std::atomic<int64_t> publisher_connected_at_ms{0};       // 发布者连接建立时间（steady_clock 毫秒）
    std::atomic<int64_t> publisher_last_active_at_ms{0};     // 发布者最近活跃时间（steady_clock 毫秒）
};

int64_t steady_ms() { // 返回 steady_clock 当前时间点相对 epoch 的毫秒数
    return std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count();
}

class TsValidator { // MPEG-TS 基础校验器
public:
    explicit TsValidator(bool require_h264) : require_h264_(require_h264) {} // 构造时决定是否强制要求 H.264

    bool inspect(const uint8_t* data, size_t len, StreamStatus& stats) { // 检查一段数据是否是合法 TS chunk
        if (len == 0) return true; // 空数据直接认为合法
        if (len % 188 != 0) {      // TS 包必须按 188 字节对齐
            last_error_ = "payload size is not aligned to 188-byte TS packets";
            return false;
        }

        for (size_t off = 0; off < len; off += 188) { // 每 188 字节遍历一个 TS 包
            const uint8_t* p = data + off;            // 当前 TS 包起始地址
            if (p[0] != 0x47) {                       // TS 包第一个字节必须是同步字节 0x47
                last_error_ = "TS sync byte mismatch";
                return false;
            }

            bool payload_unit_start = (p[1] & 0x40) != 0; // PUSI：payload unit start indicator
            uint16_t pid = static_cast<uint16_t>(((p[1] & 0x1F) << 8) | p[2]); // 解析 13-bit PID
            uint8_t adaptation_field_control = (p[3] >> 4) & 0x03;             // 解析 adaptation_field_control
            uint8_t continuity_counter = p[3] & 0x0F;                          // 解析 continuity counter

            bool has_payload = (adaptation_field_control == 1 || adaptation_field_control == 3);   // 是否有 payload
            bool has_adaptation = (adaptation_field_control == 2 || adaptation_field_control == 3); // 是否有 adaptation field

            if (has_payload && pid != 0x1FFF) { // 只有有 payload 且不是空包 PID 才检查 continuity counter
                auto it = cc_map_.find(pid);    // 查该 PID 上一次看到的 continuity counter
                if (it != cc_map_.end()) {      // 如果这个 PID 不是第一次出现
                    uint8_t expected = static_cast<uint8_t>((it->second + 1) & 0x0F); // 期望值：上次 +1 后按 16 回绕
                    if (continuity_counter != expected && !discontinuity_flag(p, has_adaptation)) {
                        // 如果 continuity counter 不等于期望值，且 adaptation field 里也没标 discontinuity
                        stats.ts_discontinuities.fetch_add(1, std::memory_order_relaxed); // 记录一次不连续
                    }
                }
                cc_map_[pid] = continuity_counter; // 更新该 PID 最新 continuity counter
            }

            if (!has_payload) continue; // 没 payload 的话，后面 PAT/PMT 解析就没法做，直接下一个包

            size_t payload_offset = 4; // TS 基本头固定 4 字节，payload 默认从第 5 字节开始
            if (has_adaptation) {      // 如果有 adaptation field
                uint8_t afl = p[4];    // adaptation_field_length
                payload_offset += 1 + afl; // 跳过 length 字段本身 + adaptation field 内容
                if (payload_offset > 188) continue; // 防越界：异常包直接忽略
            }
            if (payload_offset >= 188) continue; // 没有有效 payload 了，跳过

            const uint8_t* payload = p + payload_offset; // payload 起始地址
            size_t payload_len = 188 - payload_offset;   // payload 长度

            if (pid == 0x0000) { // PAT 固定在 PID 0
                if (payload_unit_start) parse_pat(payload, payload_len); // 只有 section 起始处才解析 PAT
            } else if (pmt_pid_ && pid == *pmt_pid_) { // 如果已经知道 PMT PID，且当前 PID 正好是它
                if (payload_unit_start) parse_pmt(payload, payload_len); // 解析 PMT
            }
        }

        if (require_h264_ && saw_pmt_ && !saw_h264_) { // 如果要求 H.264，且 PMT 已经看到了，但里面没发现 H.264
            last_error_ = "PMT parsed but no H.264 stream_type(0x1B) found";
            return false;
        }
        return true; // 整个 chunk 检查通过
    }

    const std::string& last_error() const { return last_error_; } // 返回最近一次校验错误字符串
    bool saw_h264() const { return saw_h264_; }                   // 是否看到了 H.264 ES
    bool saw_pmt() const { return saw_pmt_; }                     // 是否看到了 PMT

private:
    static bool discontinuity_flag(const uint8_t* p, bool has_adaptation) { // 检查 adaptation field 中的 discontinuity_indicator
        if (!has_adaptation) return false;           // 没 adaptation field，肯定没这个标志
        uint8_t afl = p[4];                          // adaptation field 长度
        if (afl == 0 || 5 + afl > 188) return false; // 长度非法或没空间放 flags，则认为无该标志
        return (p[5] & 0x80) != 0;                   // p[5] 是 adaptation flags，最高位是 discontinuity_indicator
    }

    void parse_pat(const uint8_t* payload, size_t len) { // 解析 Program Association Table
        if (len < 1) return;                             // 至少要有 pointer_field
        uint8_t pointer_field = payload[0];              // PSI/SI section 前面的指针字段
        if (1 + pointer_field + 8 > len) return;         // 至少要能放下 section 开头基本字段
        const uint8_t* sec = payload + 1 + pointer_field; // sec 指向真正的 PAT section 起始
        if (sec[0] != 0x00) return;                      // PAT table_id 必须为 0x00
        uint16_t section_length = static_cast<uint16_t>(((sec[1] & 0x0F) << 8) | sec[2]); // 解析 section_length
        if (3 + section_length > len - 1 - pointer_field) return; // section 不能超出 payload 范围
        if (section_length < 9) return;                  // 最小合法 PAT section_length 检查

        size_t program_info_len = section_length - 9;    // 除去固定头 + CRC 后，剩余 program loop 长度
        const uint8_t* prog = sec + 8;                   // program loop 从 sec+8 开始
        for (size_t i = 0; i + 4 <= program_info_len; i += 4) { // 每个 program entry 固定 4 字节
            uint16_t program_number = static_cast<uint16_t>((prog[i] << 8) | prog[i + 1]); // program_number
            uint16_t pid = static_cast<uint16_t>(((prog[i + 2] & 0x1F) << 8) | prog[i + 3]); // network_pid 或 PMT PID
            if (program_number != 0) { // program_number==0 表示 network PID，不是 PMT
                pmt_pid_ = pid;        // 保存该 program 对应 PMT PID
                return;                // 找到一个即可返回
            }
        }
    }

    void parse_pmt(const uint8_t* payload, size_t len) { // 解析 Program Map Table
        if (len < 1) return;                             // 至少得有 pointer_field
        uint8_t pointer_field = payload[0];              // pointer_field
        if (1 + pointer_field + 12 > len) return;        // PMT 基本头比 PAT 更长，先做下界检查
        const uint8_t* sec = payload + 1 + pointer_field; // 指向 PMT section
        if (sec[0] != 0x02) return;                      // PMT 的 table_id 必须是 0x02
        uint16_t section_length = static_cast<uint16_t>(((sec[1] & 0x0F) << 8) | sec[2]); // 解析 section_length
        if (section_length < 13) return;                 // PMT 最小 section_length 检查
        if (3 + section_length > len - 1 - pointer_field) return; // section 不能越界
        saw_pmt_ = true;                                 // 记录：已经看到过 PMT

        uint16_t program_info_length = static_cast<uint16_t>(((sec[10] & 0x0F) << 8) | sec[11]); // program descriptors 长度
        size_t pos = 12 + program_info_length;           // elementary stream loop 的开始位置
        size_t end = 3 + section_length - 4;             // 减去末尾 4 字节 CRC，得到 ES loop 结束边界
        while (pos + 5 <= end) {                         // 每个 ES 至少 5 字节固定头
            uint8_t stream_type = sec[pos];              // stream_type，例如 0x1B 表示 H.264
            uint16_t es_info_length = static_cast<uint16_t>(((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4]); // ES descriptors 长度
            if (stream_type == 0x1B) saw_h264_ = true;   // 看到 H.264
            pos += 5 + es_info_length;                   // 跳到下一个 ES
        }
    }

private:
    bool require_h264_ = false;                     // 是否强制要求 PMT 中出现 H.264
    std::optional<uint16_t> pmt_pid_;               // 从 PAT 中解析出来的 PMT PID，未解析到前为空
    bool saw_pmt_ = false;                          // 是否已看到 PMT
    bool saw_h264_ = false;                         // 是否已在 PMT 中发现 H.264
    std::unordered_map<uint16_t, uint8_t> cc_map_; // 每个 PID 最近一次 continuity counter
    std::string last_error_;                        // 最近一次校验失败信息
};

class SubscriberSession { // 表示一个订阅者连接，负责自己的发送线程和待发送队列
public:
    SubscriberSession(SRTSOCKET sock,           // 该订阅者的 SRT socket
                      sockaddr_in peer_addr,    // 订阅者对端地址
                      StreamStatus& status,     // 共享状态对象
                      const Config& cfg,        // 全局配置
                      uint64_t id)              // 订阅者唯一编号
        : sock_(sock), peer_addr_(peer_addr), status_(status), cfg_(cfg), id_(id) {}

    ~SubscriberSession() { // 析构时确保停掉线程和 socket
        stop();
    }

    void start() { // 启动订阅者发送线程
        worker_ = std::thread([this]() { this->run(); }); // 子线程执行 run()
    }

    void stop() { // 停止该订阅者会话
        bool expected = true; // compare_exchange 期望当前 running_ 为 true
        if (running_.compare_exchange_strong(expected, false)) { // 只有第一次 stop 才真正执行清理
            {
                std::lock_guard<std::mutex> lk(mu_); // 加锁保护队列状态
                closed_ = true;                      // 标记已关闭，禁止再入队
            }
            cv_.notify_all();                        // 唤醒可能正在等待队列的发送线程
            srt_close(sock_);                        // 关闭 SRT socket，促使阻塞发送/接收退出
            if (worker_.joinable()) worker_.join();  // 等待线程结束
        }
    }

    bool push_chunk(const uint8_t* data, size_t len) { // 往该订阅者的发送队列塞一个 chunk
        std::lock_guard<std::mutex> lk(mu_);           // 加锁保护队列
        if (closed_) return false;                     // 已关闭则不能再推送

        if (queue_.size() >= static_cast<size_t>(cfg_.subscriber_queue_max_chunks)) {
            // 如果队列满了
            queue_.pop_front(); // 丢掉最老的 chunk，腾位置给最新数据
            status_.subscriber_queue_drops.fetch_add(1, std::memory_order_relaxed); // 记一次队列丢弃
        }

        queue_.emplace_back(data, data + len, steady_ms()); // 拷贝数据到队列尾部，记录入队时间
        cv_.notify_one();                                   // 唤醒发送线程去发
        return true;                                        // 入队成功
    }

    bool running() const { return running_.load(std::memory_order_relaxed); } // 会话是否仍在运行
    uint64_t id() const { return id_; }                                       // 订阅者编号
    std::string peer() const { return sockaddr_to_string(peer_addr_); }       // 对端地址字符串
    uint64_t dropped_chunks() const { return dropped_chunks_.load(std::memory_order_relaxed); } // 已丢 chunk 计数（注意这里实际上没更新）
    uint64_t sent_bytes() const { return sent_bytes_.load(std::memory_order_relaxed); }          // 已发送字节数
    int64_t last_send_active_ms() const { return last_send_active_ms_.load(std::memory_order_relaxed); } // 最近成功发送时间
    int configured_latency_ms() const { return configured_latency_ms_.load(std::memory_order_relaxed); } // 该订阅者读取到的 SRT latency 配置
    int queue_depth() const { // 当前队列深度
        std::lock_guard<std::mutex> lk(mu_); // 加锁读队列
        return static_cast<int>(queue_.size());
    }

private:
    struct Chunk { // 待发送数据块
        std::vector<uint8_t> data;   // chunk 数据内容
        int64_t enqueued_at_ms = 0;  // 入队时间（目前主要用于潜在调试/扩展）
        Chunk(const uint8_t* d, const uint8_t* e, int64_t ts) // 构造函数，按范围拷贝数据
            : data(d, e), enqueued_at_ms(ts) {}
    };

    void run() { // 订阅者发送线程主循环
        status_.subscriber_count.fetch_add(1, std::memory_order_relaxed); // 在线订阅者计数 +1
        last_send_active_ms_.store(steady_ms(), std::memory_order_relaxed); // 初始化最近发送时间
        configured_latency_ms_.store(read_latency_ms(sock_), std::memory_order_relaxed); // 读取 socket latency
        log_line("INFO", "subscriber#" + std::to_string(id_) + " started: " + peer()); // 打日志：订阅者启动

        while (running_.load(std::memory_order_relaxed) && g_running.load(std::memory_order_relaxed)) {
            Chunk chunk(nullptr, nullptr, 0); // 先构造一个空 chunk，后面从队列里取实际数据
            {
                std::unique_lock<std::mutex> lk(mu_); // unique_lock 便于和条件变量配合
                cv_.wait_for(lk, 500ms, [&]() {       // 最多等 500ms，或者直到条件满足
                    return !queue_.empty() ||         // 队列有数据
                           closed_ ||                 // 会话关闭
                           !running_.load(std::memory_order_relaxed) || // 运行标志关闭
                           !g_running.load(std::memory_order_relaxed);   // 全局退出
                });

                if (!running_.load(std::memory_order_relaxed) ||
                    !g_running.load(std::memory_order_relaxed) ||
                    closed_) {
                    break; // 如果已经不该运行了，退出线程
                }
                if (queue_.empty()) continue; // 超时醒来但队列仍空，则继续下一轮等待
                chunk = std::move(queue_.front()); // 取出最老的一个待发送 chunk
                queue_.pop_front();                // 从队列移除
            }

            int sent = srt_send(sock_, reinterpret_cast<const char*>(chunk.data.data()),
                                static_cast<int>(chunk.data.size())); // 把 chunk 发给该订阅者
            if (sent == SRT_ERROR) { // 发送失败
                log_line("WARN", "subscriber#" + std::to_string(id_) + " send failed: " + last_srt_error());
                break; // 退出发送线程
            }

            sent_bytes_.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed); // 会话发送字节累加
            status_.total_egress_bytes.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed); // 全局总出流累加
            status_.egress_bytes_window.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed); // 当前监控窗口出流累加
            last_send_active_ms_.store(steady_ms(), std::memory_order_relaxed); // 更新最近发送时间
        }

        status_.subscriber_count.fetch_sub(1, std::memory_order_relaxed); // 在线订阅者计数 -1
        running_.store(false, std::memory_order_relaxed);                 // 标记本会话已结束
        log_line("INFO", "subscriber#" + std::to_string(id_) + " stopped: " + peer()); // 打日志
    }

    static int read_latency_ms(SRTSOCKET sock) { // 读取 socket 配置上的 latency
        int val = 0;                             // 临时变量
        int optlen = sizeof(val);                // 选项长度
        if (srt_getsockflag(sock, SRTO_RCVLATENCY, &val, &optlen) == 0) return val; // 优先读接收 latency
        optlen = sizeof(val);                    // 重置长度
        if (srt_getsockflag(sock, SRTO_LATENCY, &val, &optlen) == 0) return val;    // 退而读通用 latency
        return -1;                               // 读取失败返回 -1
    }

private:
    SRTSOCKET sock_;                // 该订阅者的 socket
    sockaddr_in peer_addr_{};       // 对端地址
    StreamStatus& status_;          // 共享状态
    const Config& cfg_;             // 全局配置引用
    uint64_t id_ = 0;               // 订阅者编号

    mutable std::mutex mu_;         // 保护 queue_ / closed_
    std::condition_variable cv_;    // 用于通知发送线程队列有新数据
    std::deque<Chunk> queue_;       // 待发送队列
    bool closed_ = false;           // 是否关闭
    std::atomic<bool> running_{true}; // 会话是否仍在运行
    std::thread worker_;            // 发送线程

    std::atomic<uint64_t> dropped_chunks_{0};    // 会话级丢包计数（当前代码未实际更新它）
    std::atomic<uint64_t> sent_bytes_{0};        // 会话级已发字节数
    std::atomic<int64_t> last_send_active_ms_{0}; // 最近发送成功时间
    std::atomic<int> configured_latency_ms_{-1};  // 从 socket 读到的 latency 值
};

class RelayServer { // SRT 中继服务器：管理 publisher、subscriber、广播和监控
public:
    explicit RelayServer(Config cfg)              // 构造函数
        : cfg_(std::move(cfg)),                   // 保存配置
          validator_(cfg_.require_h264) {}        // 创建 TS 校验器

    ~RelayServer() { // 析构时停止服务
        stop();
    }

    bool start() { // 启动服务器
        if (srt_startup() != 0) { // 初始化 SRT 库
            log_line("ERROR", "srt_startup failed: " + last_srt_error());
            return false;
        }

        publisher_listener_ = create_listener(cfg_.bind_ip, cfg_.publisher_port, true); // 创建发布者监听 socket
        if (publisher_listener_ == SRT_INVALID_SOCK) return false;                      // 创建失败则启动失败

        subscriber_listener_ = create_listener(cfg_.bind_ip, cfg_.subscriber_port, false); // 创建订阅者监听 socket
        if (subscriber_listener_ == SRT_INVALID_SOCK) return false;                        // 创建失败则启动失败

        accept_publisher_thread_ = std::thread([this]() { accept_publisher_loop(); });   // 启动发布者 accept 线程
        accept_subscriber_thread_ = std::thread([this]() { accept_subscriber_loop(); }); // 启动订阅者 accept 线程
        monitor_thread_ = std::thread([this]() { monitor_loop(); });                     // 启动监控日志线程

        log_line("INFO", "relay server started, publisher port=" + std::to_string(cfg_.publisher_port) +
                         ", subscriber port=" + std::to_string(cfg_.subscriber_port));   // 打印启动成功日志
        return true; // 启动成功
    }

    void stop() { // 停止服务器
        bool expected = true; // 期望当前 stopped_ 为 true（注意这个命名有点反直觉，后面单独解释）
        if (!stopped_.compare_exchange_strong(expected, false)) {
            return; // 如果已经 stop 过了，就直接返回
        }

        g_running.store(false, std::memory_order_relaxed); // 通知所有线程退出

        if (publisher_listener_ != SRT_INVALID_SOCK) { // 关闭发布者监听 socket
            srt_close(publisher_listener_);
            publisher_listener_ = SRT_INVALID_SOCK;
        }
        if (subscriber_listener_ != SRT_INVALID_SOCK) { // 关闭订阅者监听 socket
            srt_close(subscriber_listener_);
            subscriber_listener_ = SRT_INVALID_SOCK;
        }

        close_publisher();        // 关闭当前发布者连接
        close_all_subscribers();  // 关闭所有订阅者连接

        if (accept_publisher_thread_.joinable()) accept_publisher_thread_.join();   // 回收发布者 accept 线程
        if (accept_subscriber_thread_.joinable()) accept_subscriber_thread_.join(); // 回收订阅者 accept 线程
        if (monitor_thread_.joinable()) monitor_thread_.join();                     // 回收监控线程

        srt_cleanup(); // 清理 SRT 库
        log_line("INFO", "relay server stopped"); // 打印停止日志
    }

private:
    static bool set_sock_flag(SRTSOCKET sock, SRT_SOCKOPT opt, const void* val, int len, const std::string& name) {
        // 设置某个 SRT socket 选项，并在失败时打印日志
        if (srt_setsockflag(sock, opt, val, len) != 0) {
            log_line("ERROR", "set " + name + " failed: " + last_srt_error());
            return false;
        }
        return true;
    }

    SRTSOCKET create_listener(const std::string& ip, int port, bool sender_side) {
        // 创建一个监听 socket，用于 accept publisher 或 subscriber
        SRTSOCKET sock = srt_socket(AF_INET, SOCK_DGRAM, 0); // 创建 SRT socket（SRT 建立在 UDP 语义上）
        if (sock == SRT_INVALID_SOCK) {
            log_line("ERROR", "srt_socket failed: " + last_srt_error());
            return SRT_INVALID_SOCK;
        }

        int yes = 1;                    // 常用布尔选项 true
        int no = 0;                     // 常用布尔选项 false（当前代码未实际设置）
        int latency = cfg_.latency_ms;  // latency 配置
        int recv_buf = cfg_.recv_buf_bytes; // 接收缓冲区配置
        int send_buf = cfg_.send_buf_bytes; // 发送缓冲区配置
        int linger_ms = 0;              // 关闭时不 linger

        if (!set_sock_flag(sock, SRTO_REUSEADDR, &yes, sizeof(yes), "SRTO_REUSEADDR")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 设置失败则关闭 socket 并返回失败
        }
        if (!set_sock_flag(sock, SRTO_RCVSYN, &yes, sizeof(yes), "SRTO_RCVSYN")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 接收同步模式
        }
        if (!set_sock_flag(sock, SRTO_SNDSYN, &yes, sizeof(yes), "SRTO_SNDSYN")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 发送同步模式
        }
        if (!set_sock_flag(sock, SRTO_TSBPDMODE, &yes, sizeof(yes), "SRTO_TSBPDMODE")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 启用 TSBPD 模式，按时间戳/播放延时处理
        }
        if (!set_sock_flag(sock, SRTO_LATENCY, &latency, sizeof(latency), "SRTO_LATENCY")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 设置 latency
        }
        if (!set_sock_flag(sock, SRTO_RCVBUF, &recv_buf, sizeof(recv_buf), "SRTO_RCVBUF")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 设置接收缓冲区
        }
        if (!set_sock_flag(sock, SRTO_SNDBUF, &send_buf, sizeof(send_buf), "SRTO_SNDBUF")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 设置发送缓冲区
        }
        if (!set_sock_flag(sock, SRTO_LINGER, &linger_ms, sizeof(linger_ms), "SRTO_LINGER")) {
            srt_close(sock); return SRT_INVALID_SOCK; // 设置关闭 linger
        }

        // publisher 入口按发送端场景配置，subscriber 入口按接收端场景配置。
        // 这里不强依赖 SRTO_SENDER，仅保留兼容性注释。
        (void)sender_side; // 当前参数未使用，避免编译器告警
        (void)no;          // 当前变量未使用，避免编译器告警

        sockaddr_in addr{};                          // 监听地址结构
        addr.sin_family = AF_INET;                   // IPv4
        addr.sin_port = htons(static_cast<uint16_t>(port)); // 主机序端口 -> 网络序
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) { // 文本 IP -> 二进制 IP
            log_line("ERROR", "invalid bind ip: " + ip);
            srt_close(sock);
            return SRT_INVALID_SOCK;
        }

        if (srt_bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SRT_ERROR) {
            log_line("ERROR", "srt_bind failed: " + last_srt_error());
            srt_close(sock);
            return SRT_INVALID_SOCK;
        }
        if (srt_listen(sock, 64) == SRT_ERROR) { // 进入监听状态，backlog=64
            log_line("ERROR", "srt_listen failed: " + last_srt_error());
            srt_close(sock);
            return SRT_INVALID_SOCK;
        }
        return sock; // 成功返回监听 socket
    }

    void accept_publisher_loop() { // 接受发布者连接的线程
        while (g_running.load(std::memory_order_relaxed)) { // 只要服务还在运行就循环 accept
            sockaddr_in peer{};               // 对端地址
            int peer_len = sizeof(peer);      // 地址长度
            SRTSOCKET sock = srt_accept(publisher_listener_, reinterpret_cast<sockaddr*>(&peer), &peer_len); // 接受新连接
            if (sock == SRT_INVALID_SOCK) {   // accept 失败
                if (g_running.load(std::memory_order_relaxed)) {
                    log_line("WARN", "publisher accept failed: " + last_srt_error());
                }
                break; // 退出 accept 循环
            }

            std::string peer_str = sockaddr_to_string(peer); // 对端字符串
            {
                std::lock_guard<std::mutex> lk(publisher_mu_); // 保护 publisher_sock_
                if (publisher_sock_ != SRT_INVALID_SOCK) {     // 已经有发布者在线
                    log_line("WARN", "reject extra publisher from " + peer_str + ", only one publisher is allowed");
                    srt_close(sock); // 拒绝额外发布者
                    continue;
                }
                publisher_sock_ = sock; // 接纳这个发布者
                publisher_peer_ = peer; // 保存对端地址
                status_.publisher_online.store(true, std::memory_order_relaxed);     // 发布者在线
                status_.publisher_connected_at_ms.store(steady_ms(), std::memory_order_relaxed); // 记录连接时间
                status_.publisher_last_active_at_ms.store(steady_ms(), std::memory_order_relaxed); // 初始活跃时间
            }

            log_line("INFO", "publisher connected: " + peer_str); // 打印连接成功日志
            publisher_read_thread_ = std::thread([this]() { this->publisher_read_loop(); }); // 启动发布者读线程
            if (publisher_read_thread_.joinable()) publisher_read_thread_.join(); // 等待读线程结束
            close_publisher(); // 读线程结束后清理发布者状态
        }
    }

    void publisher_read_loop() { // 发布者读线程：接收 TS chunk 并广播给订阅者
        std::vector<char> buf(static_cast<size_t>(cfg_.payload_size)); // 每次 recv 使用固定大小缓冲区

        while (g_running.load(std::memory_order_relaxed)) { // 服务运行时循环接收
            SRTSOCKET sock; // 当前发布者 socket 的本地副本
            {
                std::lock_guard<std::mutex> lk(publisher_mu_); // 保护 publisher_sock_
                sock = publisher_sock_;                        // 拿到当前 socket
            }
            if (sock == SRT_INVALID_SOCK) break; // 没有发布者则退出

            int n = srt_recv(sock, buf.data(), static_cast<int>(buf.size())); // 从发布者收一个 chunk
            if (n == SRT_ERROR) { // 接收失败
                log_line("WARN", "publisher recv failed: " + last_srt_error());
                break; // 退出读循环
            }
            if (n == 0) continue; // 收到 0 字节则忽略，继续

            status_.publisher_last_active_at_ms.store(steady_ms(), std::memory_order_relaxed); // 更新最近活跃时间
            status_.total_ingress_bytes.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed); // 总入流累加
            status_.ingress_bytes_window.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed); // 窗口入流累加
            status_.ingress_chunks.fetch_add(1, std::memory_order_relaxed); // 收到 chunk 数 +1

            if (cfg_.validate_ts) { // 如果开启 TS 校验
                bool ok = validator_.inspect(reinterpret_cast<const uint8_t*>(buf.data()),
                                             static_cast<size_t>(n), status_); // 检查当前 chunk
                if (!ok) { // 校验失败
                    log_line("WARN", std::string("TS validation failed: ") + validator_.last_error());
                    continue; // 丢掉该 chunk，不广播
                }
            }

            broadcast(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n)); // 向所有订阅者广播该 chunk
        }
    }

    void accept_subscriber_loop() { // 接受订阅者连接的线程
        while (g_running.load(std::memory_order_relaxed)) { // 服务运行时持续 accept
            sockaddr_in peer{};               // 对端地址
            int peer_len = sizeof(peer);      // 地址长度
            SRTSOCKET sock = srt_accept(subscriber_listener_, reinterpret_cast<sockaddr*>(&peer), &peer_len); // 接收一个订阅者连接
            if (sock == SRT_INVALID_SOCK) {   // accept 失败
                if (g_running.load(std::memory_order_relaxed)) {
                    log_line("WARN", "subscriber accept failed: " + last_srt_error());
                }
                break; // 退出 accept 循环
            }

            uint64_t id = next_subscriber_id_.fetch_add(1, std::memory_order_relaxed); // 分配唯一订阅者 id
            auto session = std::make_shared<SubscriberSession>(sock, peer, status_, cfg_, id); // 创建会话对象
            {
                std::lock_guard<std::mutex> lk(subscribers_mu_); // 保护 subscribers_ map
                subscribers_[id] = session;                      // 放入订阅者表
            }
            session->start(); // 启动该订阅者自己的发送线程
        }
    }

    void broadcast(const uint8_t* data, size_t len) { // 把一个 chunk 广播给所有订阅者
        std::vector<uint64_t> dead;                   // 收集失效订阅者 id
        {
            std::lock_guard<std::mutex> lk(subscribers_mu_); // 遍历 subscribers_ 时加锁
            for (auto& [id, sess] : subscribers_) {          // 遍历所有订阅者
                if (!sess->running()) {                      // 如果会话已不在运行
                    dead.push_back(id);                      // 标记待清理
                    continue;
                }
                if (!sess->push_chunk(data, len)) {          // 尝试把数据塞到该订阅者发送队列
                    dead.push_back(id);                      // 入队失败则也认为它失效
                }
            }
            for (uint64_t id : dead) {                      // 清理失效订阅者
                auto it = subscribers_.find(id);
                if (it != subscribers_.end()) {
                    it->second->stop();                     // 停止会话
                    subscribers_.erase(it);                 // 从表里删除
                }
            }
        }
    }

    void close_publisher() { // 关闭当前发布者连接
        std::lock_guard<std::mutex> lk(publisher_mu_); // 保护 publisher_sock_
        if (publisher_sock_ != SRT_INVALID_SOCK) {     // 如果当前有发布者
            srt_close(publisher_sock_);                // 关闭其 socket
            publisher_sock_ = SRT_INVALID_SOCK;        // 清空句柄
        }
        status_.publisher_online.store(false, std::memory_order_relaxed); // 标记发布者离线
    }

    void close_all_subscribers() { // 关闭所有订阅者连接
        std::lock_guard<std::mutex> lk(subscribers_mu_); // 保护 subscribers_
        for (auto& [_, sess] : subscribers_) {           // 遍历所有会话
            sess->stop();                                // 逐个停止
        }
        subscribers_.clear();                            // 清空 map
    }

    void monitor_loop() { // 周期性输出统计信息
        while (g_running.load(std::memory_order_relaxed)) { // 服务运行时循环
            std::this_thread::sleep_for(Ms(cfg_.monitor_interval_ms)); // 睡眠一个监控周期
            if (!g_running.load(std::memory_order_relaxed)) break;     // 退出前再检查一次

            uint64_t ingress = status_.ingress_bytes_window.exchange(0, std::memory_order_relaxed); // 取出并清零窗口入流字节
            uint64_t egress = status_.egress_bytes_window.exchange(0, std::memory_order_relaxed);   // 取出并清零窗口出流字节
            double seconds = static_cast<double>(cfg_.monitor_interval_ms) / 1000.0; // 监控周期秒数
            double ingress_mbps = (ingress * 8.0) / seconds / 1000.0 / 1000.0;       // 计算入流 Mbps
            double egress_mbps = (egress * 8.0) / seconds / 1000.0 / 1000.0;         // 计算出流 Mbps

            std::ostringstream oss;              // 拼接日志内容
            oss << std::fixed << std::setprecision(2) // 小数保留两位
                << "publisher_online=" << (status_.publisher_online.load(std::memory_order_relaxed) ? 1 : 0)
                << " ingress_mbps=" << ingress_mbps
                << " egress_mbps=" << egress_mbps
                << " subscribers=" << status_.subscriber_count.load(std::memory_order_relaxed)
                << " ts_discontinuities=" << status_.ts_discontinuities.load(std::memory_order_relaxed)
                << " queue_drops=" << status_.subscriber_queue_drops.load(std::memory_order_relaxed)
                << " publisher_last_active_ms_ago=" << ms_ago(status_.publisher_last_active_at_ms.load(std::memory_order_relaxed));
                // 上面这些是全局统计字段

            {
                std::lock_guard<std::mutex> lk(subscribers_mu_); // 遍历订阅者时加锁
                int idx = 0;                                     // 已展开的订阅者个数
                for (const auto& [id, sess] : subscribers_) {    // 遍历订阅者
                    if (idx >= 8) { // 最多展开前 8 个，避免日志太长
                        oss << " subscribers_more=" << (subscribers_.size() - 8); // 其余只打印数量
                        break;
                    }
                    oss << " | sub#" << id
                        << " peer=" << sess->peer()
                        << " q=" << sess->queue_depth()
                        << " latency_ms~=" << sess->configured_latency_ms()
                        << " last_send_ms_ago=" << ms_ago(sess->last_send_active_ms());
                        // 打印每个订阅者的地址、队列深度、latency、最近发送时间
                    ++idx;
                }
            }

            log_line("STAT", oss.str()); // 输出一条 STAT 日志
        }
    }

    static int64_t ms_ago(int64_t t) { // 把某个历史时刻转换成“距今多少毫秒”
        if (t <= 0) return -1;         // 非法时间返回 -1
        return steady_ms() - t;        // 当前 steady ms - 历史时刻
    }

private:
    Config cfg_;                 // 服务配置
    StreamStatus status_;        // 全局状态/统计
    TsValidator validator_;      // TS 校验器

    std::atomic<bool> stopped_{true}; // 用于防止 stop() 重复执行（但命名和初始值比较别扭）

    SRTSOCKET publisher_listener_ = SRT_INVALID_SOCK;   // 发布者监听 socket
    SRTSOCKET subscriber_listener_ = SRT_INVALID_SOCK;  // 订阅者监听 socket

    std::mutex publisher_mu_;          // 保护当前发布者连接
    SRTSOCKET publisher_sock_ = SRT_INVALID_SOCK; // 当前发布者 socket
    sockaddr_in publisher_peer_{};     // 当前发布者地址

    std::mutex subscribers_mu_;        // 保护订阅者 map
    std::map<uint64_t, std::shared_ptr<SubscriberSession>> subscribers_; // 所有订阅者会话
    std::atomic<uint64_t> next_subscriber_id_{1}; // 下一个订阅者 id

    std::thread accept_publisher_thread_;  // 发布者 accept 线程
    std::thread accept_subscriber_thread_; // 订阅者 accept 线程
    std::thread publisher_read_thread_;    // 发布者读线程
    std::thread monitor_thread_;           // 监控线程
};

Config parse_args(int argc, char** argv) { // 解析命令行参数
    Config cfg;                            // 先用默认配置初始化
    for (int i = 1; i < argc; ++i) {       // 从 argv[1] 开始扫描参数
        std::string a = argv[i];           // 当前参数名
        auto need_value = [&](const std::string& name) -> std::string { // 读取该参数后面的值
            if (i + 1 >= argc) {           // 如果后面没有值
                throw std::runtime_error("missing value for " + name); // 抛异常
            }
            return argv[++i];              // 消耗并返回后一个参数
        };

        if (a == "--bind") cfg.bind_ip = need_value(a);                              // 设置绑定 IP
        else if (a == "--pub-port") cfg.publisher_port = std::stoi(need_value(a));  // 设置发布端口
        else if (a == "--sub-port") cfg.subscriber_port = std::stoi(need_value(a)); // 设置订阅端口
        else if (a == "--payload-size") cfg.payload_size = std::stoi(need_value(a)); // 设置每次收发 chunk 大小
        else if (a == "--latency-ms") cfg.latency_ms = std::stoi(need_value(a));    // 设置 SRT latency
        else if (a == "--recv-buf") cfg.recv_buf_bytes = std::stoi(need_value(a));  // 设置接收缓冲区
        else if (a == "--send-buf") cfg.send_buf_bytes = std::stoi(need_value(a));  // 设置发送缓冲区
        else if (a == "--sub-queue") cfg.subscriber_queue_max_chunks = std::stoi(need_value(a)); // 设置订阅者队列长度
        else if (a == "--no-validate-ts") cfg.validate_ts = false;                   // 关闭 TS 校验
        else if (a == "--require-h264") cfg.require_h264 = true;                     // 强制要求 PMT 里有 H.264
        else if (a == "--monitor-ms") cfg.monitor_interval_ms = std::stoi(need_value(a)); // 设置监控周期
        else if (a == "--help") { // 打印帮助并退出
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
                << "  --monitor-ms <ms>        default: 2000\n"
                << "  --no-validate-ts         disable TS validation\n"
                << "  --require-h264           require H.264 in PMT\n";
            std::exit(0); // 正常退出
        }
        else {
            throw std::runtime_error("unknown arg: " + a); // 未知参数，抛异常
        }
    }
    return cfg; // 返回解析后的配置
}

void signal_handler(int) { // 信号处理函数
    g_running.store(false, std::memory_order_relaxed); // 收到 SIGINT / SIGTERM 后通知全局退出
}

} // namespace // 匿名命名空间结束

int main(int argc, char** argv) { // 程序入口
    std::signal(SIGINT, signal_handler);  // Ctrl+C 时触发 signal_handler
    std::signal(SIGTERM, signal_handler); // kill 默认终止信号时也触发

    try {
        Config cfg = parse_args(argc, argv); // 解析命令行参数，得到配置
        RelayServer server(cfg);             // 构造中继服务器
        if (!server.start()) {               // 启动服务器
            return 1;                        // 启动失败返回非 0
        }

        while (g_running.load(std::memory_order_relaxed)) { // 主线程只是保活等待退出信号
            std::this_thread::sleep_for(200ms);             // 每 200ms 看一次是否该退出
        }

        server.stop(); // 收到退出信号后主动停止服务器
        return 0;      // 正常退出
    } catch (const std::exception& ex) { // 捕获所有标准异常
        log_line("ERROR", ex.what());    // 打印异常信息
        return 1;                        // 异常退出
    }
}