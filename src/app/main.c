// Temporary bring-up entry point: window + renderer test scene.
#include "core/engine.h"
#include "core/log.h"
#include "core/cvar.h"
#include "core/platform.h"
#include "core/jobs.h"
#include "render/render.h"
#include "render/mesh_gen.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdlib.h>
#include <stdio.h>

Engine g_engine;

int main(int argc, char **argv) {
    g_engine.argc = argc;
    g_engine.argv = argv;
    log_init("deadground.log");
    cvar_system_init();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { log_fatal("SDL_Init: %s", SDL_GetError()); }
    arena_init(&g_engine.frame_arena, MB(32), "frame");
    jobs_init(0);

    g_engine.window = SDL_CreateWindow("Deadground", 1600, 900, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!render_init(g_engine.window)) log_fatal("renderer init failed");

    int max_frames = 0;
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
    }

    BasicMeshes bm;
    basic_meshes_create(&bm);

    while (!g_engine.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) if (ev.type == SDL_EVENT_QUIT) g_engine.quit = true;
        g_engine.real_time = platform_time();
        arena_reset(&g_engine.frame_arena);
        if (render_begin_frame()) {
            RenderCamera cam = { v3(0, 2.0f, 6.0f), 0, -0.2f, 0, 70, 0.05f, 500 };
            render_set_camera(&cam);
            float t = (float)g_engine.real_time;
            DrawItem d;
            d.mesh = bm.cube;
            d.mat = material_color(v4(0.8f, 0.3f, 0.2f, 1));
            d.model = mat4_trs(v3(0, 1, 0), quat_euler(t, 0.3f, 0), v3s(1.5f));
            render_draw(&d);
            d.mesh = bm.cube;
            d.mat = material_color(v4(0.6f, 0.6f, 0.6f, 1));
            d.mat.flags = MAT_GRID;
            d.model = mat4_trs(v3(0, -0.5f, 0), quat_identity(), v3(20, 1, 20));
            render_draw(&d);
            d.mesh = bm.sphere;
            d.mat = material_color(v4(0.2f, 0.5f, 0.9f, 1));
            d.model = mat4_trs(v3(2.5f, 0.75f, 0), quat_identity(), v3s(1.5f));
            render_draw(&d);
            d.mesh = bm.capsule;
            d.mat = material_color(v4(0.2f, 0.8f, 0.3f, 1));
            d.model = mat4_trs(v3(-2.5f, 0.9f, 0), quat_identity(), v3(0.6f, 0.9f, 0.6f));
            render_draw(&d);
            LineVertex *lv = render_lines_alloc(6, false);
            if (lv) {
                lv[0] = (LineVertex){ v3(0, 0.01f, 0), 0xFF0000FFu }; lv[1] = (LineVertex){ v3(1, 0.01f, 0), 0xFF0000FFu };
                lv[2] = (LineVertex){ v3(0, 0.01f, 0), 0xFF00FF00u }; lv[3] = (LineVertex){ v3(0, 1, 0), 0xFF00FF00u };
                lv[4] = (LineVertex){ v3(0, 0.01f, 0), 0xFFFF0000u }; lv[5] = (LineVertex){ v3(0, 0.01f, 1), 0xFFFF0000u };
            }
            if (shot && max_frames && g_engine.frame_index == (u64)(max_frames - 1)) render_screenshot(shot);
            render_end_frame();
        }
        g_engine.frame_index++;
        if (max_frames && g_engine.frame_index >= (u64)max_frames) g_engine.quit = true;
    }
    render_shutdown();
    jobs_shutdown();
    SDL_DestroyWindow(g_engine.window);
    SDL_Quit();
    cvar_system_shutdown();
    log_shutdown();
    return 0;
}
