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
    const gchar *audio_device;
    gint audio_bitrate_kbps;
    const gchar *audio_codec; // aac | opus
    gint ctrl_port;           // TCP control port for pause/resume
} AppConfig;

/*
 * 唯一配置变量
 * 这里直接绑定这只罗技摄像头的稳定 by-id 路径，不再依赖 /dev/videoN
 */
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
    .srt_latency = 120,
    .audio_enabled = TRUE,
    .audio_device = "plughw:3,0",
    .audio_bitrate_kbps = 128,
    .audio_codec = "aac",
    .ctrl_port = 10090
};

static gboolean device_exists(const gchar *path) {
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0);
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;

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
typedef struct {
    GstElement *pipeline;
    GstElement *vsel;
    GstElement *asel;

    GstElement *venc;    // x264enc
    GstElement *vparse;  // h264parse

    gint ctrl_port;
    pthread_t thread;
    int listen_fd;
    gboolean stop;

    gint64 last_resume_monotonic_us; // lastest RESUME time, in monotonic microseconds
} ControlServer;

static gboolean is_force_key_unit_event(GstEvent *ev) {
    if (!ev) return FALSE;
    return gst_video_event_is_force_key_unit(ev);
}

static GstPadProbeReturn event_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
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

    if (cs->last_resume_monotonic_us <= 0) {
        return GST_PAD_PROBE_OK;
    }

    gint64 now_us = g_get_monotonic_time();
    gint64 delta_us = now_us - cs->last_resume_monotonic_us;;


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

static void *control_thread(void *arg) {
    ControlServer *cs = (ControlServer *)arg;
    cs->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cs->listen_fd < 0) {
        g_printerr("control: socket failed\n");
        return NULL;
    }

    int yes = 1;
    setsockopt(cs->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
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

    while (!cs->stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cs->listen_fd, &rfds);
        struct timeval tv = {1, 0};

        int rv = select(cs->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) continue;

        int client = accept(cs->listen_fd, NULL, NULL);
        if (client < 0) continue;

        char buf[64] = {0};
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            if (!g_ascii_strncasecmp(buf, "PAUSE", 5)) {
                if (cs->vsel) {
                    GstPad *pad = gst_element_get_static_pad(cs->vsel, "sink_1");
                    g_object_set(cs->vsel, "active-pad", pad, NULL);
                    gst_object_unref(pad);
                }
                if (cs->asel) {
                    GstPad *pad = gst_element_get_static_pad(cs->asel, "sink_1");
                    g_object_set(cs->asel, "active-pad", pad, NULL);
                    gst_object_unref(pad);
                }
                send(client, "OK\n", 3, 0);
            } else if (!g_ascii_strncasecmp(buf, "RESUME", 6)) {
                if (cs->vsel) {
                    GstPad *pad0 = gst_element_get_static_pad(cs->vsel, "sink_0");
                    if (pad0) {
                        g_object_set(cs->vsel, "active-pad", pad0, NULL);
                        gst_object_unref(pad0);
                        g_print("[CTRL] vsel switched to sink_0\n");
                    } else {
                        g_printerr("[CTRL] failed to get vsel:sink_0\n");
                    }

                    GstPad *src = NULL;

                    if (cs->vparse) {
                        src = gst_element_get_static_pad(cs->vparse, "src");
                    }

                    if (src) {
                        cs->last_resume_monotonic_us = g_get_monotonic_time();

                        GstEvent *force_kf =
                            gst_video_event_new_upstream_force_key_unit(
                                GST_CLOCK_TIME_NONE,
                                TRUE,
                                0
                            );

                        gboolean ok = gst_pad_send_event(src, force_kf);
                        g_print("[CTRL] upstream force-key-unit sent from vparse:src result=%d\n", ok);

                        gst_object_unref(src);
                    } else {
                        g_printerr("[CTRL] failed to get vparse:src\n");
                    }
                }

                if (cs->asel) {
                    GstPad *pad0 = gst_element_get_static_pad(cs->asel, "sink_0");
                    if (pad0) {
                        g_object_set(cs->asel, "active-pad", pad0, NULL);
                        gst_object_unref(pad0);
                        g_print("[CTRL] asel switched to sink_0\n");
                    } else {
                        g_printerr("[CTRL] failed to get asel:sink_0\n");
                    }
                }

                send(client, "OK\n", 3, 0);
            }else {
                send(client, "UNKNOWN\n", 8, 0);
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

static gboolean start_control_server(ControlServer *cs, GstElement *pipeline, gint port) {
    cs->pipeline = pipeline;
    cs->vsel = gst_bin_get_by_name(GST_BIN(pipeline), "vsel");
    cs->asel = gst_bin_get_by_name(GST_BIN(pipeline), "asel");
    cs->venc = gst_bin_get_by_name(GST_BIN(pipeline), "venc");
    cs->vparse = gst_bin_get_by_name(GST_BIN(pipeline), "vparse");

    cs->ctrl_port = port;
    cs->stop = FALSE;
    cs->listen_fd = -1;
    cs->last_resume_monotonic_us = 0;

    install_video_debug_probes(cs);

    if (pthread_create(&cs->thread, NULL, control_thread, cs) != 0) {
        g_printerr("control: failed to create thread\n");
        return FALSE;
    }
    return TRUE;
}

static void stop_control_server(ControlServer *cs) {
    cs->stop = TRUE;

    if (cs->listen_fd >= 0) {
        close(cs->listen_fd);
    }

    if (cs->thread) {
        pthread_join(cs->thread, NULL);
    }

    if (cs->vsel) gst_object_unref(cs->vsel);
    if (cs->asel) gst_object_unref(cs->asel);
    if (cs->venc) gst_object_unref(cs->venc);
    if (cs->vparse) gst_object_unref(cs->vparse);
}

int main(int argc, char *argv[]) {
    GstElement *pipeline = NULL;
    GstBus *bus = NULL;
    GMainLoop *loop = NULL;
    GError *error = NULL;
    ControlServer ctrl = {0};

    const AppConfig *cfg = &kAppConfig;

    gst_init(&argc, &argv);

    if (!device_exists(cfg->device_path)) {
        g_printerr("Camera device path not found: %s\n", cfg->device_path);
        g_printerr("Check whether the Logitech camera is connected and by-id link exists.\n");
        return 1;
    }

    g_print("Using camera device: %s\n", cfg->device_path);

    GString *pipeline_desc = g_string_new(NULL);

    g_string_append_printf(
        pipeline_desc,
        "mpegtsmux name=mux alignment=7 ! queue ! "
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
        "jpegdec ! videoconvert ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
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
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
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
            cfg->audio_device,
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
        return 2;
    }

    loop = g_main_loop_new(NULL, FALSE);
    bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_call, loop);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set pipeline to PLAYING\n");
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        g_main_loop_unref(loop);
        return 3;
    }

    g_print("Streaming started.\n");

    if (!start_control_server(&ctrl, pipeline, cfg->ctrl_port)) {
        g_printerr("Warning: control server not running\n");
    } else {
        g_print("Control server listening on %d (PAUSE/RESUME)\n", cfg->ctrl_port);
    }

    g_main_loop_run(loop);

    g_print("Shutting down...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    stop_control_server(&ctrl);

    gst_object_unref(bus);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);

    return 0;
}