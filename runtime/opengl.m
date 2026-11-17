// opengl32.dll / WGL / the GDI pixel-format calls, on macOS's OpenGL 4.1 core profile (CGL).
//
// Games written for Windows OpenGL get:
//   - wgl contexts as CGL 4.1 core contexts, all in one share group (so wglShareLists, which CGL cannot do after
//     creation, is a no-op that is already true). The guest may ask for a compatibility profile; macOS has none
//     above 2.1, so it gets core plus the fixups below.
//   - the window's default framebuffer emulated: each context has an FBO that stands in for framebuffer 0, its
//     color buffer an IOSurface-backed texture (three, rotated per frame) and a depth/stencil renderbuffer.
//     SwapBuffers shows the finished IOSurface in the window's CAMetalLayer with a Metal pass (no NSOpenGLView,
//     so the game may render from any thread). Every GL entry point that names framebuffer 0 or the window's
//     buffers (GL_BACK...) is translated.
//   - entry points: generated thunks for everything in gl3.h (tools/gen_gl.py -> gl_thunks.h), hand-written ones
//     here; wglGetProcAddress also maps ARB/EXT names to their core equivalents. Anything else is NULL, so the
//     game sees that extension as missing.
//   - GL_EXTENSIONS: the host's list plus every extension the 4.1 core profile subsumes (games check the strings).
//   - core-profile fixups: a default vertex array object for VAO 0, legacy enables and fixed-function calls
//     ignored, polygon mode faces, wide lines clamped.
//
// Environment: RT_GL_CHECK=1 checks glGetError after every call (logged once per function and error),
// RT_GL_SHADERS=dir dumps every shader source there.
#define GL_SILENCE_DEPRECATION 1
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>
#import <OpenGL/gl3ext.h>
#import <OpenGL/CGLIOSurface.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreGraphics/CoreGraphics.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include "rt.h"
#include "game.h"

uint64_t rt_dc_window(uint64_t dc);                       // user32.c
int rt_window_client_size(uint64_t h, int *cw, int *ch);  // window.c
void *rt_window_metal_layer(uint64_t h);                  // window.c

#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif
#define NSURF 3

// ---------------- debugging ----------------
static int gl_check = -1;
static void gl_check_err(const char *fn) {
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static struct { const char *fn; GLenum e; } seen[512]; static int nseen;
    pthread_mutex_lock(&mu);
    int known = 0;
    for (int i = 0; i < nseen; i++) if (seen[i].fn == fn && seen[i].e == e) known = 1;
    if (!known && nseen < 512) { seen[nseen].fn = fn; seen[nseen].e = e; nseen++; }
    pthread_mutex_unlock(&mu);
    if (!known) rt_log("[gl] %s -> error 0x%x\n", fn, e);
}
// No current context: Windows' opengl32 ignores the call (and returns 0); Apple's GL would dereference NULL.
static __thread struct GLCtx *cur;
static void gl_no_context(const char *fn) {
    static int n;
    if (n < 20) { n++; rt_log("[gl] %s called on thread %u with no current context: ignored\n", fn, rt_thread_id()); }
}
#define GL_PRE(f) do { if (__builtin_expect(!cur, 0)) { gl_no_context(#f); RET(0); return; } } while (0)
#define GL_POST(f) do { if (gl_check) gl_check_err(#f); } while (0)

// ---------------- drawables (one per window) and contexts ----------------
typedef struct Drawable {
    uint64_t hwnd; int w, h, srgb;
    IOSurfaceRef surf[NSURF]; GLuint tex[NSURF]; GLuint depth;
    int cur; uint32_t gen;
    void *layer;
    int layer_w, layer_h;                 // the drawable size last set on the layer
} Drawable;

typedef struct GLCtx {
    uint64_t handle; CGLContextObj cgl;
    GLuint fbo, vao;                      // stand-in for framebuffer 0, default vertex array
    Drawable *dr; uint32_t dr_gen;        // what fbo's attachments show
    int alpha_test;
    int profile_mask;                     // what the game asked for (GL_CONTEXT_PROFILE_MASK reports it back)
    GLuint *qmap; uint32_t qcap;          // virtual query name -> this context's real query object (see below)
    int cond_render;                      // a conditional render is active (it had a real query)
} GLCtx;

#define MAXCTX 64
#define MAXDR 16
static pthread_mutex_t gl_mu = PTHREAD_MUTEX_INITIALIZER;
static GLCtx *ctxs[MAXCTX];
static Drawable *drs[MAXDR];
static CGLPixelFormatObj pix;
static CGLContextObj share_root;
static __thread uint64_t cur_dc;
static int swap_interval = 1;   // WGL: 0 off, n > 0 sync, n < 0 adaptive (WGL_EXT_swap_control_tear)
static NSMutableArray *mtl_keep;          // MTLTextures of the drawables (index: drawable * NSURF + surface)
static id<MTLDevice> mtl_dev;
static id<MTLCommandQueue> mtl_queue;
static id<MTLRenderPipelineState> mtl_pipe;
static id<MTLCommandBuffer> mtl_pending[MAXDR][NSURF];

#define CTX_BASE 0x474C0000ull            // guest HGLRC values: CTX_BASE + index + 1

static GLCtx *ctx_get(uint64_t h) {
    if (h <= CTX_BASE || h > CTX_BASE + MAXCTX) return NULL;
    return ctxs[h - CTX_BASE - 1];
}

static CGLPixelFormatObj pixel_format(void) {
    if (pix) return pix;
    CGLPixelFormatAttribute a[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0 };
    GLint n = 0;
    CGLError e = CGLChoosePixelFormat(a, &pix, &n);
    if (e || !pix) rt_log("[gl] CGLChoosePixelFormat failed: %s\n", CGLErrorString(e));
    return pix;
}

static uint64_t ctx_create(int profile_mask) {
    pthread_mutex_lock(&gl_mu);
    int i = 0;
    while (i < MAXCTX && ctxs[i]) i++;
    if (i == MAXCTX || !pixel_format()) { pthread_mutex_unlock(&gl_mu); return 0; }
    CGLContextObj cgl = NULL;
    CGLError e = CGLCreateContext(pix, share_root, &cgl);
    if (e || !cgl) { pthread_mutex_unlock(&gl_mu); rt_log("[gl] CGLCreateContext failed: %s\n", CGLErrorString(e)); return 0; }
    if (!share_root) { share_root = cgl; CGLRetainContext(cgl); }
    GLCtx *x = calloc(1, sizeof *x);
    x->cgl = cgl; x->handle = CTX_BASE + i + 1; x->profile_mask = profile_mask;
    ctxs[i] = x;
    pthread_mutex_unlock(&gl_mu);
    if (gl_check < 0) gl_check = getenv("RT_GL_CHECK") != NULL;
    return x->handle;
}

// ---------------- Metal presentation ----------------
static void mtl_init(void) {
    if (mtl_dev) return;
    mtl_dev = MTLCreateSystemDefaultDevice();
    mtl_queue = [mtl_dev newCommandQueue];
    mtl_keep = [NSMutableArray array];
    for (int i = 0; i < MAXDR * NSURF; i++) [mtl_keep addObject:[NSNull null]];
    // full-screen triangle sampling the GL image; GL's bottom row is the IOSurface's first row, which Metal
    // samples at v = 0, so v grows upwards here and the picture comes out upright
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 q = float2((id << 1) & 2, id & 2); V o; o.p = float4(q * 2 - 1, 0, 1); o.uv = q; return o; }\n"
        "fragment float4 fs(V i [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
        "  constexpr sampler s(filter::linear, address::clamp_to_edge); return float4(t.sample(s, i.uv).rgb, 1); }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [mtl_dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) { rt_log("[gl] Metal present shader: %s\n", err.localizedDescription.UTF8String); return; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    mtl_pipe = [mtl_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!mtl_pipe) rt_log("[gl] Metal present pipeline: %s\n", err.localizedDescription.UTF8String);
}

static int dr_index(Drawable *d) { for (int i = 0; i < MAXDR; i++) if (drs[i] == d) return i; return -1; }

// Adaptive vsync (a negative swap interval, WGL_EXT_swap_control_tear): a frame that arrives within a refresh
// period of the last one waits for the display as usual; a late one is shown at once instead of holding on to the
// next refresh, so a game that just misses 60 doesn't fall to 30.
static double refresh_period(void) {
    static double period;
    if (!period) {
        double hz = 0;
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
        if (m) { hz = CGDisplayModeGetRefreshRate(m); CGDisplayModeRelease(m); }
        period = 1.0 / (hz >= 24 && hz <= 500 ? hz : 60.0);
    }
    return period;
}
static int present_synced(void) {
    if (swap_interval >= 0) return swap_interval != 0;
    static double last;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec / 1e9, dt = now - last;
    last = now;
    return dt <= refresh_period() * 1.25;
}
static void present(Drawable *d, int s) {
    @autoreleasepool {
        if (!mtl_pipe) return;
        if (!d->layer) d->layer = rt_window_metal_layer(d->hwnd);
        CAMetalLayer *L = (__bridge CAMetalLayer *)d->layer;
        if (!L) return;
        if (L.device != mtl_dev) { L.device = mtl_dev; L.pixelFormat = MTLPixelFormatBGRA8Unorm; L.framebufferOnly = YES; L.opaque = YES; }
        // only when our size changes: an injected overlay or scaler may resize the drawable itself
        if (d->layer_w != d->w || d->layer_h != d->h) { L.drawableSize = CGSizeMake(d->w, d->h); d->layer_w = d->w; d->layer_h = d->h; }
        L.displaySyncEnabled = present_synced();
        id<CAMetalDrawable> dr = [L nextDrawable];
        if (!dr) return;
        id<MTLCommandBuffer> cb = [mtl_queue commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dr.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:mtl_pipe];
        [enc setFragmentTexture:mtl_keep[dr_index(d) * NSURF + s] atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb presentDrawable:dr];
        [cb commit];
        mtl_pending[dr_index(d)][s] = cb;
    }
}

// ---------------- the emulated default framebuffer ----------------
static Drawable *drawable_for(uint64_t hwnd) {
    pthread_mutex_lock(&gl_mu);
    Drawable *d = NULL; int free_i = -1;
    for (int i = 0; i < MAXDR; i++) {
        if (drs[i] && drs[i]->hwnd == hwnd) { d = drs[i]; break; }
        if (!drs[i] && free_i < 0) free_i = i;
    }
    if (!d && free_i >= 0) { d = calloc(1, sizeof *d); d->hwnd = hwnd; drs[free_i] = d; }
    pthread_mutex_unlock(&gl_mu);
    return d;
}

// (re)create the drawable's buffers at the window's current size; the current context must be in the share group
static void drawable_alloc(Drawable *d, int w, int h) {
    mtl_init();
    int di = dr_index(d);
    for (int i = 0; i < NSURF; i++) if (mtl_pending[di][i]) { [mtl_pending[di][i] waitUntilCompleted]; mtl_pending[di][i] = nil; }
    GLint old_tex, old_rb;
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &old_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    if (d->tex[0]) glDeleteTextures(NSURF, d->tex);
    if (d->depth) glDeleteRenderbuffers(1, &d->depth);
    for (int i = 0; i < NSURF; i++) if (d->surf[i]) { CFRelease(d->surf[i]); d->surf[i] = NULL; }
    d->w = w; d->h = h;
    NSDictionary *props = @{ (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
    glGenTextures(NSURF, d->tex);
    d->srgb = 1;
    for (int i = 0; i < NSURF; i++) {
        d->surf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        glBindTexture(GL_TEXTURE_RECTANGLE, d->tex[i]);
        // sRGB-capable like the WGL pixel format we report; plain RGBA8 if the driver refuses
        CGLError e = d->srgb ? CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_SRGB8_ALPHA8, w, h,
                                                       GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, d->surf[i], 0) : kCGLBadValue;
        if (e) {
            if (d->srgb && i == 0) rt_log("[gl] sRGB IOSurface texture refused (%s), using RGBA8\n", CGLErrorString(e));
            d->srgb = 0;
            e = CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA8, w, h, GL_BGRA,
                                       GL_UNSIGNED_INT_8_8_8_8_REV, d->surf[i], 0);
            if (e) rt_log("[gl] CGLTexImageIOSurface2D failed: %s\n", CGLErrorString(e));
        }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        mtl_keep[di * NSURF + i] = [mtl_dev newTextureWithDescriptor:td iosurface:d->surf[i] plane:0];
    }
    glGenRenderbuffers(1, &d->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, d->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindTexture(GL_TEXTURE_RECTANGLE, old_tex);
    glBindRenderbuffer(GL_RENDERBUFFER, old_rb);
    d->cur = 0; d->gen++;
    rt_log("[gl] window 0x%llx drawable %dx%d%s\n", (unsigned long long)d->hwnd, w, h, d->srgb ? " (sRGB)" : "");
}

static void drawable_check_size(Drawable *d) {
    int w = 0, h = 0;
    if (!rt_window_client_size(d->hwnd, &w, &h) || w < 1 || h < 1) { w = d->w ? d->w : 640; h = d->h ? d->h : 480; }
    if (w != d->w || h != d->h || !d->tex[0]) drawable_alloc(d, w, h);
}

// point the context's stand-in FBO at the drawable's current color surface
static void ctx_attach(GLCtx *x) {
    Drawable *d = x->dr;
    if (!d || (x->dr_gen == d->gen && x->fbo)) return;
    GLint dfb, rfb;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    if (!x->fbo) glGenFramebuffers(1, &x->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, x->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, d->tex[d->cur], 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, d->depth);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) rt_log("[gl] default framebuffer incomplete: 0x%x\n", st);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rfb);
    x->dr_gen = d->gen;
}

static GLuint real_fb(GLuint fb) { return fb == 0 && cur && cur->fbo ? cur->fbo : fb; }
static GLint guest_fb(GLint fb) { return cur && cur->fbo && (GLuint)fb == cur->fbo ? 0 : fb; }
static int fb0_bound(GLenum which) {
    if (!cur || !cur->fbo) return 0;
    GLint b; glGetIntegerv(which, &b);
    return (GLuint)b == cur->fbo;
}
static GLenum window_buffer(GLenum b) {   // GL_BACK & co. on the stand-in FBO
    switch (b) {
    case GL_BACK: case GL_BACK_LEFT: case GL_FRONT: case GL_FRONT_LEFT: case GL_FRONT_AND_BACK: case GL_LEFT:
        return GL_COLOR_ATTACHMENT0;
    }
    return b;
}

// ---------------- extension strings ----------------
// what the 4.1 core profile subsumes, listed for games that check strings rather than the version
static const char *const core_exts[] = {
    "GL_ARB_multitexture", "GL_ARB_texture_compression", "GL_ARB_texture_cube_map", "GL_ARB_texture_border_clamp",
    "GL_ARB_depth_texture", "GL_ARB_shadow", "GL_ARB_texture_mirrored_repeat", "GL_ARB_point_parameters",
    "GL_ARB_occlusion_query", "GL_ARB_vertex_buffer_object", "GL_ARB_pixel_buffer_object", "GL_ARB_texture_non_power_of_two",
    "GL_ARB_draw_buffers", "GL_ARB_texture_rectangle", "GL_ARB_texture_float", "GL_ARB_half_float_pixel",
    "GL_ARB_half_float_vertex", "GL_ARB_color_buffer_float", "GL_ARB_depth_buffer_float", "GL_ARB_framebuffer_object",
    "GL_ARB_framebuffer_sRGB", "GL_ARB_map_buffer_range", "GL_ARB_texture_rg", "GL_ARB_vertex_array_object",
    "GL_ARB_texture_compression_rgtc", "GL_ARB_instanced_arrays", "GL_ARB_draw_instanced", "GL_ARB_copy_buffer",
    "GL_ARB_uniform_buffer_object", "GL_ARB_texture_buffer_object", "GL_ARB_draw_elements_base_vertex",
    "GL_ARB_provoking_vertex", "GL_ARB_seamless_cube_map", "GL_ARB_sync", "GL_ARB_texture_multisample",
    "GL_ARB_depth_clamp", "GL_ARB_fragment_coord_conventions", "GL_ARB_vertex_array_bgra", "GL_ARB_blend_func_extended",
    "GL_ARB_explicit_attrib_location", "GL_ARB_occlusion_query2", "GL_ARB_sampler_objects", "GL_ARB_shader_bit_encoding",
    "GL_ARB_texture_rgb10_a2ui", "GL_ARB_texture_swizzle", "GL_ARB_timer_query", "GL_ARB_vertex_type_2_10_10_10_rev",
    "GL_ARB_draw_indirect", "GL_ARB_gpu_shader5", "GL_ARB_gpu_shader_fp64", "GL_ARB_sample_shading",
    "GL_ARB_shader_subroutine", "GL_ARB_tessellation_shader", "GL_ARB_texture_buffer_object_rgb32",
    "GL_ARB_texture_cube_map_array", "GL_ARB_texture_gather", "GL_ARB_texture_query_lod", "GL_ARB_transform_feedback2",
    "GL_ARB_transform_feedback3", "GL_ARB_ES2_compatibility", "GL_ARB_get_program_binary", "GL_ARB_separate_shader_objects",
    "GL_ARB_shader_precision", "GL_ARB_vertex_attrib_64bit", "GL_ARB_viewport_array",
    "GL_EXT_framebuffer_object", "GL_EXT_framebuffer_blit", "GL_EXT_framebuffer_multisample", "GL_EXT_packed_depth_stencil",
    "GL_EXT_texture_sRGB", "GL_EXT_texture_array", "GL_EXT_texture_integer", "GL_EXT_draw_buffers2",
    "GL_EXT_transform_feedback", "GL_EXT_blend_func_separate", "GL_EXT_blend_equation_separate", "GL_EXT_blend_minmax",
    "GL_EXT_blend_subtract", "GL_EXT_blend_color", "GL_EXT_stencil_wrap", "GL_EXT_texture3D", "GL_EXT_texture_lod_bias",
    "GL_EXT_bgra", "GL_EXT_packed_float", "GL_EXT_texture_shared_exponent", "GL_EXT_texture_snorm", "GL_EXT_gpu_shader4",
    "GL_EXT_draw_range_elements", "GL_EXT_texture_swizzle", "GL_EXT_timer_query", "GL_EXT_provoking_vertex",
    "GL_EXT_texture_edge_clamp", "GL_EXT_draw_instanced", "GL_EXT_copy_texture", "GL_EXT_subtexture", "GL_EXT_texture_object",
    "GL_EXT_vertex_array_bgra", "GL_EXT_texture_rectangle",
};
static char *ext_string;
static const char **ext_list; static int next;
static void exts_build(void) {
    if (ext_string) return;
    GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    // the game's own additions (rt_game.gl_extensions), e.g. GL_ARB_vertex_program: not core, but its vertex-attribute
    // half is (glVertexAttrib*ARB); its assembly-program half is stubbed and logs if used
    char *extra = strdup(rt_game.gl_extensions ? rt_game.gl_extensions : "");
    int nextra = 0; for (char *t = strtok(extra, " "); t; t = strtok(NULL, " ")) nextra++;
    free(extra);
    int cap = n + (int)(sizeof core_exts / sizeof *core_exts) + nextra;
    const char **l = calloc(cap, sizeof *l); int k = 0;
    for (GLint i = 0; i < n; i++) l[k++] = (const char *)glGetStringi(GL_EXTENSIONS, i);
    for (size_t j = 0; j < sizeof core_exts / sizeof *core_exts; j++) {
        int dup = 0;
        for (int i = 0; i < k; i++) if (!strcmp(l[i], core_exts[j])) dup = 1;
        if (!dup) l[k++] = core_exts[j];
    }
    if (rt_game.gl_extensions) {
        char *e = strdup(rt_game.gl_extensions);   // kept: the list points into it
        for (char *t = strtok(e, " "); t; t = strtok(NULL, " ")) l[k++] = t;
    }
    size_t len = 1;
    for (int i = 0; i < k; i++) len += strlen(l[i]) + 1;
    char *s = malloc(len); s[0] = 0;
    for (int i = 0; i < k; i++) { strcat(s, l[i]); strcat(s, " "); }
    ext_list = l; next = k;
    __atomic_store_n(&ext_string, s, __ATOMIC_RELEASE);
    rt_log("[gl] %s / %s / %s, %d extensions (%d from the host)\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
           glGetString(GL_VERSION), k, n);
}

// ---------------- hand-written GL entry points ----------------
void I_opengl32_dll_glGetString(CPU *c) { GL_PRE(glGetString);
    if ((GLenum)A0 == GL_EXTENSIONS) { exts_build(); RET(G(ext_string)); return; }
    RET(G(glGetString((GLenum)A0)));
}
void I_opengl32_dll_glGetStringi(CPU *c) { GL_PRE(glGetStringi);
    if ((GLenum)A0 == GL_EXTENSIONS) { exts_build(); RET((uint32_t)A1 < (uint32_t)next ? G(ext_list[(uint32_t)A1]) : 0); return; }
    RET(G(glGetStringi((GLenum)A0, (GLuint)A1)));
}
// state queries: hide the stand-in FBO and default VAO, report the full extension count
static int fix_int(GLenum p, int64_t *v) {
    switch (p) {
    case GL_DRAW_FRAMEBUFFER_BINDING: case GL_READ_FRAMEBUFFER_BINDING: *v = guest_fb((GLint)*v); return 1;
    case GL_VERTEX_ARRAY_BINDING: if (cur && (GLuint)*v == cur->vao) *v = 0; return 1;
    case GL_NUM_EXTENSIONS: exts_build(); *v = next; return 1;
    case GL_DRAW_BUFFER: case GL_DRAW_BUFFER0:
        if (*v == GL_COLOR_ATTACHMENT0 && fb0_bound(GL_DRAW_FRAMEBUFFER_BINDING)) *v = GL_BACK; return 1;
    case GL_READ_BUFFER:
        if (*v == GL_COLOR_ATTACHMENT0 && fb0_bound(GL_READ_FRAMEBUFFER_BINDING)) *v = GL_BACK; return 1;
    case GL_CONTEXT_PROFILE_MASK: if (cur) *v = cur->profile_mask; return 1;   // what the game asked for
    }
    return 0;
}
void I_opengl32_dll_glGetIntegerv(CPU *c) { GL_PRE(glGetIntegerv);
    GLenum p = (GLenum)A0; GLint *out = PTR(A1);
    glGetIntegerv(p, out);
    int64_t v = *out;
    if (fix_int(p, &v)) *out = (GLint)v;
    GL_POST(glGetIntegerv);
}
void I_opengl32_dll_glGetInteger64v(CPU *c) { GL_PRE(glGetInteger64v);
    GLenum p = (GLenum)A0; GLint64 *out = PTR(A1);
    glGetInteger64v(p, out);
    int64_t v = *out;
    if (fix_int(p, &v)) *out = v;
    GL_POST(glGetInteger64v);
}
void I_opengl32_dll_glGetBooleanv(CPU *c) { GL_PRE(glGetBooleanv); glGetBooleanv((GLenum)A0, PTR(A1)); GL_POST(glGetBooleanv); }
void I_opengl32_dll_glGetFloatv(CPU *c) { GL_PRE(glGetFloatv); glGetFloatv((GLenum)A0, PTR(A1)); GL_POST(glGetFloatv); }
void I_opengl32_dll_glGetDoublev(CPU *c) { GL_PRE(glGetDoublev); glGetDoublev((GLenum)A0, PTR(A1)); GL_POST(glGetDoublev); }

void I_opengl32_dll_glBindFramebuffer(CPU *c) { GL_PRE(glBindFramebuffer);
    GLuint fb = (GLuint)A1;
    if (fb == 0 && cur) ctx_attach(cur);
    glBindFramebuffer((GLenum)A0, real_fb(fb));
    GL_POST(glBindFramebuffer);
}
void I_opengl32_dll_glDrawBuffer(CPU *c) { GL_PRE(glDrawBuffer);
    GLenum b = (GLenum)A0;
    glDrawBuffer(fb0_bound(GL_DRAW_FRAMEBUFFER_BINDING) ? window_buffer(b) : b);
    GL_POST(glDrawBuffer);
}
void I_opengl32_dll_glReadBuffer(CPU *c) { GL_PRE(glReadBuffer);
    GLenum b = (GLenum)A0;
    glReadBuffer(fb0_bound(GL_READ_FRAMEBUFFER_BINDING) ? window_buffer(b) : b);
    GL_POST(glReadBuffer);
}
void I_opengl32_dll_glDrawBuffers(CPU *c) { GL_PRE(glDrawBuffers);
    GLsizei n = (GLsizei)A0; const GLenum *b = PTR(A1);
    if (n > 0 && n <= 16 && fb0_bound(GL_DRAW_FRAMEBUFFER_BINDING)) {
        GLenum m[16];
        for (GLsizei i = 0; i < n; i++) m[i] = window_buffer(b[i]);
        glDrawBuffers(n, m);
    } else glDrawBuffers(n, b);
    GL_POST(glDrawBuffers);
}
void I_opengl32_dll_glGetFramebufferAttachmentParameteriv(CPU *c) { GL_PRE(glGetFramebufferAttachmentParameteriv);
    GLenum t = (GLenum)A0, a = (GLenum)A1;
    GLenum which = t == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING;
    if (fb0_bound(which)) {
        if (a == 0x1801) a = GL_DEPTH_ATTACHMENT;          // GL_DEPTH
        else if (a == 0x1802) a = GL_STENCIL_ATTACHMENT;   // GL_STENCIL
        else a = window_buffer(a);
    }
    glGetFramebufferAttachmentParameteriv(t, a, (GLenum)A2, PTR(A3));
    GL_POST(glGetFramebufferAttachmentParameteriv);
}

void I_opengl32_dll_glBindVertexArray(CPU *c) { GL_PRE(glBindVertexArray);
    GLuint v = (GLuint)A0;
    glBindVertexArray(v == 0 && cur ? cur->vao : v);
    GL_POST(glBindVertexArray);
}
void I_opengl32_dll_glDeleteVertexArrays(CPU *c) { GL_PRE(glDeleteVertexArrays);
    glDeleteVertexArrays((GLsizei)A0, PTR(A1));
    GLint b = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &b);
    if (!b && cur) glBindVertexArray(cur->vao);   // deleting the bound array reverts to 0: ours
    GL_POST(glDeleteVertexArrays);
}
void I_opengl32_dll_glPolygonMode(CPU *c) { GL_PRE(glPolygonMode); glPolygonMode(GL_FRONT_AND_BACK, (GLenum)A1); GL_POST(glPolygonMode); }
void I_opengl32_dll_glLineWidth(CPU *c) { GL_PRE(glLineWidth); glLineWidth(1.0f); GL_POST(glLineWidth); }   // core: wide lines are an error

// capabilities the core profile removed: accepted and ignored
static int legacy_cap(GLenum cap) {
    switch (cap) {
    case 0x0BC0: /* ALPHA_TEST */ case 0x0DE0: /* TEXTURE_1D */ case 0x0DE1: /* TEXTURE_2D */ case 0x8513: /* TEXTURE_CUBE_MAP */
    case 0x0B50: /* LIGHTING */ case 0x0B60: /* FOG */ case 0x0BA1: /* NORMALIZE */ case 0x0B57: /* COLOR_MATERIAL */
    case 0x0B42: /* POLYGON_STIPPLE */ case 0x0B24: /* LINE_STIPPLE */ case 0x8861: /* POINT_SPRITE */
    case 0x8890: /* DEPTH_BOUNDS_TEST_EXT */ case 0x0C60: case 0x0C61: case 0x0C62: case 0x0C63: /* TEXTURE_GEN_* */
    case 0x8620: /* VERTEX_PROGRAM_ARB */ case 0x8804: /* FRAGMENT_PROGRAM_ARB */ case 0x803A: /* RESCALE_NORMAL */
        return 1;
    }
    return 0;
}
void I_opengl32_dll_glEnable(CPU *c) { GL_PRE(glEnable);
    GLenum cap = (GLenum)A0;
    if (cap == 0x0BC0 && cur) cur->alpha_test = 1;
    if (!legacy_cap(cap)) glEnable(cap);
    GL_POST(glEnable);
}
void I_opengl32_dll_glDisable(CPU *c) { GL_PRE(glDisable);
    GLenum cap = (GLenum)A0;
    if (cap == 0x0BC0 && cur) cur->alpha_test = 0;
    if (!legacy_cap(cap)) glDisable(cap);
    GL_POST(glDisable);
}
void I_opengl32_dll_glIsEnabled(CPU *c) { GL_PRE(glIsEnabled);
    GLenum cap = (GLenum)A0;
    if (cap == 0x0BC0) { RET(cur && cur->alpha_test); return; }
    RET(legacy_cap(cap) ? 0 : glIsEnabled(cap));
    GL_POST(glIsEnabled);
}

// shaders: optional dump, and the compile / link log when it fails (the GLSL dialect is the likeliest gap)
static int shader_seq;
void I_opengl32_dll_glShaderSource(CPU *c) { GL_PRE(glShaderSource);
    GLuint sh = (GLuint)A0; GLsizei n = (GLsizei)A1; const GLchar *const *s = PTR(A2); const GLint *len = PTR(A3);
    const char *dir = getenv("RT_GL_SHADERS");
    if (dir) {
        char path[1024]; snprintf(path, sizeof path, "%s/%05d_%u.glsl", dir, __atomic_add_fetch(&shader_seq, 1, __ATOMIC_RELAXED), sh);
        FILE *f = fopen(path, "w");
        if (f) { for (GLsizei i = 0; i < n; i++) fwrite(s[i], 1, len && len[i] >= 0 ? (size_t)len[i] : strlen(s[i]), f); fclose(f); }
    }
    glShaderSource(sh, n, s, len);
    GL_POST(glShaderSource);
}
void I_opengl32_dll_glCompileShader(CPU *c) { GL_PRE(glCompileShader);
    GLuint sh = (GLuint)A0;
    glCompileShader(sh);
    GLint ok = 1; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        static int n; if (n++ < 20) {
            char log[2048] = "", src[600] = ""; GLsizei l;
            glGetShaderInfoLog(sh, sizeof log, &l, log); glGetShaderSource(sh, sizeof src, &l, src);
            rt_log("[gl] shader %u failed to compile:\n%s\n--- source begins:\n%s\n---\n", sh, log, src);
        }
    }
    GL_POST(glCompileShader);
}
void I_opengl32_dll_glLinkProgram(CPU *c) { GL_PRE(glLinkProgram);
    GLuint p = (GLuint)A0;
    glLinkProgram(p);
    GLint ok = 1; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { static int n; if (n++ < 20) { char log[2048] = ""; GLsizei l; glGetProgramInfoLog(p, sizeof log, &l, log); rt_log("[gl] program %u failed to link:\n%s\n", p, log); } }
    GL_POST(glLinkProgram);
}
void I_opengl32_dll_glGetError(CPU *c) { GL_PRE(glGetError); RET(glGetError()); }
void I_opengl32_dll_glFlush(CPU *c) { GL_PRE(glFlush); glFlush(); }
void I_opengl32_dll_glFinish(CPU *c) { GL_PRE(glFinish); glFinish(); }

// ---- legacy texture formats: ALPHA / LUMINANCE / LUMINANCE_ALPHA / INTENSITY (removed from core) ----
// stored as R / RG textures with a swizzle that gives back the old channel meaning
enum { SW_NONE, SW_ALPHA, SW_LUM, SW_LUMA, SW_INT };
static GLint legacy_internal(GLint f, int *sw) {
    *sw = SW_NONE;
    switch (f) {
    case 0x1906: case 0x803C: *sw = SW_ALPHA; return GL_R8;          // ALPHA, ALPHA8
    case 0x803E: *sw = SW_ALPHA; return GL_R16;                       // ALPHA16
    case 0x881C: *sw = SW_ALPHA; return GL_R16F;                      // ALPHA16F
    case 0x8816: *sw = SW_ALPHA; return GL_R32F;                      // ALPHA32F
    case 1: case 0x1909: case 0x8040: *sw = SW_LUM; return GL_R8;     // LUMINANCE, LUMINANCE8
    case 0x8042: *sw = SW_LUM; return GL_R16;                         // LUMINANCE16
    case 0x881E: *sw = SW_LUM; return GL_R16F;                        // LUMINANCE16F
    case 0x8818: *sw = SW_LUM; return GL_R32F;                        // LUMINANCE32F
    case 2: case 0x190A: case 0x8045: *sw = SW_LUMA; return GL_RG8;   // LUMINANCE_ALPHA, LUMINANCE8_ALPHA8
    case 0x8048: *sw = SW_LUMA; return GL_RG16;                       // LUMINANCE16_ALPHA16
    case 0x881F: *sw = SW_LUMA; return GL_RG16F;                      // LUMINANCE_ALPHA16F
    case 0x8819: *sw = SW_LUMA; return GL_RG32F;                      // LUMINANCE_ALPHA32F
    case 0x8049: case 0x804B: *sw = SW_INT; return GL_R8;             // INTENSITY, INTENSITY8
    case 0x804D: *sw = SW_INT; return GL_R16;                         // INTENSITY16
    case 0x881D: *sw = SW_INT; return GL_R16F;                        // INTENSITY16F
    case 0x8817: *sw = SW_INT; return GL_R32F;                        // INTENSITY32F
    case 3: return GL_RGB8;
    case 4: return GL_RGBA8;
    }
    return f;
}
static GLenum legacy_format(GLenum f) {
    switch (f) {
    case 0x1906: case 0x1909: return GL_RED;   // ALPHA, LUMINANCE
    case 0x190A: return GL_RG;                 // LUMINANCE_ALPHA
    }
    return f;
}
static GLenum param_target(GLenum t) {        // cube faces take their parameters on the cube map
    return t >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && t <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? GL_TEXTURE_CUBE_MAP : t;
}
static void set_swizzle(GLenum target, int sw) {
    static const GLint m[5][4] = {
        { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA }, { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED }, { GL_RED, GL_RED, GL_RED, GL_ONE },
        { GL_RED, GL_RED, GL_RED, GL_GREEN }, { GL_RED, GL_RED, GL_RED, GL_RED } };
    if (target == 0x8063 || target == 0x8064 || target == 0x8070 || target == 0x8C1B /* proxies */) return;
    glTexParameteriv(param_target(target), GL_TEXTURE_SWIZZLE_RGBA, m[sw]);
}
// glTexImage*D: (target, level, internalformat, w[, h[, d]], border, format, type, pixels)
void I_opengl32_dll_glTexImage1D(CPU *c) { GL_PRE(glTexImage1D);
    int sw; GLint in = legacy_internal((GLint)A2, &sw);
    glTexImage1D((GLenum)A0, (GLint)A1, in, (GLsizei)A3, (GLint)AN(4), legacy_format((GLenum)AN(5)), (GLenum)AN(6), PTR(AN(7)));
    if ((GLint)A1 == 0) set_swizzle((GLenum)A0, sw);
    GL_POST(glTexImage1D); }
void I_opengl32_dll_glTexImage2D(CPU *c) { GL_PRE(glTexImage2D);
    int sw; GLint in = legacy_internal((GLint)A2, &sw);
    glTexImage2D((GLenum)A0, (GLint)A1, in, (GLsizei)A3, (GLsizei)AN(4), (GLint)AN(5), legacy_format((GLenum)AN(6)), (GLenum)AN(7), PTR(AN(8)));
    if ((GLint)A1 == 0) set_swizzle((GLenum)A0, sw);
    GL_POST(glTexImage2D); }
void I_opengl32_dll_glTexImage3D(CPU *c) { GL_PRE(glTexImage3D);
    int sw; GLint in = legacy_internal((GLint)A2, &sw);
    glTexImage3D((GLenum)A0, (GLint)A1, in, (GLsizei)A3, (GLsizei)AN(4), (GLsizei)AN(5), (GLint)AN(6), legacy_format((GLenum)AN(7)), (GLenum)AN(8), PTR(AN(9)));
    if ((GLint)A1 == 0) set_swizzle((GLenum)A0, sw);
    GL_POST(glTexImage3D); }
// glTexSubImage*D: (target, level, x[, y[, z]], w[, h[, d]], format, type, pixels)
void I_opengl32_dll_glTexSubImage1D(CPU *c) { GL_PRE(glTexSubImage1D);
    glTexSubImage1D((GLenum)A0, (GLint)A1, (GLint)A2, (GLsizei)A3, legacy_format((GLenum)AN(4)), (GLenum)AN(5), PTR(AN(6)));
    GL_POST(glTexSubImage1D); }
void I_opengl32_dll_glTexSubImage2D(CPU *c) { GL_PRE(glTexSubImage2D);
    glTexSubImage2D((GLenum)A0, (GLint)A1, (GLint)A2, (GLint)A3, (GLsizei)AN(4), (GLsizei)AN(5), legacy_format((GLenum)AN(6)), (GLenum)AN(7), PTR(AN(8)));
    GL_POST(glTexSubImage2D); }
void I_opengl32_dll_glTexSubImage3D(CPU *c) { GL_PRE(glTexSubImage3D);
    glTexSubImage3D((GLenum)A0, (GLint)A1, (GLint)A2, (GLint)A3, (GLint)AN(4), (GLsizei)AN(5), (GLsizei)AN(6), (GLsizei)AN(7),
                    legacy_format((GLenum)AN(8)), (GLenum)AN(9), PTR(AN(10)));
    GL_POST(glTexSubImage3D); }
// glReadPixels(x, y, w, h, format, type, data) / glGetTexImage(target, level, format, type, pixels)
void I_opengl32_dll_glReadPixels(CPU *c) { GL_PRE(glReadPixels);
    glReadPixels((GLint)A0, (GLint)A1, (GLsizei)A2, (GLsizei)A3, legacy_format((GLenum)AN(4)), (GLenum)AN(5), PTR(AN(6)));
    GL_POST(glReadPixels); }
void I_opengl32_dll_glGetTexImage(CPU *c) { GL_PRE(glGetTexImage);
    glGetTexImage((GLenum)A0, (GLint)A1, legacy_format((GLenum)A2), (GLenum)A3, PTR(AN(4)));
    GL_POST(glGetTexImage); }

// ---- query objects: one name space for all contexts ----
// Query objects are not shared between GL contexts, but Windows drivers share them across wglShareLists'd contexts
// and engines rely on it (id Tech 5 creates its occlusion queries on the main thread's context and issues them on the
// renderer's). The guest gets virtual names from one counter; each context makes its own real object for a name the
// first time it uses it. A name read on a context that never issued it reads as finished and visible (draw rather than
// cull).
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t q_next = 1;                 // next virtual name
static uint8_t *q_live; static uint32_t q_livecap;   // 1 = generated and not deleted
static GLuint q_real(GLuint v, int create) {
    if (!v || !cur) return 0;
    if (v >= cur->qcap) {
        if (!create) return 0;
        uint32_t cap = cur->qcap ? cur->qcap : 1024;
        while (cap <= v) cap *= 2;
        cur->qmap = realloc(cur->qmap, cap * sizeof *cur->qmap);
        memset(cur->qmap + cur->qcap, 0, (cap - cur->qcap) * sizeof *cur->qmap);
        cur->qcap = cap;
    }
    if (!cur->qmap[v] && create) glGenQueries(1, &cur->qmap[v]);
    return cur->qmap[v];
}
static void q_trace(const char *fn, GLenum target, GLuint id) {
    if (!gl_check) return;
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) return;
    static int n; if (n >= 40) return; n++;
    rt_log("[gl] %s(target 0x%x, query %u) -> 0x%x on ctx 0x%llx thread %u\n", fn, target, id, e,
           (unsigned long long)(cur ? cur->handle : 0), rt_thread_id());
}
void I_opengl32_dll_glGenQueries(CPU *c) { GL_PRE(glGenQueries);
    GLsizei n = (GLsizei)A0; uint64_t out = A1;
    pthread_mutex_lock(&q_mu);
    for (GLsizei i = 0; i < n; i++) {
        uint32_t v = q_next++;
        if (v >= q_livecap) {
            uint32_t cap = q_livecap ? q_livecap * 2 : 4096; while (cap <= v) cap *= 2;
            q_live = realloc(q_live, cap); memset(q_live + q_livecap, 0, cap - q_livecap); q_livecap = cap;
        }
        q_live[v] = 1;
        wr32(out + 4ull * i, v);
    }
    pthread_mutex_unlock(&q_mu);
}
void I_opengl32_dll_glDeleteQueries(CPU *c) { GL_PRE(glDeleteQueries);
    GLsizei n = (GLsizei)A0; uint64_t ids = A1;
    for (GLsizei i = 0; i < n; i++) {
        GLuint v = rd32(ids + 4ull * i), r = q_real(v, 0);
        if (r) { glDeleteQueries(1, &r); cur->qmap[v] = 0; }   // other contexts' objects for v are left (small leak)
        pthread_mutex_lock(&q_mu); if (v < q_livecap) q_live[v] = 0; pthread_mutex_unlock(&q_mu);
    }
}
void I_opengl32_dll_glIsQuery(CPU *c) { GLuint v = (GLuint)A0; pthread_mutex_lock(&q_mu); RET(v && v < q_livecap && q_live[v]); pthread_mutex_unlock(&q_mu); }
void I_opengl32_dll_glBeginQuery(CPU *c) { GL_PRE(glBeginQuery);
    GLuint v = (GLuint)A1;
    glBeginQuery((GLenum)A0, q_real(v, 1)); q_trace("glBeginQuery", (GLenum)A0, v); }
void I_opengl32_dll_glBeginQueryIndexed(CPU *c) { GL_PRE(glBeginQueryIndexed);
    GLuint v = (GLuint)A2;
    glBeginQueryIndexed((GLenum)A0, (GLuint)A1, q_real(v, 1)); q_trace("glBeginQueryIndexed", (GLenum)A0, v); }
void I_opengl32_dll_glEndQuery(CPU *c) { GL_PRE(glEndQuery); glEndQuery((GLenum)A0); q_trace("glEndQuery", (GLenum)A0, 0); }
void I_opengl32_dll_glQueryCounter(CPU *c) { GL_PRE(glQueryCounter);
    GLuint v = (GLuint)A0;
    glQueryCounter(q_real(v, 1), (GLenum)A1); q_trace("glQueryCounter", (GLenum)A1, v); }
// result of a name this context never issued: finished, and (for occlusion) visible
static int q_unissued(GLenum pname, uint64_t out, int wide) {
    uint64_t val = pname == GL_QUERY_RESULT_AVAILABLE ? 1 : pname == GL_QUERY_RESULT ? 1 : 0;
    if (wide) wr64(out, val); else wr32(out, (uint32_t)val);
    return 1;
}
#define Q_GET(fn, T, wide) void I_opengl32_dll_##fn(CPU *c) { GL_PRE(fn); \
    GLuint v = (GLuint)A0, r = q_real(v, 0); \
    if (!r) { q_unissued((GLenum)A1, A2, wide); return; } \
    fn(r, (GLenum)A1, (T *)PTR(A2)); q_trace(#fn, (GLenum)A1, v); }
Q_GET(glGetQueryObjectuiv, GLuint, 0)
Q_GET(glGetQueryObjectiv, GLint, 0)
Q_GET(glGetQueryObjectui64v, GLuint64, 1)
Q_GET(glGetQueryObjecti64v, GLint64, 1)
// conditional rendering on a name this context never issued: draw unconditionally
void I_opengl32_dll_glBeginConditionalRender(CPU *c) { GL_PRE(glBeginConditionalRender);
    GLuint r = q_real((GLuint)A0, 0);
    cur->cond_render = r != 0;
    if (r) glBeginConditionalRender(r, (GLenum)A1);
    GL_POST(glBeginConditionalRender); }
void I_opengl32_dll_glEndConditionalRender(CPU *c) { GL_PRE(glEndConditionalRender);
    if (cur->cond_render) glEndConditionalRender();
    cur->cond_render = 0;
    GL_POST(glEndConditionalRender); }

// fixed-function calls the core profile does not have
void I_opengl32_dll_glShadeModel(CPU *c) { }
void I_opengl32_dll_glAlphaFunc(CPU *c) { }
void I_opengl32_dll_glColor4f(CPU *c) { }
void I_opengl32_dll_glMatrixMode(CPU *c) { }
void I_opengl32_dll_glLoadIdentity(CPU *c) { }
void I_opengl32_dll_glTexEnvi(CPU *c) { }

// ARB_vertex_program's assembly programs: no core equivalent. Logged so a real use shows up.
static void arbprog_used(const char *fn) {
    static int n; if (n < 10) { n++; rt_log("[gl] %s (ARB assembly program) called: not supported\n", fn); }
}
void I_opengl32_dll_glGenProgramsARB(CPU *c) {
    static uint32_t next_id = 1; arbprog_used("glGenProgramsARB");
    for (int i = 0; i < (int)A0; i++) wr32(A1 + 4 * i, next_id++);
}
void I_opengl32_dll_glDeleteProgramsARB(CPU *c) { }
void I_opengl32_dll_glBindProgramARB(CPU *c) { if (A1) arbprog_used("glBindProgramARB"); }
void I_opengl32_dll_glProgramStringARB(CPU *c) { arbprog_used("glProgramStringARB"); }
void I_opengl32_dll_glIsProgramARB(CPU *c) { RET(0); }
void I_opengl32_dll_glProgramParameterARB(CPU *c) { }   // glProgram{Env,Local}Parameter4{f,d}[v]ARB
void I_opengl32_dll_glGetProgramARB(CPU *c) { }         // glGetProgram{iv,String,EnvParameter*,LocalParameter*}ARB

#include "gl_thunks.h"

static const struct { const char *name; gfn fn; } gl_legacy[] = {
    { "glShadeModel", I_opengl32_dll_glShadeModel }, { "glAlphaFunc", I_opengl32_dll_glAlphaFunc },
    { "glColor4f", I_opengl32_dll_glColor4f }, { "glMatrixMode", I_opengl32_dll_glMatrixMode },
    { "glLoadIdentity", I_opengl32_dll_glLoadIdentity }, { "glTexEnvi", I_opengl32_dll_glTexEnvi },
    { "glGenProgramsARB", I_opengl32_dll_glGenProgramsARB }, { "glDeleteProgramsARB", I_opengl32_dll_glDeleteProgramsARB },
    { "glBindProgramARB", I_opengl32_dll_glBindProgramARB }, { "glProgramStringARB", I_opengl32_dll_glProgramStringARB },
    { "glIsProgramARB", I_opengl32_dll_glIsProgramARB },
    { "glProgramEnvParameter4dARB", I_opengl32_dll_glProgramParameterARB }, { "glProgramEnvParameter4dvARB", I_opengl32_dll_glProgramParameterARB },
    { "glProgramEnvParameter4fARB", I_opengl32_dll_glProgramParameterARB }, { "glProgramEnvParameter4fvARB", I_opengl32_dll_glProgramParameterARB },
    { "glProgramLocalParameter4dARB", I_opengl32_dll_glProgramParameterARB }, { "glProgramLocalParameter4dvARB", I_opengl32_dll_glProgramParameterARB },
    { "glProgramLocalParameter4fARB", I_opengl32_dll_glProgramParameterARB }, { "glProgramLocalParameter4fvARB", I_opengl32_dll_glProgramParameterARB },
    { "glGetProgramivARB", I_opengl32_dll_glGetProgramARB }, { "glGetProgramStringARB", I_opengl32_dll_glGetProgramARB },
    { "glGetProgramEnvParameterdvARB", I_opengl32_dll_glGetProgramARB }, { "glGetProgramEnvParameterfvARB", I_opengl32_dll_glGetProgramARB },
    { "glGetProgramLocalParameterdvARB", I_opengl32_dll_glGetProgramARB }, { "glGetProgramLocalParameterfvARB", I_opengl32_dll_glGetProgramARB },
};

// ---------------- WGL ----------------
static void make_current(uint64_t dc, GLCtx *x) {
    if (!x) { CGLSetCurrentContext(NULL); cur = NULL; cur_dc = 0; return; }
    CGLSetCurrentContext(x->cgl);
    cur = x; cur_dc = dc;
    uint64_t hwnd = rt_dc_window(dc);
    int first = !x->vao;
    if (first) { glGenVertexArrays(1, &x->vao); glBindVertexArray(x->vao); }
    if (hwnd) {
        Drawable *d = drawable_for(hwnd);
        if (d) { drawable_check_size(d); if (x->dr != d) { x->dr = d; x->dr_gen = 0; } ctx_attach(x); }
    }
    if (first && x->fbo) glBindFramebuffer(GL_FRAMEBUFFER, x->fbo);   // a new context starts on framebuffer 0
}

void I_opengl32_dll_wglCreateContext(CPU *c) {
    uint64_t h = ctx_create(2);   // a legacy context is a compatibility one
    if (gl_check > 0) rt_log("[gl] wglCreateContext(dc 0x%llx) -> 0x%llx on thread %u\n", (unsigned long long)A0, (unsigned long long)h, rt_thread_id());
    RET(h);
}
void I_opengl32_dll_wglDeleteContext(CPU *c) {
    GLCtx *x = ctx_get(A0);
    if (!x) { RET(0); return; }
    if (cur == x) make_current(0, NULL);
    pthread_mutex_lock(&gl_mu);
    ctxs[A0 - CTX_BASE - 1] = NULL;
    pthread_mutex_unlock(&gl_mu);
    CGLReleaseContext(x->cgl);   // the share root keeps its own reference
    free(x);
    RET(1);
}
void I_opengl32_dll_wglMakeCurrent(CPU *c) {
    if (gl_check > 0) rt_log("[gl] wglMakeCurrent(dc 0x%llx, ctx 0x%llx) on thread %u\n", (unsigned long long)A0, (unsigned long long)A1, rt_thread_id());
    if (!A1) { make_current(0, NULL); RET(1); return; }
    GLCtx *x = ctx_get(A1);
    if (!x) { set_last_error(2000); RET(0); return; }   // ERROR_INVALID_PIXEL_FORMAT
    make_current(A0, x);
    RET(1);
}
void I_opengl32_dll_wglGetCurrentContext(CPU *c) { RET(cur ? cur->handle : 0); }
void I_opengl32_dll_wglGetCurrentDC(CPU *c) { RET(cur ? cur_dc : 0); }
void I_opengl32_dll_wglShareLists(CPU *c) { RET(1); }   // every context is in one share group already

// RT_GL_SCREENSHOT=n: frame n of the window to RT_GL_SCREENSHOT_FILE (default /tmp/gl_frame.ppm), as GL drew it
static void screenshot(Drawable *d, int s) {
    const char *path = getenv("RT_GL_SCREENSHOT_FILE"); if (!path) path = "/tmp/gl_frame.ppm";
    FILE *f = fopen(path, "wb"); if (!f) return;
    IOSurfaceLock(d->surf[s], kIOSurfaceLockReadOnly, NULL);
    const uint8_t *base = IOSurfaceGetBaseAddress(d->surf[s]); size_t stride = IOSurfaceGetBytesPerRow(d->surf[s]);
    fprintf(f, "P6\n%d %d\n255\n", d->w, d->h);
    uint8_t *row = malloc((size_t)d->w * 3);
    for (int y = d->h - 1; y >= 0; y--) {          // GL rows are bottom-up
        const uint8_t *p = base + (size_t)y * stride;
        for (int x = 0; x < d->w; x++) { row[3 * x] = p[4 * x + 2]; row[3 * x + 1] = p[4 * x + 1]; row[3 * x + 2] = p[4 * x]; }
        fwrite(row, 3, (size_t)d->w, f);
    }
    free(row);
    IOSurfaceUnlock(d->surf[s], kIOSurfaceLockReadOnly, NULL);
    fclose(f);
    rt_log("[gl] frame saved to %s (%dx%d)\n", path, d->w, d->h);
}
static void frame_stats(Drawable *d, int s) {
    static uint64_t frames, t0, worst, last; static int shot = -2, fps = -1;
    if (shot == -2) { const char *e = getenv("RT_GL_SCREENSHOT"); shot = e ? atoi(e) : -1; fps = getenv("RT_FPS") != NULL; }
    frames++;
    if ((int64_t)frames == shot) screenshot(d, s);
    if (!fps) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!t0) { t0 = last = now; return; }
    if (now - last > worst) worst = now - last;
    last = now;
    if (now - t0 >= 5000000000ull) {
        static uint64_t f0; uint64_t n = frames - f0; f0 = frames;
        rt_log("[gl] %.1f fps, worst frame %.1f ms (%dx%d)\n", n * 1e9 / (double)(now - t0), worst / 1e6, d->w, d->h);
        t0 = now; worst = 0;
    }
}

static void swap(void) {
    GLCtx *x = cur;
    if (!x || !x->dr) return;
    Drawable *d = x->dr;
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    glDeleteSync(f);
    frame_stats(d, d->cur);
    present(d, d->cur);
    int di = dr_index(d);
    d->cur = (d->cur + 1) % NSURF; d->gen++;
    drawable_check_size(d);
    if (mtl_pending[di][d->cur]) { [mtl_pending[di][d->cur] waitUntilCompleted]; mtl_pending[di][d->cur] = nil; }
    ctx_attach(x);
}
void I_gdi32_dll_SwapBuffers(CPU *c) { swap(); RET(1); }
void I_opengl32_dll_wglSwapBuffers(CPU *c) { swap(); RET(1); }
void I_opengl32_dll_wglSwapLayerBuffers(CPU *c) { swap(); RET(1); }

// one pixel format: 32-bit RGBA, 24-bit depth, 8-bit stencil, double-buffered, sRGB-capable
void I_gdi32_dll_ChoosePixelFormat(CPU *c) { RET(1); }
void I_gdi32_dll_SetPixelFormat(CPU *c) { RET(1); }
void I_gdi32_dll_GetPixelFormat(CPU *c) { RET(1); }
void I_gdi32_dll_DescribePixelFormat(CPU *c) {
    uint64_t p = A3;   // DescribePixelFormat(hdc, format, size, pfd)
    if (p && (uint32_t)A2 >= 40) {
        uint8_t *b = PTR(p); memset(b, 0, 40);
        wr16(p, 40); wr16(p + 2, 1);
        wr32(p + 4, 0x4 | 0x20 | 0x1);               // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
        b[8] = 0; b[9] = 32;                          // PFD_TYPE_RGBA, cColorBits
        b[10] = 8; b[11] = 16; b[12] = 8; b[13] = 8; b[14] = 8; b[15] = 0; b[16] = 8; b[17] = 24;
        b[23] = 24; b[24] = 8;                        // depth, stencil
    }
    RET(1);
}
void I_opengl32_dll_wglChoosePixelFormat(CPU *c) { RET(1); }
void I_opengl32_dll_wglSetPixelFormat(CPU *c) { RET(1); }
void I_opengl32_dll_wglDescribePixelFormat(CPU *c) { I_gdi32_dll_DescribePixelFormat(c); }

static const char wgl_exts[] = "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_pixel_format WGL_ARB_create_context "
                               "WGL_ARB_create_context_profile WGL_EXT_swap_control WGL_EXT_swap_control_tear WGL_ARB_framebuffer_sRGB "
                               "WGL_EXT_framebuffer_sRGB";
static void wglGetExtensionsStringARB(CPU *c) { RET(G(wgl_exts)); }
static void wglGetExtensionsStringEXT(CPU *c) { RET(G(wgl_exts)); }
static int pf_attrib(int a) {
    switch (a) {
    case 0x2000: return 1;                          // NUMBER_PIXEL_FORMATS
    case 0x2001: case 0x2010: case 0x2011: return 1;   // DRAW_TO_WINDOW, SUPPORT_OPENGL, DOUBLE_BUFFER
    case 0x2003: return 0x2027;                     // ACCELERATION: FULL
    case 0x2007: return 0x2028;                     // SWAP_METHOD: EXCHANGE
    case 0x2013: return 0x202B;                     // PIXEL_TYPE: RGBA
    case 0x2014: return 32;                         // COLOR_BITS
    case 0x2015: case 0x2017: case 0x2019: case 0x201B: return 8;   // RED/GREEN/BLUE/ALPHA_BITS
    case 0x2016: return 16; case 0x2018: return 8; case 0x201A: return 0; case 0x201C: return 24;   // shifts
    case 0x2022: return 24; case 0x2023: return 8;  // DEPTH_BITS, STENCIL_BITS
    case 0x20A9: return 1;                          // FRAMEBUFFER_SRGB_CAPABLE
    }
    return 0;
}
// wglChoosePixelFormatARB(hdc, iattribs, fattribs, max, formats, nformats)
static void wglChoosePixelFormatARB(CPU *c) {
    uint64_t formats = AN(4), n = AN(5);
    if ((uint32_t)A3 && formats) wr32(formats, 1);
    if (n) wr32(n, 1);
    RET(1);
}
// wglGetPixelFormatAttribivARB(hdc, format, layer, n, attribs, values)
static void wglGetPixelFormatAttribivARB(CPU *c) {
    uint32_t n = (uint32_t)A3; uint64_t at = AN(4), out = AN(5);
    for (uint32_t i = 0; i < n; i++) wr32(out + 4 * i, (uint32_t)pf_attrib((int)rd32(at + 4 * i)));
    RET(1);
}
static void wglGetPixelFormatAttribfvARB(CPU *c) {
    uint32_t n = (uint32_t)A3; uint64_t at = AN(4), out = AN(5);
    for (uint32_t i = 0; i < n; i++) { float v = (float)pf_attrib((int)rd32(at + 4 * i)); wr32(out + 4 * i, *(uint32_t *)&v); }
    RET(1);
}
// wglCreateContextAttribsARB(hdc, share, attribs): any version / profile request gets the 4.1 core context
static void wglCreateContextAttribsARB(CPU *c) {
    uint64_t at = A2; int maj = 0, min = 0, prof = 0, flags = 0;
    for (; at && rd32(at); at += 8) {
        uint32_t k = rd32(at), v = rd32(at + 4);
        if (k == 0x2091) maj = v; else if (k == 0x2092) min = v; else if (k == 0x2094) flags = v; else if (k == 0x9126) prof = v;
    }
    // no profile attribute: core for 3.2+ requests, else (legacy) compatibility, as WGL_ARB_create_context_profile says
    uint64_t h = ctx_create(prof ? prof : (maj > 3 || (maj == 3 && min >= 2) ? 1 : 2));
    rt_log("[gl] wglCreateContextAttribsARB(%d.%d, profile %d, flags 0x%x) -> 0x%llx (4.1 core)\n", maj, min, prof, flags, (unsigned long long)h);
    RET(h);
}
static void wglSwapIntervalEXT(CPU *c) {
    int n = (int)A0;
    if (n != swap_interval) rt_log("[gl] swap interval %d (%s)\n", n, n > 0 ? "vsync" : n < 0 ? "adaptive vsync" : "off");
    swap_interval = n;
    RET(1);
}
static void wglGetSwapIntervalEXT(CPU *c) { RET(swap_interval); }

static const struct { const char *name; gfn fn; } wgl_procs[] = {
    { "wglGetExtensionsStringARB", wglGetExtensionsStringARB }, { "wglGetExtensionsStringEXT", wglGetExtensionsStringEXT },
    { "wglChoosePixelFormatARB", wglChoosePixelFormatARB }, { "wglGetPixelFormatAttribivARB", wglGetPixelFormatAttribivARB },
    { "wglGetPixelFormatAttribfvARB", wglGetPixelFormatAttribfvARB }, { "wglCreateContextAttribsARB", wglCreateContextAttribsARB },
    { "wglSwapIntervalEXT", wglSwapIntervalEXT }, { "wglGetSwapIntervalEXT", wglGetSwapIntervalEXT },
};
// extension names whose core function is not simply the name without the suffix
static const struct { const char *ext, *core; } aliases[] = {
    { "glColorMaskIndexedEXT", "glColorMaski" }, { "glEnableIndexedEXT", "glEnablei" }, { "glDisableIndexedEXT", "glDisablei" },
    { "glIsEnabledIndexedEXT", "glIsEnabledi" }, { "glGetIntegerIndexedvEXT", "glGetIntegeri_v" },
    { "glGetBooleanIndexedvEXT", "glGetBooleani_v" }, { "glVertexAttribDivisorARB", "glVertexAttribDivisor" },
};

static gfn find_proc(const char *n) {
    for (size_t i = 0; i < sizeof gl_procs / sizeof *gl_procs; i++) if (!strcmp(gl_procs[i].name, n)) return gl_procs[i].fn;
    for (size_t i = 0; i < sizeof gl_legacy / sizeof *gl_legacy; i++) if (!strcmp(gl_legacy[i].name, n)) return gl_legacy[i].fn;
    for (size_t i = 0; i < sizeof wgl_procs / sizeof *wgl_procs; i++) if (!strcmp(wgl_procs[i].name, n)) return wgl_procs[i].fn;
    return NULL;
}
void I_opengl32_dll_wglGetProcAddress(CPU *c) {
    const char *n = PTR(A0);
    if (!n) { RET(0); return; }
    gfn f = find_proc(n);
    if (!f) for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++) if (!strcmp(aliases[i].ext, n)) f = find_proc(aliases[i].core);
    size_t l = strlen(n);
    if (!f && l > 3 && l < 128 && (!strcmp(n + l - 3, "ARB") || !strcmp(n + l - 3, "EXT"))) {
        char core[128]; memcpy(core, n, l - 3); core[l - 3] = 0;
        f = find_proc(core);
    }
    if (!f && gl_check > 0) rt_log("[gl] wglGetProcAddress(%s): not available\n", n);
    RET(f ? rt_host_fn(f, strdup(n)) : 0);
}
