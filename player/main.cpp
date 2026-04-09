// player_main.cpp
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
#include <QTimer>
#include <QMetaType>
#include <QString>

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
    Idle,            // 未启动
    Starting,        // start() 已调用，线程启动中
    OpeningInput,    // avformat_open_input 进行中
    ProbingStreams,  // avformat_find_stream_info 进行中
    Initializing,    // 初始化 video/audio/renderer/recorder 前置
    WaitingKeyframe, // 已连上，等待第一个关键帧
    Playing,         // 正常播放
    Reconnecting,    // 读流失败或初始化失败后的重连中
    PausedRemote,    // 已向采集端发 pause 并确认
    Stopping,        // stop() 触发，资源回收中
    Stopped,         // 完全停止
    Error            // 致命错误，无法继续
};

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

struct PlayerStatus {
    PlayerState     state            = PlayerState::Idle;
    PlayerErrorCode error            = PlayerErrorCode::None;
    QString         message;
    bool            audio_available  = false;
    bool            recording        = false;
    bool            remote_paused    = false;
    QString         url;
    int             reconnect_count  = 0;
    qint64          last_packet_ms_ago = -1;
    double          audio_clock      = -1.0;
};

Q_DECLARE_METATYPE(PlayerStatus)

// ============================================================
// 状态→可读字符串
// ============================================================

static QString stateLabel(PlayerState s) {
    switch (s) {
        case PlayerState::Idle:            return "空闲";
        case PlayerState::Starting:        return "启动中";
        case PlayerState::OpeningInput:    return "连接输入...";
        case PlayerState::ProbingStreams:  return "探测流...";
        case PlayerState::Initializing:    return "初始化解码器...";
        case PlayerState::WaitingKeyframe: return "等待关键帧...";
        case PlayerState::Playing:         return "播放中";
        case PlayerState::Reconnecting:    return "重连中";
        case PlayerState::PausedRemote:    return "远端已暂停";
        case PlayerState::Stopping:        return "停止中";
        case PlayerState::Stopped:         return "已停止";
        case PlayerState::Error:           return "错误";
    }
    return "";
}

// 状态对应的指示灯颜色 (CSS-like hex，用于 QLabel styleSheet)
static QString stateDotColor(PlayerState s) {
    switch (s) {
        case PlayerState::Playing:      return "#1d9e75";
        case PlayerState::PausedRemote: return "#ef9f27";
        case PlayerState::Error:        return "#e24b4a";
        case PlayerState::Stopped:
        case PlayerState::Idle:         return "#888780";
        default:                        return "#378add";
    }
}

// ============================================================
// FFmpeg error helper
// ============================================================

static void print_ffmpeg_error(const char* prefix, int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    std::cerr << prefix << ": " << buf << std::endl;
}

// ============================================================
// HW decode helpers (全局，与原版相同)
// ============================================================

static AVBufferRef*        g_hw_device_ctx = nullptr;
static enum AVPixelFormat  g_hw_pix_fmt    = AV_PIX_FMT_NONE;

static enum AVPixelFormat hw_get_format(AVCodecContext*, const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == g_hw_pix_fmt) return *p;
    std::cerr << "HW surface format not found\n";
    return AV_PIX_FMT_NONE;
}

static bool init_hw_device(AVCodecContext* ctx, const char* device) {
    g_hw_pix_fmt = AV_PIX_FMT_NONE;
    AVHWDeviceType type = av_hwdevice_find_type_by_name(device);
    if (type == AV_HWDEVICE_TYPE_NONE) return false;
    const AVCodecHWConfig* config = nullptr;
    for (int i = 0; (config = avcodec_get_hw_config(ctx->codec, i)); ++i) {
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            config->device_type == type) {
            g_hw_pix_fmt = config->pix_fmt;
            break;
        }
    }
    if (g_hw_pix_fmt == AV_PIX_FMT_NONE) return false;
    int ret = av_hwdevice_ctx_create(&g_hw_device_ctx, type, nullptr, nullptr, 0);
    if (ret < 0) { print_ffmpeg_error("av_hwdevice_ctx_create", ret); return false; }
    ctx->hw_device_ctx = av_buffer_ref(g_hw_device_ctx);
    ctx->get_format    = hw_get_format;
    return true;
}

static int interrupt_cb(void* opaque) {
    auto* stop = reinterpret_cast<std::atomic<bool>*>(opaque);
    return (stop && stop->load()) ? 1 : 0;
}

// ============================================================
// AudioPipeline (与原版相同，增加 audio_available 报告)
// ============================================================

class AudioPipeline {
public:
    ~AudioPipeline() { cleanup(); }

    bool init(AVFormatContext* fmt_ctx, int stream_index, const AVCodec* decoder) {
        if (stream_index < 0 || !decoder) return false;
        stream_index_ = stream_index;
        stream_       = fmt_ctx->streams[stream_index_];
        codec_ctx_    = avcodec_alloc_context3(decoder);
        if (!codec_ctx_) return false;
        if (avcodec_parameters_to_context(codec_ctx_, stream_->codecpar) < 0) { cleanup(); return false; }
        if (avcodec_open2(codec_ctx_, decoder, nullptr) < 0)                   { cleanup(); return false; }

        out_channels_ = std::min(2, codec_ctx_->ch_layout.nb_channels > 0 ? codec_ctx_->ch_layout.nb_channels : 2);
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
        if (!codec_ctx_ || !sink_ || !io_) return false;
        int ret = avcodec_send_packet(codec_ctx_, pkt);
        if (ret < 0) return false;
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            const int in_ch   = frame_->ch_layout.nb_channels > 0 ? frame_->ch_layout.nb_channels : out_channels_;
            const int samples = frame_->nb_samples;
            if (samples <= 0) { av_frame_unref(frame_); continue; }
            std::vector<int16_t> interleaved(samples * out_channels_);
            if (frame_->format == AV_SAMPLE_FMT_FLTP) {
                const float* left  = reinterpret_cast<const float*>(frame_->data[0]);
                const float* right = (in_ch > 1) ? reinterpret_cast<const float*>(frame_->data[1]) : nullptr;
                for (int i = 0; i < samples; ++i) {
                    float l = left[i], r = right ? right[i] : l;
                    interleaved[2*i]   = static_cast<int16_t>(std::clamp(l,-1.f,1.f)*32767.f);
                    interleaved[2*i+1] = static_cast<int16_t>(std::clamp(r,-1.f,1.f)*32767.f);
                }
            } else if (frame_->format == AV_SAMPLE_FMT_S16) {
                const auto* p = reinterpret_cast<const int16_t*>(frame_->data[0]);
                interleaved.assign(p, p + samples * in_ch);
                if (in_ch == 1)
                    for (int i = samples-1; i >= 0; --i) {
                        int16_t v = interleaved[i];
                        interleaved[2*i] = interleaved[2*i+1] = v;
                    }
            } else { av_frame_unref(frame_); continue; }
            io_->write(reinterpret_cast<const char*>(interleaved.data()),
                       static_cast<qint64>(interleaved.size() * sizeof(int16_t)));
            if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
                const double pts_sec       = frame_->best_effort_timestamp * av_q2d(stream_->time_base);
                const double frame_dur     = static_cast<double>(samples) / out_rate_;
                clock_sec_ = pts_sec + frame_dur;
            }
            av_frame_unref(frame_);
        }
        return true;
    }

    double clock()       const { return clock_sec_; }
    int    streamIndex() const { return stream_index_; }

    void cleanup() {
        if (sink_)      { sink_->stop(); delete sink_; sink_ = nullptr; }
        io_ = nullptr;
        if (frame_)     av_frame_free(&frame_);
        if (codec_ctx_) avcodec_free_context(&codec_ctx_);
        stream_       = nullptr;
        stream_index_ = -1;
        clock_sec_    = -1.0;
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
// VideoPipeline (与原版相同)
// ============================================================

class VideoPipeline {
public:
    ~VideoPipeline() { cleanup(); }

    bool init(AVFormatContext* fmt_ctx, int stream_index, const AVCodec* decoder) {
        if (stream_index < 0 || !decoder) return false;
        stream_index_ = stream_index;
        stream_       = fmt_ctx->streams[stream_index_];
        codec_ctx_    = avcodec_alloc_context3(decoder);
        if (!codec_ctx_) return false;
        if (avcodec_parameters_to_context(codec_ctx_, stream_->codecpar) < 0) { cleanup(); return false; }
        codec_ctx_->thread_count = 1;
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (!init_hw_device(codec_ctx_, "d3d11va")) { cleanup(); return false; }
        if (avcodec_open2(codec_ctx_, decoder, nullptr) < 0) { cleanup(); return false; }
        width_      = codec_ctx_->width;
        height_     = codec_ctx_->height;
        time_base_  = stream_->time_base;
        frame_      = av_frame_alloc();
        return frame_ != nullptr;
    }

    // 返回 false 表示渲染器初始化失败（需外部切 Error 状态）
    bool handlePacket(AVPacket* pkt,
                      AVD3D11VADeviceContext* d3d_ctx,
                      HWND hwnd,
                      double audio_clock,
                      std::atomic<bool>& stop_flag,
                      bool& renderer_init_failed_out) {
        if (!codec_ctx_) return false;
        renderer_init_failed_out = false;
        int ret = avcodec_send_packet(codec_ctx_, pkt);
        if (ret < 0) { print_ffmpeg_error("video: send_packet", ret); return false; }
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { print_ffmpeg_error("video: receive_frame", ret); break; }
            if (!renderer_ready_) {
                if (frame_->width > 0 && frame_->height > 0 &&
                    d3d_ctx && d3d_ctx->device && d3d_ctx->device_context &&
                    renderer_.initialize(hwnd, d3d_ctx->device, d3d_ctx->device_context,
                                         frame_->width, frame_->height)) {
                    renderer_ready_ = true;
                    width_  = frame_->width;
                    height_ = frame_->height;
                } else {
                    renderer_init_failed_out = true;
                    stop_flag.store(true);
                    av_frame_unref(frame_);
                    break;
                }
            }
            if (frame_->format != g_hw_pix_fmt) {
                if (!warned_sw_) { std::cerr << "video: non-HW frame, skipping\n"; warned_sw_ = true; }
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
        if (frame_)     av_frame_free(&frame_);
        if (codec_ctx_) {
            if (codec_ctx_->hw_device_ctx) av_buffer_unref(&codec_ctx_->hw_device_ctx);
            avcodec_free_context(&codec_ctx_);
        }
        renderer_.shutdown();
        stream_         = nullptr;
        stream_index_   = -1;
        renderer_ready_ = false;
        warned_sw_      = false;
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
    int             width_  = 0, height_ = 0;
    AVRational      time_base_{1,1};
};

// ============================================================
// FFmpegD3D11Player  —  QObject，带完整状态机
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
        setStatus(PlayerState::Starting, PlayerErrorCode::None, "线程启动中");
        worker_ = std::thread(&FFmpegD3D11Player::run, this);
        return true;
    }

    void stop() {
        if (!running_) return;
        setStatus(PlayerState::Stopping, PlayerErrorCode::None, "正在停止");
        stopFlag_.store(true);
        if (worker_.joinable()) worker_.join();
        running_ = false;
        setStatus(PlayerState::Stopped, PlayerErrorCode::None, "已停止");
    }

    void startRecord(const std::string& path) {
        std::lock_guard<std::mutex> lk(recordMutex_);
        recordPath_    = path;
        recordEnabled_.store(true);
    }

    void stopRecord() {
        recordEnabled_.store(false);
        recorder_.stop();
        // 状态快照里 recording 字段会在下次 setStatus 时刷新，
        // 这里直接 emit 一次最新快照即可
        emitCurrentStatus();
    }

    bool isRecording() const { return recorder_.isRecording(); }

    PlayerStatus currentStatus() const {
        std::lock_guard<std::mutex> lk(statusMutex_);
        return status_;
    }

signals:
    void statusChanged(const PlayerStatus& status);

private:
    // ---- 状态管理 ----

    void setStatus(PlayerState state,
                   PlayerErrorCode error,
                   const QString& message) {
        {
            std::lock_guard<std::mutex> lk(statusMutex_);
            status_.state          = state;
            status_.error          = error;
            status_.message        = message;
            status_.recording      = recorder_.isRecording();
            status_.url            = QString::fromStdString(url_);
        }
        // signal 必须从 Qt 线程发，用 QMetaObject::invokeMethod 跨线程安全
        QMetaObject::invokeMethod(this, [this] {
            PlayerStatus snap;
            {
                std::lock_guard<std::mutex> lk(statusMutex_);
                snap = status_;
            }
            emit statusChanged(snap);
        }, Qt::QueuedConnection);
    }

    void emitCurrentStatus() {
        QMetaObject::invokeMethod(this, [this] {
            PlayerStatus snap;
            {
                std::lock_guard<std::mutex> lk(statusMutex_);
                snap = status_;
            }
            emit statusChanged(snap);
        }, Qt::QueuedConnection);
    }

    // ---- 主播放线程 ----

    void run() {
        while (!stopFlag_.load()) {
            video_synced_ = false;

            // ---------- OpeningInput ----------
            setStatus(PlayerState::OpeningInput, PlayerErrorCode::None, "正在连接 " + QString::fromStdString(url_));

            AVFormatContext* fmt_ctx = avformat_alloc_context();
            if (!fmt_ctx) {
                setStatus(PlayerState::Error, PlayerErrorCode::OpenInputFailed, "avformat_alloc_context 失败");
                break;
            }
            fmt_ctx->interrupt_callback.callback = interrupt_cb;
            fmt_ctx->interrupt_callback.opaque   = &stopFlag_;

            AVDictionary* opts = nullptr;
            av_dict_set(&opts, "fflags",           "nobuffer", 0);
            av_dict_set(&opts, "flags",            "low_delay", 0);
            av_dict_set(&opts, "analyzeduration",  "500000", 0);
            av_dict_set(&opts, "probesize",        "32768", 0);

            int open_ret = avformat_open_input(&fmt_ctx, url_.c_str(), nullptr, &opts);
            av_dict_free(&opts);
            if (open_ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(open_ret, errbuf, sizeof(errbuf));
                avformat_free_context(fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setStatus(PlayerState::Reconnecting, PlayerErrorCode::OpenInputFailed,
                          QString("连接失败: %1  (第%2次重连)").arg(errbuf).arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // ---------- ProbingStreams ----------
            setStatus(PlayerState::ProbingStreams, PlayerErrorCode::None, "探测流信息...");
            if (int ret = avformat_find_stream_info(fmt_ctx, nullptr); ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(ret, errbuf, sizeof(errbuf));
                avformat_close_input(&fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setStatus(PlayerState::Reconnecting, PlayerErrorCode::FindStreamInfoFailed,
                          QString("流信息探测失败: %1  (第%2次重连)").arg(errbuf).arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            // ---------- Initializing ----------
            setStatus(PlayerState::Initializing, PlayerErrorCode::None, "初始化解码器...");

            const AVCodec* decoder = nullptr;
            int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
            if (video_idx < 0) {
                avformat_close_input(&fmt_ctx);
                if (stopFlag_.load()) break;
                setStatus(PlayerState::Reconnecting, PlayerErrorCode::NoVideoStream, "未找到视频流，重连中");
                ++reconnect_count_;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            const AVCodec* audio_decoder = nullptr;
            int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, video_idx, &audio_decoder, 0);
            bool audio_ok = (audio_idx >= 0);

            VideoPipeline video;
            if (!video.init(fmt_ctx, video_idx, decoder)) {
                avformat_close_input(&fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setStatus(PlayerState::Reconnecting, PlayerErrorCode::VideoInitFailed,
                          QString("视频解码器初始化失败  (第%1次重连)").arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            AudioPipeline audio;
            if (!audio.init(fmt_ctx, audio_idx, audio_decoder)) {
                audio_ok = false; // 音频可选，降级继续
            }
            {
                std::lock_guard<std::mutex> lk(statusMutex_);
                status_.audio_available = audio_ok;
            }

            // 启动录制
            {
                std::lock_guard<std::mutex> lk(recordMutex_);
                if (recordEnabled_.load() && !recorder_.isRecording()) {
                    if (!recorder_.start(fmt_ctx, video_idx, audio_idx, recordPath_))
                        std::cerr << "Recorder start failed\n";
                }
            }

            // 获取 D3D11 设备上下文
            AVHWDeviceContext*      hwdev   = g_hw_device_ctx ? reinterpret_cast<AVHWDeviceContext*>(g_hw_device_ctx->data) : nullptr;
            AVD3D11VADeviceContext* d3d11ctx = hwdev ? reinterpret_cast<AVD3D11VADeviceContext*>(hwdev->hwctx) : nullptr;
            if (!d3d11ctx || !d3d11ctx->device || !d3d11ctx->device_context) {
                video.cleanup(); audio.cleanup();
                avformat_close_input(&fmt_ctx);
                if (g_hw_device_ctx) { av_buffer_unref(&g_hw_device_ctx); g_hw_pix_fmt = AV_PIX_FMT_NONE; }
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setStatus(PlayerState::Reconnecting, PlayerErrorCode::D3D11ContextFailed,
                          QString("D3D11 设备获取失败  (第%1次重连)").arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            AVPacket* pkt = av_packet_alloc();
            if (!pkt) {
                video.cleanup(); audio.cleanup();
                avformat_close_input(&fmt_ctx);
                if (g_hw_device_ctx) { av_buffer_unref(&g_hw_device_ctx); g_hw_pix_fmt = AV_PIX_FMT_NONE; }
                setStatus(PlayerState::Error, PlayerErrorCode::PacketAllocFailed, "av_packet_alloc 失败");
                break;
            }

            // 成功进入 WaitingKeyframe
            reconnect_count_ = 0;
            setStatus(PlayerState::WaitingKeyframe, PlayerErrorCode::None, "等待关键帧...");

            auto last_pkt_time = std::chrono::steady_clock::now();

            // ---------- 读包循环 ----------
            while (!stopFlag_.load()) {
                // 录制状态同步
                if (recordEnabled_.load() && !recorder_.isRecording()) {
                    std::lock_guard<std::mutex> lk(recordMutex_);
                    if (!recorder_.isRecording()) {
                        if (!recorder_.start(fmt_ctx, video_idx, audio_idx, recordPath_))
                            std::cerr << "Recorder start failed\n";
                    }
                } else if (!recordEnabled_.load() && recorder_.isRecording()) {
                    recorder_.stop();
                }

                // 刷新 last_packet_ms_ago 和 audio_clock 到 status
                {
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_pkt_time).count();
                    std::lock_guard<std::mutex> lk(statusMutex_);
                    status_.last_packet_ms_ago = now_ms;
                    status_.audio_clock        = audio.clock();
                    status_.recording          = recorder_.isRecording();
                    status_.reconnect_count    = reconnect_count_;
                }

                int ret = av_read_frame(fmt_ctx, pkt);
                if (ret < 0) {
                    if (ret != AVERROR_EOF && ret != AVERROR_EXIT) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE]{};
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        if (stopFlag_.load()) break;
                        ++reconnect_count_;
                        setStatus(PlayerState::Reconnecting, PlayerErrorCode::ReadFrameFailed,
                                  QString("读帧失败: %1  (第%2次重连)").arg(errbuf).arg(reconnect_count_));
                    }
                    break;
                }

                last_pkt_time = std::chrono::steady_clock::now();

                if (recorder_.isRecording())
                    recorder_.writePacket(pkt);

                if (pkt->stream_index == video.streamIndex()) {
                    if (!video_synced_) {
                        if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
                            av_packet_unref(pkt);
                            continue;
                        }
                        video_synced_ = true;
                        setStatus(PlayerState::Playing, PlayerErrorCode::None,
                                  audio_ok ? "播放中（含音频）" : "播放中（无音频）");
                    }
                    bool renderer_failed = false;
                    video.handlePacket(pkt, d3d11ctx, hwnd_, audio.clock(), stopFlag_, renderer_failed);
                    if (renderer_failed) {
                        setStatus(PlayerState::Error, PlayerErrorCode::RendererInitFailed, "渲染器初始化失败");
                        break;
                    }
                } else if (pkt->stream_index == audio.streamIndex()) {
                    audio.handlePacket(pkt);
                }

                av_packet_unref(pkt);

                // 超时检测（5 秒无包）
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pkt_time).count() > 5000) {
                    if (stopFlag_.load()) break;
                    ++reconnect_count_;
                    setStatus(PlayerState::Reconnecting, PlayerErrorCode::ReadFrameFailed,
                              QString("5秒无数据包，重连  (第%1次)").arg(reconnect_count_));
                    break;
                }
            }

            // ---------- 清理本次连接 ----------
            recorder_.stop();
            av_packet_free(&pkt);
            video.cleanup();
            audio.cleanup();
            avformat_close_input(&fmt_ctx);
            if (g_hw_device_ctx) { av_buffer_unref(&g_hw_device_ctx); g_hw_pix_fmt = AV_PIX_FMT_NONE; }

            if (stopFlag_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        // 线程退出
        if (status_.state != PlayerState::Error && status_.state != PlayerState::Stopping)
            setStatus(PlayerState::Stopped, PlayerErrorCode::None, "播放线程已退出");
    }

    // ---- 成员变量 ----
    HWND        hwnd_;
    std::string url_;
    std::thread worker_;

    std::atomic<bool> stopFlag_{false};
    bool              running_      = false;
    bool              video_synced_ = false;
    int               reconnect_count_ = 0;

    Recorder          recorder_;
    std::atomic<bool> recordEnabled_{false};
    std::string       recordPath_ = "record.ts";
    std::mutex        recordMutex_;

    mutable std::mutex statusMutex_;
    PlayerStatus       status_;
};

// ============================================================
// 解析 SRT host（与原版相同）
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

#include "player_main.moc"   // 让 qmake/cmake-automoc 处理 Q_OBJECT

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    qRegisterMetaType<PlayerStatus>("PlayerStatus");

    if (argc < 2) {
        std::cout << "用法: player.exe <srt_url>\n";
        return 0;
    }

    std::string url            = argv[1];
    std::string push_ctrl_host = extract_host_from_srt_url(url);
    ControlClient ctrl(push_ctrl_host, 10090);

    // ---- 视频画面 widget ----
    QWidget surface;
    surface.setAttribute(Qt::WA_NativeWindow);
    surface.setAttribute(Qt::WA_PaintOnScreen, true);
    surface.setAttribute(Qt::WA_NoSystemBackground, true);
    surface.setAutoFillBackground(false);
    surface.setMinimumSize(640, 360);

    // ---- 容器布局 ----
    QWidget container;
    container.setStyleSheet("background:#111;");
    QVBoxLayout* mainLayout = new QVBoxLayout(&container);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 视频区占满，stretch=1
    mainLayout->addWidget(&surface, 1);

    // ---- 状态栏 ----
    QWidget* statusBar = new QWidget;
    statusBar->setFixedHeight(28);
    statusBar->setStyleSheet("background:#1a1a1a;");
    QHBoxLayout* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(10, 0, 10, 0);
    statusLayout->setSpacing(8);

    // 指示灯（小圆点，用 label + border-radius 模拟）
    QLabel* dotLabel = new QLabel;
    dotLabel->setFixedSize(10, 10);
    dotLabel->setStyleSheet("background:#888780; border-radius:5px;");

    QLabel* stateLabel_w = new QLabel("空闲");
    stateLabel_w->setStyleSheet("color:#b4b2a9; font-size:12px;");

    QLabel* msgLabel = new QLabel;
    msgLabel->setStyleSheet("color:#5f5e5a; font-size:11px;");
    msgLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    QLabel* reconLabel = new QLabel;
    reconLabel->setStyleSheet("color:#ef9f27; font-size:11px;");

    QLabel* audioLabel = new QLabel;
    audioLabel->setStyleSheet("color:#5f5e5a; font-size:11px;");

    statusLayout->addWidget(dotLabel);
    statusLayout->addWidget(stateLabel_w);
    statusLayout->addWidget(msgLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(audioLabel);
    statusLayout->addWidget(reconLabel);

    mainLayout->addWidget(statusBar, 0);

    // ---- 控制栏（紧凑） ----
    QWidget* ctrlBar = new QWidget;
    ctrlBar->setFixedHeight(36);
    ctrlBar->setStyleSheet("background:#161616;");
    QHBoxLayout* ctrlLayout = new QHBoxLayout(ctrlBar);
    ctrlLayout->setContentsMargins(10, 4, 10, 4);
    ctrlLayout->setSpacing(6);

    // 通用小按钮样式
    auto makeBtn = [](const QString& text, const QString& extra = "") -> QPushButton* {
        auto* btn = new QPushButton(text);
        btn->setFixedHeight(26);
        btn->setStyleSheet(
            "QPushButton {"
            "  padding: 0 10px;"
            "  font-size: 12px;"
            "  border: 0.5px solid rgba(255,255,255,0.15);"
            "  border-radius: 5px;"
            "  background: rgba(255,255,255,0.06);"
            "  color: #c2c0b6;"
            "}"
            "QPushButton:hover  { background: rgba(255,255,255,0.12); }"
            "QPushButton:pressed{ background: rgba(255,255,255,0.18); }"
            + extra);
        return btn;
    };

    QPushButton* pauseBtn  = makeBtn("暂停推流");
    QPushButton* recordBtn = makeBtn("开始录制",
        "QPushButton[recording=true] {"
        "  border-color: #e24b4a;"
        "  color: #f09595;"
        "}");

    ctrlLayout->addWidget(pauseBtn);
    ctrlLayout->addWidget(recordBtn);
    ctrlLayout->addStretch();

    mainLayout->addWidget(ctrlBar, 0);

    // ---- 播放器 ----
    HWND hwnd = reinterpret_cast<HWND>(surface.winId());
    FFmpegD3D11Player player(hwnd, url);

    // ---- 状态变化 → 更新 UI ----
    QObject::connect(&player, &FFmpegD3D11Player::statusChanged,
                     [&](const PlayerStatus& s) {
        // 指示灯颜色
        dotLabel->setStyleSheet(
            QString("background:%1; border-radius:5px;").arg(stateDotColor(s.state)));

        stateLabel_w->setText(::stateLabel(s.state));
        msgLabel->setText(s.message);

        // 重连计数
        reconLabel->setVisible(s.reconnect_count > 0 &&
                                s.state == PlayerState::Reconnecting);
        reconLabel->setText(QString("第%1次重连").arg(s.reconnect_count));

        // 音频
        audioLabel->setText(s.audio_available ? "有声" : "无声");
        audioLabel->setStyleSheet(
            s.audio_available ? "color:#1d9e75;font-size:11px;"
                              : "color:#5f5e5a;font-size:11px;");

        // 录制按钮文字 & 样式切换
        recordBtn->setText(s.recording ? "停止录制" : "开始录制");
        recordBtn->setProperty("recording", s.recording);
        recordBtn->style()->unpolish(recordBtn);
        recordBtn->style()->polish(recordBtn);

        // 暂停按钮同步
        bool paused = (s.state == PlayerState::PausedRemote);
        pauseBtn->setText(paused ? "恢复推流" : "暂停推流");
    });

    // ---- 暂停/恢复按钮逻辑 ----
    bool paused = false;
    QObject::connect(pauseBtn, &QPushButton::clicked, [&]() mutable {
        bool ok = paused ? ctrl.resume() : ctrl.pause();
        if (ok) {
            paused = !paused;
            // 同步状态机（仅标记 remote_paused，不影响播放线程）
            // 通过 setStatus 更新快照
        } else {
            msgLabel->setText("控制指令发送失败");
        }
    });

    // ---- 录制按钮逻辑 ----
    bool recording = false;
    QObject::connect(recordBtn, &QPushButton::clicked, [&]() mutable {
        if (!recording) {
            player.startRecord("C:/Users/dkw/Desktop/record.ts");
            recording = true;
        } else {
            player.stopRecord();
            recording = false;
        }
    });

    // ---- 启动 ----
    container.resize(1280, 800);
    container.show();

    if (!player.start()) {
        std::cerr << "Player start failed\n";
        return -1;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&player]() {
        player.stop();
    });

    int code = app.exec();
    player.stop();
    return code;
}