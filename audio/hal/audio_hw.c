/*
 * SPDX-FileCopyrightText: The Android Open Source Project
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Primary audio HAL for the LG V30 (joan) on the mainline sdm845 machine
 * driver: one q6asm playback frontend (MultiMedia1, PCM 0) and one capture
 * frontend (MultiMedia2, PCM 1). The frontend has to be routed to a backend
 * before the PCM can be opened, so routes are applied from mixer_paths.xml
 * while the stream is in standby and every device change goes through
 * standby. Volume is left to the framework (software gain).
 */

#define LOG_TAG "audio_hw_joan"
/* #define LOG_NDEBUG 0 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <audio_route/audio_route.h>
#include <cutils/str_parms.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <system/audio.h>
#include <tinyalsa/asoundlib.h>

#define MIXER_XML_PATH "/vendor/etc/mixer_paths.xml"
#define CARD_NAME "LGV30"

#define PCM_PLAYBACK 0
#define PCM_CAPTURE 1

#define SAMPLE_RATE 48000
#define OUT_PERIOD_SIZE 960 /* 20 ms */
#define OUT_PERIOD_COUNT 4
#define IN_PERIOD_SIZE 960
#define IN_PERIOD_COUNT 4

#define PCM_OPEN_RETRIES 10
#define PCM_OPEN_WAIT_US 20000

struct joan_audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock; /* lock order: device, then stream */
    int card;
    struct audio_route *ar;
    time_t route_retry_after;
    audio_mode_t mode;
    bool mic_mute;
    const char *out_path; /* route applied for the active output, or NULL */
    const char *in_path;
    struct joan_stream_out *output;
    struct joan_stream_in *input;
};

struct joan_stream_out {
    struct audio_stream_out stream;

    pthread_mutex_t lock;
    struct joan_audio_device *dev;
    struct pcm_config config;
    struct pcm *pcm;
    audio_devices_t devices;
    bool standby;
    uint64_t frames_written;
    uint64_t frames_presented_base; /* frames written before the current pcm */
};

struct joan_stream_in {
    struct audio_stream_in stream;

    pthread_mutex_t lock;
    struct joan_audio_device *dev;
    struct pcm_config config;
    struct pcm *pcm;
    audio_devices_t devices;
    audio_source_t source;
    bool standby;
    uint64_t frames_read;
};

static const struct pcm_config out_config_default = {
    .channels = 2,
    .rate = SAMPLE_RATE,
    .period_size = OUT_PERIOD_SIZE,
    .period_count = OUT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = OUT_PERIOD_SIZE * 2,
    .avail_min = OUT_PERIOD_SIZE,
};

static const struct pcm_config in_config_default = {
    .channels = 1,
    .rate = SAMPLE_RATE,
    .period_size = IN_PERIOD_SIZE,
    .period_count = IN_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 1,
};

static int find_card(void)
{
    char line[256];
    FILE *f = fopen("/proc/asound/cards", "r");
    int card = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        char id[64];

        if (sscanf(line, " %d [%63[^] ]", &n, id) == 2 && !strcmp(id, CARD_NAME)) {
            card = n;
            break;
        }
    }
    fclose(f);
    return card;
}

/* Called with the device lock held, or while constructing the device. */
static int ensure_routes(struct joan_audio_device *adev)
{
    struct timespec now;
    int card;

    if (adev->ar)
        return 0;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -ENODEV;
    if (now.tv_sec < adev->route_retry_after)
        return -ENODEV;
    adev->route_retry_after = now.tv_sec + 1;

    /* The ADSP and its codec can appear after audioserver has booted. Do
     * not bind a different card at index zero or make this a permanent
     * failure. A later stream write/read will retry without blocking boot.
     */
    card = find_card();
    if (card < 0)
        return -ENODEV;
    adev->ar = audio_route_init(card, MIXER_XML_PATH);
    if (!adev->ar)
        return -ENODEV;
    adev->card = card;
    ALOGI("audio routes ready on card %d", card);
    return 0;
}

static const char *output_path(audio_devices_t devices, audio_mode_t mode)
{
    if (devices & (AUDIO_DEVICE_OUT_WIRED_HEADSET | AUDIO_DEVICE_OUT_WIRED_HEADPHONE))
        return "headphones";
    if (devices & AUDIO_DEVICE_OUT_EARPIECE)
        return "earpiece";
    (void)mode;
    return "speaker";
}

/*
 * wcd934x sizes the SLIMbus capture stream from the TX ports a path enables,
 * so the path has to match the channel count the PCM is opened with.
 */
static const char *input_path(audio_devices_t devices, unsigned int channels)
{
    if (devices == AUDIO_DEVICE_IN_WIRED_HEADSET)
        return "headset-mic";
    if (channels == 2)
        return "dual-mic";
    return "main-mic";
}

/* Must be called with the device lock held. */
static void set_out_route(struct joan_audio_device *adev, const char *path)
{
    if (adev->out_path == path || !adev->ar)
        return;
    if (adev->out_path)
        audio_route_reset_and_update_path(adev->ar, adev->out_path);
    adev->out_path = path;
    if (path)
        audio_route_apply_and_update_path(adev->ar, path);
    ALOGV("output route: %s", path ? path : "none");
}

static void set_in_route(struct joan_audio_device *adev, const char *path)
{
    if (adev->in_path == path || !adev->ar)
        return;
    if (adev->in_path)
        audio_route_reset_and_update_path(adev->ar, adev->in_path);
    adev->in_path = path;
    if (path)
        audio_route_apply_and_update_path(adev->ar, path);
    ALOGV("input route: %s", path ? path : "none");
}

static struct pcm *open_pcm(int card, int device, unsigned int flags, struct pcm_config *config)
{
    struct pcm *pcm = NULL;
    int i;

    for (i = 0; i < PCM_OPEN_RETRIES; i++) {
        pcm = pcm_open(card, device, flags, config);
        if (pcm && pcm_is_ready(pcm))
            return pcm;
        ALOGE("cannot open pcm %d,%d: %s", card, device, pcm ? pcm_get_error(pcm) : "?");
        if (pcm)
            pcm_close(pcm);
        pcm = NULL;
        usleep(PCM_OPEN_WAIT_US);
    }
    return NULL;
}

/* ---------------------------------------------------------------- output */

/* Must be called with device and stream locks held. */
static int do_out_standby(struct joan_stream_out *out)
{
    struct joan_audio_device *adev = out->dev;

    if (!out->standby) {
        if (out->pcm) {
            pcm_close(out->pcm);
            out->pcm = NULL;
        }
        out->frames_presented_base = out->frames_written;
        out->standby = true;
        set_out_route(adev, NULL);
    }
    return 0;
}

static int start_output(struct joan_stream_out *out)
{
    struct joan_audio_device *adev = out->dev;

    if (ensure_routes(adev) != 0)
        return -ENODEV;

    set_out_route(adev, output_path(out->devices, adev->mode));
    out->pcm = open_pcm(adev->card, PCM_PLAYBACK, PCM_OUT | PCM_MONOTONIC, &out->config);
    if (!out->pcm) {
        set_out_route(adev, NULL);
        return -ENODEV;
    }
    out->standby = false;
    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    return ((const struct joan_stream_out *)stream)->config.rate;
}

static int out_set_sample_rate(struct audio_stream *stream __unused, uint32_t rate __unused)
{
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    const struct joan_stream_out *out = (const struct joan_stream_out *)stream;

    return out->config.period_size *
           audio_stream_out_frame_size((const struct audio_stream_out *)stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream __unused)
{
    return AUDIO_CHANNEL_OUT_STEREO;
}

static audio_format_t out_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream __unused, audio_format_t format __unused)
{
    return -ENOSYS;
}

static int out_standby(struct audio_stream *stream)
{
    struct joan_stream_out *out = (struct joan_stream_out *)stream;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    do_out_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return 0;
}

static int out_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct joan_stream_out *out = (struct joan_stream_out *)stream;
    struct joan_audio_device *adev = out->dev;
    struct str_parms *parms = str_parms_create_str(kvpairs);
    char value[32];

    if (!parms)
        return -ENOMEM;
    if (str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value)) >= 0) {
        audio_devices_t devices = (audio_devices_t)strtoul(value, NULL, 0);

        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        if (devices != AUDIO_DEVICE_NONE && devices != out->devices) {
            const char *path = output_path(devices, adev->mode);

            /* the route can only change while the frontend is closed */
            if (!out->standby && path != adev->out_path)
                do_out_standby(out);
            out->devices = devices;
        }
        pthread_mutex_unlock(&out->lock);
        pthread_mutex_unlock(&adev->lock);
    }
    str_parms_destroy(parms);
    return 0;
}

static char *out_get_parameters(const struct audio_stream *stream __unused,
                                const char *keys __unused)
{
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    const struct joan_stream_out *out = (const struct joan_stream_out *)stream;

    return out->config.period_size * out->config.period_count * 1000 / out->config.rate;
}

static int out_set_volume(struct audio_stream_out *stream __unused, float left __unused,
                          float right __unused)
{
    return -ENOSYS;
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer, size_t bytes)
{
    struct joan_stream_out *out = (struct joan_stream_out *)stream;
    struct joan_audio_device *adev = out->dev;
    size_t frame_size = audio_stream_out_frame_size(stream);
    int ret = 0;

    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby)
        ret = start_output(out);
    pthread_mutex_unlock(&adev->lock);

    if (ret == 0)
        ret = pcm_write(out->pcm, buffer, bytes);
    if (ret == 0) {
        out->frames_written += bytes / frame_size;
    } else {
        ALOGE_IF(out->pcm, "pcm_write: %s", pcm_get_error(out->pcm));
        pthread_mutex_unlock(&out->lock);
        /* keep the mixer thread on time when the hardware is not there */
        usleep(bytes * 1000000ULL / frame_size / out_get_sample_rate(&stream->common));
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        do_out_standby(out);
        pthread_mutex_unlock(&adev->lock);
        out->frames_written += bytes / frame_size;
    }
    pthread_mutex_unlock(&out->lock);
    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream __unused,
                                   uint32_t *dsp_frames __unused)
{
    return -ENOSYS;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                         uint64_t *frames, struct timespec *timestamp)
{
    struct joan_stream_out *out = (struct joan_stream_out *)stream;
    unsigned int avail;
    int ret = -ENODATA;

    pthread_mutex_lock(&out->lock);
    if (out->pcm && pcm_get_htimestamp(out->pcm, &avail, timestamp) == 0) {
        uint64_t queued = out->config.period_size * out->config.period_count - avail;

        if (out->frames_written >= queued) {
            *frames = out->frames_written - queued;
            ret = 0;
        }
    }
    pthread_mutex_unlock(&out->lock);
    return ret;
}

static int out_add_audio_effect(const struct audio_stream *stream __unused,
                                effect_handle_t effect __unused)
{
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream __unused,
                                   effect_handle_t effect __unused)
{
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream __unused,
                                        int64_t *timestamp __unused)
{
    return -ENOSYS;
}

/* ----------------------------------------------------------------- input */

static int do_in_standby(struct joan_stream_in *in)
{
    struct joan_audio_device *adev = in->dev;

    if (!in->standby) {
        if (in->pcm) {
            pcm_close(in->pcm);
            in->pcm = NULL;
        }
        in->standby = true;
        set_in_route(adev, NULL);
    }
    return 0;
}

static int start_input(struct joan_stream_in *in)
{
    struct joan_audio_device *adev = in->dev;

    if (ensure_routes(adev) != 0)
        return -ENODEV;

    set_in_route(adev, input_path(in->devices, in->config.channels));
    in->pcm = open_pcm(adev->card, PCM_CAPTURE, PCM_IN | PCM_MONOTONIC, &in->config);
    if (!in->pcm) {
        set_in_route(adev, NULL);
        return -ENODEV;
    }
    in->standby = false;
    return 0;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    return ((const struct joan_stream_in *)stream)->config.rate;
}

static int in_set_sample_rate(struct audio_stream *stream __unused, uint32_t rate __unused)
{
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    const struct joan_stream_in *in = (const struct joan_stream_in *)stream;

    return in->config.period_size * in->config.channels * sizeof(int16_t);
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    const struct joan_stream_in *in = (const struct joan_stream_in *)stream;

    return audio_channel_in_mask_from_count(in->config.channels);
}

static audio_format_t in_get_format(const struct audio_stream *stream __unused)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream __unused, audio_format_t format __unused)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    struct joan_stream_in *in = (struct joan_stream_in *)stream;

    pthread_mutex_lock(&in->dev->lock);
    pthread_mutex_lock(&in->lock);
    do_in_standby(in);
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&in->dev->lock);
    return 0;
}

static int in_dump(const struct audio_stream *stream __unused, int fd __unused)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct joan_stream_in *in = (struct joan_stream_in *)stream;
    struct joan_audio_device *adev = in->dev;
    struct str_parms *parms = str_parms_create_str(kvpairs);
    char value[32];

    if (!parms)
        return -ENOMEM;
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_INPUT_SOURCE, value, sizeof(value)) >= 0)
        in->source = (audio_source_t)atoi(value);
    if (str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value)) >= 0) {
        audio_devices_t devices = (audio_devices_t)strtoul(value, NULL, 0);

        if (devices != AUDIO_DEVICE_NONE && devices != in->devices) {
            if (!in->standby &&
                input_path(devices, in->config.channels) != adev->in_path)
                do_in_standby(in);
            in->devices = devices;
        }
    }
    pthread_mutex_unlock(&in->lock);
    pthread_mutex_unlock(&adev->lock);
    str_parms_destroy(parms);
    return 0;
}

static char *in_get_parameters(const struct audio_stream *stream __unused,
                               const char *keys __unused)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream __unused, float gain __unused)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer, size_t bytes)
{
    struct joan_stream_in *in = (struct joan_stream_in *)stream;
    struct joan_audio_device *adev = in->dev;
    size_t frame_size = audio_stream_in_frame_size(stream);
    int ret = 0;

    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    if (in->standby)
        ret = start_input(in);
    pthread_mutex_unlock(&adev->lock);

    if (ret == 0)
        ret = pcm_read(in->pcm, buffer, bytes);
    if (ret != 0) {
        ALOGE_IF(in->pcm, "pcm_read: %s", in->pcm ? pcm_get_error(in->pcm) : "");
        memset(buffer, 0, bytes);
        pthread_mutex_unlock(&in->lock);
        usleep(bytes * 1000000ULL / frame_size / in_get_sample_rate(&stream->common));
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&in->lock);
        do_in_standby(in);
        pthread_mutex_unlock(&adev->lock);
    } else if (adev->mic_mute) {
        memset(buffer, 0, bytes);
    }
    in->frames_read += bytes / frame_size;
    pthread_mutex_unlock(&in->lock);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream __unused)
{
    return 0;
}

static int in_get_capture_position(const struct audio_stream_in *stream, int64_t *frames,
                                   int64_t *time)
{
    struct joan_stream_in *in = (struct joan_stream_in *)stream;
    struct timespec ts;
    unsigned int avail;
    int ret = -ENOSYS;

    pthread_mutex_lock(&in->lock);
    if (in->pcm && pcm_get_htimestamp(in->pcm, &avail, &ts) == 0) {
        *frames = in->frames_read + avail;
        *time = ts.tv_sec * 1000000000LL + ts.tv_nsec;
        ret = 0;
    }
    pthread_mutex_unlock(&in->lock);
    return ret;
}

static int in_add_audio_effect(const struct audio_stream *stream __unused,
                               effect_handle_t effect __unused)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream __unused,
                                  effect_handle_t effect __unused)
{
    return 0;
}

/* ---------------------------------------------------------------- device */

static int adev_open_output_stream(struct audio_hw_device *dev, audio_io_handle_t handle __unused,
                                   audio_devices_t devices, audio_output_flags_t flags __unused,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address __unused)
{
    struct joan_audio_device *adev = (struct joan_audio_device *)dev;
    struct joan_stream_out *out;

    *stream_out = NULL;
    if (config->sample_rate != SAMPLE_RATE || config->format != AUDIO_FORMAT_PCM_16_BIT ||
        config->channel_mask != AUDIO_CHANNEL_OUT_STEREO) {
        config->sample_rate = SAMPLE_RATE;
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
        return -EINVAL;
    }

    out = calloc(1, sizeof(*out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    pthread_mutex_init(&out->lock, NULL);
    out->dev = adev;
    out->config = out_config_default;
    out->devices = devices;
    out->standby = true;

    pthread_mutex_lock(&adev->lock);
    adev->output = out;
    pthread_mutex_unlock(&adev->lock);

    *stream_out = &out->stream;
    return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
                                     struct audio_stream_out *stream)
{
    struct joan_audio_device *adev = (struct joan_audio_device *)dev;
    struct joan_stream_out *out = (struct joan_stream_out *)stream;

    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    do_out_standby(out);
    pthread_mutex_unlock(&out->lock);
    if (adev->output == out)
        adev->output = NULL;
    pthread_mutex_unlock(&adev->lock);
    pthread_mutex_destroy(&out->lock);
    free(out);
}

static int adev_set_parameters(struct audio_hw_device *dev __unused, const char *kvpairs)
{
    ALOGV("set_parameters: %s", kvpairs);
    return 0;
}

static char *adev_get_parameters(const struct audio_hw_device *dev __unused,
                                 const char *keys __unused)
{
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev __unused)
{
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev __unused, float volume __unused)
{
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev __unused, float volume __unused)
{
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev __unused, float *volume __unused)
{
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev __unused, bool muted __unused)
{
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev __unused, bool *muted __unused)
{
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    struct joan_audio_device *adev = (struct joan_audio_device *)dev;

    pthread_mutex_lock(&adev->lock);
    adev->mode = mode;
    pthread_mutex_unlock(&adev->lock);
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    struct joan_audio_device *adev = (struct joan_audio_device *)dev;

    adev->mic_mute = state;
    return 0;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    const struct joan_audio_device *adev = (const struct joan_audio_device *)dev;

    *state = adev->mic_mute;
    return 0;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev __unused,
                                         const struct audio_config *config)
{
    return IN_PERIOD_SIZE * audio_channel_count_from_in_mask(config->channel_mask) *
           sizeof(int16_t);
}

static int adev_open_input_stream(struct audio_hw_device *dev, audio_io_handle_t handle __unused,
                                  audio_devices_t devices, struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags __unused,
                                  const char *address __unused, audio_source_t source)
{
    struct joan_audio_device *adev = (struct joan_audio_device *)dev;
    struct joan_stream_in *in;
    unsigned int channels;

    *stream_in = NULL;
    channels = audio_channel_count_from_in_mask(config->channel_mask);
    if (config->sample_rate != SAMPLE_RATE || config->format != AUDIO_FORMAT_PCM_16_BIT ||
        (channels != 1 && channels != 2)) {
        config->sample_rate = SAMPLE_RATE;
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        config->channel_mask = AUDIO_CHANNEL_IN_MONO;
        return -EINVAL;
    }

    in = calloc(1, sizeof(*in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;
    in->stream.get_capture_position = in_get_capture_position;

    pthread_mutex_init(&in->lock, NULL);
    in->dev = adev;
    in->config = in_config_default;
    in->config.channels = channels;
    in->devices = devices;
    in->source = source;
    in->standby = true;

    pthread_mutex_lock(&adev->lock);
    adev->input = in;
    pthread_mutex_unlock(&adev->lock);

    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev, struct audio_stream_in *stream)
{
    struct joan_audio_device *adev = (struct joan_audio_device *)dev;
    struct joan_stream_in *in = (struct joan_stream_in *)stream;

    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&in->lock);
    do_in_standby(in);
    pthread_mutex_unlock(&in->lock);
    if (adev->input == in)
        adev->input = NULL;
    pthread_mutex_unlock(&adev->lock);
    pthread_mutex_destroy(&in->lock);
    free(in);
}

static int adev_dump(const audio_hw_device_t *device __unused, int fd __unused)
{
    return 0;
}

static int adev_close(hw_device_t *device)
{
    struct joan_audio_device *adev = (struct joan_audio_device *)device;

    if (adev->ar)
        audio_route_free(adev->ar);
    free(adev);
    return 0;
}

static int adev_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
    struct joan_audio_device *adev;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(*adev));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *)module;
    adev->hw_device.common.close = adev_close;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.get_master_volume = adev_get_master_volume;
    adev->hw_device.set_master_mute = adev_set_master_mute;
    adev->hw_device.get_master_mute = adev_get_master_mute;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    pthread_mutex_init(&adev->lock, NULL);
    adev->mode = AUDIO_MODE_NORMAL;
    adev->card = -1;

    /*
     * Without a sound card (ADSP or codec not up) keep running with no
     * routes: the streams then play into the void at real-time pace
     * instead of failing audioserver, which would block boot.
     */
    if (ensure_routes(adev) != 0)
        ALOGW("audio card not ready; streams will retry route initialization");

    *device = &adev->hw_device.common;
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "LG V30 audio HAL",
        .author = "The LineageOS Project",
        .methods = &hal_module_methods,
    },
};
