/* SDL_sim_cursor-backed overlay for handhelds whose X driver doesn't
 * render a real mouse cursor.
 *
 * We can't blit/render into the game's OpenGL window cleanly (it has no
 * SDL surface or SDL_Renderer attached), so we run a tiny *second* SDL
 * window with a software renderer. The window is borderless and stays
 * on top; each tick we warp it to the virtual-cursor position and ask
 * SDL_sim_cursor to draw onto its surface.
 *
 * Enable with LSWTCS_CURSOR=1 (default off so it doesn't grab anything
 * if the player already has a real OS cursor). The cursor is intended
 * for the language picker and other touch-only menus; toggle off via
 * SELECT on the gamepad once you're in the game. */

#define SDL_SIM_CURSOR_COMPILE 1
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "SDL_sim_cursor.h"

#include <stdio.h>
#include <stdlib.h>

#include "platform.h"

#define CURSOR_W 24
#define CURSOR_H 24

static SDL_Window  *cur_win = NULL;
static SDL_Surface *cur_surf = NULL;
static int          cur_enabled = 0;

extern float tt_vcursor_x(void);
extern float tt_vcursor_y(void);

void cursor_overlay_init(SDL_Window *parent)
{
    if (cur_win) return;
    /* Default off. Set LSWTCS_CURSOR=1 to enable. */
    const char *env = getenv("LSWTCS_CURSOR");
    cur_enabled = (env && env[0] == '1');
    if (!cur_enabled) return;

    /* Force a software-rendered second window so Mesa's GPU path isn't
     * re-entered for the overlay. On Panfrost the GL context for the
     * main window crashes inside libgallium if a second window also
     * pulls in the renderer. */
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

    Uint32 flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                   SDL_WINDOW_SKIP_TASKBAR;
    cur_win = SDL_CreateWindow("lswtcs-cursor",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               CURSOR_W, CURSOR_H, flags);
    if (!cur_win) {
        fprintf(stderr, "[cursor] SDL_CreateWindow failed: %s\n", SDL_GetError());
        cur_enabled = 0;
        return;
    }
    cur_surf = SDL_GetWindowSurface(cur_win);
    if (!cur_surf) {
        fprintf(stderr, "[cursor] SDL_GetWindowSurface failed: %s\n", SDL_GetError());
    }
    SDL_SIM_MouseInit();
    /* Force the default arrow as current so BlitCursor has something to draw. */
    SDL_SIM_SetCursor(SDL_SIM_GetDefaultCursor());
    SDL_SIM_ShowCursor(1);
    if (parent) SDL_RaiseWindow(parent);
    fprintf(stderr, "[cursor] overlay enabled, win=%p surf=%p\n",
            (void *)cur_win, (void *)cur_surf);
}

void cursor_overlay_tick(void)
{
    if (!cur_enabled || !cur_win || !cur_surf) return;

    /* Convert game-window-relative cursor coords into screen-absolute by
     * adding the main window's screen position. */
    extern SDL_Window *sdl_get_main_window(void);
    SDL_Window *main_w = sdl_get_main_window();
    int wx = 0, wy = 0;
    if (main_w) SDL_GetWindowPosition(main_w, &wx, &wy);

    int sx = wx + (int)tt_vcursor_x() - CURSOR_W / 2;
    int sy = wy + (int)tt_vcursor_y() - CURSOR_H / 2;
    SDL_SetWindowPosition(cur_win, sx, sy);

    /* Re-fetch the surface every frame; the window manager can invalidate
     * it on resize/reposition. Cheap on this size. */
    cur_surf = SDL_GetWindowSurface(cur_win);
    if (!cur_surf) return;

    /* Fill bright magenta so we instantly notice the window even if
     * BlitCursor draws nothing (debug). Once confirmed, switch to 0. */
    SDL_FillRect(cur_surf, NULL,
                 SDL_MapRGB(cur_surf->format, 0xff, 0x00, 0xff));
    SDL_SIM_BlitCursor(cur_surf);
    SDL_UpdateWindowSurface(cur_win);
}

void cursor_overlay_destroy(void)
{
    if (!cur_enabled) return;
    SDL_SIM_MouseQuit();
    if (cur_win) { SDL_DestroyWindow(cur_win); cur_win = NULL; }
    cur_surf = NULL;
    cur_enabled = 0;
}

void cursor_overlay_set_enabled(int on)
{
    cur_enabled = !!on;
    if (cur_win) {
        if (on) SDL_ShowWindow(cur_win);
        else    SDL_HideWindow(cur_win);
    }
}
int cursor_overlay_is_enabled(void) { return cur_enabled && cur_win != NULL; }
