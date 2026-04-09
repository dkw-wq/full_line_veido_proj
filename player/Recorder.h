#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <string>
#include <vector>
#include <mutex>

class Recorder {
public:
    Recorder() = default;
    ~Recorder();

    bool start(AVFormatContext* inputFmtCtx,
               int videoStreamIndex,
               int audioStreamIndex,
               const std::string& outputPath);

    void stop();

    bool writePacket(const AVPacket* pkt);

    bool isRecording() const { return recording_; }

private:
    void cleanup();

private:
    AVFormatContext* inFmtCtx_ = nullptr;
    AVFormatContext* outFmtCtx_ = nullptr;

    std::vector<int> inToOutStreamMap_;
    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;

    bool recording_ = false;
    std::mutex mutex_;
};