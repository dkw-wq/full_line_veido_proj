// pusher/srt_cam_push.c
// 应用入口。GStreamer 管线、音频枚举、控制服务器和遥测注入已拆分到独立模块。

#include "app_config.h"
#include "audio_device.h"
#include "control_server.h"
#include "gst_runtime.h"
#include "telemetry_injector.h"
#include "telemetry.h"

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

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

    gchar *pipeline_str = build_pipeline_description(cfg, resolved_audio_device);

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