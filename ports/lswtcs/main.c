/* Entry point for the Lego Star Wars: The Complete Saga port.
 *
 * Mirrors gmloader's main(): parse a window-size override, init the GL
 * window, open the APK, inflate libTTapp.so from lib/armeabi-v7a/, run
 * so_load, apply patches, then hand off to invoke_app(). Unlike gmloader
 * we don't need libc++_shared.so - libTTapp ships statically linked.
 *
 * The actual game data (Audio.dat, Levels.dat, Others.dat, Textures.dat)
 * must live in tt_data_path() - see io_remap.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>

#include "io_util.h"
#include "platform.h"
#include "so_util.h"
#include "fake_jni.h"
#include "zip_util.h"
#include "media.h"
#include "gles2.h"

#include "symtables.h"

#include <execinfo.h>
#include <signal.h>

#include "libtt.h"

static void crash_handler(int sig)
{
    void *frames[32];
    int n = backtrace(frames, 32);
    fprintf(stderr, "\n*** signal %d caught, %d frames:\n", sig, n);
    backtrace_symbols_fd(frames, n, 2);
    /* Re-raise with the default handler so we still produce a core. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
}

extern DynLibFunction symtable_lswtcs[];

/* Static patches: tables consulted while relocating libTTapp.so. We have
 * no game-specific static rewrite list, but other bridges (math, libc, ...)
 * still need to be visible via the dynamic resolution path below. */
DynLibFunction *so_static_patches[] = {
    symtable_openal,
    NULL
};

/* Order matters: symtable_lswtcs is searched FIRST so its overrides
 * (notably the pthread_attr_t size-converting shims and pthread_create)
 * win over the generic bridges, which assume bionic-and-glibc agree
 * on opaque struct sizes - they don't. */
DynLibFunction *so_dynamic_libraries[] = {
    symtable_lswtcs,
    symtable_ctype,
    symtable_math,
    symtable_misc,
    symtable_openal,
    symtable_pthread,
    symtable_stdio,
    symtable_zip,
    NULL
};

static void print_usage(const char *prog)
{
    fatal_error("LSWTCS Loader: Lego Star Wars TCS compatibility layer.\n");
    fatal_error("Usage: %s [--width W] [--height H] <libTTapp-source>\n", prog);
    fatal_error("  <libTTapp-source> may be either an .apk or a path to libTTapp.so\n");
    fatal_error("  Game data files (Audio/Levels/Others/Textures.dat) are read from\n");
    fatal_error("  $LSWTCS_DATA_PATH (default ./lswtcs_data/).\n");
}

static int has_suffix(const char *s, const char *suf)
{
    size_t ns = strlen(s), nf = strlen(suf);
    return ns >= nf && strcmp(s + ns - nf, suf) == 0;
}

int main(int argc, char *argv[])
{
    install_crash_handler();
    int disp_w = -1, disp_h = -1;
    uintptr_t addr_tt = 0x80000000;

    static struct option long_options[] = {
        {"width",  required_argument, 0, 'w'},
        {"height", required_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:h:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'w': disp_w = atoi(optarg); break;
            case 'h': disp_h = atoi(optarg); break;
            default:  print_usage(argv[0]); return -1;
        }
    }

    if (optind >= argc) { print_usage(argv[0]); return -1; }
    const char *src_path = argv[optind];

    if (!init_display(disp_w, disp_h)) return -1;
    if (!init_gles2()) return -1;

    /* Release the GL context on the main thread so the engine's render
     * thread can claim it cleanly. Without this, strict EGL backends
     * (KMSDRM, Wayland) refuse the cross-thread MakeCurrent with
     * EGL_BAD_ACCESS - the PC's GLX backend silently allows the steal
     * but handheld stacks (Mesa-on-EGL) do not. */
    extern void sdl_gl_release_main(void);
    sdl_gl_release_main();

    /* Load libTTapp.so either directly or by inflating it from the APK. */
    void  *so_buf = NULL;
    size_t so_len = 0;
    zip_t *apk    = NULL;

    if (has_suffix(src_path, ".so")) {
        int fd;
        if (!io_buffer_from_file(src_path, &fd, &so_buf, &so_len, 0)) {
            fatal_error("Unable to read '%s'.\n", src_path);
            return -1;
        }
    } else {
        int err, fd = open(src_path, O_RDONLY);
        if (fd < 0) { fatal_error("Cannot open APK '%s': %s\n", src_path, strerror(errno)); return -1; }
        apk = zip_fdopen(fd, ZIP_RDONLY, &err);
        if (!apk) { fatal_error("zip_fdopen failed (err=%d)\n", err); return -1; }

        const char *path_in_apk = "lib/armeabi-v7a/libTTapp.so";
        warning("Inflating %s...\n", path_in_apk);
        if (zip_inflate_buf(apk, path_in_apk, &so_len, &so_buf) == 0 || !so_buf) {
            fatal_error("Failed to extract %s from APK.\n", path_in_apk);
            return -1;
        }
    }

    warning("Loading libTTapp.so (%p, %zu bytes)...\n", so_buf, so_len);
    so_module so_mod = {};
    int rc = so_load(&so_mod, "libTTapp.so", addr_tt, so_buf, so_len);
    if (rc != 0) { fatal_error("so_load failed: %d\n", rc); return -1; }

    libtt = &so_mod;
    warning("[stage] tt_activity_resolve...\n");
    tt_activity_resolve(&so_mod);
    warning("[stage] patch_specifics...\n");
    patch_specifics(&so_mod);
    warning("[stage] so_flush_caches...\n");
    so_flush_caches(&so_mod, 0);
    warning("[stage] invoke_app...\n");

    invoke_app(apk, src_path);

    deinit_display();
    deinit_gles2();
    return 0;
}
