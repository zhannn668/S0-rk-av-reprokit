// app_config.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* video */
    const char *video_device;      // e.g. "/dev/video0"
    int         width;
    int         height;
    int         fps;
    int         bitrate;           // bps, e.g. 2000000
    uint32_t    v4l2_fourcc;       // 0=auto（预留）

    /* audio */
    const char *audio_device;      // e.g. "hw:0,0"
    unsigned int sample_rate;      // e.g. 48000
    unsigned int channels;         // e.g. 2
    unsigned int audio_chunk_ms;   // stats purpose (best-effort)

    /* output */
    const char *sink_type;         // "file" / "pipe" (reserved)
    const char *output_path_h264;  // e.g. "out.h264"
    const char *output_path_pcm;   // e.g. "out.pcm"
    unsigned int duration_sec;     // default 10
} AppConfig;

int  app_config_load_default(AppConfig *cfg);
int  app_config_parse_args(AppConfig *cfg, int argc, char **argv);
void app_config_print_summary(const AppConfig *cfg);
void app_config_print_usage(const char *prog);

#ifdef __cplusplus
}
#endif
