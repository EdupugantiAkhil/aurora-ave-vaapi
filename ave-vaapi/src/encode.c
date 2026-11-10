// SPDX-License-Identifier: MIT
#include "backend_internal.h"
#include "encode_params.h"

#include <errno.h>
#include <limits.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#define VENUS_ENCODE_OUTPUT_BUFFERS 4u
#define VENUS_ENCODE_CAPTURE_BUFFERS 8u
#define VENUS_ENCODE_DEFAULT_FPS 30u
#define AVE_KEYFRAME_IDLE_TIMEOUT_MS 5000

/*
 * VA encoding is accumulated between BeginPicture and EndPicture.  This layer
 * translates the collected VA parameter buffers into apple-ave V4L2 controls
 * (see encode_params.c for the per-field mapping), copies the input surface
 * into the encoder's OUTPUT layout and associates returned coded packets
 * with their VA coded buffers.
 */
struct venus_h264_encode_parameters {
    const VAEncSequenceParameterBufferH264 *sequence;
    const VAEncPictureParameterBufferH264 *picture;
    struct ave_rate_request rate;
    unsigned int slices;
    uint32_t slice_macroblocks;
    bool intra;
};

struct venus_hevc_encode_parameters {
    const VAEncSequenceParameterBufferHEVC *sequence;
    const VAEncPictureParameterBufferHEVC *picture;
    struct ave_rate_request rate;
    uint32_t num_ctu_in_slice;
    unsigned int slices;
    bool intra;
};

static int64_t monotonic_milliseconds(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return -1;
    return (int64_t)timestamp.tv_sec * 1000 +
           timestamp.tv_nsec / 1000000;
}

static size_t buffer_bytes(const struct venus_buffer *buffer)
{
    if (!buffer || buffer->element_size >
                       SIZE_MAX / buffer->num_elements)
        return 0;
    return buffer->element_size * buffer->num_elements;
}

static int parse_misc_parameter(const struct venus_buffer *buffer,
                                struct ave_rate_request *rate)
{
    const VAEncMiscParameterBuffer *header;
    size_t size = buffer_bytes(buffer);

    if (size < sizeof(*header))
        return -EINVAL;
    header = (const VAEncMiscParameterBuffer *)buffer->data;
    return ave_parse_misc(header->type, header->data,
                          size - sizeof(*header), rate);
}

static int collect_parameters(
    struct venus_backend *backend, struct venus_context *context,
    struct venus_h264_encode_parameters *parameters)
{
    size_t index;

    memset(parameters, 0, sizeof(*parameters));

    for (index = 0; index < context->pending_count; index++) {
        struct venus_buffer *buffer = venus_backend_find_buffer(
            backend, context->pending[index]);
        size_t element;
        int status;

        if (!buffer || buffer_bytes(buffer) == 0)
            return -EINVAL;

        switch (buffer->type) {
        case VAEncSequenceParameterBufferType:
            if (parameters->sequence ||
                buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncSequenceParameterBufferH264))
                return -EINVAL;
            parameters->sequence =
                (const VAEncSequenceParameterBufferH264 *)buffer->data;
            break;
        case VAEncPictureParameterBufferType:
            if (parameters->picture ||
                buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncPictureParameterBufferH264))
                return -EINVAL;
            parameters->picture =
                (const VAEncPictureParameterBufferH264 *)buffer->data;
            break;
        case VAEncSliceParameterBufferType:
            if (buffer->element_size <
                sizeof(VAEncSliceParameterBufferH264))
                return -EINVAL;
            for (element = 0; element < buffer->num_elements; element++) {
                const VAEncSliceParameterBufferH264 *slice =
                    (const VAEncSliceParameterBufferH264 *)
                        (buffer->data + element * buffer->element_size);
                unsigned int slice_type = slice->slice_type % 5u;

                /* AVE runs B_FRAMES=0; SP/SI do not exist in AVE output. */
                if (slice_type != 0 && slice_type != 2)
                    return -ENOTSUP;
                if (slice->num_macroblocks == 0 ||
                    slice->macroblock_address != 0)
                    return -ENOTSUP;
                parameters->intra = slice_type == 2;
                parameters->slice_macroblocks = slice->num_macroblocks;
                parameters->slices++;
            }
            break;
        case VAEncPackedHeaderParameterBufferType:
        case VAEncPackedHeaderDataBufferType:
            /*
             * Packed headers are not advertised: AVE writes its own
             * SPS/PPS/VPS and slice headers and cannot take client NALs.
             */
            return -ENOTSUP;
        case VAEncMiscParameterBufferType:
            status = parse_misc_parameter(buffer, &parameters->rate);
            if (status < 0)
                return status;
            break;
        default:
            return -ENOTSUP;
        }
    }

    /* VAConfigAttribEncMaxSlices is 1: one slice covering the picture. */
    if (!parameters->picture || parameters->slices != 1)
        return parameters->slices > 1 ? -ENOTSUP : -EINVAL;
    return 0;
}

static int collect_hevc_parameters(
    struct venus_backend *backend, struct venus_context *context,
    struct venus_hevc_encode_parameters *parameters)
{
    size_t index;

    memset(parameters, 0, sizeof(*parameters));
    for (index = 0; index < context->pending_count; index++) {
        struct venus_buffer *buffer = venus_backend_find_buffer(
            backend, context->pending[index]);
        int status;

        if (!buffer || buffer_bytes(buffer) == 0)
            return -EINVAL;
        switch (buffer->type) {
        case VAEncSequenceParameterBufferType:
            if (parameters->sequence || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncSequenceParameterBufferHEVC))
                return -EINVAL;
            parameters->sequence =
                (const VAEncSequenceParameterBufferHEVC *)buffer->data;
            break;
        case VAEncPictureParameterBufferType:
            if (parameters->picture || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncPictureParameterBufferHEVC))
                return -EINVAL;
            parameters->picture =
                (const VAEncPictureParameterBufferHEVC *)buffer->data;
            break;
        case VAEncSliceParameterBufferType: {
            const VAEncSliceParameterBufferHEVC *slice;

            if (parameters->slices || buffer->num_elements != 1 ||
                buffer->element_size <
                    sizeof(VAEncSliceParameterBufferHEVC))
                return parameters->slices ? -ENOTSUP : -EINVAL;
            slice = (const VAEncSliceParameterBufferHEVC *)buffer->data;
            /* HEVC slice_type: 0 B, 1 P, 2 I. */
            if (slice->slice_segment_address != 0 ||
                slice->num_ctu_in_slice == 0 ||
                (slice->slice_type != 1 && slice->slice_type != 2))
                return -ENOTSUP;
            parameters->num_ctu_in_slice = slice->num_ctu_in_slice;
            parameters->intra = slice->slice_type == 2;
            parameters->slices = 1;
            break;
        }
        case VAEncPackedHeaderParameterBufferType:
        case VAEncPackedHeaderDataBufferType:
            /*
             * Packed headers are not advertised: AVE writes its own
             * SPS/PPS/VPS and slice headers and cannot take client NALs.
             */
            return -ENOTSUP;
        case VAEncMiscParameterBufferType:
            status = parse_misc_parameter(buffer, &parameters->rate);
            if (status < 0)
                return status;
            break;
        default:
            return -ENOTSUP;
        }
    }

    if (!parameters->picture || parameters->slices != 1)
        return -EINVAL;
    if (parameters->picture->pic_fields.bits.idr_pic_flag &&
        !parameters->intra)
        return -ENOTSUP;
    return 0;
}

int venus_encode_h264_dimensions(
    const VAEncSequenceParameterBufferH264 *sequence,
    uint32_t *width, uint32_t *height)
{
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t crop_width = 0;
    uint32_t crop_height = 0;

    if (!sequence || !width || !height ||
        sequence->picture_width_in_mbs == 0 ||
        sequence->picture_height_in_mbs == 0)
        return -EINVAL;
    if (sequence->seq_fields.bits.chroma_format_idc != 1 ||
        !sequence->seq_fields.bits.frame_mbs_only_flag ||
        sequence->bit_depth_luma_minus8 != 0 ||
        sequence->bit_depth_chroma_minus8 != 0)
        return -ENOTSUP;

    coded_width =
        (uint32_t)sequence->picture_width_in_mbs * 16u;
    coded_height =
        (uint32_t)sequence->picture_height_in_mbs * 16u;
    if ((uint32_t)sequence->picture_width_in_mbs *
            sequence->picture_height_in_mbs >
        VENUS_H264_MAX_MACROBLOCKS)
        return -EINVAL;

    if (sequence->frame_cropping_flag) {
        uint64_t horizontal = sequence->frame_crop_right_offset;
        uint64_t vertical = sequence->frame_crop_bottom_offset;

        /* AVE encodes from the top-left corner; only right/bottom crop. */
        if (sequence->frame_crop_left_offset ||
            sequence->frame_crop_top_offset)
            return -ENOTSUP;
        horizontal *= 2u;
        vertical *= 2u;
        if (horizontal >= 16u || vertical >= 16u)
            return -ENOTSUP;
        crop_width = (uint32_t)horizontal;
        crop_height = (uint32_t)vertical;
    }

    *width = coded_width - crop_width;
    *height = coded_height - crop_height;
    if (*width < VENUS_MIN_WIDTH ||
        *height < VENUS_MIN_HEIGHT ||
        *width > VENUS_MAX_WIDTH ||
        *height > VENUS_MAX_HEIGHT ||
        (*width & 1u) || (*height & 1u))
        return -ERANGE;
    return 0;
}

static uint32_t profile_to_v4l2(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
    case VAProfileH264Main:
        return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    case VAProfileH264High:
        return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
    default:
        return UINT32_MAX;
    }
}

static uint32_t level_to_v4l2(uint8_t level_idc)
{
    switch (level_idc) {
    case 9:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1B;
    case 10:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
    case 11:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
    case 12:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
    case 13:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
    case 20:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
    case 21:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
    case 22:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
    case 30:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
    case 31:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
    case 32:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
    case 40:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
    case 41:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
    case 42:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
    case 50:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
    case 51:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
    case 52:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_2;
    default:
        return UINT32_MAX;
    }
}

int venus_encode_queue_coded_buffer_locked(
    struct venus_context *context, VABufferID buffer_id,
    uint64_t tag)
{
    struct venus_buffer *buffer;
    size_t offset;
    size_t tail;

    if (!context || !context->backend || tag == 0)
        return -EINVAL;
    if (context->encode_queue_count >= VENUS_MAX_SURFACES)
        return -ENOSPC;

    buffer = venus_backend_find_buffer(
        context->backend, buffer_id);
    if (!buffer || buffer->type != VAEncCodedBufferType ||
        buffer->context_id != context->id)
        return -ENOENT;

    for (offset = 0; offset < context->encode_queue_count;
         offset++) {
        size_t index =
            (context->encode_queue_head + offset) %
            VENUS_MAX_SURFACES;

        if (context->encode_queue[index] == buffer_id)
            return -EALREADY;
    }

    tail = (context->encode_queue_head +
            context->encode_queue_count) %
           VENUS_MAX_SURFACES;
    context->encode_queue[tail] = buffer_id;
    context->encode_queue_count++;
    buffer->coded_sequence = context->encode_sequence;
    buffer->coded_tag = tag;
    return 0;
}

static void remove_encode_queue_entry(
    struct venus_context *context, size_t offset)
{
    size_t index;

    for (index = offset;
         index + 1 < context->encode_queue_count;
         index++) {
        size_t destination =
            (context->encode_queue_head + index) %
            VENUS_MAX_SURFACES;
        size_t source =
            (context->encode_queue_head + index + 1) %
            VENUS_MAX_SURFACES;

        context->encode_queue[destination] =
            context->encode_queue[source];
    }
    context->encode_queue_count--;
}

static void rollback_coded_buffer(struct venus_context *context,
                                  VABufferID buffer_id)
{
    struct venus_buffer *buffer;
    size_t tail;

    if (context->encode_queue_count == 0)
        return;

    tail = (context->encode_queue_head +
            context->encode_queue_count - 1) %
           VENUS_MAX_SURFACES;
    if (context->encode_queue[tail] == buffer_id) {
        context->encode_queue_count--;
        buffer = venus_backend_find_buffer(
            context->backend, buffer_id);
        if (buffer) {
            buffer->coded_sequence = 0;
            buffer->coded_tag = 0;
        }
    }
}

int venus_encode_store_packet_locked(
    struct venus_context *context,
    const struct venus_v4l2_packet *packet)
{
    struct venus_backend *backend;
    struct venus_buffer *buffer;
    struct venus_surface *surface;
    VABufferID buffer_id;
    size_t capacity;
    size_t offset;
    bool complete;

    if (!packet || !context || !context->backend ||
        packet->tag == 0 ||
        context->encode_queue_count == 0)
        return -EINVAL;

    backend = context->backend;
    buffer = NULL;
    buffer_id = VA_INVALID_ID;
    for (offset = 0; offset < context->encode_queue_count;
         offset++) {
        size_t index =
            (context->encode_queue_head + offset) %
            VENUS_MAX_SURFACES;
        struct venus_buffer *candidate;

        buffer_id = context->encode_queue[index];
        candidate = venus_backend_find_buffer(
            backend, buffer_id);
        if (candidate &&
            candidate->type == VAEncCodedBufferType &&
            candidate->coded_tag == packet->tag) {
            buffer = candidate;
            break;
        }
    }
    if (!buffer)
        return -ENOENT;

    if (buffer->element_size >
        SIZE_MAX / buffer->capacity_elements)
        return -EOVERFLOW;
    capacity =
        buffer->element_size * buffer->capacity_elements;
    if (packet->size > capacity - buffer->coded_size) {
        buffer->coded_segment.status |=
            VA_CODED_BUF_STATUS_FRAME_SIZE_OVERFLOW;
        return -ENOSPC;
    }

    memcpy(buffer->data + buffer->coded_size,
           packet->data, packet->size);
    buffer->coded_size += packet->size;
    buffer->coded_packets++;
    buffer->coded_segment.size = (uint32_t)buffer->coded_size;
    buffer->coded_segment.buf = buffer->data;
    buffer->coded_segment.bit_offset = 0;
    buffer->coded_segment.next = NULL;

    if (packet->flags & V4L2_BUF_FLAG_ERROR)
        buffer->coded_segment.status |=
            VA_CODED_BUF_STATUS_BAD_BITSTREAM;

    complete =
        (packet->flags &
         (V4L2_BUF_FLAG_KEYFRAME |
          V4L2_BUF_FLAG_PFRAME |
          V4L2_BUF_FLAG_BFRAME |
          V4L2_BUF_FLAG_ERROR)) != 0;
    if (!complete) {
        return 0;
    }
    /* A requested key frame must come back flagged as one. */
    if (buffer->keyframe_expected &&
        !(packet->flags & (V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_ERROR))) {
        if (context->keyframe_misses++ == 0)
            fprintf(stderr, "ave-vaapi: requested key frame was not "
                            "coded as a key frame\n");
    }

    buffer->coded_ready = true;
    remove_encode_queue_entry(context, offset);
    surface = venus_backend_find_surface(
        backend, buffer->source_surface_id);
    if (surface &&
        surface->coded_buffer_id == buffer->id &&
        buffer->input_done)
        surface->encode_pending = false;

    return 0;
}

static uint32_t hevc_level_to_v4l2(uint8_t level_idc)
{
    static const struct {
        uint8_t idc;
        uint32_t control;
    } levels[] = {
        { 30, V4L2_MPEG_VIDEO_HEVC_LEVEL_1 },
        { 60, V4L2_MPEG_VIDEO_HEVC_LEVEL_2 },
        { 63, V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1 },
        { 90, V4L2_MPEG_VIDEO_HEVC_LEVEL_3 },
        { 93, V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1 },
        { 120, V4L2_MPEG_VIDEO_HEVC_LEVEL_4 },
        { 123, V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1 },
        { 150, V4L2_MPEG_VIDEO_HEVC_LEVEL_5 },
        { 153, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1 },
        { 156, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2 },
        { 180, V4L2_MPEG_VIDEO_HEVC_LEVEL_6 },
        { 183, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1 },
        { 186, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2 },
    };
    size_t index;

    for (index = 0; index < sizeof(levels) / sizeof(levels[0]); index++) {
        if (levels[index].idc == level_idc)
            return levels[index].control;
    }
    return UINT32_MAX;
}

static int store_packet(const struct venus_v4l2_packet *packet,
                        void *opaque)
{
    return venus_encode_store_packet_locked(opaque, packet);
}

static int store_output_done(uint64_t tag, void *opaque)
{
    struct venus_context *context = opaque;
    struct venus_backend *backend = context->backend;
    unsigned int index;

    for (index = 0; index < VENUS_MAX_BUFFERS; index++) {
        struct venus_buffer *buffer = &backend->buffers[index];
        struct venus_surface *surface;

        if (!buffer->used ||
            buffer->type != VAEncCodedBufferType ||
            buffer->context_id != context->id ||
            buffer->coded_tag != tag)
            continue;

        buffer->input_done = true;
        surface = venus_backend_find_surface(
            backend, buffer->source_surface_id);
        if (surface &&
            surface->coded_buffer_id == buffer->id &&
            buffer->coded_ready)
            surface->encode_pending = false;
        return 0;
    }

    return -ENOENT;
}

static uint32_t frame_rate_or_default(const struct ave_rate_request *rate,
                                      uint32_t fallback)
{
    if (rate->frame_rate_seen)
        return rate->frames_per_second;
    if (fallback > 0 && fallback <= 240)
        return fallback;
    return VENUS_ENCODE_DEFAULT_FPS;
}

static void log_open_failure(const char *codec,
                             const struct venus_v4l2_error *error,
                             int status)
{
    fprintf(stderr, "ave-vaapi: %s encoder setup failed at %s: %s\n",
            codec, error->operation[0] ? error->operation : "parameters",
            strerror(-status));
}

static int open_encoder(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config,
    const struct venus_h264_encode_parameters *parameters)
{
    const VAEncSequenceParameterBufferH264 *sequence =
        parameters->sequence;
    struct venus_v4l2_encoder_config encoder_config;
    struct venus_v4l2_error error = { 0 };
    struct ave_rate_settings rate;
    uint32_t profile = profile_to_v4l2(config->profile);
    uint32_t level;
    uint32_t vui_fps = 0;
    uint32_t encode_width;
    uint32_t encode_height;
    size_t capture_size;
    int status;

    /* The first picture of a stream must carry the sequence parameters. */
    if (!sequence || profile == UINT32_MAX)
        return -EINVAL;
    if (sequence->ip_period > 1)
        return -ENOTSUP;
    /*
     * apple-ave treats H264_LEVEL as a minimum and raises it to fit the
     * size, rate and bit rate (ave_pick_level), up to 5.2.
     */
    level = level_to_v4l2(sequence->level_idc);
    if (level == UINT32_MAX)
        return -ENOTSUP;

    status = venus_encode_h264_dimensions(
        sequence, &encode_width, &encode_height);
    if (status < 0)
        return status;
    if ((encode_width + 15u) / 16u * 16u != context->width ||
        (encode_height + 15u) / 16u * 16u != context->height)
        return -EINVAL;
    if (parameters->slice_macroblocks !=
        (uint32_t)sequence->picture_width_in_mbs *
            sequence->picture_height_in_mbs)
        return -ENOTSUP;

    status = ave_rate_settings_from_va(config->rate_control,
                                       &parameters->rate,
                                       sequence->bits_per_second, &rate);
    if (status < 0)
        return status;
    if (sequence->vui_fields.bits.timing_info_present_flag &&
        sequence->num_units_in_tick > 0) {
        uint64_t derived = sequence->time_scale /
                           ((uint64_t)sequence->num_units_in_tick * 2u);

        if (derived > 0 && derived <= 240)
            vui_fps = (uint32_t)derived;
    }

    status = venus_v4l2_encoder_compressed_size(
        encode_width, encode_height, &capture_size);
    if (status < 0)
        return status;

    encoder_config = (struct venus_v4l2_encoder_config) {
        .device = backend->capabilities.encoder_path,
        .coded_format = V4L2_PIX_FMT_H264,
        .raw_format = V4L2_PIX_FMT_NV12,
        .width = encode_width,
        .height = encode_height,
        .frames_per_second =
            frame_rate_or_default(&parameters->rate, vui_fps),
        .bitrate_mode = rate.v4l2_mode,
        .bitrate = rate.bitrate,
        .bitrate_peak = rate.peak,
        .qp_range = rate.qp_range,
        .min_qp = rate.min_qp,
        .max_qp = rate.max_qp,
        .gop_size = ave_gop_from_va(sequence->intra_idr_period,
                                    sequence->intra_period),
        .h264_profile = profile,
        .h264_level = level,
        .capture_buffer_size = capture_size,
        .output_buffers = VENUS_ENCODE_OUTPUT_BUFFERS,
        .capture_buffers = VENUS_ENCODE_CAPTURE_BUFFERS,
        .output_done = store_output_done,
        .output_done_opaque = context,
    };

    status = venus_v4l2_encoder_open(
        &encoder_config, &context->encoder, &error);
    if (status < 0) {
        log_open_failure("H.264", &error, status);
        return status;
    }
    context->encode_width = encode_width;
    context->encode_height = encode_height;
    context->encode_frames_per_second = encoder_config.frames_per_second;
    context->encode_bitrate = rate.bitrate;
    return 0;
}

static int open_hevc_encoder(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config,
    const struct venus_hevc_encode_parameters *parameters,
    const struct venus_surface *input)
{
    const VAEncSequenceParameterBufferHEVC *sequence =
        parameters->sequence;
    const VAEncPictureParameterBufferHEVC *picture =
        parameters->picture;
    bool main10 = config->profile == VAProfileHEVCMain10;
    struct venus_v4l2_encoder_config encoder_config;
    struct venus_v4l2_error error = { 0 };
    struct ave_rate_settings rate;
    uint32_t level;
    uint32_t vui_fps = 0;
    uint32_t min_cb;
    uint32_t ctu;
    uint32_t ctu_count;
    uint32_t encode_width;
    uint32_t encode_height;
    size_t capture_size;
    int status;

    if (!sequence || !picture || !parameters->intra ||
        !picture->pic_fields.bits.idr_pic_flag ||
        sequence->general_profile_idc != (main10 ? 2u : 1u) ||
        sequence->seq_fields.bits.chroma_format_idc != 1 ||
        sequence->seq_fields.bits.bit_depth_luma_minus8 !=
            (main10 ? 2u : 0u) ||
        sequence->seq_fields.bits.bit_depth_chroma_minus8 !=
            (main10 ? 2u : 0u) ||
        sequence->seq_fields.bits.separate_colour_plane_flag ||
        sequence->seq_fields.bits.pcm_enabled_flag ||
        sequence->ip_period > 1 ||
        /* apple-ave has no tier control: Main tier only. */
        sequence->general_tier_flag ||
        picture->num_tile_columns_minus1 ||
        picture->num_tile_rows_minus1 ||
        picture->pic_fields.bits.tiles_enabled_flag)
        return -ENOTSUP;
    level = hevc_level_to_v4l2(sequence->general_level_idc);
    /* The apple-ave HEVC_LEVEL menu stops at 5.1 (a minimum level). */
    if (level == UINT32_MAX || level > V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1)
        return -ENOTSUP;
    if (sequence->log2_min_luma_coding_block_size_minus3 > 3 ||
        sequence->log2_diff_max_min_luma_coding_block_size > 3 ||
        sequence->log2_min_luma_coding_block_size_minus3 +
            sequence->log2_diff_max_min_luma_coding_block_size > 3)
        return -EINVAL;
    min_cb = 1u <<
        (sequence->log2_min_luma_coding_block_size_minus3 + 3);
    ctu = min_cb <<
        sequence->log2_diff_max_min_luma_coding_block_size;
    if (sequence->pic_width_in_luma_samples != context->width ||
        sequence->pic_height_in_luma_samples != context->height)
        return -EINVAL;
    /*
     * VA HEVC has no conformance window outside packed headers.  Clients
     * such as FFmpeg pad the coded size to a coding-tree-block boundary and
     * upload the visible picture into a surface of the visible size.  FFmpeg
     * can advertise an 8-pixel minimum CB in the sequence while using its
     * 16-pixel fallback surface alignment, so validate the padding against
     * the CTU rather than the advertised minimum CB.  Encode the input size
     * and let apple-ave write the conformance window (ave_build_process crop
     * flag).  Otherwise the coded size is encoded.
     */
    encode_width = context->width;
    encode_height = context->height;
    if (input->width <= context->width && input->height <= context->height &&
        context->width - input->width < ctu &&
        context->height - input->height < ctu) {
        encode_width = input->width;
        encode_height = input->height;
    }
    if ((encode_width & 1u) || (encode_height & 1u) ||
        encode_width < VENUS_MIN_WIDTH ||
        encode_height < VENUS_MIN_HEIGHT ||
        (uint64_t)encode_width * encode_height > AVE_HEVC_MAX_SAMPLES)
        return -ERANGE;
    ctu_count =
        ((sequence->pic_width_in_luma_samples + ctu - 1) / ctu) *
        ((sequence->pic_height_in_luma_samples + ctu - 1) / ctu);
    if (parameters->num_ctu_in_slice != ctu_count)
        return -ENOTSUP;

    status = ave_rate_settings_from_va(config->rate_control,
                                       &parameters->rate,
                                       sequence->bits_per_second, &rate);
    if (status < 0)
        return status;
    if (sequence->vui_num_units_in_tick && sequence->vui_time_scale)
        vui_fps = sequence->vui_time_scale /
                  sequence->vui_num_units_in_tick;

    status = venus_v4l2_encoder_compressed_size(
        encode_width, encode_height, &capture_size);
    if (status < 0)
        return status;

    encoder_config = (struct venus_v4l2_encoder_config) {
        .device = backend->capabilities.encoder_path,
        .coded_format = V4L2_PIX_FMT_HEVC,
        .raw_format = main10 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12,
        .width = encode_width,
        .height = encode_height,
        .frames_per_second =
            frame_rate_or_default(&parameters->rate, vui_fps),
        .bitrate_mode = rate.v4l2_mode,
        .bitrate = rate.bitrate,
        .bitrate_peak = rate.peak,
        .qp_range = rate.qp_range,
        .min_qp = rate.min_qp,
        .max_qp = rate.max_qp,
        .gop_size = ave_gop_from_va(sequence->intra_idr_period,
                                    sequence->intra_period),
        .hevc_profile = main10 ? V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10
                               : V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
        .hevc_level = level,
        .capture_buffer_size = capture_size,
        .output_buffers = VENUS_ENCODE_OUTPUT_BUFFERS,
        .capture_buffers = VENUS_ENCODE_CAPTURE_BUFFERS,
        .output_done = store_output_done,
        .output_done_opaque = context,
    };
    status = venus_v4l2_encoder_open(
        &encoder_config, &context->encoder, &error);
    if (status < 0) {
        log_open_failure("HEVC", &error, status);
        return status;
    }
    context->encode_width = encode_width;
    context->encode_height = encode_height;
    context->encode_frames_per_second = encoder_config.frames_per_second;
    context->encode_bitrate = rate.bitrate;
    return 0;
}

/*
 * Rate changes after the session started: apple-ave applies a new average
 * bit rate at the next frame (ave_build_process).  Mode, peak, frame rate
 * and QP range are read once at STREAMON and cannot change mid-stream.
 */
static int update_rate_locked(struct venus_context *context,
                              const struct venus_config *config,
                              const struct ave_rate_request *request,
                              uint32_t sequence_bits_per_second)
{
    struct ave_rate_settings rate;
    int status;

    if (!request->rate_seen && !sequence_bits_per_second)
        return 0;
    status = ave_rate_settings_from_va(config->rate_control, request,
                                       sequence_bits_per_second, &rate);
    if (status < 0)
        return status;
    if (rate.bitrate == context->encode_bitrate)
        return 0;
    status = venus_v4l2_encoder_set_bitrate(context->encoder, rate.bitrate);
    if (status < 0)
        return status;
    context->encode_bitrate = rate.bitrate;
    return 0;
}

VAStatus venus_encode_end_picture_locked(
    struct venus_backend *backend, struct venus_context *context,
    const struct venus_config *config)
{
    struct venus_h264_encode_parameters parameters;
    struct venus_hevc_encode_parameters hevc_parameters;
    struct venus_buffer *coded;
    struct venus_surface *surface;
    VABufferID coded_buffer_id;
    bool hevc = config->profile == VAProfileHEVCMain ||
                config->profile == VAProfileHEVCMain10;
    bool keyframe_requested;
    size_t coded_capacity;
    uint64_t frame_tag;
    int status;

    status = hevc
                 ? collect_hevc_parameters(backend, context,
                                           &hevc_parameters)
                 : collect_parameters(backend, context, &parameters);
    if (status < 0)
        return venus_backend_encode_status_from_errno(status);

    coded_buffer_id = hevc ? hevc_parameters.picture->coded_buf
                           : parameters.picture->coded_buf;
    coded = venus_backend_find_buffer(
        backend, coded_buffer_id);
    surface = venus_backend_find_surface(backend, context->target);
    if (!coded || coded->context_id != context->id ||
        coded->type != VAEncCodedBufferType)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!surface || !surface->ready)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (coded->element_size >
        SIZE_MAX / coded->capacity_elements)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    coded_capacity =
        coded->element_size * coded->capacity_elements;
    if (coded_capacity == 0 || coded_capacity > UINT32_MAX)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    if (!context->encoder) {
        status = hevc
                     ? open_hevc_encoder(backend, context, config,
                                         &hevc_parameters, surface)
                     : open_encoder(backend, context, config,
                                    &parameters);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
    } else if (hevc) {
        if (hevc_parameters.sequence &&
            (hevc_parameters.sequence->pic_width_in_luma_samples !=
                 context->width ||
             hevc_parameters.sequence->pic_height_in_luma_samples !=
                 context->height))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        status = update_rate_locked(
            context, config, &hevc_parameters.rate,
            hevc_parameters.sequence
                ? hevc_parameters.sequence->bits_per_second : 0);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
    } else {
        if (parameters.sequence) {
            uint32_t width;
            uint32_t height;

            status = venus_encode_h264_dimensions(
                parameters.sequence, &width, &height);
            if (status < 0 ||
                width != context->encode_width ||
                height != context->encode_height)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        status = update_rate_locked(
            context, config, &parameters.rate,
            parameters.sequence ? parameters.sequence->bits_per_second
                                : 0);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
    }

    /* An IDR or any intra picture is produced as an AVE key frame. */
    keyframe_requested = hevc
                             ? hevc_parameters.intra
                             : parameters.picture->pic_fields.bits
                                       .idr_pic_flag ||
                                   parameters.intra;
    if (context->encode_sequence > 0 && keyframe_requested) {
        /*
         * FORCE_KEY_FRAME applies to the next frame apple-ave hands to the
         * firmware.  Make sure that is this frame, not an earlier one
         * still waiting in the OUTPUT queue.
         */
        status = venus_v4l2_encoder_wait_output_idle(
            context->encoder, AVE_KEYFRAME_IDLE_TIMEOUT_MS,
            store_packet, context);
        if (status == 0)
            status = venus_v4l2_encoder_force_keyframe(context->encoder);
        if (status < 0)
            return venus_backend_encode_status_from_errno(status);
    }

    if (!context->encode_frames_per_second)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (surface->width < context->encode_width ||
        surface->height < context->encode_height)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    status = venus_surface_begin_cpu_read(surface);
    if (status < 0)
        return venus_backend_encode_status_from_errno(status);

    context->encode_sequence++;
    if (context->encode_sequence == 0)
        context->encode_sequence++;
    frame_tag = 1u + (context->encode_sequence - 1u) * 1000000u /
                        context->encode_frames_per_second;

    coded->coded_size = 0;
    coded->coded_ready = false;
    coded->coded_delivered = false;
    coded->input_done = false;
    coded->coded_packets = 0;
    coded->coded_sequence = 0;
    coded->coded_tag = 0;
    coded->keyframe_expected = keyframe_requested ||
                               context->encode_sequence == 1;
    coded->source_surface_id = surface->id;
    memset(&coded->coded_segment, 0,
           sizeof(coded->coded_segment));
    coded->coded_segment.buf = coded->data;

    status = venus_encode_queue_coded_buffer_locked(
        context, coded->id, frame_tag);
    if (status < 0) {
        venus_surface_end_cpu_read(surface);
        return venus_backend_encode_status_from_errno(status);
    }

    surface->encode_pending = true;
    surface->coded_buffer_id = coded->id;

    status = venus_v4l2_encoder_submit(
        context->encoder, surface->data, surface->data_size,
        surface->stride, surface->uv_offset,
        frame_tag, store_packet, context);
    {
        int sync_status = venus_surface_end_cpu_read(surface);

        if (sync_status < 0 && status == 0)
            status = sync_status;
    }
    if (status < 0) {
        rollback_coded_buffer(context, coded->id);
        surface->encode_pending = false;
        return venus_backend_encode_status_from_errno(status);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus venus_encode_sync_buffer_locked(
    struct venus_backend *backend, struct venus_buffer *buffer,
    int timeout_ms)
{
    struct venus_context *context;
    int64_t deadline;

    if (!buffer || buffer->type != VAEncCodedBufferType)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (buffer->coded_ready)
        return VA_STATUS_SUCCESS;

    context = venus_backend_find_context(
        backend, buffer->context_id);
    if (!context || !context->encoder)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    deadline = monotonic_milliseconds() + timeout_ms;
    while (!buffer->coded_ready &&
           monotonic_milliseconds() < deadline) {
        int64_t remaining =
            deadline - monotonic_milliseconds();
        int wait_ms;
        int status;

        if (remaining <= 0)
            break;
        wait_ms = remaining > 1000 ? 1000 : (int)remaining;
        status = venus_v4l2_encoder_pump(
            context->encoder, wait_ms,
            store_packet, context, NULL);
        if (status == -ETIMEDOUT || status == -EAGAIN)
            continue;
        if (status < 0) {
            return venus_backend_encode_status_from_errno(
                status);
        }
    }

    return buffer->coded_ready
               ? VA_STATUS_SUCCESS
               : VA_STATUS_ERROR_HW_BUSY;
}

VAStatus venus_encode_sync_surface_locked(
    struct venus_backend *backend, struct venus_surface *surface,
    int timeout_ms)
{
    struct venus_buffer *buffer;
    struct venus_context *context;
    int64_t deadline;

    if (!surface->encode_pending)
        return surface->ready
                   ? VA_STATUS_SUCCESS
                   : VA_STATUS_ERROR_SURFACE_BUSY;

    buffer = venus_backend_find_buffer(
        backend, surface->coded_buffer_id);
    if (!buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    context = venus_backend_find_context(
        backend, buffer->context_id);
    if (!context || !context->encoder)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    deadline = monotonic_milliseconds() + timeout_ms;
    while (surface->encode_pending &&
           monotonic_milliseconds() < deadline) {
        int64_t remaining =
            deadline - monotonic_milliseconds();
        int wait_ms;
        int status;

        if (remaining <= 0)
            break;
        wait_ms = remaining > 1000 ? 1000 : (int)remaining;
        status = venus_v4l2_encoder_pump(
            context->encoder, wait_ms,
            store_packet, context, NULL);
        if (status == -ETIMEDOUT || status == -EAGAIN)
            continue;
        if (status < 0) {
            return venus_backend_encode_status_from_errno(
                status);
        }
    }

    return surface->encode_pending
               ? VA_STATUS_ERROR_HW_BUSY
               : VA_STATUS_SUCCESS;
}

void venus_encode_close_context(struct venus_context *context)
{
    if (!context)
        return;

    venus_v4l2_encoder_close(context->encoder);
    context->encoder = NULL;
    context->encode_dmabuf = false;
    context->encode_queue_head = 0;
    context->encode_queue_count = 0;
}
