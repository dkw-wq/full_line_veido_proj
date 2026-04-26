namespace ws_util {


static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static std::string sha1_b64(const std::string& input) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    std::vector<uint8_t> msg(input.begin(), input.end());
    uint64_t bit_len = msg.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back((bit_len >> (i * 8)) & 0xFF);
    for (size_t i = 0; i < msg.size(); i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; ++j)
            w[j] = ((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)|
                   ((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for (int j = 16; j < 80; ++j) w[j] = rotl32(w[j-3]^w[j-8]^w[j-14]^w[j-16],1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int j = 0; j < 80; ++j) {
            uint32_t f,k;
            if      (j<20){f=(b&c)|(~b&d);k=0x5A827999;}
            else if (j<40){f=b^c^d;       k=0x6ED9EBA1;}
            else if (j<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else          {f=b^c^d;       k=0xCA62C1D6;}
            uint32_t t=rotl32(a,5)+f+e+k+w[j];
            e=d;d=c;c=rotl32(b,30);b=a;a=t;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    uint8_t digest[20];
    for (int i = 0; i < 5; ++i) {
        digest[i*4]   = (h[i]>>24)&0xFF;
        digest[i*4+1] = (h[i]>>16)&0xFF;
        digest[i*4+2] = (h[i]>>8)&0xFF;
        digest[i*4+3] = h[i]&0xFF;
    }
    static const char* b64t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < 20; i += 3) {
        int rem = std::min(3, 20 - i);
        uint32_t buf = ((uint32_t)digest[i]<<16)
                     | (rem>1?(uint32_t)digest[i+1]<<8:0)
                     | (rem>2?(uint32_t)digest[i+2]:0);
        out += b64t[(buf>>18)&63];
        out += b64t[(buf>>12)&63];
        out += (rem>1) ? b64t[(buf>>6)&63] : '=';
        out += (rem>2) ? b64t[buf&63]      : '=';
    }
    return out;
}

static std::string http_header(const std::string& req, const std::string& key) {
    std::string lo_req = req;
    std::string lo_key = key;
    std::transform(lo_req.begin(), lo_req.end(), lo_req.begin(), ::tolower);
    std::transform(lo_key.begin(), lo_key.end(), lo_key.begin(), ::tolower);
    auto pos = lo_req.find(lo_key + ":");
    if (pos == std::string::npos) return {};
    auto vs = req.find(':', pos) + 1;
    auto ve = req.find("\r\n", vs);
    std::string v = req.substr(vs, ve - vs);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
    return v;
}

static std::vector<uint8_t> ws_frame(const std::string& text) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);
    size_t len = text.size();
    if (len <= 125) {
        frame.push_back((uint8_t)len);
    } else if (len <= 65535) {
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) frame.push_back((len >> (i*8)) & 0xFF);
    }
    frame.insert(frame.end(), text.begin(), text.end());
    return frame;
}

static std::string ws_recv_frame(int fd) {
    uint8_t hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return "";
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return "";
        len = ((uint64_t)ext[0]<<8)|ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return "";
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len<<8)|ext[i];
    }
    uint8_t mask[4] = {};
    if (masked && recv(fd, mask, 4, MSG_WAITALL) != 4) return "";
    if (len == 0) return "";
    std::vector<uint8_t> payload(len);
    if ((uint64_t)recv(fd, payload.data(), (int)len, MSG_WAITALL) != len) return "";
    if (masked) for (size_t i = 0; i < len; ++i) payload[i] ^= mask[i & 3];
    return std::string(payload.begin(), payload.end());
}

} // namespace ws_util