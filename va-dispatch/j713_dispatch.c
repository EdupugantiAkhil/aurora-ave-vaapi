// SPDX-License-Identifier: MIT
/*
 * j713_drv_video: a VA-API driver that exposes decode and encode on the M4 Air
 * through one VADisplay, by loading two VA drivers and routing every call:
 *
 *   decode (VAEntrypointVLD, video processing) -> libva-v4l2-request (AVD)
 *   encode (VAEntrypointEnc*)                  -> apple-ave VA driver (AVE)
 *
 * Chrome (and libva) load exactly one VA driver per process, so without this
 * enabling the encoder driver loses hardware decode.
 *
 * Object IDs are NOT translated.  The encoder driver allocates every VA object
 * ID with bit 31 set; libva-v4l2-request allocates IDs below 0x20000000.  An
 * ID therefore identifies its driver, and IDs embedded in parameter buffers
 * (reference frames, coded buffers) pass through unchanged.  Calls that take
 * an object are routed by that object's ID.  Calls that create an object are
 * routed by what it is for:
 *   - configs: by entrypoint;
 *   - contexts: by their config (all render targets must belong to the same
 *     driver);
 *   - buffers: by their context;
 *   - surfaces (vaCreateSurfaces2): by VASurfaceAttribUsageHint (ENCODER ->
 *     encoder; DECODER/VPP/GENERIC -> decoder).  Imported DMA-BUFs without a
 *     usage hint go to the encoder: in Chrome only the encoder imports
 *     pixmaps (VaapiVideoEncodeAccelerator input).  Anything else goes to
 *     the decoder (legacy vaCreateSurfaces, unhinted CPU surfaces).
 *   - images (vaCreateImage) go to the decoder; vaDeriveImage follows the
 *     surface.
 *
 * Inner driver paths: compile-time J713_DEC_DRIVER / J713_ENC_DRIVER,
 * overridable with J713_VA_DEC_DRIVER / J713_VA_ENC_DRIVER.  If one inner
 * driver fails to initialise, the other is still exposed.
 * J713_VA_DISPATCH_TRACE=1 logs routing decisions to stderr.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_backend_prot.h>
#include <va/va_drmcommon.h>

#ifndef J713_DEC_DRIVER
#define J713_DEC_DRIVER "v4l2_request_drv_video.so"
#endif
#ifndef J713_ENC_DRIVER
#define J713_ENC_DRIVER "ave_drv_video.so"
#endif

#define J713_ENC_ID_BIT 0x80000000u
/* Both inner drivers and this one are built against libva 2.24 (VA 1.24). */
#define J713_INIT_SYMBOL "__vaDriverInit_1_24"

enum { DEC = 0, ENC = 1, NUM_INNER = 2 };

struct inner {
    bool ok;
    void *handle;
    struct VADriverContext ctx;
    struct VADriverVTable vt;
    struct VADriverVTableVPP vt_vpp;
    struct VADriverVTableProt vt_prot;
};

struct dispatch {
    struct inner in[NUM_INNER];
    char vendor[256];
};

static bool trace;

static void tracef(const char *fmt, ...)
{
    va_list ap;
    if (!trace)
        return;
    va_start(ap, fmt);
    fputs("j713-va: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static struct dispatch *D(VADriverContextP ctx)
{
    return ctx->pDriverData;
}

static int owner_of(VAGenericID id)
{
    return (id & J713_ENC_ID_BIT) ? ENC : DEC;
}

static bool is_encode_entrypoint(VAEntrypoint e)
{
    return e == VAEntrypointEncSlice || e == VAEntrypointEncPicture ||
           e == VAEntrypointEncSliceLP || e == VAEntrypointFEI ||
           e == VAEntrypointStats;
}

static int owner_of_entrypoint(VAEntrypoint e)
{
    return is_encode_entrypoint(e) ? ENC : DEC;
}

/* Call fn on inner driver i; INVALID_* if that driver is absent. */
#define CALL(d, i, err, fn, ...)                                              \
    do {                                                                      \
        struct inner *in_ = &(d)->in[(i)];                                    \
        if (!in_->ok)                                                         \
            return (err);                                                     \
        if (!in_->vt.fn)                                                      \
            return VA_STATUS_ERROR_UNIMPLEMENTED;                             \
        return in_->vt.fn(&in_->ctx, ##__VA_ARGS__);                          \
    } while (0)

#define BY_ID(fn, err, id, ...)                                               \
    CALL(D(ctx), owner_of(id), err, fn, ##__VA_ARGS__)

/* ---- configs ---------------------------------------------------------- */

static VAStatus j_terminate(VADriverContextP ctx)
{
    struct dispatch *d = D(ctx);
    for (int i = 0; i < NUM_INNER; i++) {
        struct inner *in = &d->in[i];
        if (in->ok && in->vt.vaTerminate)
            in->vt.vaTerminate(&in->ctx);
        if (in->handle)
            dlclose(in->handle);
    }
    free(d);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

static bool inner_has_entrypoint(struct inner *in, VAProfile p, bool encode)
{
    int n = 0;
    bool found = false;
    VAEntrypoint *e;
    if (!in->ok || !in->vt.vaQueryConfigEntrypoints)
        return false;
    e = calloc(in->ctx.max_entrypoints > 0 ? in->ctx.max_entrypoints : 1,
               sizeof(*e));
    if (!e)
        return false;
    if (in->vt.vaQueryConfigEntrypoints(&in->ctx, p, e, &n) ==
        VA_STATUS_SUCCESS)
        for (int k = 0; k < n && !found; k++)
            found = is_encode_entrypoint(e[k]) == encode;
    free(e);
    return found;
}

static VAStatus j_query_config_profiles(VADriverContextP ctx,
                                        VAProfile *profiles, int *num)
{
    struct dispatch *d = D(ctx);
    int out = 0;
    for (int i = 0; i < NUM_INNER; i++) {
        struct inner *in = &d->in[i];
        int n = 0;
        VAProfile *list;
        if (!in->ok || !in->vt.vaQueryConfigProfiles)
            continue;
        list = calloc(in->ctx.max_profiles > 0 ? in->ctx.max_profiles : 1,
                      sizeof(*list));
        if (!list)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        if (in->vt.vaQueryConfigProfiles(&in->ctx, list, &n) !=
            VA_STATUS_SUCCESS)
            n = 0;
        for (int k = 0; k < n; k++) {
            VAProfile p = list[k];
            bool dup = false;
            /* Only advertise what this driver is routed for. */
            if (!inner_has_entrypoint(in, p, i == ENC))
                continue;
            for (int j = 0; j < out; j++)
                dup |= profiles[j] == p;
            if (!dup && out < ctx->max_profiles)
                profiles[out++] = p;
        }
        free(list);
    }
    *num = out;
    return VA_STATUS_SUCCESS;
}

static VAStatus j_query_config_entrypoints(VADriverContextP ctx,
                                           VAProfile profile,
                                           VAEntrypoint *entrypoints,
                                           int *num)
{
    struct dispatch *d = D(ctx);
    int out = 0;
    bool any = false;
    for (int i = 0; i < NUM_INNER; i++) {
        struct inner *in = &d->in[i];
        int n = 0;
        VAEntrypoint *list;
        if (!in->ok || !in->vt.vaQueryConfigEntrypoints)
            continue;
        list = calloc(in->ctx.max_entrypoints > 0 ? in->ctx.max_entrypoints
                                                  : 1, sizeof(*list));
        if (!list)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        if (in->vt.vaQueryConfigEntrypoints(&in->ctx, profile, list, &n) ==
            VA_STATUS_SUCCESS) {
            any = true;
            for (int k = 0; k < n; k++) {
                VAEntrypoint e = list[k];
                if (owner_of_entrypoint(e) != i ||
                    out >= ctx->max_entrypoints)
                    continue;
                entrypoints[out++] = e;
            }
        }
        free(list);
    }
    *num = out;
    return any ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
}

static VAStatus j_get_config_attributes(VADriverContextP ctx,
                                        VAProfile profile,
                                        VAEntrypoint entrypoint,
                                        VAConfigAttrib *attribs, int num)
{
    CALL(D(ctx), owner_of_entrypoint(entrypoint),
         VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, vaGetConfigAttributes,
         profile, entrypoint, attribs, num);
}

static atomic_int last_config_owner = DEC;

static VAStatus j_create_config(VADriverContextP ctx, VAProfile profile,
                                VAEntrypoint entrypoint,
                                VAConfigAttrib *attribs, int num,
                                VAConfigID *config)
{
    int i = owner_of_entrypoint(entrypoint);
    struct inner *in = &D(ctx)->in[i];
    VAStatus s;
    if (!in->ok)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    s = in->vt.vaCreateConfig(&in->ctx, profile, entrypoint, attribs, num,
                              config);
    if (s == VA_STATUS_SUCCESS) {
        if (owner_of(*config) != i) {
            /* ID-range contract broken: refuse rather than misroute. */
            in->vt.vaDestroyConfig(&in->ctx, *config);
            *config = VA_INVALID_ID;
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        atomic_store(&last_config_owner, i);
    }
    tracef("CreateConfig profile=%d entry=%d -> %s id=%#x status=%d", profile,
           entrypoint, i == ENC ? "enc" : "dec",
           s == VA_STATUS_SUCCESS ? *config : 0, s);
    return s;
}

static VAStatus j_destroy_config(VADriverContextP ctx, VAConfigID config)
{
    BY_ID(vaDestroyConfig, VA_STATUS_ERROR_INVALID_CONFIG, config, config);
}

static VAStatus j_query_config_attributes(VADriverContextP ctx,
                                          VAConfigID config,
                                          VAProfile *profile,
                                          VAEntrypoint *entrypoint,
                                          VAConfigAttrib *attribs, int *num)
{
    BY_ID(vaQueryConfigAttributes, VA_STATUS_ERROR_INVALID_CONFIG, config,
          config, profile, entrypoint, attribs, num);
}

static VAStatus j_query_surface_attributes(VADriverContextP ctx,
                                           VAConfigID config,
                                           VASurfaceAttrib *attribs,
                                           unsigned int *num)
{
    BY_ID(vaQuerySurfaceAttributes, VA_STATUS_ERROR_INVALID_CONFIG, config,
          config, attribs, num);
}

static VAStatus j_get_surface_attributes(VADriverContextP ctx,
                                         VAConfigID config,
                                         VASurfaceAttrib *attribs,
                                         unsigned int num)
{
    BY_ID(vaGetSurfaceAttributes, VA_STATUS_ERROR_INVALID_CONFIG, config,
          config, attribs, num);
}

static VAStatus j_query_processing_rate(VADriverContextP ctx,
                                        VAConfigID config,
                                        VAProcessingRateParameter *p,
                                        unsigned int *rate)
{
    BY_ID(vaQueryProcessingRate, VA_STATUS_ERROR_INVALID_CONFIG, config,
          config, p, rate);
}

/* ---- surfaces --------------------------------------------------------- */

static int surface_owner_for_attribs(const VASurfaceAttrib *a, unsigned int n,
                                     const char **why)
{
    bool imported = false;
    for (unsigned int k = 0; a && k < n; k++) {
        if (!(a[k].flags & VA_SURFACE_ATTRIB_SETTABLE))
            continue;
        if (a[k].type == VASurfaceAttribUsageHint &&
            a[k].value.type == VAGenericValueTypeInteger) {
            int hint = a[k].value.value.i;
            if (hint & VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER) {
                *why = "hint=encoder";
                return ENC;
            }
            *why = "hint=decoder/vpp/generic";
            return DEC;
        }
        if (a[k].type == VASurfaceAttribMemoryType &&
            a[k].value.type == VAGenericValueTypeInteger &&
            (a[k].value.value.i & (VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 |
                                   VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM)))
            imported = true;
    }
    if (imported) {
        *why = "imported dmabuf, no hint";
        return ENC;
    }
    *why = "default";
    return DEC;
}

static VAStatus j_create_surfaces2(VADriverContextP ctx, unsigned int format,
                                   unsigned int width, unsigned int height,
                                   VASurfaceID *surfaces, unsigned int num,
                                   VASurfaceAttrib *attribs,
                                   unsigned int num_attribs)
{
    struct dispatch *d = D(ctx);
    const char *why = "";
    int i = surface_owner_for_attribs(attribs, num_attribs, &why);
    struct inner *in = &d->in[i];
    VAStatus s;
    if (!in->ok || !in->vt.vaCreateSurfaces2)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    s = in->vt.vaCreateSurfaces2(&in->ctx, format, width, height, surfaces,
                                 num, attribs, num_attribs);
    tracef("CreateSurfaces2 %ux%u fmt=%#x n=%u (%s) -> %s first=%#x "
           "status=%d", width, height, format, num, why,
           i == ENC ? "enc" : "dec",
           s == VA_STATUS_SUCCESS && num ? surfaces[0] : 0, s);
    return s;
}

static VAStatus j_create_surfaces(VADriverContextP ctx, int width, int height,
                                  int format, int num, VASurfaceID *surfaces)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT,
         vaCreateSurfaces, width, height, format, num, surfaces);
}

static VAStatus j_destroy_surfaces(VADriverContextP ctx, VASurfaceID *list,
                                   int num)
{
    struct dispatch *d = D(ctx);
    VAStatus result = VA_STATUS_SUCCESS;
    VASurfaceID *part = malloc(sizeof(*part) * (num > 0 ? num : 1));
    if (!part)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    for (int i = 0; i < NUM_INNER; i++) {
        int n = 0;
        for (int k = 0; k < num; k++)
            if (owner_of(list[k]) == i)
                part[n++] = list[k];
        if (!n)
            continue;
        if (!d->in[i].ok) {
            result = VA_STATUS_ERROR_INVALID_SURFACE;
            continue;
        }
        VAStatus s = d->in[i].vt.vaDestroySurfaces(&d->in[i].ctx, part, n);
        if (s != VA_STATUS_SUCCESS)
            result = s;
    }
    free(part);
    return result;
}

static VAStatus j_sync_surface(VADriverContextP ctx, VASurfaceID s)
{
    BY_ID(vaSyncSurface, VA_STATUS_ERROR_INVALID_SURFACE, s, s);
}

static VAStatus j_sync_surface2(VADriverContextP ctx, VASurfaceID s,
                                uint64_t timeout)
{
    BY_ID(vaSyncSurface2, VA_STATUS_ERROR_INVALID_SURFACE, s, s, timeout);
}

static VAStatus j_query_surface_status(VADriverContextP ctx, VASurfaceID s,
                                       VASurfaceStatus *status)
{
    BY_ID(vaQuerySurfaceStatus, VA_STATUS_ERROR_INVALID_SURFACE, s, s,
          status);
}

static VAStatus j_query_surface_error(VADriverContextP ctx, VASurfaceID s,
                                      VAStatus error, void **info)
{
    BY_ID(vaQuerySurfaceError, VA_STATUS_ERROR_INVALID_SURFACE, s, s, error,
          info);
}

static VAStatus j_put_surface(VADriverContextP ctx, VASurfaceID s, void *draw,
                              short sx, short sy, unsigned short sw,
                              unsigned short sh, short dx, short dy,
                              unsigned short dw, unsigned short dh,
                              VARectangle *clip, unsigned int nclip,
                              unsigned int flags)
{
    BY_ID(vaPutSurface, VA_STATUS_ERROR_INVALID_SURFACE, s, s, draw, sx, sy,
          sw, sh, dx, dy, dw, dh, clip, nclip, flags);
}

static VAStatus j_lock_surface(VADriverContextP ctx, VASurfaceID s,
                               unsigned int *fourcc,
                               unsigned int *luma_stride,
                               unsigned int *chroma_u_stride,
                               unsigned int *chroma_v_stride,
                               unsigned int *luma_offset,
                               unsigned int *chroma_u_offset,
                               unsigned int *chroma_v_offset,
                               unsigned int *buffer_name, void **buffer)
{
    BY_ID(vaLockSurface, VA_STATUS_ERROR_INVALID_SURFACE, s, s, fourcc,
          luma_stride, chroma_u_stride, chroma_v_stride, luma_offset,
          chroma_u_offset, chroma_v_offset, buffer_name, buffer);
}

static VAStatus j_unlock_surface(VADriverContextP ctx, VASurfaceID s)
{
    BY_ID(vaUnlockSurface, VA_STATUS_ERROR_INVALID_SURFACE, s, s);
}

static VAStatus j_export_surface_handle(VADriverContextP ctx, VASurfaceID s,
                                        uint32_t mem_type, uint32_t flags,
                                        void *descriptor)
{
    BY_ID(vaExportSurfaceHandle, VA_STATUS_ERROR_INVALID_SURFACE, s, s,
          mem_type, flags, descriptor);
}

/* ---- contexts / pictures ---------------------------------------------- */

static VAStatus j_create_context(VADriverContextP ctx, VAConfigID config,
                                 int width, int height, int flag,
                                 VASurfaceID *targets, int num_targets,
                                 VAContextID *context)
{
    int i = owner_of(config);
    struct inner *in = &D(ctx)->in[i];
    VAStatus s;
    for (int k = 0; k < num_targets; k++)
        if (targets && owner_of(targets[k]) != i) {
            tracef("CreateContext: render target %#x not owned by %s",
                   targets[k], i == ENC ? "enc" : "dec");
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    if (!in->ok)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    s = in->vt.vaCreateContext(&in->ctx, config, width, height, flag, targets,
                               num_targets, context);
    tracef("CreateContext config=%#x %dx%d targets=%d -> %s ctx=%#x "
           "status=%d", config, width, height, num_targets,
           i == ENC ? "enc" : "dec", s == VA_STATUS_SUCCESS ? *context : 0,
           s);
    return s;
}

static VAStatus j_destroy_context(VADriverContextP ctx, VAContextID c)
{
    BY_ID(vaDestroyContext, VA_STATUS_ERROR_INVALID_CONTEXT, c, c);
}

static VAStatus j_begin_picture(VADriverContextP ctx, VAContextID c,
                                VASurfaceID target)
{
    if (owner_of(target) != owner_of(c))
        return VA_STATUS_ERROR_INVALID_SURFACE;
    BY_ID(vaBeginPicture, VA_STATUS_ERROR_INVALID_CONTEXT, c, c, target);
}

static VAStatus j_render_picture(VADriverContextP ctx, VAContextID c,
                                 VABufferID *buffers, int num)
{
    for (int k = 0; k < num; k++)
        if (owner_of(buffers[k]) != owner_of(c))
            return VA_STATUS_ERROR_INVALID_BUFFER;
    BY_ID(vaRenderPicture, VA_STATUS_ERROR_INVALID_CONTEXT, c, c, buffers,
          num);
}

static VAStatus j_end_picture(VADriverContextP ctx, VAContextID c)
{
    BY_ID(vaEndPicture, VA_STATUS_ERROR_INVALID_CONTEXT, c, c);
}

/* ---- buffers ---------------------------------------------------------- */

static VAStatus j_create_buffer(VADriverContextP ctx, VAContextID c,
                                VABufferType type, unsigned int size,
                                unsigned int num, void *data, VABufferID *buf)
{
    BY_ID(vaCreateBuffer, VA_STATUS_ERROR_INVALID_CONTEXT, c, c, type, size,
          num, data, buf);
}

static VAStatus j_create_buffer2(VADriverContextP ctx, VAContextID c,
                                 VABufferType type, unsigned int width,
                                 unsigned int height,
                                 unsigned int *unit_size,
                                 unsigned int *pitch, VABufferID *buf)
{
    BY_ID(vaCreateBuffer2, VA_STATUS_ERROR_INVALID_CONTEXT, c, c, type, width,
          height, unit_size, pitch, buf);
}

static VAStatus j_buffer_set_num_elements(VADriverContextP ctx, VABufferID b,
                                          unsigned int n)
{
    BY_ID(vaBufferSetNumElements, VA_STATUS_ERROR_INVALID_BUFFER, b, b, n);
}

static VAStatus j_map_buffer(VADriverContextP ctx, VABufferID b, void **p)
{
    BY_ID(vaMapBuffer, VA_STATUS_ERROR_INVALID_BUFFER, b, b, p);
}

static VAStatus j_map_buffer2(VADriverContextP ctx, VABufferID b, void **p,
                              uint32_t flags)
{
    struct inner *in = &D(ctx)->in[owner_of(b)];
    if (!in->ok)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    /* Drivers without vaMapBuffer2 still map through vaMapBuffer. */
    if (!in->vt.vaMapBuffer2)
        return in->vt.vaMapBuffer(&in->ctx, b, p);
    return in->vt.vaMapBuffer2(&in->ctx, b, p, flags);
}

static VAStatus j_unmap_buffer(VADriverContextP ctx, VABufferID b)
{
    BY_ID(vaUnmapBuffer, VA_STATUS_ERROR_INVALID_BUFFER, b, b);
}

static VAStatus j_destroy_buffer(VADriverContextP ctx, VABufferID b)
{
    BY_ID(vaDestroyBuffer, VA_STATUS_ERROR_INVALID_BUFFER, b, b);
}

static VAStatus j_buffer_info(VADriverContextP ctx, VABufferID b,
                              VABufferType *type, unsigned int *size,
                              unsigned int *num)
{
    BY_ID(vaBufferInfo, VA_STATUS_ERROR_INVALID_BUFFER, b, b, type, size,
          num);
}

static VAStatus j_acquire_buffer_handle(VADriverContextP ctx, VABufferID b,
                                        VABufferInfo *info)
{
    BY_ID(vaAcquireBufferHandle, VA_STATUS_ERROR_INVALID_BUFFER, b, b, info);
}

static VAStatus j_release_buffer_handle(VADriverContextP ctx, VABufferID b)
{
    BY_ID(vaReleaseBufferHandle, VA_STATUS_ERROR_INVALID_BUFFER, b, b);
}

static VAStatus j_sync_buffer(VADriverContextP ctx, VABufferID b,
                              uint64_t timeout)
{
    BY_ID(vaSyncBuffer, VA_STATUS_ERROR_INVALID_BUFFER, b, b, timeout);
}

/* ---- images ----------------------------------------------------------- */

static VAStatus j_query_image_formats(VADriverContextP ctx,
                                      VAImageFormat *formats, int *num)
{
    struct dispatch *d = D(ctx);
    int i = d->in[DEC].ok ? DEC : ENC;
    CALL(d, i, VA_STATUS_ERROR_OPERATION_FAILED, vaQueryImageFormats,
         formats, num);
}

static VAStatus j_create_image(VADriverContextP ctx, VAImageFormat *format,
                               int width, int height, VAImage *image)
{
    struct dispatch *d = D(ctx);
    int i = d->in[DEC].ok ? DEC : ENC;
    CALL(d, i, VA_STATUS_ERROR_OPERATION_FAILED, vaCreateImage, format, width,
         height, image);
}

static VAStatus j_derive_image(VADriverContextP ctx, VASurfaceID s,
                               VAImage *image)
{
    BY_ID(vaDeriveImage, VA_STATUS_ERROR_INVALID_SURFACE, s, s, image);
}

static VAStatus j_destroy_image(VADriverContextP ctx, VAImageID image)
{
    BY_ID(vaDestroyImage, VA_STATUS_ERROR_INVALID_IMAGE, image, image);
}

static VAStatus j_set_image_palette(VADriverContextP ctx, VAImageID image,
                                    unsigned char *palette)
{
    BY_ID(vaSetImagePalette, VA_STATUS_ERROR_INVALID_IMAGE, image, image,
          palette);
}

static VAStatus j_get_image(VADriverContextP ctx, VASurfaceID s, int x, int y,
                            unsigned int w, unsigned int h, VAImageID image)
{
    if (owner_of(s) != owner_of(image))
        return VA_STATUS_ERROR_INVALID_IMAGE;
    BY_ID(vaGetImage, VA_STATUS_ERROR_INVALID_SURFACE, s, s, x, y, w, h,
          image);
}

static VAStatus j_put_image(VADriverContextP ctx, VASurfaceID s,
                            VAImageID image, int sx, int sy, unsigned int sw,
                            unsigned int sh, int dx, int dy, unsigned int dw,
                            unsigned int dh)
{
    if (owner_of(s) != owner_of(image))
        return VA_STATUS_ERROR_INVALID_IMAGE;
    BY_ID(vaPutImage, VA_STATUS_ERROR_INVALID_SURFACE, s, s, image, sx, sy,
          sw, sh, dx, dy, dw, dh);
}

/* ---- subpictures / display attributes: decoder only ------------------- */

static VAStatus j_query_subpicture_formats(VADriverContextP ctx,
                                           VAImageFormat *f,
                                           unsigned int *flags,
                                           unsigned int *num)
{
    struct inner *in = &D(ctx)->in[DEC];
    if (!in->ok || !in->vt.vaQuerySubpictureFormats) {
        *num = 0;
        return VA_STATUS_SUCCESS;
    }
    return in->vt.vaQuerySubpictureFormats(&in->ctx, f, flags, num);
}

static VAStatus j_query_display_attributes(VADriverContextP ctx,
                                           VADisplayAttribute *a, int *num)
{
    struct inner *in = &D(ctx)->in[DEC];
    if (!in->ok || !in->vt.vaQueryDisplayAttributes) {
        *num = 0;
        return VA_STATUS_SUCCESS;
    }
    return in->vt.vaQueryDisplayAttributes(&in->ctx, a, num);
}

static VAStatus j_get_display_attributes(VADriverContextP ctx,
                                         VADisplayAttribute *a, int num)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED,
         vaGetDisplayAttributes, a, num);
}

static VAStatus j_set_display_attributes(VADriverContextP ctx,
                                         VADisplayAttribute *a, int num)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED,
         vaSetDisplayAttributes, a, num);
}

/* libva requires the subpicture entries; forward them to the decoder. */
static VAStatus j_create_subpicture(VADriverContextP ctx, VAImageID image,
                                    VASubpictureID *sub)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED, vaCreateSubpicture,
         image, sub);
}

static VAStatus j_destroy_subpicture(VADriverContextP ctx, VASubpictureID sub)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED, vaDestroySubpicture,
         sub);
}

static VAStatus j_set_subpicture_image(VADriverContextP ctx,
                                       VASubpictureID sub, VAImageID image)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED, vaSetSubpictureImage,
         sub, image);
}

static VAStatus j_set_subpicture_chromakey(VADriverContextP ctx,
                                           VASubpictureID sub,
                                           unsigned int min, unsigned int max,
                                           unsigned int mask)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED, vaSetSubpictureChromakey,
         sub, min, max, mask);
}

static VAStatus j_set_subpicture_global_alpha(VADriverContextP ctx,
                                              VASubpictureID sub, float alpha)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED,
         vaSetSubpictureGlobalAlpha, sub, alpha);
}

static VAStatus j_associate_subpicture(VADriverContextP ctx,
                                       VASubpictureID sub,
                                       VASurfaceID *targets, int num,
                                       short sx, short sy, unsigned short sw,
                                       unsigned short sh, short dx, short dy,
                                       unsigned short dw, unsigned short dh,
                                       unsigned int flags)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED, vaAssociateSubpicture,
         sub, targets, num, sx, sy, sw, sh, dx, dy, dw, dh, flags);
}

static VAStatus j_deassociate_subpicture(VADriverContextP ctx,
                                         VASubpictureID sub,
                                         VASurfaceID *targets, int num)
{
    CALL(D(ctx), DEC, VA_STATUS_ERROR_UNIMPLEMENTED, vaDeassociateSubpicture,
         sub, targets, num);
}

/* ---- init ------------------------------------------------------------- */

static bool load_inner(VADriverContextP outer, struct inner *in,
                       const char *path, const char *what)
{
    VAStatus (*init)(VADriverContextP);

    in->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!in->handle) {
        fprintf(stderr, "j713-va: %s driver %s: %s\n", what, path, dlerror());
        return false;
    }
    init = (VAStatus (*)(VADriverContextP))dlsym(in->handle,
                                                 J713_INIT_SYMBOL);
    if (!init) {
        fprintf(stderr, "j713-va: %s driver %s has no %s\n", what, path,
                J713_INIT_SYMBOL);
        dlclose(in->handle);
        in->handle = NULL;
        return false;
    }

    /* Each inner driver sees its own context and vtables, sharing the
     * display, DRM state and callbacks of the outer one. */
    in->ctx = *outer;
    in->ctx.pDriverData = NULL;
    in->ctx.vtable = &in->vt;
    in->ctx.vtable_vpp = &in->vt_vpp;
    in->ctx.vtable_prot = &in->vt_prot;
    in->ctx.vtable_glx = NULL;
    in->ctx.vtable_egl = NULL;
    in->ctx.vtable_wayland = NULL;
    in->ctx.vtable_tpi = NULL;
    in->ctx.handle = in->handle;
    in->ctx.str_vendor = NULL;
    in->vt_vpp.version = VA_DRIVER_VTABLE_VPP_VERSION;
    in->vt_prot.version = VA_DRIVER_VTABLE_PROT_VERSION;

    VAStatus s = init(&in->ctx);
    if (s != VA_STATUS_SUCCESS) {
        fprintf(stderr, "j713-va: %s driver %s init failed: %s\n", what, path,
                vaErrorStr(s));
        dlclose(in->handle);
        in->handle = NULL;
        return false;
    }
    in->ok = true;
    tracef("%s driver %s: %s", what, path,
           in->ctx.str_vendor ? in->ctx.str_vendor : "(no vendor)");
    return true;
}

static int imax(int a, int b) { return a > b ? a : b; }

VAStatus __attribute__((visibility("default")))
__vaDriverInit_1_24(VADriverContextP ctx);

VAStatus __vaDriverInit_1_24(VADriverContextP ctx)
{
    struct dispatch *d;
    struct VADriverVTable *vt = ctx->vtable;
    const char *dec = getenv("J713_VA_DEC_DRIVER");
    const char *enc = getenv("J713_VA_ENC_DRIVER");
    const char *t = getenv("J713_VA_DISPATCH_TRACE");

    trace = t && *t && *t != '0';
    d = calloc(1, sizeof(*d));
    if (!d)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    load_inner(ctx, &d->in[DEC], dec && *dec ? dec : J713_DEC_DRIVER,
               "decode");
    load_inner(ctx, &d->in[ENC], enc && *enc ? enc : J713_ENC_DRIVER,
               "encode");
    if (!d->in[DEC].ok && !d->in[ENC].ok) {
        free(d);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    ctx->max_profiles = 0;
    ctx->max_entrypoints = 0;
    ctx->max_attributes = 0;
    ctx->max_image_formats = 0;
    ctx->max_subpic_formats = 0;
    ctx->max_display_attributes = 0;
    for (int i = 0; i < NUM_INNER; i++) {
        struct VADriverContext *c = &d->in[i].ctx;
        if (!d->in[i].ok)
            continue;
        ctx->max_profiles += c->max_profiles;
        ctx->max_entrypoints += c->max_entrypoints;
        ctx->max_attributes = imax(ctx->max_attributes, c->max_attributes);
        ctx->max_image_formats =
            imax(ctx->max_image_formats, c->max_image_formats);
        ctx->max_subpic_formats =
            imax(ctx->max_subpic_formats, c->max_subpic_formats);
        ctx->max_display_attributes =
            imax(ctx->max_display_attributes, c->max_display_attributes);
    }
    /* Keep the decoder's vendor string first: Chrome classifies the VA
     * implementation by it, and decode behaviour must not change. */
    snprintf(d->vendor, sizeof(d->vendor), "%s%s%s",
             d->in[DEC].ok && d->in[DEC].ctx.str_vendor
                 ? d->in[DEC].ctx.str_vendor : "",
             d->in[DEC].ok && d->in[ENC].ok ? " + " : "",
             d->in[ENC].ok && d->in[ENC].ctx.str_vendor
                 ? d->in[ENC].ctx.str_vendor : "");
    ctx->str_vendor = d->vendor;
    ctx->pDriverData = d;

#define SET(field, fn) vt->field = fn
    SET(vaTerminate, j_terminate);
    SET(vaQueryConfigProfiles, j_query_config_profiles);
    SET(vaQueryConfigEntrypoints, j_query_config_entrypoints);
    SET(vaGetConfigAttributes, j_get_config_attributes);
    SET(vaCreateConfig, j_create_config);
    SET(vaDestroyConfig, j_destroy_config);
    SET(vaQueryConfigAttributes, j_query_config_attributes);
    SET(vaCreateSurfaces, j_create_surfaces);
    SET(vaDestroySurfaces, j_destroy_surfaces);
    SET(vaCreateContext, j_create_context);
    SET(vaDestroyContext, j_destroy_context);
    SET(vaCreateBuffer, j_create_buffer);
    SET(vaBufferSetNumElements, j_buffer_set_num_elements);
    SET(vaMapBuffer, j_map_buffer);
    SET(vaUnmapBuffer, j_unmap_buffer);
    SET(vaDestroyBuffer, j_destroy_buffer);
    SET(vaBeginPicture, j_begin_picture);
    SET(vaRenderPicture, j_render_picture);
    SET(vaEndPicture, j_end_picture);
    SET(vaSyncSurface, j_sync_surface);
    SET(vaQuerySurfaceStatus, j_query_surface_status);
    SET(vaQuerySurfaceError, j_query_surface_error);
    SET(vaPutSurface, j_put_surface);
    SET(vaQueryImageFormats, j_query_image_formats);
    SET(vaCreateImage, j_create_image);
    SET(vaDeriveImage, j_derive_image);
    SET(vaDestroyImage, j_destroy_image);
    SET(vaSetImagePalette, j_set_image_palette);
    SET(vaGetImage, j_get_image);
    SET(vaPutImage, j_put_image);
    SET(vaQuerySubpictureFormats, j_query_subpicture_formats);
    SET(vaCreateSubpicture, j_create_subpicture);
    SET(vaDestroySubpicture, j_destroy_subpicture);
    SET(vaSetSubpictureImage, j_set_subpicture_image);
    SET(vaSetSubpictureChromakey, j_set_subpicture_chromakey);
    SET(vaSetSubpictureGlobalAlpha, j_set_subpicture_global_alpha);
    SET(vaAssociateSubpicture, j_associate_subpicture);
    SET(vaDeassociateSubpicture, j_deassociate_subpicture);
    SET(vaQueryDisplayAttributes, j_query_display_attributes);
    SET(vaGetDisplayAttributes, j_get_display_attributes);
    SET(vaSetDisplayAttributes, j_set_display_attributes);
    SET(vaBufferInfo, j_buffer_info);
    SET(vaLockSurface, j_lock_surface);
    SET(vaUnlockSurface, j_unlock_surface);
    SET(vaGetSurfaceAttributes, j_get_surface_attributes);
    SET(vaCreateSurfaces2, j_create_surfaces2);
    SET(vaQuerySurfaceAttributes, j_query_surface_attributes);
    SET(vaAcquireBufferHandle, j_acquire_buffer_handle);
    SET(vaReleaseBufferHandle, j_release_buffer_handle);
    SET(vaCreateBuffer2, j_create_buffer2);
    SET(vaQueryProcessingRate, j_query_processing_rate);
    SET(vaExportSurfaceHandle, j_export_surface_handle);
    SET(vaSyncSurface2, j_sync_surface2);
    SET(vaSyncBuffer, j_sync_buffer);
    SET(vaMapBuffer2, j_map_buffer2);
#undef SET
    /* MF and vaCopy stay unimplemented (NULL); libva treats them as
     * optional and neither inner driver supports them. */
    return VA_STATUS_SUCCESS;
}
