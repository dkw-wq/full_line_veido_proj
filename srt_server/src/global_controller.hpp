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