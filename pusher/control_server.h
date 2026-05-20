#pragma once

#include "app_config.h"

#include <gst/gst.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct ControlServer ControlServer;

struct ControlServer {
    GstElement *pipeline;
    GstElement *vsel;
    GstElement *asel;

    GstElement *venc;
    GstElement *vparse;
    GstElement *vcaps;
    GstElement *blackcaps;

    GMainContext *main_context;

    gint ctrl_port;
    pthread_t thread;
    int listen_fd;
    atomic_bool stop;

    atomic_llong last_resume_monotonic_us;
    atomic_int current_width;
    atomic_int current_height;
    atomic_int current_fps;
    atomic_int current_bitrate_kbps;
};

gboolean start_control_server(ControlServer *cs,
                              GstElement *pipeline,
                              GMainContext *main_context,
                              gint port,
                              const AppConfig *cfg);
void stop_control_server(ControlServer *cs);