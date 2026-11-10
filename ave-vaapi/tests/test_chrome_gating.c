// SPDX-License-Identifier: MIT
/*
 * Host-only check: drive the VA vtable the way Chrome 156's
 * VASupportedProfiles::FillSupportedProfileInfos / GetRequiredAttribs /
 * AreAttribsSupported / FillProfileInfo_Locked and
 * VaapiWrapper::GetSupportedEncodeProfiles do (media/gpu/vaapi/
 * vaapi_wrapper.cc @ 156.0.8078.79), against a backend built from
 * synthetic apple-ave capabilities.  No device node is opened: the encoder
 * path points at a non-existent file.
 */
#include "backend.h"
#include "backend_internal.h"
#include "capabilities.h"
#include "va_stubs.h"

#include <linux/videodev2.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

static int failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,          \
                    __LINE__, #condition);                                  \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static struct VADriverVTable vtable;
static struct VADriverContext driver;

static void setup(void)
{
    struct venus_capabilities caps;
    void *backend = NULL;

    venus_capabilities_reset(&caps);
    /* What apple-ave's ENUM_FMT reports (ave-v4l2.c ave_coded/raw_fmts). */
    CHECK(venus_capabilities_add_fourcc(&caps, VENUS_ROLE_ENCODER,
                                        V4L2_PIX_FMT_H264));
    CHECK(venus_capabilities_add_fourcc(&caps, VENUS_ROLE_ENCODER,
                                        V4L2_PIX_FMT_HEVC));
    CHECK(venus_capabilities_add_raw_fourcc(&caps, VENUS_ROLE_ENCODER,
                                            V4L2_PIX_FMT_NV12));
    CHECK(venus_capabilities_add_raw_fourcc(&caps, VENUS_ROLE_ENCODER,
                                            V4L2_PIX_FMT_P010));
    snprintf(caps.encoder_path, sizeof(caps.encoder_path),
             "/nonexistent/ave-host-test-video");
    CHECK(venus_backend_create(&caps, &backend) == VA_STATUS_SUCCESS);

    memset(&driver, 0, sizeof(driver));
    memset(&vtable, 0, sizeof(vtable));
    driver.vtable = &vtable;
    driver.pDriverData = backend;
    driver.max_profiles = 7;
    driver.max_entrypoints = 2;
    driver.max_attributes = 8;
    venus_vtable_init(&vtable);
    venus_backend_fill_vtable(&vtable);
}

/* Chrome kSupportableEncoderProfiles (vaapi_wrapper.cc IsVAProfileSupported). */
static const VAProfile chrome_encoder_profiles[] = {
    VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High,
    VAProfileVP8Version0_3, VAProfileVP9Profile0, VAProfileAV1Profile0,
};

struct chrome_profile_info {
    bool supported;
    int min_w, min_h, max_w, max_h;
    bool yuv420;
};

/* One iteration of FillSupportedProfileInfos for (mode, profile). */
static struct chrome_profile_info chrome_fill(VAProfile profile,
                                              uint32_t rate_control)
{
    struct chrome_profile_info info = { 0 };
    VAEntrypoint entrypoints[8];
    VAConfigAttrib required[3];
    VAConfigAttrib query[3];
    VAConfigAttrib packed = { .type = VAConfigAttribEncPackedHeaders };
    VASurfaceAttrib surface_attributes[16];
    VAConfigAttrib config_attributes[16];
    unsigned int num_surface_attributes = 0;
    int num_config_attributes = 0;
    VAProfile queried_profile;
    VAEntrypoint queried_entrypoint;
    VAConfigID config;
    int num_entrypoints = 0;
    int i;
    bool has_encslice = false;
    uint32_t wanted = VA_ENC_PACKED_HEADER_SEQUENCE |
                      VA_ENC_PACKED_HEADER_PICTURE |
                      VA_ENC_PACKED_HEADER_SLICE;

    if (vtable.vaQueryConfigEntrypoints(&driver, profile, entrypoints,
                                        &num_entrypoints) !=
        VA_STATUS_SUCCESS)
        return info;
    for (i = 0; i < num_entrypoints; i++)
        has_encslice |= entrypoints[i] == VAEntrypointEncSlice;
    if (!has_encslice)
        return info;

    /* GetRequiredAttribs */
    required[0] = (VAConfigAttrib) { VAConfigAttribRTFormat,
                                     VA_RT_FORMAT_YUV420 };
    required[1] = (VAConfigAttrib) { VAConfigAttribRateControl,
                                     rate_control };
    if (vtable.vaGetConfigAttributes(&driver, profile, VAEntrypointEncSlice,
                                     &packed, 1) != VA_STATUS_SUCCESS)
        return info;
    required[2] = (VAConfigAttrib) {
        VAConfigAttribEncPackedHeaders,
        (packed.value & wanted) == wanted ? wanted
                                          : VA_ENC_PACKED_HEADER_NONE,
    };

    /* AreAttribsSupported */
    memcpy(query, required, sizeof(query));
    for (i = 0; i < 3; i++)
        query[i].value = 0;
    if (vtable.vaGetConfigAttributes(&driver, profile, VAEntrypointEncSlice,
                                     query, 3) != VA_STATUS_SUCCESS)
        return info;
    for (i = 0; i < 3; i++) {
        if (query[i].type != required[i].type ||
            (query[i].value & required[i].value) != required[i].value)
            return info;
    }

    /* FillProfileInfo_Locked */
    if (vtable.vaCreateConfig(&driver, profile, VAEntrypointEncSlice,
                              required, 3, &config) != VA_STATUS_SUCCESS)
        return info;
    if (vtable.vaQuerySurfaceAttributes(&driver, config, NULL,
                                        &num_surface_attributes) !=
            VA_STATUS_SUCCESS ||
        num_surface_attributes == 0 || num_surface_attributes > 16 ||
        vtable.vaQuerySurfaceAttributes(&driver, config, surface_attributes,
                                        &num_surface_attributes) !=
            VA_STATUS_SUCCESS) {
        vtable.vaDestroyConfig(&driver, config);
        return info;
    }
    for (i = 0; i < (int)num_surface_attributes; i++) {
        int value = surface_attributes[i].value.value.i;

        switch (surface_attributes[i].type) {
        case VASurfaceAttribMaxWidth: info.max_w = value; break;
        case VASurfaceAttribMaxHeight: info.max_h = value; break;
        case VASurfaceAttribMinWidth: info.min_w = value; break;
        case VASurfaceAttribMinHeight: info.min_h = value; break;
        default: break;
        }
    }
    vtable.vaDestroyConfig(&driver, config);
    if (!info.max_w || !info.max_h)
        return info;
    /* min_resolution.SetToMax(16x16) then QVGA+1 for encoders. */
    if (info.min_w < 321) info.min_w = 321;
    if (info.min_h < 241) info.min_h = 241;

    if (vtable.vaCreateConfig(&driver, profile, VAEntrypointEncSlice, NULL,
                              0, &config) != VA_STATUS_SUCCESS)
        return info;
    if (vtable.vaQueryConfigAttributes(&driver, config, &queried_profile,
                                       &queried_entrypoint,
                                       config_attributes,
                                       &num_config_attributes) ==
        VA_STATUS_SUCCESS) {
        for (i = 0; i < num_config_attributes; i++) {
            if (config_attributes[i].type == VAConfigAttribRTFormat)
                info.yuv420 =
                    (config_attributes[i].value & VA_RT_FORMAT_YUV420) != 0;
        }
    }
    vtable.vaDestroyConfig(&driver, config);
    info.supported = info.yuv420;
    return info;
}

static bool driver_lists(VAProfile profile)
{
    VAProfile profiles[16];
    int count = 0;
    int i;

    CHECK(vtable.vaQueryConfigProfiles(&driver, profiles, &count) ==
          VA_STATUS_SUCCESS);
    for (i = 0; i < count; i++) {
        if (profiles[i] == profile)
            return true;
    }
    return false;
}

static void test_profile_gating(void)
{
    size_t i;

    for (i = 0; i < sizeof(chrome_encoder_profiles) /
                        sizeof(chrome_encoder_profiles[0]);
         i++) {
        VAProfile profile = chrome_encoder_profiles[i];
        bool h264 = profile == VAProfileH264ConstrainedBaseline ||
                    profile == VAProfileH264Main ||
                    profile == VAProfileH264High;
        struct chrome_profile_info cbr;
        struct chrome_profile_info vbr;
        struct chrome_profile_info cqp;

        if (!driver_lists(profile)) {
            CHECK(!h264);
            continue;
        }
        cbr = chrome_fill(profile, VA_RC_CBR);
        vbr = chrome_fill(profile, VA_RC_VBR);
        cqp = chrome_fill(profile, VA_RC_CQP);
        /* kEncodeConstantBitrate drives GetSupportedEncodeProfiles. */
        CHECK(cbr.supported == h264);
        /* Driver-side VBR works; Chrome blocks it without ChromeOSHWVBREncoding. */
        CHECK(vbr.supported == h264);
        /* AVE has no constant-QP mode: must not be advertised. */
        CHECK(!cqp.supported);
        if (h264) {
            CHECK(cbr.max_w == 4096 && cbr.max_h == 2304);
            CHECK(cbr.min_w == 321 && cbr.min_h == 241);
        }
    }

    /* HEVC is listed for FFmpeg-style clients, but Chrome never asks. */
    CHECK(driver_lists(VAProfileHEVCMain));
    CHECK(driver_lists(VAProfileHEVCMain10));
}

/*
 * blink IsAcceleratedConfigurationSupported on Linux: kVariable maps to
 * kVariableMode only (the CBR fallback is Android/ChromeOS-only).
 */
static void test_webcodecs_decision(void)
{
    struct chrome_profile_info cbr = chrome_fill(VAProfileH264Main, VA_RC_CBR);
    bool vbr_feature_enabled;

    CHECK(cbr.supported);
    for (vbr_feature_enabled = false;; vbr_feature_enabled = true) {
        /* IsBlockedDriver: VBR mode needs kChromeOSHWVBREncoding (off on ARM). */
        bool vbr_profile =
            vbr_feature_enabled &&
            chrome_fill(VAProfileH264Main, VA_RC_VBR).supported;
        bool constant_ok = cbr.supported;
        bool variable_ok = vbr_profile;

        CHECK(constant_ok);
        CHECK(variable_ok == vbr_feature_enabled);
        if (vbr_feature_enabled)
            break;
    }
}

static VABufferID make_buffer(VAContextID context, VABufferType type,
                              unsigned int size, void *data)
{
    VABufferID id = VA_INVALID_ID;

    CHECK(vtable.vaCreateBuffer(&driver, context, type, size, 1, data, &id) ==
          VA_STATUS_SUCCESS);
    return id;
}

/* Chrome's AllocateMiscParameterBuffer layout: header type + payload. */
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

/* A Chrome-shaped 1280x720 IDR picture (h264_vaapi_video_encoder_delegate.cc). */
static VAStatus submit_h264(VAContextID context, VAConfigID config,
                            VASurfaceID surface, VABufferID coded,
                            bool packed, int slice_type, int slices)
{
    VAEncSequenceParameterBufferH264 sequence = { 0 };
    VAEncPictureParameterBufferH264 picture = { 0 };
    VAEncSliceParameterBufferH264 slice = { 0 };
    VAEncMiscParameterRateControl rate = { 0 };
    VAEncMiscParameterFrameRate frame_rate = { 0 };
    VAEncMiscParameterHRD hrd = { 0 };
    VABufferID buffers[10];
    int count = 0;
    int i;

    (void)config;
    sequence.level_idc = 31;
    sequence.intra_period = 0;
    sequence.intra_idr_period = 2048;
    sequence.ip_period = 1;
    sequence.bits_per_second = 2000000;
    sequence.picture_width_in_mbs = 80;
    sequence.picture_height_in_mbs = 45;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.coded_buf = coded;
    picture.pic_fields.bits.idr_pic_flag = 1;
    picture.pic_fields.bits.entropy_coding_mode_flag = 1;
    picture.pic_init_qp = 26;
    slice.num_macroblocks = 80 * 45;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = slice_type;
    rate.bits_per_second = 2000000;
    rate.target_percentage = 100;
    rate.min_qp = 1;
    rate.max_qp = 42;
    rate.rc_flags.bits.disable_frame_skip = 1;
    frame_rate.framerate = 30;
    hrd.buffer_size = 3000000;

    buffers[count++] = make_buffer(context, VAEncSequenceParameterBufferType,
                                   sizeof(sequence), &sequence);
    buffers[count++] = make_buffer(context, VAEncPictureParameterBufferType,
                                   sizeof(picture), &picture);
    for (i = 0; i < slices; i++)
        buffers[count++] = make_buffer(context, VAEncSliceParameterBufferType,
                                       sizeof(slice), &slice);
    buffers[count++] = make_misc(context, VAEncMiscParameterTypeRateControl,
                                 &rate, sizeof(rate));
    buffers[count++] = make_misc(context, VAEncMiscParameterTypeFrameRate,
                                 &frame_rate, sizeof(frame_rate));
    buffers[count++] = make_misc(context, VAEncMiscParameterTypeHRD,
                                 &hrd, sizeof(hrd));
    if (packed) {
        VAEncPackedHeaderParameterBuffer header = {
            .type = VAEncPackedHeaderSequence,
            .bit_length = 32,
        };
        uint8_t bytes[4] = { 0, 0, 0, 1 };

        buffers[count++] = make_buffer(context,
                                       VAEncPackedHeaderParameterBufferType,
                                       sizeof(header), &header);
        buffers[count++] = make_buffer(context,
                                       VAEncPackedHeaderDataBufferType,
                                       sizeof(bytes), bytes);
    }

    CHECK(vtable.vaBeginPicture(&driver, context, surface) ==
          VA_STATUS_SUCCESS);
    CHECK(vtable.vaRenderPicture(&driver, context, buffers, count) ==
          VA_STATUS_SUCCESS);
    return vtable.vaEndPicture(&driver, context);
}

static void test_encode_validation(void)
{
    VAConfigAttrib attributes[3] = {
        { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 },
        { VAConfigAttribRateControl, VA_RC_CBR },
        { VAConfigAttribEncPackedHeaders, VA_ENC_PACKED_HEADER_NONE },
    };
    VAConfigAttrib bad_packed[2] = {
        { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 },
        { VAConfigAttribEncPackedHeaders, VA_ENC_PACKED_HEADER_SEQUENCE },
    };
    VAConfigAttrib cqp[1] = { { VAConfigAttribRateControl, VA_RC_CQP } };
    VAConfigID config;
    VAConfigID rejected;
    VAContextID context;
    VASurfaceID surface;
    VABufferID coded;

    CHECK(vtable.vaCreateConfig(&driver, VAProfileH264Main,
                                VAEntrypointEncSlice, bad_packed, 2,
                                &rejected) ==
          VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
    CHECK(vtable.vaCreateConfig(&driver, VAProfileH264Main,
                                VAEntrypointEncSlice, cqp, 1, &rejected) ==
          VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
    CHECK(vtable.vaCreateConfig(&driver, VAProfileH264Main,
                                VAEntrypointEncSlice, attributes, 3,
                                &config) == VA_STATUS_SUCCESS);
    /* AVE minimum is 320x240: smaller contexts are refused up front. */
    CHECK(vtable.vaCreateContext(&driver, config, 160, 128, VA_PROGRESSIVE,
                                 NULL, 0, &context) ==
          VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    CHECK(vtable.vaCreateContext(&driver, config, 1280, 720, VA_PROGRESSIVE,
                                 NULL, 0, &context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateSurfaces2(&driver, VA_RT_FORMAT_YUV420, 1280, 720,
                                   &surface, 1, NULL, 0) ==
          VA_STATUS_SUCCESS);
    coded = make_buffer(context, VAEncCodedBufferType, 1 << 20, NULL);

    /* Packed headers outside the negotiated config: refused before open. */
    CHECK(submit_h264(context, config, surface, coded, true, 2, 1) ==
          VA_STATUS_ERROR_UNIMPLEMENTED);
    /* B slices and multi-slice pictures are not supported. */
    CHECK(submit_h264(context, config, surface, coded, false, 1, 1) ==
          VA_STATUS_ERROR_UNIMPLEMENTED);
    CHECK(submit_h264(context, config, surface, coded, false, 2, 2) ==
          VA_STATUS_ERROR_UNIMPLEMENTED);
    /*
     * A valid Chrome-shaped picture passes every check and fails only at
     * opening the (non-existent) device: ENOENT -> ENCODING_ERROR.
     */
    CHECK(submit_h264(context, config, surface, coded, false, 2, 1) ==
          VA_STATUS_ERROR_ENCODING_ERROR);

    CHECK(vtable.vaDestroyContext(&driver, context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroySurfaces(&driver, &surface, 1) ==
          VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroyConfig(&driver, config) == VA_STATUS_SUCCESS);
}

/*
 * Chrome imports WebCodecs frames as legacy DRM_PRIME NV12 buffers
 * (VaapiWrapper::CreateVASurfaceForPixmap for VAImplementation::kOther).
 * A memfd stands in for the DMA-BUF: only mapping and bookkeeping run here.
 */
static void test_import(void)
{
    const unsigned int width = 1280, height = 720, pitch = 1280;
    const size_t size = (size_t)pitch * 736 * 3 / 2;
    VASurfaceAttribExternalBuffers ext = { 0 };
    VASurfaceAttrib attributes[2];
    uintptr_t fds[1];
    VASurfaceID surface = VA_INVALID_ID;
    VAImage image;
    int fd = memfd_create("ave-import-test", MFD_CLOEXEC);

    CHECK(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
    fds[0] = (uintptr_t)fd;
    ext.pixel_format = VA_FOURCC_NV12;
    ext.width = width;
    ext.height = height;
    ext.data_size = (uint32_t)size;
    ext.num_planes = 2;
    ext.pitches[0] = ext.pitches[1] = pitch;
    ext.offsets[1] = pitch * 736;
    ext.buffers = fds;
    ext.num_buffers = 1;
    memset(attributes, 0, sizeof(attributes));
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &ext;

    CHECK(vtable.vaCreateSurfaces2(&driver, VA_RT_FORMAT_YUV420, width,
                                   height, &surface, 1, attributes, 2) ==
          VA_STATUS_SUCCESS);
    /* The caller may close its fd; the driver holds its own reference. */
    close(fd);
    /* Read-only import: no derived (writable) image. */
    CHECK(vtable.vaDeriveImage(&driver, surface, &image) ==
          VA_STATUS_ERROR_OPERATION_FAILED);
    CHECK(vtable.vaDestroySurfaces(&driver, &surface, 1) ==
          VA_STATUS_SUCCESS);

    /* Export was never implementable here and must say so. */
    {
        VASurfaceID plain;
        VADRMPRIMESurfaceDescriptor prime;

        CHECK(vtable.vaCreateSurfaces2(&driver, VA_RT_FORMAT_YUV420, width,
                                       height, &plain, 1, NULL, 0) ==
              VA_STATUS_SUCCESS);
        CHECK(vtable.vaExportSurfaceHandle(
                  &driver, plain, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                  VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime) ==
              VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
        CHECK(vtable.vaDestroySurfaces(&driver, &plain, 1) ==
              VA_STATUS_SUCCESS);
    }
}

/* FFmpeg-shaped HEVC IDR: 1920x1088 coded, 1920x1080 input. */
static VAStatus submit_hevc_profile(VAContextID context, VASurfaceID surface,
                                    VABufferID coded, bool high_tier,
                                    bool main10)
{
    VAEncSequenceParameterBufferHEVC sequence = { 0 };
    VAEncPictureParameterBufferHEVC picture = { 0 };
    VAEncSliceParameterBufferHEVC slice = { 0 };
    VAEncMiscParameterRateControl rate = { 0 };
    VABufferID buffers[4];

    sequence.general_profile_idc = main10 ? 2 : 1;
    sequence.seq_fields.bits.bit_depth_luma_minus8 = main10 ? 2 : 0;
    sequence.seq_fields.bits.bit_depth_chroma_minus8 = main10 ? 2 : 0;
    sequence.general_level_idc = 123;
    sequence.general_tier_flag = high_tier;
    sequence.intra_idr_period = 120;
    sequence.ip_period = 1;
    sequence.bits_per_second = 3000000;
    sequence.pic_width_in_luma_samples = 1920;
    sequence.pic_height_in_luma_samples = 1088;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    /* FFmpeg's fallback reports min CB 8 here but aligns surfaces to 16. */
    sequence.log2_min_luma_coding_block_size_minus3 = 0; /* 8 */
    sequence.log2_diff_max_min_luma_coding_block_size = 2; /* CTU 32 */
    picture.coded_buf = coded;
    picture.pic_fields.bits.idr_pic_flag = 1;
    picture.pic_init_qp = 30;
    slice.num_ctu_in_slice = 60 * 34;
    slice.slice_type = 2;
    rate.bits_per_second = 3000000;
    rate.target_percentage = 100;

    buffers[0] = make_buffer(context, VAEncSequenceParameterBufferType,
                             sizeof(sequence), &sequence);
    buffers[1] = make_buffer(context, VAEncPictureParameterBufferType,
                             sizeof(picture), &picture);
    buffers[2] = make_buffer(context, VAEncSliceParameterBufferType,
                             sizeof(slice), &slice);
    buffers[3] = make_misc(context, VAEncMiscParameterTypeRateControl,
                           &rate, sizeof(rate));
    CHECK(vtable.vaBeginPicture(&driver, context, surface) ==
          VA_STATUS_SUCCESS);
    CHECK(vtable.vaRenderPicture(&driver, context, buffers, 4) ==
          VA_STATUS_SUCCESS);
    return vtable.vaEndPicture(&driver, context);
}

static VAStatus submit_hevc(VAContextID context, VASurfaceID surface,
                            VABufferID coded, bool high_tier)
{
    return submit_hevc_profile(context, surface, coded, high_tier, false);
}

/*
 * FFmpeg hevc_vaapi -profile:v main10 with "format=p010,hwupload":
 * recon pool 1920x1088 YUV420_10 passed to vaCreateContext, hwupload pool
 * 1920x1080 YUV420_10 + PixelFormat P010, upload through vaDeriveImage +
 * vaMapBuffer, then Begin/Render/EndPicture on the upload surface.
 */
static void test_main10_ffmpeg_sequence(void)
{
    VAConfigAttrib attributes[2] = {
        { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420_10 },
        { VAConfigAttribRateControl, VA_RC_VBR },
    };
    VASurfaceAttrib p010 = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypeInteger,
                   .value.i = VA_FOURCC_P010 },
    };
    VASurfaceID recon[2];
    VASurfaceID input;
    VAConfigID config;
    VAContextID context;
    VABufferID coded;
    VAImage image;
    void *mapped = NULL;
    VAStatus status;

    CHECK(vtable.vaCreateConfig(&driver, VAProfileHEVCMain10,
                                VAEntrypointEncSlice, attributes, 2,
                                &config) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateSurfaces2(&driver, VA_RT_FORMAT_YUV420_10, 1920,
                                   1088, recon, 2, &p010, 1) ==
          VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateContext(&driver, config, 1920, 1088, VA_PROGRESSIVE,
                                 recon, 2, &context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateSurfaces2(&driver, VA_RT_FORMAT_YUV420_10, 1920,
                                   1080, &input, 1, &p010, 1) ==
          VA_STATUS_SUCCESS);
    CHECK(vtable.vaDeriveImage(&driver, input, &image) == VA_STATUS_SUCCESS);
    CHECK(image.format.fourcc == VA_FOURCC_P010);
    CHECK(vtable.vaMapBuffer(&driver, image.buf, &mapped) ==
          VA_STATUS_SUCCESS);
    if (mapped)
        memset(mapped, 0x40, (size_t)image.pitches[0] * 1080);
    CHECK(vtable.vaUnmapBuffer(&driver, image.buf) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroyImage(&driver, image.image_id) ==
          VA_STATUS_SUCCESS);
    coded = make_buffer(context, VAEncCodedBufferType, 1 << 21, NULL);
    status = submit_hevc_profile(context, input, coded, false, true);
    if (status != VA_STATUS_ERROR_ENCODING_ERROR)
        fprintf(stderr, "main10 EndPicture returned %d\n", status);
    /* Must reach the (absent) device, like Main does. */
    CHECK(status == VA_STATUS_ERROR_ENCODING_ERROR);
    CHECK(vtable.vaDestroyContext(&driver, context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroySurfaces(&driver, &input, 1) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroySurfaces(&driver, recon, 2) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroyConfig(&driver, config) == VA_STATUS_SUCCESS);
}

static void test_hevc_validation(void)
{
    VAConfigAttrib attributes[2] = {
        { VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420 },
        { VAConfigAttribRateControl, VA_RC_CBR },
    };
    VAConfigID config;
    VAContextID context;
    VASurfaceID surface;
    VABufferID coded;

    CHECK(vtable.vaCreateConfig(&driver, VAProfileHEVCMain,
                                VAEntrypointEncSlice, attributes, 2,
                                &config) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateContext(&driver, config, 1920, 1088, VA_PROGRESSIVE,
                                 NULL, 0, &context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaCreateSurfaces2(&driver, VA_RT_FORMAT_YUV420, 1920, 1080,
                                   &surface, 1, NULL, 0) ==
          VA_STATUS_SUCCESS);
    coded = make_buffer(context, VAEncCodedBufferType, 1 << 20, NULL);
    /* No tier control on apple-ave: High tier is refused. */
    CHECK(submit_hevc(context, surface, coded, true) ==
          VA_STATUS_ERROR_UNIMPLEMENTED);
    /* Valid: reaches the (absent) device. */
    CHECK(submit_hevc(context, surface, coded, false) ==
          VA_STATUS_ERROR_ENCODING_ERROR);
    CHECK(vtable.vaDestroyContext(&driver, context) == VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroySurfaces(&driver, &surface, 1) ==
          VA_STATUS_SUCCESS);
    CHECK(vtable.vaDestroyConfig(&driver, config) == VA_STATUS_SUCCESS);
}

int main(void)
{
    setup();
    test_profile_gating();
    test_webcodecs_decision();
    test_encode_validation();
    test_import();
    test_hevc_validation();
    test_main10_ffmpeg_sequence();
    vtable.vaTerminate(&driver);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("chrome gating simulation: ok\n");
    return 0;
}
