/* Port-local symbol table for libTTapp.so.
 *
 * libTTapp pulls in a much wider symbol set than gmloader's libyoyo:
 * the full GLES2/EGL surfaces, OpenSL ES, ANativeWindow, AAsset, and a
 * pile of locale / wide-char libc routines. The droidports framework
 * doesn't ship symtables for most of those, so we register them here.
 *
 * Layout:
 *   1. GLES2: re-uses the GLES2_FUNCS / GLES2_EXT_FUNCS macro tables
 *      to auto-generate forwarders (tt_gl_*), so we don't have to list
 *      ~200 functions by hand. The actual fn pointers (glActiveTexture
 *      etc.) live in gles2_bridge.c and are populated by gles2_loader.
 *   2. EGL: minimal stubs that pretend everything succeeded. The real
 *      GL context already lives in SDL, so the .so just needs the calls
 *      to not crash.
 *   3. OpenSL ES: slCreateEngine returns SL_RESULT_FEATURE_UNSUPPORTED
 *      so the game falls back to a silent mixer. SL_IID_* are data
 *      relocations (R_ARM_ABS32) - they MUST resolve or so_resolve
 *      fatal-errors, so we point them at dummy storage.
 *   4. ANativeWindow_* / AAsset_*: forwarded to the stubs in
 *      asset_manager.c.
 *   5. libc/locale/wchar/pthread: passthroughs to the host libc, with
 *      a few small wrappers where Bionic and glibc disagree on signatures.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <math.h>
#include <dlfcn.h>
#include <assert.h>

#include <SDL2/SDL.h>

#include "platform.h"
#include "so_util.h"
#include "gles2.h"
#include "gles2_macros.h"

/* ---- GLES2 thunks ------------------------------------------------- *
 * gles2_bridge.c already declares each gl* as a global function pointer
 * (initialized by gles2_loader.c). We just need plain wrappers that
 * forward the call, since the bridge_* originals are `static` in that
 * translation unit. */

#define GB_DECL_FWD(func, ret, args, vars) \
    extern ret (*func) vars; \
    ABI_ATTR ret tt_gl_##func vars { return func args; }
#define GB_DECL_FWD_NR(func, ret, args, vars) \
    extern void (*func) vars; \
    ABI_ATTR void tt_gl_##func vars { func args; }
#define GB_DECL_FWD_HOOK(func, ret, args, vars) \
    extern ret (*func) vars; \
    ABI_ATTR ret tt_gl_##func vars { return func args; }
GLES2_FUNCS
GLES2_EXT_FUNCS
#undef GB_DECL_FWD_HOOK
#undef GB_DECL_FWD_NR
#undef GB_DECL_FWD

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline long tt_tid(void) { return syscall(SYS_gettid); }

/* Verbose tracing - enable by setting LSWTCS_DEBUG=egl,gl in the env.
 * Levels are bit-ORed:  1=egl  2=gl  4=input  */
static int tt_dbg(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("LSWTCS_DEBUG");
        v = 0;
        if (s) {
            if (strstr(s, "egl"))   v |= 1;
            if (strstr(s, "gl"))    v |= 2;
            if (strstr(s, "input")) v |= 4;
            if (strstr(s, "all"))   v |= 7;
        }
    }
    return v;
}
#define LOG_EGL(...) do { if (tt_dbg() & 1) fprintf(stderr, __VA_ARGS__); } while (0)
#define LOG_GL(...)  do { if (tt_dbg() & 2) fprintf(stderr, __VA_ARGS__); } while (0)

extern void (*glClear)(unsigned int);
extern unsigned int (*glGetError)(void);
extern void (*glDrawArrays)(unsigned int, int, int);
extern void (*glDrawElements)(unsigned int, int, unsigned int, const void *);
extern void (*glViewport)(int, int, int, int);
extern void (*glBindFramebuffer)(unsigned int, unsigned int);

ABI_ATTR static void tt_dbg_glClear(unsigned int mask)
{
    static int n = 0;
    if ((++n % 60) == 1)
        LOG_GL("[gl tid=%ld] glClear #%d mask=0x%x err=0x%x\n",
                tt_tid(), n, mask, glGetError ? glGetError() : 0);
    glClear(mask);
}
ABI_ATTR static void tt_dbg_glDrawArrays(unsigned int mode, int first, int count)
{
    static int n = 0;
    if ((++n % 200) == 1)
        LOG_GL("[gl tid=%ld] glDrawArrays #%d mode=0x%x count=%d\n",
                tt_tid(), n, mode, count);
    glDrawArrays(mode, first, count);
}
ABI_ATTR static void tt_dbg_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices)
{
    static int n = 0;
    if ((++n % 200) == 1)
        LOG_GL("[gl tid=%ld] glDrawElements #%d mode=0x%x count=%d\n",
                tt_tid(), n, mode, count);
    glDrawElements(mode, count, type, indices);
}
ABI_ATTR static void tt_dbg_glViewport(int x, int y, int w, int h)
{
    LOG_GL("[gl tid=%ld] glViewport %d,%d %dx%d\n", tt_tid(), x, y, w, h);
    glViewport(x, y, w, h);
}
ABI_ATTR static void tt_dbg_glBindFramebuffer(unsigned int target, unsigned int fb)
{
    LOG_GL("[gl tid=%ld] glBindFramebuffer target=0x%x fb=%u\n", tt_tid(), target, fb);
    glBindFramebuffer(target, fb);
}

/* ---- EGL stubs ---------------------------------------------------- *
 * Real EGL types aren't needed - all calls are opaque void*. */
typedef void *EGLDisplay; typedef void *EGLConfig; typedef void *EGLContext;
typedef void *EGLSurface; typedef void *EGLNativeWindowType; typedef int EGLBoolean;
typedef int EGLint;

#define EGL_TRUE  1
#define EGL_FALSE 0
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void*)0)

static int s_egl_error = 0x3000; /* EGL_SUCCESS */

ABI_ATTR static EGLDisplay tt_eglGetDisplay(void *display_id)
{ LOG_EGL("[egl] GetDisplay(%p)\n", display_id); return (EGLDisplay)0x1; }
ABI_ATTR static EGLBoolean tt_eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{ (void)d; if (major) *major = 1; if (minor) *minor = 4;
  LOG_EGL("[egl] Initialize\n"); return EGL_TRUE; }
ABI_ATTR static EGLBoolean tt_eglTerminate(EGLDisplay d) { (void)d; return EGL_TRUE; }
ABI_ATTR static EGLBoolean tt_eglBindAPI(EGLint api)
{ LOG_EGL("[egl] BindAPI(0x%x)\n", api); return EGL_TRUE; }
ABI_ATTR static EGLBoolean tt_eglChooseConfig(EGLDisplay d, const EGLint *attribs, EGLConfig *cfgs,
                                              EGLint sz, EGLint *num)
{ (void)d; (void)attribs; if (cfgs && sz > 0) cfgs[0] = (EGLConfig)0x2; if (num) *num = 1;
  LOG_EGL("[egl] ChooseConfig\n"); return EGL_TRUE; }
ABI_ATTR static EGLBoolean tt_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *val)
{ (void)d; (void)c; (void)attr; if (val) *val = 0; return EGL_TRUE; }
/* The engine creates one context per worker (asset-loader threads each
 * own a GL context so they can upload textures in parallel). Returning
 * the same magic value for every CreateContext / CreateSurface call
 * makes the engine treat them as the same object and deadlock waiting
 * for "another thread" to release. Hand out unique values. */
static uintptr_t s_egl_next_handle = 0x1000;
static uintptr_t s_egl_window_surface = 0;   /* the *first* window surface = our SDL window */

ABI_ATTR static EGLContext tt_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrs)
{
    (void)d; (void)c; (void)share; (void)attrs;
    extern void *sdl_gl_create_shared(void);
    void *sdl_ctx = sdl_gl_create_shared();
    LOG_EGL("[egl] CreateContext -> sdl_ctx=%p\n", sdl_ctx);
    return (EGLContext)sdl_ctx;
}
ABI_ATTR static EGLBoolean tt_eglDestroyContext(EGLDisplay d, EGLContext c)
{
    (void)d;
    extern void sdl_gl_destroy(void *);
    sdl_gl_destroy((void *)c);
    return EGL_TRUE;
}
ABI_ATTR static EGLSurface tt_eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a)
{
    (void)d; (void)c; (void)w; (void)a;
    uintptr_t h = ++s_egl_next_handle;
    if (!s_egl_window_surface) s_egl_window_surface = h;
    LOG_EGL("[egl] CreateWindowSurface(window=%p) -> %p\n", w, (void*)h);
    return (EGLSurface)h;
}
ABI_ATTR static EGLSurface tt_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *a)
{
    (void)d; (void)c; (void)a;
    uintptr_t h = ++s_egl_next_handle;
    LOG_EGL("[egl] CreatePbufferSurface -> %p\n", (void*)h);
    return (EGLSurface)h;
}
ABI_ATTR static EGLBoolean tt_eglDestroySurface(EGLDisplay d, EGLSurface s) { (void)d; (void)s; return EGL_TRUE; }
ABI_ATTR static EGLBoolean tt_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    (void)d; (void)rd;
    extern int  sdl_gl_bind(void *ctx);
    extern void sdl_gl_release_main(void);
    LOG_EGL("[egl tid=%ld] MakeCurrent ctx=%p draw=%p\n", tt_tid(), c, dr);
    /* The fake EGLContext we returned from tt_eglCreateContext is the
     * real SDL_GLContext pointer; bind it on the calling thread. NULL
     * means "release this thread's binding". */
    if (c == NULL) {
        sdl_gl_release_main();
        return EGL_TRUE;
    }
    int r = sdl_gl_bind((void *)c);
    if (r != 0) LOG_EGL("[egl] sdl_gl_bind FAILED: %s\n", SDL_GetError());
    return r == 0 ? EGL_TRUE : EGL_FALSE;
}
ABI_ATTR static EGLBoolean tt_eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    (void)d;
    extern void sdl_gl_swap(void);
    static int swap_count = 0;
    if ((uintptr_t)s == s_egl_window_surface) {
        if ((++swap_count % 60) == 1) LOG_EGL("[egl] SwapBuffers #%d (window)\n", swap_count);
        sdl_gl_swap();
    } else {
        if ((swap_count & 0xff) == 0)
            LOG_EGL("[egl] SwapBuffers (pbuffer %p, ignored)\n", s);
    }
    return EGL_TRUE;
}
#define EGL_HEIGHT                0x3056
#define EGL_WIDTH                 0x3057
#define EGL_LARGEST_PBUFFER       0x3058
#define EGL_TEXTURE_FORMAT        0x3080
#define EGL_TEXTURE_TARGET        0x3081
#define EGL_MIPMAP_TEXTURE        0x3082
#define EGL_MIPMAP_LEVEL          0x3083
#define EGL_RENDER_BUFFER         0x3086
#define EGL_BACK_BUFFER           0x3084
#define EGL_HORIZONTAL_RESOLUTION 0x3090
#define EGL_VERTICAL_RESOLUTION   0x3091
#define EGL_PIXEL_ASPECT_RATIO    0x3092
#define EGL_SWAP_BEHAVIOR         0x3093
#define EGL_BUFFER_PRESERVED      0x3094

ABI_ATTR static EGLBoolean tt_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val)
{
    (void)d;
    if (!val) return EGL_FALSE;

    /* Use the SDL window size for the on-screen surface, fall back to
     * 960x544 (the Vita resolution the assets target) for any pbuffer.
     * Returning zero - as the old stub did - made the engine clamp
     * viewport to 0x0 and produce a black frame. */
    extern void get_display_size(int *w, int *h);
    int w = 960, h = 544;
    if ((uintptr_t)s == s_egl_window_surface) get_display_size(&w, &h);

    switch (attr) {
        case EGL_WIDTH:                 *val = w;            break;
        case EGL_HEIGHT:                *val = h;            break;
        case EGL_RENDER_BUFFER:         *val = EGL_BACK_BUFFER; break;
        case EGL_HORIZONTAL_RESOLUTION: *val = 220 * 25400;  break; /* "EGL units" = pixels per metre × 25400 */
        case EGL_VERTICAL_RESOLUTION:   *val = 220 * 25400;  break;
        case EGL_PIXEL_ASPECT_RATIO:    *val = 1 * 10000;    break;
        case EGL_SWAP_BEHAVIOR:         *val = EGL_BUFFER_PRESERVED; break;
        default:                        *val = 0;            break;
    }
    return EGL_TRUE;
}
ABI_ATTR static EGLint     tt_eglGetError(void) { return s_egl_error; }
ABI_ATTR static EGLBoolean tt_eglSwapInterval(EGLDisplay d, EGLint i) { (void)d; (void)i; return EGL_TRUE; }

/* OpenSL ES is implemented in opensles_bridge.c on top of OpenAL.
 * We re-export the SL_IID_* pointers it defines so the .so's data
 * relocations resolve to the same constants the bridge compares
 * against in GetInterface. */
extern const void *tt_SL_IID_ENGINE;
extern const void *tt_SL_IID_ENGINECAPABILITIES;
extern const void *tt_SL_IID_ENVIRONMENTALREVERB;
extern const void *tt_SL_IID_VOLUME;
extern const void *tt_SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
extern const void *tt_SL_IID_PLAY;
extern unsigned int tt_slCreateEngine(void *, unsigned int, void *,
                                      unsigned int, void *, void *);

/* ---- ANativeWindow / AAsset forwarders ---------------------------- *
 * Implementations live in asset_manager.c. */
extern int  ANativeWindow_getWidth(void *);
extern int  ANativeWindow_getHeight(void *);
extern int  ANativeWindow_getFormat(void *);
extern int  ANativeWindow_setBuffersGeometry(void *, int, int, int);
extern void ANativeWindow_acquire(void *);
extern void ANativeWindow_release(void *);
extern int  AAsset_read(void *, void *, size_t);
extern long AAsset_seek(void *, long, int);
extern long AAsset_getLength(void *);
extern long AAsset_getRemainingLength(void *);
extern int  AAsset_openFileDescriptor(void *, long *, long *);
extern void AAsset_close(void *);
extern void *AAssetManager_open(void *, const char *, int);

/* From io_remap.c - rewrites Android assetpack paths onto tt_data_path().
 * tt_fopen returns a wrapped BRIDGE_FILE (via bridge_fopen), not a raw
 * FILE *, so it interoperates with the stdio_bridge's other functions. */
extern void *tt_fopen(const char *path, const char *mode);
extern int   tt_open(const char *path, int flags, int mode);
extern const char *tt_path_remap(const char *path);

ABI_ATTR static int tt_access(const char *path, int mode)
{ return access(tt_path_remap(path), mode); }

ABI_ATTR static int tt_stat(const char *path, void *buf)
{ extern int stat(const char *, void *); return stat(tt_path_remap(path), buf); }

ABI_ATTR static void *tt_ANativeWindow_fromSurface(void *env, void *surface)
{ (void)env; return surface; /* our 'surface' jobject IS our ANativeWindow */ }

/* ---- pthread_attr_t bionic→glibc shims ---------------------------
 * Bionic's pthread_attr_t is a single uint32_t (4 bytes). glibc's is
 * 64 bytes. libTTapp allocates pthread_attr_t on the stack assuming
 * the bionic size, then calls pthread_attr_init - if that hit glibc
 * directly it would write 60 extra bytes off the end of the local
 * frame, smash the saved canary, and abort.
 *
 * Trick: treat the bionic-sized slot as a pointer (4 bytes is enough)
 * to a real heap-allocated glibc pthread_attr_t. All other attr_*
 * calls dereference it back. */

ABI_ATTR static int tt_pthread_attr_init(pthread_attr_t **slot)
{
    pthread_attr_t *a = calloc(1, sizeof(*a));
    if (!a) return -1;
    int r = pthread_attr_init(a);
    *slot = a;
    return r;
}

ABI_ATTR static int tt_pthread_attr_destroy(pthread_attr_t **slot)
{
    if (!slot || !*slot) return 0;
    int r = pthread_attr_destroy(*slot);
    free(*slot);
    *slot = NULL;
    return r;
}

ABI_ATTR static int tt_pthread_attr_setdetachstate(pthread_attr_t **slot, int v)
{ return pthread_attr_setdetachstate(*slot, v); }

ABI_ATTR static int tt_pthread_attr_setstacksize(pthread_attr_t **slot, size_t v)
{ return pthread_attr_setstacksize(*slot, v); }

ABI_ATTR static int tt_pthread_attr_setschedparam(pthread_attr_t **slot, const struct sched_param *p)
{ return pthread_attr_setschedparam(*slot, p); }

ABI_ATTR static int tt_pthread_getschedparam(pthread_t th, int *policy, struct sched_param *param)
{ return pthread_getschedparam(th, policy, param); }

/* The default pthread_create_bridge passes NULL attr; that meant any
 * detachstate / stacksize the engine set would be silently dropped.
 * Dereference the bionic slot to get the real attr block. */
ABI_ATTR static int tt_pthread_create(pthread_t *thread, pthread_attr_t **slot,
                                      void *(*entry)(void *), void *arg)
{
    return pthread_create(thread, slot ? *slot : NULL, entry, arg);
}

/* ---- ABI_ATTR math shims --------------------------------------- *
 * The Android .so is softfp - doubles arrive in core register pairs.
 * glibc on hardfp expects them in VFP regs. ABI_ATTR forces our
 * wrapper to receive softfp, then we forward to the hardfp glibc fn. */
ABI_ATTR static void   tt_sincos(double x, double *s, double *c) { sincos(x, s, c); }
ABI_ATTR static void   tt_sincosf(float x, float *s, float *c)   { sincosf(x, s, c); }
ABI_ATTR static double tt_ldexp(double x, int n)                 { return ldexp(x, n); }
ABI_ATTR static double tt_rint(double x)                         { return rint(x); }

/* ---- libc wrappers where Bionic differs --------------------------- */

static int tt_mbtowc(wchar_t *pwc, const char *s, size_t n)
{
    mbstate_t st = {0};
    return (int)mbrtowc(pwc, s, n, &st);
}

ABI_ATTR static void tt_assert2(const char *file, int line, const char *fn, const char *fail)
{ fprintf(stderr, "Assertion failure at %s:%d in %s: %s\n", file, line, fn, fail); abort(); }

/* ---- Symbol table ------------------------------------------------- */

DynLibFunction symtable_lswtcs[] = {
    /* Instrumented overrides (first match wins inside an array, so these
     * MUST come before the macro-expanded GLES2 block below). */
    {"glClear",                (uintptr_t)&tt_dbg_glClear},
    {"glDrawArrays",           (uintptr_t)&tt_dbg_glDrawArrays},
    {"glDrawElements",         (uintptr_t)&tt_dbg_glDrawElements},
    {"glViewport",             (uintptr_t)&tt_dbg_glViewport},
    {"glBindFramebuffer",      (uintptr_t)&tt_dbg_glBindFramebuffer},

    /* GLES2 */
    #define GB_DECL_FWD(func, ret, args, vars)      {#func, (uintptr_t)&tt_gl_##func},
    #define GB_DECL_FWD_NR(func, ret, args, vars)   {#func, (uintptr_t)&tt_gl_##func},
    #define GB_DECL_FWD_HOOK(func, ret, args, vars) {#func, (uintptr_t)&tt_gl_##func},
    GLES2_FUNCS
    GLES2_EXT_FUNCS
    #undef GB_DECL_FWD_HOOK
    #undef GB_DECL_FWD_NR
    #undef GB_DECL_FWD

    /* EGL */
    {"eglGetDisplay",          (uintptr_t)&tt_eglGetDisplay},
    {"eglInitialize",          (uintptr_t)&tt_eglInitialize},
    {"eglTerminate",           (uintptr_t)&tt_eglTerminate},
    {"eglBindAPI",             (uintptr_t)&tt_eglBindAPI},
    {"eglChooseConfig",        (uintptr_t)&tt_eglChooseConfig},
    {"eglGetConfigAttrib",     (uintptr_t)&tt_eglGetConfigAttrib},
    {"eglCreateContext",       (uintptr_t)&tt_eglCreateContext},
    {"eglDestroyContext",      (uintptr_t)&tt_eglDestroyContext},
    {"eglCreateWindowSurface", (uintptr_t)&tt_eglCreateWindowSurface},
    {"eglCreatePbufferSurface",(uintptr_t)&tt_eglCreatePbufferSurface},
    {"eglDestroySurface",      (uintptr_t)&tt_eglDestroySurface},
    {"eglMakeCurrent",         (uintptr_t)&tt_eglMakeCurrent},
    {"eglSwapBuffers",         (uintptr_t)&tt_eglSwapBuffers},
    {"eglQuerySurface",        (uintptr_t)&tt_eglQuerySurface},
    {"eglGetError",            (uintptr_t)&tt_eglGetError},
    {"eglSwapInterval",        (uintptr_t)&tt_eglSwapInterval},

    /* OpenSL ES - the SL_IID_* are data relocations, so we hand the
     * .so the address of OUR pointer variable; tt_slCreateEngine and
     * the GetInterface dispatch on the other side compare against the
     * exact same pointers. */
    {"slCreateEngine",                   (uintptr_t)&tt_slCreateEngine},
    {"SL_IID_ENGINE",                    (uintptr_t)&tt_SL_IID_ENGINE},
    {"SL_IID_ENGINECAPABILITIES",        (uintptr_t)&tt_SL_IID_ENGINECAPABILITIES},
    {"SL_IID_ENVIRONMENTALREVERB",       (uintptr_t)&tt_SL_IID_ENVIRONMENTALREVERB},
    {"SL_IID_VOLUME",                    (uintptr_t)&tt_SL_IID_VOLUME},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE",  (uintptr_t)&tt_SL_IID_ANDROIDSIMPLEBUFFERQUEUE},
    {"SL_IID_PLAY",                      (uintptr_t)&tt_SL_IID_PLAY},

    /* ANativeWindow / AAsset */
    {"ANativeWindow_fromSurface",         (uintptr_t)&tt_ANativeWindow_fromSurface},
    {"ANativeWindow_getWidth",            (uintptr_t)&ANativeWindow_getWidth},
    {"ANativeWindow_getHeight",           (uintptr_t)&ANativeWindow_getHeight},
    {"ANativeWindow_getFormat",           (uintptr_t)&ANativeWindow_getFormat},
    {"ANativeWindow_setBuffersGeometry",  (uintptr_t)&ANativeWindow_setBuffersGeometry},
    {"ANativeWindow_acquire",             (uintptr_t)&ANativeWindow_acquire},
    {"ANativeWindow_release",             (uintptr_t)&ANativeWindow_release},
    {"AAssetManager_open",                (uintptr_t)&AAssetManager_open},
    {"AAsset_read",                       (uintptr_t)&AAsset_read},
    {"AAsset_seek",                       (uintptr_t)&AAsset_seek},
    {"AAsset_getLength",                  (uintptr_t)&AAsset_getLength},
    {"AAsset_getRemainingLength",         (uintptr_t)&AAsset_getRemainingLength},
    {"AAsset_openFileDescriptor",         (uintptr_t)&AAsset_openFileDescriptor},
    {"AAsset_close",                      (uintptr_t)&AAsset_close},

    /* I/O - route the android paths through tt_path_remap before falling
     * back to libc. */
    {"fopen",         (uintptr_t)&tt_fopen},
    {"open",          (uintptr_t)&tt_open},
    {"access",        (uintptr_t)&tt_access},
    {"stat",          (uintptr_t)&tt_stat},
    {"close",         (uintptr_t)&close},
    {"read",          (uintptr_t)&read},
    {"rename",        (uintptr_t)&rename},
    {"chdir",         (uintptr_t)&chdir},
    {"syscall",       (uintptr_t)&syscall},
    {"dladdr",        (uintptr_t)&dladdr},
    {"putchar",       (uintptr_t)&putchar},
    {"__assert2",     (uintptr_t)&tt_assert2},
    {"strerror_r",    (uintptr_t)&strerror_r},

    /* locale */
    {"newlocale",     (uintptr_t)&newlocale},
    {"freelocale",    (uintptr_t)&freelocale},
    {"uselocale",     (uintptr_t)&uselocale},
    {"localeconv",    (uintptr_t)&localeconv},
    {"strcoll",       (uintptr_t)&strcoll},
    {"strxfrm",       (uintptr_t)&strxfrm},
    {"wcscoll",       (uintptr_t)&wcscoll},
    {"wcsxfrm",       (uintptr_t)&wcsxfrm},
    {"strtoll_l",     (uintptr_t)&strtoll_l},
    {"strtoull_l",    (uintptr_t)&strtoull_l},
    {"strtold_l",     (uintptr_t)&strtold_l},

    /* wide-char / multibyte */
    {"wmemcpy",       (uintptr_t)&wmemcpy},
    {"wmemset",       (uintptr_t)&wmemset},
    {"wmemmove",      (uintptr_t)&wmemmove},
    {"wmemcmp",       (uintptr_t)&wmemcmp},
    {"wmemchr",       (uintptr_t)&wmemchr},
    {"wcstol",        (uintptr_t)&wcstol},
    {"wcstoul",       (uintptr_t)&wcstoul},
    {"wcstoll",       (uintptr_t)&wcstoll},
    {"wcstoull",      (uintptr_t)&wcstoull},
    {"wcstof",        (uintptr_t)&wcstof},
    {"wcstod",        (uintptr_t)&wcstod},
    {"swprintf",      (uintptr_t)&swprintf},
    {"btowc",         (uintptr_t)&btowc},
    {"wctob",         (uintptr_t)&wctob},
    {"wcsnrtombs",    (uintptr_t)&wcsnrtombs},
    {"wcrtomb",       (uintptr_t)&wcrtomb},
    {"mbsnrtowcs",    (uintptr_t)&mbsnrtowcs},
    {"mbrtowc",       (uintptr_t)&mbrtowc},
    {"mbtowc",        (uintptr_t)&tt_mbtowc},
    {"mbrlen",        (uintptr_t)&mbrlen},
    {"mbsrtowcs",     (uintptr_t)&mbsrtowcs},
    {"iswspace",      (uintptr_t)&iswspace},
    {"iswprint",      (uintptr_t)&iswprint},
    {"iswcntrl",      (uintptr_t)&iswcntrl},
    {"iswalpha",      (uintptr_t)&iswalpha},
    {"iswdigit",      (uintptr_t)&iswdigit},
    {"iswpunct",      (uintptr_t)&iswpunct},
    {"iswxdigit",     (uintptr_t)&iswxdigit},
    {"vsscanf",       (uintptr_t)&vsscanf},

    /* misc libc/math */
    {"posix_memalign", (uintptr_t)&posix_memalign},
    {"sched_yield",    (uintptr_t)&sched_yield},
    /* These MUST go through ABI_ATTR shims (softfp -> hardfp). Without
     * the wrappers the .so passes doubles in core regs while glibc
     * reads from VFP regs and gets garbage -> segfault inside libm. */
    {"rint",           (uintptr_t)&tt_rint},
    {"ldexp",          (uintptr_t)&tt_ldexp},
    {"sincos",         (uintptr_t)&tt_sincos},
    {"sincosf",        (uintptr_t)&tt_sincosf},

    /* pthread fillers (rest live in symtable_pthread).
     * The pthread_attr_* entries go through size-converting shims so
     * the .so's 4-byte bionic attr slot is treated as a pointer to a
     * real glibc-sized attr block - see tt_pthread_attr_* above. */
    {"pthread_self",                (uintptr_t)&pthread_self},
    {"pthread_equal",               (uintptr_t)&pthread_equal},
    {"pthread_detach",              (uintptr_t)&pthread_detach},
    {"pthread_exit",                (uintptr_t)&pthread_exit},
    {"pthread_mutex_trylock",       (uintptr_t)&pthread_mutex_trylock},
    {"pthread_create",              (uintptr_t)&tt_pthread_create},
    {"pthread_attr_init",           (uintptr_t)&tt_pthread_attr_init},
    {"pthread_attr_destroy",        (uintptr_t)&tt_pthread_attr_destroy},
    {"pthread_attr_setdetachstate", (uintptr_t)&tt_pthread_attr_setdetachstate},
    {"pthread_attr_setstacksize",   (uintptr_t)&tt_pthread_attr_setstacksize},
    {"pthread_attr_setschedparam",  (uintptr_t)&tt_pthread_attr_setschedparam},
    {"pthread_getschedparam",       (uintptr_t)&tt_pthread_getschedparam},

    { NULL, 0 }
};
