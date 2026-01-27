// encoder_mpp.h
#pragma once

#include <stdint.h>
#include <stddef.h>

#if defined(__has_include)
#  if __has_include("rk_mpi.h")
#    include "rk_mpi.h"
#    define RK_MPP_AVAILABLE 1
#  elif __has_include(<rk_mpi.h>)
#    include <rk_mpi.h>
#    define RK_MPP_AVAILABLE 1
#  else
#    define RK_MPP_AVAILABLE 0
#  endif
#else
#  include "rk_mpi.h"
#  define RK_MPP_AVAILABLE 1
#endif

#if !RK_MPP_AVAILABLE
typedef void *MppCtx;
typedef struct MppApi MppApi;
typedef void *MppBufferGroup;
typedef void *MppBuffer;
typedef int MppCodingType;
enum { MPP_VIDEO_CodingAVC = 7 };
#endif

#include "sink.h"

typedef struct {
    MppCtx         ctx;
    MppApi        *mpi;

    MppBufferGroup buf_grp;
    MppBuffer      frm_buf;

    int            width;
    int            height;
    int            hor_stride;
    int            ver_stride;
    size_t         frame_size;

    MppCodingType  type;
} EncoderMPP;

int encoder_mpp_init(EncoderMPP *enc,
                     int width, int height,
                     int fps,
                     int bitrate_bps,
                     MppCodingType type);

int encoder_mpp_encode(EncoderMPP *enc,
                       const uint8_t *frame_data,
                       size_t frame_size,
                       EncSink *sink,
                       size_t *out_bytes);

void encoder_mpp_deinit(EncoderMPP *enc);
