#pragma once

#ifdef __cplusplus
  #include <cstdint>
  #include <cstring>
#else
  #include <stdint.h>
  #include <string.h>
#endif

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
#else
  #include <arpa/inet.h>
  #include <time.h>
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t seq;
    uint64_t t_push_us;      // 推流端 wall clock us
    uint64_t t_relay_in_us;  // 中继端 wall clock us
} TelemetryPkt;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t seq;
} TelemetryAck;
#pragma pack(pop)

#ifdef __cplusplus
  static_assert(sizeof(TelemetryPkt) == 32, "TelemetryPkt size mismatch");
  static_assert(sizeof(TelemetryAck) == 16, "TelemetryAck size mismatch");
  static constexpr uint32_t TLMY_MAGIC     = 0x544C4D59u;
  static constexpr uint16_t TLMY_VERSION   = 1u;
  static constexpr int      TLMY_PORT      = 19900;
  static constexpr int      TLMY_FWD_PORT  = 19901;
  static constexpr int      TLMY_ACK_PORT  = 19902;
#else
  #define TLMY_MAGIC      0x544C4D59u
  #define TLMY_VERSION    1u
  #define TLMY_PORT       19900
  #define TLMY_FWD_PORT   19901
  #define TLMY_ACK_PORT   19902
#endif

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  #define TLMY_LITTLE_ENDIAN 1
#elif defined(_WIN32)
  #define TLMY_LITTLE_ENDIAN 1
#else
  #define TLMY_LITTLE_ENDIAN 0
#endif

static inline uint64_t tlmy_hton64(uint64_t v) {
#if TLMY_LITTLE_ENDIAN
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFFu));
    return ((uint64_t)lo << 32) | (uint64_t)hi;
#else
    return v;
#endif
}
#define tlmy_ntoh64 tlmy_hton64

static inline void tlmy_encode(TelemetryPkt *p, uint64_t seq,
                               uint64_t t_push, uint64_t t_relay) {
    p->magic         = htonl(TLMY_MAGIC);
    p->version       = htons(TLMY_VERSION);
    p->reserved      = 0;
    p->seq           = tlmy_hton64(seq);
    p->t_push_us     = tlmy_hton64(t_push);
    p->t_relay_in_us = tlmy_hton64(t_relay);
}

static inline int tlmy_decode(const void *buf, int len, TelemetryPkt *out) {
    TelemetryPkt tmp;
    if (len < (int)sizeof(TelemetryPkt)) return 0;
    memcpy(&tmp, buf, sizeof(tmp));
    if (ntohl(tmp.magic)   != TLMY_MAGIC)   return 0;
    if (ntohs(tmp.version) != TLMY_VERSION) return 0;

    out->magic         = TLMY_MAGIC;
    out->version       = TLMY_VERSION;
    out->reserved      = 0;
    out->seq           = tlmy_ntoh64(tmp.seq);
    out->t_push_us     = tlmy_ntoh64(tmp.t_push_us);
    out->t_relay_in_us = tlmy_ntoh64(tmp.t_relay_in_us);
    return 1;
}

static inline void tlmy_ack_encode(TelemetryAck* p, uint64_t seq) {
    p->magic    = htonl(TLMY_MAGIC);
    p->version  = htons(TLMY_VERSION);
    p->reserved = 0;
    p->seq      = tlmy_hton64(seq);
}

static inline int tlmy_ack_decode(const void* buf, int len, TelemetryAck* out) {
    TelemetryAck tmp;
    if (len < (int)sizeof(TelemetryAck)) return 0;
    memcpy(&tmp, buf, sizeof(tmp));
    if (ntohl(tmp.magic)   != TLMY_MAGIC)   return 0;
    if (ntohs(tmp.version) != TLMY_VERSION) return 0;

    out->magic    = TLMY_MAGIC;
    out->version  = TLMY_VERSION;
    out->reserved = 0;
    out->seq      = tlmy_ntoh64(tmp.seq);
    return 1;
}

/* 单机内相对计时 */
static inline uint64_t mono_us(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (uint64_t)(cnt.QuadPart * 1000000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

/* 保留 wall_us，给 push->relay 用 */
static inline uint64_t wall_us(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    return (uli.QuadPart - EPOCH_DIFF_100NS) / 10ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

#ifdef __cplusplus
inline void tlmy_encode(TelemetryPkt& p, uint64_t seq,
                        uint64_t t_push, uint64_t t_relay) {
    tlmy_encode(&p, seq, t_push, t_relay);
}
inline bool tlmy_decode(const void* buf, int len, TelemetryPkt& out) {
    return tlmy_decode(buf, len, &out) != 0;
}
inline void tlmy_ack_encode(TelemetryAck& p, uint64_t seq) {
    tlmy_ack_encode(&p, seq);
}
inline bool tlmy_ack_decode(const void* buf, int len, TelemetryAck& out) {
    return tlmy_ack_decode(buf, len, &out) != 0;
}
#endif