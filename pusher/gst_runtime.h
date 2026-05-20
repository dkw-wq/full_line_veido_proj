#pragma once

#include "app_config.h"

#include <glib.h>
#include <gst/gst.h>

gboolean device_exists(const gchar *path);
gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);
gchar *build_pipeline_description(const AppConfig *cfg, const gchar *resolved_audio_device);