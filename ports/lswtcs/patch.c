/* Game-specific in-place patches applied to libTTapp.so after relocation.
 *
 * The Vita port pins the various engine threads to specific CPU cores via
 * sceKernelChangeThreadCpuAffinityMask(). On Linux that's neither portable
 * nor useful (the kernel scheduler handles it), so those hooks are no-ops
 * here. What we *do* keep is the controller mapping fix: the stock build
 * derives in-game gamepad slot indices from Android keycodes assuming a
 * standard layout, which doesn't match the swap LSWTCS applied for the
 * touch-friendly Vita controls. The remap below preserves the documented
 * "Triangle=Tag / Circle=Special / Cross=Jump / Square=Action" scheme on
 * desktop too. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "so_util.h"

#include "libtt.h"
#include "keycodes.h"

/* If LSWTCS_LEGACY_CONTROLS=1, skip the remap and let the .so use its
 * default keycode→slot mapping. Useful when the user wires an Android-spec
 * gamepad through SDL. */
static int patch_controls_enabled(void)
{
    const char *v = getenv("LSWTCS_LEGACY_CONTROLS");
    return !(v && v[0] == '1');
}

/* The engine's language picker is touch-only; on a handheld with no
 * touchscreen the player can't get past it. NuLanguageConsoleSelectable
 * is the predicate the engine reads to decide "do we show the picker".
 *
 * IMPORTANT: looking at the disassembly,
 *     mov r0, #1
 *     bx  lr
 * the function *returns* the bool (in r0) AND ignores the out-pointer.
 * We must match - returning 0 (false) AND writing 0 through the
 * pointer keeps both possible callers happy. */
ABI_ATTR static int NuLanguageConsoleSelectable_force_off(unsigned char *out)
{
    static int seen = 0;
    if (!seen) { warning("[patch] NuLanguageConsoleSelectable hook FIRED\n"); seen = 1; }
    if (out) *out = 0;
    return 0;
}

/* The intro cutscene references rigid props (blasterBolt, blasterBoltGlow)
 * that aren't getting fixed up in our environment - the engine logs
 * "NuGCutRigidSysFixUp: cannot fixup ..." for each, then segfaults when
 * downstream code uses the NULL rigid. We don't have a clean way to
 * provide those rigids, so make the cutscene playback a no-op. The game
 * sees the cutscene "finish" immediately and moves on. Disable with
 * LSWTCS_PLAY_CUTSCENES=1 if you want to risk the crash. */
ABI_ATTR static int instNuGCutScenePlay_noop(void *unused1, int unused2)
{
    (void)unused1; (void)unused2;
    return 0;
}

/* The fixup pass itself is where the blasterBolt warnings print and is
 * the leading suspect for the SIGSEGV - it leaves the cutscene's rigid
 * table partly populated with NULLs that downstream code dereferences
 * without checking. Make it a no-op so the cutscene never finishes
 * preparing, and the engine treats it as empty/already done. */
ABI_ATTR static void NuGCutSceneFixUp_noop(void *a, void *b, void *c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
}

ABI_ATTR static void NuGCutSceneFixUpExtra_noop(void *a, void *b)
{
    (void)a; (void)b;
}

/* NuInputDevicePS::GetGamePadButtonIndex(int android_code, int *is_success)
 *
 * Returns the engine's internal "button slot" for a given Android keycode.
 * Same table as the Vita port's reimpl. */
ABI_ATTR static unsigned int GetGamePadButtonIndex_reimpl(int android_code, int *is_success)
{
    *is_success = 1;
    switch (android_code) {
        case AKEYCODE_HOME:
        case AKEYCODE_BUTTON_START:   return GAMEPAD_START;
        case AKEYCODE_DPAD_UP:
        case AKEYCODE_DPAD_DOWN:
        case AKEYCODE_DPAD_LEFT:
        case AKEYCODE_DPAD_RIGHT:     return 0;
        case AKEYCODE_BUTTON_A:       return GAMEPAD_JUMP;
        case AKEYCODE_BUTTON_B:       return GAMEPAD_SPECIAL;
        case AKEYCODE_BUTTON_X:       return GAMEPAD_ACTION;
        case AKEYCODE_BUTTON_Y:       return GAMEPAD_TAG;
        case AKEYCODE_BUTTON_L1:      return GAMEPAD_L1;
        case AKEYCODE_BUTTON_R1:      return GAMEPAD_R1;
        case AKEYCODE_BUTTON_L2:      return GAMEPAD_L2;
        case AKEYCODE_BUTTON_R2:      return GAMEPAD_R2;
        case AKEYCODE_BUTTON_THUMBL:  return GAMEPAD_L3;
        case AKEYCODE_BUTTON_THUMBR:  return GAMEPAD_R3;
        default:
            *is_success = 0;
            return 0;
    }
}

/* Log every time the engine looks up a gamepad-button slot from an
 * android keycode. Useful to confirm the remap is on the input path
 * the menu actually reads. */
ABI_ATTR static unsigned int GetGamePadButtonIndex_trace(int android_code, int *is_success)
{
    unsigned int r = GetGamePadButtonIndex_reimpl(android_code, is_success);
    warning("[input] GetGamePadButtonIndex(code=%d) -> slot=%u ok=%d\n",
            android_code, r, *is_success);
    return r;
}

void patch_specifics(so_module *mod)
{
    libtt = mod;

    uintptr_t sym = (uintptr_t)so_symbol(mod, "_ZN15NuInputDevicePS21GetGamePadButtonIndexEiPi");
    warning("[patch] GetGamePadButtonIndex symbol: 0x%lx\n", (unsigned long)sym);

    /* hook_symbols stops at the first NULL entry, so we build the list
     * dynamically and append/skip per env-var. */
    DynLibHooks hooks[8] = {{0}};
    int n = 0;

    /* Controller remap is the ONLY hook gated on LSWTCS_LEGACY_CONTROLS;
     * the cutscene/language hooks still install. */
    if (patch_controls_enabled() && sym) {
        hooks[n++] = (DynLibHooks){
            "_ZN15NuInputDevicePS21GetGamePadButtonIndexEiPi",
            (uintptr_t)&GetGamePadButtonIndex_trace, 1};
    } else {
        warning("[patch] Legacy controls requested; skipping button remap.\n");
    }

    /* Skip the touch-only language picker so the game boots straight
     * into the front-end. Disable with LSWTCS_SHOW_LANG_PICKER=1. */
    if (!getenv("LSWTCS_SHOW_LANG_PICKER")) {
        uintptr_t s = (uintptr_t)so_symbol(mod, "_Z27NuLanguageConsoleSelectablePb");
        warning("[patch] NuLanguageConsoleSelectable resolved=0x%lx\n", (unsigned long)s);
        hooks[n++] = (DynLibHooks){
            "_Z27NuLanguageConsoleSelectablePb",
            (uintptr_t)&NuLanguageConsoleSelectable_force_off, 1};
    }

    /* Stub the cutscene player AND the FixUp pass that emits the
     * blasterBolt warnings. Either alone isn't enough - the crash
     * happens between FixUp populating NULL rigids and downstream code
     * dereferencing them. Disable with LSWTCS_PLAY_CUTSCENES=1. */
    if (!getenv("LSWTCS_PLAY_CUTSCENES")) {
        hooks[n++] = (DynLibHooks){
            "instNuGCutScenePlay",
            (uintptr_t)&instNuGCutScenePlay_noop, 1};
        hooks[n++] = (DynLibHooks){
            "NuGCutSceneFixUp",
            (uintptr_t)&NuGCutSceneFixUp_noop, 1};
        hooks[n++] = (DynLibHooks){
            "NuGCutSceneFixUpExtra",
            (uintptr_t)&NuGCutSceneFixUpExtra_noop, 1};
    }

    hook_symbols(mod, hooks);
    warning("[patch] %d hooks installed\n", n);
}
