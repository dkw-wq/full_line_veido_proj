#include "app_config.h"

const AppConfig kAppConfig = {
    .device_path = "/dev/v4l/by-id/usb-046d_0825_1ED9E9C0-video-index0",
    .host = "10.158.134.17",
    .port = 9000,
    .width = 1280,
    .height = 720,
    .fps = 25,
    .bitrate_kbps = 2000,
    .gop = 50,
    .profile = "baseline",
    .srt_latency = 20,
    .audio_enabled = TRUE,
    .audio_device = NULL,
    .audio_bitrate_kbps = 128,
    .audio_codec = "aac",
    .ctrl_port = 10090
};
