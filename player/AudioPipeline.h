#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#include <libavutil/channel_layout.h>

#include <QAudioFormat>
#include <QIODevice>
#include <QAudioSink>
#include <QMediaDevices>

#include <algorithm>
#include <cstdint>
#include <vector>

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
