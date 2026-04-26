#pragma once

#include "D3D11Renderer.h"
#include "FFmpegUtil.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/avutil.h>
}
#include <libavutil/hwcontext_d3d11va.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

class VideoPipeline {
public:
    ~VideoPipeline() { cleanup(); }

    bool init(AVFormatContext* fmt_ctx, int stream_index, const AVCodec* dec) {
        if (stream_index < 0 || !dec) return false;

        stream_index_ = stream_index;
        stream_       = fmt_ctx->streams[stream_index_];
        codec_ctx_    = avcodec_alloc_context3(dec);
        if (!codec_ctx_) return false;

        if (avcodec_parameters_to_context(codec_ctx_, stream_->codecpar) < 0) {
            cleanup();
            return false;
        }

        codec_ctx_->thread_count = 1;
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

        // 关键：把实例指针挂到 opaque，供 get_format 静态回调取回
        codec_ctx_->opaque = this;

        if (!initHwDevice(codec_ctx_, "d3d11va")) {
            cleanup();
            return false;
        }

        if (avcodec_open2(codec_ctx_, dec, nullptr) < 0) {
            cleanup();
            return false;
        }

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
                    width_ = frame_->width;
                    height_ = frame_->height;
                } else {
                    renderer_failed_out = true;
                    stop_flag.store(true);
                    av_frame_unref(frame_);
                    break;
                }
            }

            if (frame_->format != hw_pix_fmt_) {
                if (!warned_sw_) {
                    std::cerr << "video: non-HW frame skipped\n";
                    warned_sw_ = true;
                }
                av_frame_unref(frame_);
                continue;
            }

            const double vpts = frame_->best_effort_timestamp * av_q2d(time_base_);
            if (audio_clock >= 0.0) {
                const double diff = vpts - audio_clock;
                if      (diff >  0.12) std::this_thread::sleep_for(std::chrono::milliseconds((int)((diff - 0.04) * 1000)));
                else if (diff < -0.15) { av_frame_unref(frame_); continue; }
            }

            renderer_.presentFrame(frame_, width_, height_);
            av_frame_unref(frame_);
        }

        return true;
    }

    int streamIndex() const { return stream_index_; }
    AVRational timeBase() const { return time_base_; }

    AVD3D11VADeviceContext* d3d11DeviceContext() const {
        if (!hw_device_ctx_) return nullptr;

        auto* hwdev = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data);
        if (!hwdev || !hwdev->hwctx) return nullptr;

        return reinterpret_cast<AVD3D11VADeviceContext*>(hwdev->hwctx);
    }

    AVPixelFormat hwPixelFormat() const {
        return hw_pix_fmt_;
    }

    void cleanup() {
        if (frame_) {
            av_frame_free(&frame_);
            frame_ = nullptr;
        }

        if (codec_ctx_) {
            codec_ctx_->opaque = nullptr;
            if (codec_ctx_->hw_device_ctx) {
                av_buffer_unref(&codec_ctx_->hw_device_ctx);
            }
            avcodec_free_context(&codec_ctx_);
        }

        if (hw_device_ctx_) {
            av_buffer_unref(&hw_device_ctx_);
        }

        renderer_.shutdown();
        stream_ = nullptr;
        stream_index_ = -1;
        renderer_ready_ = false;
        warned_sw_ = false;
        width_ = 0;
        height_ = 0;
        time_base_ = AVRational{1, 1};
        hw_pix_fmt_ = AV_PIX_FMT_NONE;
    }

private:
    static enum AVPixelFormat hw_get_format(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
        if (!ctx || !ctx->opaque) {
            std::cerr << "HW get_format: missing pipeline instance\n";
            return AV_PIX_FMT_NONE;
        }

        auto* self = reinterpret_cast<VideoPipeline*>(ctx->opaque);
        for (const auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == self->hw_pix_fmt_) return *p;
        }

        std::cerr << "HW surface format not found\n";
        return AV_PIX_FMT_NONE;
    }

    bool initHwDevice(AVCodecContext* ctx, const char* device) {
        hw_pix_fmt_ = AV_PIX_FMT_NONE;

        AVHWDeviceType type = av_hwdevice_find_type_by_name(device);
        if (type == AV_HWDEVICE_TYPE_NONE) return false;

        const AVCodecHWConfig* cfg = nullptr;
        for (int i = 0; (cfg = avcodec_get_hw_config(ctx->codec, i)); ++i) {
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                cfg->device_type == type) {
                hw_pix_fmt_ = cfg->pix_fmt;
                break;
            }
        }

        if (hw_pix_fmt_ == AV_PIX_FMT_NONE) return false;

        int r = av_hwdevice_ctx_create(&hw_device_ctx_, type, nullptr, nullptr, 0);
        if (r < 0) {
            print_ffmpeg_error("av_hwdevice_ctx_create", r);
            return false;
        }

        ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        if (!ctx->hw_device_ctx) {
            std::cerr << "av_buffer_ref(hw_device_ctx_) failed\n";
            return false;
        }

        ctx->get_format = &VideoPipeline::hw_get_format;
        return true;
    }

private:
    AVCodecContext* codec_ctx_      = nullptr;
    AVStream*       stream_         = nullptr;
    int             stream_index_   = -1;
    AVFrame*        frame_          = nullptr;

    AVBufferRef*    hw_device_ctx_  = nullptr;
    AVPixelFormat   hw_pix_fmt_     = AV_PIX_FMT_NONE;

    D3D11Renderer   renderer_;
    bool            renderer_ready_ = false;
    bool            warned_sw_      = false;
    int             width_          = 0;
    int             height_         = 0;
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