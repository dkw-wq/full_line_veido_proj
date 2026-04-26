#pragma once

extern "C" {
#include <libavutil/avutil.h>
}

#include <atomic>
#include <iostream>

static void print_ffmpeg_error(const char* prefix, int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, buf, sizeof(buf));
    std::cerr << prefix << ": " << buf << "\n";
}

static int interrupt_cb(void* opaque) {
    return reinterpret_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}