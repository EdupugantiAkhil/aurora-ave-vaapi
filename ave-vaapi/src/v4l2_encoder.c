// SPDX-License-Identifier: MIT
#include "v4l2_encoder.h"
#include "v4l2_probe.h"

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct mapped_buffer {
    void *data;
    size_t length;
    int dma_fd;
    uint64_t tag;
    bool queued;
};

struct venus_v4l2_encoder {
    int fd;
    struct mapped_buffer *output;
    struct mapped_buffer *capture;
    unsigned int output_count;
    unsigned int capture_count;
    struct v4l2_pix_format_mplane output_format;
    struct v4l2_pix_format_mplane capture_format;
    uint32_t visible_width;
    uint32_t visible_height;
    enum v4l2_memory output_memory;
    bool output_streaming;
    bool capture_streaming;
    bool stop_sent;
    venus_v4l2_output_done_callback output_done;
    void *output_done_opaque;
    char last_operation[64];
};

/*
 * apple-ave is a stateful V4L2 M2M encoder: raw NV12/P010 frames are queued
 * on OUTPUT and Annex B access units are dequeued from CAPTURE.  Both queues
 * are MMAP; raw frames are copied into AVE's layout (stride = S_FMT
 * bytesperline, chroma straight after the visible luma rows).
 */
static int xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    return result;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 +
           now.tv_nsec / 1000000;
}

static int encoder_error(struct venus_v4l2_encoder *encoder,
                         const char *operation)
{
    snprintf(encoder->last_operation, sizeof(encoder->last_operation),
             "%s", operation);
    return -errno;
}

static int verify_device(struct venus_v4l2_encoder *encoder)
{
    struct v4l2_capability capability = { 0 };

    if (xioctl(encoder->fd, VIDIOC_QUERYCAP, &capability) < 0)
        return encoder_error(encoder, "VIDIOC_QUERYCAP");

    if (!ave_capability_is_encoder(&capability)) {
        errno = ENODEV;
        return encoder_error(encoder, "encoder capability contract");
    }

    return 0;
}

static unsigned int raw_bytes_per_sample(uint32_t raw_format)
{
    return raw_format == V4L2_PIX_FMT_P010 ? 2u : 1u;
}

/* The coded format first: apple-ave sizes and validates the raw side from it. */
static int set_capture_format(
    struct venus_v4l2_encoder *encoder,
    const struct venus_v4l2_encoder_config *config)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };

    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = config->coded_format;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage =
        (uint32_t)config->capture_buffer_size;

    if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) < 0)
        return encoder_error(encoder, "VIDIOC_S_FMT(CAPTURE)");

    if (format.fmt.pix_mp.pixelformat != config->coded_format ||
        format.fmt.pix_mp.num_planes != 1 ||
        format.fmt.pix_mp.plane_fmt[0].sizeimage == 0) {
        errno = EINVAL;
        return encoder_error(encoder, "coded capture format contract");
    }

    encoder->capture_format = format.fmt.pix_mp;
    return 0;
}

static int set_output_format(
    struct venus_v4l2_encoder *encoder,
    const struct venus_v4l2_encoder_config *config)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    const struct v4l2_pix_format_mplane *result = &format.fmt.pix_mp;
    uint64_t row_bytes =
        (uint64_t)config->width * raw_bytes_per_sample(config->raw_format);

    format.fmt.pix_mp.width = config->width;
    format.fmt.pix_mp.height = config->height;
    format.fmt.pix_mp.pixelformat = config->raw_format;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;

    if (xioctl(encoder->fd, VIDIOC_S_FMT, &format) < 0)
        return encoder_error(encoder, "VIDIOC_S_FMT(OUTPUT)");

    /*
     * apple-ave clamps sizes and silently swaps P010 for NV12 when the
     * pairing is not HEVC; any adjustment means this request cannot be
     * encoded as asked.
     */
    if (result->pixelformat != config->raw_format ||
        result->num_planes != 1 ||
        result->width != config->width ||
        result->height != config->height ||
        result->plane_fmt[0].bytesperline < row_bytes ||
        (uint64_t)result->plane_fmt[0].sizeimage <
            (uint64_t)result->plane_fmt[0].bytesperline *
                config->height * 3u / 2u) {
        errno = ERANGE;
        return encoder_error(encoder, "raw output format contract");
    }

    encoder->output_format = format.fmt.pix_mp;
    return 0;
}

/*
 * Set one control and check that the driver kept the value: integer
 * controls are clamped by the V4L2 core rather than rejected, which would
 * otherwise hide an unsupported request.
 */
static int set_control(struct venus_v4l2_encoder *encoder,
                       uint32_t id, int32_t value,
                       const char *operation)
{
    struct v4l2_control control = {
        .id = id,
        .value = value,
    };

    if (xioctl(encoder->fd, VIDIOC_S_CTRL, &control) < 0)
        return encoder_error(encoder, operation);
    if (id != V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME &&
        control.value != value) {
        errno = ERANGE;
        return encoder_error(encoder, operation);
    }
    return 0;
}

int venus_v4l2_encoder_compressed_size(
    uint32_t width, uint32_t height, size_t *size)
{
    size_t aligned_width;
    size_t aligned_height;
    size_t compressed;

    if (!width || !height || !size)
        return -EINVAL;
    /* apple-ave's own minimum (ave_fill_coded): 1.5 B/pixel + 64 KiB. */
    aligned_width = ((size_t)width + 31u) & ~(size_t)31u;
    aligned_height = ((size_t)height + 15u) & ~(size_t)15u;
    if (aligned_width > SIZE_MAX / aligned_height / 3u)
        return -EOVERFLOW;
    compressed = aligned_width * aligned_height * 3u / 2u;
    compressed = (compressed + 16383u) & ~(size_t)16383u;
    *size = compressed + 65536u;
    return 0;
}

static uint32_t hevc_level_limit(void)
{
    return V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1;
}

int ave_encoder_controls(const struct venus_v4l2_encoder_config *config,
                         struct ave_control *controls, size_t max_controls,
                         size_t *count)
{
    size_t n = 0;
    bool hevc;

#define AVE_PUSH(control_id, control_value, control_name)                  \
    do {                                                                    \
        if (n >= max_controls)                                              \
            return -ENOSPC;                                                 \
        controls[n++] = (struct ave_control) {                              \
            (control_id), (int32_t)(control_value), (control_name)          \
        };                                                                  \
    } while (0)

    if (!config || !controls || !count)
        return -EINVAL;
    *count = 0;
    hevc = config->coded_format == V4L2_PIX_FMT_HEVC;
    if (!hevc && config->coded_format != V4L2_PIX_FMT_H264)
        return -ENOTSUP;

    /* apple-ave ranges (ave_init_ctrls): bit rate 100 kbit/s..100 Mbit/s. */
    if (config->bitrate < 100000u || config->bitrate > 100000000u)
        return -ERANGE;
    if (config->bitrate_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_CBR &&
        config->bitrate_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
        return -ENOTSUP;
    if (config->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR &&
        config->bitrate_peak &&
        (config->bitrate_peak < config->bitrate ||
         config->bitrate_peak > 200000000u))
        return -ERANGE;
    if (config->gop_size < 1 || config->gop_size > 65535u)
        return -ERANGE;
    if (config->qp_range &&
        (config->min_qp > config->max_qp || config->max_qp > 51u))
        return -ERANGE;

    /* Low-delay P frames: VA encode returns one coded frame per input. */
    AVE_PUSH(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "S_CTRL(B_FRAMES)");
    AVE_PUSH(V4L2_CID_MPEG_VIDEO_HEADER_MODE,
             V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
             "S_CTRL(HEADER_MODE)");
    AVE_PUSH(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, config->bitrate_mode,
             "S_CTRL(BITRATE_MODE)");
    AVE_PUSH(V4L2_CID_MPEG_VIDEO_BITRATE, config->bitrate,
             "S_CTRL(BITRATE)");
    if (config->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
        AVE_PUSH(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, config->bitrate_peak,
                 "S_CTRL(BITRATE_PEAK)");
    AVE_PUSH(V4L2_CID_MPEG_VIDEO_GOP_SIZE, config->gop_size,
             "S_CTRL(GOP_SIZE)");

    if (hevc) {
        if (config->hevc_profile != V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN &&
            config->hevc_profile != V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10)
            return -ENOTSUP;
        if ((config->hevc_profile == V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10) !=
            (config->raw_format == V4L2_PIX_FMT_P010))
            return -EINVAL;
        if (config->hevc_level > hevc_level_limit())
            return -ENOTSUP;
        AVE_PUSH(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, config->hevc_profile,
                 "S_CTRL(HEVC_PROFILE)");
        AVE_PUSH(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, config->hevc_level,
                 "S_CTRL(HEVC_LEVEL)");
        if (config->qp_range) {
            AVE_PUSH(V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, config->min_qp,
                     "S_CTRL(HEVC_MIN_QP)");
            AVE_PUSH(V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, config->max_qp,
                     "S_CTRL(HEVC_MAX_QP)");
        }
    } else {
        if (config->raw_format != V4L2_PIX_FMT_NV12)
            return -EINVAL;
        if (config->h264_profile !=
                V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE &&
            config->h264_profile != V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE &&
            config->h264_profile != V4L2_MPEG_VIDEO_H264_PROFILE_MAIN &&
            config->h264_profile != V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)
            return -ENOTSUP;
        if (config->h264_level > V4L2_MPEG_VIDEO_H264_LEVEL_5_2)
            return -ENOTSUP;
        AVE_PUSH(V4L2_CID_MPEG_VIDEO_H264_PROFILE, config->h264_profile,
                 "S_CTRL(H264_PROFILE)");
        AVE_PUSH(V4L2_CID_MPEG_VIDEO_H264_LEVEL, config->h264_level,
                 "S_CTRL(H264_LEVEL)");
        if (config->qp_range) {
            AVE_PUSH(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, config->min_qp,
                     "S_CTRL(H264_MIN_QP)");
            AVE_PUSH(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, config->max_qp,
                     "S_CTRL(H264_MAX_QP)");
        }
    }
#undef AVE_PUSH

    *count = n;
    return 0;
}

static int set_parameters(
    struct venus_v4l2_encoder *encoder,
    const struct venus_v4l2_encoder_config *config)
{
    struct v4l2_streamparm parameters = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    struct ave_control controls[AVE_MAX_CONTROLS];
    size_t count;
    size_t index;
    int status;

    status = ave_encoder_controls(config, controls, AVE_MAX_CONTROLS,
                                  &count);
    if (status < 0) {
        snprintf(encoder->last_operation, sizeof(encoder->last_operation),
                 "unsupported encoder parameters");
        return status;
    }

    parameters.parm.output.timeperframe.numerator = 1;
    parameters.parm.output.timeperframe.denominator =
        config->frames_per_second;
    if (xioctl(encoder->fd, VIDIOC_S_PARM, &parameters) < 0)
        return encoder_error(encoder, "VIDIOC_S_PARM(OUTPUT)");
    if (parameters.parm.output.timeperframe.numerator != 1 ||
        parameters.parm.output.timeperframe.denominator !=
            config->frames_per_second) {
        errno = ERANGE;
        return encoder_error(encoder, "VIDIOC_S_PARM(OUTPUT) adjusted");
    }

    for (index = 0; index < count; index++) {
        status = set_control(encoder, controls[index].id,
                             controls[index].value, controls[index].name);
        if (status < 0)
            return status;
    }
    return 0;
}

static unsigned int device_min_output_buffers(
    struct venus_v4l2_encoder *encoder)
{
    struct v4l2_control control = {
        .id = V4L2_CID_MIN_BUFFERS_FOR_OUTPUT,
    };

    if (xioctl(encoder->fd, VIDIOC_G_CTRL, &control) < 0 ||
        control.value < 1)
        return 0;
    return (unsigned int)control.value;
}

static int request_and_map(struct venus_v4l2_encoder *encoder,
                           enum v4l2_buf_type type,
                           unsigned int requested,
                           struct mapped_buffer **mapped,
                           unsigned int *mapped_count)
{
    struct v4l2_requestbuffers request = {
        .count = requested,
        .type = type,
        .memory = V4L2_MEMORY_MMAP,
    };
    struct mapped_buffer *buffers;
    unsigned int index;

    if (xioctl(encoder->fd, VIDIOC_REQBUFS, &request) < 0)
        return encoder_error(
            encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                         ? "VIDIOC_REQBUFS(OUTPUT)"
                         : "VIDIOC_REQBUFS(CAPTURE)");
    if (request.count == 0)
        return -ENOMEM;

    buffers = calloc(request.count, sizeof(*buffers));
    if (!buffers)
        return -ENOMEM;

    for (index = 0; index < request.count; index++)
        buffers[index].dma_fd = -1;

    for (index = 0; index < request.count; index++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer buffer = {
            .index = index,
            .type = type,
            .memory = V4L2_MEMORY_MMAP,
            .length = 1,
            .m.planes = planes,
        };

        if (xioctl(encoder->fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            encoder_error(
                encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                             ? "VIDIOC_QUERYBUF(OUTPUT)"
                             : "VIDIOC_QUERYBUF(CAPTURE)");
            goto fail;
        }
        if (buffer.length != 1 || planes[0].length == 0) {
            errno = EINVAL;
            encoder_error(encoder, "VIDIOC_QUERYBUF(plane contract)");
            goto fail;
        }

        buffers[index].length = planes[0].length;
        buffers[index].data =
            mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                 MAP_SHARED, encoder->fd, planes[0].m.mem_offset);
        if (buffers[index].data == MAP_FAILED) {
            buffers[index].data = NULL;
            encoder_error(
                encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                             ? "mmap(OUTPUT)"
                             : "mmap(CAPTURE)");
            goto fail;
        }
    }

    *mapped = buffers;
    *mapped_count = request.count;
    return 0;

fail:
    {
        int status = -errno;

        while (index > 0) {
            index--;
            if (buffers[index].data)
                munmap(buffers[index].data, buffers[index].length);
        }
        free(buffers);
        return status;
    }
}

static void release_buffers(struct venus_v4l2_encoder *encoder,
                            enum v4l2_buf_type type,
                            enum v4l2_memory memory,
                            struct mapped_buffer **mapped,
                            unsigned int *mapped_count)
{
    struct v4l2_requestbuffers request = {
        .count = 0,
        .type = type,
        .memory = memory,
    };
    unsigned int index;

    if (*mapped) {
        for (index = 0; index < *mapped_count; index++) {
            if ((*mapped)[index].data)
                munmap((*mapped)[index].data,
                       (*mapped)[index].length);
            if ((*mapped)[index].dma_fd >= 0)
                close((*mapped)[index].dma_fd);
        }
        free(*mapped);
        *mapped = NULL;
        *mapped_count = 0;
    }

    if (encoder->fd >= 0)
        xioctl(encoder->fd, VIDIOC_REQBUFS, &request);
}

static int queue_capture(struct venus_v4l2_encoder *encoder,
                         unsigned int index)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer buffer = {
        .index = index,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .length = 1,
        .m.planes = planes,
    };

    planes[0].length = (uint32_t)encoder->capture[index].length;
    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) < 0)
        return encoder_error(encoder, "VIDIOC_QBUF(CAPTURE)");

    encoder->capture[index].queued = true;
    return 0;
}

static int stream_on(struct venus_v4l2_encoder *encoder,
                     enum v4l2_buf_type type)
{
    if (xioctl(encoder->fd, VIDIOC_STREAMON, &type) < 0)
        return encoder_error(
            encoder, type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                         ? "VIDIOC_STREAMON(OUTPUT)"
                         : "VIDIOC_STREAMON(CAPTURE)");

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
        encoder->output_streaming = true;
    else
        encoder->capture_streaming = true;
    return 0;
}

static void stream_off(struct venus_v4l2_encoder *encoder,
                       enum v4l2_buf_type type)
{
    if (encoder->fd >= 0)
        xioctl(encoder->fd, VIDIOC_STREAMOFF, &type);
}

static void subscribe_eos(struct venus_v4l2_encoder *encoder)
{
    struct v4l2_event_subscription subscription = {
        .type = V4L2_EVENT_EOS,
    };

    xioctl(encoder->fd, VIDIOC_SUBSCRIBE_EVENT, &subscription);
}

int venus_v4l2_encoder_open(
    const struct venus_v4l2_encoder_config *config,
    struct venus_v4l2_encoder **result,
    struct venus_v4l2_error *error)
{
    struct venus_v4l2_encoder *encoder;
    unsigned int output_buffers;
    unsigned int minimum;
    unsigned int index;
    int status;

    if (error)
        memset(error, 0, sizeof(*error));
    if (result)
        *result = NULL;

    if (!config || !result || !config->device ||
        (config->coded_format != V4L2_PIX_FMT_H264 &&
         config->coded_format != V4L2_PIX_FMT_HEVC) ||
        (config->raw_format != V4L2_PIX_FMT_NV12 &&
         !(config->coded_format == V4L2_PIX_FMT_HEVC &&
           config->raw_format == V4L2_PIX_FMT_P010)) ||
        config->width == 0 || config->height == 0 ||
        (config->width & 1u) || (config->height & 1u) ||
        config->frames_per_second == 0 ||
        config->frames_per_second > 240 ||
        config->capture_buffer_size == 0 ||
        config->capture_buffer_size > UINT32_MAX ||
        config->output_buffers == 0 ||
        config->capture_buffers == 0)
        return -EINVAL;

    encoder = calloc(1, sizeof(*encoder));
    if (!encoder)
        return -ENOMEM;
    encoder->fd = -1;
    encoder->visible_width = config->width;
    encoder->visible_height = config->height;
    encoder->output_memory = V4L2_MEMORY_MMAP;
    encoder->output_done = config->output_done;
    encoder->output_done_opaque = config->output_done_opaque;

    encoder->fd =
        open(config->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (encoder->fd < 0) {
        status = -errno;
        snprintf(encoder->last_operation,
                 sizeof(encoder->last_operation),
                 "open(encoder device)");
        goto fail;
    }

    status = verify_device(encoder);
    if (status < 0)
        goto fail;
    status = set_capture_format(encoder, config);
    if (status < 0)
        goto fail;
    status = set_output_format(encoder, config);
    if (status < 0)
        goto fail;

    /* The firmware holds up to AVE_MAX_SLOTS frames; the driver says so. */
    output_buffers = config->output_buffers;
    minimum = device_min_output_buffers(encoder);
    if (minimum > output_buffers)
        output_buffers = minimum > 16u ? 16u : minimum;

    status = request_and_map(
        encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, output_buffers,
        &encoder->output, &encoder->output_count);
    if (status < 0)
        goto fail;
    status = request_and_map(
        encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        config->capture_buffers,
        &encoder->capture,
        &encoder->capture_count);
    if (status < 0)
        goto fail;

    for (index = 0; index < encoder->capture_count; index++) {
        status = queue_capture(encoder, index);
        if (status < 0)
            goto fail;
    }

    subscribe_eos(encoder);
    /* apple-ave reads its controls when both queues start streaming. */
    status = set_parameters(encoder, config);
    if (status < 0)
        goto fail;

    *result = encoder;
    return 0;

fail:
    if (error) {
        error->code = -status;
        snprintf(error->operation, sizeof(error->operation), "%s",
                 venus_v4l2_encoder_last_operation(encoder));
    }
    venus_v4l2_encoder_close(encoder);
    return status;
}

static int dequeue_output(struct venus_v4l2_encoder *encoder,
                          bool *made_progress)
{
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = encoder->output_memory,
            .length = 1,
            .m.planes = planes,
        };

        if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                return 0;
            return encoder_error(encoder, "VIDIOC_DQBUF(OUTPUT)");
        }
        if (buffer.index >= encoder->output_count)
            return -EIO;

        uint64_t tag = encoder->output[buffer.index].tag;

        encoder->output[buffer.index].queued = false;
        encoder->output[buffer.index].tag = 0;
        if (encoder->output[buffer.index].dma_fd >= 0) {
            close(encoder->output[buffer.index].dma_fd);
            encoder->output[buffer.index].dma_fd = -1;
        }
        if (tag && encoder->output_done) {
            int status = encoder->output_done(
                tag, encoder->output_done_opaque);

            if (status < 0)
                return status;
        }
        *made_progress = true;
    }
}

static int dequeue_capture(struct venus_v4l2_encoder *encoder,
                           venus_v4l2_packet_callback callback,
                           void *opaque, bool *end_of_stream,
                           bool *made_progress)
{
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .length = 1,
            .m.planes = planes,
        };
        struct venus_v4l2_packet packet;
        int status;

        if (xioctl(encoder->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                return 0;
            return encoder_error(encoder, "VIDIOC_DQBUF(CAPTURE)");
        }
        if (buffer.index >= encoder->capture_count ||
            planes[0].bytesused > encoder->capture[buffer.index].length)
            return -EIO;

        encoder->capture[buffer.index].queued = false;
        *made_progress = true;
        packet = (struct venus_v4l2_packet) {
            .data = encoder->capture[buffer.index].data,
            .size = planes[0].bytesused,
            .tag = (uint64_t)buffer.timestamp.tv_sec * 1000000u +
                   (uint64_t)buffer.timestamp.tv_usec,
            .flags = buffer.flags,
        };

        /* Venus wraps VP8 CAPTURE data in an IVF per-frame header. */
        if (encoder->capture_format.pixelformat == V4L2_PIX_FMT_VP8 &&
            packet.size >= 12) {
            uint32_t frame_size = (uint32_t)packet.data[0] |
                                  (uint32_t)packet.data[1] << 8 |
                                  (uint32_t)packet.data[2] << 16 |
                                  (uint32_t)packet.data[3] << 24;

            if (frame_size == packet.size - 12) {
                packet.data += 12;
                packet.size = frame_size;
            }
        }

        /* Errored frames come back empty but still finish a VA picture. */
        if ((packet.size > 0 || (buffer.flags & V4L2_BUF_FLAG_ERROR)) &&
            callback) {
            status = callback(&packet, opaque);
            if (status < 0)
                return status;
        }

        if (buffer.flags & V4L2_BUF_FLAG_LAST) {
            if (end_of_stream)
                *end_of_stream = true;
            return 0;
        }

        status = queue_capture(encoder, buffer.index);
        if (status < 0)
            return status;
    }
}

int venus_v4l2_encoder_pump(struct venus_v4l2_encoder *encoder,
                            int timeout_ms,
                            venus_v4l2_packet_callback callback,
                            void *opaque, bool *end_of_stream)
{
    struct pollfd poll_fd;
    bool made_progress = false;
    int status;

    if (!encoder || encoder->fd < 0 || timeout_ms < 0)
        return -EINVAL;
    if (end_of_stream)
        *end_of_stream = false;

    poll_fd = (struct pollfd) {
        .fd = encoder->fd,
        .events = POLLIN | POLLOUT | POLLPRI,
    };
    do {
        status = poll(&poll_fd, 1, timeout_ms);
    } while (status < 0 && errno == EINTR);
    if (status < 0)
        return encoder_error(encoder, "poll");
    if (status == 0)
        return -ETIMEDOUT;

    status = dequeue_output(encoder, &made_progress);
    if (status < 0)
        return status;
    status = dequeue_capture(
        encoder, callback, opaque, end_of_stream, &made_progress);
    if (status < 0)
        return status;

    if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
        !made_progress)
        return -EIO;
    return made_progress ? 0 : -EAGAIN;
}

static int find_free_output(struct venus_v4l2_encoder *encoder,
                            venus_v4l2_packet_callback callback,
                            void *opaque)
{
    unsigned int index;

    for (;;) {
        for (index = 0; index < encoder->output_count; index++) {
            if (!encoder->output[index].queued)
                return (int)index;
        }

        {
            int status = venus_v4l2_encoder_pump(
                encoder, 5000, callback, opaque, NULL);

            if (status < 0 && status != -EAGAIN)
                return status;
        }
    }
}

int ave_pack_frame(uint8_t *destination, size_t destination_size,
                   uint32_t destination_stride,
                   const uint8_t *source_y, const uint8_t *source_uv,
                   uint32_t source_stride, uint32_t width, uint32_t height,
                   unsigned int bytes_per_sample, size_t *packed_size)
{
    size_t row_bytes;
    size_t required;
    size_t chroma;
    unsigned int row;

    if (!destination || !source_y || !source_uv || !packed_size ||
        !width || !height || (width & 1u) || (height & 1u) ||
        (bytes_per_sample != 1 && bytes_per_sample != 2))
        return -EINVAL;
    row_bytes = (size_t)width * bytes_per_sample;
    if (destination_stride < row_bytes || source_stride < row_bytes)
        return -EINVAL;
    if ((size_t)destination_stride > SIZE_MAX / height / 2u)
        return -EOVERFLOW;
    chroma = (size_t)destination_stride * height;
    required = chroma + (size_t)destination_stride * (height / 2u);
    if (required > destination_size)
        return -ENOSPC;

    for (row = 0; row < height; row++)
        memcpy(destination + (size_t)row * destination_stride,
               source_y + (size_t)row * source_stride, row_bytes);
    for (row = 0; row < height / 2u; row++)
        memcpy(destination + chroma + (size_t)row * destination_stride,
               source_uv + (size_t)row * source_stride, row_bytes);

    *packed_size = required;
    return 0;
}

int venus_v4l2_encoder_submit(
    struct venus_v4l2_encoder *encoder,
    const uint8_t *data, size_t size,
    uint32_t stride, size_t uv_offset,
    uint64_t tag, venus_v4l2_packet_callback callback,
    void *opaque)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .length = 1,
        .m.planes = planes,
    };
    unsigned int bytes_per_sample;
    uint32_t output_size;
    size_t row_bytes;
    size_t packed_size;
    int index;
    int status;

    if (!encoder || !data || !size || !tag)
        return -EINVAL;

    bytes_per_sample =
        raw_bytes_per_sample(encoder->output_format.pixelformat);
    row_bytes = (size_t)encoder->visible_width * bytes_per_sample;
    /* Source: luma rows at @stride, chroma rows from @uv_offset. */
    if (stride < row_bytes ||
        uv_offset < (size_t)stride * encoder->visible_height ||
        uv_offset > size ||
        size - uv_offset <
            (size_t)stride * (encoder->visible_height / 2u - 1u) +
                row_bytes)
        return -EINVAL;

    output_size = encoder->output_format.plane_fmt[0].sizeimage;
    if (!output_size)
        return -EINVAL;

    index = find_free_output(encoder, callback, opaque);
    if (index < 0)
        return index;
    if (output_size > encoder->output[index].length)
        return -ENOSPC;

    status = ave_pack_frame(
        encoder->output[index].data, encoder->output[index].length,
        encoder->output_format.plane_fmt[0].bytesperline,
        data, data + uv_offset, stride,
        encoder->visible_width, encoder->visible_height,
        bytes_per_sample, &packed_size);
    if (status < 0)
        return status;
    if (packed_size > output_size)
        return -ENOSPC;

    buffer.index = (unsigned int)index;
    buffer.field = V4L2_FIELD_NONE;
    buffer.timestamp.tv_sec = (long)(tag / 1000000u);
    buffer.timestamp.tv_usec = (long)(tag % 1000000u);
    /*
     * A payload of the full sizeimage tells apple-ave the frame already
     * uses its stride (ave_sess_input only re-strides smaller payloads).
     */
    planes[0].bytesused = output_size;
    planes[0].data_offset = 0;
    planes[0].length = (uint32_t)encoder->output[index].length;

    if (xioctl(encoder->fd, VIDIOC_QBUF, &buffer) < 0) {
        int saved_errno = errno;

        snprintf(encoder->last_operation,
                 sizeof(encoder->last_operation),
                 "QBUF_OUT %ux%u stride=%u used=%u",
                 encoder->visible_width, encoder->visible_height,
                 encoder->output_format.plane_fmt[0].bytesperline,
                 output_size);
        return -saved_errno;
    }

    encoder->output[index].tag = tag;
    encoder->output[index].queued = true;

    if (!encoder->output_streaming) {
        status = stream_on(
            encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        if (status < 0)
            return status;
    }
    if (!encoder->capture_streaming) {
        status = stream_on(
            encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
        if (status < 0)
            return status;
    }

    return 0;
}

static bool output_idle(const struct venus_v4l2_encoder *encoder)
{
    unsigned int index;

    for (index = 0; index < encoder->output_count; index++) {
        if (encoder->output[index].queued)
            return false;
    }
    return true;
}

int venus_v4l2_encoder_wait_output_idle(
    struct venus_v4l2_encoder *encoder, int timeout_ms,
    venus_v4l2_packet_callback callback, void *opaque)
{
    int64_t deadline;

    if (!encoder || encoder->fd < 0 || timeout_ms < 0)
        return -EINVAL;
    deadline = monotonic_milliseconds() + timeout_ms;
    while (!output_idle(encoder)) {
        int64_t remaining = deadline - monotonic_milliseconds();
        int status;

        if (remaining <= 0)
            return -ETIMEDOUT;
        status = venus_v4l2_encoder_pump(
            encoder, remaining > 1000 ? 1000 : (int)remaining,
            callback, opaque, NULL);
        if (status < 0 && status != -EAGAIN && status != -ETIMEDOUT)
            return status;
    }
    return 0;
}

int venus_v4l2_encoder_force_keyframe(
    struct venus_v4l2_encoder *encoder)
{
    if (!encoder || encoder->fd < 0)
        return -EINVAL;
    return set_control(
        encoder, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
        0, "S_CTRL(FORCE_KEY_FRAME)");
}

int venus_v4l2_encoder_set_bitrate(
    struct venus_v4l2_encoder *encoder, uint32_t bitrate)
{
    if (!encoder || encoder->fd < 0)
        return -EINVAL;
    if (bitrate < 100000u || bitrate > 100000000u)
        return -ERANGE;
    return set_control(encoder, V4L2_CID_MPEG_VIDEO_BITRATE,
                       (int32_t)bitrate, "S_CTRL(BITRATE)");
}

int venus_v4l2_encoder_stop(struct venus_v4l2_encoder *encoder)
{
    struct v4l2_encoder_cmd command = {
        .cmd = V4L2_ENC_CMD_STOP,
    };

    if (!encoder || encoder->fd < 0)
        return -EINVAL;
    if (encoder->stop_sent)
        return 0;

    if (xioctl(encoder->fd, VIDIOC_ENCODER_CMD, &command) < 0)
        return encoder_error(encoder, "VIDIOC_ENCODER_CMD(STOP)");

    encoder->stop_sent = true;
    return 0;
}

void venus_v4l2_encoder_close(struct venus_v4l2_encoder *encoder)
{
    int64_t deadline;

    if (!encoder)
        return;

    if (encoder->output_streaming &&
        encoder->capture_streaming &&
        !encoder->stop_sent &&
        venus_v4l2_encoder_stop(encoder) == 0) {
        deadline = monotonic_milliseconds() + 2000;
        while (monotonic_milliseconds() < deadline) {
            bool eos = false;
            int status = venus_v4l2_encoder_pump(
                encoder, 200, NULL, NULL, &eos);

            if (eos || (status < 0 &&
                        status != -EAGAIN &&
                        status != -ETIMEDOUT))
                break;
        }
    }

    if (encoder->output_streaming)
        stream_off(
            encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (encoder->capture_streaming)
        stream_off(
            encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    release_buffers(
        encoder, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        V4L2_MEMORY_MMAP, &encoder->capture,
        &encoder->capture_count);
    release_buffers(
        encoder, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        encoder->output_memory, &encoder->output,
        &encoder->output_count);

    if (encoder->fd >= 0)
        close(encoder->fd);
    free(encoder);
}

const char *venus_v4l2_encoder_last_operation(
    const struct venus_v4l2_encoder *encoder)
{
    if (!encoder || encoder->last_operation[0] == '\0')
        return "none";
    return encoder->last_operation;
}

unsigned int venus_v4l2_encoder_output_count(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->output_count : 0;
}

unsigned int venus_v4l2_encoder_capture_count(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->capture_count : 0;
}

uint32_t venus_v4l2_encoder_output_size(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->output_format.plane_fmt[0].sizeimage : 0;
}

uint32_t venus_v4l2_encoder_capture_size(
    const struct venus_v4l2_encoder *encoder)
{
    return encoder ? encoder->capture_format.plane_fmt[0].sizeimage : 0;
}
