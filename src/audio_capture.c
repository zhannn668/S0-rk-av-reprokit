// src/audio_capture.c
#include "audio_capture.h"
#include "log.h"

#include <string.h>

#define TAG "audio"

#if !RK_ALSA_AVAILABLE
int audio_capture_open(AudioCapture *ac,
                       const char *device,
                       unsigned int sample_rate,
                       int channels)
{
    (void)ac;
    (void)device;
    (void)sample_rate;
    (void)channels;
    LOGE("[%s] ALSA headers not found. Please install ALSA dev package.", TAG);
    return -1;
}

ssize_t audio_capture_read(AudioCapture *ac, uint8_t *buf, size_t bytes)
{
    (void)ac;
    (void)buf;
    (void)bytes;
    LOGE("[%s] ALSA not available.", TAG);
    return -1;
}

void audio_capture_close(AudioCapture *ac)
{
    (void)ac;
}

#else

int audio_capture_open(AudioCapture *ac,
                       const char *device,
                       unsigned int sample_rate,
                       int channels)
{
    if (!ac || !device) return -1;
    memset(ac, 0, sizeof(*ac));

    ac->sample_rate = sample_rate;
    ac->channels    = channels;
    ac->format      = SND_PCM_FORMAT_S16_LE;  // 16bit 小端
    ac->frames_per_period = 1024;            // 可以随便调

    int err;

    // 1. 打开 PCM 采集设备
    if ((err = snd_pcm_open(&ac->handle, device,
                            SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        LOGE("[%s] snd_pcm_open(%s) failed: %s",
             TAG, device, snd_strerror(err));
        return -1;
    }

    // 2. 配置硬件参数
    snd_pcm_hw_params_t *hwparams = NULL;
    snd_pcm_hw_params_alloca(&hwparams);

    snd_pcm_hw_params_any(ac->handle, hwparams);
    snd_pcm_hw_params_set_access(ac->handle, hwparams,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(ac->handle, hwparams, ac->format);
    snd_pcm_hw_params_set_channels(ac->handle, hwparams, ac->channels);
    snd_pcm_hw_params_set_rate_near(ac->handle, hwparams,
                                    &ac->sample_rate, NULL);
    snd_pcm_hw_params_set_period_size_near(ac->handle, hwparams,
                                           &ac->frames_per_period, NULL);

    if ((err = snd_pcm_hw_params(ac->handle, hwparams)) < 0) {
        LOGE("[%s] snd_pcm_hw_params failed: %s",
             TAG, snd_strerror(err));
        snd_pcm_close(ac->handle);
        ac->handle = NULL;
        return -1;
    }

    ac->bytes_per_frame =
        snd_pcm_format_width(ac->format) / 8 * ac->channels; // e.g. 2ch*2B = 4B

    LOGI("[%s] opened device=%s, %u Hz, ch=%d, period=%lu frames, %zu B/frame",
         TAG, device, ac->sample_rate, ac->channels,
         (unsigned long)ac->frames_per_period, ac->bytes_per_frame);

    return 0;
}

ssize_t audio_capture_read(AudioCapture *ac, uint8_t *buf, size_t bytes)
{
    if (!ac || !ac->handle || !buf || bytes == 0) return -1;

    size_t frames_to_read = bytes / ac->bytes_per_frame;
    if (frames_to_read == 0) return 0;

    snd_pcm_sframes_t n = snd_pcm_readi(ac->handle, buf, frames_to_read);
    if (n < 0) {
        // 尝试恢复一次
        n = snd_pcm_recover(ac->handle, n, 1);
        if (n < 0) {
            LOGE("[%s] snd_pcm_readi failed: %s",
                 TAG, snd_strerror((int)n));
            return -1;
        }
    }

    return n * ac->bytes_per_frame;
}

void audio_capture_close(AudioCapture *ac)
{
    if (!ac) return;

    if (ac->handle) {
        snd_pcm_close(ac->handle);
        ac->handle = NULL;
    }

    memset(ac, 0, sizeof(*ac));
    LOGI("[%s] audio capture closed", TAG);
}

#endif
