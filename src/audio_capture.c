// src/audio_capture.c
#include "audio_capture.h"
#include "log.h"

#include <string.h>

/* ssize_t 在不同平台的声明位置不同：
 * - Linux/Unix: 通常来自 <sys/types.h>
 * - Windows: MSVC 环境可用 SSIZE_T
 */
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

#define TAG "audio"

#if !RK_ALSA_AVAILABLE

/*
 * 打开音频采集（当 ALSA 不可用时的占位实现）。
 *
 * 该分支用于：编译环境缺少 ALSA 头文件/库时，给出明确错误提示。
 *
 * @param ac          采集上下文（未使用）
 * @param device      ALSA 设备名（未使用）
 * @param sample_rate 采样率（未使用）
 * @param channels    声道数（未使用）
 * @return            -1 表示不可用
 */
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

/*
 * 读取音频数据（当 ALSA 不可用时的占位实现）。
 *
 * @param ac     采集上下文（未使用）
 * @param buf    输出缓冲（未使用）
 * @param bytes  期望读取字节数（未使用）
 * @return       -1 表示不可用
 */
ssize_t audio_capture_read(AudioCapture *ac, uint8_t *buf, size_t bytes)
{
    (void)ac;
    (void)buf;
    (void)bytes;
    LOGE("[%s] ALSA not available.", TAG);
    return -1;
}

/*
 * 关闭音频采集（当 ALSA 不可用时为 no-op）。
 */
void audio_capture_close(AudioCapture *ac)
{
    (void)ac;
}

#else

/*
 * 打开并初始化 ALSA PCM 采集。
 *
 * 典型流程：
 * 1) snd_pcm_open 打开采集设备
 * 2) 配置硬件参数（交错格式/采样格式/声道/采样率/period size）
 * 3) 计算 bytes_per_frame（每个采样帧字节数）
 *
 * @param ac          输出：采集上下文
 * @param device      ALSA 设备名（例如 "hw:0,0"）
 * @param sample_rate 期望采样率（驱动可能会近似调整）
 * @param channels    声道数
 * @return            0 成功；-1 失败
 */
int audio_capture_open(AudioCapture *ac,
                       const char *device,
                       unsigned int sample_rate,
                       int channels)
{
    if (!ac || !device) return -1;
    memset(ac, 0, sizeof(*ac));

    ac->sample_rate = sample_rate;
    ac->channels    = channels;
    /* 当前固定为 16-bit little-endian，交错格式（LRLR...）。 */
    ac->format      = SND_PCM_FORMAT_S16_LE;  // 16bit 小端
    /* period 可以按延迟/吞吐需求调整：越小延迟越低但系统开销越大。 */
    ac->frames_per_period = 1024;            // 可以随便调

    int err;

    /* 1) 打开 PCM 采集设备 */
    if ((err = snd_pcm_open(&ac->handle, device,
                            SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        LOGE("[%s] snd_pcm_open(%s) failed: %s",
             TAG, device, snd_strerror(err));
        return -1;
    }

    /* 2) 配置硬件参数 */
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

    /* bytes_per_frame：每个“采样帧”的字节数 = (位宽/8) * 声道数 */
    ac->bytes_per_frame =
        snd_pcm_format_width(ac->format) / 8 * ac->channels; // e.g. 2ch*2B = 4B

    LOGI("[%s] opened device=%s, %u Hz, ch=%d, period=%lu frames, %zu B/frame",
         TAG, device, ac->sample_rate, ac->channels,
         (unsigned long)ac->frames_per_period, ac->bytes_per_frame);

    return 0;
}

/*
 * 从 ALSA 采集设备读取音频数据。
 *
 * @param ac     采集上下文
 * @param buf    输出缓冲
 * @param bytes  期望读取字节数（会按 bytes_per_frame 换算为帧数读取）
 * @return       >0 实际读取字节数；0 表示本次请求不足以构成 1 帧；-1 失败
 */
ssize_t audio_capture_read(AudioCapture *ac, uint8_t *buf, size_t bytes)
{
    if (!ac || !ac->handle || !buf || bytes == 0) return -1;

    /* ALSA 的 readi 按“帧数”读取，这里把字节数换算成帧数。 */
    size_t frames_to_read = bytes / ac->bytes_per_frame;
    if (frames_to_read == 0) return 0;

    snd_pcm_sframes_t n = snd_pcm_readi(ac->handle, buf, frames_to_read);
    if (n < 0) {
        /* 发生 underrun/设备暂停等错误时，尝试恢复一次（允许重启 stream）。 */
        n = snd_pcm_recover(ac->handle, n, 1);
        if (n < 0) {
            LOGE("[%s] snd_pcm_readi failed: %s",
                 TAG, snd_strerror((int)n));
            return -1;
        }
    }

    /* 返回实际读取到的字节数。 */
    return n * ac->bytes_per_frame;
}

/*
 * 关闭 ALSA 采集并清理上下文。
 */
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
