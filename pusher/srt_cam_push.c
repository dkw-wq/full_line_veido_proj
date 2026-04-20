//pusher/srt_cam_push.c

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <gst/video/video.h>
#include <sys/stat.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>

#include "telemetry.h"

typedef struct {
    const gchar *device_path;
    const gchar *host;
    gint port;
    gint width;
    gint height;
    gint fps;
    gint bitrate_kbps;
    gint gop;
    const gchar *profile;   // baseline / main
    gint srt_latency;       // ms
    gboolean audio_enabled;
    const gchar *audio_device;   // preferred capture keyword; NULL means no preference
    gint audio_bitrate_kbps;
    const gchar *audio_codec; // aac | opus
    gint ctrl_port;           // TCP control port for pause/resume
} AppConfig;

typedef struct {
    guint64 media_ts_packets;
    guint64 next_inject_at;
    guint64 seq;
    guint8  cc;
} TelemetryInjectCtx;

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

static gboolean install_tlmy_injector(GstElement *pipeline, TelemetryInjectCtx *ctx) {
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

static const AppConfig kAppConfig = {
    .device_path = "/dev/v4l/by-id/usb-046d_0825_1ED9E9C0-video-index0",
    .host = "10.160.196.17",
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


typedef struct {
    gchar *device;
    gchar *card_id;
    gchar *card_name;
    gchar *pcm_name;
    gint card_index;
    gint device_index;
} AudioInputCandidate;

static void audio_input_candidate_free(AudioInputCandidate *dev) {
    if (!dev) return;
    g_free(dev->device);
    g_free(dev->card_id);
    g_free(dev->card_name);
    g_free(dev->pcm_name);
    g_free(dev);
}

static gboolean ascii_contains_ci(const gchar *haystack, const gchar *needle) {
    if (!haystack || !needle || !*needle) {
        return FALSE;
    }

    gchar *haystack_lower = g_ascii_strdown(haystack, -1);
    gchar *needle_lower = g_ascii_strdown(needle, -1);
    gboolean matched = (strstr(haystack_lower, needle_lower) != NULL);

    g_free(haystack_lower);
    g_free(needle_lower);
    return matched;
}

static GPtrArray *enumerate_alsa_capture_devices(void) {
    GPtrArray *devices = g_ptr_array_new_with_free_func((GDestroyNotify)audio_input_candidate_free);
    int card = -1;

    if (snd_card_next(&card) < 0) {
        g_printerr("[AUDIO] snd_card_next failed while enumerating capture devices\n");
        return devices;
    }

    while (card >= 0) {
        char ctl_name[32];
        snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);

        snd_ctl_t *ctl = NULL;
        if (snd_ctl_open(&ctl, ctl_name, 0) >= 0) {
            snd_ctl_card_info_t *card_info = NULL;
            snd_ctl_card_info_alloca(&card_info);

            if (snd_ctl_card_info(ctl, card_info) >= 0) {
                const char *card_id = snd_ctl_card_info_get_id(card_info);
                const char *card_name = snd_ctl_card_info_get_name(card_info);

                int dev = -1;
                while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
                    snd_pcm_info_t *pcm_info = NULL;
                    snd_pcm_info_alloca(&pcm_info);
                    snd_pcm_info_set_device(pcm_info, (unsigned int)dev);
                    snd_pcm_info_set_subdevice(pcm_info, 0);
                    snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);

                    if (snd_ctl_pcm_info(ctl, pcm_info) >= 0) {
                        AudioInputCandidate *candidate = g_new0(AudioInputCandidate, 1);
                        if (!candidate) {
                            continue;
                        }

                        candidate->card_index = card;
                        candidate->device_index = dev;
                        candidate->card_id = g_strdup(card_id ? card_id : "");
                        candidate->card_name = g_strdup(card_name ? card_name : "");
                        candidate->pcm_name = g_strdup(snd_pcm_info_get_name(pcm_info));

                        if (card_id && *card_id) {
                            candidate->device = g_strdup_printf("plughw:CARD=%s,DEV=%d", card_id, dev);
                        } else {
                            candidate->device = g_strdup_printf("plughw:%d,%d", card, dev);
                        }

                        g_ptr_array_add(devices, candidate);
                    }
                }
            }

            snd_ctl_close(ctl);
        }

        if (snd_card_next(&card) < 0) {
            break;
        }
    }

    return devices;
}

static gchar *resolve_audio_device(const gchar *preferred_keyword) {
    GPtrArray *devices = enumerate_alsa_capture_devices();
    gchar *selected = NULL;
    const gchar *reason = NULL;

    g_print("[AUDIO] preferred keyword: %s\n",
            (preferred_keyword && *preferred_keyword) ? preferred_keyword : "(none)");

    for (guint i = 0; i < devices->len; ++i) {
        AudioInputCandidate *dev = g_ptr_array_index(devices, i);
        g_print("[AUDIO] capture[%u]: device=%s card_id=%s card_name=%s pcm_name=%s\n",
                i,
                dev->device ? dev->device : "(null)",
                dev->card_id ? dev->card_id : "",
                dev->card_name ? dev->card_name : "",
                dev->pcm_name ? dev->pcm_name : "");
    }

    if (preferred_keyword && *preferred_keyword) {
        for (guint i = 0; i < devices->len; ++i) {
            AudioInputCandidate *dev = g_ptr_array_index(devices, i);
            if (ascii_contains_ci(dev->device, preferred_keyword) ||
                ascii_contains_ci(dev->card_id, preferred_keyword) ||
                ascii_contains_ci(dev->card_name, preferred_keyword) ||
                ascii_contains_ci(dev->pcm_name, preferred_keyword)) {
                selected = g_strdup(dev->device);
                reason = "matched preferred keyword";
                break;
            }
        }
    }

    if (!selected && devices->len == 1) {
        AudioInputCandidate *dev = g_ptr_array_index(devices, 0);
        selected = g_strdup(dev->device);
        reason = "only one capture device detected";
    }

    if (!selected) {
        selected = g_strdup("default");
        reason = "fallback to default";
    }

    g_print("[AUDIO] selected capture device: %s (%s)\n", selected, reason);
    g_ptr_array_unref(devices);
    return selected;
}

static gboolean device_exists(const gchar *path) {
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0);
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("EOS received, stopping...\n");
            g_main_loop_quit(loop);
            break;

        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("ERROR from %s: %s\n",
                       GST_OBJECT_NAME(msg->src),
                       err ? err->message : "unknown");
            if (debug) {
                g_printerr("Debug details: %s\n", debug);
            }
            g_clear_error(&err);
            g_free(debug);
            g_main_loop_quit(loop);
            break;
        }

        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_warning(msg, &err, &debug);
            g_printerr("WARNING from %s: %s\n",
                       GST_OBJECT_NAME(msg->src),
                       err ? err->message : "unknown");
            if (debug) {
                g_printerr("Debug details: %s\n", debug);
            }
            g_clear_error(&err);
            g_free(debug);
            break;
        }

        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) != NULL &&
                GST_IS_PIPELINE(GST_MESSAGE_SRC(msg))) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
                g_print("Pipeline state changed: %s -> %s\n",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));
            }
            break;

        default:
            break;
    }

    return TRUE;
}

// -------- Control server: accepts PAUSE/RESUME over TCP --------
typedef enum {
    CTRL_CMD_NONE = 0,
    CTRL_CMD_PAUSE,
    CTRL_CMD_RESUME,
    CTRL_CMD_IDR,
    CTRL_CMD_SET_BITRATE,
    CTRL_CMD_SET_FPS,
    CTRL_CMD_SET_RESOLUTION,
    CTRL_CMD_VIDEO_MODE,
    CTRL_CMD_AUDIO_MODE,
} ControlCommand;

typedef enum {
    CTRL_MODE_NONE = 0,
    CTRL_MODE_NORMAL,
    CTRL_MODE_BLACK,
    CTRL_MODE_SILENT,
} ControlMode;

typedef struct {
    ControlCommand cmd;
    gint bitrate_kbps;
    gint fps;
    gint width;
    gint height;
    ControlMode mode;
} ControlCommandSpec;

typedef struct ControlServer ControlServer;

typedef struct {
    ControlServer *cs;
    ControlCommandSpec spec;
    GMutex lock;
    GCond cond;
    gboolean done;
    gboolean ok;
    atomic_int ref_count;
} ControlRequest;

struct ControlServer {
    GstElement *pipeline;
    GstElement *vsel;
    GstElement *asel;

    GstElement *venc;    // x264enc
    GstElement *vparse;  // h264parse
    GstElement *vcaps;   // live camera raw caps
    GstElement *blackcaps;

    GMainContext *main_context;

    gint ctrl_port;
    pthread_t thread;
    int listen_fd;
    atomic_bool stop;

    atomic_llong last_resume_monotonic_us; // latest RESUME time, monotonic us
    atomic_int current_width;
    atomic_int current_height;
    atomic_int current_fps;
    atomic_int current_bitrate_kbps;
};

static ControlRequest *control_request_new(ControlServer *cs, ControlCommandSpec spec) {
    ControlRequest *req = g_new0(ControlRequest, 1);
    if (!req) return NULL;

    req->cs = cs;
    req->spec = spec;
    g_mutex_init(&req->lock);
    g_cond_init(&req->cond);
    atomic_init(&req->ref_count, 1);
    return req;
}

static void control_request_ref(ControlRequest *req) {
    if (!req) return;
    atomic_fetch_add_explicit(&req->ref_count, 1, memory_order_relaxed);
}

static void control_request_unref(ControlRequest *req) {
    if (!req) return;

    if (atomic_fetch_sub_explicit(&req->ref_count, 1, memory_order_acq_rel) == 1) {
        g_cond_clear(&req->cond);
        g_mutex_clear(&req->lock);
        g_free(req);
    }
}

static gboolean selector_set_active_pad(GstElement *selector,
                                        const gchar *pad_name,
                                        const gchar *selector_name) {
    if (!selector) return TRUE;

    GstPad *pad = gst_element_get_static_pad(selector, pad_name);
    if (!pad) {
        g_printerr("[CTRL] failed to get %s:%s\n", selector_name, pad_name);
        return FALSE;
    }

    g_object_set(selector, "active-pad", pad, NULL);
    gst_object_unref(pad);
    g_print("[CTRL] %s switched to %s\n", selector_name, pad_name);
    return TRUE;
}

static gboolean send_upstream_force_key_unit(GstElement *vparse,
                                              atomic_llong *last_resume_monotonic_us) {
    if (!vparse) {
        g_printerr("[CTRL] vparse is NULL\n");
        return FALSE;
    }

    GstPad *src = gst_element_get_static_pad(vparse, "src");
    if (!src) {
        g_printerr("[CTRL] failed to get vparse:src\n");
        return FALSE;
    }

    atomic_store_explicit(last_resume_monotonic_us,
                          (long long)g_get_monotonic_time(),
                          memory_order_relaxed);

    GstEvent *force_kf = gst_video_event_new_upstream_force_key_unit(
        GST_CLOCK_TIME_NONE,
        TRUE,
        0
    );

    gboolean ok = gst_pad_send_event(src, force_kf);
    g_print("[CTRL] upstream force-key-unit sent from vparse:src result=%d\n", ok);

    gst_object_unref(src);
    return ok;
}

static gboolean set_video_caps_filter(GstElement *filter,
                                       const gchar *name,
                                       gint width,
                                       gint height,
                                       gint fps) {
    if (!filter) {
        g_printerr("[CTRL] capsfilter %s is NULL\n", name);
        return FALSE;
    }

    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "width", G_TYPE_INT, width,
        "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, fps, 1,
        NULL
    );
    if (!caps) {
        g_printerr("[CTRL] failed to allocate caps for %s\n", name);
        return FALSE;
    }

    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_print("[CTRL] %s caps set to %dx%d@%d\n", name, width, height, fps);
    return TRUE;
}

static gboolean apply_current_video_caps(ControlServer *cs) {
    gint width = atomic_load_explicit(&cs->current_width, memory_order_relaxed);
    gint height = atomic_load_explicit(&cs->current_height, memory_order_relaxed);
    gint fps = atomic_load_explicit(&cs->current_fps, memory_order_relaxed);

    gboolean ok = TRUE;
    ok &= set_video_caps_filter(cs->vcaps, "vcaps", width, height, fps);
    ok &= set_video_caps_filter(cs->blackcaps, "blackcaps", width, height, fps);
    return ok;
}

static gboolean set_encoder_bitrate(ControlServer *cs, gint bitrate_kbps) {
    if (!cs->venc || bitrate_kbps <= 0) {
        g_printerr("[CTRL] invalid bitrate request: %d\n", bitrate_kbps);
        return FALSE;
    }

    g_object_set(cs->venc, "bitrate", bitrate_kbps, NULL);
    atomic_store_explicit(&cs->current_bitrate_kbps, bitrate_kbps, memory_order_relaxed);
    g_print("[CTRL] encoder bitrate set to %d kbps\n", bitrate_kbps);
    return TRUE;
}

static gboolean set_video_mode(ControlServer *cs, ControlMode mode) {
    if (mode == CTRL_MODE_BLACK) {
        return selector_set_active_pad(cs->vsel, "sink_1", "vsel");
    }
    if (mode == CTRL_MODE_NORMAL) {
        return selector_set_active_pad(cs->vsel, "sink_0", "vsel");
    }
    return FALSE;
}

static gboolean set_audio_mode(ControlServer *cs, ControlMode mode) {
    if (mode == CTRL_MODE_SILENT) {
        return selector_set_active_pad(cs->asel, "sink_1", "asel");
    }
    if (mode == CTRL_MODE_NORMAL) {
        return selector_set_active_pad(cs->asel, "sink_0", "asel");
    }
    return FALSE;
}

static gboolean apply_control_command_in_main(ControlServer *cs, const ControlCommandSpec *spec) {
    gboolean ok = TRUE;

    switch (spec->cmd) {
        case CTRL_CMD_PAUSE:
            ok &= set_video_mode(cs, CTRL_MODE_BLACK);
            ok &= set_audio_mode(cs, CTRL_MODE_SILENT);
            break;

        case CTRL_CMD_RESUME:
            ok &= set_video_mode(cs, CTRL_MODE_NORMAL);
            ok &= send_upstream_force_key_unit(cs->vparse, &cs->last_resume_monotonic_us);
            ok &= set_audio_mode(cs, CTRL_MODE_NORMAL);
            break;

        case CTRL_CMD_IDR:
            ok &= send_upstream_force_key_unit(cs->vparse, &cs->last_resume_monotonic_us);
            break;

        case CTRL_CMD_SET_BITRATE:
            ok &= set_encoder_bitrate(cs, spec->bitrate_kbps);
            break;

        case CTRL_CMD_SET_FPS:
            if (spec->fps <= 0) {
                ok = FALSE;
                break;
            }
            atomic_store_explicit(&cs->current_fps, spec->fps, memory_order_relaxed);
            ok &= apply_current_video_caps(cs);
            break;

        case CTRL_CMD_SET_RESOLUTION:
            if (spec->width <= 0 || spec->height <= 0) {
                ok = FALSE;
                break;
            }
            atomic_store_explicit(&cs->current_width, spec->width, memory_order_relaxed);
            atomic_store_explicit(&cs->current_height, spec->height, memory_order_relaxed);
            ok &= apply_current_video_caps(cs);
            break;

        case CTRL_CMD_VIDEO_MODE:
            ok &= set_video_mode(cs, spec->mode);
            break;

        case CTRL_CMD_AUDIO_MODE:
            ok &= set_audio_mode(cs, spec->mode);
            break;

        default:
            ok = FALSE;
            break;
    }

    return ok;
}

static gboolean control_apply_in_main(gpointer user_data) {
    ControlRequest *req = (ControlRequest *)user_data;
    gboolean ok = apply_control_command_in_main(req->cs, &req->spec);

    g_mutex_lock(&req->lock);
    req->ok = ok;
    req->done = TRUE;
    g_cond_signal(&req->cond);
    g_mutex_unlock(&req->lock);

    return G_SOURCE_REMOVE;
}

static gboolean control_execute_sync(ControlServer *cs,
                                     ControlCommandSpec spec,
                                     gint timeout_ms) {
    if (!cs || !cs->main_context) {
        g_printerr("[CTRL] main context is not available\n");
        return FALSE;
    }

    ControlRequest *req = control_request_new(cs, spec);
    if (!req) {
        g_printerr("[CTRL] failed to allocate control request\n");
        return FALSE;
    }

    control_request_ref(req); // ownership transferred to the main-context callback
    g_main_context_invoke_full(cs->main_context,
                               G_PRIORITY_DEFAULT,
                               control_apply_in_main,
                               req,
                               (GDestroyNotify)control_request_unref);

    gboolean completed = FALSE;
    gboolean ok = FALSE;
    gint64 deadline_us = g_get_monotonic_time() + ((gint64)timeout_ms * 1000);

    g_mutex_lock(&req->lock);
    while (!req->done) {
        if (!g_cond_wait_until(&req->cond, &req->lock, deadline_us)) {
            break;
        }
    }
    completed = req->done;
    ok = req->ok;
    g_mutex_unlock(&req->lock);

    if (!completed) {
        g_printerr("[CTRL] command timed out waiting for main loop\n");
    }

    control_request_unref(req);
    return completed && ok;
}

static gboolean is_force_key_unit_event(GstEvent *ev) {
    if (!ev) return FALSE;
    return gst_video_event_is_force_key_unit(ev);
}

static GstPadProbeReturn event_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)user_data;

    if (!(GST_PAD_PROBE_INFO_TYPE(info) &
          (GST_PAD_PROBE_TYPE_EVENT_UPSTREAM | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM))) {
        return GST_PAD_PROBE_OK;
    }

    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
    if (!ev || !is_force_key_unit_event(ev)) {
        return GST_PAD_PROBE_OK;
    }

    GstElement *parent = gst_pad_get_parent_element(pad);
    const gchar *parent_name = parent ? GST_OBJECT_NAME(parent) : "?";
    const gchar *pad_name = GST_PAD_NAME(pad);
    GstEventType type = GST_EVENT_TYPE(ev);

    if (type == GST_EVENT_CUSTOM_UPSTREAM) {
        GstClockTime running_time = GST_CLOCK_TIME_NONE;
        gboolean all_headers = FALSE;
        guint count = 0;

        if (gst_video_event_parse_upstream_force_key_unit(
                ev, &running_time, &all_headers, &count)) {
            g_print("[FKU][EVENT][UP] elem=%s pad=%s running=%" GST_TIME_FORMAT
                    " all_headers=%d count=%u\n",
                    parent_name,
                    pad_name,
                    GST_TIME_ARGS(running_time),
                    all_headers,
                    count);
        } else {
            g_print("[FKU][EVENT][UP] elem=%s pad=%s parse_failed\n",
                    parent_name,
                    pad_name);
        }
    } else if (type == GST_EVENT_CUSTOM_DOWNSTREAM ||
               type == GST_EVENT_CUSTOM_DOWNSTREAM_OOB) {

        GstClockTime timestamp = GST_CLOCK_TIME_NONE;
        GstClockTime stream_time = GST_CLOCK_TIME_NONE;
        GstClockTime running_time = GST_CLOCK_TIME_NONE;
        gboolean all_headers = FALSE;
        guint count = 0;

        if (gst_video_event_parse_downstream_force_key_unit(
                ev, &timestamp, &stream_time, &running_time, &all_headers, &count)) {
            g_print("[FKU][EVENT][DOWN] elem=%s pad=%s ts=%" GST_TIME_FORMAT
                    " stream=%" GST_TIME_FORMAT
                    " running=%" GST_TIME_FORMAT
                    " all_headers=%d count=%u\n",
                    parent_name,
                    pad_name,
                    GST_TIME_ARGS(timestamp),
                    GST_TIME_ARGS(stream_time),
                    GST_TIME_ARGS(running_time),
                    all_headers,
                    count);
        } else {
            g_print("[FKU][EVENT][DOWN] elem=%s pad=%s parse_failed\n",
                    parent_name,
                    pad_name);
        }
    } else {
        g_print("[FKU][EVENT] elem=%s pad=%s type=%s\n",
                parent_name,
                pad_name,
                GST_EVENT_TYPE_NAME(ev));
    }

    if (parent) gst_object_unref(parent);
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn buffer_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    ControlServer *cs = (ControlServer *)user_data;

    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) {
        return GST_PAD_PROBE_OK;
    }

    gint64 last_resume_us = (gint64)atomic_load_explicit(&cs->last_resume_monotonic_us,
                                                         memory_order_relaxed);
    if (last_resume_us <= 0) {
        return GST_PAD_PROBE_OK;
    }

    gint64 now_us = g_get_monotonic_time();
    gint64 delta_us = now_us - last_resume_us;

    gboolean is_delta = GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    gboolean is_header = GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_HEADER);

    if (delta_us >= 0 && delta_us <= 200000) {
        GstElement *parent = gst_pad_get_parent_element(pad);
        const gchar *parent_name = parent ? GST_OBJECT_NAME(parent) : "?";

        g_print("[FKU][BUF] elem=%s pad=%s since_resume=%" G_GINT64_FORMAT " ms "
                "pts=%" GST_TIME_FORMAT " delta=%d header=%d size=%zu\n",
                parent_name,
                GST_PAD_NAME(pad),
                delta_us / 1000,
                GST_TIME_ARGS(GST_BUFFER_PTS(buf)),
                is_delta,
                is_header,
                gst_buffer_get_size(buf));

        if (!is_delta) {
            g_print("[FKU][BUF] >>> non-delta frame seen within 200ms after RESUME\n");
        }

        if (parent) gst_object_unref(parent);
    }

    return GST_PAD_PROBE_OK;
}

static void add_probe_to_pad(GstElement *elem,
                             const gchar *pad_name,
                             GstPadProbeType probe_type,
                             GstPadProbeCallback cb,
                             gpointer user_data) {
    if (!elem) {
        g_printerr("probe: element is NULL for pad %s\n", pad_name);
        return;
    }

    GstPad *pad = gst_element_get_static_pad(elem, pad_name);
    if (!pad) {
        g_printerr("probe: failed to get %s:%s\n",
                   GST_OBJECT_NAME(elem), pad_name);
        return;
    }

    gst_pad_add_probe(pad, probe_type, cb, user_data, NULL);
    g_print("probe attached: %s:%s\n", GST_OBJECT_NAME(elem), pad_name);
    gst_object_unref(pad);
}

static gboolean parse_control_command(char *line, ControlCommandSpec *out);
static void send_control_reply(int client, const char *text);

static void *control_thread(void *arg) {
    ControlServer *cs = (ControlServer *)arg;
    cs->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cs->listen_fd < 0) {
        g_printerr("control: socket failed\n");
        return NULL;
    }

    int yes = 1;
    setsockopt(cs->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)cs->ctrl_port);

    if (bind(cs->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(cs->listen_fd, 4) < 0) {
        g_printerr("control: bind/listen failed\n");
        close(cs->listen_fd);
        cs->listen_fd = -1;
        return NULL;
    }

    while (!atomic_load_explicit(&cs->stop, memory_order_relaxed)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cs->listen_fd, &rfds);
        struct timeval tv = {1, 0};

        int rv = select(cs->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) continue;

        int client = accept(cs->listen_fd, NULL, NULL);
        if (client < 0) continue;

        char buf[128] = {0};
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            ControlCommandSpec spec;
            if (parse_control_command(buf, &spec)) {
                gboolean ok = control_execute_sync(cs, spec, 2000);
                send_control_reply(client, ok ? "OK\n" : "ERR\n");
            } else {
                send_control_reply(client, "UNKNOWN\n");
            }
        }

        close(client);
    }

    return NULL;
}

static void install_video_debug_probes(ControlServer *cs) {
    if (!cs || !cs->vsel || !cs->venc || !cs->vparse) {
        g_printerr("probe: missing elements, skip install\n");
        return;
    }

    add_probe_to_pad(cs->vsel, "src",
                     GST_PAD_PROBE_TYPE_EVENT_UPSTREAM |
                     GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                     event_probe_cb, cs);

    add_probe_to_pad(cs->vsel, "sink_0",
                     GST_PAD_PROBE_TYPE_EVENT_UPSTREAM |
                     GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                     event_probe_cb, cs);

    add_probe_to_pad(cs->venc, "sink",
                     GST_PAD_PROBE_TYPE_EVENT_UPSTREAM |
                     GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                     event_probe_cb, cs);

    add_probe_to_pad(cs->venc, "src",
                     GST_PAD_PROBE_TYPE_EVENT_UPSTREAM |
                     GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                     event_probe_cb, cs);

    add_probe_to_pad(cs->vparse, "sink",
                     GST_PAD_PROBE_TYPE_EVENT_UPSTREAM |
                     GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                     event_probe_cb, cs);

    add_probe_to_pad(cs->vparse, "src",
                     GST_PAD_PROBE_TYPE_EVENT_UPSTREAM |
                     GST_PAD_PROBE_TYPE_BUFFER,
                     buffer_probe_cb, cs);
}

static ControlCommandSpec control_spec(ControlCommand cmd) {
    ControlCommandSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.cmd = cmd;
    return spec;
}

static gboolean parse_resolution_token(const char *a, const char *b, gint *width, gint *height) {
    if (!a || !width || !height) return FALSE;

    int w = 0;
    int h = 0;
    if (strchr(a, 'x') || strchr(a, 'X')) {
        if (sscanf(a, "%dx%d", &w, &h) != 2 &&
            sscanf(a, "%dX%d", &w, &h) != 2) {
            return FALSE;
        }
    } else {
        if (!b) return FALSE;
        w = atoi(a);
        h = atoi(b);
    }

    if (w <= 0 || h <= 0) return FALSE;
    *width = (gint)w;
    *height = (gint)h;
    return TRUE;
}

static gboolean parse_control_command(char *line, ControlCommandSpec *out) {
    if (!line || !out) return FALSE;

    char *trimmed = g_strstrip(line);
    char cmd[32] = {0};
    char a[32] = {0};
    char b[32] = {0};
    int parts = sscanf(trimmed, "%31s %31s %31s", cmd, a, b);
    if (parts <= 0) return FALSE;

    if (!g_ascii_strcasecmp(cmd, "PAUSE")) {
        *out = control_spec(CTRL_CMD_PAUSE);
        return TRUE;
    }
    if (!g_ascii_strcasecmp(cmd, "RESUME")) {
        *out = control_spec(CTRL_CMD_RESUME);
        return TRUE;
    }
    if (!g_ascii_strcasecmp(cmd, "IDR")) {
        *out = control_spec(CTRL_CMD_IDR);
        return TRUE;
    }
    if (!g_ascii_strcasecmp(cmd, "SET_BITRATE") && parts >= 2) {
        *out = control_spec(CTRL_CMD_SET_BITRATE);
        out->bitrate_kbps = atoi(a);
        return out->bitrate_kbps > 0;
    }
    if (!g_ascii_strcasecmp(cmd, "SET_FPS") && parts >= 2) {
        *out = control_spec(CTRL_CMD_SET_FPS);
        out->fps = atoi(a);
        return out->fps > 0;
    }
    if (!g_ascii_strcasecmp(cmd, "SET_RESOLUTION") && parts >= 2) {
        *out = control_spec(CTRL_CMD_SET_RESOLUTION);
        return parse_resolution_token(a, parts >= 3 ? b : NULL, &out->width, &out->height);
    }
    if (!g_ascii_strcasecmp(cmd, "VIDEO_MODE") && parts >= 2) {
        *out = control_spec(CTRL_CMD_VIDEO_MODE);
        if (!g_ascii_strcasecmp(a, "BLACK")) {
            out->mode = CTRL_MODE_BLACK;
            return TRUE;
        }
        if (!g_ascii_strcasecmp(a, "NORMAL")) {
            out->mode = CTRL_MODE_NORMAL;
            return TRUE;
        }
        return FALSE;
    }
    if (!g_ascii_strcasecmp(cmd, "AUDIO_MODE") && parts >= 2) {
        *out = control_spec(CTRL_CMD_AUDIO_MODE);
        if (!g_ascii_strcasecmp(a, "SILENT")) {
            out->mode = CTRL_MODE_SILENT;
            return TRUE;
        }
        if (!g_ascii_strcasecmp(a, "NORMAL")) {
            out->mode = CTRL_MODE_NORMAL;
            return TRUE;
        }
        return FALSE;
    }

    return FALSE;
}

static void send_control_reply(int client, const char *text) {
    if (!text) return;
    send(client, text, strlen(text), 0);
}

static gboolean start_control_server(ControlServer *cs,
                                      GstElement *pipeline,
                                      GMainContext *main_context,
                                      gint port,
                                      const AppConfig *cfg) {
    cs->pipeline = pipeline;
    cs->vsel = gst_bin_get_by_name(GST_BIN(pipeline), "vsel");
    cs->asel = gst_bin_get_by_name(GST_BIN(pipeline), "asel");
    cs->venc = gst_bin_get_by_name(GST_BIN(pipeline), "venc");
    cs->vparse = gst_bin_get_by_name(GST_BIN(pipeline), "vparse");
    cs->vcaps = gst_bin_get_by_name(GST_BIN(pipeline), "vcaps");
    cs->blackcaps = gst_bin_get_by_name(GST_BIN(pipeline), "blackcaps");
    cs->main_context = main_context ? g_main_context_ref(main_context) : NULL;

    cs->ctrl_port = port;
    cs->listen_fd = -1;
    atomic_init(&cs->stop, false);
    atomic_init(&cs->last_resume_monotonic_us, 0);
    atomic_init(&cs->current_width, cfg ? cfg->width : 1280);
    atomic_init(&cs->current_height, cfg ? cfg->height : 720);
    atomic_init(&cs->current_fps, cfg ? cfg->fps : 25);
    atomic_init(&cs->current_bitrate_kbps, cfg ? cfg->bitrate_kbps : 2000);

    install_video_debug_probes(cs);

    if (pthread_create(&cs->thread, NULL, control_thread, cs) != 0) {
        g_printerr("control: failed to create thread\n");
        if (cs->main_context) {
            g_main_context_unref(cs->main_context);
            cs->main_context = NULL;
        }
        return FALSE;
    }
    return TRUE;
}

static void stop_control_server(ControlServer *cs) {
    atomic_store_explicit(&cs->stop, true, memory_order_relaxed);

    if (cs->listen_fd >= 0) {
        close(cs->listen_fd);
        cs->listen_fd = -1;
    }

    if (cs->thread) {
        pthread_join(cs->thread, NULL);
    }

    if (cs->vsel) gst_object_unref(cs->vsel);
    if (cs->asel) gst_object_unref(cs->asel);
    if (cs->venc) gst_object_unref(cs->venc);
    if (cs->vparse) gst_object_unref(cs->vparse);
    if (cs->vcaps) gst_object_unref(cs->vcaps);
    if (cs->blackcaps) gst_object_unref(cs->blackcaps);
    if (cs->main_context) g_main_context_unref(cs->main_context);
}

int main(int argc, char *argv[]) {
    GstElement *pipeline = NULL;
    GstBus *bus = NULL;
    GMainLoop *loop = NULL;
    GError *error = NULL;
    gchar *resolved_audio_device = NULL;
    ControlServer ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    const AppConfig *cfg = &kAppConfig;

    gst_init(&argc, &argv);

    if (!device_exists(cfg->device_path)) {
        g_printerr("Camera device path not found: %s\n", cfg->device_path);
        g_printerr("Check whether the Logitech camera is connected and by-id link exists.\n");
        return 1;
    }

    g_print("Using camera device: %s\n", cfg->device_path);

    if (cfg->audio_enabled) {
        resolved_audio_device = resolve_audio_device(cfg->audio_device);
    }

    GString *pipeline_desc = g_string_new(NULL);

    g_string_append_printf(
        pipeline_desc,
        "mpegtsmux name=mux alignment=7 ! queue ! identity name=tlmyinject silent=true ! "
        "srtclientsink uri=\"srt://%s:%d?mode=caller&latency=%d\" ",
        cfg->host,
        cfg->port,
        cfg->srt_latency
    );

    g_string_append_printf(
        pipeline_desc,
        "input-selector name=vsel sync-streams=true ! "
        "x264enc name=venc tune=zerolatency speed-preset=veryfast "
        "bitrate=%d key-int-max=%d bframes=0 byte-stream=true ! "
        "video/x-h264,profile=%s ! "
        "h264parse name=vparse config-interval=-1 ! queue ! mux. ",
        cfg->bitrate_kbps,
        cfg->gop,
        cfg->profile
    );

    g_string_append_printf(
        pipeline_desc,
        "v4l2src device=%s ! "
        "image/jpeg,width=%d,height=%d,framerate=%d/1 ! "
        "jpegdec ! videoconvert ! videorate ! videoscale ! "
        "capsfilter name=vcaps caps=\"video/x-raw,width=(int)%d,height=(int)%d,framerate=(fraction)%d/1\" ! "
        "queue ! vsel.sink_0 ",
        cfg->device_path,
        cfg->width,
        cfg->height,
        cfg->fps,
        cfg->width,
        cfg->height,
        cfg->fps
    );

    g_string_append_printf(
        pipeline_desc,
        "videotestsrc pattern=black is-live=true ! "
        "videorate ! videoscale ! "
        "capsfilter name=blackcaps caps=\"video/x-raw,width=(int)%d,height=(int)%d,framerate=(fraction)%d/1\" ! "
        "queue ! vsel.sink_1 ",
        cfg->width,
        cfg->height,
        cfg->fps
    );

    if (cfg->audio_enabled) {
        gint audio_bps = cfg->audio_bitrate_kbps * 1000;

        if (g_strcmp0(cfg->audio_codec, "opus") == 0) {
            g_string_append_printf(
                pipeline_desc,
                " input-selector name=asel sync-streams=true ! queue ! "
                "opusenc bitrate=%d frame-size=20 ! queue ! mux. ",
                audio_bps
            );
        } else {
            g_string_append_printf(
                pipeline_desc,
                " input-selector name=asel sync-streams=true ! queue ! "
                "avenc_aac bitrate=%d ! queue ! mux. ",
                audio_bps
            );
        }

        int audio_rate = 48000;

        g_string_append_printf(
            pipeline_desc,
            " alsasrc device=%s ! audioresample ! audioconvert ! "
            "audio/x-raw,rate=%d,channels=2 ! queue ! asel.sink_0 ",
            resolved_audio_device,
            audio_rate
        );

        g_string_append_printf(
            pipeline_desc,
            " audiotestsrc wave=silence is-live=true ! audioresample ! audioconvert ! "
            "audio/x-raw,rate=%d,channels=2 ! queue ! asel.sink_1 ",
            audio_rate
        );
    }

    gchar *pipeline_str = g_string_free(pipeline_desc, FALSE);

    g_print("Starting pipeline:\n%s\n\n", pipeline_str);

    pipeline = gst_parse_launch(pipeline_str, &error);
    g_free(pipeline_str);

    if (!pipeline) {
        g_printerr("Failed to create pipeline: %s\n",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        g_free(resolved_audio_device);
        return 2;
    }

    loop = g_main_loop_new(NULL, FALSE);
    bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_call, loop);

    TelemetryInjectCtx tlmy_inject;
    memset(&tlmy_inject, 0, sizeof(tlmy_inject));
    tlmy_inject.next_inject_at = TLMY_TS_INTERVAL;

    if (!install_tlmy_injector(pipeline, &tlmy_inject)) {
        g_printerr("[TLMY] injector install failed\n");
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set pipeline to PLAYING\n");

        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus,
            3 * GST_SECOND,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING)
        );

        if (msg) {
            GError *err = NULL;
            gchar *debug = NULL;

            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                gst_message_parse_error(msg, &err, &debug);
                g_printerr("ERROR from %s: %s\n",
                        GST_OBJECT_NAME(msg->src),
                        err ? err->message : "unknown");
            } else {
                gst_message_parse_warning(msg, &err, &debug);
                g_printerr("WARNING from %s: %s\n",
                        GST_OBJECT_NAME(msg->src),
                        err ? err->message : "unknown");
            }

            if (debug) {
                g_printerr("Debug details: %s\n", debug);
            }

            g_clear_error(&err);
            g_free(debug);
            gst_message_unref(msg);
        } else {
            g_printerr("No detailed bus message received within timeout\n");
        }

        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        g_main_loop_unref(loop);
        g_free(resolved_audio_device);
        return 3;
    }
    g_print("Streaming started.\n");

    if (!start_control_server(&ctrl,
                               pipeline,
                               g_main_loop_get_context(loop),
                               cfg->ctrl_port,
                               cfg)) {
        g_printerr("Warning: control server not running\n");
    } else {
        g_print("Control server listening on %d (PAUSE/RESUME/IDR/SET_*)\n", cfg->ctrl_port);
    }

    g_main_loop_run(loop);

    g_print("Shutting down...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    stop_control_server(&ctrl);

    gst_object_unref(bus);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    g_free(resolved_audio_device);

    return 0;
}
