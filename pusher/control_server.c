#include "control_server.h"

#include <arpa/inet.h>
#include <gst/video/video.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

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

gboolean start_control_server(ControlServer *cs,
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

void stop_control_server(ControlServer *cs) {
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
