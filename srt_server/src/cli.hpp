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