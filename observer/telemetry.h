#pragma once

/*
 * telemetry.h — 三端通用（推流端/中继/播放端），C99 & C++ 兼容
 *
 * 两套机制：
 *   1. UDP TelemetryPkt/Ack  — 原有独立 UDP 通道（保留）
 *   2. TS 帧内嵌             — 推流端把时间戳插入 SRT 数据流，随帧传输
 *      推流端: tlmy_ts_build()  构造一个 188 字节 telemetry TS 包
 *      播放端: tlmy_ts_scan()   扫描收到的数据，找出 telemetry TS 包
 *      播放端: tlmy_frame_ack_encode/decode  通过 UDP 把延迟结果回报给中继
 *
 * 时钟说明：
 *   - wall_us()  使用系统 wall clock，跨端延迟计算需要 NTP 同步
 *   - mono_us()  单机相对计时，不跨端使用
 */

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

/* ============================================================
 * 端口常量
 * ============================================================ */
#ifdef __cplusplus
  static constexpr int TLMY_PORT         = 19900;  /* 推流端 -> 中继，原 UDP telemetry */
  static constexpr int TLMY_FWD_PORT     = 19901;  /* 中继   -> 播放端，原 UDP telemetry 转发 */
  static constexpr int TLMY_ACK_PORT     = 19902;  /* 播放端 -> 中继，原 UDP ACK */
  static constexpr int TLMY_FRAME_ACK_PORT = 19903;/* 播放端 -> 中继，帧内嵌 ACK */
#else
  #define TLMY_PORT           19900
  #define TLMY_FWD_PORT       19901
  #define TLMY_ACK_PORT       19902
  #define TLMY_FRAME_ACK_PORT 19903
#endif

/* ============================================================
 * Magic / Version
 * ============================================================ */
#ifdef __cplusplus
  static constexpr uint32_t TLMY_MAGIC          = 0x544C4D59u; /* 'TLMY' */
  static constexpr uint32_t TLMY_FRAME_ACK_MAGIC= 0x544C4641u; /* 'TLFA' */
  static constexpr uint16_t TLMY_VERSION        = 1u;
#else
  #define TLMY_MAGIC           0x544C4D59u
  #define TLMY_FRAME_ACK_MAGIC 0x544C4641u
  #define TLMY_VERSION         1u
#endif

/* ============================================================
 * TS 帧内嵌常量
 * ============================================================ */
#ifdef __cplusplus
  /* 私有 PID，不与常见节目 PID 冲突 */
  static constexpr uint16_t TLMY_TS_PID      = 0x1FFEu;
  /* PSI table_id，0xFC-0xFE 是用户私有 */
  static constexpr uint8_t  TLMY_TABLE_ID    = 0xFCu;
  /* 推流端每发出 N 个 SRT 包就插入一个 telemetry TS 包 */
  static constexpr int      TLMY_TS_INTERVAL = 30;
#else
  #define TLMY_TS_PID       0x1FFEu
  #define TLMY_TABLE_ID     0xFCu
  #define TLMY_TS_INTERVAL  30
#endif

/* ============================================================
 * 字节序辅助
 * ============================================================ */
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

/* ============================================================
 * 时钟
 * ============================================================ */

/* wall_us: UNIX 纪元 wall clock，单位 μs。跨端延迟计算依赖 NTP 同步 */
static inline uint64_t wall_us(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (uli.QuadPart - 116444736000000000ULL) / 10ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

/* mono_us: 单机单调时钟，单位 μs。只用于同一进程内的相对计时 */
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

/* ============================================================
 * 原有 UDP TelemetryPkt / TelemetryAck（保留，不改）
 * ============================================================ */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t seq;
    uint64_t t_push_us;       /* 推流端 wall clock us */
    uint64_t t_relay_in_us;   /* 中继端 wall clock us */
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
#endif

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

static inline void tlmy_ack_encode(TelemetryAck *p, uint64_t seq) {
    p->magic    = htonl(TLMY_MAGIC);
    p->version  = htons(TLMY_VERSION);
    p->reserved = 0;
    p->seq      = tlmy_hton64(seq);
}

static inline int tlmy_ack_decode(const void *buf, int len, TelemetryAck *out) {
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

/* ============================================================
 * 帧内嵌 TS 包：构造与解析
 *
 * 布局（188 字节）：
 *   [0]      0x47               TS sync byte
 *   [1]      0x60|(PID>>8)      payload_unit_start=1, PID 高5位
 *   [2]      PID & 0xFF         PID 低8位
 *   [3]      0x10|(cc&0x0F)     adaptation_field_control=01（仅 payload），cc
 *   [4]      0x00               pointer_field（PSI 起始）
 *   [5]      0xFC               private_section table_id
 *   [6]      0x00               section_syntax_indicator=0, 保留
 *   [7]      0x14               section_length = 20
 *                               （payload: 8 t_push + 8 seq + 4 crc = 20）
 *   [8..15]  t_push_us          big-endian uint64，推流端 wall_us()
 *   [16..23] seq                big-endian uint64，包序号
 *   [24..27] CRC32/MPEG         覆盖 buf[5..23]（19 字节）
 *   [28..187] 0xFF              填充
 * ============================================================ */

/* MPEG CRC32，生成多项式 0x04C11DB7 */
static inline uint32_t tlmy_crc32(const uint8_t *data, int len) {
    uint32_t crc = 0xFFFFFFFFu;
    int i, b;
    for (i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i] << 24;
        for (b = 0; b < 8; ++b)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    }
    return crc;
}

/*
 * tlmy_ts_build — 推流端调用
 *   buf      : 调用方提供的 188 字节缓冲区
 *   t_push_us: wall_us() 取得的推流端时间戳
 *   seq      : 单调递增包序号
 *   cc       : TS continuity_counter（0~15 循环，调用方维护）
 */
static inline void tlmy_ts_build(uint8_t *buf,
                                  uint64_t t_push_us,
                                  uint64_t seq,
                                  uint8_t  cc) {
    int i;
    uint32_t crc;
    memset(buf, 0xFF, 188);

    buf[0] = 0x47u;
    buf[1] = (uint8_t)(0x60u | ((TLMY_TS_PID >> 8) & 0x1Fu)); /* payload_unit_start=1 */
    buf[2] = (uint8_t)(TLMY_TS_PID & 0xFFu);
    buf[3] = (uint8_t)(0x10u | (cc & 0x0Fu));  /* adaptation_field_control=01 */
    buf[4] = 0x00u;                             /* pointer_field */
    buf[5] = TLMY_TABLE_ID;
    buf[6] = 0x00u;
    buf[7] = 0x14u;                             /* section_length = 20 */

    /* t_push_us big-endian */
    for (i = 0; i < 8; ++i)
        buf[8 + i] = (uint8_t)((t_push_us >> (56 - i * 8)) & 0xFFu);

    /* seq big-endian */
    for (i = 0; i < 8; ++i)
        buf[16 + i] = (uint8_t)((seq >> (56 - i * 8)) & 0xFFu);

    /* CRC32 覆盖 buf[5..23]，共 19 字节 */
    crc = tlmy_crc32(buf + 5, 19);
    buf[24] = (uint8_t)((crc >> 24) & 0xFFu);
    buf[25] = (uint8_t)((crc >> 16) & 0xFFu);
    buf[26] = (uint8_t)((crc >>  8) & 0xFFu);
    buf[27] = (uint8_t)( crc        & 0xFFu);
}

/*
 * tlmy_ts_parse — 解析单个 188 字节 TS 包
 *   返回 1：是合法的 telemetry 包，字段已写入 out 参数
 *   返回 0：不是 telemetry 包，或 CRC 校验失败
 */
static inline int tlmy_ts_parse(const uint8_t *buf,
                                 uint64_t *t_push_us_out,
                                 uint64_t *seq_out) {
    uint16_t pid;
    uint32_t crc_stored, crc_calc;
    uint64_t t, s;
    int i;

    if (buf[0] != 0x47u) return 0;
    pid = (uint16_t)(((buf[1] & 0x1Fu) << 8) | buf[2]);
    if (pid != TLMY_TS_PID) return 0;
    if (buf[5] != TLMY_TABLE_ID) return 0;

    crc_stored = ((uint32_t)buf[24] << 24) | ((uint32_t)buf[25] << 16)
               | ((uint32_t)buf[26] <<  8) |  (uint32_t)buf[27];
    crc_calc = tlmy_crc32(buf + 5, 19);
    if (crc_stored != crc_calc) return 0;

    t = 0;
    for (i = 0; i < 8; ++i) t = (t << 8) | buf[8 + i];
    s = 0;
    for (i = 0; i < 8; ++i) s = (s << 8) | buf[16 + i];

    *t_push_us_out = t;
    *seq_out       = s;
    return 1;
}

/*
 * tlmy_ts_scan — 播放端调用，扫描任意长度的数据缓冲区
 *   data     : 收到的数据（可以是多个 TS 包拼在一起的 chunk）
 *   len      : 数据字节数
 *   t_out    : 输出 t_push_us
 *   seq_out  : 输出 seq
 *   返回值   : 找到返回 1，未找到返回 0
 *
 * 注意：返回第一个找到的 telemetry 包就停止扫描。
 * 如果数据未对齐到 188 字节边界，会尝试按字节搜索 sync_byte。
 */
static inline int tlmy_ts_scan(const uint8_t *data, int len,
                                uint64_t *t_out, uint64_t *seq_out) {
    int off;
    if (!data || len < 188) return 0;

    /* 快速路径：数据已对齐 188 字节 */
    if (len % 188 == 0) {
        for (off = 0; off + 188 <= len; off += 188) {
            if (tlmy_ts_parse(data + off, t_out, seq_out))
                return 1;
        }
        return 0;
    }

    /* 慢速路径：逐字节搜索 sync_byte */
    for (off = 0; off + 188 <= len; ++off) {
        if (data[off] == 0x47u) {
            if (tlmy_ts_parse(data + off, t_out, seq_out))
                return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 帧内嵌 ACK：播放端 -> 中继
 *
 * 播放端收到 telemetry TS 包后，通过 UDP 把延迟数据回报给中继，
 * 中继侧可在 WebSocket dashboard 中展示真实端到端帧延迟。
 *
 * 端口：TLMY_FRAME_ACK_PORT (19903)
 * ============================================================ */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;            /* TLMY_FRAME_ACK_MAGIC = 'TLFA' */
    uint16_t version;
    uint16_t reserved;
    uint64_t seq;
    uint64_t t_push_us;        /* 从 TS 包里解析出来的推流端时间戳 */
    uint64_t t_player_recv_us; /* 播放端收到该帧时的 wall_us() */
} TelemetryFrameAck;
#pragma pack(pop)

#ifdef __cplusplus
  static_assert(sizeof(TelemetryFrameAck) == 32, "TelemetryFrameAck size mismatch");
#endif

static inline void tlmy_frame_ack_encode(TelemetryFrameAck *p,
                                          uint64_t seq,
                                          uint64_t t_push_us,
                                          uint64_t t_player_recv_us) {
    p->magic             = htonl(TLMY_FRAME_ACK_MAGIC);
    p->version           = htons(TLMY_VERSION);
    p->reserved          = 0;
    p->seq               = tlmy_hton64(seq);
    p->t_push_us         = tlmy_hton64(t_push_us);
    p->t_player_recv_us  = tlmy_hton64(t_player_recv_us);
}

static inline int tlmy_frame_ack_decode(const void *buf, int len,
                                         TelemetryFrameAck *out) {
    TelemetryFrameAck tmp;
    if (len < (int)sizeof(TelemetryFrameAck)) return 0;
    memcpy(&tmp, buf, sizeof(tmp));
    if (ntohl(tmp.magic)   != TLMY_FRAME_ACK_MAGIC) return 0;
    if (ntohs(tmp.version) != TLMY_VERSION)          return 0;
    out->magic            = TLMY_FRAME_ACK_MAGIC;
    out->version          = TLMY_VERSION;
    out->reserved         = 0;
    out->seq              = tlmy_ntoh64(tmp.seq);
    out->t_push_us        = tlmy_ntoh64(tmp.t_push_us);
    out->t_player_recv_us = tlmy_ntoh64(tmp.t_player_recv_us);
    return 1;
}

/* ============================================================
 * C++ 重载（仅 C++ 编译单元可见，不影响 C 端）
 * ============================================================ */
#ifdef __cplusplus
inline void tlmy_encode(TelemetryPkt &p, uint64_t seq,
                        uint64_t t_push, uint64_t t_relay) {
    tlmy_encode(&p, seq, t_push, t_relay);
}
inline bool tlmy_decode(const void *buf, int len, TelemetryPkt &out) {
    return tlmy_decode(buf, len, &out) != 0;
}
inline void tlmy_ack_encode(TelemetryAck &p, uint64_t seq) {
    tlmy_ack_encode(&p, seq);
}
inline bool tlmy_ack_decode(const void *buf, int len, TelemetryAck &out) {
    return tlmy_ack_decode(buf, len, &out) != 0;
}
inline void tlmy_frame_ack_encode(TelemetryFrameAck &p,
                                   uint64_t seq,
                                   uint64_t t_push_us,
                                   uint64_t t_player_recv_us) {
    tlmy_frame_ack_encode(&p, seq, t_push_us, t_player_recv_us);
}
inline bool tlmy_frame_ack_decode(const void *buf, int len,
                                   TelemetryFrameAck &out) {
    return tlmy_frame_ack_decode(buf, len, &out) != 0;
}
#endif /* __cplusplus */