// SPDX-License-Identifier: MIT
/*
 * Host-only unit checks for the apple-ave port: control mapping against
 * the control set apple-ave implements, VA rate-control translation, the
 * OUTPUT buffer layout, DMA-BUF import validation and the sysfs probe
 * filter.  Nothing here opens a V4L2 device.
 */
#include "encode_params.h"
#include "surface_import.h"
#include "v4l2_encoder.h"
#include "v4l2_probe.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,          \
                    __LINE__, #condition);                                  \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/*
 * The controls apple-ave registers, transcribed from
 * /etc/nixos/j713-encoder-src/driver/ave-v4l2.c ave_init_ctrls() and
 * ave_s_ctrl().  menu_mask lists allowed menu entries (bit = value).
 */
struct ave_ctrl_spec {
    uint32_t id;
    int32_t min;
    int32_t max;
    int32_t step;
    uint64_t menu_mask;
};

static const struct ave_ctrl_spec ave_ctrls[] = {
    { V4L2_CID_MPEG_VIDEO_BITRATE_MODE, 0, 2, 1,
      (1u << V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) |
          (1u << V4L2_MPEG_VIDEO_BITRATE_MODE_CBR) },
    { V4L2_CID_MPEG_VIDEO_BITRATE, 100000, 100000000, 1, 0 },
    { V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 0, 200000000, 1, 0 },
    { V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0, 65535, 1, 0 },
    { V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, 0, 0, 0 },
    { V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, 3, 3, 0 },
    { V4L2_CID_MPEG_VIDEO_HEADER_MODE, 0, 1, 1,
      1u << V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME },
    { V4L2_CID_MPEG_VIDEO_H264_PROFILE, 0, 4, 1,
      (1u << V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
          (1u << V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
          (1u << V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
          (1u << V4L2_MPEG_VIDEO_H264_PROFILE_HIGH) },
    { V4L2_CID_MPEG_VIDEO_H264_LEVEL, 0, V4L2_MPEG_VIDEO_H264_LEVEL_5_2, 1,
      (1u << (V4L2_MPEG_VIDEO_H264_LEVEL_5_2 + 1)) - 1 },
    { V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 0, 51, 1, 0 },
    { V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 0, 51, 1, 0 },
    { V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, 0, 2, 1,
      (1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN) |
          (1u << V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10) },
    { V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, 0, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1, 1,
      (1u << (V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1 + 1)) - 1 },
    { V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, 0, 51, 1, 0 },
    { V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, 0, 51, 1, 0 },
};

static const struct ave_ctrl_spec *find_spec(uint32_t id)
{
    size_t i;

    for (i = 0; i < sizeof(ave_ctrls) / sizeof(ave_ctrls[0]); i++) {
        if (ave_ctrls[i].id == id)
            return &ave_ctrls[i];
    }
    return NULL;
}

static bool value_accepted(const struct ave_ctrl_spec *spec, int32_t value)
{
    if (value < spec->min || value > spec->max)
        return false;
    if (spec->menu_mask)
        return (spec->menu_mask >> value) & 1u;
    if (spec->step > 1)
        return (value - spec->min) % spec->step == 0;
    return true;
}

static struct venus_v4l2_encoder_config base_config(bool hevc)
{
    return (struct venus_v4l2_encoder_config) {
        .device = "/nonexistent",
        .coded_format = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264,
        .raw_format = V4L2_PIX_FMT_NV12,
        .width = 1280,
        .height = 720,
        .frames_per_second = 30,
        .bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR,
        .bitrate = 2000000,
        .qp_range = true,
        .min_qp = 1,
        .max_qp = 42,
        .gop_size = 2048,
        .h264_profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
        .h264_level = V4L2_MPEG_VIDEO_H264_LEVEL_3_1,
        .hevc_profile = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
        .hevc_level = V4L2_MPEG_VIDEO_HEVC_LEVEL_4,
        .capture_buffer_size = 1 << 20,
        .output_buffers = 4,
        .capture_buffers = 8,
    };
}

static void check_controls_supported(const struct venus_v4l2_encoder_config *c)
{
    struct ave_control controls[AVE_MAX_CONTROLS];
    size_t count = 0;
    size_t i;
    bool b_frames_zero = false;

    CHECK(ave_encoder_controls(c, controls, AVE_MAX_CONTROLS, &count) == 0);
    for (i = 0; i < count; i++) {
        const struct ave_ctrl_spec *spec = find_spec(controls[i].id);

        if (!spec) {
            fprintf(stderr, "control %s (0x%x) is not an apple-ave control\n",
                    controls[i].name, controls[i].id);
            failures++;
            continue;
        }
        if (!value_accepted(spec, controls[i].value)) {
            fprintf(stderr, "%s value %d outside apple-ave range\n",
                    controls[i].name, controls[i].value);
            failures++;
        }
        if (controls[i].id == V4L2_CID_MPEG_VIDEO_B_FRAMES)
            b_frames_zero = controls[i].value == 0;
        /* The exact Venus controls that do not exist on apple-ave. */
        CHECK(controls[i].id != V4L2_CID_MPEG_VIDEO_AU_DELIMITER);
        CHECK(controls[i].id != V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE);
        CHECK(controls[i].id != V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP);
        CHECK(controls[i].id != V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP);
        CHECK(controls[i].id != V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE);
        CHECK(controls[i].id != V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM);
        CHECK(controls[i].id != V4L2_CID_MPEG_VIDEO_HEVC_TIER);
    }
    /* apple-ave defaults to 3 B-frames; VA low-delay needs 0. */
    CHECK(b_frames_zero);
}

static void test_controls(void)
{
    struct venus_v4l2_encoder_config c;
    struct ave_control controls[AVE_MAX_CONTROLS];
    size_t count;

    c = base_config(false);
    check_controls_supported(&c);
    c.bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
    c.bitrate_peak = 4000000;
    check_controls_supported(&c);
    c.h264_profile = V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
    c.qp_range = false;
    check_controls_supported(&c);

    c = base_config(true);
    check_controls_supported(&c);
    c.raw_format = V4L2_PIX_FMT_P010;
    c.hevc_profile = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10;
    c.hevc_level = V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1;
    check_controls_supported(&c);

    /* Values apple-ave would silently clamp are refused instead. */
    c = base_config(false);
    c.bitrate = 50000;
    CHECK(ave_encoder_controls(&c, controls, AVE_MAX_CONTROLS, &count) ==
          -ERANGE);
    c = base_config(false);
    c.bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_CQ;
    CHECK(ave_encoder_controls(&c, controls, AVE_MAX_CONTROLS, &count) ==
          -ENOTSUP);
    c = base_config(false);
    c.gop_size = 0;
    CHECK(ave_encoder_controls(&c, controls, AVE_MAX_CONTROLS, &count) ==
          -ERANGE);
    c = base_config(true);
    c.hevc_level = V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2;
    CHECK(ave_encoder_controls(&c, controls, AVE_MAX_CONTROLS, &count) ==
          -ENOTSUP);
    c = base_config(true);
    c.raw_format = V4L2_PIX_FMT_P010; /* Main profile with P010 input */
    CHECK(ave_encoder_controls(&c, controls, AVE_MAX_CONTROLS, &count) ==
          -EINVAL);
}

static void test_rate_mapping(void)
{
    struct ave_rate_request request = { 0 };
    struct ave_rate_settings settings;
    VAEncMiscParameterRateControl rc = { 0 };
    VAEncMiscParameterFrameRate fr = { 0 };
    VAEncMiscParameterHRD hrd = { 0 };

    rc.bits_per_second = 2000000;
    rc.target_percentage = 100;
    rc.min_qp = 1;
    rc.max_qp = 42;
    rc.rc_flags.bits.disable_frame_skip = 1;
    CHECK(ave_parse_misc(VAEncMiscParameterTypeRateControl, &rc, sizeof(rc),
                         &request) == 0);
    fr.framerate = 30;
    CHECK(ave_parse_misc(VAEncMiscParameterTypeFrameRate, &fr, sizeof(fr),
                         &request) == 0);
    CHECK(request.frames_per_second == 30);
    fr.framerate = 30000 | (1001u << 16);
    CHECK(ave_parse_misc(VAEncMiscParameterTypeFrameRate, &fr, sizeof(fr),
                         &request) == 0);
    CHECK(request.frames_per_second == 30);
    CHECK(ave_parse_misc(VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd),
                         &request) == 0);
    CHECK(ave_parse_misc(VAEncMiscParameterTypeROI, &hrd, sizeof(hrd),
                         &request) == -ENOTSUP);
    CHECK(ave_parse_misc(VAEncMiscParameterTypeMaxFrameSize, &hrd,
                         sizeof(hrd), &request) == -ENOTSUP);

    /* CBR: constant bit rate, no peak, QP clamp from VA. */
    CHECK(ave_rate_settings_from_va(VA_RC_CBR, &request, 0, &settings) == 0);
    CHECK(settings.v4l2_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
    CHECK(settings.bitrate == 2000000 && settings.peak == 0);
    CHECK(settings.qp_range && settings.min_qp == 1 && settings.max_qp == 42);

    /* VBR: VA bits_per_second is the peak, target a percentage of it. */
    request.target_percentage = 50;
    request.bits_per_second = 8000000;
    CHECK(ave_rate_settings_from_va(VA_RC_VBR, &request, 0, &settings) == 0);
    CHECK(settings.v4l2_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
    CHECK(settings.bitrate == 4000000 && settings.peak == 8000000);
    request.target_percentage = 100;
    CHECK(ave_rate_settings_from_va(VA_RC_VBR, &request, 0, &settings) == 0);
    CHECK(settings.bitrate == 8000000 && settings.peak == 0);

    CHECK(ave_rate_settings_from_va(VA_RC_CQP, &request, 0, &settings) ==
          -ENOTSUP);
    {
        struct ave_rate_request none = { 0 };

        CHECK(ave_rate_settings_from_va(VA_RC_CBR, &none, 0, &settings) ==
              -EINVAL);
        CHECK(ave_rate_settings_from_va(VA_RC_CBR, &none, 3000000,
                                        &settings) == 0);
        CHECK(settings.bitrate == 3000000 && !settings.qp_range);
    }

    rc.rc_flags.bits.disable_bit_stuffing = 1;
    CHECK(ave_parse_misc(VAEncMiscParameterTypeRateControl, &rc, sizeof(rc),
                         &request) == -ENOTSUP);
    rc.rc_flags.value = 0;
    rc.rc_flags.bits.temporal_id = 1;
    CHECK(ave_parse_misc(VAEncMiscParameterTypeRateControl, &rc, sizeof(rc),
                         &request) == -ENOTSUP);

    CHECK(ave_gop_from_va(2048, 0) == 2048);
    CHECK(ave_gop_from_va(0, 120) == 120);
    CHECK(ave_gop_from_va(0, 0) == 65535);
    CHECK(ave_gop_from_va(100000, 0) == 65535);
    CHECK(ave_gop_from_va(1, 0) == 1);
}

static void test_pack_layout(void)
{
    /* 1920x1080 NV12: AVE stride 1920, chroma at stride*1080 (not 1088). */
    const uint32_t width = 1920, height = 1080, src_stride = 2048;
    const uint32_t dst_stride = 1920;
    size_t src_size = (size_t)src_stride * 1088 * 3 / 2;
    size_t dst_size = (size_t)dst_stride * 1088 * 3 / 2;
    uint8_t *src = malloc(src_size);
    uint8_t *dst = calloc(1, dst_size);
    size_t uv_offset = (size_t)src_stride * 1088; /* padded source */
    size_t packed = 0;
    uint32_t x, y;

    CHECK(src && dst);
    if (!src || !dst)
        goto out;
    for (y = 0; y < 1088; y++)
        for (x = 0; x < src_stride; x++)
            src[(size_t)y * src_stride + x] = (uint8_t)(y * 7 + x);
    for (y = 0; y < 544; y++)
        for (x = 0; x < src_stride; x++)
            src[uv_offset + (size_t)y * src_stride + x] =
                (uint8_t)(0x80 ^ (y * 3 + x));

    CHECK(ave_pack_frame(dst, dst_size, dst_stride, src, src + uv_offset,
                         src_stride, width, height, 1, &packed) == 0);
    CHECK(packed == (size_t)dst_stride * height * 3 / 2);
    for (y = 0; y < height; y += 97)
        CHECK(memcmp(dst + (size_t)y * dst_stride,
                     src + (size_t)y * src_stride, width) == 0);
    for (y = 0; y < height / 2; y += 53)
        CHECK(memcmp(dst + (size_t)dst_stride * height +
                         (size_t)y * dst_stride,
                     src + uv_offset + (size_t)y * src_stride, width) == 0);
    /* Too small a destination is refused, not overrun. */
    CHECK(ave_pack_frame(dst, packed - 1, dst_stride, src, src + uv_offset,
                         src_stride, width, height, 1, &packed) == -ENOSPC);
    /* P010: two bytes per sample. */
    CHECK(ave_pack_frame(dst, dst_size, 1920 * 2, src, src + uv_offset,
                         src_stride, 640, 360, 2, &packed) == 0);
    CHECK(packed == (size_t)1920 * 2 * 360 * 3 / 2);
out:
    free(src);
    free(dst);
}

static void test_import_layouts(void)
{
    VASurfaceAttribExternalBuffers ext = { 0 };
    VADRMPRIMESurfaceDescriptor prime = { 0 };
    struct ave_import_layout layout;
    uintptr_t fds[1] = { 5 };

    /* Chrome legacy DRM_PRIME: one fd, two planes (vaapi_wrapper.cc). */
    ext.pixel_format = VA_FOURCC_NV12;
    ext.width = 1280;
    ext.height = 720;
    ext.num_planes = 2;
    ext.pitches[0] = ext.pitches[1] = 1280;
    ext.offsets[0] = 0;
    ext.offsets[1] = 1280 * 736;
    ext.data_size = 1280 * 736 * 3 / 2;
    ext.buffers = fds;
    ext.num_buffers = 1;
    CHECK(ave_import_layout_from_external(&ext, 1280, 720, VA_FOURCC_NV12,
                                          &layout) == 0);
    CHECK(layout.stride == 1280 && layout.uv_offset == 1280 * 736 &&
          layout.fd == 5);
    ext.flags = VA_SURFACE_EXTBUF_DESC_ENABLE_TILING;
    CHECK(ave_import_layout_from_external(&ext, 1280, 720, VA_FOURCC_NV12,
                                          &layout) == -ENOTSUP);
    ext.flags = 0;
    ext.offsets[1] = 1280 * 700; /* chroma overlapping luma */
    CHECK(ave_import_layout_from_external(&ext, 1280, 720, VA_FOURCC_NV12,
                                          &layout) == -EINVAL);
    ext.offsets[1] = 1280 * 736;
    ext.pitches[1] = 1536;
    CHECK(ave_import_layout_from_external(&ext, 1280, 720, VA_FOURCC_NV12,
                                          &layout) == -EINVAL);

    prime.fourcc = VA_FOURCC_NV12;
    prime.width = 1280;
    prime.height = 720;
    prime.num_objects = 1;
    prime.objects[0].fd = 7;
    prime.objects[0].size = 1280 * 720 * 3 / 2;
    prime.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    prime.num_layers = 2;
    prime.layers[0].drm_format = DRM_FORMAT_R8;
    prime.layers[0].num_planes = 1;
    prime.layers[0].pitch[0] = 1280;
    prime.layers[1].drm_format = DRM_FORMAT_GR88;
    prime.layers[1].num_planes = 1;
    prime.layers[1].offset[0] = 1280 * 720;
    prime.layers[1].pitch[0] = 1280;
    CHECK(ave_import_layout_from_prime2(&prime, 1280, 720, VA_FOURCC_NV12,
                                        &layout) == 0);
    prime.objects[0].drm_format_modifier = DRM_FORMAT_MOD_INVALID;
    CHECK(ave_import_layout_from_prime2(&prime, 1280, 720, VA_FOURCC_NV12,
                                        &layout) == -ENOTSUP);
    prime.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    prime.objects[0].size = 1280 * 720; /* chroma beyond the object */
    CHECK(ave_import_layout_from_prime2(&prime, 1280, 720, VA_FOURCC_NV12,
                                        &layout) == -EINVAL);
}

static void write_name(const char *root, unsigned int index, const char *name)
{
    char path[512];
    FILE *file;

    snprintf(path, sizeof(path), "%s/video%u", root, index);
    CHECK(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/video%u/name", root, index);
    file = fopen(path, "w");
    CHECK(file != NULL);
    if (file) {
        fputs(name, file);
        fclose(file);
    }
}

static void test_probe_filter(void)
{
    char root[] = "/tmp/ave-probe-test-XXXXXX";
    unsigned int indices[8];
    size_t count;

    CHECK(ave_probe_name_matches("apple-ave\n"));
    CHECK(ave_probe_name_matches("apple-ave"));
    CHECK(!ave_probe_name_matches("apple-avex\n"));
    CHECK(!ave_probe_name_matches("avd\n"));
    CHECK(!ave_probe_name_matches("apple-isp\n"));

    if (!mkdtemp(root)) {
        CHECK(!"mkdtemp");
        return;
    }
    /* This machine's layout: ISP, AVD decoder, AVE encoder. */
    write_name(root, 0, "apple-isp\n");
    write_name(root, 1, "avd\n");
    write_name(root, 2, "apple-ave\n");
    count = ave_probe_select_nodes(root, 64, indices, 8);
    CHECK(count == 1 && indices[0] == 2);
    for (unsigned int i = 0; i < 3; i++) {
        char path[512];

        snprintf(path, sizeof(path), "%s/video%u/name", root, i);
        CHECK(unlink(path) == 0);
        snprintf(path, sizeof(path), "%s/video%u", root, i);
        CHECK(rmdir(path) == 0);
    }
    CHECK(rmdir(root) == 0);
}

int main(void)
{
    test_controls();
    test_rate_mapping();
    test_pack_layout();
    test_import_layouts();
    test_probe_filter();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("apple-ave unit checks: ok\n");
    return 0;
}
