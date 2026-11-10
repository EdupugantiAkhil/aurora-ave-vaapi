// Fake VA driver for dispatcher tests. Build with -DFAKE_ENC=0 (decoder IDs
// below 0x20000000) or -DFAKE_ENC=1 (IDs with bit 31). FAKE_BAD_IDS=1 makes
// the encoder hand out low IDs; FAKE_FAIL_<DEC|ENC>=1 fails init.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va.h>
#include <va/va_backend.h>
extern void fake_log(const char *who, const char *what, unsigned id);
#if FAKE_ENC
#define WHO "enc"
#define BASE (getenv("FAKE_BAD_IDS") ? 0x01000000u : 0x81000000u)
#else
#define WHO "dec"
#define BASE 0x01000000u
#endif
static unsigned next = 1;
static unsigned nid(unsigned type) { return BASE + (type << 24) + next++; }
static VAStatus term(VADriverContextP c) { fake_log(WHO, "Terminate", 0); return 0; }
static VAStatus qprof(VADriverContextP c, VAProfile *p, int *n) { p[0] = VAProfileH264High; p[1] = VAProfileVP9Profile0; *n = FAKE_ENC ? 1 : 2; return 0; }
static VAStatus qentry(VADriverContextP c, VAProfile p, VAEntrypoint *e, int *n) {
#if FAKE_ENC
    e[0] = VAEntrypointEncSlice; e[1] = VAEntrypointVLD; *n = 2; /* VLD must be filtered */
#else
    e[0] = VAEntrypointVLD; *n = 1;
#endif
    return 0; }
static VAStatus getattr(VADriverContextP c, VAProfile p, VAEntrypoint e, VAConfigAttrib *a, int n) { fake_log(WHO, "GetConfigAttributes", e); return 0; }
static VAStatus mkcfg(VADriverContextP c, VAProfile p, VAEntrypoint e, VAConfigAttrib *a, int n, VAConfigID *id) { *id = nid(0); fake_log(WHO, "CreateConfig", *id); return 0; }
static VAStatus rmcfg(VADriverContextP c, VAConfigID id) { fake_log(WHO, "DestroyConfig", id); return 0; }
static VAStatus mksurf2(VADriverContextP c, unsigned f, unsigned w, unsigned h, VASurfaceID *s, unsigned n, VASurfaceAttrib *a, unsigned na) { for (unsigned i = 0; i < n; i++) s[i] = nid(2); fake_log(WHO, "CreateSurfaces2", s[0]); return 0; }
static VAStatus mksurf(VADriverContextP c, int w, int h, int f, int n, VASurfaceID *s) { for (int i = 0; i < n; i++) s[i] = nid(2); fake_log(WHO, "CreateSurfaces", s[0]); return 0; }
static VAStatus rmsurf(VADriverContextP c, VASurfaceID *s, int n) { for (int i = 0; i < n; i++) fake_log(WHO, "DestroySurface", s[i]); return 0; }
static VAStatus mkctx(VADriverContextP c, VAConfigID cfg, int w, int h, int fl, VASurfaceID *t, int nt, VAContextID *id) { *id = nid(1); fake_log(WHO, "CreateContext", *id); return 0; }
static VAStatus mkbuf(VADriverContextP c, VAContextID ctx, VABufferType t, unsigned sz, unsigned n, void *d, VABufferID *id) { *id = nid(3); fake_log(WHO, "CreateBuffer", *id); return 0; }
static char mem[64];
static VAStatus mapbuf(VADriverContextP c, VABufferID id, void **p) { *p = mem; fake_log(WHO, "MapBuffer", id); return 0; }
static VAStatus begin(VADriverContextP c, VAContextID ctx, VASurfaceID s) { fake_log(WHO, "BeginPicture", s); return 0; }
static VAStatus render(VADriverContextP c, VAContextID ctx, VABufferID *b, int n) { fake_log(WHO, "RenderPicture", b[0]); return 0; }
static VAStatus end(VADriverContextP c, VAContextID ctx) { fake_log(WHO, "EndPicture", ctx); return 0; }
static VAStatus sync(VADriverContextP c, VASurfaceID s) { fake_log(WHO, "SyncSurface", s); return 0; }
static VAStatus exportsurf(VADriverContextP c, VASurfaceID s, uint32_t m, uint32_t f, void *d) { fake_log(WHO, "ExportSurfaceHandle", s); return 0; }
VAStatus __vaDriverInit_1_24(VADriverContextP c)
{
    if (getenv(FAKE_ENC ? "FAKE_FAIL_ENC" : "FAKE_FAIL_DEC")) return VA_STATUS_ERROR_OPERATION_FAILED;
    struct VADriverVTable *v = c->vtable;
    c->max_profiles = 4; c->max_entrypoints = 4; c->max_attributes = 8; c->max_image_formats = 2;
    c->str_vendor = FAKE_ENC ? "fake-enc" : "fake-dec";
    v->vaTerminate = term; v->vaQueryConfigProfiles = qprof; v->vaQueryConfigEntrypoints = qentry;
    v->vaGetConfigAttributes = getattr; v->vaCreateConfig = mkcfg; v->vaDestroyConfig = rmcfg;
    v->vaCreateSurfaces2 = mksurf2; v->vaCreateSurfaces = mksurf; v->vaDestroySurfaces = rmsurf;
    v->vaCreateContext = mkctx; v->vaCreateBuffer = mkbuf; v->vaMapBuffer = mapbuf;
    v->vaBeginPicture = begin; v->vaRenderPicture = render; v->vaEndPicture = end;
    v->vaSyncSurface = sync; v->vaExportSurfaceHandle = exportsurf;
    return 0;
}
