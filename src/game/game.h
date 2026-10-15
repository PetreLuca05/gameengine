// Game: owns the simulated world, the local player view, the fixed-timestep loop and
// glue between simulation, networking, presentation and UI.
//
// Model: host-authoritative. In single player / when hosting, this process runs the
// authoritative World and the local player's commands are applied directly. As a client,
// the World is a replica built from snapshots with the local player predicted.
#ifndef DG_GAME_H
#define DG_GAME_H

#include "core/base.h"
#include "core/cvar.h"
#include "world/world.h"
#include "player/camera.h"
#include "render/mesh_gen.h"
#include "gameplay/items.h"
#include "usercmd.h"

typedef enum GameMode { GM_NONE, GM_HOST, GM_CLIENT } GameMode;

#define CMD_HISTORY 128
#define MAX_HINTS 6
#define MAX_KILLFEED 5
#define MAX_HURT_MARKERS 8

typedef struct Hint { char text[64]; float time; } Hint;
typedef struct HurtMarker { vec3 from; float time; } HurtMarker;

typedef struct Game {
    GameMode mode;
    World *world;
    int local_slot;
    char map[64];
    BasicMeshes meshes;
    ItemDB items;

    // fixed timestep
    double accumulator;
    float alpha;
    u32 client_seq;

    // local view + command building
    ViewState view;
    u8 hotbar_slot;
    UserCmd last_cmd;
    vec2 frame_look;
    bool use_pressed_frame;

    // client prediction
    UserCmd cmd_history[CMD_HISTORY];
    PlayerMove predicted_prev;   // previous predicted tick (render interpolation)
    u32 last_ack_seq;
    float prediction_error;
    float recoil_pred_error, recoil_pred_error_max;   // predicted vs replayed recoil state (should be 0)
    vec3 error_offset;           // smoothed visual correction after reconciliation

    // interaction focus (local)
    EntityId focus;
    char focus_prompt[96];
    float hold_progress;         // 0..1 for hold interactions (from host state)

    // UI state
    bool menu_open;
    bool inventory_open;
    bool container_open;
    EntityId container_ent;
    InvSlot container_slots[CONTAINER_SLOTS];
    int inv_selected;            // slot picked up for moving (-1 none)

    // HUD state
    Hint hints[MAX_HINTS];
    Hint killfeed[MAX_KILLFEED];
    HurtMarker hurt[MAX_HURT_MARKERS];
    float hitmarker_time;
    bool hitmarker_kill, hitmarker_head;
    float damage_flash;

    bool noclip_request;
    bool show_netstats;

    // weapon feel (client-only presentation, stepped per tick, sampled with alpha)
    RecoilVisual recoil_vis;
    float spread_prev, spread_cur;   // bullet cone (deg) for the crosshair
    float bolt_timer;                // bolt / pump cycle sound pending
    int magdump_ticks;               // wpn_magdump: >0 while holding the trigger
    bool magdump_ads;
    float magdump_dist;
    int magdump_first_tick, magdump_last_tick, magdump_shots, magdump_last_mag;

    Cvar *g_map, *sv_hostname, *cl_name, *dev_mode, *r_drawhitboxes, *r_drawcollision;
    Cvar *cl_showfps, *net_autohost, *world_seed, *interact_range, *anim_rate;
} Game;

void game_init(void);
void game_shutdown(void);
bool game_host(const char *map);
void game_frame(float dt);
void game_render(void);
void game_disconnect_to_local(const char *reason);
Game *game(void);
World *game_world(void);
Player *game_local_player(void);
bool game_is_host(void);

// Called by the net layer.
int game_sv_player_join(int peer, const char *name);
void game_sv_player_leave(int slot);
void game_sv_receive_cmd(int slot, const UserCmd *cmd);
void game_cl_welcome(int slot, const char *map, u32 seed, u32 server_tick);
void game_cl_disconnected(const char *reason);
void game_handle_event(const GameEvent *ev);   // client-side event handling
void game_rpc(int type, int a, int b, int c);   // client -> host request (inventory, containers)
void game_sv_handle_rpc(int slot, int type, int a, int b, int c);

enum RpcType { RPC_INV_MOVE = 1, RPC_INV_DROP, RPC_CONTAINER_TAKE, RPC_CONTAINER_PUT, RPC_CONTAINER_CLOSE, RPC_INV_USE };

#endif
