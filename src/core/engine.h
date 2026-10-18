// The single global engine context. Every subsystem's state hangs off g_engine;
// modules keep no other mutable globals.
#ifndef DG_ENGINE_H
#define DG_ENGINE_H

#include "base.h"
#include "arena.h"

struct SDL_Window;

typedef struct Engine {
    // core services (opaque, owned by their modules)
    struct LogState *log;
    struct CvarSystem *cvars;
    struct JobSystem *jobs;

    // subsystems
    struct SDL_Window *window;
    struct Renderer *render;
    struct InputSystem *input;
    struct AudioSystem *audio;
    struct UiSystem *ui;
    struct NetSystem *net;
    struct Game *game;
    struct Console *console;
    struct DebugDraw *debug;
    struct PhysicsGlobal *physics;
    struct AssetSystem *assets;
    struct FontSystem *fonts;
    struct PlayerTuning *ptuning;
    struct ItemDB *items;
    struct FxSystem *fx;
    struct HandsSystem *hands;
    struct CameraCvars *camera_cvars;
    struct GameRender *game_render;
    struct ChunkGlobals *chunk_globals;
    struct Menus *menus;
    struct VideoState *video;
    struct AnimLibrary *anim;         // shared skeleton + animation assets (src/anim/anim_library.h)
    struct CharGlobals *chars;        // character system cvars / GPU meshes (src/gameplay/characters.h)

    Arena perm_arena;   // lives for the whole run
    Arena frame_arena;  // reset every rendered frame
    Arena level_arena;  // reset on level load

    // timing
    double real_time;     // seconds since boot
    double render_time;   // shader / cosmetic animation clock: real_time, or under dev_fixed_fps the sum of the
                          // fixed steps (captures and perf_verify passes render the same time values)
    double frame_dt;      // last render frame duration (clamped)
    u64 frame_index;
    float sim_alpha;      // interpolation factor between previous and current sim tick
    double fps_smoothed;

    int window_w, window_h;
    bool quit;
    bool has_focus;
    bool headless;        // dedicated / test mode without window

    int argc;
    char **argv;
} Engine;

extern Engine g_engine;

#define SIM_HZ 60
#define SIM_DT (1.0f / (float)SIM_HZ)

#endif
