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