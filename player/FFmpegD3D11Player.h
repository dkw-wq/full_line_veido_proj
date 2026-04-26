#pragma once

#include "AudioPipeline.h"
#include "FFmpegUtil.h"
#include "PlayerStatus.h"
#include "Recorder.h"
#include "VideoPipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <QMetaObject>
#include <QObject>
#include <QString>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

class FFmpegD3D11Player : public QObject {
    Q_OBJECT

public:
    enum class LifecycleState {
        Stopped,
        Starting,
        Started,
        Stopping
    };

public:
    explicit FFmpegD3D11Player(HWND hwnd,
                            std::string url,
                            QObject* parent = nullptr)
        : QObject(parent), hwnd_(hwnd), url_(std::move(url)) {
        status_.url = QString::fromStdString(url_);
    }

    ~FFmpegD3D11Player() override { stop(); }

    bool start() {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);

        if (lifecycleState_ != LifecycleState::Stopped) {
            return true;
        }

        stopFlag_.store(false);
        lifecycleState_ = LifecycleState::Starting;

        mutateStatus([&](PlayerStatus& s) {
            s.state   = PlayerState::Starting;
            s.error   = PlayerErrorCode::None;
            s.message = "线程启动中";
        });

        try {
            worker_ = std::thread(&FFmpegD3D11Player::run, this);
            lifecycleState_ = LifecycleState::Started;
        } catch (...) {
            lifecycleState_ = LifecycleState::Stopped;
            mutateStatus([&](PlayerStatus& s) {
                s.state   = PlayerState::Error;
                s.error   = PlayerErrorCode::DecoderFailed;
                s.message = "播放线程创建失败";
            });
            return false;
        }

        return true;
    }

    void stop() {
        std::thread worker_to_join;
        bool should_set_stopped = false;

        {
            std::lock_guard<std::mutex> lk(lifecycleMutex_);

            if (lifecycleState_ == LifecycleState::Stopped ||
                lifecycleState_ == LifecycleState::Stopping) {
                return;
            }

            lifecycleState_ = LifecycleState::Stopping;

            mutateStatus([&](PlayerStatus& s) {
                s.state   = PlayerState::Stopping;
                s.message = "正在停止";
            });

            stopFlag_.store(true);

            if (worker_.joinable()) {
                worker_to_join = std::move(worker_);
            }
        }

        if (worker_to_join.joinable()) {
            worker_to_join.join();
        }

        {
            std::lock_guard<std::mutex> lk(lifecycleMutex_);
            lifecycleState_ = LifecycleState::Stopped;
        }

        {
            std::lock_guard<std::mutex> lk(statusMutex_);
            should_set_stopped = (status_.state != PlayerState::Error);
        }

        if (should_set_stopped) {
            mutateStatus([&](PlayerStatus& s) {
                s.state   = PlayerState::Stopped;
                s.message = "已停止";
            });
        }
    }

    LifecycleState lifecycleState() const {
        std::lock_guard<std::mutex> lk(lifecycleMutex_);
        return lifecycleState_;
    }

    // ── 录制控制：只改意图字段，播放线程负责落地 ──────────────
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
    void setRemotePaused(bool paused) {
        mutateStatus([paused](PlayerStatus& s) {
            s.remote_paused = paused;
        });
    }

    PlayerStatus currentStatus() const {
        std::lock_guard<std::mutex> lk(statusMutex_);
        return status_;
    }

signals:
    void statusChanged(const PlayerStatus& status);

private:
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

    void run() {
        while (!stopFlag_.load()) {
            video_synced_ = false;

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

            setPlaybackStatus(PlayerState::Initializing, PlayerErrorCode::None,
                              "初始化解码器...", audio_ok, true);

            auto* d3d11ctx = video.d3d11DeviceContext();
            if (!d3d11ctx || !d3d11ctx->device || !d3d11ctx->device_context) {
                video.cleanup();
                audio.cleanup();
                avformat_close_input(&fmt_ctx);
                if (stopFlag_.load()) break;
                ++reconnect_count_;
                setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::D3D11ContextFailed,
                                  QString("D3D11 设备获取失败  (第%1次重连)").arg(reconnect_count_));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }

            AVPacket* pkt = av_packet_alloc();
            if (!pkt) {
                video.cleanup();
                audio.cleanup();
                avformat_close_input(&fmt_ctx);
                setPlaybackStatus(PlayerState::Error, PlayerErrorCode::PacketAllocFailed,
                                  "av_packet_alloc 失败");
                break;
            }

            reconnect_count_ = 0;
            setPlaybackStatus(PlayerState::WaitingKeyframe, PlayerErrorCode::None, "等待关键帧...");

            auto last_pkt_time = std::chrono::steady_clock::now();

            while (!stopFlag_.load()) {
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

                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_pkt_time).count() > 5000) {
                    if (stopFlag_.load()) break;
                    ++reconnect_count_;
                    setPlaybackStatus(PlayerState::Reconnecting, PlayerErrorCode::ReadFrameFailed,
                                      QString("5秒无数据,重连  (第%1次)").arg(reconnect_count_));
                    break;
                }
            }

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

            if (stopFlag_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        //setPlaybackStatus(PlayerState::Stopped, PlayerErrorCode::None, "播放线程已退出");
    }

private:
    HWND        hwnd_;
    std::string url_;
    std::thread worker_;

    mutable std::mutex lifecycleMutex_;
    std::atomic<bool>  stopFlag_{false};
    LifecycleState     lifecycleState_ = LifecycleState::Stopped;

    bool              video_synced_    = false;
    int               reconnect_count_ = 0;

    Recorder          recorder_;
    std::string       recordPath_  = "record.ts";
    std::mutex        recordPathMutex_;

    mutable std::mutex statusMutex_;
    PlayerStatus       status_;
};
