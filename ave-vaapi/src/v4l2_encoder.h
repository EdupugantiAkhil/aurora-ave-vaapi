// SPDX-License-Identifier: MIT
#ifndef VENUS_V4L2_ENCODER_H
#define VENUS_V4L2_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "v4l2_common.h"

struct venus_v4l2_encoder;

/* Called when an OUTPUT (raw) buffer is released by the encoder. */
typedef int (*venus_v4l2_output_done_callback)(uint64_t tag,
                                                void *opaque);

/*
 * Everything the apple-ave stateful encoder can be told.  Each field maps
 * to one V4L2 control/format the kernel driver implements (ave-v4l2.c);
 * there are deliberately no fields for controls apple-ave lacks (frame QP,
 * entropy mode, 8x8 transform, AU delimiter, tier, frame RC enable).
 */
struct venus_v4l2_encoder_config {
    const char *device;
    uint32_t coded_format;      /* V4L2_PIX_FMT_H264 / _HEVC */
    uint32_t raw_format;        /* V4L2_PIX_FMT_NV12 / _P010 */
    uint32_t width;             /* visible size, even */
    uint32_t height;
    uint32_t frames_per_second; /* 1..240 */
    uint32_t bitrate_mode;      /* V4L2_MPEG_VIDEO_BITRATE_MODE_CBR / _VBR */
    uint32_t bitrate;           /* average (VBR) or constant (CBR) bit/s */
    uint32_t bitrate_peak;      /* VBR peak bit/s, 0 = driver default (2x) */
    bool qp_range;              /* apply min_qp/max_qp */
    uint32_t min_qp;
    uint32_t max_qp;
    uint32_t gop_size;          /* IDR interval in frames, 1..65535 */
    uint32_t h264_profile;      /* V4L2_MPEG_VIDEO_H264_PROFILE_* */
    uint32_t h264_level;        /* minimum level; the driver may raise it */
    uint32_t hevc_profile;      /* V4L2_MPEG_VIDEO_HEVC_PROFILE_* */
    uint32_t hevc_level;        /* minimum level; the driver may raise it */
    size_t capture_buffer_size;
    unsigned int output_buffers;
    unsigned int capture_buffers;
    venus_v4l2_output_done_callback output_done;
    void *output_done_opaque;
};

struct ave_control {
    uint32_t id;
    int32_t value;
    const char *name;
};

#define AVE_MAX_CONTROLS 16

/*
 * Pure translation of a config to the ordered S_CTRL list (host-testable).
 * Returns 0 or a negative errno for values apple-ave cannot honour.
 */
int ave_encoder_controls(const struct venus_v4l2_encoder_config *config,
                         struct ave_control *controls, size_t max_controls,
                         size_t *count);

/*
 * Copy a visible 4:2:0 two-plane frame into AVE's OUTPUT layout: luma rows
 * at @destination_stride, chroma rows immediately after @height luma rows
 * (no padding between planes; any padding is at the end, ave-sess.c).
 */
int ave_pack_frame(uint8_t *destination, size_t destination_size,
                   uint32_t destination_stride,
                   const uint8_t *source_y, const uint8_t *source_uv,
                   uint32_t source_stride, uint32_t width, uint32_t height,
                   unsigned int bytes_per_sample, size_t *packed_size);

struct venus_v4l2_packet {
    const uint8_t *data;
    size_t size;
    /* Tags are V4L2 timestamps in microseconds and identify the input frame. */
    uint64_t tag;
    uint32_t flags;
};

typedef int (*venus_v4l2_packet_callback)(
    const struct venus_v4l2_packet *packet, void *opaque);

/* Open the stateful encoder and configure its raw and coded queues. */
int venus_v4l2_encoder_open(
    const struct venus_v4l2_encoder_config *config,
    struct venus_v4l2_encoder **encoder,
    struct venus_v4l2_error *error);
/*
 * Queue one frame.  @data points at the luma plane; chroma is at
 * @uv_offset bytes from it; both planes use @stride.
 */
int venus_v4l2_encoder_submit(
    struct venus_v4l2_encoder *encoder,
    const uint8_t *data, size_t size,
    uint32_t stride, size_t uv_offset,
    uint64_t tag, venus_v4l2_packet_callback callback,
    void *opaque);
int venus_v4l2_encoder_compressed_size(
    uint32_t width, uint32_t height, size_t *size);
/* Wait until the encoder holds no queued raw frame (keyframe sequencing). */
int venus_v4l2_encoder_wait_output_idle(
    struct venus_v4l2_encoder *encoder, int timeout_ms,
    venus_v4l2_packet_callback callback, void *opaque);
int venus_v4l2_encoder_force_keyframe(
    struct venus_v4l2_encoder *encoder);
/* Bit rate changes take effect at the next frame sent to the firmware. */
int venus_v4l2_encoder_set_bitrate(
    struct venus_v4l2_encoder *encoder, uint32_t bitrate);
int venus_v4l2_encoder_pump(struct venus_v4l2_encoder *encoder,
                            int timeout_ms,
                            venus_v4l2_packet_callback callback,
                            void *opaque, bool *end_of_stream);
int venus_v4l2_encoder_stop(struct venus_v4l2_encoder *encoder);
void venus_v4l2_encoder_close(struct venus_v4l2_encoder *encoder);

const char *venus_v4l2_encoder_last_operation(
    const struct venus_v4l2_encoder *encoder);
unsigned int venus_v4l2_encoder_output_count(
    const struct venus_v4l2_encoder *encoder);
unsigned int venus_v4l2_encoder_capture_count(
    const struct venus_v4l2_encoder *encoder);
uint32_t venus_v4l2_encoder_output_size(
    const struct venus_v4l2_encoder *encoder);
uint32_t venus_v4l2_encoder_capture_size(
    const struct venus_v4l2_encoder *encoder);

#endif
