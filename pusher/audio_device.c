#include "audio_device.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

gchar *resolve_audio_device(const gchar *preferred_keyword) {
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