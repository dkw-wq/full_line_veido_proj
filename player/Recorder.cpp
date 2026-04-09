#include "Recorder.h"

#include <iostream>

static void print_rec_error(const char* prefix, int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    std::cerr << prefix << ": " << buf << std::endl;
}

Recorder::~Recorder() {
    stop();
}

bool Recorder::start(AVFormatContext* inputFmtCtx,
                     int videoStreamIndex,
                     int audioStreamIndex,
                     const std::string& outputPath) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (recording_) {
        return true;
    }
    if (!inputFmtCtx) {
        std::cerr << "Recorder::start: inputFmtCtx is null" << std::endl;
        return false;
    }

    inFmtCtx_ = inputFmtCtx;
    videoStreamIndex_ = videoStreamIndex;
    audioStreamIndex_ = audioStreamIndex;

    int ret = avformat_alloc_output_context2(&outFmtCtx_, nullptr, nullptr, outputPath.c_str());
    if (ret < 0 || !outFmtCtx_) {
        print_rec_error("avformat_alloc_output_context2 failed", ret);
        cleanup();
        return false;
    }

    inToOutStreamMap_.assign(inFmtCtx_->nb_streams, -1);

    for (unsigned int i = 0; i < inFmtCtx_->nb_streams; ++i) {
        if (static_cast<int>(i) != videoStreamIndex_ &&
            static_cast<int>(i) != audioStreamIndex_) {
            continue;
        }

        AVStream* inStream = inFmtCtx_->streams[i];
        AVCodecParameters* inCodecPar = inStream->codecpar;
        if (!inCodecPar) {
            continue;
        }

        AVStream* outStream = avformat_new_stream(outFmtCtx_, nullptr);
        if (!outStream) {
            std::cerr << "avformat_new_stream failed" << std::endl;
            cleanup();
            return false;
        }

        ret = avcodec_parameters_copy(outStream->codecpar, inCodecPar);
        if (ret < 0) {
            print_rec_error("avcodec_parameters_copy failed", ret);
            cleanup();
            return false;
        }

        outStream->codecpar->codec_tag = 0;
        outStream->time_base = inStream->time_base;

        inToOutStreamMap_[i] = outStream->index;
    }

    if (!(outFmtCtx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outFmtCtx_->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            print_rec_error("avio_open failed", ret);
            cleanup();
            return false;
        }
    }

    ret = avformat_write_header(outFmtCtx_, nullptr);
    if (ret < 0) {
        print_rec_error("avformat_write_header failed", ret);
        cleanup();
        return false;
    }

    recording_ = true;
    std::cout << "Recorder started: " << outputPath << std::endl;
    return true;
}

bool Recorder::writePacket(const AVPacket* pkt) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!recording_ || !outFmtCtx_ || !inFmtCtx_ || !pkt) {
        return false;
    }

    if (pkt->stream_index < 0 || pkt->stream_index >= static_cast<int>(inToOutStreamMap_.size())) {
        return false;
    }

    int outStreamIndex = inToOutStreamMap_[pkt->stream_index];
    if (outStreamIndex < 0) {
        return false;
    }

    AVStream* inStream = inFmtCtx_->streams[pkt->stream_index];
    AVStream* outStream = outFmtCtx_->streams[outStreamIndex];
    if (!inStream || !outStream) {
        return false;
    }

    AVPacket outPkt;
    av_init_packet(&outPkt);

    int ret = av_packet_ref(&outPkt, pkt);
    if (ret < 0) {
        print_rec_error("av_packet_ref failed", ret);
        return false;
    }

    outPkt.stream_index = outStreamIndex;

    av_packet_rescale_ts(&outPkt, inStream->time_base, outStream->time_base);

    ret = av_interleaved_write_frame(outFmtCtx_, &outPkt);
    av_packet_unref(&outPkt);

    if (ret < 0) {
        print_rec_error("av_interleaved_write_frame failed", ret);
        return false;
    }

    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!recording_) {
        cleanup();
        return;
    }

    if (outFmtCtx_) {
        av_write_trailer(outFmtCtx_);
    }

    cleanup();
    std::cout << "Recorder stopped" << std::endl;
}

void Recorder::cleanup() {
    recording_ = false;

    if (outFmtCtx_) {
        if (!(outFmtCtx_->oformat->flags & AVFMT_NOFILE) && outFmtCtx_->pb) {
            avio_closep(&outFmtCtx_->pb);
        }
        avformat_free_context(outFmtCtx_);
        outFmtCtx_ = nullptr;
    }

    inFmtCtx_ = nullptr;
    inToOutStreamMap_.clear();
    videoStreamIndex_ = -1;
    audioStreamIndex_ = -1;
}