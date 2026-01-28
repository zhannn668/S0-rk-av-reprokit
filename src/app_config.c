// app_config.c
#include "app_config.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>

/*
 * 解析形如 "WxH" 的分辨率字符串（例如 "1920x1080"）。
 *
 * @param s  输入字符串
 * @param w  输出宽度
 * @param h  输出高度
 * @return   0 成功；-1 失败（格式不对或宽高非正数）
 */
static int parse_size(const char *s, int *w, int *h)
{
    if (!s || !w || !h) return -1;
    int ww = 0, hh = 0;
    if (sscanf(s, "%dx%d", &ww, &hh) != 2) return -1;
    if (ww <= 0 || hh <= 0) return -1;
    *w = ww;
    *h = hh;
    return 0;
}

/*
 * 加载默认配置。
 *
 * 注意：该函数会先 memset(cfg, 0, sizeof(*cfg))，再写入默认值。
 * 通常程序启动时应先调用本函数，再调用 app_config_parse_args 覆盖用户参数。
 *
 * @param cfg  配置结构体指针
 * @return     0 成功；-1 失败
 */
int app_config_load_default(AppConfig *cfg) //预设默认值
{
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    cfg->video_device = "/dev/video0";
    cfg->width        = 1280;
    cfg->height       = 720;
    cfg->fps          = 30;
    cfg->bitrate      = 2000000;   // 2Mbps default
    cfg->v4l2_fourcc  = 0;         // auto

    cfg->audio_device   = "hw:0,0";
    cfg->sample_rate    = 48000;
    cfg->channels       = 2;
    cfg->audio_chunk_ms = 20;

    cfg->sink_type        = "file";
    cfg->output_path_h264 = "out.h264";
    cfg->output_path_pcm  = "out.pcm";
    cfg->duration_sec     = 10;

    return 0;
}

/*
 * 打印命令行帮助信息。
 *
 * @param prog  程序名（一般传 argv[0]）
 */
void app_config_print_usage(const char *prog) //当用户传 -h/--help 或者遇到未知参数时会用到
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [options]\n\n"
        "Options:\n"
        "  --video-dev <path>       Video device node (default: /dev/video0)\n"
        "  --size <WxH>             Capture size (default: 1280x720)\n"
        "  --fps <n>                Capture fps (default: 30)\n"
        "  --bitrate <bps>          H.264 target bitrate (default: 2000000)\n"
        "  --audio-dev <dev>        ALSA capture device (default: hw:0,0)\n"
        "  --sr <hz>                Audio sample rate (default: 48000)\n"
        "  --ch <n>                 Audio channels (default: 2)\n"
        "  --sec <n>                Record duration seconds (default: 10)\n"
        "  --out-h264 <file>        Output H.264 file (default: out.h264)\n"
        "  --out-pcm <file>         Output PCM file (default: out.pcm)\n"
        "  -h, --help               Show this help\n\n"
        "Examples:\n"
        "  %s --video-dev /dev/video0 --size 1920x1080 --fps 30 --bitrate 4000000 --sec 10\n"
        "  %s --out-h264 out.h264 --out-pcm out.pcm --sec 10\n",
        prog, prog, prog);
}

/*
 * 解析命令行参数并写入 cfg。
 *
 * 设计思路：
 * - 使用 getopt_long() 支持长选项（--xxx）与短选项（-h）。
 * - 只覆盖用户显式传入的字段；解析结束后对部分字段做兜底默认与合法性校验。
 *
 * 重要约定：调用者应先调用 app_config_load_default(cfg)，保证 cfg 里有默认值。
 *
 * @param cfg   配置结构体（输入/输出）
 * @param argc  参数个数
 * @param argv  参数数组
 * @return      0 成功；-1 失败（如 --size 格式不合法、或宽高非法等）
 */
int app_config_parse_args(AppConfig *cfg, int argc, char **argv)
{
    if (!cfg) return -1;

    /*
     * 长选项返回值：从 1000 起是为了避开 ASCII 短选项字符（例如 'h'）。
     * getopt_long() 解析到某个长选项时，会返回这里定义的值。
     */
    enum {
        OPT_VIDEO_DEV = 1000,
        OPT_SIZE,
        OPT_FPS,
        OPT_BITRATE,
        OPT_AUDIO_DEV,
        OPT_SR,
        OPT_CH,
        OPT_SEC,
        OPT_OUT_H264,
        OPT_OUT_PCM,
    };

    /*
     * 长选项表：
     * - name:  选项名（对应 --name）
     * - has_arg: required_argument 表示必须带参数；no_argument 表示不带参数
     * - flag:  这里为 0，表示 getopt_long 直接返回 val
     * - val:   返回值（上面 enum 或字符，如 'h'）
     */
    static const struct option long_opts[] = {
        {"video-dev", required_argument, 0, OPT_VIDEO_DEV},
        {"size",      required_argument, 0, OPT_SIZE},
        {"fps",       required_argument, 0, OPT_FPS},
        {"bitrate",   required_argument, 0, OPT_BITRATE},
        {"audio-dev", required_argument, 0, OPT_AUDIO_DEV},
        {"sr",        required_argument, 0, OPT_SR},
        {"ch",        required_argument, 0, OPT_CH},
        {"sec",       required_argument, 0, OPT_SEC},
        {"out-h264",  required_argument, 0, OPT_OUT_H264},
        {"out-pcm",   required_argument, 0, OPT_OUT_PCM},
        {"help",      no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    /*
     * 解析循环：
     * - 返回值 c 是当前解析到的选项（long_opts 中的 val 或短选项字符）
     * - 对于 required_argument 的选项，参数字符串通过全局变量 optarg 提供
     */
    while ((c = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
        switch (c) {
        case OPT_VIDEO_DEV:  cfg->video_device = optarg; break;
        case OPT_SIZE:
            if (parse_size(optarg, &cfg->width, &cfg->height) != 0) {
                LOGE("[CFG] invalid --size: %s", optarg);
                return -1;
            }
            break;
        case OPT_FPS:       cfg->fps = atoi(optarg); break;
        case OPT_BITRATE:   cfg->bitrate = atoi(optarg); break;
        case OPT_AUDIO_DEV: cfg->audio_device = optarg; break;
        case OPT_SR:        cfg->sample_rate = (unsigned int)atoi(optarg); break;
        case OPT_CH:        cfg->channels = (unsigned int)atoi(optarg); break;
        case OPT_SEC:       cfg->duration_sec = (unsigned int)atoi(optarg); break;
        case OPT_OUT_H264:  cfg->output_path_h264 = optarg; break;
        case OPT_OUT_PCM:   cfg->output_path_pcm = optarg; break;
        case 'h':
        default:
            /* -h/--help 或未知参数：打印用法并退出进程。 */
            app_config_print_usage(argv[0]);
            exit(0);
        }
    }

    /* 解析结束后兜底默认值与合法性校验。 */
    if (cfg->fps <= 0) cfg->fps = 30;
    if (cfg->width <= 0 || cfg->height <= 0) {
        LOGE("[CFG] invalid size: %dx%d", cfg->width, cfg->height);
        return -1;
    }
    if (cfg->bitrate <= 0) cfg->bitrate = 2000000;
    if (cfg->sample_rate == 0) cfg->sample_rate = 48000;
    if (cfg->channels == 0) cfg->channels = 2;

    return 0;
}

/*
 * 打印配置摘要（方便启动时确认最终生效的参数）。
 *
 * @param cfg  配置结构体（只读）
 */
void app_config_print_summary(const AppConfig *cfg)
{
    if (!cfg) return;
    LOGI("[CFG] video=%s %dx%d@%d bitrate=%d | audio=%s %uHz ch=%u | out=%s,%s | sec=%u",
         cfg->video_device ? cfg->video_device : "(null)",
         cfg->width, cfg->height, cfg->fps,
         cfg->bitrate,
         cfg->audio_device ? cfg->audio_device : "(null)",
         cfg->sample_rate, cfg->channels,
         cfg->output_path_h264 ? cfg->output_path_h264 : "(null)",
         cfg->output_path_pcm ? cfg->output_path_pcm : "(null)",
         cfg->duration_sec);
}
