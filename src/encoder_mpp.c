// src/encoder_mpp.c
#include "encoder_mpp.h"
#include "log.h"

#include <string.h>

#define TAG "mpp_enc"

#if !RK_MPP_AVAILABLE

int encoder_mpp_init(EncoderMPP *enc,
                     int width, int height,
                     int fps,
                     int bitrate_bps,
                     MppCodingType type)
{
    (void)enc;
    (void)width;
    (void)height;
    (void)fps;
    (void)bitrate_bps;
    (void)type;
    LOGE("[%s] MPP headers not found. Please install MPP dev package.", TAG);
    return -1;
}

int encoder_mpp_encode(EncoderMPP *enc,
                       const uint8_t *frame_data,
                       size_t frame_size,
                       EncSink *sink,
                       size_t *out_bytes)
{
    (void)enc;
    (void)frame_data;
    (void)frame_size;
    (void)sink;
    if (out_bytes) *out_bytes = 0;
    LOGE("[%s] MPP not available.", TAG);
    return -1;
}

void encoder_mpp_deinit(EncoderMPP *enc)
{
    (void)enc;
}

#else  // RK_MPP_AVAILABLE

// 输入格式假定 NV12 (YUV420SP)
#define ENC_INPUT_FMT MPP_FMT_YUV420SP

int encoder_mpp_init(EncoderMPP *enc,
                     int width, int height,
                     int fps,
                     int bitrate_bps,
                     MppCodingType type)
{
    if (!enc) return -1;
    memset(enc, 0, sizeof(*enc));

    enc->width  = width;
    enc->height = height;
    enc->type   = type;

    enc->hor_stride = (width  + 15) & (~15);
    enc->ver_stride = (height + 15) & (~15);
    enc->frame_size = (size_t)enc->hor_stride * (size_t)enc->ver_stride * 3 / 2;

    MPP_RET ret;

    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret) {
        LOGE("[%s] mpp_create failed: %d", TAG, ret);
        return -1;
    }

    ret = mpp_init(enc->ctx, MPP_CTX_ENC, type);
    if (ret) {
        LOGE("[%s] mpp_init failed: %d", TAG, ret);
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        enc->mpi = NULL;
        return -1;
    }

    // buffer group
    ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_ION);
    if (ret) {
        LOGE("[%s] mpp_buffer_group_get_internal failed: %d", TAG, ret);
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        enc->mpi = NULL;
        return -1;
    }

    ret = mpp_buffer_get(enc->buf_grp, &enc->frm_buf, enc->frame_size);
    if (ret) {
        LOGE("[%s] mpp_buffer_get failed: %d", TAG, ret);
        mpp_buffer_group_put(enc->buf_grp);
        enc->buf_grp = NULL;
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        enc->mpi = NULL;
        return -1;
    }

    // config
    MppEncCfg cfg = NULL;
    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_CFG, &cfg);
    if (ret || !cfg) {
        LOGE("[%s] MPP_ENC_GET_CFG failed: %d", TAG, ret);
        return -1;
    }

    // prep
    mpp_enc_cfg_set_s32(cfg, "prep:width",       enc->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height",      enc->height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride",  enc->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride",  enc->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format",      ENC_INPUT_FMT);

    // rc (CBR)
    RK_S32 bps = (bitrate_bps > 0) ? bitrate_bps : (enc->width * enc->height * 5);
    mpp_enc_cfg_set_s32(cfg, "rc:mode",          MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target",    bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max",       bps * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min",       bps * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",    fps > 0 ? fps : 30);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",   fps > 0 ? fps : 30);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm",1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop",           (fps > 0 ? fps : 30) * 2);

    // apply
    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, cfg);
    if (ret) {
        LOGE("[%s] MPP_ENC_SET_CFG failed: %d", TAG, ret);
        return -1;
    }

    LOGI("[%s] init ok %dx%d fps=%d bitrate=%d", TAG, enc->width, enc->height, fps, bps);
    return 0;
}

int encoder_mpp_encode(EncoderMPP *enc,
                       const uint8_t *frame_data,
                       size_t frame_size,
                       EncSink *sink,
                       size_t *out_bytes)
{
    if (out_bytes) *out_bytes = 0;

    if (!enc || !enc->ctx || !enc->mpi || !enc->frm_buf) {
        LOGE("[%s] encoder_mpp_encode: invalid encoder", TAG);
        return -1;
    }
    if (!frame_data || frame_size == 0) {
        LOGE("[%s] encoder_mpp_encode: no input data", TAG);
        return -1;
    }

    // copy frame into MPP buffer
    void  *dst = mpp_buffer_get_ptr(enc->frm_buf);
    size_t copy_size = frame_size > enc->frame_size ? enc->frame_size : frame_size;
    memcpy(dst, frame_data, copy_size);
    if (copy_size < enc->frame_size) {
        memset((uint8_t *)dst + copy_size, 0, enc->frame_size - copy_size);
    }

    // build frame
    MppFrame frame = NULL;
    MPP_RET ret = mpp_frame_init(&frame);
    if (ret) {
        LOGE("[%s] mpp_frame_init failed: %d", TAG, ret);
        return -1;
    }

    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, ENC_INPUT_FMT);
    mpp_frame_set_buffer(frame, enc->frm_buf);
    mpp_frame_set_eos(frame, 0);

    // put frame
    ret = enc->mpi->encode_put_frame(enc->ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret) {
        LOGE("[%s] encode_put_frame failed: %d", TAG, ret);
        return -1;
    }

    // get packet
    MppPacket pkt = NULL;
    ret = enc->mpi->encode_get_packet(enc->ctx, &pkt);
    if (ret) {
        // no packet ready is OK, but usually should not happen for realtime
        return 0;
    }

    if (pkt) {
        void  *ptr = mpp_packet_get_pos(pkt);
        size_t len = mpp_packet_get_length(pkt);

        int sink_ret = 0;
        if (ptr && len > 0 && sink) {
            sink_ret = enc_sink_write(sink, (const uint8_t *)ptr, len);
            if (sink_ret == 0 && out_bytes) *out_bytes = len;
        }

        mpp_packet_deinit(&pkt);

        if (sink_ret != 0) {
            return -1;
        }
    }

    return 0;
}

void encoder_mpp_deinit(EncoderMPP *enc)
{
    if (!enc) return;

    LOGI("[%s] encoder_mpp_deinit", TAG);

    if (enc->frm_buf) {
        mpp_buffer_put(enc->frm_buf);
        enc->frm_buf = NULL;
    }
    if (enc->buf_grp) {
        mpp_buffer_group_put(enc->buf_grp);
        enc->buf_grp = NULL;
    }
    if (enc->ctx) {
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        enc->mpi = NULL;
    }

    memset(enc, 0, sizeof(*enc));
}

#endif  // RK_MPP_AVAILABLE
