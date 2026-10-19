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
#include "gameplay/inventory.h"
#include "gameplay/kill_stats.h"
#include "gameplay/ping.h"
#include "usercmd.h"

typedef enum GameMode { GM_NONE, GM_HOST, GM_CLIENT } GameMode;

#define CMD_HISTORY 128
#define MAX_HINTS 6
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
    u8 sel_slot;                 // selection (UserCmd.slot): INV_SEL_* (docs/INVENTORY.md "Selection")
    u8 sel_weapon;               // the last weapon slot selected (the hands fall back to it)
    bool attack_wait_release;    // fire masked until released (the hands fell back to a weapon mid-press)
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
    // Grid inventory (docs/INVENTORY.md). Client: the host's last trees (bases) and the predicted
    // copies (bases + the unacknowledged intents re-applied). The host's own player uses the real trees.
    Inventory inv_base;
    bool inv_base_valid;
    InvTree loot;                // the open world inventory (predicted)
    InvTree loot_base;
    EntityId loot_ent;           // ENT_NONE: no tree received yet
    u32 loot_rev;
    InvIntent inv_pending[64];
    int inv_pending_count;
    u32 inv_seq;
    bool paused;                 // single player / host alone: the pause menu stops the simulation
    bool flashlight;             // local flashlight (F), sent as BTN_FLASHLIGHT
    float ads_prev;              // the local player's ADS progress before the last tick (render interpolation)

    // HUD state
    Hint hints[MAX_HINTS];
    KillStats kill_stats;        // the local player's kills (docs/UI_WHEELS_HUD.md "Kill counter")
    KillFeed kill_feed;
    PingList pings;              // live pings (gameplay/ping.h), from EV_PING on every machine
    u32 ping_uid;                // host: the next ping uid
    PingLimiter ping_limit;      // host: per-player rate limit
    float hud_time;              // HUD clock (seconds since the map loaded; pings, streaks)
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
    Cvar *cl_showfps, *net_autohost, *world_seed, *interact_range, *anim_rate, *host_timescale;
} Game;

void game_init(void);
void game_config_loaded(void);   // after the config file: one-time settings migrations
void game_fidelity_settings_loaded(bool first_run);   // Engine Fidelity: after the config, before scripts
void game_shutdown(void);
bool game_host(const char *map);
enum { GH_KEEP_NET = 1u << 0, GH_NO_AUTOHOST = 1u << 1 };
bool game_host_ex(const char *map, u32 flags);
void game_frame(float dt);
void game_render(void);
const char *game_cam_debug_text(void);   // cam_debug stats line (HUD)
void game_disconnect_to_local(const char *reason);
Game *game(void);
World *game_world(void);
Player *game_local_player(void);
// Performance audit runs (game_perf.c, docs/PERF_AUDIT.md): perf_audit / perf_verify.
void game_perf_init(void);
void game_perf_frame(void);   // main loop, after the command buffer
float game_ads_vis(void);       // the viewed player's ADS progress interpolated to this frame (zoom, sight presentation)
int game_cam_mode(void);        // this frame's world camera: 0 fallback eye, 1 head camera (first person), 2 death, 3 third person / free / vehicle orbit
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
void game_select(int sel);                      // the local selection (INV_SEL_*): keys 1-3, the wheels, `slot`
const PingTypes *game_ping_types(void);         // assets/ui/wheels/ping.json (game_pings.c)
void wheels_draw(void);                         // radial wheels + the HUD timed ring (game_wheels.c)
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
void daynight_add_listener(TodEventFn fn, void *user);
// Auto-exposure range of the day / night cycle (EV100): the night floor and the day maximum.
void daynight_ev_range(float *night, float *day);
float daynight_daylight(void);   // 0 night .. 1 full sun (eye adaptation for exposure-aware effects)   // sunrise / sunset / midnight / noon on every machine

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
    RPC_INV_DROP = 1,      // a = selection (INV_SEL_*): the held item on the ground (G tap); inventory changes are MSG_INV_INTENT
    RPC_CONTAINER_CLOSE,   // the inventory screen closed: the open world inventory too
    RPC_ATTACH,            // a = inventory slot, b = attachment slot, c = attachment id (bench)
    RPC_SET_PART,          // modular character part
    RPC_SPECTATE,    // a = SpecMode, b = target slot or SpecTargetReq, c = 1 dev (alive; host / sv_cheats)
    RPC_RESPAWN,     // respawn request (sv_respawn_mode 1)
    RPC_SPEC_POS,    // free camera position x 16 (relevance)
    RPC_DOOR_ACTION, // a = door entity, b = DoorAction (docs/UI_WHEELS_HUD.md "Door wheel")
    RPC_PING,        // a / b / c = ping_pack (gameplay/ping.h)
};

// Inventory intents (gameplay/inventory/inv_intent.h, docs/INVENTORY.md): applied at once on the
// host; a client predicts them on its copies and re-applies the unacknowledged ones on every
// inventory / loot tree the host sends.
int game_inv_intent(InvIntent *in);   // returns the (predicted) InvResult; fills in->seq
int game_inv_pending(void);           // client: intents not yet acknowledged
void game_cl_inventory(int slot, u32 revision, u32 ack_seq, u16 hands, const InvTree *tree);
void game_cl_loot(EntityId e, u32 revision, const InvTree *tree);   // tree NULL: closed
// The open world inventory as this machine sees it (host: the real tree; client: the predicted
// copy), NULL when none is open / not received yet.
InvTree *game_loot_tree(void);
EntityId game_loot_ent(void);
void game_sv_inv_intent(int slot, const InvIntent *in);   // host: validate + apply + world effects
int game_sv_inv_intent_r(int slot, const InvIntent *in);  // the same, returning the InvResult
void game_sv_loot_close(int slot);
float game_loot_reach(World *w, EntityId e);   // sv_inv_reach (+ 1 m for vehicles)
void game_loot_register(void);

#endif
