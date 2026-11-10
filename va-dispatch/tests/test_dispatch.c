// Host test of j713_drv_video routing with two fake inner drivers.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_drmcommon.h>
static char last_who[8], last_what[64]; static unsigned last_id; static int dec_destroyed, enc_destroyed;
void fake_log(const char *who, const char *what, unsigned id)
{ snprintf(last_who, sizeof last_who, "%s", who); snprintf(last_what, sizeof last_what, "%s", what); last_id = id;
  if (!strcmp(what, "DestroySurface")) { if (!strcmp(who, "dec")) dec_destroyed++; else enc_destroyed++; } }
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (last %s %s %#x)\n", __FILE__, __LINE__, #c, last_who, last_what, last_id); fails++; } } while (0)
static struct VADriverContext ctx; static struct VADriverVTable vt; static struct VADriverVTableVPP vpp;
static VAStatus (*init)(VADriverContextP);
static void setup(void) { memset(&ctx, 0, sizeof ctx); memset(&vt, 0, sizeof vt); ctx.vtable = &vt; ctx.vtable_vpp = &vpp; CHECK(init(&ctx) == VA_STATUS_SUCCESS); }
static VASurfaceAttrib hint(int h) { VASurfaceAttrib a = { .type = VASurfaceAttribUsageHint, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = { .type = VAGenericValueTypeInteger, .value.i = h } }; return a; }
static VASurfaceAttrib memtype(int m) { VASurfaceAttrib a = { .type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE, .value = { .type = VAGenericValueTypeInteger, .value.i = m } }; return a; }
int main(int argc, char **argv)
{
    void *h = dlopen(argv[1], RTLD_NOW); if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    init = dlsym(h, "__vaDriverInit_1_24"); if (!init) return 2;

    /* 1. both drivers */
    setup();
    /* libva's va_openDriver CHECK_VTABLE list (va.c) */
    void *req[] = { vt.vaTerminate, vt.vaQueryConfigProfiles, vt.vaQueryConfigEntrypoints, vt.vaQueryConfigAttributes,
        vt.vaCreateConfig, vt.vaDestroyConfig, vt.vaGetConfigAttributes, vt.vaCreateSurfaces, vt.vaDestroySurfaces,
        vt.vaCreateContext, vt.vaDestroyContext, vt.vaCreateBuffer, vt.vaBufferSetNumElements, vt.vaMapBuffer,
        vt.vaUnmapBuffer, vt.vaDestroyBuffer, vt.vaBeginPicture, vt.vaRenderPicture, vt.vaEndPicture, vt.vaSyncSurface,
        vt.vaQuerySurfaceStatus, vt.vaQueryImageFormats, vt.vaCreateImage, vt.vaDeriveImage, vt.vaDestroyImage,
        vt.vaSetImagePalette, vt.vaGetImage, vt.vaPutImage, vt.vaQuerySubpictureFormats, vt.vaCreateSubpicture,
        vt.vaDestroySubpicture, vt.vaSetSubpictureImage, vt.vaSetSubpictureChromakey, vt.vaSetSubpictureGlobalAlpha,
        vt.vaAssociateSubpicture, vt.vaDeassociateSubpicture, vt.vaQueryDisplayAttributes, vt.vaGetDisplayAttributes,
        vt.vaSetDisplayAttributes };
    for (unsigned i = 0; i < sizeof req / sizeof req[0]; i++) CHECK(req[i] != NULL);
    CHECK(ctx.str_vendor && strstr(ctx.str_vendor, "fake-dec") == ctx.str_vendor && strstr(ctx.str_vendor, "fake-enc"));
    VAProfile prof[16]; int np = 0; CHECK(vt.vaQueryConfigProfiles(&ctx, prof, &np) == 0);
    int h264 = 0, vp9 = 0; for (int i = 0; i < np; i++) { h264 += prof[i] == VAProfileH264High; vp9 += prof[i] == VAProfileVP9Profile0; }
    CHECK(h264 == 1 && vp9 == 1 && np == 2);
    VAEntrypoint ent[16]; int ne = 0; CHECK(vt.vaQueryConfigEntrypoints(&ctx, VAProfileH264High, ent, &ne) == 0);
    int vld = 0, enc = 0; for (int i = 0; i < ne; i++) { vld += ent[i] == VAEntrypointVLD; enc += ent[i] == VAEntrypointEncSlice; }
    CHECK(ne == 2 && vld == 1 && enc == 1); /* encoder's VLD filtered */
    vt.vaGetConfigAttributes(&ctx, VAProfileH264High, VAEntrypointEncSlice, NULL, 0); CHECK(!strcmp(last_who, "enc"));
    vt.vaGetConfigAttributes(&ctx, VAProfileH264High, VAEntrypointVLD, NULL, 0); CHECK(!strcmp(last_who, "dec"));

    VAConfigID dcfg, ecfg;
    CHECK(vt.vaCreateConfig(&ctx, VAProfileH264High, VAEntrypointVLD, NULL, 0, &dcfg) == 0 && !strcmp(last_who, "dec") && !(dcfg & 0x80000000u));
    CHECK(vt.vaCreateConfig(&ctx, VAProfileH264High, VAEntrypointEncSlice, NULL, 0, &ecfg) == 0 && !strcmp(last_who, "enc") && (ecfg & 0x80000000u));

    VASurfaceID s_enc[2], s_dec[2], s_imp, s_none, s_legacy;
    VASurfaceAttrib a;
    a = hint(VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER); CHECK(vt.vaCreateSurfaces2(&ctx, VA_RT_FORMAT_YUV420, 64, 64, s_enc, 2, &a, 1) == 0 && !strcmp(last_who, "enc"));
    a = hint(VA_SURFACE_ATTRIB_USAGE_HINT_DECODER); CHECK(vt.vaCreateSurfaces2(&ctx, VA_RT_FORMAT_YUV420, 64, 64, s_dec, 2, &a, 1) == 0 && !strcmp(last_who, "dec"));
    VASurfaceAttrib imp[2] = { memtype(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME), { .type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE } };
    CHECK(vt.vaCreateSurfaces2(&ctx, VA_RT_FORMAT_YUV420, 64, 64, &s_imp, 1, imp, 2) == 0 && !strcmp(last_who, "enc"));
    a = hint(VA_SURFACE_ATTRIB_USAGE_HINT_GENERIC); CHECK(vt.vaCreateSurfaces2(&ctx, VA_RT_FORMAT_RGB32, 64, 64, &s_none, 1, &a, 1) == 0 && !strcmp(last_who, "dec"));
    CHECK(vt.vaCreateSurfaces2(&ctx, VA_RT_FORMAT_YUV420, 64, 64, &s_none, 1, NULL, 0) == 0 && !strcmp(last_who, "dec"));
    CHECK(vt.vaCreateSurfaces(&ctx, 64, 64, VA_RT_FORMAT_YUV420, 1, &s_legacy) == 0 && !strcmp(last_who, "dec"));

    VAContextID ectx, dctx;
    VASurfaceID mixed[2] = { s_enc[0], s_dec[0] };
    CHECK(vt.vaCreateContext(&ctx, ecfg, 64, 64, 0, mixed, 2, &ectx) == VA_STATUS_ERROR_INVALID_SURFACE);
    CHECK(vt.vaCreateContext(&ctx, ecfg, 64, 64, 0, s_enc, 2, &ectx) == 0 && !strcmp(last_who, "enc") && (ectx & 0x80000000u));
    CHECK(vt.vaCreateContext(&ctx, dcfg, 64, 64, 0, s_dec, 2, &dctx) == 0 && !strcmp(last_who, "dec"));
    VABufferID eb, db; void *p;
    CHECK(vt.vaCreateBuffer(&ctx, ectx, VAEncCodedBufferType, 16, 1, NULL, &eb) == 0 && !strcmp(last_who, "enc") && (eb & 0x80000000u));
    CHECK(vt.vaCreateBuffer(&ctx, dctx, VASliceDataBufferType, 16, 1, NULL, &db) == 0 && !strcmp(last_who, "dec"));
    CHECK(vt.vaBeginPicture(&ctx, ectx, s_imp) == 0 && !strcmp(last_who, "enc") && last_id == s_imp);
    CHECK(vt.vaBeginPicture(&ctx, ectx, s_dec[0]) == VA_STATUS_ERROR_INVALID_SURFACE);
    VABufferID bad[2] = { eb, db };
    CHECK(vt.vaRenderPicture(&ctx, ectx, bad, 2) == VA_STATUS_ERROR_INVALID_BUFFER);
    CHECK(vt.vaRenderPicture(&ctx, ectx, &eb, 1) == 0 && !strcmp(last_who, "enc"));
    CHECK(vt.vaEndPicture(&ctx, ectx) == 0 && !strcmp(last_who, "enc"));
    CHECK(vt.vaMapBuffer(&ctx, eb, &p) == 0 && !strcmp(last_who, "enc"));
    CHECK(vt.vaMapBuffer2(&ctx, db, &p, 0) == 0 && !strcmp(last_who, "dec")); /* falls back to MapBuffer */
    CHECK(vt.vaSyncSurface(&ctx, s_enc[1]) == 0 && !strcmp(last_who, "enc"));
    CHECK(vt.vaExportSurfaceHandle(&ctx, s_dec[1], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, NULL) == 0 && !strcmp(last_who, "dec"));
    VASurfaceID all[5] = { s_enc[0], s_dec[0], s_enc[1], s_dec[1], s_imp };
    dec_destroyed = enc_destroyed = 0;
    CHECK(vt.vaDestroySurfaces(&ctx, all, 5) == 0 && dec_destroyed == 2 && enc_destroyed == 3);
    CHECK(vt.vaDestroyConfig(&ctx, ecfg) == 0 && !strcmp(last_who, "enc"));
    CHECK(vt.vaTerminate(&ctx) == 0);

    /* 2. encoder init fails: decode-only, no encode entrypoints */
    setenv("FAKE_FAIL_ENC", "1", 1); setup(); unsetenv("FAKE_FAIL_ENC");
    CHECK(vt.vaQueryConfigEntrypoints(&ctx, VAProfileH264High, ent, &ne) == 0 && ne == 1 && ent[0] == VAEntrypointVLD);
    CHECK(vt.vaCreateConfig(&ctx, VAProfileH264High, VAEntrypointEncSlice, NULL, 0, &ecfg) == VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
    CHECK(vt.vaTerminate(&ctx) == 0);

    /* 3. encoder breaks the ID-range contract: refuse, don't misroute */
    setenv("FAKE_BAD_IDS", "1", 1); setup();
    CHECK(vt.vaCreateConfig(&ctx, VAProfileH264High, VAEntrypointEncSlice, NULL, 0, &ecfg) == VA_STATUS_ERROR_OPERATION_FAILED && ecfg == VA_INVALID_ID);
    unsetenv("FAKE_BAD_IDS"); CHECK(vt.vaTerminate(&ctx) == 0);

    /* 4. both fail: init fails */
    setenv("FAKE_FAIL_ENC", "1", 1); setenv("FAKE_FAIL_DEC", "1", 1);
    memset(&ctx, 0, sizeof ctx); ctx.vtable = &vt; ctx.vtable_vpp = &vpp; CHECK(init(&ctx) != VA_STATUS_SUCCESS);

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}
