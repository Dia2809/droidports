/* JNI class glue for com.tt.tech.TTActivity.
 *
 * Uses prelude_helpers.h to auto-generate:
 *   - extern function-pointer slots for the NATIVE methods
 *     (Java_com_tt_tech_TTActivity_* symbols, located in libTTapp.so)
 *   - bridge wrappers for the MANAGED methods so they can be invoked
 *     through the FakeJNI's CallStatic*Method machinery.
 *
 * The MANAGED implementations are mostly stubs: they cover the handful of
 * Java methods libTTapp.so actually looks up at runtime (analytics + system
 * web-browser launches), and a couple of static fields it queries on the
 * android.os.Build / android.content.Context constants. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include "fake_jni.h"
#include "so_util.h"
#include "platform.h"
#include "libtt.h"

#define MANGLED_CLASSPATH "Java_com_tt_tech_TTActivity_"
#define CLASSPATH "com/tt/tech/TTActivity"
#define CLASSNAME "TTActivity"
#define CLASS _jclass TTActivity_class
#define STRUCT_NAME TTActivity_obj
#define NATIVE_LIB_FUNCS LSWTCS_NATIVE_LIB_FUNCS
#define MANAGED_LIB_FUNCS LSWTCS_MANAGED_LIB_FUNCS
#define RESOLVER Resolve_TTActivity
#define FIELDS LSWTCS_FIELDS

/* ---- Managed-method implementations --------------------------------
 * Signatures must match the ARG-count given in libtt.h's
 * LSWTCS_MANAGED_LIB_FUNCS table; the prologue_helpers F_* macros
 * unpack the va_list / jvalue arguments in matching order. */

static void TTActivity_FlurryEvent(jstring event)
{
    if (event && event->str) {
        warning("[FLURRY] Event: %s\n", event->str);
    } else {
        warning("[FLURRY] Event: <null>\n");
    }
}

static jboolean TTActivity_IsMusicActive(void)
{
    /* No background music probe on a desktop. Reporting "no" matches the
     * Vita port and keeps the game's mixer unmuted. */
    return JNI_FALSE;
}

static void TTActivity_OpenPrivacyPolicy(void)
{
    static const char URL[] = "https://policies.warnerbros.com/privacy/children/";
    warning("[TTActivity] Privacy policy URL: %s\n", URL);
}

static void TTActivity_OpenTermsOfServices(void)
{
    static const char URL[] = "https://policies.warnerbros.com/terms/en-us/html/terms_en-us_1.2.0.html";
    warning("[TTActivity] Terms of service URL: %s\n", URL);
}

static jstring TTActivity_getCountryCode(void)
{
    static _jstring cc;
    static char code[8] = "US";

    /* Honor the user's locale where possible; fall back to "US" so the
     * game's region gates still resolve. */
    const char *lc = setlocale(LC_ALL, NULL);
    if (lc) {
        const char *u = strchr(lc, '_');
        if (u && strlen(u + 1) >= 2) {
            code[0] = u[1];
            code[1] = u[2];
            code[2] = '\0';
        }
    }
    cc = (_jstring){.clazz = NULL, .str = code};
    return &cc;
}

#include "prelude_helpers.h"

/* Emits Resolve_TTActivity() and the TTActivity_class struct, consuming
 * the LSWTCS_NATIVE/MANAGED_LIB_FUNCS + FIELDS tables defined above. */
#include "prologue_helpers.h"
