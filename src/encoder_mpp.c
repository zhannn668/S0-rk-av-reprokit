// src/encoder_mpp.c
#include "encoder_mpp.h"
#include "log.h"

#include <string.h>

#define TAG "mpp_enc"

#if !RK_MPP_AVAILABLE

/*
 * 初始化 MPP 编码器（当 RK_MPP 不可用时的占位实现）。
 *
 * 该分支用于：编译环境缺少 Rockchip MPP 头文件/库时，给出明确错误提示，
 * 让上层在运行时快速定位依赖缺失。
 *
 * @param enc          编码器实例（未使用）
 * @param width        输入宽度（未使用）
 * @param height       输入高度（未使用）
 * @param fps          帧率（未使用）
 * @param bitrate_bps  码率（未使用）
 * @param type         编码类型（未使用）
 * @return             -1 表示不可用
 */
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

/*
 * 编码一帧（当 RK_MPP 不可用时的占位实现）。
 *
 * @param enc         编码器实例（未使用）
 * @param frame_data  输入帧数据（未使用）
 * @param frame_size  输入帧大小（未使用）
 * @param sink        输出 sink（未使用）
 * @param out_bytes   输出：写出的字节数（该分支恒为 0）
 * @return            -1 表示不可用
 */
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

/*
 * 释放编码器资源（当 RK_MPP 不可用时为 no-op）。
 */
void encoder_mpp_deinit(EncoderMPP *enc)
{
    (void)enc;
}

#else  // RK_MPP_AVAILABLE

// 输入格式假定 NV12 (YUV420SP)
#define ENC_INPUT_FMT MPP_FMT_YUV420SP

/*
 * 初始化 MPP 硬编码器。
 *
 * 当前实现：
 * - 输入格式假定为 NV12（MPP_FMT_YUV420SP）
 * - rate control 使用 CBR
 * - 申请 ION buffer group，并分配一块帧缓冲（frm_buf）用于输入帧拷贝
 *
 * @param enc          输出：编码器实例
 * @param width        输入宽度
 * @param height       输入高度
 * @param fps          目标帧率（<=0 则按 30 兜底用于 RC 配置）
 * @param bitrate_bps  目标码率（<=0 则按分辨率估算）
 * @param type         MPP 编码类型（例如 MPP_VIDEO_CodingAVC）
 * @return             0 成功；-1 失败
 */
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

    /* MPP 通常要求 stride 16 对齐（便于硬件处理）。 */
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

    /* buffer group：使用 ION 分配可供硬件访问的缓冲。 */
    ret = mpp_buffer_group_get_internal(&enc->buf_grp, MPP_BUFFER_TYPE_ION);
    if (ret) {
        LOGE("[%s] mpp_buffer_group_get_internal failed: %d", TAG, ret);
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        enc->mpi = NULL;
        return -1;
    }

    /* 为输入帧申请一块连续缓冲，后续每帧把 NV12 数据 memcpy 进来。 */
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

    /* 获取编码器配置句柄。 */
    MppEncCfg cfg = NULL;

    ret = mpp_enc_cfg_init(&cfg);
    if (ret || !cfg) {
        LOGE("[%s] mpp_enc_cfg_init failed: %d", TAG, ret);
        return -1;
    }

    ret = enc->mpi->control(enc->ctx, MPP_ENC_GET_CFG, cfg);
    if (ret) {
        LOGE("[%s] MPP_ENC_GET_CFG failed: %d", TAG, ret);
        mpp_enc_cfg_deinit(cfg);
        return -1;
    }


    /* prep：输入图像参数与格式。 */
    mpp_enc_cfg_set_s32(cfg, "prep:width",       enc->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height",      enc->height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride",  enc->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride",  enc->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format",      ENC_INPUT_FMT);

    /* rc：码率控制（CBR），并设置 fps/gop 等关键参数。 */
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

    /* 应用配置到编码器。 */
    ret = enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, cfg);
    if (ret) {
        LOGE("[%s] MPP_ENC_SET_CFG failed: %d", TAG, ret);
        encoder_mpp_deinit(enc);
        return -1;
    }

    LOGI("[%s] init ok %dx%d fps=%d bitrate=%d", TAG, enc->width, enc->height, fps, bps);
    return 0;
}

/*
 * 编码一帧 NV12 数据：
 * 1) 将输入 frame_data 复制到 MPP buffer（不足则补 0）
 * 2) 构造 MppFrame 并 encode_put_frame
 * 3) encode_get_packet 获取编码输出（可能暂时拿不到 packet）
 * 4) 若获取到 packet 且提供了 sink，则写入 sink
 *
 * 返回值约定：
 * - 0：成功（含“暂时无 packet”这种情况）
 * - -1：失败
 */
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

    /* 将一帧输入数据拷贝到 MPP 的输入缓冲。 */
    void  *dst = mpp_buffer_get_ptr(enc->frm_buf);
    size_t copy_size = frame_size > enc->frame_size ? enc->frame_size : frame_size;
    memcpy(dst, frame_data, copy_size);
    if (copy_size < enc->frame_size) {
        /* 输入不足时补 0，避免读到未初始化内容。 */
        memset((uint8_t *)dst + copy_size, 0, enc->frame_size - copy_size);
    }

    /* 构造 MppFrame 元数据，并绑定输入 buffer。 */
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

    /* 投递一帧到编码器。 */
    ret = enc->mpi->encode_put_frame(enc->ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret) {
        LOGE("[%s] encode_put_frame failed: %d", TAG, ret);
        return -1;
    }

    /* 拉取编码输出 packet。 */
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
            /* 将编码后的字节流写入下游（例如文件）。 */
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

/*
 * 释放编码器资源：buffer、buffer group、MPP ctx，并将 enc 清零。
 */
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
