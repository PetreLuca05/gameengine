// Game: owns the simulated world, the local player view, the fixed-timestep loop and
// glue between simulation, networking, presentation and UI.
//
// Model: host-authoritative. In single player / when hosting, this process runs the
// authoritative World and the local player's commands are applied directly. As a client,
// the World is a replica built from snapshots with the local player predicted.
#ifndef DG_GAME_H
#define DG_GAME_H

#include "player/camera_fp.h"
#include "core/base.h"
#include "core/cvar.h"
#include "world/world.h"
#include "player/camera.h"
#include "render/mesh_gen.h"
#include "gameplay/items.h"
#include "gameplay/inv_ops.h"
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
    CamHeadStab cam_stab;        // first-person head camera stabilizer (src/player/camera_fp.c)
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
    EntityId use_focus;            // focus at this frame's use press (before the tick consumed it)
    vec3 use_focus_point;          // its focus point (pickups vanish inside the tick)
    int use_focus_hands;           // HandMode of its interaction (-1 none)
    char focus_prompt[96];
    float hold_progress;         // 0..1 for hold interactions (from host state)

    // UI state
    bool menu_open;
    bool phone_out;           // the built-in phone is taken out (X, BTN_PHONE)
    bool inventory_open;
    bool container_open;
    EntityId container_ent;
    bool bench_open;                 // weapon bench attachment UI (EV_BENCH)
    EntityId bench_ent;
    InvSlot container_slots[CONTAINER_SLOTS];
    int inv_selected;            // slot picked up for moving (-1 none)
    // inventory intents (client prediction)
    InvOp inv_pending[64];
    int inv_pending_count;
    u32 inv_seq;
    bool paused;                 // single player / host alone: the pause menu stops the simulation
    bool flashlight;             // local flashlight (F), sent as BTN_FLASHLIGHT
    float ads_prev;              // the local player's ADS progress before the last tick (render interpolation)

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
void game_config_loaded(void);   // after the config file: one-time settings migrations
void game_fidelity_settings_loaded(bool first_run);   // Engine Fidelity: after the config, before scripts
void game_shutdown(void);
bool game_host(const char *map);
void game_frame(float dt);
void game_render(void);
const char *game_cam_debug_text(void);   // cam_debug stats line (HUD)
void game_disconnect_to_local(const char *reason);
Game *game(void);
World *game_world(void);
Player *game_local_player(void);
float game_ads_vis(void);       // the viewed player's ADS progress interpolated to this frame (zoom, sight presentation)
// Spectating (docs/SPECTATE.md): whose eyes the first-person presentation uses. The watched player
// in first / third person, else the local player. Input, prediction and RPCs stay on local_slot.
int game_view_slot(void);
Player *game_view_player(void);
const UserCmd *game_view_cmd(void);   // the viewed player's latest command (local: G->last_cmd)
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
// Host: weapon attachment change (validated; RPC_ATTACH / the attach command / the weapon bench).
bool game_sv_attach(World *w, int slot, int inv_slot, int aslot, int att);

// Time of day (game_daynight.c, docs/DAYNIGHT.md): gameplay queries on the replicated world time.
typedef void (*TodEventFn)(u32 event, float hours, void *user);   // event: TodEvent bit
float world_time_hours(World *w);                // 0..24
int world_day_number(World *w);
bool world_is_night(World *w);                   // sun below -6 deg (civil twilight over)
float world_sun_elevation_deg(World *w);
float world_light_level(World *w, vec3 pos);     // 0..1: sun / moon (visible from pos) + sky + lamps
void daynight_add_listener(TodEventFn fn, void *user);   // sunrise / sunset / midnight / noon on every machine

// Weather (game_weather.c, docs/WEATHER.md): queries on the replicated weather. Events fire on
// every machine from the evaluated parameters (no network traffic of their own).
typedef enum WeatherEvent { WEV_RAIN_START, WEV_RAIN_STOP, WEV_SNOW_START, WEV_SNOW_STOP, WEV_STORM_START, WEV_STORM_STOP, WEV_LIGHTNING, WEV_COUNT } WeatherEvent;
typedef void (*WeatherEventFn)(WeatherEvent ev, void *user);
typedef struct WeatherLocal {
    float exposed;       // 0 under a roof .. 1 open sky
    bool indoor;         // exposed < 0.3
    float rain, snow;    // precipitation reaching pos 0..1
    vec3 wind;           // m/s (sheltered spots get less)
    float temperature;   // C
    float wetness, snow_cover, fog;
} WeatherLocal;
WeatherLocal weather_at(World *w, vec3 pos);
float weather_exposure(World *w, vec3 pos);      // cached upward casts (1 m cells, 5 s)
float weather_grip(World *w, vec3 pos);          // ground grip multiplier 0.4..1 (wet / puddles / snow)
float weather_hearing_scale(World *w);           // rain / wind mask sounds (AI hearing)
float weather_puddle_mask_at(float x, float z);  // the baked puddle mask 0..1
void weather_add_listener(WeatherEventFn fn, void *user);
const char *weather_event_name(WeatherEvent e);
const char *weather_name(World *w);              // the current preset ("clear", "light_rain" ...; photo sidecars)

enum RpcType {
    RPC_INV_MOVE = 1, RPC_INV_DROP, RPC_CONTAINER_TAKE, RPC_CONTAINER_PUT, RPC_CONTAINER_CLOSE, RPC_INV_USE, RPC_ATTACH, RPC_INV_OP, RPC_SET_PART,
    RPC_SPECTATE,    // a = SpecMode, b = target slot or SpecTargetReq, c = 1 dev (alive; host / sv_cheats)
    RPC_RESPAWN,     // respawn request (sv_respawn_mode 1)
    RPC_SPEC_POS,    // free camera position x 16 (relevance)
};

// Inventory intents (gameplay/inv_ops.h, docs/UI.md): applied at once on the host; a client predicts
// them on its copy and re-applies the unacknowledged ones on every inventory the host sends.
struct InvOp;
int game_inv_op(struct InvOp *op);   // returns the (predicted) InvOpResult
void game_cl_inventory(int slot, u32 revision, u32 ack_seq, const InvSlot *slots);
int game_inv_pending(void);          // client: intents not yet acknowledged

#endif
