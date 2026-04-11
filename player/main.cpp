// main.cpp
// 依赖：Qt6, FFmpeg, D3D11Renderer.h, ControlClient.h, Recorder.h

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
}
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/channel_layout.h>

#include <QApplication>
#include <QWidget>
#include <QObject>
#include <QCoreApplication>
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioFormat>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaType>
#include <QString>
#include <QStyle>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>
#include <mutex>

#include "D3D11Renderer.h"
#include "ControlClient.h"
#include "Recorder.h"

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

// ============================================================
// UI 渲染辅助函数
// 所有 UI 状态的计算集中在这里，不散落在各处
// ============================================================

// "有效显示状态"：remote_paused 时覆盖主状态，仅影响显示，不改数据
static PlayerState effectiveDisplayState(const PlayerStatus& s) {
    if (s.remote_paused) return PlayerState::Idle; // 用一个不存在的枚举占位
    return s.state;
}

static QString stateText(const PlayerStatus& s) {
    if (s.remote_paused) return "远端已暂停";
    switch (s.state) {
        case PlayerState::Idle:            return "空闲";
        case PlayerState::Starting:        return "启动中";
        case PlayerState::OpeningInput:    return "连接输入...";
        case PlayerState::ProbingStreams:  return "探测流...";
        case PlayerState::Initializing:    return "初始化解码器...";
        case PlayerState::WaitingKeyframe: return "等待关键帧...";
        case PlayerState::Playing:         return "播放中";
        case PlayerState::Reconnecting:    return "重连中";
        case PlayerState::Stopping:        return "停止中";
        case PlayerState::Stopped:         return "已停止";
        case PlayerState::Error:           return "错误";
    }
    return {};
}

static QString dotColor(const PlayerStatus& s) {
    if (s.remote_paused) return "#ef9f27";  // 橙
    switch (s.state) {
        case PlayerState::Playing:         return "#1d9e75";  // 绿
        case PlayerState::Error:           return "#e24b4a";  // 红
        case PlayerState::Reconnecting:
        case PlayerState::OpeningInput:
        case PlayerState::ProbingStreams:
        case PlayerState::Initializing:
        case PlayerState::WaitingKeyframe:
        case PlayerState::Starting:        return "#378add";  // 蓝（过渡态）
        default:                           return "#888780";  // 灰
    }
}

static std::pair<QString,QString> stateBadgeColors(const PlayerStatus& s) {
    if (s.remote_paused) return {"#854f0b", "#fac775"};
    switch (s.state) {
        case PlayerState::Playing:      return {"#0f6e56", "#9fe1cb"};
        case PlayerState::Error:        return {"#a32d2d", "#f09595"};
        case PlayerState::Reconnecting: return {"#185fa5", "#b5d4f4"};
        default:                        return {"#2c2c2a", "#888780"};
    }
}

// 录制按钮：用 RecordPhase 统一决定显示，不读 recorder_.isRecording()
// Requested / Active → 都显示"停止录制"（用户已有意图，立即响应）
// StopRequested / Idle → 都显示"开始录制"
static bool recordBtnShowStop(const PlayerStatus& s) {
    return s.record_phase == RecordPhase::Requested
        || s.record_phase == RecordPhase::Active;
}

// ============================================================
// FFmpeg error helper
// ============================================================

static void print_ffmpeg_error(const char* prefix, int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    std::cerr << prefix << ": " << buf << "\n";
}

// ============================================================
// HW decode helpers
// ============================================================

static AVBufferRef*       g_hw_device_ctx = nullptr;
static enum AVPixelFormat g_hw_pix_fmt    = AV_PIX_FMT_NONE;

static enum AVPixelFormat hw_get_format(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == g_hw_pix_fmt) return *p;
    std::cerr << "HW surface format not found\n";
    return AV_PIX_FMT_NONE;
}

static bool init_hw_device(AVCodecContext* ctx, const char* device) {
    g_hw_pix_fmt = AV_PIX_FMT_NONE;
    AVHWDeviceType type = av_hwdevice_find_type_by_name(device);
    if (type == AV_HWDEVICE_TYPE_NONE) return false;
    const AVCodecHWConfig* cfg = nullptr;
    for (int i = 0; (cfg = avcodec_get_hw_config(ctx->codec, i)); ++i) {
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == type) {
            g_hw_pix_fmt = cfg->pix_fmt;
            break;
        }
    }
    if (g_hw_pix_fmt == AV_PIX_FMT_NONE) return false;
    int r = av_hwdevice_ctx_create(&g_hw_device_ctx, type, nullptr, nullptr, 0);
    if (r < 0) { print_ffmpeg_error("av_hwdevice_ctx_create", r); return false; }
    ctx->hw_device_ctx = av_buffer_ref(g_hw_device_ctx);
    ctx->get_format    = hw_get_format;
    return true;
}

static int interrupt_cb(void* opaque) {
    return reinterpret_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}

// ============================================================
// AudioPipeline
// ============================================================

class AudioPipeline {
public:
    ~AudioPipeline() { cleanup(); }

    bool init(AVFormatContext* fmt_ctx, int stream_index, const AVCodec* dec) {
        if (stream_index < 0 || !dec) return false;
        stream_index_ = stream_index;
        stream_       = fmt_ctx->streams[stream_index_];
        codec_ctx_    = avcodec_alloc_context3(dec);
        if (!codec_ctx_) return false;
        if (avcodec_parameters_to_context(codec_ctx_, stream_->codecpar) < 0) { cleanup(); return false; }
        if (avcodec_open2(codec_ctx_, dec, nullptr) < 0)                       { cleanup(); return false; }

        out_channels_ = std::min(2, codec_ctx_->ch_layout.nb_channels > 0
                                    ? codec_ctx_->ch_layout.nb_channels : 2);
        out_rate_     = codec_ctx_->sample_rate > 0 ? codec_ctx_->sample_rate : 48000;

        QAudioFormat fmt;
        fmt.setChannelCount(out_channels_);
        fmt.setSampleRate(out_rate_);
        fmt.setSampleFormat(QAudioFormat::Int16);
        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        if (!dev.isFormatSupported(fmt)) { cleanup(); return false; }

        sink_  = new QAudioSink(dev, fmt);
        io_    = sink_->start();
        frame_ = av_frame_alloc();
        return sink_ && io_ && frame_;
    }

    bool handlePacket(AVPacket* pkt) {
        if (!codec_ctx_ || !io_) return false;
        if (avcodec_send_packet(codec_ctx_, pkt) < 0) return false;
        int ret = 0;
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            const int in_ch   = frame_->ch_layout.nb_channels > 0
                                 ? frame_->ch_layout.nb_channels : out_channels_;
            const int samples = frame_->nb_samples;
            if (samples <= 0) { av_frame_unref(frame_); continue; }

            std::vector<int16_t> buf(samples * out_channels_);
            if (frame_->format == AV_SAMPLE_FMT_FLTP) {
                const float* L = reinterpret_cast<const float*>(frame_->data[0]);
                const float* R = (in_ch > 1) ? reinterpret_cast<const float*>(frame_->data[1]) : nullptr;
                for (int i = 0; i < samples; ++i) {
                    float l = L[i], r = R ? R[i] : l;
                    buf[2*i]   = static_cast<int16_t>(std::clamp(l,-1.f,1.f)*32767.f);
                    buf[2*i+1] = static_cast<int16_t>(std::clamp(r,-1.f,1.f)*32767.f);
                }
            } else if (frame_->format == AV_SAMPLE_FMT_S16) {
                const auto* p = reinterpret_cast<const int16_t*>(frame_->data[0]);
                buf.assign(p, p + samples * in_ch);
                if (in_ch == 1)
                    for (int i = samples-1; i >= 0; --i) {
                        int16_t v = buf[i]; buf[2*i] = buf[2*i+1] = v;
                    }
            } else { av_frame_unref(frame_); continue; }

            io_->write(reinterpret_cast<const char*>(buf.data()),
                       static_cast<qint64>(buf.size() * sizeof(int16_t)));

            if (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                clock_sec_ = frame_->best_effort_timestamp * av_q2d(stream_->time_base)
                             + static_cast<double>(samples) / out_rate_;
            av_frame_unref(frame_);
        }
        return true;
    }

    double clock()       const { return clock_sec_; }
    int    streamIndex() const { return stream_index_; }

    void cleanup() {
        if (sink_)      { sink_->stop(); delete sink_; sink_ = nullptr; }
        io_ = nullptr;
        if (frame_)     { av_frame_free(&frame_); frame_ = nullptr; }
        if (codec_ctx_) { avcodec_free_context(&codec_ctx_); }
        stream_ = nullptr; stream_index_ = -1; clock_sec_ = -1.0;
    }

private:
    AVCodecContext* codec_ctx_    = nullptr;
    AVStream*       stream_       = nullptr;
    int             stream_index_ = -1;
    QAudioSink*     sink_         = nullptr;
    QIODevice*      io_           = nullptr;
    AVFrame*        frame_        = nullptr;
    int             out_channels_ = 2;
    int             out_rate_     = 48000;
    double          clock_sec_    = -1.0;
};

// ============================================================
// VideoPipeline
// ============================================================

class VideoPipeline {
public:
    ~VideoPipeline() { cleanup(); }

    bool init(AVFormatContext* fmt_ctx, int stream_index, const AVCodec* dec) {
        if (stream_index < 0 || !dec) return false;
        stream_index_ = stream_index;
        stream_       = fmt_ctx->streams[stream_index_];
        codec_ctx_    = avcodec_alloc_context3(dec);
        if (!codec_ctx_) return false;
        if (avcodec_parameters_to_context(codec_ctx_, stream_->codecpar) < 0) { cleanup(); return false; }
        codec_ctx_->thread_count = 1;
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (!init_hw_device(codec_ctx_, "d3d11va")) { cleanup(); return false; }
        if (avcodec_open2(codec_ctx_, dec, nullptr) < 0)  { cleanup(); return false; }
        width_     = codec_ctx_->width;
        height_    = codec_ctx_->height;
        time_base_ = stream_->time_base;
        frame_     = av_frame_alloc();
        return frame_ != nullptr;
    }

    bool handlePacket(AVPacket* pkt,
                      AVD3D11VADeviceContext* d3d,
                      HWND hwnd,
                      double audio_clock,
                      std::atomic<bool>& stop_flag,
                      bool& renderer_failed_out) {
        if (!codec_ctx_) return false;
        renderer_failed_out = false;
        if (avcodec_send_packet(codec_ctx_, pkt) < 0) return false;
        int ret = 0;
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            if (!renderer_ready_) {
                if (frame_->width > 0 && frame_->height > 0 && d3d &&
                    renderer_.initialize(hwnd, d3d->device, d3d->device_context,
                                         frame_->width, frame_->height)) {
                    renderer_ready_ = true;
                    width_ = frame_->width; height_ = frame_->height;
                } else {
                    renderer_failed_out = true;
                    stop_flag.store(true);
                    av_frame_unref(frame_);
                    break;
                }
            }
            if (frame_->format != g_hw_pix_fmt) {
                if (!warned_sw_) { std::cerr << "video: non-HW frame skipped\n"; warned_sw_ = true; }
                av_frame_unref(frame_); continue;
            }
            const double vpts = frame_->best_effort_timestamp * av_q2d(time_base_);
            if (audio_clock >= 0.0) {
                const double diff = vpts - audio_clock;
                if      (diff >  0.12) std::this_thread::sleep_for(std::chrono::milliseconds((int)((diff-0.04)*1000)));
                else if (diff < -0.15) { av_frame_unref(frame_); continue; }
            }
            renderer_.presentFrame(frame_, width_, height_);
            av_frame_unref(frame_);
        }
        return true;
    }

    void cleanup() {
        if (frame_)     { av_frame_free(&frame_); frame_ = nullptr; }
        if (codec_ctx_) {
            if (codec_ctx_->hw_device_ctx) av_buffer_unref(&codec_ctx_->hw_device_ctx);
            avcodec_free_context(&codec_ctx_);
        }
        renderer_.shutdown();
        stream_ = nullptr; stream_index_ = -1; renderer_ready_ = false; warned_sw_ = false;
    }

    int        streamIndex() const { return stream_index_; }
    AVRational timeBase()    const { return time_base_; }

private:
    AVCodecContext* codec_ctx_      = nullptr;
    AVStream*       stream_         = nullptr;
    int             stream_index_   = -1;
    AVFrame*        frame_          = nullptr;
    D3D11Renderer   renderer_;
    bool            renderer_ready_ = false;
    bool            warned_sw_      = false;
    int             width_ = 0, height_ = 0;
    AVRational      time_base_{1,1};
};

// ============================================================
// FFmpegD3D11Player — QObject + 重构后的状态机
//
// 核心设计原则：
//   1. PlayerStatus 是唯一状态源，所有 UI 从同一份快照渲染
//   2. state 字段只由播放线程写，外部不得修改
//   3. remote_paused 只由主线程写，不干预 state
//   4. record_phase 是录制的完整生命周期（意图 + 实况），
//      播放线程负责将 Requested→Active, StopRequested→Idle
//   5. 所有状态变更都通过 commitStatus() 统一触发 emit，
//      不存在零散的 setStatus / emitCurrentStatus 混用
// ============================================================

class FFmpegD3D11Player : public QObject {
    Q_OBJECT

public:
    explicit FFmpegD3D11Player(HWND hwnd, std::string url, QObject* parent = nullptr)
        : QObject(parent), hwnd_(hwnd), url_(std::move(url)) {
        status_.url = QString::fromStdString(url_);
    }

    ~FFmpegD3D11Player() override { stop(); }

    bool start() {
        if (running_) return true;
        running_ = true;
        mutateStatus([&](PlayerStatus& s) {
            s.state   = PlayerState::Starting;
            s.message = "线程启动中";
        });
        worker_ = std::thread(&FFmpegD3D11Player::run, this);
        return true;
    }

    void stop() {
        if (!running_) return;
        mutateStatus([&](PlayerStatus& s) {
            s.state   = PlayerState::Stopping;
            s.message = "正在停止";
        });
        stopFlag_.store(true);
        if (worker_.joinable()) worker_.join();
        running_ = false;
        mutateStatus([&](PlayerStatus& s) {
            s.state   = PlayerState::Stopped;
            s.message = "已停止";
        });
    }

    // ── 录制控制：只改意图字段，播放线程负责落地 ──────────────
    // 立即 emit，UI 立即响应，不等播放线程
    void requestStartRecord(const std::string& path) {
        {
            std::lock_guard<std::mutex> lk(recordPathMutex_);
            recordPath_ = path;
        }
        mutateStatus([](PlayerStatus& s) {
            if (s.record_phase == RecordPhase::Idle)
                s.record_phase = RecordPhase::Requested;
        });
    }

    void requestStopRecord() {
        mutateStatus([](PlayerStatus& s) {
            if (s.record_phase == RecordPhase::Active ||
                s.record_phase == RecordPhase::Requested)
                s.record_phase = RecordPhase::StopRequested;
        });
    }

    // ── 远端暂停控制：只改 remote_paused，不碰 state ──────────
    // 调用方已确认指令成功后才调用此函数
    void setRemotePaused(bool paused) {
        mutateStatus([paused](PlayerStatus& s) {
            s.remote_paused = paused;
            // state 完全不动：播放线程继续独立运行，
            // UI 层通过 stateText(s) / dotColor(s) 统一处理覆盖逻辑
        });
    }

    PlayerStatus currentStatus() const {
        std::lock_guard<std::mutex> lk(statusMutex_);
        return status_;
    }

signals:
    void statusChanged(const PlayerStatus& status);

private:
    // ----------------------------------------------------------------
    // mutateStatus — 唯一的状态修改入口
    // 接受一个 lambda，在锁内修改 status_，然后调度到主线程 emit
    // ----------------------------------------------------------------
    template<typename Fn>
    void mutateStatus(Fn&& fn) {
        {
            std::lock_guard<std::mutex> lk(statusMutex_);
            fn(status_);
        }
        QMetaObject::invokeMethod(this, [this] {
            PlayerStatus snap;
            {
                std::lock_guard<std::mutex> lk(statusMutex_);
                snap = status_;
            }
            emit statusChanged(snap);
        }, Qt::QueuedConnection);
    }

    // ── 播放线程专用的状态写函数（不写 remote_paused / record_phase） ──
    // 只更新 state / error / message / audio_available / reconnect_count
    void setPlaybackStatus(PlayerState state, PlayerErrorCode error, const QString& message,
                           bool audio_available_override = false, bool set_audio = false) {
        mutateStatus([&](PlayerStatus& s) {
            s.state           = state;
            s.error           = error;
            s.message         = message;
            s.reconnect_count = reconnect_count_;
            s.url             = QString::fromStdString(url_);
            if (set_audio) s.audio_available = audio_available_override;
        });
    }

    // ----------------------------------------------------------------
    // 主播放线程
    // ----------------------------------------------------------------

    void run() {
        while (!stopFlag_.load()) {
            video_synced_ = false;

            // ── OpeningInput ──────────────────────────────────────────
            setPlaybackStatus(PlayerState::OpeningInput, PlayerErrorCode::None,
                              "正在连接 " + QString::fromStdString(url_));

            AVFormatContext* fmt_ctx = avformat_alloc_context();
            if (!fmt_ctx) {
                setPlaybackStatus(PlayerState::Error, PlayerErrorCode::OpenInputFailed,
                                  "avformat_alloc_context 失败");
                break;
            }
            fmt_ctx->interrupt_callback.callback = interrupt_cb;
            fmt_ctx->interrupt_callback.opaque   = &stopFlag_;

            AVDictionary* opts = nullptr;
            av_dict_set(&opts, "fflags",          "nobuffer", 0);
            av_dict_set(&opts, "flags",           "low_delay", 0);
            av_dict_set(&opts, "analyzeduration", "500000",   0);
            av_dict_set(&opts, "probesize",       "32768",    0);

            int open_ret = avformat_open_input(&fmt_ctx, url_.c_str(), nullptr, &opts);
            av_dict_free(&opts);
            if (open_ret < 0) {
                char eb[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(open_ret, eb, sizeof(eb));
                avformat_free_context(fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::OpenInputFailed,
                                  QString("连接失败: %1  (第%2次重连)").arg(eb).arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // ── ProbingStreams ────────────────────────────────────────
            setPlaybackStatus(PlayerState::ProbingStreams, PlayerErrorCode::None, "探测流信息...");
            if (int r = avformat_find_stream_info(fmt_ctx, nullptr); r < 0) {
                char eb[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(r, eb, sizeof(eb));
                avformat_close_input(&fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::FindStreamInfoFailed,
                                  QString("流信息探测失败: %1  (第%2次重连)").arg(eb).arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            // ── Initializing ──────────────────────────────────────────
            setPlaybackStatus(PlayerState::Initializing, PlayerErrorCode::None, "初始化解码器...");

            const AVCodec* dec = nullptr;
            int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
            if (video_idx < 0) {
                avformat_close_input(&fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::NoVideoStream,
                                  QString("未找到视频流  (第%1次重连)").arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            const AVCodec* audio_dec = nullptr;
            int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, video_idx, &audio_dec, 0);
            bool audio_ok = (audio_idx >= 0);

            VideoPipeline video;
            if (!video.init(fmt_ctx, video_idx, dec)) {
                avformat_close_input(&fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::VideoInitFailed,
                                  QString("视频解码器初始化失败  (第%1次重连)").arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            AudioPipeline audio;
            if (!audio.init(fmt_ctx, audio_idx, audio_dec)) audio_ok = false;

            // 同步 audio_available（使用带 set_audio 标志的重载）
            setPlaybackStatus(PlayerState::Initializing, PlayerErrorCode::None,
                              "初始化解码器...", audio_ok, true);

            auto* hwdev    = g_hw_device_ctx
                             ? reinterpret_cast<AVHWDeviceContext*>(g_hw_device_ctx->data) : nullptr;
            auto* d3d11ctx = hwdev
                             ? reinterpret_cast<AVD3D11VADeviceContext*>(hwdev->hwctx) : nullptr;
            if (!d3d11ctx || !d3d11ctx->device || !d3d11ctx->device_context) {
                video.cleanup(); audio.cleanup(); avformat_close_input(&fmt_ctx);
                if (g_hw_device_ctx) { av_buffer_unref(&g_hw_device_ctx); g_hw_pix_fmt = AV_PIX_FMT_NONE; }
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::D3D11ContextFailed,
                                  QString("D3D11 设备获取失败  (第%1次重连)").arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            AVPacket* pkt = av_packet_alloc();
            if (!pkt) {
                video.cleanup(); audio.cleanup(); avformat_close_input(&fmt_ctx);
                if (g_hw_device_ctx) { av_buffer_unref(&g_hw_device_ctx); g_hw_pix_fmt = AV_PIX_FMT_NONE; }
                setPlaybackStatus(PlayerState::Error, PlayerErrorCode::PacketAllocFailed,
                                  "av_packet_alloc 失败");
                break;
            }

            reconnect_count_ = 0;
            setPlaybackStatus(PlayerState::WaitingKeyframe, PlayerErrorCode::None, "等待关键帧...");

            auto last_pkt_time = std::chrono::steady_clock::now();

            // ── 读包循环 ─────────────────────────────────────────────
            while (!stopFlag_.load()) {

                // ── 录制生命周期管理（意图 → 实况转换） ─────────────
                // 这是解决录制按钮异步问题的核心：
                //   Requested    → 调用 recorder_.start()，成功则推进到 Active
                //   StopRequested→ 调用 recorder_.stop()，完成后推进到 Idle
                // 全部在播放线程内完成，无竞态
                {
                    RecordPhase phase;
                    {
                        std::lock_guard<std::mutex> lk(statusMutex_);
                        phase = status_.record_phase;
                    }

                    if (phase == RecordPhase::Requested) {
                        std::string path;
                        { std::lock_guard<std::mutex> lk(recordPathMutex_); path = recordPath_; }
                        bool started = recorder_.start(fmt_ctx, video_idx, audio_idx, path);
                        mutateStatus([started](PlayerStatus& s) {
                            s.record_phase = started ? RecordPhase::Active : RecordPhase::Idle;
                        });
                    } else if (phase == RecordPhase::StopRequested) {
                        recorder_.stop();
                        mutateStatus([](PlayerStatus& s) {
                            s.record_phase = RecordPhase::Idle;
                        });
                    }
                }

                // 刷新非关键实时字段（不 emit，避免过频繁）
                {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_pkt_time).count();
                    std::lock_guard<std::mutex> lk(statusMutex_);
                    status_.last_packet_ms_ago = elapsed;
                    status_.audio_clock        = audio.clock();
                }

                int ret = av_read_frame(fmt_ctx, pkt);
                if (ret < 0) {
                    if (ret != AVERROR_EOF && ret != AVERROR_EXIT && !stopFlag_.load()) {
                        char eb[AV_ERROR_MAX_STRING_SIZE]{};
                        av_strerror(ret, eb, sizeof(eb));
                        ++reconnect_count_;
                        setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::ReadFrameFailed,
                                          QString("读帧失败: %1  (第%2次重连)").arg(eb).arg(reconnect_count_));
                    }
                    break;
                }

                last_pkt_time = std::chrono::steady_clock::now();

                if (recorder_.isRecording()) recorder_.writePacket(pkt);

                if (pkt->stream_index == video.streamIndex()) {
                    if (!video_synced_) {
                        if (!(pkt->flags & AV_PKT_FLAG_KEY)) { av_packet_unref(pkt); continue; }
                        video_synced_ = true;
                        setPlaybackStatus(PlayerState::Playing, PlayerErrorCode::None,
                                          audio_ok ? "播放中（含音频）" : "播放中（无音频）");
                    }
                    bool rfail = false;
                    video.handlePacket(pkt, d3d11ctx, hwnd_, audio.clock(), stopFlag_, rfail);
                    if (rfail) {
                        setPlaybackStatus(PlayerState::Error, PlayerErrorCode::RendererInitFailed,
                                          "渲染器初始化失败");
                        break;
                    }
                } else if (pkt->stream_index == audio.streamIndex()) {
                    audio.handlePacket(pkt);
                }

                av_packet_unref(pkt);

                // 5 秒无数据 → 重连
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_pkt_time).count() > 5000) {
                    if (stopFlag_.load()) break;
                    ++reconnect_count_;
                    setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::ReadFrameFailed,
                                      QString("5秒无数据，重连  (第%1次)").arg(reconnect_count_));
                    break;
                }
            }

            // ── 清理本次连接（若录制在跑则停止） ─────────────────────
            if (recorder_.isRecording()) {
                recorder_.stop();
                mutateStatus([](PlayerStatus& s) {
                    s.record_phase = RecordPhase::Idle;
                });
            }

            av_packet_free(&pkt);
            video.cleanup();
            audio.cleanup();
            avformat_close_input(&fmt_ctx);
            if (g_hw_device_ctx) { av_buffer_unref(&g_hw_device_ctx); g_hw_pix_fmt = AV_PIX_FMT_NONE; }

            if (stopFlag_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        {
            std::lock_guard<std::mutex> lk(statusMutex_);
            if (status_.state != PlayerState::Error &&
                status_.state != PlayerState::Stopping)
                ; // 下面的 setPlaybackStatus 会处理
        }
        setPlaybackStatus(PlayerState::Stopped, PlayerErrorCode::None, "播放线程已退出");
    }

    // ---- 成员 ----
    HWND        hwnd_;
    std::string url_;
    std::thread worker_;

    std::atomic<bool> stopFlag_{false};
    bool              running_         = false;
    bool              video_synced_    = false;
    int               reconnect_count_ = 0;

    Recorder          recorder_;
    std::string       recordPath_  = "record.ts";
    std::mutex        recordPathMutex_;

    mutable std::mutex statusMutex_;
    PlayerStatus       status_;
};

// ============================================================
// SRT host 解析
// ============================================================

static std::string extract_host_from_srt_url(const std::string& url) {
    const std::string prefix = "srt://";
    size_t pos   = url.rfind(prefix, 0);
    size_t start = (pos == 0) ? prefix.size() : 0;
    size_t end   = url.find_first_of(":/?", start);
    if (end == std::string::npos) end = url.size();
    return url.substr(start, end - start);
}

// ============================================================
// main
// ============================================================

#include "main.moc"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    qRegisterMetaType<PlayerStatus>("PlayerStatus");

    if (argc < 2) {
        std::cout << "用法: player.exe <srt_url>\n";
        return 0;
    }

    const std::string url            = argv[1];
    const std::string push_ctrl_host = extract_host_from_srt_url(url);
    ControlClient ctrl(push_ctrl_host, 10090);

    // ── 视频画面 ────────────────────────────────────────────────────
    QWidget surface;
    surface.setAttribute(Qt::WA_NativeWindow);
    surface.setAttribute(Qt::WA_PaintOnScreen, true);
    surface.setAttribute(Qt::WA_NoSystemBackground, true);
    surface.setAutoFillBackground(false);
    surface.setMinimumSize(640, 360);

    // ── 顶层容器 ────────────────────────────────────────────────────
    QWidget container;
    container.setStyleSheet("background:#111;");
    QVBoxLayout* mainLayout = new QVBoxLayout(&container);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(&surface, 1);

    // ── 状态栏（底部，高 28px）────────────────────────────────────
    QWidget* statusBar = new QWidget;
    statusBar->setFixedHeight(28);
    statusBar->setStyleSheet("background:#1a1a1a;");

    QHBoxLayout* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(10, 0, 10, 0);
    statusLayout->setSpacing(8);

    QLabel* dotLbl   = new QLabel;
    dotLbl->setFixedSize(10, 10);
    dotLbl->setStyleSheet("background:#888780; border-radius:5px;");

    QLabel* stateLbl = new QLabel("空闲");
    stateLbl->setStyleSheet("color:#b4b2a9; font-size:12px; font-weight:500;");

    QLabel* msgLbl = new QLabel;
    msgLbl->setStyleSheet("color:#5f5e5a; font-size:11px;");
    msgLbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    msgLbl->setMaximumWidth(600);

    QLabel* audioLbl = new QLabel;
    audioLbl->setStyleSheet("color:#5f5e5a; font-size:11px;");

    QLabel* reconLbl = new QLabel;
    reconLbl->setStyleSheet("color:#ef9f27; font-size:11px;");
    reconLbl->hide();

    statusLayout->addWidget(dotLbl);
    statusLayout->addWidget(stateLbl);
    statusLayout->addWidget(msgLbl);
    statusLayout->addStretch();
    statusLayout->addWidget(audioLbl);
    statusLayout->addWidget(reconLbl);

    mainLayout->addWidget(statusBar, 0);

    // ── 控制栏（高 38px）─────────────────────────────────────────
    QWidget* ctrlBar = new QWidget;
    ctrlBar->setFixedHeight(38);
    ctrlBar->setStyleSheet("background:#161616;");

    QHBoxLayout* ctrlLayout = new QHBoxLayout(ctrlBar);
    ctrlLayout->setContentsMargins(10, 5, 10, 5);
    ctrlLayout->setSpacing(6);

    QLabel* stateBadge = new QLabel("空闲");
    stateBadge->setFixedHeight(24);
    stateBadge->setContentsMargins(8, 0, 8, 0);
    stateBadge->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    stateBadge->setStyleSheet(
        "border-radius:4px; font-size:12px; font-weight:500;"
        "background:#2c2c2a; color:#888780; padding:0 8px;");

    auto makeBtn = [](const QString& text) -> QPushButton* {
        auto* b = new QPushButton(text);
        b->setFixedHeight(26);
        b->setStyleSheet(
            "QPushButton {"
            "  padding:0 10px; font-size:12px;"
            "  border:0.5px solid rgba(255,255,255,0.14);"
            "  border-radius:4px;"
            "  background:rgba(255,255,255,0.06);"
            "  color:#c2c0b6;"
            "}"
            "QPushButton:hover  { background:rgba(255,255,255,0.11); }"
            "QPushButton:pressed{ background:rgba(255,255,255,0.18); }");
        return b;
    };

    QPushButton* pauseBtn  = makeBtn("暂停推流");
    QPushButton* recordBtn = makeBtn("开始录制");

    ctrlLayout->addWidget(stateBadge);
    ctrlLayout->addSpacing(6);
    ctrlLayout->addWidget(pauseBtn);
    ctrlLayout->addWidget(recordBtn);
    ctrlLayout->addStretch();

    mainLayout->addWidget(ctrlBar, 0);

    // ── 播放器实例 ───────────────────────────────────────────────
    HWND hwnd = reinterpret_cast<HWND>(surface.winId());
    FFmpegD3D11Player player(hwnd, url);

    // ── statusChanged → 全量渲染 UI ─────────────────────────────
    // 所有 UI 元素都从同一份 PlayerStatus 快照里读，保证一致性。
    // 没有任何本地 bool 变量参与判断，彻底消除"谁最后写就显示谁"的问题。
    QObject::connect(&player, &FFmpegD3D11Player::statusChanged,
                     [&](const PlayerStatus& s) {

        // 1. 指示灯
        dotLbl->setStyleSheet(
            QString("background:%1; border-radius:5px;").arg(dotColor(s)));

        // 2. 状态栏文字（统一走 stateText(s)，内部处理 remote_paused 覆盖）
        stateLbl->setText(stateText(s));
        msgLbl->setText(s.message);

        // 3. 重连角标
        if (s.reconnect_count > 0 && s.state == PlayerState::Reconnecting) {
            reconLbl->setText(QString("第%1次").arg(s.reconnect_count));
            reconLbl->show();
        } else {
            reconLbl->hide();
        }

        // 4. 音频角标
        if (s.audio_available) {
            audioLbl->setText("有声");
            audioLbl->setStyleSheet("color:#1d9e75; font-size:11px;");
        } else {
            audioLbl->setText("无声");
            audioLbl->setStyleSheet("color:#5f5e5a; font-size:11px;");
        }

        // 5. 控制栏 state badge（统一走 stateBadgeColors(s)）
        auto [bg, fg] = stateBadgeColors(s);
        stateBadge->setText(stateText(s));
        stateBadge->setStyleSheet(
            QString("border-radius:4px; font-size:12px; font-weight:500;"
                    "background:%1; color:%2; padding:0 8px;").arg(bg).arg(fg));

        // 6. 暂停按钮 — 只跟 s.remote_paused 走，文字/颜色完全一致
        if (s.remote_paused) {
            pauseBtn->setText("恢复推流");
            pauseBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid #854f0b;"
                "  border-radius:4px;"
                "  background:#412402; color:#fac775;"
                "}"
                "QPushButton:hover  { background:#633806; }"
                "QPushButton:pressed{ background:#854f0b; }");
        } else {
            pauseBtn->setText("暂停推流");
            pauseBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid rgba(255,255,255,0.14);"
                "  border-radius:4px;"
                "  background:rgba(255,255,255,0.06); color:#c2c0b6;"
                "}"
                "QPushButton:hover  { background:rgba(255,255,255,0.11); }"
                "QPushButton:pressed{ background:rgba(255,255,255,0.18); }");
        }

        // 7. 录制按钮 — 只跟 s.record_phase 走（recordBtnShowStop 统一计算）
        //    Requested / Active → "停止录制"（用户意图已登记，立即切换）
        //    StopRequested / Idle → "开始录制"
        if (recordBtnShowStop(s)) {
            recordBtn->setText("停止录制");
            recordBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid #a32d2d;"
                "  border-radius:4px;"
                "  background:#501313; color:#f09595;"
                "}"
                "QPushButton:hover  { background:#791f1f; }"
                "QPushButton:pressed{ background:#a32d2d; }");
        } else {
            recordBtn->setText("开始录制");
            recordBtn->setStyleSheet(
                "QPushButton {"
                "  padding:0 10px; font-size:12px;"
                "  border:0.5px solid rgba(255,255,255,0.14);"
                "  border-radius:4px;"
                "  background:rgba(255,255,255,0.06); color:#c2c0b6;"
                "}"
                "QPushButton:hover  { background:rgba(255,255,255,0.11); }"
                "QPushButton:pressed{ background:rgba(255,255,255,0.18); }");
        }
    });

    // ── 暂停按钮点击 ─────────────────────────────────────────────
    // 读快照 → 发指令 → 成功才调 setRemotePaused()
    // setRemotePaused 只改 remote_paused，不动 state，无竞态
    QObject::connect(pauseBtn, &QPushButton::clicked, [&] {
        const bool currently_paused = player.currentStatus().remote_paused;
        const bool ok = currently_paused ? ctrl.resume() : ctrl.pause();
        if (ok) {
            player.setRemotePaused(!currently_paused);
        } else {
            msgLbl->setText(currently_paused ? "恢复指令发送失败" : "暂停指令发送失败");
        }
    });

    // ── 录制按钮点击 ─────────────────────────────────────────────
    // 读快照中的 record_phase 判断方向，调用意图函数，立即 emit，不等底层
    QObject::connect(recordBtn, &QPushButton::clicked, [&] {
        const RecordPhase phase = player.currentStatus().record_phase;
        if (phase == RecordPhase::Idle || phase == RecordPhase::StopRequested) {
            player.requestStartRecord("C:/Users/dkw/Desktop/record.ts");
        } else {
            player.requestStopRecord();
        }
    });

    // ── 启动 ─────────────────────────────────────────────────────
    container.resize(1280, 800);
    container.show();

    if (!player.start()) {
        std::cerr << "Player start failed\n";
        return -1;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     [&player] { player.stop(); });

    int code = app.exec();
    player.stop();
    return code;
}