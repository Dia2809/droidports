/* libTTapp.so wrapper.
 *
 * Walks the Android Activity lifecycle (cache JNI vars → set device info
 * → onCreate/onStart/onResume → setSurface/screen dimensions → window
 * focus), then drops into the SDL event loop. libTTapp manages its own
 * render thread, so the main thread's only job after startup is pumping
 * SDL events and swapping buffers.
 *
 * The asset-pack paths we hand to nativeAddAssetsPath() match the strings
 * the game's path checker validates against; the actual reads land in
 * tt_data_path() because the stdio bridge rewrites them (see io_remap.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>

#include "platform.h"
#include "so_util.h"
#include "fake_jni.h"
#include "media.h"
#include "libtt.h"

#define INTERNAL_PATH "/data/user/0/com.wb.lego.tcs/files"
/* Identify as the Vita - the engine's touch / UI subsystem may switch
 * code paths based on this. The Vita port uses these exact strings and
 * is the only documented working configuration. */
#define ANDROID_MANUFACTURER "Sony"
#define ANDROID_MODEL        "PlayStation Vita"
#define ANDROID_RELEASE      "5.0.2"
#define APK_VERSION_NAME     "2.0.2.02"

#define OBB_INFO_FORCE_ETC1   1
#define OBB_INFO_MAIN_SIZE    2
#define OBB_INFO_MAIN_VERSION 2017
#define OBB_INFO_PATCH_SIZE   0
#define OBB_INFO_PATCH_VERSION 2017

so_module  *libtt = NULL;
TTActivity  activity;
int         libttIsForeground = 1;

static char g_savedir[512];

const char *get_platform_savedir(const char *gamename)
{
    (void)gamename;
    return g_savedir;
}

void setup_platform_savedir(const char *gamename)
{
    const char *base = tt_data_path();
    snprintf(g_savedir, sizeof(g_savedir), "%ssave/%s/", base, gamename);
    mkdir(g_savedir, 0755);
}

/* ---- Activity native-pointer resolution ---------------------------- */

#define LOAD_SYM(field) \
    do { \
        activity.field = (void *)so_symbol(libtt, "Java_com_tt_tech_TTActivity_" #field); \
        if (!activity.field) \
            warning("TTActivity::" #field " not found in libTTapp.so\n"); \
    } while (0)

void tt_activity_resolve(so_module *mod)
{
    libtt = mod;
    LOAD_SYM(nativeAddAssetsPath);
    LOAD_SYM(nativeCacheJNIVars);
    LOAD_SYM(nativeOnCreate);
    LOAD_SYM(nativeOnKeyDown);
    LOAD_SYM(nativeOnKeyUp);
    LOAD_SYM(nativeOnPause);
    LOAD_SYM(nativeOnResume);
    LOAD_SYM(nativeOnSensorUpdate);
    LOAD_SYM(nativeOnStart);
    LOAD_SYM(nativeOnStop);
    LOAD_SYM(nativeOnTouchDown);
    LOAD_SYM(nativeOnTouchMove);
    LOAD_SYM(nativeOnTouchUp);
    LOAD_SYM(nativeOnWindowFocusChanged);
    LOAD_SYM(nativeSetAndroidVersion);
    LOAD_SYM(nativeSetAssetManager);
    LOAD_SYM(nativeSetCaps);
    LOAD_SYM(nativeSetLanguage);
    LOAD_SYM(nativeSetManufacturer);
    LOAD_SYM(nativeSetModel);
    LOAD_SYM(nativeSetObbInfo);
    LOAD_SYM(nativeSetPath);
    LOAD_SYM(nativeSetScreenDimesions);
    LOAD_SYM(nativeSetSurface);
    LOAD_SYM(nativeUpdateGamepadAxisValues);
}

/* AAssetManager is owned by asset_manager.c; forward-declare its factory
 * so we don't pull its header in here. */
extern void *AAssetManager_create(void);
extern void *tt_native_window(int w, int h);

/* Controls thread - mirrors the Vita port's setup so input arrives on
 * a non-main thread, which some engine paths require. */
static void *controls_thread(void *unused)
{
    (void)unused;
    while (libttIsForeground) {
        if (!update_input()) { libttIsForeground = 0; break; }
        SDL_Delay(8);
    }
    return NULL;
}

/* One-shot startup tapper. Fires synthesized touches at the screen
 * centerline (or a user-supplied X,Y) two seconds after boot, with
 * the goal of dismissing the touch-only language picker. */
void *tt_autotap_thread(void *unused)
{
    (void)unused;
    extern void get_display_size(int *w, int *h);

    SDL_Delay(2000);

    int w = 960, h = 544;
    get_display_size(&w, &h);

    /* Default: a single tap at the "English" entry on the language picker.
     * Empirically that's at (~50% wide, ~30% tall) on a 1280x720 layout;
     * scaling it by current window size lands on the same UI element at
     * other resolutions. Override the position via LSWTCS_AUTOTAP_XY=X,Y. */
    const char *xy = getenv("LSWTCS_AUTOTAP_XY");
    float pts[9][2];
    int   n = 0;
    if (xy && strchr(xy, ',')) {
        pts[n][0] = (float)atof(xy);
        pts[n][1] = (float)atof(strchr(xy, ',') + 1);
        n = 1;
    } else {
        pts[0][0] = w * 0.5f;
        pts[0][1] = h * 0.30f;
        n = 1;
    }

    for (int i = 0; i < n && libttIsForeground; i++) {
        warning("[autotap] tap (%.0f, %.0f)\n", pts[i][0], pts[i][1]);
        if (activity.nativeOnTouchDown)
            activity.nativeOnTouchDown(jni_get_env(), TT_FAKE_CLASS,
                                       0, 1000 + i, pts[i][0], pts[i][1]);
        SDL_Delay(80);
        if (activity.nativeOnTouchUp)
            activity.nativeOnTouchUp(jni_get_env(), TT_FAKE_CLASS,
                                     0, 1000 + i);
        SDL_Delay(400);
    }
    return NULL;
}

/* ---- Main app entry ------------------------------------------------ */

static jstring mk_jstr(const char *s)
{
    _jstring *j = malloc(sizeof(*j));
    j->clazz = NULL;
    j->str = strdup(s);
    return j;
}

static jobjectArray mk_strarray(const char *const *strs, int n)
{
    _jarray *arr = calloc(1, sizeof(*arr));
    arr->count = n;
    arr->elements = calloc(n, sizeof(jstring));
    jstring *slot = (jstring *)arr->elements;
    for (int i = 0; i < n; i++) slot[i] = mk_jstr(strs[i]);
    return arr;
}

static void tt_on_create(JNIEnv *env)
{
    const char *asset_paths[4] = {
        INTERNAL_PATH "/assetpacks/asset_Audio/20202/20202/assets/Audio.dat",
        INTERNAL_PATH "/assetpacks/asset_Levels/20202/20202/assets/Levels.dat",
        INTERNAL_PATH "/assetpacks/asset_Others/20202/20202/assets/Others.dat",
        INTERNAL_PATH "/assetpacks/asset_Textures/20202/20202/assets/Textures.dat",
    };

    warning("  > nativeCacheJNIVars\n");
    activity.nativeCacheJNIVars(env, TT_FAKE_CLASS);
    warning("  > nativeSetManufacturer\n");
    activity.nativeSetManufacturer(env, TT_FAKE_CLASS, mk_jstr(ANDROID_MANUFACTURER));
    warning("  > nativeSetModel\n");
    activity.nativeSetModel(env, TT_FAKE_CLASS, mk_jstr(ANDROID_MODEL));
    warning("  > nativeSetObbInfo\n");
    activity.nativeSetObbInfo(env, TT_FAKE_CLASS,
                              OBB_INFO_MAIN_VERSION, OBB_INFO_MAIN_SIZE,
                              OBB_INFO_PATCH_VERSION, OBB_INFO_PATCH_SIZE,
                              mk_jstr(APK_VERSION_NAME), OBB_INFO_FORCE_ETC1);
    warning("  > nativeAddAssetsPath\n");
    activity.nativeAddAssetsPath(env, TT_FAKE_CLASS, mk_strarray(asset_paths, 4));
    warning("  > nativeSetCaps\n");
    activity.nativeSetCaps(env, TT_FAKE_CLASS, 0);

    warning("  > nativeSetPath\n");
    activity.nativeSetPath(env, TT_FAKE_CLASS, mk_jstr(INTERNAL_PATH));
    warning("  > nativeSetLanguage\n");
    activity.nativeSetLanguage(env, TT_FAKE_CLASS, mk_jstr("en"));
    warning("  > nativeSetAndroidVersion\n");
    activity.nativeSetAndroidVersion(env, TT_FAKE_CLASS, mk_jstr(ANDROID_RELEASE));
    warning("  > nativeSetAssetManager\n");
    activity.nativeSetAssetManager(env, TT_FAKE_CLASS, (jobject)AAssetManager_create());
    warning("  > nativeOnCreate\n");
    activity.nativeOnCreate(env, TT_FAKE_CLASS);
    warning("  > tt_on_create done.\n");
}

void invoke_app(zip_t *apk, const char *apk_path)
{
    (void)apk; (void)apk_path;

    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    warning("[invoke_app] entered.\n");

    JNIEnv *env = jni_get_env();
    warning("[invoke_app] jni_get_env=%p\n", env);

    /* Register TTActivity so libTTapp.so's GetMethodID lookups for the
     * managed methods (FlurryEvent, getCountryCode, ...) succeed. */
    jni_register_class(&TTActivity_class);
    warning("[invoke_app] Resolve_TTActivity...\n");
    Resolve_TTActivity(libtt);
    warning("[invoke_app] Resolve_TTActivity done.\n");

    /* libTTapp's JNI_OnLoad caches JavaVM* and registers natives.
     * droidports' fake_jni does not currently expose a JavaVM pointer,
     * so we pass NULL - the implementations we care about (the
     * Java_com_tt_tech_TTActivity_native* entries) are reachable via
     * so_symbol regardless, since they're plain exported functions. */
    jint (*tt_JNI_OnLoad)(JavaVM *, void *) =
        (jint (*)(JavaVM *, void *))so_symbol(libtt, "JNI_OnLoad");
    warning("[invoke_app] JNI_OnLoad=%p\n", (void *)tt_JNI_OnLoad);
    if (tt_JNI_OnLoad) {
        JavaVM *vm = jni_get_vm();
        warning("[invoke_app] calling JNI_OnLoad(vm=%p, NULL)...\n", vm);
        jint r = tt_JNI_OnLoad(vm, NULL);
        warning("[invoke_app] JNI_OnLoad returned %d\n", r);
    } else {
        warning("libTTapp.so has no JNI_OnLoad; continuing without it.\n");
    }

    /* (deferred) `language_selected` is forced AFTER nativeOnResume below;
     * writing it here gets zeroed by the engine's init. */

    /* Pin language to English (0). NuLanguageSet writes a single global
     * the rest of the engine reads via NuLanguageGet; without it we end
     * up with whichever language the picker would have shown by default,
     * which is platform-dependent garbage. Override with LSWTCS_LANG=N. */
    {
        ABI_ATTR void (*nu_lang_set)(int) =
            (ABI_ATTR void (*)(int))so_symbol(libtt, "NuLanguageSet");
        if (nu_lang_set) {
            const char *env_lang = getenv("LSWTCS_LANG");
            int lang = env_lang ? atoi(env_lang) : 0;
            warning("[invoke_app] NuLanguageSet(%d)\n", lang);
            nu_lang_set(lang);
        }
        ABI_ATTR void (*text_set_lang)(int) =
            (ABI_ATTR void (*)(int))so_symbol(libtt, "_Z16Text_SetLanguagei");
        if (text_set_lang) {
            int lang = 0;
            const char *env_lang = getenv("LSWTCS_LANG");
            if (env_lang) lang = atoi(env_lang);
            text_set_lang(lang);
        }
    }

    warning("[invoke_app] setup_platform_savedir...\n");
    setup_platform_savedir("lswtcs");

    int w, h;
    get_display_size(&w, &h);
    warning("[invoke_app] display %dx%d\n", w, h);

    /* Tell the engine the gamepad is connected so in-game controls
     * (movement, jump, action, tag) work. Earlier I worried this would
     * lock the menus into gamepad-mode and silently drop touches, but
     * the user confirmed mouse-on-PC menus work regardless. Override
     * with LSWTCS_NO_GAMEPAD=1 to fall back to touch-only mode. */
    int pad_connected = getenv("LSWTCS_NO_GAMEPAD") ? 0 : 1;
    void (*set_pad_connected)(JNIEnv *, jclass, jboolean) =
        (void (*)(JNIEnv *, jclass, jboolean))so_symbol(libtt,
            "Java_com_tt_tech_CheckGamepadStatus_nativeSetGamePadConnected");
    if (set_pad_connected) {
        warning("[invoke_app] CheckGamepadStatus_nativeSetGamePadConnected(%d)\n",
                pad_connected);
        set_pad_connected(env, TT_FAKE_CLASS, pad_connected);
    } else {
        warning("[invoke_app] CheckGamepadStatus_nativeSetGamePadConnected NOT FOUND\n");
    }

    warning("[invoke_app] tt_on_create...\n");
    tt_on_create(env);
    warning("[invoke_app] nativeOnStart...\n");
    activity.nativeOnStart(env, TT_FAKE_CLASS);
    warning("[invoke_app] nativeOnResume...\n");
    activity.nativeOnResume(env, TT_FAKE_CLASS);

    /* The Vita port calls nativeSetSurface TWICE: once for the
     * "surface created" event and again for "surface changed". Doing
     * it once isn't enough - the engine's input system only finishes
     * arming after the second call. */
    jobject surface = (jobject)tt_native_window(w, h);
    activity.nativeSetSurface(env, TT_FAKE_CLASS, surface);    /* surfaceCreated */
    activity.nativeSetSurface(env, TT_FAKE_CLASS, surface);    /* surfaceChanged */
    activity.nativeSetScreenDimesions(env, TT_FAKE_CLASS,
                                      (w / 220.0f) * 25.4f,
                                      (h / 220.0f) * 25.4f);
    activity.nativeOnWindowFocusChanged(env, TT_FAKE_CLASS, JNI_TRUE);

    /* Re-poke the connected flag once focus is up - some engines latch
     * the flag at multiple points and the early call alone isn't enough. */
    if (set_pad_connected)
        set_pad_connected(env, TT_FAKE_CLASS, pad_connected);

    /* NOW mark "language already chosen" - the engine's per-frame UI
     * loop reads this each tick. Writing it before nativeOnCreate gets
     * wiped during init; writing after focus-changed is too late to be
     * undone. Override with LSWTCS_LANG=N. */
    {
        int *lang_sel = (int *)so_symbol(libtt, "language_selected");
        int *cfg_lang = (int *)so_symbol(libtt, "config_setlanguage");
        warning("[invoke_app] forcing language_selected=1 (post-init)\n");
        if (lang_sel) *lang_sel = 1;
        if (cfg_lang) {
            const char *env_lang = getenv("LSWTCS_LANG");
            *cfg_lang = env_lang ? atoi(env_lang) : 0;
        }
    }

    warning("LSWTCS started; entering event loop.\n");

    /* Auto-tap helper: if LSWTCS_AUTOTAP=1 (default) spawn a one-shot
     * thread that, two seconds after boot, taps a vertical strip down
     * the screen centerline. One of those positions should hit the
     * "English" entry in the language picker and advance the screen.
     * Set LSWTCS_AUTOTAP=0 to disable, or LSWTCS_AUTOTAP_XY=X,Y to
     * tap a single specific coordinate instead. */
    extern void *tt_autotap_thread(void *);
    const char *autotap = getenv("LSWTCS_AUTOTAP");
    if (!autotap || autotap[0] != '0') {
        pthread_t at;
        pthread_create(&at, NULL, tt_autotap_thread, NULL);
        pthread_detach(at);
    }

    /* Spawn a dedicated controls thread (matches the Vita port's
     * controls_thread). Some input paths in the engine only consume
     * events from a non-main thread - dispatching from the main thread
     * was leaving the menu unresponsive even though events arrived. */
    pthread_t input_tid;
    pthread_create(&input_tid, NULL, controls_thread, NULL);
    pthread_detach(input_tid);

    /* Main thread just parks - render is on the engine's thread, input
     * is on the controls thread. */
    while (libttIsForeground) SDL_Delay(100);

    activity.nativeOnPause(env, TT_FAKE_CLASS);
    activity.nativeOnStop(env, TT_FAKE_CLASS);
}
