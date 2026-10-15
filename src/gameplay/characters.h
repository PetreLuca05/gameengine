// Skinned characters in the world (players, zombies, dummies) on the shared skeleton.
//
// Host:    sim-only animation for every character every tick (controller + base pose, no IK,
//          no skinning) -> per-part hitboxes + lag-compensation history -> replicated state.
// Clients: run the same controller from the replicated parameters (interpolated), snap to the
//          host's state ids on divergence, and add every cosmetic layer locally.
// Render:  LOD-scaled post-pass (damage additive, spine, hold + glue, foot IK) at tick rate,
//          interpolated between the last two post-IK poses (hands + item move together).
#ifndef DG_CHARACTERS_H
#define DG_CHARACTERS_H

#include "world/world.h"
#include "anim/character_rt.h"
#include "anim/anim_net.h"
#include "anim/anim_lod.h"
#include "gameplay/hitboxes.h"
#include "physics/ragdoll.h"

#define MAX_CHARACTERS 256
#define CHAR_NET_SAMPLES 8

typedef enum ZombieMode { ZM_IDLE, ZM_WANDER, ZM_ATTACK } ZombieMode;

typedef struct ZombieAI {
    u8 mode;
    float timer;
    vec3 home, target;
    float wander_radius;
    float yaw;
    float speed;
    float attack_t;
} ZombieAI;

typedef struct NetSample { u32 tick; AnimNetState s; } NetSample;

typedef struct CharacterInst {
    EntityId ent;
    const CharacterDef *def;
    int def_index;
    CharRuntime rt;
    // simulation (host) / replicated input state
    Xform root;                       // feet + body yaw (world)
    float body_yaw;
    HitboxSet hit;                    // current hitboxes (host: sim pose; client: local pose)
    HitboxHistory *history;           // host: lag compensation ring
    AnimNetState net;                 // host: captured each tick; client: latest received
    u32 state_enter_tick[2];
    i16 last_state[2];
    NetSample samples[CHAR_NET_SAMPLES];
    int sample_count, sample_head;
    int mismatch_ticks, resyncs;
    bool synced;                      // client: controller adopted the host's states once (join / spawn)
    u32 last_land_tick;
    bool was_grounded, was_reloading;
    float last_equip_t;
    float fp_pitch[2], fp_yaw[2], fp_body_yaw[2];   // aim used by the last two post-passes (prev, cur)
    // render (any machine that draws)
    Xform *pose_prev, *pose_cur;      // post-IK bone world transforms
    Xform item_prev, item_cur;
    Xform item_cam_prev, item_cam_cur;   // hold point item in camera space (owner re-places per frame)
    bool item_valid;
    const struct WeaponDef *item_weapon;
    const struct ItemDef *item_def;
    bool have_pose;
    int lod, fade_from;
    float lod_fade;
    float pixels;
    u32 last_eval;
    int interval;
    bool culled;
    vec3 kick_x, kick_v;              // remote recoil kick on the item (cosmetic)
    // death / ragdoll
    Ragdoll rag;
    bool dead, rag_pending, settled;
    bool tool_rag;                    // ragdolled by a dev tool (not dead): no auto revive
    u32 death_tick;
    u8 death_seed, death_part;
    vec3 death_impulse;
    Xform *settled_pose;
    // host gameplay
    float leg_t, arm_t;
    u8 limb;
    float predicted_hit_t;
    int predicted_hit_part;
    ZombieAI ai;
    bool is_player, local;
    int slot;
    u32 events_received[AEV_COUNT];
} CharacterInst;

typedef struct CharacterSystem {
    CharacterInst *c[MAX_CHARACTERS];
    int count;
    u16 index[MAX_ENTITIES];          // entity index -> character slot + 1
    int ragdolls;
    u32 tick;
    // Clients: reliable events wait until the interpolated render tick reaches their host tick,
    // so a death / hit lines up with the replicated state stream (no transient mismatch).
    AnimEvent pending[256];
    int pending_count;
} CharacterSystem;

void characters_register_cvars(void);
void characters_shutdown_globals(void);
void chars_system_create(World *w);
void chars_system_destroy(World *w);
CharacterInst *char_attach(World *w, EntityId e, u8 character_id);
void char_detach(World *w, EntityId e);
CharacterInst *char_get(const World *w, EntityId e);
// Visual muzzle of the item a character holds (world space, barrel forward). VFX for remote shooters.
bool chars_item_muzzle(World *w, EntityId e, vec3 *pos, vec3 *dir);
// Eye (camera socket) of a character's current pose, ragdoll included (death camera).
bool chars_head_world(World *w, EntityId e, float alpha, vec3 *out);
// First person: this frame's final camera (the held item is placed relative to it).
void chars_set_fp_camera(vec3 pos, quat rot, bool valid);
// Last first-person frame: hand <-> socket error ([0] left, [1] right; m / deg) and ADS sight error.
void chars_fp_diag(float glue_pos[2], float glue_ang[2], float *ads_pos, float *ads_ang);
const char *chars_holdpoint_text(World *w, int local_slot);   // "hold <state> <blend>% pull .. reach .."
CharacterInst *char_get_index(const World *w, int ent_index);

// Host: sim-only animation for every character (call after movement / weapons, before lag-comp).
void chars_host_tick(World *w, float dt);
// Gameplay notifications (host + local prediction).
void chars_notify_attack(World *w, int player_slot, bool melee, bool first_predict);
// Clients: replicated state from snapshots, then one tick of every replicated character.
void chars_client_receive(World *w, EntityId e, const AnimNetState *s, u32 tick);
void chars_client_tick(World *w, double render_tick, float dt);
// Local predicted player (client): controller follows the predicted PlayerSim.
void chars_local_predict(World *w, int slot, bool first);
// Render side, once per sim tick: LOD, post-pass, ragdolls.
void chars_present_tick(World *w, vec3 cam_pos, float fov_deg, float screen_h, float dt, int local_slot);
void chars_render(World *w, float alpha, vec3 cam_pos, int local_slot);
// First-person body of the local player (cl_fp_body): call after hands_update, before
// hands_render. Draws the body (head hidden) and its arms on the first-person hand targets.
void chars_render_local_fp(World *w, float alpha, int local_slot);
void chars_set_thirdperson(bool on);   // debug: draw the local player like everyone else
// Eye offset from the spine bend for an aim (yaw, pitch): added to the gameplay eye.
vec3 chars_eye_offset(float yaw, float pitch);
// First-person camera socket (Unreal-style camera on a head socket): prepares this frame's body
// pose with the spine aimed at (yaw, pitch) and returns the "camera" socket's world transform.
// The camera takes the position; its rotation stays the view (pawn control rotation).
bool chars_fp_bone_world(int role, Xform *out);
float chars_fp_hold_weight(void);
bool chars_fp_item_cam(Xform *item_off, int *state, float *blend);   // rendered item relative to its hold point (false: no hold)                 // 1 = the first-person hold IK owns both hands this frame   // this frame's first-person pose (tests)
bool chars_fp_camera(World *w, float alpha, int local_slot, float yaw, float pitch, Xform *socket_world);
// Events (reliable, compact): host queues + applies, clients apply what arrives.
void chars_push_event(World *w, const AnimEvent *e);
void chars_on_event(World *w, const AnimEvent *e);
const AnimNetState *chars_net_state(const World *w, int ent_index);
// Host damage hook: limb status, hit / death events. dir = shot direction (world).
void chars_on_damage(World *w, EntityId target, float dealt, int part, vec3 dir, vec3 point, bool killed, int damage_type);
// Hit tests against characters (host: rewound to view_tick256 when > 0; clients: current pose).
bool chars_raycast(World *w, EntityId ignore, vec3 origin, vec3 dir, float range, u32 view_tick256, EntityId *ent, HitboxRayHit *hit);
bool chars_sweep(World *w, EntityId ignore, vec3 a, vec3 b, float radius, EntityId *ent, HitboxRayHit *hit);
// Client-side predicted (unconfirmed) reaction on a target the local player hit.
void chars_predict_hit(World *w, EntityId target, int part, vec3 dir);
// Zombies (host).
EntityId spawn_zombie(World *w, vec3 feet, float yaw, u8 character_id);
void zombies_attack(World *w);
// Extra spread (degrees) from arm injuries.
float chars_limb_spread(const World *w, EntityId e);
// Debug / tools.
void chars_debug_draw(World *w, vec3 cam_pos);
void chars_force_lod(int lod);
bool chars_ragdoll_now(World *w, EntityId e, vec3 impulse);
bool chars_ragdoll_release(World *w, EntityId e);   // stands a tool ragdoll back up
int chars_count(const World *w);
void chars_budget_stats(int *evals, int *deferred);   // last present tick: post-passes run / deferred by anim_eval_budget
const LodConfig *chars_lod_config(void);

#endif
