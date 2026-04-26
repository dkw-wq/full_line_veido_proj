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