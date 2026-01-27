// src/sink.c
#include "sink.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>

int enc_sink_init(EncSink *sink, EncSinkType type, const char *target)
{
    if (!sink) return -1;

    memset(sink, 0, sizeof(*sink));
    sink->type = type;

    if (target) {
        strncpy(sink->target, target, sizeof(sink->target) - 1);
        sink->target[sizeof(sink->target) - 1] = '\0';
    }

    return 0;
}

int enc_sink_open(EncSink *sink)
{
    if (!sink) return -1;

    switch (sink->type) {
    case ENC_SINK_FILE:
        sink->file_fp = fopen(sink->target, "wb");
        if (!sink->file_fp) {
            LOGE("open file failed: %s", sink->target);
            return -1;
        }
        LOGI("file sink opened: %s", sink->target);
        break;

    case ENC_SINK_PIPE_FFMPEG:
        // 这一版先不实现，后面做 RTMP 再填
        LOGW("PIPE_FFMPEG not implemented yet");
        return -1;

    case ENC_SINK_NONE:
    default:
        LOGW("no sink type selected");
        break;
    }

    return 0;
}

int enc_sink_write(EncSink *sink, const uint8_t *data, size_t len)
{
    if (!sink || !data || !len) return -1;

    size_t written = 0;

    switch (sink->type) {
    case ENC_SINK_FILE:
        if (!sink->file_fp) return -1;
        written = fwrite(data, 1, len, sink->file_fp);
        break;

    case ENC_SINK_PIPE_FFMPEG:
    case ENC_SINK_NONE:
    default:
        return 0;  // 暂时什么都不做
    }

    if (written != len) {
        LOGW("partial write: %zu/%zu", written, len);
        return -1;
    }

    return 0;
}

void enc_sink_close(EncSink *sink)
{
    if (!sink) return;

    if (sink->file_fp) {
        fclose(sink->file_fp);
        sink->file_fp = NULL;
    }
    if (sink->pipe_fp) {
        // 之后用 pclose
        fclose(sink->pipe_fp);
        sink->pipe_fp = NULL;
    }

    LOGI("sink closed");
}