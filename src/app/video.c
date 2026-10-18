#include "video.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct VideoState {
    Cvar *width, *height, *mode, *maxfps;
    char config_path[512];
} VideoState;

#define V (g_engine.video)

void video_register_cvars(void) {
    V = (VideoState *)calloc(1, sizeof(VideoState));
    V->width = cvar_reg_int("r_width", 1600, 320, 16384, CVAR_ARCHIVE | CVAR_EDITABLE, "window / fullscreen width");
    V->height = cvar_reg_int("r_height", 900, 200, 16384, CVAR_ARCHIVE | CVAR_EDITABLE, "window / fullscreen height");
    V->mode = cvar_reg_int("r_mode", 0, 0, 2, CVAR_ARCHIVE | CVAR_EDITABLE, "0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen");
    V->maxfps = cvar_reg_int("r_maxfps", 0, 0, 1000, CVAR_ARCHIVE | CVAR_EDITABLE, "frame rate cap (0 = unlimited)");
    snprintf(V->config_path, sizeof(V->config_path), "%sconfig.cfg", platform_pref_dir());
}

const char *video_config_path(void) { return V->config_path; }
int video_max_fps(void) { return V->maxfps->i; }

void video_apply(void) {
    SDL_Window *win = g_engine.window;
    if (!win) return;
    int mode = V->mode->i;
    if (mode == 0) {
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowSize(win, V->width->i, V->height->i);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (mode == 1) {
        SDL_SetWindowFullscreenMode(win, NULL); // borderless desktop
        SDL_SetWindowFullscreen(win, true);
    } else {
        SDL_DisplayID d = SDL_GetDisplayForWindow(win);
        SDL_DisplayMode closest;
        if (SDL_GetClosestFullscreenDisplayMode(d, V->width->i, V->height->i, 0.0f, true, &closest)) {
            SDL_SetWindowFullscreenMode(win, &closest);
        } else {
            log_warn("video: no fullscreen mode near %dx%d, using borderless", V->width->i, V->height->i);
            SDL_SetWindowFullscreenMode(win, NULL);
        }
        SDL_SetWindowFullscreen(win, true);
    }
    SDL_SyncWindow(win);
    log_info("video: mode %d, %dx%d", mode, V->width->i, V->height->i);
}
