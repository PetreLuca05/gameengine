// World: handle-based entity table + component arrays, players, static level geometry.
// EntityIds are stable across the network: the host allocates them and clients mirror
// the same index/generation in their replica worlds.
#ifndef DG_WORLD_H
#define DG_WORLD_H

#include "core/base.h"
#include "core/dgmath.h"
#include "physics/physics.h"
#include "render/render.h"
#include "gameplay/items.h"
#include "gameplay/inventory.h"
#include "world/time_of_day.h"
#include "world/weather.h"
#include "player/player.h"
#include "gameplay/events.h"

#define MAX_ENTITIES 4096
#define MAX_PLAYERS 8
#define CLIENT_ENTITY_BASE 3584     // indices >= this are client-local (never replicated)
#define LAGCOMP_TICKS 64

typedef u32 EntityId;
#define ENT_NONE 0u
#define ENT_INDEX(e) ((e) & 0xFFFu)
#define ENT_GEN(e) ((e) >> 12)
#define ENT_MAKE(i, g) ((((u32)(g)) << 12) | ((u32)(i) & 0xFFFu))

typedef enum EntityType {
    ET_NONE, ET_PLAYER, ET_PROP, ET_DOOR, ET_GATE, ET_LEVER, ET_SWITCH, ET_LIGHT,
    ET_CONTAINER, ET_PICKUP, ET_DUMMY, ET_PROJECTILE, ET_MARKER, ET_ZOMBIE, ET_BENCH, ET_VEHICLE, ET_FUEL_PUMP, ET_COUNT
} EntityType;

enum EntityFlags {
    EF_HIDDEN    = 1 << 0,
    EF_LOCKED    = 1 << 1,
    EF_ON        = 1 << 2,   // lever / switch / light state
    EF_OPEN      = 1 << 3,
    EF_DEAD      = 1 << 4,
    EF_CROUCHED  = 1 << 5,
    EF_SPRINT    = 1 << 6,
    EF_FIRING    = 1 << 7,
    EF_RELOADING = 1 << 8,
    EF_ADS       = 1 << 9,
    EF_HANG      = 1 << 10,
    EF_MANTLE    = 1 << 11,
    EF_STATIC_BODY = 1 << 12,
    EF_FLASHLIGHT = 1 << 13,  // players: flashlight on
    EF_SEATED    = 1 << 14,   // players: in a vehicle seat (game_vehicles.c)
    EF_HEADLIGHTS = 1 << 15,  // vehicles: headlights on
    EF_BRAKING   = 1 << 16,   // vehicles: brake lights
    EF_WRECKED   = 1 << 17,   // vehicles: destroyed (no engine)
    EF_ENGINE    = 1 << 18,   // vehicles: engine running
};

enum ComponentBits {
    C_VISUAL = 1 << 0, C_BODY = 1 << 1, C_HEALTH = 1 << 2, C_INTERACT = 1 << 3, C_DOOR = 1 << 4,
    C_LEVER = 1 << 5, C_LIGHT = 1 << 6, C_CONTAINER = 1 << 7, C_PICKUP = 1 << 8, C_PROJECTILE = 1 << 9,
    C_PLAYER = 1 << 10, C_HITBOX = 1 << 11,
};

typedef struct Transform {
    vec3 pos;
    quat rot;
    vec3 prev_pos;   // previous sim tick (render interpolation)
    quat prev_rot;
} Transform;

typedef struct Visual {
    u8 shape;        // VisShape
    u8 emissive;     // 0..255 emissive strength
    u16 mat_flags;
    vec3 half;       // box half extents, or (radius, half_height, radius)
    u32 color;       // RGBA8
    u16 model;       // asset model id (data/assets.json), 0 = primitive shape
    u16 pad;
} Visual;

typedef struct BodyComp {
    BodyId body;
    PhysMotion motion;
    float mass;
} BodyComp;

typedef struct Health {
    float hp, max_hp;
    float armor, max_armor;
    float resist[DMG_COUNT];  // multiplier per damage type (1 = normal)
    float respawn_timer;
    float last_damage_time;
    float regen_delay;        // dummies: refill after this many seconds without damage
} Health;

typedef struct World World;

typedef enum HandMode { HANDS_RIGHT, HANDS_BOTH, HANDS_POINT } HandMode;

// Interactable callbacks. on_focus runs on clients (prompt text), on_use/on_hold run on the host.
typedef struct InteractDef {
    const char *name;
    void (*on_focus)(World *w, EntityId self, int player, char *prompt, int prompt_size);
    bool (*can_use)(World *w, EntityId self, int player);
    void (*on_use)(World *w, EntityId self, int player);
    void (*on_hold)(World *w, EntityId self, int player, float progress); // progress 0..1, <0 cancelled
    // World-space point/orientation the given hand (0 = left, 1 = right) should reach for.
    bool (*hand_target)(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot);
    float hold_time;
    HandMode hands;
} InteractDef;

typedef struct Interactable {
    const InteractDef *def;
    float range;
    vec3 focus_offset;    // local offset of the focus point
    bool enabled;
} Interactable;

typedef struct Door {
    vec3 hinge;           // world-space hinge (or closed position for sliding gates)
    quat closed_rot;
    vec3 closed_pos;
    float open_angle;     // radians (swing doors) / height (gates)
    float speed;          // progress per second
    float target;         // 0 closed .. 1 open
    float progress;
    float swing_sign;     // +1/-1 chosen when opened (opens away from the user)
    u16 key_item;
    bool sliding;
    vec3 knob_local;      // knob position relative to door center (closed frame)
    vec3 visor_local;     // door viewer (peephole) at eye height, panel centre (docs/UI_WHEELS_HUD.md "Peek")
    float speed_mult;     // > 1 while slamming (back to 1 when it stops)
} Door;

typedef struct Lever {
    EntityId target;      // gate or door this lever drives
    char target_name[24];
    float progress;       // handle animation 0..1
    u32 light_group;      // switches: light group toggled
} Lever;

// LightComp.group bit: the lamp casts no local shadow (open-world street lamps; replicated with the group).
#define LIGHT_GROUP_NO_SHADOW (1u << 31)
typedef struct LightComp {
    vec3 color;
    float radius, intensity;
    u32 group;
    bool scheduled;       // light_schedule: on at dusk / off at dawn (time of day), per-light offset
    float flicker;        // 0..1 cosmetic flicker (replicated in NetEnt.extra.x)
    bool sched_state;     // host: last scheduled state (a switch may override until the next change)
} LightComp;

typedef struct Container {
    InvSlot slots[CONTAINER_SLOTS];
    float lid;            // 0 closed .. 1 open (animation)
    bool opened;
    u32 revision;
} Container;

typedef struct Pickup {
    u16 item;
    u16 count;
    i16 ammo;
    u8 att[5];            // weapon attachments (ATT_SLOTS, gameplay/attachments.h)
    float bob_phase;
    float respawn_time;   // >0: respawns after being taken (dev convenience)
    float respawn_timer;
    vec3 home;            // spawn point (respawns fall from here again)
    float rest_y;         // resting height of the entity origin (model on the surface below)
    float fall_vel;       // m/s, while falling to rest_y
    bool settled;
} Pickup;

typedef struct Projectile {
    vec3 vel;
    EntityId owner;
    int weapon;
    float life;
    // Throwables (gameplay/throwables.c): item != 0.
    u16 item;
    float fuse;           // s until it goes off (grenades)
    float burn;           // s left burning (flares, molotov fire)
    bool resting;
} Projectile;

typedef struct Hitbox {
    float radius, height;     // capsule standing on the entity position
    float head_radius;
    float head_height;        // center of head sphere above feet
} Hitbox;

typedef struct LagRecord { vec3 pos; float height; u8 valid; } LagRecord;

typedef struct Player {
    bool active;
    bool local;               // controlled by this machine
    EntityId ent;
    char name[32];
    int peer;                 // transport peer id (host side), -1 for local
    PlayerSim sim;
    struct PhysCharacter *character;
    Inventory inv;
    u32 inv_sent_revision;
    u32 inv_ack_seq;          // host: the last RPC_INV_OP sequence applied (sent with MSG_INVENTORY)
    UserCmd last_cmd;
    u32 last_processed_seq;
    // command queue (host side)
    UserCmd queue[32];
    int queue_count;
    // interaction (host side)
    EntityId hold_target;
    float hold_progress;
    float hold_time;
    // stats
    int kills, deaths;
    float respawn_timer;
    u32 spawn_index;
    EntityId open_container;
} Player;

// cull_dist > 0: skipped beyond that distance from cull_center (open-world detail tiles).
typedef struct StaticDraw { MeshId mesh; Material mat; mat4 model; vec3 cull_center; float cull_dist; } StaticDraw;
typedef struct WorldLabel { vec3 pos, right, up; float size; u32 color; bool centered; char text[40]; } WorldLabel;
typedef struct SpawnPoint { vec3 pos; float yaw; } SpawnPoint;

#define MAX_STATIC_DRAWS 4096
#define MAX_LABELS 1024
#define MAX_SPAWNS 16
#define MAX_STATIC_BODIES 8192
#define MAX_LEVEL_VGEO 64

// Virtualized geometry placement (level entity "vgeo", docs/VGEO.md "Levels"): a mesh (a .vgeo file
// path or a model name with "vgeo": true in data/assets.json) placed once or on a grid. Render-only
// (no collision); the renderer registers the instances (render_vgeo_set_level).
typedef struct LevelVgeo {
    char mesh[96];
    vec3 pos, color;
    float yaw, scale, scale_jitter, spacing, roughness;
    u32 grid[2], seed;
} LevelVgeo;   // collision: "collision": "mesh" (coarsest LOD, default for single placements), "sphere" (grids), "none"

typedef struct LevelStatic {
    char name[32];
    StaticDraw draws[MAX_STATIC_DRAWS];
    int draw_count;
    BodyId bodies[MAX_STATIC_BODIES];
    int body_count;
    WorldLabel labels[MAX_LABELS];
    int label_count;
    SpawnPoint spawns[MAX_SPAWNS];
    int spawn_count;
    vec3 bounds_min, bounds_max;
    vec3 ambient_sky, ambient_ground, fog_color;
    float fog_density;
    vec3 sun_dir, sun_color;
    float sun_intensity;
    bool sky;                  // procedural sky (open levels): env "sky_zenith" / "sky_horizon" / "clouds"
    vec3 sky_zenith, sky_horizon;
    float clouds;
    u32 seed;
    LevelVgeo vgeo[MAX_LEVEL_VGEO];
    int vgeo_count;
} LevelStatic;

struct World {
    bool authority;            // true on the host
    u32 tick;
    PhysicsWorld *phys;
    const ItemDB *items;

    u16 gen[MAX_ENTITIES];
    u8 alive[MAX_ENTITIES];
    u8 type[MAX_ENTITIES];
    u32 flags[MAX_ENTITIES];
    u32 comps[MAX_ENTITIES];
    char names[MAX_ENTITIES][24];
    float anim[MAX_ENTITIES];       // generic replicated 0..1 animation value
    float anim_prev[MAX_ENTITIES];
    u16 item[MAX_ENTITIES];         // replicated item id (pickups, player's weapon)
    u8 attach[MAX_ENTITIES][5];     // replicated attachment ids of that item (ATT_SLOTS, per slot delta)
    int max_used;                   // highest used index + 1

    Transform xf[MAX_ENTITIES];
    Visual vis[MAX_ENTITIES];
    BodyComp body[MAX_ENTITIES];
    Health health[MAX_ENTITIES];
    Interactable inter[MAX_ENTITIES];
    Door door[MAX_ENTITIES];
    Lever lever[MAX_ENTITIES];
    LightComp light[MAX_ENTITIES];
    Container cont[MAX_ENTITIES];
    Pickup pickup[MAX_ENTITIES];
    Projectile proj[MAX_ENTITIES];
    Hitbox hitbox[MAX_ENTITIES];
    u8 player_slot[MAX_ENTITIES];
    u8 char_id[MAX_ENTITIES];       // character definition (character_id) for players / zombies / dummies
    float life[MAX_ENTITIES];       // >0: host destroys the entity when it runs out (dropped items)

    LagRecord lag[LAGCOMP_TICKS][MAX_ENTITIES]; // hitbox entity positions per tick (lag compensation)

    Player players[MAX_PLAYERS];
    LevelStatic level;
    bool streaming;            // chunk-streamed world instead of a fixed level
    struct ChunkStreamer *chunks;
    void *openworld;           // open-world level state (src/world/openworld.c), freed by openworld_free
    void (*openworld_free)(struct World *w);

    GameEvent events[MAX_PENDING_EVENTS];   // authority: produced this tick, drained by net/game
    int event_count;
    struct AnimEvent *anim_events;          // authority: compact animation events this tick (reliable)
    int anim_event_count;
    struct CharacterSystem *chars;          // skinned characters (players, zombies, dummies)
    float time;                // seconds of simulated time
    TimeState tod;             // time of day: host-authoritative, in every snapshot (world/time_of_day.h)
    void *vehicles;            // vehicle system (src/game/game_vehicles.c), freed by vehicles_free
    void (*vehicles_free)(struct World *w);
    WeatherState weather;      // weather: host-authoritative, in snapshots on changes + every 10th (world/weather.h)
};

World *world_create(bool authority, const ItemDB *items);
void world_destroy(World *w);
void world_clear(World *w);

EntityId ent_spawn(World *w, EntityType type);
EntityId ent_spawn_at(World *w, EntityId id, EntityType type); // client replication
EntityId ent_spawn_local(World *w, EntityType type);           // client-only entity
void ent_destroy(World *w, EntityId e);
bool ent_valid(const World *w, EntityId e);
EntityId ent_find_by_name(const World *w, const char *name);
EntityId ent_from_index(const World *w, int index);
const char *ent_type_name(EntityType t);

void ent_set_pos(World *w, EntityId e, vec3 pos, quat rot, bool teleport);
vec3 ent_focus_point(const World *w, EntityId e);
void ent_create_body(World *w, EntityId e, PhysMotion motion, PhysLayer layer, float mass);
void ent_sync_from_body(World *w, EntityId e);

int world_player_by_ent(const World *w, EntityId e);
void world_store_prev(World *w); // copy current transforms to prev (start of tick)
void world_record_lagcomp(World *w);
bool world_lagcomp_get(const World *w, EntityId e, u32 tick, vec3 *pos);

#define ENT_FOREACH(w, i) for (int i = 1; i < (w)->max_used; i++) if ((w)->alive[i])

#endif
