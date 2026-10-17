// Deadground entry point and frame loop.
//
// Frame: events -> input -> console commands -> net poll -> fixed-step simulation (60 Hz,
// inside game_frame) -> render with interpolation -> UI -> present.
//
// Command line:
//   +<command> [args]     run a console command at startup (e.g. +map devroom +connect 127.0.0.1)
//   --exec <file>         execute a config/script file
//   --script <file>       queue a script into the command buffer (supports "wait N")
//   --log <path>          write the log there (tests next to a running game)
//   --frames <n>          quit after n frames;  --screenshot <path> saves the last frame
//   --validation          enable Vulkan validation layers;  --nosound  disable audio
//   --test                automated test mode: no config load/save, no audio, 60 fps cap
//   --rebuild-cache       re-bake every FBX (skeleton, meshes, clips) into assets/cache/
#include "core/engine.h"
#include "core/log.h"
#include "core/cvar.h"
#include "core/platform.h"
#include "core/jobs.h"
#include "core/crash.h"
#include "assets/anim_cache.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "physics/physics.h"
#include "input/input.h"
#include "audio/audio.h"
#include "ui/ui.h"
#include "ui/console.h"
#include "ui/menus.h"
#include "ui/vfx_editor.h"
#include "ui/holdpoint_editor.h"
#include "gameplay/phone/phone.h"
#include "game/game.h"
#include "net/net.h"
#include "net/lobby_net.h"
#include "ui/mainmenu/mainmenu.h"
#include "app/video.h"
#include "assets/assets.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdlib.h>
#include <stdio.h>

Engine g_engine;

typedef struct AppOptions { int max_frames; const char *screenshot; bool test; } AppOptions;

static void cmd_quit(int argc, const char **argv) { g_engine.quit = true; }

static void cmd_screenshot(int argc, const char **argv) {
    char path[512];
    if (argc > 1) dg_strcpy(path, sizeof(path), argv[1]);
    else snprintf(path, sizeof(path), "%sscreenshot_%llu.png", platform_pref_dir(), (unsigned long long)g_engine.frame_index);
    render_screenshot(path);
}

static void cmd_reload_shaders(int argc, const char **argv) { render_reload_shaders(); }
static void cmd_vid_restart(int argc, const char **argv) { video_apply(); }
static void cmd_writeconfig(int argc, const char **argv) {
    if (config_save(video_config_path())) log_info("config written to %s", video_config_path());
    else log_info("writeconfig: disabled in --test runs");
}

static void parse_args(int argc, char **argv, AppOptions *o) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--frames") && i + 1 < argc) o->max_frames = atoi(argv[++i]);
        else if (!strcmp(a, "--screenshot") && i + 1 < argc) o->screenshot = argv[++i];
        else if (!strcmp(a, "--test")) o->test = true;
        else if (!strcmp(a, "--exec") && i + 1 < argc) { char b[1024]; snprintf(b, sizeof(b), "exec \"%s\"", argv[++i]); cbuf_add(b); }
        else if (!strcmp(a, "--script") && i + 1 < argc) {
            size_t n = 0;
            char *text = platform_read_file(argv[++i], &n);
            if (text) { cbuf_add(text); free(text); }
            else log_error("cannot read script %s", argv[i]);
        } else if (a[0] == '+') {
            // +command arg arg ... up to the next +command / --option
            char line[1024];
            dg_strcpy(line, sizeof(line), a + 1);
            while (i + 1 < argc && argv[i + 1][0] != '+' && strncmp(argv[i + 1], "--", 2) != 0) {
                strncat(line, " ", sizeof(line) - strlen(line) - 1);
                strncat(line, argv[++i], sizeof(line) - strlen(line) - 1);
            }
            // +set applies at once: before the boot map hosts (e.g. +set net_port for parallel tests).
            if (!strncmp(line, "set ", 4)) cmd_execute_line(line);
            else cbuf_add(line);
        }
    }
}

static bool boot_commands_connect(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "+connect") || !strcmp(argv[i], "+map") || !strcmp(argv[i], "+connect_lobby") || !strcmp(argv[i], "+lobby_join") || !strcmp(argv[i], "+lobby_host")) return true;
    return false;
}

static void handle_event(const SDL_Event *ev) {
    switch (ev->type) {
    case SDL_EVENT_QUIT: g_engine.quit = true; return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: g_engine.has_focus = true; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_engine.has_focus = false;
        input_set_game_focus(false);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.repeat) break;
        if (ev->key.scancode == SDL_SCANCODE_GRAVE) {
            console_toggle();
            return;
        }
        if (ev->key.scancode == SDL_SCANCODE_ESCAPE && !input_capturing()) {
            if (console_is_open()) { console_set_open(false); return; }
            // The cursor on the HUD phone panel: Esc is the phone's back (docs/PHONE.md).
            if (!menus_open() && phone_ui_escape()) return;
            // No screen open: pause. Screens open: Esc is "back" for the UI (below).
            if (!menus_open()) { menus_toggle_pause(); return; }
        }
        if (ev->key.scancode == SDL_SCANCODE_F7 && !console_is_open() && !input_capturing()) {
            vfx_editor_toggle();
            input_set_game_focus(!vfx_editor_open() && !menus_wants_mouse());
            return;
        }
        if (ev->key.scancode == SDL_SCANCODE_F8 && !console_is_open() && !input_capturing()) {
            holdpoint_editor_toggle();
            input_set_game_focus(!menus_wants_mouse());
            return;
        }
        if (console_handle_event(ev)) return;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // Clicking back into the window recaptures the mouse when no UI wants it.
        if (!input_game_focus() && !menus_wants_mouse() && !console_is_open() && g_engine.has_focus) {
            input_set_game_focus(true);
            return;
        }
        break;
    default: break;
    }
    // Nuklear: the console and the developer editors. The game UI sees every event (it only acts
    // on them while a screen is open, or for its global keys: Start, inventory).
    if (console_is_open() || menus_dev_tools_open()) ui_handle_event(ev);
    else menus_handle_event(ev);
    input_handle_event(ev);
}

int main(int argc, char **argv) {
    g_engine.argc = argc;
    g_engine.argv = argv;
    AppOptions opt = { 0 };

    char log_path[512];
    SDL_SetAppMetadata("Deadground", DG_VERSION, "com.deadground.game");
    snprintf(log_path, sizeof(log_path), "%sdeadground.log", platform_pref_dir());
    // --log <path>: a separate log (scripted tests next to a running game).
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--log")) snprintf(log_path, sizeof(log_path), "%s", argv[i + 1]);
    log_init(log_path);
    crash_install();
    log_info("Deadground %s starting (log: %s)", DG_VERSION, log_path);
    cvar_system_init();
    arena_init(&g_engine.perm_arena, MB(16), "perm");
    arena_init(&g_engine.frame_arena, MB(32), "frame");
    arena_init(&g_engine.level_arena, MB(64), "level");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) log_fatal("SDL_Init failed: %s", SDL_GetError());
    jobs_init(0);
    phys_global_init();
    video_register_cvars();
    cmd_register("quit", cmd_quit, "exit the game");
    cmd_register("screenshot", cmd_screenshot, "screenshot [path]");
    cmd_register("r_reloadshaders", cmd_reload_shaders, "recompile + reload shaders");
    cmd_register("vid_restart", cmd_vid_restart, "re-apply window settings");
    cmd_register("writeconfig", cmd_writeconfig, "save config now");

    // Peek at archived video settings before creating the window.
    Cvar *cw = cvar_find("r_width"), *ch = cvar_find("r_height");
    g_engine.window = SDL_CreateWindow("Deadground - DevRoom", cw->i, ch->i, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_engine.window) log_fatal("SDL_CreateWindow failed: %s", SDL_GetError());
    if (!render_init(g_engine.window)) log_fatal("Vulkan renderer failed to initialize (Vulkan 1.3 GPU + driver required)");
    dbg_init();
    if (!ui_init()) log_fatal("UI init failed");
    console_init();
    menus_init();
    input_init();
    audio_init();
    net_init();
    assets_init("data/assets.json");
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--rebuild-cache")) anim_cache_set_rebuild(true);   // before the anim library loads
    game_init();
    lobby_net_init();    // maps, cosmetics, profile (docs/MENU_LOBBY.md)
    mainmenu_init();

    // Persistent settings, then the command line (overrides).
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--test")) opt.test = true;
    // Test runs never read or write the user's settings (they used to overwrite them on exit).
    config_set_enabled(!opt.test);
    bool first_run = false;
    cvar_reg_float("dev_fixed_fps", 0.0f, 0.0f, 1000.0f, 0, "frame time fixed to 1 / this (0 = real time): deterministic captures on slow machines");
    if (opt.test) cvar_set("r_maxfps", "60");
    else if (config_exec_file(video_config_path())) log_info("config loaded from %s", video_config_path());
    else { log_info("no config yet: settings will be saved to %s", video_config_path()); first_run = true; }
    game_fidelity_settings_loaded(first_run);   // Engine Fidelity: saved level + user choices in force
    input_bind_missing_defaults();
    config_clear_dirty();   // loading the config is not a change
    if (!opt.test) game_config_loaded();   // migrations are changes: saved on exit
    if (cvar_find("r_mode")->i != 0 || cw->i != 1600 || ch->i != 900) video_apply();
    parse_args(argc, argv, &opt);
    // Players start in the main menu (a +connect_lobby from a Steam invite then joins from it);
    // --test runs (scripts) boot straight into the boot map as before.
    if (mainmenu_enabled()) mainmenu_boot();
    else if (!boot_commands_connect(argc, argv)) game_host(game()->g_map->str);
    cbuf_execute();
    if (!game_world() && net_role() != NET_CLIENT) game_host("devroom");
    lobby_profile_changed();   // equipped cosmetics onto the character (cl_skin / cl_parts)

    g_engine.has_focus = true;
    input_set_game_focus(!menus_wants_mouse() && !console_is_open());   // the main menu keeps the cursor
    audio_play_music("ambient");

    Cvar *fixed_fps = cvar_find("dev_fixed_fps");
    double last = platform_time();
    double worst_dt = 0, total_time = 0, smoothed_dt = 0;
    while (!g_engine.quit) {
        double frame_start = platform_time();
        double dt = frame_start - last;
        last = frame_start;
        if (g_engine.frame_index > 5) { if (dt > worst_dt) worst_dt = dt; total_time += dt; }
        if (dt > 0.25) dt = 0.25;
        // Captures / tests on slow machines: every frame advances exactly 1 / dev_fixed_fps seconds
        // (one sim tick at 60), whatever the wall clock did.
        if (fixed_fps && fixed_fps->f > 0.0f) dt = 1.0 / fixed_fps->f;
        g_engine.real_time = frame_start;
        g_engine.frame_dt = dt;
        // Smooth the frame time (not 1/dt, which outliers dominate).
        if (dt > 0) { smoothed_dt = smoothed_dt > 0 ? smoothed_dt * 0.95 + dt * 0.05 : dt; g_engine.fps_smoothed = 1.0 / smoothed_dt; }
        arena_reset(&g_engine.frame_arena);

        input_begin_frame();
        ui_input_begin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) handle_event(&ev);
        // A screen that wants the cursor never runs with the mouse captured (whoever grabbed it).
        if (input_game_focus() && (menus_wants_mouse() || console_is_open())) input_set_game_focus(false);
        ui_input_end();
        input_end_frame((float)dt);
        if (input_consume_press(ACT_NETSTATS)) game()->show_netstats = !game()->show_netstats;

        cbuf_execute();
        config_autosave(video_config_path());
        menus_tick((float)dt);
        net_update((float)dt);
        lobby_net_update((float)dt);
        game_frame((float)dt);
        jobs_pump_completions(8);
        render_poll_shader_hot_reload();

        if (render_begin_frame()) {
            game_render();
            ui_render();
            if (opt.screenshot && opt.max_frames && g_engine.frame_index + 1 == (u64)opt.max_frames) render_screenshot(opt.screenshot);
            render_end_frame();
        } else {
            ui_render();
            platform_sleep_ms(10);
        }
        if (g_engine.frame_index < 6 || g_engine.frame_index == 50) log_debug("frame %llu done at %.3f", (unsigned long long)g_engine.frame_index, platform_time());
        g_engine.frame_index++;
        if (opt.max_frames && g_engine.frame_index >= (u64)opt.max_frames) g_engine.quit = true;

        int cap = video_max_fps();
        if (cap > 0) {
            double target = 1.0 / cap;
            double spent = platform_time() - frame_start;
            if (spent < target) platform_sleep_ms((u32)((target - spent) * 1000.0));
        }
    }

    if (g_engine.frame_index > 6) log_info("frames %llu, avg %.2f ms, worst %.1f ms", (unsigned long long)g_engine.frame_index, total_time * 1000.0 / (double)(g_engine.frame_index - 6), worst_dt * 1000.0);
    log_info("shutting down");
    if (config_save_if_dirty(video_config_path())) log_info("settings saved to %s", video_config_path());
    render_wait_idle();
    lobby_net_shutdown();   // leaves the lobby (others migrate), saves the profile
    game_shutdown();
    assets_shutdown();
    net_shutdown();         // Steam shut down here
    audio_shutdown();
    input_shutdown();
    console_shutdown();
    ui_shutdown();
    dbg_shutdown();
    render_shutdown();
    phys_global_shutdown();
    jobs_shutdown();
    SDL_DestroyWindow(g_engine.window);
    SDL_Quit();
    cvar_system_shutdown();
    log_shutdown();
    return 0;
}
