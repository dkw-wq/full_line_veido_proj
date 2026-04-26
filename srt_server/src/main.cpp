// srt_server/src/main.cpp
// 应用入口。服务端能力按模块拆分，保持单翻译单元以降低大规模拆分风险。

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
#include "ws_util.hpp"

namespace {

#include "server_context.hpp"
#include "ws_broadcaster.hpp"
#include "ts_validator.hpp"
#include "subscriber_controller.hpp"
#include "global_controller.hpp"
#include "pusher_control_client.hpp"
#include "subscriber_session.hpp"
#include "telemetry_relay.hpp"
#include "relay_server.hpp"
#include "cli.hpp"

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