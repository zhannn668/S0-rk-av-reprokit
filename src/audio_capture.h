// src/audio_capture.h
#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(__has_include)
#  if __has_include(<alsa/asoundlib.h>)
#    include <alsa/asoundlib.h>
#    define RK_ALSA_AVAILABLE 1
#  else
#    define RK_ALSA_AVAILABLE 0
#  endif
#else
#  include <alsa/asoundlib.h>
#  define RK_ALSA_AVAILABLE 1
#endif

#if !RK_ALSA_AVAILABLE
typedef struct _snd_pcm snd_pcm_t;
typedef int snd_pcm_format_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
#endif

typedef struct {
    snd_pcm_t          *handle;
    unsigned int        sample_rate;
    int                 channels;
    snd_pcm_format_t    format;
    snd_pcm_uframes_t   frames_per_period;
    size_t              bytes_per_frame;
} AudioCapture;

/**
 * 打开 ALSA 采集设备
 *  device: "hw:0,0" 这种
 */
int audio_capture_open(AudioCapture *ac,
                       const char *device,
                       unsigned int sample_rate,
                       int channels);

/**
 * 从设备读取一段 PCM 数据（阻塞）
 *  buf:   输出缓冲区
 *  bytes: 期望读取的字节数（建议是 frames_per_period * bytes_per_frame 的整数倍）
 * 返回: 实际读取的字节数，<0 表示出错
 */
ssize_t audio_capture_read(AudioCapture *ac, uint8_t *buf, size_t bytes);

/** 关闭设备，释放资源 */
void audio_capture_close(AudioCapture *ac);
