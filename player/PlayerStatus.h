#pragma once

#include <QMetaType>
#include <QString>

// ============================================================
// 状态机类型定义
// ============================================================

enum class PlayerState {
    Idle,
    Starting,
    OpeningInput,
    ProbingStreams,
    Initializing,
    WaitingKeyframe,
    Playing,
    Reconnecting,
    Stopping,
    Stopped,
    Error
};
// 注意：删掉了 PausedRemote —— 它不是播放线程的真实状态，
// 而是一个"覆盖显示层"的概念，通过 remote_paused 字段单独表达。

enum class PlayerErrorCode {
    None,
    OpenInputFailed,
    FindStreamInfoFailed,
    NoVideoStream,
    VideoInitFailed,
    AudioInitFailed,
    D3D11ContextFailed,
    PacketAllocFailed,
    ReadFrameFailed,
    RendererInitFailed,
    DecoderFailed,
    ControlCommandFailed
};

// ── RecordPhase：区分"用户意图"和"底层实况" ────────────────
// 这是解决录制按钮闪烁的关键：
//   Idle          → 用户未请求，底层也未录制
//   Requested     → 用户已点"开始录制"，底层尚未 start
//   Active        → 底层已确认开始录制
//   StopRequested → 用户已点"停止录制"，底层尚未 stop
enum class RecordPhase {
    Idle,
    Requested,
    Active,
    StopRequested
};

struct PlayerStatus {
    // ── 播放线程驱动的字段（只有播放线程写） ──────────────────
    PlayerState     state              = PlayerState::Idle;
    PlayerErrorCode error              = PlayerErrorCode::None;
    QString         message;
    bool            audio_available    = false;
    QString         url;
    int             reconnect_count    = 0;
    qint64          last_packet_ms_ago = -1;
    double          audio_clock        = -1.0;

    // ── 用户意图字段（只有主线程写） ──────────────────────────
    // remote_paused：用户是否已向推流端发出暂停指令并成功
    // 这个字段永远不影响 state，UI 显示时单独叠加处理
    bool            remote_paused      = false;

    // record_phase：录制的完整生命周期，包含意图和实况两层
    RecordPhase     record_phase       = RecordPhase::Idle;
};

Q_DECLARE_METATYPE(PlayerStatus)
