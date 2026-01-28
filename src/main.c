#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

#include "log.h"
#include "app_config.h"
#include "av_stats.h"
#include "v4l2_capture.h"
#include "encoder_mpp.h"
#include "sink.h"
#include "audio_capture.h"

static volatile sig_atomic_t g_stop = 0;
static AvStats g_stats;

/*
 * 信号处理函数：收到 SIGINT/SIGTERM 时设置全局停止标志。
 *
 * 约束：信号处理上下文里应尽量只做“最小且异步安全”的操作。
 * 这里仅写一个 sig_atomic_t 标志位，供各线程轮询并退出。
 */
static void on_sigint(int signo)
{
    (void)signo;
    g_stop = 1;
}

/* ===================== Timer Thread ===================== */
typedef struct {
    unsigned int sec;
} TimerArgs;

/*
 * 计时器线程：睡眠指定秒数后将 g_stop 置 1，触发全局停止。
 *
 * @param arg  TimerArgs*，包含录制时长（秒）
 */
static void *timer_thread(void *arg)
{
    TimerArgs *t = (TimerArgs *)arg;
    if (!t || t->sec == 0) return NULL;
    sleep(t->sec);
    g_stop = 1;
    return NULL;
}

/* ===================== Stats Thread ===================== */
/*
 * 统计线程：每秒打印一次统计信息，直到 g_stop 被置位。
 */
static void *stats_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        sleep(1);
        av_stats_tick_print(&g_stats);
    }
    return NULL;
}

/* ===================== Audio Thread ===================== */
typedef struct {
    const AppConfig *cfg;
} AudioArgs;

/*
 * 音频线程：
 * - 打开 ALSA 音频采集
 * - 按 chunk 读取 PCM 并写入文件
 * - 达到时长限制或收到停止信号后退出
 *
 * @param arg  AudioArgs*，包含 AppConfig 指针
 */
static void *audio_thread(void *arg)
{
    AudioArgs *a = (AudioArgs *)arg;
    const AppConfig *cfg = a ? a->cfg : NULL;
    if (!cfg) return NULL;

    AudioCapture ac;
    if (audio_capture_open(&ac, cfg->audio_device, cfg->sample_rate, cfg->channels) != 0) {
        LOGE("[audio] audio_capture_open failed");
        av_stats_add_drop(&g_stats, 1);
        return NULL;
    }

    FILE *af = fopen(cfg->output_path_pcm, "wb");
    if (!af) {
        LOGE("[audio] fopen %s failed", cfg->output_path_pcm);
        audio_capture_close(&ac);
        av_stats_add_drop(&g_stats, 1);
        return NULL;
    }

    LOGI("[audio] start capture -> %s", cfg->output_path_pcm);

    /*
     * 计算目标写入字节数：duration_sec>0 则按秒数限制；否则认为无限（直到外部 stop）。
     * bytes_per_frame 表示“每个采样帧”的字节数（与位深/声道有关）。
     */
    size_t bytes_per_sec = (size_t)ac.sample_rate * (size_t)ac.bytes_per_frame;
    size_t total_bytes   = (cfg->duration_sec > 0) ? (bytes_per_sec * (size_t)cfg->duration_sec) : (size_t)-1;

    /* 单次读写的 chunk 大小（按 ALSA period 计算）。 */
    size_t chunk = (size_t)ac.frames_per_period * (size_t)ac.bytes_per_frame;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    if (!buf) {
        LOGE("[audio] malloc failed");
        fclose(af);
        audio_capture_close(&ac);
        av_stats_add_drop(&g_stats, 1);
        return NULL;
    }

    size_t written = 0;
    while (!g_stop && written < total_bytes) {
        ssize_t n = audio_capture_read(&ac, buf, chunk);
        if (n <= 0) {
            /* 读不到数据就小睡一下，避免忙等占满 CPU。 */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 }; // 1ms
            nanosleep(&ts, NULL);

            continue;
        }
        size_t wn = fwrite(buf, 1, (size_t)n, af);
        if (wn != (size_t)n) {
            LOGE("[audio] fwrite failed");
            av_stats_add_drop(&g_stats, 1);
            break;
        }
        written += wn;
        /* 音频 chunk 统计：用于每秒打印的速率与累计。 */
        av_stats_inc_audio_chunk(&g_stats);
    }

    free(buf);
    fclose(af);
    audio_capture_close(&ac);

    LOGI("[audio] done, bytes=%zu", written);
    return NULL;
}

/* ===================== Video Thread ===================== */
typedef struct {
    const AppConfig *cfg;
} VideoArgs;

/*
 * 视频线程：
 * - 打开 V4L2 采集（当前使用 NV12M 多平面，内部合成为连续 NV12）
 * - 初始化 MPP H.264 编码器
 * - 采集帧 -> 编码 -> 写入 sink（文件）
 * - 达到时长/帧数限制或收到停止信号后退出
 *
 * @param arg  VideoArgs*，包含 AppConfig 指针
 */
static void *video_thread(void *arg)
{
    VideoArgs *v = (VideoArgs *)arg;
    const AppConfig *cfg = v ? v->cfg : NULL;
    if (!cfg) return NULL;

    V4L2Capture cap;
    EncoderMPP  enc;
    EncSink     sink;

    int ret;

    ret = v4l2_capture_open(&cap, cfg->video_device, (unsigned int)cfg->width, (unsigned int)cfg->height);
    if (ret) {
        LOGE("[video] v4l2_capture_open failed: %s", cfg->video_device);
        av_stats_add_drop(&g_stats, 1);
        return NULL;
    }
    v4l2_capture_start(&cap);

    /* 初始化硬编码器：当前选择 AVC(H.264)。 */
    ret = encoder_mpp_init(&enc, cfg->width, cfg->height, cfg->fps, cfg->bitrate, MPP_VIDEO_CodingAVC);
    if (ret) {
        LOGE("[video] encoder_mpp_init failed");
        v4l2_capture_close(&cap);
        av_stats_add_drop(&g_stats, 1);
        return NULL;
    }

    /* 输出端：当前落盘为 .h264 文件。 */
    enc_sink_init(&sink, ENC_SINK_FILE, cfg->output_path_h264);
    if (enc_sink_open(&sink) != 0) {
        LOGE("[video] enc_sink_open failed: %s", cfg->output_path_h264);
        encoder_mpp_deinit(&enc);
        v4l2_capture_close(&cap);
        av_stats_add_drop(&g_stats, 1);
        return NULL;
    }

    LOGI("[video] start encode -> %s (%dx%d@%d)", cfg->output_path_h264, cfg->width, cfg->height, cfg->fps);

    uint32_t last_seq = 0;
    int      has_seq = 0;
    /* 若配置了 sec 与 fps，则将录制时长转换为目标帧数；0 表示不限制（直到 stop）。 */
    int      frames_target = (cfg->duration_sec > 0 && cfg->fps > 0) ? (int)(cfg->duration_sec * (unsigned int)cfg->fps) : 0;
    int      frames = 0;

    while (!g_stop && (frames_target == 0 || frames < frames_target)) {
        int index;
        void *data;
        size_t len;

        ret = v4l2_capture_dqbuf(&cap, &index, &data, &len);
        if (ret != 0) {
            /* 非阻塞采集：可能暂时无帧（EAGAIN），小睡一下再试。 */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000 * 1000 }; // 1ms
            nanosleep(&ts, NULL);

            continue;
        }

        /* drop 统计：根据 v4l2 sequence 检测丢帧（序号跳变）。 */
        if (!has_seq) {
            last_seq = cap.last_sequence;
            has_seq = 1;
        } else {
            uint32_t cur = cap.last_sequence;
            if (cur > last_seq + 1) {
                av_stats_add_drop(&g_stats, (uint64_t)(cur - last_seq - 1));
            }
            last_seq = cur;
        }

        size_t out_bytes = 0;
        ret = encoder_mpp_encode(&enc, (const uint8_t *)data, len, &sink, &out_bytes);
        if (ret != 0) {
            av_stats_add_drop(&g_stats, 1);
        } else {
            av_stats_inc_video_frame(&g_stats);
            av_stats_add_enc_bytes(&g_stats, (uint64_t)out_bytes);
        }

        v4l2_capture_qbuf(&cap, index);
        frames++;
    }

    LOGI("[video] done, frames=%d", frames);

    enc_sink_close(&sink);
    encoder_mpp_deinit(&enc);
    v4l2_capture_close(&cap);
    return NULL;
}

/* ===================== Main ===================== */
/*
 * 程序入口：
 * - 注册信号处理（Ctrl+C 停止）
 * - 加载默认配置并解析命令行
 * - 启动线程：统计/定时停止/视频/音频
 * - 等待线程退出并收尾
 */
int main(int argc, char **argv)
{
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    AppConfig cfg;
    /* 先加载默认值，再用命令行参数覆盖。 */
    app_config_load_default(&cfg);
    if (app_config_parse_args(&cfg, argc, argv) != 0) {
        app_config_print_usage(argv[0]);
        return -1;
    }

    // 最终配置摘要（你要求的那一行）
    app_config_print_summary(&cfg);

    av_stats_init(&g_stats);

    pthread_t th_v, th_a, th_s, th_t;
    TimerArgs targs = { .sec = cfg.duration_sec };

    // stats thread
    if (pthread_create(&th_s, NULL, stats_thread, NULL) != 0) {
        LOGE("[main] pthread_create stats failed");
        return -1;
    }

    // timer thread（录制到点自动停）
    if (cfg.duration_sec > 0) {
        if (pthread_create(&th_t, NULL, timer_thread, &targs) != 0) {
            LOGE("[main] pthread_create timer failed");
            g_stop = 1;
            pthread_join(th_s, NULL);
            return -1;
        }
    }

    VideoArgs vargs = { .cfg = &cfg };
    AudioArgs aargs = { .cfg = &cfg };

    if (pthread_create(&th_v, NULL, video_thread, &vargs) != 0) {
        LOGE("[main] pthread_create video failed");
        g_stop = 1;
        pthread_join(th_s, NULL);
        if (cfg.duration_sec > 0) pthread_join(th_t, NULL);
        return -1;
    }
    if (pthread_create(&th_a, NULL, audio_thread, &aargs) != 0) {
        LOGE("[main] pthread_create audio failed");
        g_stop = 1;
        pthread_join(th_v, NULL);
        pthread_join(th_s, NULL);
        if (cfg.duration_sec > 0) pthread_join(th_t, NULL);
        return -1;
    }

    pthread_join(th_a, NULL);
    /* 音频线程结束后，确保停止标志置位，促使其他线程尽快退出。 */
    g_stop = 1; // ensure stop
    pthread_join(th_v, NULL);

    // stop stats
    pthread_join(th_s, NULL);
    if (cfg.duration_sec > 0) pthread_join(th_t, NULL);

    LOGI("[main] done. video=%s audio=%s", cfg.output_path_h264, cfg.output_path_pcm);
    return 0;
}
