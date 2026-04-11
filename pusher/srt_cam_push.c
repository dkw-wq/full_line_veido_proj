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
    gint ctrl_port;
    pthread_t thread;
    int listen_fd;
    gboolean stop;
} ControlServer;

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
                    g_object_set(cs->vsel, "active-pad", pad0, NULL);
                    gst_object_unref(pad0);

                    GstStructure *s = gst_structure_new(
                        "GstForceKeyUnit",
                        "all-headers", G_TYPE_BOOLEAN, TRUE,
                        "count", G_TYPE_UINT, 0,
                        "timestamp", G_TYPE_UINT64, GST_CLOCK_TIME_NONE,
                        "stream-time", G_TYPE_UINT64, GST_CLOCK_TIME_NONE,
                        NULL
                    );
                    GstEvent *force_kf = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, s);
                    GstPad *src = gst_element_get_static_pad(cs->vsel, "src");
                    gst_pad_send_event(src, force_kf);
                    gst_object_unref(src);
                }

                if (cs->asel) {
                    GstPad *pad0 = gst_element_get_static_pad(cs->asel, "sink_0");
                    g_object_set(cs->asel, "active-pad", pad0, NULL);
                    gst_object_unref(pad0);
                }

                send(client, "OK\n", 3, 0);
            } else {
                send(client, "UNKNOWN\n", 8, 0);
            }
        }

        close(client);
    }

    return NULL;
}

static gboolean start_control_server(ControlServer *cs, GstElement *pipeline, gint port) {
    cs->pipeline = pipeline;
    cs->vsel = gst_bin_get_by_name(GST_BIN(pipeline), "vsel");
    cs->asel = gst_bin_get_by_name(GST_BIN(pipeline), "asel");
    cs->ctrl_port = port;
    cs->stop = FALSE;
    cs->listen_fd = -1;

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
        "x264enc tune=zerolatency speed-preset=veryfast "
        "bitrate=%d key-int-max=%d bframes=0 byte-stream=true ! "
        "video/x-h264,profile=%s ! "
        "h264parse config-interval=-1 ! queue ! mux. ",
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