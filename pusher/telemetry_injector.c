#include "telemetry_injector.h"

#include "telemetry.h"

#include <string.h>

static GstBuffer *append_tlmy_ts_packet(GstBuffer *buf, TelemetryInjectCtx *ctx) {
    guint8 pkt[188];
    tlmy_ts_build(pkt, wall_us(), ctx->seq++, ctx->cc);
    ctx->cc = (guint8)((ctx->cc + 1) & 0x0F);

    GstBuffer *tail = gst_buffer_new_allocate(NULL, 188, NULL);
    if (!tail) return buf;

    GstMapInfo map;
    if (!gst_buffer_map(tail, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(tail);
        return buf;
    }

    memcpy(map.data, pkt, 188);
    gst_buffer_unmap(tail, &map);

    return gst_buffer_append(buf, tail);
}

static GstPadProbeReturn tlmy_inject_probe_cb(GstPad *pad,
                                              GstPadProbeInfo *info,
                                              gpointer user_data) {
    (void)pad;

    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    TelemetryInjectCtx *ctx = (TelemetryInjectCtx *)user_data;
    gsize sz = gst_buffer_get_size(buf);
    if (sz < 188 || (sz % 188) != 0) {
        return GST_PAD_PROBE_OK;
    }

    guint64 start_pkt = ctx->media_ts_packets + 1;
    guint64 end_pkt   = ctx->media_ts_packets + (guint64)(sz / 188);
    ctx->media_ts_packets = end_pkt;

    guint inject_count = 0;
    while (ctx->next_inject_at != 0 && ctx->next_inject_at >= start_pkt && ctx->next_inject_at <= end_pkt) {
        ++inject_count;
        ctx->next_inject_at += TLMY_TS_INTERVAL;
    }

    if (inject_count == 0) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer *out = gst_buffer_make_writable(buf);
    for (guint i = 0; i < inject_count; ++i) {
        out = append_tlmy_ts_packet(out, ctx);
    }

    GST_PAD_PROBE_INFO_DATA(info) = out;
    return GST_PAD_PROBE_OK;
}

gboolean install_tlmy_injector(GstElement *pipeline, TelemetryInjectCtx *ctx) {
    GstElement *ident = gst_bin_get_by_name(GST_BIN(pipeline), "tlmyinject");
    if (!ident) {
        g_printerr("[TLMY] identity element 'tlmyinject' not found\n");
        return FALSE;
    }

    GstPad *src = gst_element_get_static_pad(ident, "src");
    gst_object_unref(ident);
    if (!src) {
        g_printerr("[TLMY] failed to get tlmyinject:src\n");
        return FALSE;
    }

    gst_pad_add_probe(src,
                      GST_PAD_PROBE_TYPE_BUFFER,
                      tlmy_inject_probe_cb,
                      ctx,
                      NULL);
    gst_object_unref(src);
    g_print("[TLMY] TS injector installed on tlmyinject:src\n");
    return TRUE;
}
