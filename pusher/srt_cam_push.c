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

typedef struct {
    gchar *device;
    gchar *host;
    gint port;
    gint width;
    gint height;
    gint fps;
    gint bitrate_kbps;
    gint gop;
    gchar *profile;   // baseline / main
    gint srt_latency; // ms
    gboolean audio_enabled;
    gchar *audio_device;
    gint audio_bitrate_kbps;
    gchar *audio_codec; // aac | opus
    gint ctrl_port;     // TCP control port for pause/resume
} AppConfig;

static const gchar *kConfigGroup = "srt_cam_push";

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

static void config_init(AppConfig *cfg) {
    cfg->device = g_strdup("/dev/video0");
    cfg->host = g_strdup("127.0.0.1");
    cfg->port = 9000;
    cfg->width = 1280;
    cfg->height = 720;
    cfg->fps = 25;
    cfg->bitrate_kbps = 2000;
    cfg->gop = 50;           // 25fps * 2 sec
    cfg->profile = g_strdup("baseline");
    cfg->srt_latency = 120;  // ms
    cfg->audio_enabled = TRUE;
    cfg->audio_device = g_strdup("hw:0");
    cfg->audio_bitrate_kbps = 128;
    cfg->audio_codec = g_strdup("aac");
    cfg->ctrl_port = 10090;
}

static void config_free(AppConfig *cfg) {
    g_free(cfg->device);
    g_free(cfg->host);
    g_free(cfg->profile);
    g_free(cfg->audio_device);
    g_free(cfg->audio_codec);
}

static gboolean load_config_file(const gchar *path, AppConfig *cfg, GError **error) {
    GKeyFile *kf = g_key_file_new();
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, error);
    if (!ok) {
        g_key_file_unref(kf);
        return FALSE;
    }

    if (!g_key_file_has_group(kf, kConfigGroup)) {
        g_set_error(error,
                    g_quark_from_static_string("srt-cam-push"),
                    1,
                    "Missing [%s] section in config file: %s",
                    kConfigGroup,
                    path);
        g_key_file_unref(kf);
        return FALSE;
    }

    if (g_key_file_has_key(kf, kConfigGroup, "device", NULL)) {
        gchar *v = g_key_file_get_string(kf, kConfigGroup, "device", NULL);
        g_free(cfg->device);
        cfg->device = v;
    }
    if (g_key_file_has_key(kf, kConfigGroup, "host", NULL)) {
        gchar *v = g_key_file_get_string(kf, kConfigGroup, "host", NULL);
        g_free(cfg->host);
        cfg->host = v;
    }
    if (g_key_file_has_key(kf, kConfigGroup, "profile", NULL)) {
        gchar *v = g_key_file_get_string(kf, kConfigGroup, "profile", NULL);
        g_free(cfg->profile);
        cfg->profile = v;
    }

    if (g_key_file_has_key(kf, kConfigGroup, "port", NULL))
        cfg->port = g_key_file_get_integer(kf, kConfigGroup, "port", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "width", NULL))
        cfg->width = g_key_file_get_integer(kf, kConfigGroup, "width", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "height", NULL))
        cfg->height = g_key_file_get_integer(kf, kConfigGroup, "height", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "fps", NULL))
        cfg->fps = g_key_file_get_integer(kf, kConfigGroup, "fps", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "bitrate", NULL))
        cfg->bitrate_kbps = g_key_file_get_integer(kf, kConfigGroup, "bitrate", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "gop", NULL))
        cfg->gop = g_key_file_get_integer(kf, kConfigGroup, "gop", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "latency", NULL))
        cfg->srt_latency = g_key_file_get_integer(kf, kConfigGroup, "latency", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "audio", NULL))
        cfg->audio_enabled = g_key_file_get_boolean(kf, kConfigGroup, "audio", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "audio_device", NULL)) {
        gchar *v = g_key_file_get_string(kf, kConfigGroup, "audio_device", NULL);
        g_free(cfg->audio_device);
        cfg->audio_device = v;
    }
    if (g_key_file_has_key(kf, kConfigGroup, "audio_bitrate", NULL))
        cfg->audio_bitrate_kbps = g_key_file_get_integer(kf, kConfigGroup, "audio_bitrate", NULL);
    if (g_key_file_has_key(kf, kConfigGroup, "audio_codec", NULL)) {
        gchar *v = g_key_file_get_string(kf, kConfigGroup, "audio_codec", NULL);
        g_free(cfg->audio_codec);
        cfg->audio_codec = v;
    }
    if (g_key_file_has_key(kf, kConfigGroup, "ctrl_port", NULL))
        cfg->ctrl_port = g_key_file_get_integer(kf, kConfigGroup, "ctrl_port", NULL);

    g_key_file_unref(kf);
    return TRUE;
}

static const gchar *find_config_path_arg(int argc, char **argv) {
    const gchar *path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config")) {
            if (i + 1 >= argc) {
                return NULL;
            }
            path = argv[i + 1];
            ++i;
        }
    }
    return path;
}

static gboolean parse_args(int argc, char **argv, AppConfig *cfg) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            ++i; // already loaded earlier; keep for CLI compatibility
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            g_free(cfg->device);
            cfg->device = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            g_free(cfg->host);
            cfg->host = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            cfg->port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
            cfg->width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            cfg->height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            cfg->fps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) {
            cfg->bitrate_kbps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gop") && i + 1 < argc) {
            cfg->gop = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            g_free(cfg->profile);
            cfg->profile = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--latency") && i + 1 < argc) {
            cfg->srt_latency = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-audio")) {
            cfg->audio_enabled = FALSE;
        } else if (!strcmp(argv[i], "--audio-device") && i + 1 < argc) {
            g_free(cfg->audio_device);
            cfg->audio_device = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--audio-bitrate") && i + 1 < argc) {
            cfg->audio_bitrate_kbps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--audio-codec") && i + 1 < argc) {
            g_free(cfg->audio_codec);
            cfg->audio_codec = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--ctrl-port") && i + 1 < argc) {
            cfg->ctrl_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {
            return FALSE;
        } else {
            g_printerr("Unknown arg: %s\n", argv[i]);
            return FALSE;
        }
    }
    return TRUE;
}

static void print_usage(const char *prog) {
    g_print(
        "Usage:\n"
        "  %s [options]\n\n"
        "Options:\n"
        "  --config   ./srt_cam_push.conf\n"
        "  --device   /dev/video0\n"
        "  --host     127.0.0.1\n"
        "  --port     9000\n"
        "  --width    1280\n"
        "  --height   720\n"
        "  --fps      25\n"
        "  --bitrate  2000       (kbps)\n"
        "  --gop      50         (frames)\n"
        "  --profile  baseline   (baseline|main)\n"
        "  --latency  120        (ms)\n"
        "  --no-audio            disable audio capture\n"
        "  --audio-device hw:0   ALSA device (alsasrc)\n"
        "  --audio-bitrate 128   (kbps)\n"
        "  --audio-codec aac     (aac|opus)\n"
        "  --ctrl-port 10090     TCP control port (PAUSE/RESUME)\n",
        prog
    );
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
                // Switch selectors to black/silence (sink_1).
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
                // Switch back to live (sink_0) and force an immediate keyframe.
                if (cs->vsel) {
                    GstPad *pad0 = gst_element_get_static_pad(cs->vsel, "sink_0");
                    g_object_set(cs->vsel, "active-pad", pad0, NULL);
                    gst_object_unref(pad0);
                    GstStructure *s = gst_structure_new("GstForceKeyUnit",
                                                        "all-headers", G_TYPE_BOOLEAN, TRUE,
                                                        "count", G_TYPE_UINT, 0,
                                                        "timestamp", G_TYPE_UINT64, GST_CLOCK_TIME_NONE,
                                                        "stream-time", G_TYPE_UINT64, GST_CLOCK_TIME_NONE,
                                                        NULL);
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

    AppConfig cfg;
    config_init(&cfg);

    gst_init(&argc, &argv);

    const gchar *config_path = find_config_path_arg(argc, argv);
    if (config_path != NULL) {
        if (!load_config_file(config_path, &cfg, &error)) {
            g_printerr("Failed to load config file %s: %s\n",
                       config_path,
                       error ? error->message : "unknown");
            g_clear_error(&error);
            config_free(&cfg);
            return 1;
        }
    }

    if (!parse_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        config_free(&cfg);
        return 1;
    }

    GString *pipeline_desc = g_string_new(NULL);
    // First, connect mux output to SRT sink.
    g_string_append_printf(
        pipeline_desc,
        "mpegtsmux name=mux alignment=7 ! queue ! "
        "srtclientsink uri=\"srt://%s:%d?mode=caller&latency=%d\" ",
        cfg.host,
        cfg.port,
        cfg.srt_latency
    );

    // Video branch with selector (live vs black).
    g_string_append_printf(
        pipeline_desc,
        "input-selector name=vsel sync-streams=true ! "
        "x264enc tune=zerolatency speed-preset=veryfast "
        "bitrate=%d key-int-max=%d bframes=0 byte-stream=true ! "
        "video/x-h264,profile=%s ! "
        "h264parse config-interval=-1 ! queue ! mux. ",
        cfg.bitrate_kbps,
        cfg.gop,
        cfg.profile
    );

    g_string_append_printf(
        pipeline_desc,
        "v4l2src device=%s ! "
        "image/jpeg,width=%d,height=%d,framerate=%d/1 ! "
        "jpegdec ! videoconvert ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
        "queue ! vsel.sink_0 ",
        cfg.device,
        cfg.width,
        cfg.height,
        cfg.fps,
        cfg.width,
        cfg.height,
        cfg.fps
    );

    g_string_append_printf(
        pipeline_desc,
        "videotestsrc pattern=black is-live=true ! "
        "video/x-raw,width=%d,height=%d,framerate=%d/1 ! "
        "queue ! vsel.sink_1 ",
        cfg.width,
        cfg.height,
        cfg.fps
    );

    if (cfg.audio_enabled) {
        gint audio_bps = cfg.audio_bitrate_kbps * 1000;
        if (g_strcmp0(cfg.audio_codec, "opus") == 0) {
            g_string_append_printf(
                pipeline_desc,
                " input-selector name=asel sync-streams=true ! queue ! "
                "opusenc bitrate=%d frame-size=20 ! queue ! mux. ",
                audio_bps
            );
        } else { // default: aac
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
            cfg.audio_device,
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
        config_free(&cfg);
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
        config_free(&cfg);
        return 3;
    }

    g_print("Streaming started.\n");

    if (!start_control_server(&ctrl, pipeline, cfg.ctrl_port)) {
        g_printerr("Warning: control server not running\n");
    } else {
        g_print("Control server listening on %d (PAUSE/RESUME)\n", cfg.ctrl_port);
    }

    g_main_loop_run(loop);

    g_print("Shutting down...\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    stop_control_server(&ctrl);

    gst_object_unref(bus);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    config_free(&cfg);

    return 0;
}
