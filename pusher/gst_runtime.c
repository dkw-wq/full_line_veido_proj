#include "gst_runtime.h"

#include <sys/stat.h>

gboolean device_exists(const gchar *path) {
    struct stat st;
    return (path != NULL) && (stat(path, &st) == 0);
}

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
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

gchar *build_pipeline_description(const AppConfig *cfg, const gchar *resolved_audio_device) {
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

    return g_string_free(pipeline_desc, FALSE);
}