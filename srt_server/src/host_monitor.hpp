#pragma once

// Linux host counters sampled by the relay monitor thread. Network rates use
// the interface carrying the default route, not a sum of virtual interfaces.
struct HostMetrics {
    std::string nic;
    double nic_rx_mbps = -1;
    double nic_tx_mbps = -1;
    double loopback_rx_mbps = -1;
    double loopback_tx_mbps = -1;
    double cpu_percent = -1;
    int64_t mem_available_mb = -1;
    int64_t process_rss_mb = -1;
};

class HostMonitor {
public:
    HostMetrics sample() {
        HostMetrics m;
        m.nic = default_nic();
        const auto now = Clock::now();
        uint64_t rx = 0, tx = 0;
        if (!m.nic.empty() && nic_bytes(m.nic, rx, tx)) {
            if (previous_nic_ == m.nic && previous_time_ != Clock::time_point{} &&
                now > previous_time_ && rx >= previous_rx_ && tx >= previous_tx_) {
                const double seconds = std::chrono::duration<double>(now - previous_time_).count();
                m.nic_rx_mbps = (rx - previous_rx_) * 8.0 / seconds / 1e6;
                m.nic_tx_mbps = (tx - previous_tx_) * 8.0 / seconds / 1e6;
            }
            previous_nic_ = m.nic;
            previous_rx_ = rx;
            previous_tx_ = tx;
            previous_time_ = now;
        }
        if (nic_bytes("lo", rx, tx)) {
            if (previous_loopback_time_ != Clock::time_point{} && now > previous_loopback_time_ &&
                rx >= previous_loopback_rx_ && tx >= previous_loopback_tx_) {
                const double seconds = std::chrono::duration<double>(now - previous_loopback_time_).count();
                m.loopback_rx_mbps = (rx - previous_loopback_rx_) * 8.0 / seconds / 1e6;
                m.loopback_tx_mbps = (tx - previous_loopback_tx_) * 8.0 / seconds / 1e6;
            }
            previous_loopback_rx_ = rx;
            previous_loopback_tx_ = tx;
            previous_loopback_time_ = now;
        }

        std::ifstream stat("/proc/stat");
        std::string label;
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
                 irq = 0, softirq = 0, steal = 0;
        if (stat >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
            const uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
            const uint64_t idle_total = idle + iowait;
            if (previous_cpu_total_ && total > previous_cpu_total_ && idle_total >= previous_cpu_idle_)
                m.cpu_percent = 100.0 * (1.0 -
                    double(idle_total - previous_cpu_idle_) / double(total - previous_cpu_total_));
            previous_cpu_total_ = total;
            previous_cpu_idle_ = idle_total;
        }

        std::ifstream mem("/proc/meminfo");
        std::string key, unit;
        uint64_t value = 0;
        while (mem >> key >> value >> unit) {
            if (key == "MemAvailable:") { m.mem_available_mb = static_cast<int64_t>(value / 1024); break; }
        }
        std::ifstream self("/proc/self/status");
        while (self >> key) {
            if (key == "VmRSS:") {
                if (self >> value >> unit) m.process_rss_mb = static_cast<int64_t>(value / 1024);
                break;
            }
            std::getline(self, unit);
        }
        return m;
    }

private:
    static std::string default_nic() {
        std::ifstream route("/proc/net/route");
        std::string line;
        std::getline(route, line);
        while (std::getline(route, line)) {
            std::istringstream row(line);
            std::string iface, destination;
            if (row >> iface >> destination && destination == "00000000") return iface;
        }
        return {};
    }

    static bool nic_bytes(const std::string& iface, uint64_t& rx, uint64_t& tx) {
        std::ifstream dev("/proc/net/dev");
        std::string line;
        while (std::getline(dev, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            const size_t first = name.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            name = name.substr(first, name.find_last_not_of(" \t") - first + 1);
            if (name != iface) continue;
            std::istringstream row(line.substr(colon + 1));
            uint64_t ignored = 0;
            if (!(row >> rx)) return false;
            for (int i = 0; i < 7; ++i) if (!(row >> ignored)) return false;
            return bool(row >> tx);
        }
        return false;
    }

    std::string previous_nic_;
    Clock::time_point previous_time_{};
    uint64_t previous_rx_ = 0, previous_tx_ = 0;
    Clock::time_point previous_loopback_time_{};
    uint64_t previous_loopback_rx_ = 0, previous_loopback_tx_ = 0;
    uint64_t previous_cpu_total_ = 0, previous_cpu_idle_ = 0;
};
