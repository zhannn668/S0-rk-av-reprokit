// src/v4l2_capture.c
#include "v4l2_capture.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/videodev2.h>

#define TAG "v4l2"

#ifndef VIDEO_MAX_PLANES
#define VIDEO_MAX_PLANES 8
#endif

static int xioctl(int fd, int req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}


static void fourcc_to_str(uint32_t fourcc, char out[5])
{
    out[0] = (fourcc) & 0xff;
    out[1] = (fourcc >> 8) & 0xff;
    out[2] = (fourcc >> 16) & 0xff;
    out[3] = (fourcc >> 24) & 0xff;
    out[4] = '\0';
}

void v4l2_capture_dump_format(V4L2Capture *cap)
{
    if (!cap || cap->fd < 0) return;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(cap->fd, VIDIOC_G_FMT, &fmt) < 0) {
        LOGW("[%s] VIDIOC_G_FMT failed: %s", TAG, strerror(errno));
        return;
    }

    const struct v4l2_pix_format_mplane *p = &fmt.fmt.pix_mp;
    char fourcc[5];
    fourcc_to_str(p->pixelformat, fourcc);

    LOGI("[%s] device fmt: fourcc=%s w=%u h=%u num_planes=%u", TAG,
         fourcc, p->width, p->height, p->num_planes);

    for (uint32_t i = 0; i < p->num_planes && i < VIDEO_MAX_PLANES; i++) {
        LOGI("[%s] plane[%u]: bytesperline(stride)=%u sizeimage=%u", TAG,
             i, p->plane_fmt[i].bytesperline, p->plane_fmt[i].sizeimage);
    }
}

int v4l2_capture_open(V4L2Capture *cap, const char *dev,
                      unsigned int width, unsigned int height)
{
    if (!cap || !dev) return -1;

    memset(cap, 0, sizeof(*cap));
    cap->fd = -1;

    cap->fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if (cap->fd < 0) {
        LOGE("[%s] open %s failed: %s", TAG, dev, strerror(errno));
        return -1;
    }

    /* 设置格式：NV12M 多平面 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = width;
    fmt.fmt.pix_mp.height      = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    fmt.fmt.pix_mp.num_planes  = 2;

    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOGE("[%s] VIDIOC_S_FMT failed: %s", TAG, strerror(errno));
        close(cap->fd);
        cap->fd = -1;
        return -1;
    }

    cap->width  = width;
    cap->height = height;
    cap->frame_size = width * height * 3 / 2;

    cap->nv12_frame = (uint8_t *)malloc(cap->frame_size);
    if (!cap->nv12_frame) {
        LOGE("[%s] malloc nv12_frame failed", TAG);
        close(cap->fd);
        cap->fd = -1;
        return -1;
    }

    LOGI("[%s] format set: %ux%u NV12M", TAG, cap->width, cap->height);

    v4l2_capture_dump_format(cap);

    /* 申请 buffer */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = V4L2_MAX_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0) {
        LOGE("[%s] REQBUFS failed: %s", TAG, strerror(errno));
        goto fail;
    }
    if (req.count < 2) {
        LOGE("[%s] not enough buffers", TAG);
        goto fail;
    }

    cap->buf_count = req.count;
    if (cap->buf_count > V4L2_MAX_BUFS)
        cap->buf_count = V4L2_MAX_BUFS;   // 保护一下

    /* mmap 每个 buffer 的两个 plane：Y / UV */
    for (unsigned int i = 0; i < cap->buf_count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane  planes[VIDEO_MAX_PLANES];

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        buf.length = 2;
        buf.m.planes = planes;

        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOGE("[%s] QUERYBUF[%u] failed: %s", TAG, i, strerror(errno));
            goto fail;
        }

        for (unsigned int p = 0; p < buf.length && p < V4L2_MAX_PLANES && p < V4L2_MAX_PLANES; p++) {
            size_t len = planes[p].length;
            void *addr = mmap(NULL, len,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,
                              cap->fd,
                              planes[p].m.mem_offset);
            if (addr == MAP_FAILED) {
                LOGE("[%s] mmap[%u][%u] failed: %s", TAG, i, p, strerror(errno));
                goto fail;
            }

            cap->bufs[i].planes[p]  = addr;
            cap->bufs[i].lengths[p] = len;
        }

        /* buffer 入队 */
        if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
            LOGE("[%s] QBUF[%u] failed: %s", TAG, i, strerror(errno));
            goto fail;
        }
    }

    LOGI("[%s] %u buffers prepared", TAG, cap->buf_count);
    return 0;

fail:
    v4l2_capture_close(cap);
    return -1;
}

int v4l2_capture_start(V4L2Capture *cap)
{
    if (!cap || cap->fd < 0) return -1;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) < 0) {
        LOGE("[%s] STREAMON failed: %s", TAG, strerror(errno));
        return -1;
    }

    LOGI("[%s] STREAMON", TAG);
    return 0;
}

/* 从 NV12M 两个 plane 合成一帧连续 NV12，返回给上层 */
int v4l2_capture_dqbuf(V4L2Capture *cap, int *index,
                       void **data, size_t *length)
{
    if (!cap || cap->fd < 0 || !index || !data || !length)
        return -1;

    struct v4l2_buffer buf;
    struct v4l2_plane  planes[VIDEO_MAX_PLANES];

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 2;
    buf.m.planes = planes;

    int r = xioctl(cap->fd, VIDIOC_DQBUF, &buf);
    if (r < 0) {
        if (errno == EAGAIN) return 1;   // 暂时没数据
        LOGE("[%s] DQBUF failed: %s", TAG, strerror(errno));
        return -1;
    }

    int idx = buf.index;
    *index  = idx;
    cap->last_index = idx;
    cap->last_sequence = buf.sequence;

    // NV12M: plane0 = Y, plane1 = UV
    size_t y_size  = cap->width * cap->height;
    size_t uv_size = cap->width * cap->height / 2;

    // 防止 bytesused 比理论值小
    if (planes[0].bytesused && planes[0].bytesused < y_size)
        y_size = planes[0].bytesused;
    if (planes[1].bytesused && planes[1].bytesused < uv_size)
        uv_size = planes[1].bytesused;

    uint8_t *dst = cap->nv12_frame;

    memcpy(dst,
           cap->bufs[idx].planes[0],
           y_size);

    memcpy(dst + cap->width * cap->height,
           cap->bufs[idx].planes[1],
           uv_size);

    *data   = cap->nv12_frame;
    *length = cap->frame_size;

    return 0;
}

int v4l2_capture_qbuf(V4L2Capture *cap, int index)
{
    if (!cap || cap->fd < 0) return -1;

    struct v4l2_buffer buf;
    struct v4l2_plane  planes[VIDEO_MAX_PLANES];

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = index;
    buf.length = 2;
    buf.m.planes = planes;

    if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
        LOGE("[%s] QBUF[%d] failed: %s", TAG, index, strerror(errno));
        return -1;
    }

    return 0;
}

void v4l2_capture_close(V4L2Capture *cap)
{
    if (!cap) return;

    if (cap->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned int i = 0; i < cap->buf_count; i++) {
        for (int p = 0; p < V4L2_MAX_PLANES; p++) {
            if (cap->bufs[i].planes[p] && cap->bufs[i].lengths[p]) {
                munmap(cap->bufs[i].planes[p],
                       cap->bufs[i].lengths[p]);
                cap->bufs[i].planes[p]  = NULL;
                cap->bufs[i].lengths[p] = 0;
            }
        }
    }

    if (cap->fd >= 0) {
        close(cap->fd);
        cap->fd = -1;
    }

    if (cap->nv12_frame) {
        free(cap->nv12_frame);
        cap->nv12_frame = NULL;
    }

    cap->buf_count = 0;
    cap->last_index = -1;

    LOGI("[%s] capture closed", TAG);
}
