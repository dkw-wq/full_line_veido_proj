#pragma once

#include <gst/gst.h>

typedef struct {
    guint64 media_ts_packets;
    guint64 next_inject_at;
    guint64 seq;
    guint8  cc;
} TelemetryInjectCtx;

gboolean install_tlmy_injector(GstElement *pipeline, TelemetryInjectCtx *ctx);