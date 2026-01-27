// v4l2_capture.h
#pragma once

#include <stddef.h>
#include <stdint.h>

#define V4L2_MAX_BUFS    8
#define V4L2_MAX_PLANES  2      // NV12M 用 2 个 plane：Y + UV

typedef struct {
    void  *planes[V4L2_MAX_PLANES];   // 每个 plane 的起始地址
    size_t lengths[V4L2_MAX_PLANES];  // 每个 plane 的 mmap 长度
} V4L2Buf;

typedef struct {
    int           fd;

    unsigned int  width;
    unsigned int  height;

    unsigned int  buf_count;
    int           last_index;

    V4L2Buf       bufs[V4L2_MAX_BUFS];

    // 合成后的连续 NV12 帧缓冲
    uint8_t      *nv12_frame;
    size_t        frame_size;

    // 最近一次 DQBUF 的 sequence（用于 drop 统计）
    uint32_t      last_sequence;
} V4L2Capture;

int  v4l2_capture_open (V4L2Capture *cap, const char *dev,
                        unsigned int width, unsigned int height);
int  v4l2_capture_start(V4L2Capture *cap);
int  v4l2_capture_dqbuf(V4L2Capture *cap, int *index,
                        void **data, size_t *length);
int  v4l2_capture_qbuf (V4L2Capture *cap, int index);
void v4l2_capture_dump_format(V4L2Capture *cap);
void v4l2_capture_close(V4L2Capture *cap);
