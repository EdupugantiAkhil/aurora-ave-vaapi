// SPDX-License-Identifier: MIT
/*
 * Host-only end-to-end check with a fake apple-ave V4L2 node.  open, ioctl,
 * mmap, munmap, close and poll are wrapped at link time (-Wl,--wrap=...);
 * only the fake path below is emulated, everything else goes to libc.
 * The fake follows the apple-ave format rules from ave-v4l2.c
 * (ave_fill_raw / ave_fill_coded / ave_s_fmt_out) and records the OUTPUT
 * payload so the AVE raw layout can be checked.
 *
 * Reproduces FFmpeg's hevc_vaapi -profile:v main10 sequence after
 * "format=p010,hwupload" at 1920x1080 (coded 1920x1088), and the same for
 * H.264 and HEVC Main for comparison.
 */
#include "backend.h"
#include "backend_internal.h"
#include "capabilities.h"
#include "va_stubs.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <va/va_backend.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#define FAKE_PATH "/nonexistent/fake-apple-ave"
#define FAKE_FD 9001
#define FAKE_BUFS 16

static int failures;
/* Lazy mode: frames finish only when the client polls (like hardware). */
static bool lazy;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,          \
                    __LINE__, #condition);                                  \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/* ---- fake apple-ave ---------------------------------------------------- */

struct fake_buf {
    uint8_t *mem;
    size_t size;
    bool queued;
    bool done;
    struct timeval timestamp;
    uint32_t bytesused;
    uint32_t flags;
};

static struct {
    bool open;
    struct v4l2_pix_format_mplane src, dst;
    struct fake_buf out[FAKE_BUFS], cap[FAKE_BUFS];
    unsigned int nout, ncap;
    bool out_on, cap_on;
    unsigned int frames;
    int32_t b_frames;
    /* Copy of the last raw frame (first 2 rows of luma and chroma). */
    uint8_t last_luma0, last_chroma0;
    uint32_t last_stride, last_height, last_bytesused, last_sizeimage;
} ave;

#define ALIGN_UP(x, a) (((x) + (a) - 1) / (a) * (a))

static void fake_fill_raw(struct v4l2_pix_format_mplane *f, uint32_t coded)
{
    uint32_t bytes, stride;

    if (f->pixelformat != V4L2_PIX_FMT_P010 || coded != V4L2_PIX_FMT_HEVC)
        f->pixelformat = V4L2_PIX_FMT_NV12;
    bytes = f->pixelformat == V4L2_PIX_FMT_P010 ? 2 : 1;
    f->width = ALIGN_UP(f->width, 2);
    f->height = ALIGN_UP(f->height, 2);
    if (f->width < 320) f->width = 320;
    if (f->height < 240) f->height = 240;
    if (f->width > 4096) f->width = 4096;
    if (f->height > 2304) f->height = 2304;
    stride = f->plane_fmt[0].bytesperline;
    if (stride < f->width * bytes) stride = f->width * bytes;
    stride = ALIGN_UP(stride, 64);
    f->num_planes = 1;
    f->plane_fmt[0].bytesperline = stride;
    f->plane_fmt[0].sizeimage = stride * ALIGN_UP(f->height, 16) * 3 / 2;
}

static void fake_fill_coded(struct v4l2_pix_format_mplane *f)
{
    uint32_t min;

    if (f->pixelformat != V4L2_PIX_FMT_H264 &&
        f->pixelformat != V4L2_PIX_FMT_HEVC)
        f->pixelformat = V4L2_PIX_FMT_H264;
    min = ALIGN_UP(3 * ALIGN_UP(f->width, 32) * ALIGN_UP(f->height, 16) / 2,
                   16384) + 65536;
    f->num_planes = 1;
    f->plane_fmt[0].bytesperline = 0;
    if (f->plane_fmt[0].sizeimage < min)
        f->plane_fmt[0].sizeimage = min;
}

/* "Encode" queued OUTPUT buffers into free CAPTURE buffers, in order. */
static void fake_process(void)
{
    unsigned int o, c;

    if (!ave.out_on || !ave.cap_on)
        return;
    for (o = 0; o < ave.nout; o++) {
        struct fake_buf *in = &ave.out[o];

        if (!in->queued || in->done)
            continue;
        for (c = 0; c < ave.ncap; c++) {
            if (ave.cap[c].queued && !ave.cap[c].done)
                break;
        }
        if (c == ave.ncap)
            return;
        ave.last_stride = ave.src.plane_fmt[0].bytesperline;
        ave.last_height = ave.src.height;
        ave.last_bytesused = in->bytesused;
        ave.last_sizeimage = ave.src.plane_fmt[0].sizeimage;
        ave.last_luma0 = in->mem[0];
        /* apple-ave: chroma right after `height` luma rows. */
        ave.last_chroma0 =
            in->mem[(size_t)ave.last_stride * ave.last_height];
        {
            static const uint8_t au[] = { 0, 0, 0, 1, 0x26, 0x01, 0xaf };
            struct fake_buf *out = &ave.cap[c];

            memcpy(out->mem, au, sizeof(au));
            out->bytesused = sizeof(au);
            out->timestamp = in->timestamp;
            out->flags = ave.frames == 0 ? V4L2_BUF_FLAG_KEYFRAME
                                         : V4L2_BUF_FLAG_PFRAME;
            out->done = true;
        }
        in->done = true;
        ave.frames++;
    }
}

static int fake_ioctl(unsigned long request, void *arg)
{
    switch (request) {
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability *cap = arg;

        memset(cap, 0, sizeof(*cap));
        strcpy((char *)cap->driver, "apple-ave");
        strcpy((char *)cap->card, "Apple AVE video encoder");
        cap->capabilities = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_M2M_MPLANE |
                            V4L2_CAP_STREAMING;
        cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
        return 0;
    }
    case VIDIOC_S_FMT: {
        struct v4l2_format *f = arg;

        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            fake_fill_coded(&f->fmt.pix_mp);
            ave.dst = f->fmt.pix_mp;
        } else {
            fake_fill_raw(&f->fmt.pix_mp, ave.dst.pixelformat);
            ave.src = f->fmt.pix_mp;
            ave.dst.width = ave.src.width;
            ave.dst.height = ave.src.height;
        }
        return 0;
    }
    case VIDIOC_S_PARM:
        return 0;
    case VIDIOC_S_CTRL: {
        struct v4l2_control *ctrl = arg;

        if (ctrl->id == V4L2_CID_MPEG_VIDEO_B_FRAMES)
            ave.b_frames = ctrl->value;
        return 0;
    }
    case VIDIOC_G_CTRL: {
        struct v4l2_control *ctrl = arg;

        if (ctrl->id != V4L2_CID_MIN_BUFFERS_FOR_OUTPUT) {
            errno = EINVAL;
            return -1;
        }
        ctrl->value = 8;
        return 0;
    }
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers *r = arg;
        bool output = r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        struct fake_buf *bufs = output ? ave.out : ave.cap;
        unsigned int *n = output ? &ave.nout : &ave.ncap;
        size_t size = output ? ave.src.plane_fmt[0].sizeimage
                             : ave.dst.plane_fmt[0].sizeimage;
        unsigned int i;

        for (i = 0; i < *n; i++)
            free(bufs[i].mem);
        memset(bufs, 0, sizeof(struct fake_buf) * FAKE_BUFS);
        if (r->count > FAKE_BUFS)
            r->count = FAKE_BUFS;
        for (i = 0; i < r->count; i++) {
            bufs[i].size = size;
            bufs[i].mem = calloc(1, size);
        }
        *n = r->count;
        return 0;
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer *b = arg;
        bool output = b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        struct fake_buf *buf = output ? &ave.out[b->index] : &ave.cap[b->index];

        b->length = 1;
        b->m.planes[0].length = (uint32_t)buf->size;
        /* Offset encodes queue and index for the fake mmap. */
        b->m.planes[0].m.mem_offset =
            ((output ? 0x10000u : 0x20000u) + b->index) * 4096u;
        return 0;
    }
    case VIDIOC_QBUF: {
        struct v4l2_buffer *b = arg;
        bool output = b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        struct fake_buf *buf = output ? &ave.out[b->index] : &ave.cap[b->index];

        buf->queued = true;
        buf->done = false;
        if (output) {
            buf->timestamp = b->timestamp;
            buf->bytesused = b->m.planes[0].bytesused;
        }
        if (!lazy)
            fake_process();
        return 0;
    }
    case VIDIOC_DQBUF: {
        struct v4l2_buffer *b = arg;
        bool output = b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        struct fake_buf *bufs = output ? ave.out : ave.cap;
        unsigned int n = output ? ave.nout : ave.ncap;
        unsigned int i;

        fake_process();
        for (i = 0; i < n; i++) {
            if (bufs[i].queued && bufs[i].done) {
                bufs[i].queued = false;
                b->index = i;
                b->timestamp = bufs[i].timestamp;
                b->flags = output ? 0 : bufs[i].flags;
                b->m.planes[0].bytesused = output ? 0 : bufs[i].bytesused;
                return 0;
            }
        }
        errno = EAGAIN;
        return -1;
    }
    case VIDIOC_STREAMON: {
        enum v4l2_buf_type *type = arg;

        if (*type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
            ave.out_on = true;
        else
            ave.cap_on = true;
        if (!lazy)
            fake_process();
        return 0;
    }
    case VIDIOC_STREAMOFF:
    case VIDIOC_SUBSCRIBE_EVENT:
        return 0;
    case VIDIOC_ENCODER_CMD:
        errno = EINVAL; /* no drain emulation; close copes */
        return -1;
    default:
        errno = ENOTTY;
        return -1;
    }
}

int __real_open(const char *path, int flags, ...);
int __real_open64(const char *path, int flags, ...);
int __real___open_2(const char *path, int flags);
int __real___open64_2(const char *path, int flags);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *addr, size_t length, int prot, int flags, int fd,
                  off_t offset);
void *__real_mmap64(void *addr, size_t length, int prot, int flags, int fd,
                    off64_t offset);
int __real_munmap(void *addr, size_t length);
int __real_close(int fd);
int __real_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int __real___poll_chk(struct pollfd *fds, nfds_t nfds, int timeout,
                      size_t fdslen);

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if (path && strcmp(path, FAKE_PATH) == 0) {
        memset(&ave, 0, sizeof(ave));
        ave.open = true;
        return FAKE_FD;
    }
    if (flags & O_CREAT) {
        va_list args;

        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

int __wrap_open64(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if (path && strcmp(path, FAKE_PATH) == 0)
        return __wrap_open(path, flags);
    if (flags & O_CREAT) {
        va_list args;

        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return __real_open64(path, flags, mode);
}

int __wrap___open_2(const char *path, int flags)
{
    return __wrap_open(path, flags);
}

int __wrap___open64_2(const char *path, int flags)
{
    return __wrap_open64(path, flags);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    void *arg;

    va_start(args, request);
    arg = va_arg(args, void *);
    va_end(args);
    if (fd == FAKE_FD)
        return fake_ioctl(request, arg);
    return __real_ioctl(fd, request, arg);
}

void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd,
                  off_t offset)
{
    if (fd == FAKE_FD) {
        unsigned int key = (unsigned int)(offset / 4096);
        bool output = key < 0x20000u;
        unsigned int index = key - (output ? 0x10000u : 0x20000u);

        return output ? ave.out[index].mem : ave.cap[index].mem;
    }
    return __real_mmap(addr, length, prot, flags, fd, offset);
}

void *__wrap_mmap64(void *addr, size_t length, int prot, int flags, int fd,
                    off64_t offset)
{
    if (fd == FAKE_FD)
        return __wrap_mmap(addr, length, prot, flags, fd, (off_t)offset);
    return __real_mmap64(addr, length, prot, flags, fd, offset);
}

static bool fake_memory(const void *addr)
{
    unsigned int i;

    for (i = 0; i < FAKE_BUFS; i++) {
        if ((ave.out[i].mem && addr == ave.out[i].mem) ||
            (ave.cap[i].mem && addr == ave.cap[i].mem))
            return true;
    }
    return false;
}

int __wrap_munmap(void *addr, size_t length)
{
    if (fake_memory(addr))
        return 0; /* freed at the next REQBUFS / test end */
    return __real_munmap(addr, length);
}

int __wrap_close(int fd)
{
    if (fd == FAKE_FD) {
        ave.open = false;
        return 0;
    }
    return __real_close(fd);
}

int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (nfds == 1 && fds[0].fd == FAKE_FD) {
        fake_process();
        fds[0].revents = POLLIN | POLLOUT;
        return 1;
    }
    return __real_poll(fds, nfds, timeout);
}

int __wrap___poll_chk(struct pollfd *fds, nfds_t nfds, int timeout,
                      size_t fdslen)
{
    (void)fdslen;
    return __wrap_poll(fds, nfds, timeout);
}

/* ---- VA client (FFmpeg-shaped) ----------------------------------------- */

static struct VADriverVTable vtable;
static struct VADriverContext driver;

static void setup(void)
{
    struct venus_capabilities caps;
    void *backend = NULL;

    venus_capabilities_reset(&caps);
    venus_capabilities_add_fourcc(&caps, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_H264);
    venus_capabilities_add_fourcc(&caps, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_HEVC);
    venus_capabilities_add_raw_fourcc(&caps, VENUS_ROLE_ENCODER,
                                      V4L2_PIX_FMT_NV12);
    venus_capabilities_add_raw_fourcc(&caps, VENUS_ROLE_ENCODER,
                                      V4L2_PIX_FMT_P010);
    snprintf(caps.encoder_path, sizeof(caps.encoder_path), FAKE_PATH);
    CHECK(venus_backend_create(&caps, &backend) == VA_STATUS_SUCCESS);
    memset(&driver, 0, sizeof(driver));
    driver.vtable = &vtable;
    driver.pDriverData = backend;
    venus_vtable_init(&vtable);
    venus_backend_fill_vtable(&vtable);
}

static VABufferID make_buffer(VAContextID context, VABufferType type,
                              unsigned int size, void *data)
{
    VABufferID id = VA_INVALID_ID;

    CHECK(vtable.vaCreateBuffer(&driver, context, type, size, 1, data, &id) ==
          VA_STATUS_SUCCESS);
    return id;
}

static VABufferID make_misc(VAContextID context, VAEncMiscParameterType type,
                            const void *payload, size_t size)
{
    uint8_t bytes[sizeof(VAEncMiscParameterBuffer) + 128] = { 0 };
    VAEncMiscParameterBuffer header = { .type = type };

    memcpy(bytes, &header, sizeof(header));
    memcpy(bytes + sizeof(header), payload, size);
    return make_buffer(context, VAEncMiscParameterBufferType,
                       (unsigned int)(sizeof(header) + size), bytes);
}

/* hwupload: vaDeriveImage + vaMapBuffer, luma 0x11.., chroma 0x22.. */
static void upload(VASurfaceID surface, unsigned int height, bool p010)
{
    VAImage image;
    void *mapped = NULL;

    CHECK(vtable.vaDeriveImage(&driver, surface, &image) == VA_STATUS_SUCCESS);
    CHECK(image.format.fourcc == (p010 ? VA_FOURCC_P010 : VA_FOURCC_NV12));
    CHECK(vtable.vaMapBuffer(&driver, image.buf, &mapped) == VA_STATUS_SUCCESS);
    if (mapped) {
        memset(mapped, 0x11, (size_t)image.pitches[0] * height);
        memset((uint8_t *)mapped + image.offsets[1], 0x22,
               (size_t)image.pitches[1] * (height / 2));
    }
    CHECK(vtable.vaUnmapBuffer(&driver, image.buf) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroyImage(&driver, image.image_id) == VA_STATUS_SUCCESS);
}

enum codec { H264, HEVC, HEVC10 };

static VAStatus picture(enum codec codec, VAContextID context,
                        VASurfaceID input, VASurfaceID recon, VABufferID coded,
                        unsigned int frame, unsigned int width,
                        unsigned int coded_height, unsigned int height)
{
    VAEncMiscParameterRateControl rate = {
        .bits_per_second = 16000000, .target_percentage = 50,
    };
    VAEncMiscParameterFrameRate frame_rate = { .framerate = 30 };
    VABufferID buffers[6];
    int n = 0;
    bool idr = frame == 0;

    if (codec == H264) {
        VAEncSequenceParameterBufferH264 seq = { 0 };
        VAEncPictureParameterBufferH264 pic = { 0 };
        VAEncSliceParameterBufferH264 slice = { 0 };

        seq.level_idc = 40;
        seq.intra_idr_period = 60;
        seq.ip_period = 1;
        seq.bits_per_second = 16000000;
        seq.picture_width_in_mbs = width / 16;
        seq.picture_height_in_mbs = coded_height / 16;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        if (coded_height != height) {
            seq.frame_cropping_flag = 1;
            seq.frame_crop_bottom_offset = (coded_height - height) / 2;
        }
        pic.coded_buf = coded;
        pic.CurrPic.picture_id = recon;
        pic.pic_fields.bits.idr_pic_flag = idr;
        pic.pic_init_qp = 26;
        slice.num_macroblocks = (width / 16) * (coded_height / 16);
        slice.slice_type = idr ? 2 : 0;
        if (idr)
            buffers[n++] = make_buffer(context, VAEncSequenceParameterBufferType,
                                       sizeof(seq), &seq);
        buffers[n++] = make_buffer(context, VAEncPictureParameterBufferType,
                                   sizeof(pic), &pic);
        buffers[n++] = make_buffer(context, VAEncSliceParameterBufferType,
                                   sizeof(slice), &slice);
    } else {
        VAEncSequenceParameterBufferHEVC seq = { 0 };
        VAEncPictureParameterBufferHEVC pic = { 0 };
        VAEncSliceParameterBufferHEVC slice = { 0 };
        bool main10 = codec == HEVC10;

        seq.general_profile_idc = main10 ? 2 : 1;
        seq.general_level_idc = 120;
        seq.intra_idr_period = 60;
        seq.ip_period = 1;
        seq.bits_per_second = 16000000;
        seq.pic_width_in_luma_samples = width;
        seq.pic_height_in_luma_samples = coded_height;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.bit_depth_luma_minus8 = main10 ? 2 : 0;
        seq.seq_fields.bits.bit_depth_chroma_minus8 = main10 ? 2 : 0;
        /* FFmpeg fallback: min CB 8 in VA sequence, surface alignment 16. */
        seq.log2_min_luma_coding_block_size_minus3 = 0;
        seq.log2_diff_max_min_luma_coding_block_size = 2;
        pic.coded_buf = coded;
        pic.decoded_curr_pic.picture_id = recon;
        pic.pic_fields.bits.idr_pic_flag = idr;
        pic.pic_init_qp = 30;
        slice.num_ctu_in_slice = (width / 32) * ((coded_height + 31) / 32);
        slice.slice_type = idr ? 2 : 1;
        if (idr)
            buffers[n++] = make_buffer(context, VAEncSequenceParameterBufferType,
                                       sizeof(seq), &seq);
        buffers[n++] = make_buffer(context, VAEncPictureParameterBufferType,
                                   sizeof(pic), &pic);
        buffers[n++] = make_buffer(context, VAEncSliceParameterBufferType,
                                   sizeof(slice), &slice);
    }
    if (idr) {
        buffers[n++] = make_misc(context, VAEncMiscParameterTypeRateControl,
                                 &rate, sizeof(rate));
        buffers[n++] = make_misc(context, VAEncMiscParameterTypeFrameRate,
                                 &frame_rate, sizeof(frame_rate));
    }
    if (vtable.vaBeginPicture(&driver, context, input) != VA_STATUS_SUCCESS)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (vtable.vaRenderPicture(&driver, context, buffers, n) !=
        VA_STATUS_SUCCESS)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return vtable.vaEndPicture(&driver, context);
}

static void run(const char *name, enum codec codec, unsigned int width,
                unsigned int coded_height, unsigned int height)
{
    bool p010 = codec == HEVC10;
    unsigned int rt = p010 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    VAProfile profile = codec == H264 ? VAProfileH264High
                        : p010 ? VAProfileHEVCMain10 : VAProfileHEVCMain;
    VAConfigAttrib attributes[2] = {
        { VAConfigAttribRTFormat, rt },
        { VAConfigAttribRateControl, VA_RC_VBR },
    };
    VASurfaceAttrib format = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypeInteger,
                   .value.i = p010 ? VA_FOURCC_P010 : VA_FOURCC_NV12 },
    };
    VASurfaceID recon[3], input[3];
    VABufferID coded[3];
    VAConfigID config;
    VAContextID context;
    unsigned int frame;
    unsigned int bps = p010 ? 2 : 1;
    bool lazy_issued = false;

    fprintf(stderr, "-- %s\n", name);
    CHECK(vtable.vaCreateConfig(&driver, profile, VAEntrypointEncSlice,
                                attributes, 2, &config) == VA_STATUS_SUCCESS);
    /* FFmpeg: recon pool at the coded size, upload pool at the visible size. */
    CHECK(vtable.vaCreateSurfaces2(&driver, rt, width, coded_height, recon, 3,
                                   &format, 1) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateContext(&driver, config, (int)width,
                                 (int)coded_height, VA_PROGRESSIVE, recon, 3,
                                 &context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateSurfaces2(&driver, rt, width, height, input, 3,
                                   &format, 1) == VA_STATUS_SUCCESS);
    for (frame = 0; frame < 3; frame++)
        coded[frame] = make_buffer(context, VAEncCodedBufferType, 1 << 22,
                                   NULL);

    for (frame = 0; frame < 3; frame++) {
        VAStatus status;
        VACodedBufferSegment *segment = NULL;

        if (frame == 0) {
            unsigned int issue;

            /* Lazy: issue every frame before syncing any (async depth). */
            for (issue = frame; issue < (lazy ? 3u : frame + 1u); issue++) {
                upload(input[issue], height, p010);
                status = picture(codec, context, input[issue], recon[issue],
                                 coded[issue], issue, width, coded_height,
                                 height);
                if (status != VA_STATUS_SUCCESS)
                    fprintf(stderr, "%s frame %u: EndPicture -> %d (%s)\n",
                            name, issue, status, vaErrorStr(status));
                CHECK(status == VA_STATUS_SUCCESS);
            }
            if (lazy)
                lazy_issued = true;
        }
        if (!lazy && frame > 0) {
            upload(input[frame], height, p010);
            status = picture(codec, context, input[frame], recon[frame],
                             coded[frame], frame, width, coded_height, height);
            if (status != VA_STATUS_SUCCESS)
                fprintf(stderr, "%s frame %u: EndPicture -> %d (%s)\n", name,
                        frame, status, vaErrorStr(status));
            CHECK(status == VA_STATUS_SUCCESS);
        }
        CHECK(vtable.vaSyncSurface(&driver, input[frame]) ==
              VA_STATUS_SUCCESS);
        CHECK(vtable.vaMapBuffer(&driver, coded[frame], (void **)&segment) ==
              VA_STATUS_SUCCESS);
        CHECK(segment && segment->size == 7);
        CHECK(vtable.vaUnmapBuffer(&driver, coded[frame]) ==
              VA_STATUS_SUCCESS);
        if (lazy_issued)
            continue; /* layout already checked in eager mode */
        /* AVE layout: visible size, its stride, chroma after `height` rows. */
        CHECK(ave.last_height == height);
        CHECK(ave.last_stride == ALIGN_UP(width * bps, 64));
        CHECK(ave.last_bytesused == ave.last_sizeimage);
        CHECK(ave.last_luma0 == 0x11 && ave.last_chroma0 == 0x22);
    }
    CHECK(ave.b_frames == 0);
    CHECK(ave.src.pixelformat ==
          (p010 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12));

    CHECK(vtable.vaDestroyContext(&driver, context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroySurfaces(&driver, input, 3) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroySurfaces(&driver, recon, 3) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroyConfig(&driver, config) == VA_STATUS_SUCCESS);
    for (frame = 0; frame < FAKE_BUFS; frame++) {
        free(ave.out[frame].mem);
        free(ave.cap[frame].mem);
        ave.out[frame].mem = ave.cap[frame].mem = NULL;
    }
}

int main(void)
{
    setup();
    run("h264 1920x1080 (coded 1088)", H264, 1920, 1088, 1080);
    run("hevc main 1280x720", HEVC, 1280, 720, 720);
    run("hevc main 1920x1080 (coded 1088)", HEVC, 1920, 1088, 1080);
    run("hevc main10 p010 1920x1080 (coded 1088)", HEVC10, 1920, 1088, 1080);
    run("hevc main10 p010 1280x720", HEVC10, 1280, 720, 720);
    lazy = true;
    run("async h264 1920x1080", H264, 1920, 1088, 1080);
    run("async hevc main10 p010 1920x1080", HEVC10, 1920, 1088, 1080);
    vtable.vaTerminate(&driver);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fake apple-ave end-to-end: ok\n");
    return 0;
}
