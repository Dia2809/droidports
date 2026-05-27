/* SDL2-backed display + event pump for the lswtcs port.
 *
 * Almost identical to gmloader's sdl2_media.c, but trimmed to what
 * LSWTCS actually needs: a single 960x544 (or windowed) GL window, a
 * gamepad-and-touch event loop that maps SDL events onto the TTActivity
 * native callbacks, and the small set of init/deinit/flip helpers
 * media.h declares. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "platform.h"
#include "media.h"
#include "libtt.h"
#include "keycodes.h"

static SDL_Window    *sdl_win;
static SDL_GLContext  sdl_ctx;

static SDL_GameController *sdl_pad;

void deinit_display(void)
{
    if (sdl_pad) { SDL_GameControllerClose(sdl_pad); sdl_pad = NULL; }
    if (sdl_ctx) { SDL_GL_DeleteContext(sdl_ctx); sdl_ctx = NULL; }
    if (sdl_win) { SDL_DestroyWindow(sdl_win); sdl_win = NULL; }
    SDL_Quit();
}

int init_display(int w, int h)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fatal_error("SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    if (w <= 0 || h <= 0) { w = 960; h = 544; }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

    sdl_win = SDL_CreateWindow("Lego Star Wars: The Complete Saga",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               w, h, SDL_WINDOW_OPENGL);
    if (!sdl_win) { fatal_error("SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }

    sdl_ctx = SDL_GL_CreateContext(sdl_win);
    if (!sdl_ctx) { fatal_error("SDL_GL_CreateContext: %s\n", SDL_GetError()); return 0; }

    SDL_GL_MakeCurrent(sdl_win, sdl_ctx);
    SDL_GL_SetSwapInterval(1);

    SDL_ShowCursor(SDL_ENABLE);
    /* Bring up the SDL_sim_cursor overlay if LSWTCS_CURSOR=1. */
    extern void cursor_overlay_init(SDL_Window *);
    cursor_overlay_init(sdl_win);
    /* Raise + grab the window so the handheld's WM doesn't drop our
     * input events. SDL_SetWindowInputFocus is best-effort - it may
     * fail under some compositors, that's ok. */
    SDL_RaiseWindow(sdl_win);
    SDL_SetWindowInputFocus(sdl_win);
    SDL_SetWindowGrab(sdl_win, SDL_TRUE);

    /* Dump initial window state so we know what SDL thinks. */
    Uint32 flags = SDL_GetWindowFlags(sdl_win);
    fprintf(stderr, "[sdl] window flags=0x%x  (INPUT_FOCUS=%d  INPUT_GRABBED=%d  MOUSE_FOCUS=%d  SHOWN=%d)\n",
            flags,
            !!(flags & SDL_WINDOW_INPUT_FOCUS),
            !!(flags & SDL_WINDOW_INPUT_GRABBED),
            !!(flags & SDL_WINDOW_MOUSE_FOCUS),
            !!(flags & SDL_WINDOW_SHOWN));
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0)
        fprintf(stderr, "[sdl] display mode %dx%d @ %dHz\n", dm.w, dm.h, dm.refresh_rate);
    SDL_Window *kf = SDL_GetKeyboardFocus();
    SDL_Window *mf = SDL_GetMouseFocus();
    fprintf(stderr, "[sdl] keyboard_focus=%p mouse_focus=%p (ours=%p)\n",
            (void*)kf, (void*)mf, (void*)sdl_win);

    /* Grab the first controller we see, if any. */
    fprintf(stderr, "[input] %d joysticks present\n", SDL_NumJoysticks());
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        const char *jname = SDL_JoystickNameForIndex(i);
        int is_gc = SDL_IsGameController(i);
        fprintf(stderr, "[input]   joystick %d: '%s' game_controller=%d\n",
                i, jname ? jname : "?", is_gc);
        if (is_gc && !sdl_pad) {
            sdl_pad = SDL_GameControllerOpen(i);
            if (sdl_pad)
                fprintf(stderr, "[input]   opened as game controller: %s\n",
                        SDL_GameControllerName(sdl_pad));
            else
                fprintf(stderr, "[input]   open failed: %s\n", SDL_GetError());
        }
    }
    return 1;
}

void get_display_size(int *w, int *h)
{
    SDL_GetWindowSize(sdl_win, w, h);
}

void flip_display_surface(void)
{
    SDL_GL_SwapWindow(sdl_win);
}

/* Bind the *main* SDL GL context on the calling thread - used during
 * tt_eglCreateContext so SHARE_WITH_CURRENT_CONTEXT works no matter
 * which engine thread spins up a new context. */
int sdl_gl_bind_main(void)
{
    return SDL_GL_MakeCurrent(sdl_win, sdl_ctx);
}

/* Bind an arbitrary SDL_GLContext - the engine hands us back the same
 * pointer we returned from tt_eglCreateContext. */
int sdl_gl_bind(void *ctx)
{
    return SDL_GL_MakeCurrent(sdl_win, (SDL_GLContext)ctx);
}

/* Drop whatever this thread holds. Strict EGL backends (KMSDRM /
 * Wayland) demand the old owner unbind before a new thread can
 * MakeCurrent; without this they fail with EGL_BAD_ACCESS. */
void sdl_gl_release_main(void)
{
    SDL_GL_MakeCurrent(sdl_win, NULL);
}

/* Create a brand new SDL GL context that shares textures/buffers with
 * sdl_ctx. We must have *some* GL context current to enable sharing,
 * so bind main first, then drop the new context off so the engine can
 * MakeCurrent it from whichever worker thread will use it. */
void *sdl_gl_create_shared(void)
{
    SDL_GL_MakeCurrent(sdl_win, sdl_ctx);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GLContext c = SDL_GL_CreateContext(sdl_win);
    if (!c) return NULL;
    /* SDL_GL_CreateContext makes the new ctx current; release so the
     * engine's target thread can pick it up. */
    SDL_GL_MakeCurrent(sdl_win, NULL);
    return c;
}

void sdl_gl_destroy(void *ctx)
{
    if (ctx) SDL_GL_DeleteContext((SDL_GLContext)ctx);
}

void sdl_gl_swap(void)
{
    SDL_GL_SwapWindow(sdl_win);
}

/* ---- SDL → TTActivity dispatch ------------------------------------ */

static int map_sdl_key(SDL_Keycode k)
{
    switch (k) {
        case SDLK_UP:    return AKEYCODE_DPAD_UP;
        case SDLK_DOWN:  return AKEYCODE_DPAD_DOWN;
        case SDLK_LEFT:  return AKEYCODE_DPAD_LEFT;
        case SDLK_RIGHT: return AKEYCODE_DPAD_RIGHT;
        case SDLK_SPACE: return AKEYCODE_BUTTON_A;     /* Jump */
        case SDLK_LSHIFT:return AKEYCODE_BUTTON_B;     /* Special */
        case SDLK_LCTRL: return AKEYCODE_BUTTON_X;     /* Action */
        case SDLK_TAB:   return AKEYCODE_BUTTON_Y;     /* Tag */
        case SDLK_q:     return AKEYCODE_BUTTON_L1;
        case SDLK_e:     return AKEYCODE_BUTTON_R1;
        case SDLK_RETURN:return AKEYCODE_BUTTON_START;
        default: return -1;
    }
}

static int map_sdl_btn(Uint8 b)
{
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return AKEYCODE_DPAD_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return AKEYCODE_DPAD_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return AKEYCODE_DPAD_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return AKEYCODE_DPAD_RIGHT;
        case SDL_CONTROLLER_BUTTON_A:          return AKEYCODE_BUTTON_A;
        case SDL_CONTROLLER_BUTTON_B:          return AKEYCODE_BUTTON_B;
        case SDL_CONTROLLER_BUTTON_X:          return AKEYCODE_BUTTON_X;
        case SDL_CONTROLLER_BUTTON_Y:          return AKEYCODE_BUTTON_Y;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return AKEYCODE_BUTTON_L1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:  return AKEYCODE_BUTTON_THUMBL;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return AKEYCODE_BUTTON_THUMBR;
        case SDL_CONTROLLER_BUTTON_START:      return AKEYCODE_BUTTON_START;
        case SDL_CONTROLLER_BUTTON_BACK:       return AKEYCODE_BUTTON_SELECT;
        case SDL_CONTROLLER_BUTTON_GUIDE:      return AKEYCODE_HOME;
        default: return -1;
    }
}

/* Virtual touch cursor driven by the gamepad. The engine's menus are
 * almost entirely Touch-UI (`MechTouchUI*`); button events alone don't
 * navigate them. Move with D-pad / left stick, tap with A. */
static struct {
    float x, y;       /* current cursor position in window space */
    int   pressed;    /* is the virtual finger down right now */
} vcur = {0, 0, 0};

float tt_vcursor_x(void) { return vcur.x; }
float tt_vcursor_y(void) { return vcur.y; }
SDL_Window *sdl_get_main_window(void) { return sdl_win; }

static void vcursor_init_once(void)
{
    if (vcur.x == 0 && vcur.y == 0) {
        int w, h; SDL_GetWindowSize(sdl_win, &w, &h);
        vcur.x = w * 0.5f; vcur.y = h * 0.5f;
    }
}

/* Per-press unique touch id - matches what a real touchscreen does
 * (each finger-down event gets a fresh id). Some engines dedupe by id
 * and drop subsequent touches that reuse a previous one. */
static int s_touch_id = 100;

/* Cursor-vs-gameplay mode toggle.
 *   cursor mode (default at startup) : left stick drives the on-screen
 *       cursor, A button taps. Use for menus.
 *   gameplay mode                    : left stick is forwarded to the
 *       game as the analog stick, A is just the JUMP button. Cursor
 *       drift is suppressed.
 * Toggle with SELECT (BACK). */
static int s_cursor_mode = 1;

static void vcursor_press(JNIEnv *env)
{
    if (vcur.pressed) return;
    vcur.pressed = 1;
    s_touch_id++;
    fprintf(stderr, "[vcur] DOWN id=%d at (%.1f, %.1f)\n",
            s_touch_id, vcur.x, vcur.y);
    if (activity.nativeOnTouchDown)
        activity.nativeOnTouchDown(env, TT_FAKE_CLASS, 0, s_touch_id, vcur.x, vcur.y);
}

static void vcursor_release(JNIEnv *env)
{
    if (!vcur.pressed) return;
    vcur.pressed = 0;
    fprintf(stderr, "[vcur] UP id=%d\n", s_touch_id);
    if (activity.nativeOnTouchUp)
        activity.nativeOnTouchUp(env, TT_FAKE_CLASS, 0, s_touch_id);
}

static void vcursor_step(JNIEnv *env, float dx, float dy)
{
    if (dx == 0 && dy == 0) return;
    int w, h; SDL_GetWindowSize(sdl_win, &w, &h);
    vcur.x += dx; vcur.y += dy;
    if (vcur.x < 0) vcur.x = 0; else if (vcur.x > w) vcur.x = w;
    if (vcur.y < 0) vcur.y = 0; else if (vcur.y > h) vcur.y = h;
    if (vcur.pressed && activity.nativeOnTouchMove)
        activity.nativeOnTouchMove(env, TT_FAKE_CLASS, 0, s_touch_id, vcur.x, vcur.y);
}

int update_input(void)
{
    SDL_Event ev;
    vcursor_init_once();
    /* Periodically dump SDL focus state so we can tell whether the handheld
     * is keeping our window focused. */
    static int focus_log_counter = 0;
    if ((++focus_log_counter % 240) == 1) {
        Uint32 flags = SDL_GetWindowFlags(sdl_win);
        fprintf(stderr, "[sdl] flags=0x%x ifocus=%d mfocus=%d shown=%d\n",
                flags,
                !!(flags & SDL_WINDOW_INPUT_FOCUS),
                !!(flags & SDL_WINDOW_MOUSE_FOCUS),
                !!(flags & SDL_WINDOW_SHOWN));
    }
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                libttIsForeground = 0;
                return 0;

            case SDL_KEYDOWN: {
                if (ev.key.repeat) break;
                int k = map_sdl_key(ev.key.keysym.sym);
                if (k >= 0 && activity.nativeOnKeyDown)
                    activity.nativeOnKeyDown(jni_get_env(), TT_FAKE_CLASS, k);
                break;
            }
            case SDL_KEYUP: {
                int k = map_sdl_key(ev.key.keysym.sym);
                if (k >= 0 && activity.nativeOnKeyUp)
                    activity.nativeOnKeyUp(jni_get_env(), TT_FAKE_CLASS, k);
                break;
            }

            case SDL_CONTROLLERBUTTONDOWN: {
                int k = map_sdl_btn(ev.cbutton.button);
                fprintf(stderr, "[input] btn down sdl=%d -> android=%d\n", ev.cbutton.button, k);
                /* SELECT / BACK toggles cursor mode. */
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                    s_cursor_mode = !s_cursor_mode;
                    fprintf(stderr, "[input] mode -> %s\n",
                            s_cursor_mode ? "CURSOR (menus)" : "GAMEPLAY");
                    break;     /* don't pass SELECT through to the engine */
                }
                if (k >= 0 && activity.nativeOnKeyDown)
                    activity.nativeOnKeyDown(jni_get_env(), TT_FAKE_CLASS, k);
                /* When START is pressed, ALSO fire the generic "primary
                 * action" Android keycodes some title screens watch
                 * for instead of (or in addition to) BUTTON_START. */
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START && activity.nativeOnKeyDown) {
                    activity.nativeOnKeyDown(jni_get_env(), TT_FAKE_CLASS, 23); /* AKEYCODE_DPAD_CENTER */
                    activity.nativeOnKeyDown(jni_get_env(), TT_FAKE_CLASS, 66); /* AKEYCODE_ENTER */
                }
                /* A taps via virtual touch ONLY when the cursor overlay
                 * is active (LSWTCS_CURSOR=1). Lets the player click
                 * menus while the overlay is up; doesn't interfere
                 * with normal gameplay when the overlay is off. */
                extern int cursor_overlay_is_enabled(void);
                if (cursor_overlay_is_enabled() &&
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)
                    vcursor_press(jni_get_env());
                break;
            }
            case SDL_CONTROLLERBUTTONUP: {
                int k = map_sdl_btn(ev.cbutton.button);
                fprintf(stderr, "[input] btn up   sdl=%d -> android=%d\n", ev.cbutton.button, k);
                if (k >= 0 && activity.nativeOnKeyUp)
                    activity.nativeOnKeyUp(jni_get_env(), TT_FAKE_CLASS, k);
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START && activity.nativeOnKeyUp) {
                    activity.nativeOnKeyUp(jni_get_env(), TT_FAKE_CLASS, 23);
                    activity.nativeOnKeyUp(jni_get_env(), TT_FAKE_CLASS, 66);
                }
                {
                    extern int cursor_overlay_is_enabled(void);
                    if (cursor_overlay_is_enabled() &&
                        ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)
                        vcursor_release(jni_get_env());
                }
                break;
            }

            case SDL_CONTROLLERDEVICEADDED:
                fprintf(stderr, "[input] CONTROLLERDEVICEADDED which=%d\n", ev.cdevice.which);
                if (!sdl_pad)
                    sdl_pad = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                fprintf(stderr, "[input] CONTROLLERDEVICEREMOVED which=%d\n", ev.cdevice.which);
                if (sdl_pad) { SDL_GameControllerClose(sdl_pad); sdl_pad = NULL; }
                break;

            case SDL_JOYDEVICEADDED:
                fprintf(stderr, "[input] JOYDEVICEADDED which=%d (game_controller=%d)\n",
                        ev.jdevice.which, SDL_IsGameController(ev.jdevice.which));
                break;
            case SDL_JOYBUTTONDOWN:
                fprintf(stderr, "[input] JOYBUTTONDOWN btn=%d (raw joystick - no controller mapping)\n",
                        ev.jbutton.button);
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT && activity.nativeOnTouchDown) {
                    s_touch_id++;
                    activity.nativeOnTouchDown(jni_get_env(), TT_FAKE_CLASS, 0, s_touch_id,
                                               (jfloat)ev.button.x, (jfloat)ev.button.y);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && activity.nativeOnTouchUp)
                    activity.nativeOnTouchUp(jni_get_env(), TT_FAKE_CLASS, 0, s_touch_id);
                break;
            case SDL_MOUSEMOTION:
                if ((ev.motion.state & SDL_BUTTON_LMASK) && activity.nativeOnTouchMove)
                    activity.nativeOnTouchMove(jni_get_env(), TT_FAKE_CLASS, 0, s_touch_id,
                                               (jfloat)ev.motion.x, (jfloat)ev.motion.y);
                break;
        }
    }

    /* Stream analog axes once per frame so the game's character movement
     * stays smooth even when no button events fire. */
    if (sdl_pad && activity.nativeUpdateGamepadAxisValues) {
        const float scale = 1.0f / 32767.0f;
        Sint16 lx = SDL_GameControllerGetAxis(sdl_pad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ly = SDL_GameControllerGetAxis(sdl_pad, SDL_CONTROLLER_AXIS_LEFTY);
        Sint16 rx = SDL_GameControllerGetAxis(sdl_pad, SDL_CONTROLLER_AXIS_RIGHTX);
        Sint16 ry = SDL_GameControllerGetAxis(sdl_pad, SDL_CONTROLLER_AXIS_RIGHTY);

        float hatX = 0.0f, hatY = 0.0f;
        if (SDL_GameControllerGetButton(sdl_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) hatX += 1.0f;
        if (SDL_GameControllerGetButton(sdl_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  hatX -= 1.0f;
        if (SDL_GameControllerGetButton(sdl_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  hatY += 1.0f;
        if (SDL_GameControllerGetButton(sdl_pad, SDL_CONTROLLER_BUTTON_DPAD_UP))    hatY -= 1.0f;

        /* Deadzone the analog values we forward to the engine too, so
         * stick drift doesn't make the player walk on its own. */
        float lx_dz = lx * scale, ly_dz = ly * scale;
        float rx_dz = rx * scale, ry_dz = ry * scale;
        if (lx_dz > -0.2f && lx_dz < 0.2f) lx_dz = 0.0f;
        if (ly_dz > -0.2f && ly_dz < 0.2f) ly_dz = 0.0f;
        if (rx_dz > -0.2f && rx_dz < 0.2f) rx_dz = 0.0f;
        if (ry_dz > -0.2f && ry_dz < 0.2f) ry_dz = 0.0f;
        activity.nativeUpdateGamepadAxisValues(jni_get_env(), TT_FAKE_CLASS,
                                               hatX, hatY,
                                               lx_dz, ly_dz, rx_dz, ry_dz);

        /* When the cursor overlay is on, D-pad drives the virtual cursor
         * and the overlay window follows. Left stick is always gameplay. */
        extern int  cursor_overlay_is_enabled(void);
        extern void cursor_overlay_tick(void);
        if (cursor_overlay_is_enabled()) {
            vcursor_init_once();
            float step = 18.0f;
            vcursor_step(jni_get_env(), hatX * step, hatY * step);
            cursor_overlay_tick();
        }
    }

    return libttIsForeground;
}
