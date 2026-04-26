#pragma once

#include <glib.h>

typedef struct {
    const gchar *device_path;
    const gchar *host;
    gint port;
    gint width;
    gint height;
    gint fps;
    gint bitrate_kbps;
    gint gop;
    const gchar *profile;
    gint srt_latency;
    gboolean audio_enabled;
    const gchar *audio_device;
    gint audio_bitrate_kbps;
    const gchar *audio_codec;
    gint ctrl_port;
} AppConfig;

extern const AppConfig kAppConfig;