// src/sink.h
#pragma once

#include <stdio.h>
#include <stdint.h>

typedef enum {
    ENC_SINK_NONE = 0,
    ENC_SINK_FILE,        // 写本地文件（本步只实现这个）
    ENC_SINK_PIPE_FFMPEG, // 预留：之后用 popen(ffmpeg)
} EncSinkType;

typedef struct {
    EncSinkType type;
    char target[512];

    FILE *file_fp;
    FILE *pipe_fp;

} EncSink;

int enc_sink_init(EncSink *sink, EncSinkType type, const char *target);
int enc_sink_open(EncSink *sink);
int enc_sink_write(EncSink *sink, const uint8_t *data, size_t size);
void enc_sink_close(EncSink *sink);