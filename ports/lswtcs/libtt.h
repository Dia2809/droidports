#ifndef __LIBTT_H__
#define __LIBTT_H__

#include "platform.h"
struct zip;
typedef struct zip zip_t;
#include "so_util.h"
#include "fake_jni.h"

/* The loaded .so module (libTTapp.so) and a globally-shared "this" pointer
 * used for the JNI jclass / jobject arguments. The game does not actually
 * dereference these pointers; it just needs *something* non-null. */
extern so_module *libtt;
/* TT_FAKE_CLASS must point at the real registered TTActivity_class:
 * libTTapp's nativeXxx implementations call back through the env with
 * GetStaticMethodID(env, clazz, ...), and droidports' fake_jni walks
 * clazz->methods to resolve them. A magic int (the trick FalsoJNI gets
 * away with) corrupts whatever the engine copies the lookup result into. */
extern _jclass TTActivity_class;
#define TT_FAKE_CLASS  ((jclass)&TTActivity_class)
#define TT_FAKE_OBJECT ((jobject)&TTActivity_class)

/* The bag of native callbacks resolved out of libTTapp.so. Mirrors
 * the TTActivity Java class's native method declarations.
 *
 * **EVERY** function pointer needs ABI_ATTR (softfp / aapcs). libTTapp
 * is the prebuilt Android .so, compiled softfp - floats are passed in
 * core registers. Our toolchain is hardfp by default, which would push
 * float args through VFP registers. Without ABI_ATTR, any call carrying
 * a jfloat (touch coords, axis values, screen dimensions) transmits
 * garbage and the engine silently ignores it. */
typedef struct TTActivity {
    ABI_ATTR void (*nativeAddAssetsPath)(JNIEnv *, jclass, jobjectArray);
    ABI_ATTR void (*nativeCacheJNIVars)(JNIEnv *, jclass);
    ABI_ATTR void (*nativeOnCreate)(JNIEnv *, jclass);
    ABI_ATTR void (*nativeOnKeyDown)(JNIEnv *, jclass, jint);
    ABI_ATTR void (*nativeOnKeyUp)(JNIEnv *, jclass, jint);
    ABI_ATTR void (*nativeOnPause)(JNIEnv *, jclass);
    ABI_ATTR void (*nativeOnResume)(JNIEnv *, jclass);
    ABI_ATTR void (*nativeOnSensorUpdate)(JNIEnv *, jclass, jint, jfloat, jfloat, jfloat);
    ABI_ATTR void (*nativeOnStart)(JNIEnv *, jclass);
    ABI_ATTR void (*nativeOnStop)(JNIEnv *, jclass);
    ABI_ATTR void (*nativeOnTouchDown)(JNIEnv *, jclass, jint, jint, jfloat, jfloat);
    ABI_ATTR void (*nativeOnTouchMove)(JNIEnv *, jclass, jint, jint, jfloat, jfloat);
    ABI_ATTR void (*nativeOnTouchUp)(JNIEnv *, jclass, jint, jint);
    ABI_ATTR void (*nativeOnWindowFocusChanged)(JNIEnv *, jclass, jboolean);
    ABI_ATTR void (*nativeSetAndroidVersion)(JNIEnv *, jclass, jstring);
    ABI_ATTR void (*nativeSetAssetManager)(JNIEnv *, jclass, jobject);
    ABI_ATTR void (*nativeSetCaps)(JNIEnv *, jclass, jint);
    ABI_ATTR void (*nativeSetLanguage)(JNIEnv *, jclass, jstring);
    ABI_ATTR void (*nativeSetManufacturer)(JNIEnv *, jclass, jstring);
    ABI_ATTR void (*nativeSetModel)(JNIEnv *, jclass, jstring);
    ABI_ATTR void (*nativeSetObbInfo)(JNIEnv *, jclass, jint, jint, jint, jint, jstring, jint);
    ABI_ATTR void (*nativeSetPath)(JNIEnv *, jclass, jstring);
    ABI_ATTR void (*nativeSetScreenDimesions)(JNIEnv *, jclass, jfloat, jfloat);
    ABI_ATTR void (*nativeSetSurface)(JNIEnv *, jclass, jobject);
    ABI_ATTR void (*nativeUpdateGamepadAxisValues)(JNIEnv *, jclass, jfloat, jfloat, jfloat, jfloat, jfloat, jfloat);
} TTActivity;

extern TTActivity activity;
extern int libttIsForeground;

extern const char *get_platform_savedir(const char *gamename);
extern void setup_platform_savedir(const char *gamename);
extern void patch_specifics(so_module *mod);
extern void invoke_app(zip_t *apk, const char *apk_path);
extern void tt_activity_resolve(so_module *mod);

/* Path used to host the extracted .dat asset files. Overridable via
 * env-var LSWTCS_DATA_PATH. Default: ./lswtcs_data/ */
extern const char *tt_data_path(void);

/* Macro table used by prelude_helpers.h to build the JNI class glue.
 * Two groups:
 *   - LSWTCS_NATIVE_LIB_FUNCS:  native methods *implemented* by libTTapp.so
 *                               (resolved with so_symbol).
 *   - LSWTCS_MANAGED_LIB_FUNCS: regular Java methods *implemented* by us
 *                               (called BY the .so via JNI).
 * Only the managed ones need the prelude_helpers dispatch machinery, but
 * we keep both in TTActivity's class so that GetMethodID() can find them. */
#define LSWTCS_NATIVE_LIB_FUNCS \
    DECL_NATIVE(TTActivity, nativeAddAssetsPath,       void, jobjectArray) \
    DECL_NATIVE(TTActivity, nativeCacheJNIVars,        void) \
    DECL_NATIVE(TTActivity, nativeOnCreate,            void) \
    DECL_NATIVE(TTActivity, nativeOnKeyDown,           void, jint) \
    DECL_NATIVE(TTActivity, nativeOnKeyUp,             void, jint) \
    DECL_NATIVE(TTActivity, nativeOnPause,             void) \
    DECL_NATIVE(TTActivity, nativeOnResume,            void) \
    DECL_NATIVE(TTActivity, nativeOnSensorUpdate,      void, jint, jfloat, jfloat, jfloat) \
    DECL_NATIVE(TTActivity, nativeOnStart,             void) \
    DECL_NATIVE(TTActivity, nativeOnStop,              void) \
    DECL_NATIVE(TTActivity, nativeOnTouchDown,         void, jint, jint, jfloat, jfloat) \
    DECL_NATIVE(TTActivity, nativeOnTouchMove,         void, jint, jint, jfloat, jfloat) \
    DECL_NATIVE(TTActivity, nativeOnTouchUp,           void, jint, jint) \
    DECL_NATIVE(TTActivity, nativeOnWindowFocusChanged, void, jboolean) \
    DECL_NATIVE(TTActivity, nativeSetAndroidVersion,   void, jstring) \
    DECL_NATIVE(TTActivity, nativeSetAssetManager,     void, jobject) \
    DECL_NATIVE(TTActivity, nativeSetCaps,             void, jint) \
    DECL_NATIVE(TTActivity, nativeSetLanguage,         void, jstring) \
    DECL_NATIVE(TTActivity, nativeSetManufacturer,     void, jstring) \
    DECL_NATIVE(TTActivity, nativeSetModel,            void, jstring) \
    DECL_NATIVE(TTActivity, nativeSetObbInfo,          void, jint, jint, jint, jint, jstring, jint) \
    DECL_NATIVE(TTActivity, nativeSetPath,             void, jstring) \
    DECL_NATIVE(TTActivity, nativeSetScreenDimesions,  void, jfloat, jfloat) \
    DECL_NATIVE(TTActivity, nativeSetSurface,          void, jobject) \
    DECL_NATIVE(TTActivity, nativeUpdateGamepadAxisValues, void, jfloat, jfloat, jfloat, jfloat, jfloat, jfloat)

#define LSWTCS_MANAGED_LIB_FUNCS \
    DECL_STATIC_MANAGED_NR(TTActivity, FlurryEvent,         void,     F_s, ARG1, "(Ljava/lang/String;)V") \
    DECL_STATIC_MANAGED   (TTActivity, IsMusicActive,       jboolean, F_v, ARG0, "()Z")                   \
    DECL_STATIC_MANAGED_NR(TTActivity, OpenPrivacyPolicy,   void,     F_v, ARG0, "()V")                   \
    DECL_STATIC_MANAGED_NR(TTActivity, OpenTermsOfServices, void,     F_v, ARG0, "()V")                   \
    DECL_STATIC_MANAGED   (TTActivity, getCountryCode,      jstring,  F_v, ARG0, "()Ljava/lang/String;")

#define LSWTCS_FIELDS \
    STATIC_FIELD(jint, SDK_INT, 21, "I") \
    STATIC_FIELD(char *, WINDOW_SERVICE, "window", "Ljava/lang/String;")

extern _jclass TTActivity_class;
extern void Resolve_TTActivity(struct so_module *mod);

#endif
