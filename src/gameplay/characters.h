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
#include "render/lod_fade.h"
#include "anim/fire_anim.h"
#include "gameplay/hitboxes.h"
#include "gameplay/aim.h"
#include "physics/ragdoll.h"

#define MAX_CHARACTERS 2048   // hordes (src/gameplay/zombies)
#define CHAR_NET_SAMPLES 8
#define ZOMBIE_ATTACK_TIME 1.4f   // a zombie stands for its attack (the clips run ~1.3-1.5 s)

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
    float wet;                        // 0..1 soaked (docs/WATER.md): from the swim / wade flags, dries over ~90 s
    // render (any machine that draws)
    Xform *pose_prev, *pose_cur;      // post-IK bone world transforms
    Xform *pose_draw;                 // the pose drawn this frame (interpolated / ragdoll): skinned decals
    u64 pose_draw_frame;              // engine frame of pose_draw (0 = never drawn)
    u8 armor_left[BP_COUNT];          // cosmetic (each machine): hits until the part's armour breaks
    Xform item_prev, item_cur;
    Xform veh_prev, veh_cur;   // seated: the vehicle transform at the two evaluated poses (re-attached when drawn)
    bool veh_valid;
    Xform item_cam_prev, item_cam_cur;   // hold point item in camera space (owner re-places per frame)
    quat ads_aim_prev, ads_aim_cur;      // the exact ADS item rotation turned by the hold aim, same ticks (ADS checks)
    vec3 muzzle_cam;                     // aim model: the muzzle socket relative to the player's simulated eye / view (docs/AIM.md),
                                         // of the gameplay pose aimed at this tick's aim point (the drawn pose without a hold solve)
    vec3 barrel_cam;                     // the barrel's forward axis (item -Z) in the same camera space
    bool muzzle_cam_valid;
    u32 muzzle_cam_tick;                 // chars tick of that pose
    AimHoldRaw aim_raw;                  // the gameplay pose before the hold aim, same frame: every machine re-aims it at its P
    bool aim_raw_valid;                  // the hold solve ran for a gun (not the LOD2+ baked arms; weight 0 without the hold aim)
    u32 aim_raw_tick;
    vec3 drawn_barrel_cam;               // the drawn item's barrel (sway, kick), same frame (aim_report)
    vec3 eye_cam;                        // aim model: the head's camera socket, same frame (the camera ray's origin)
    bool eye_cam_valid;
    u32 eye_cam_tick;
    bool item_valid;
    const struct WeaponDef *item_weapon;
    const struct ItemDef *item_def;
    u8 item_att[5];                   // attachment ids of the held weapon (ATT_SLOTS, replicated)
    ItemSockets item_sock;            // held weapon sockets with the attachments applied (sight, grip_l, muzzle)
    float item_ads_time;              // the held weapon's effective ADS time (attachments applied)
    bool have_pose;
    int lod, fade_from;
    float lod_fade;
    float pixels;
    u32 last_eval;
    int interval;
    bool culled;
    // docs/LOD.md (lod_active): the cross-fade (completes even when the selector reverses), the update
    // interval eased in log2 (one octave per update_ramp_s, never halved in one step) with its eval
    // accumulator and the next gap (span, ticks), the LOD0 systems' weight (foot IK / lean / turn over a
    // pixel band) and the sun shadow's coverage (faded at the shadow size).
    LodFade lodf;
    float interval_log2, eval_acc, ik_w, shadow_w;
    int span;
    bool outside;                     // outside the last frame's view (grown by a margin): PF-0033's test
    bool offscreen;                   // ... and posed at the cheap rate without IK (PF-0033 on)
    // docs/perf/FINDINGS.md PF-0034: the base layer's phase / state and the root of the last two
    // post-passes (prev, cur), and the shared pose drawn this frame (NULL: the character's own) placed
    // at shared_root (last frame's: shared_root_prev; character scale included).
    float draw_phase[2];
    i16 draw_state[2];
    Xform draw_root[2];
    struct PoseEntry *shared;
    u32 shared_gen;
    u64 shared_frame;
    bool shared_cont;                 // also drawn shared the frame before (its root then is the velocity's)
    Xform shared_root, shared_root_prev;
    mat4 shared_m[2];                 // ... as matrices (the draws' root / prev_root)
    // Third-person fire animation (docs/FIRE_ANIM.md): layers 1, 2 and 4 at a reduced scale, driven
    // by the fire events with a deterministic seed, so everyone sees the kick and the slide.
    FireAnimState tp_fire;
    FireAnimOut tp_fire_out;
    u32 tp_fire_shots;
    // death / ragdoll
    Ragdoll rag;
    bool dead, rag_pending, settled;
    bool tool_rag;                    // ragdolled by a dev tool (not dead): no auto revive
    bool corpse;                      // a corpse entity's body (docs/REVIVE.md): never evaluated, holds an adopted ragdoll / pose
    // Controller states with "root_y": "clip" (the hang climb, clip_root_apply): the root height at the
    // state's start and the height the clip climbs from.
    bool croot_on;
    i16 croot_state;
    float croot_from_y, croot_anchor_y;
    bool adopted;                     // corpse: took over its player's ragdoll when the player came back
    bool phantom;                     // client-local display body (fabricator reconstruction): idle, no foot IK
    vec4 recon;                       // phantom: MAT_RECON spine base + progress (docs/REVIVE.md)
    u32 recon_flags;                  // MAT_RECON | MAT_RECON_FX
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
    // Horde zombies (src/gameplay/zombies/horde.c): the horde moves them; the host evaluates
    // their animation + hitboxes every sim_interval ticks (phase spreads the work), shifting the
    // hitboxes with the body in between.
    int horde;                        // horde slot + 1 (0 = not in a horde)
    int sim_interval, sim_phase;
    vec3 hit_feet;                    // feet when the hitboxes were last built
    vec3 cl_prev_feet;                // client: zombie locomotion derived from the replicated motion
    float cl_speed;
    float cl_attack_t;                // client: a zombie's attack runs (from its melee event), drives is_attacking
    u8 last_attack;                   // zombies: the previous attack variant + 1 (never twice in a row)
    float look_yaw, look_pitch;       // zombies: smoothed head / spine look at the nearest player (yaw relative to the body)
    bool is_player, local;
    int slot;
    u32 events_received[AEV_COUNT];
    // Footsteps (docs/AUDIO_PROCEDURAL.md "Footsteps"): plants counted where the controller ticks
    // (any thread: this character only), taken on the main thread by chars_take_steps.
    u8 steps_pending, step_foot;
    double last_step_time;
    // Modular parts (docs/CHARACTERS.md): one SkeletonInstance (pose + shared palette) whatever the
    // part count; each selected part is a SkeletalMesh skinned from it.
    u8 parts[CSLOT_COUNT];            // part selection (host: authoritative; clients: replicated)
    u16 part_gpu[CSLOT_COUNT];        // built GPU part + 1 per slot (render cache)
    u8 part_gpu_sel[CSLOT_COUNT];     // selection the cache was built for
    u32 part_gpu_gen;
    SkeletonInstance *si;
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
// Aim model (docs/AIM.md): the held weapon's muzzle socket in camera space (x right, y up, -z forward)
// relative to the player's simulated eye and view angles, from the latest evaluated tick pose, and its
// age in ticks. The shooter claims it in its UserCmd; the host checks the claim against its own copy.
bool chars_muzzle_cam(World *w, EntityId e, vec3 *cam, int *age_ticks);
// The barrel's forward axis (item -Z) of the same pose, same camera space (unit): the bullet's direction.
bool chars_barrel_cam(World *w, EntityId e, vec3 *dir, int *age_ticks);
// The same for the first-person camera (the head's "camera" socket): where the player's view really
// starts. The capsule eye (player_eye_pos) can be 10 cm (standing) to 30 cm (crouched) away from it.
bool chars_eye_cam(World *w, EntityId e, vec3 *cam, int *age_ticks);
// The gameplay hold pose before the hold aim (docs/AIM.md "Hold aim"), same camera space: the muzzle,
// barrel, pivot, weight and limit aim_hold_solve re-aims at any P (the command's claim, the host's
// check, the crosshair). False without a hold solve (no gun, LOD2+ baked arms, dead).
bool chars_aim_raw(World *w, EntityId e, AimHoldRaw *raw, int *age_ticks);
// Hold aim diagnostics of the latest pose (aim_report): the wall pull, the weight, the limit, the
// turn, and the drawn barrel (camera space as above).
typedef struct CharAimInfo { float pull, w, max_deg, angle_deg, miss; vec3 drawn_barrel; } CharAimInfo;
bool chars_aim_info(World *w, EntityId e, CharAimInfo *out);
// Eye (camera socket) of a character's current pose, ragdoll included (death camera).
bool chars_head_world(World *w, EntityId e, float alpha, vec3 *out);
// Corpses (docs/REVIVE.md): the body of an ET_CORPSE entity. While its player is dead the player's
// own ragdoll is the body; when the player respawns the corpse adopts that ragdoll / pose.
CharacterInst *chars_attach_corpse(World *w, EntityId corpse, int owner_slot);
// Reconstruction display body: the given player's character (definition + modular parts) on a
// client-local entity, evaluated in an idle pose and drawn with the dissolve (docs/REVIVE.md).
CharacterInst *chars_attach_phantom(World *w, EntityId local_ent, int player_slot);
// Pelvis + head of a character's current (ragdoll / settled / animated) pose.
bool chars_body_axis(World *w, EntityId e, vec3 *pelvis, vec3 *head);
// extract_blood hand IK: [0] where the free hand steadies the body (chest), [1] the needle point (arm / neck).
bool chars_extract_points(World *w, EntityId e, vec3 out[2]);
// Death sequence impact probe (docs/REVIVE.md): the ragdoll's head and chest bodies, their speeds
// and their clearance above the world below (m). False without an active ragdoll.
bool chars_ragdoll_probe(World *w, EntityId e, vec3 pos[2], float speed[2], float clearance[2]);
bool chars_palm_world(World *w, EntityId e, int hand, vec3 *out);   // latest pose, world space
// First person: this frame's final camera (the held item is placed relative to it).
void chars_set_fp_camera(vec3 pos, quat rot, bool valid);
// Aim offset of the held item (degrees right / up of the view, the view pitch in radians): the
// first-person item turns about the eye so its sight points where the shots go.
void chars_set_fp_aim_offset(float right_deg, float up_deg, float view_pitch, float world_fov_deg, float vm_fov_deg);
// Fire animation of the first-person weapon (docs/FIRE_ANIM.md): tick = this tick's output (the
// gun kick on the hold point, evaluated per tick), frame = this frame's interpolated output (hand
// lag, trigger, slide, attachment wobble at render). NULL keeps the previous one; both NULL clears.
Xform chars_fp_item_cam_raw(void);   // the rendered first-person item in camera space (this frame)
struct FireAnimOut;
void chars_set_fp_fire(const struct FireAnimOut *tick, const struct FireAnimOut *frame);
void chars_set_fp_fire_snap(bool snap);   // a shot landed this tick: no item interpolation until the next one
// Last first-person frame: hand <-> socket error ([0] left, [1] right; m / deg) and ADS sight error.
void chars_fp_diag(float glue_pos[2], float glue_ang[2], float *ads_pos, float *ads_ang);
const char *chars_holdpoint_text(World *w, int local_slot);   // "hold <state> <blend>% pull .. reach .."
CharacterInst *char_get_index(const World *w, int ent_index);

// Host: sim-only animation for every character (call after movement / weapons, before lag-comp).
void chars_host_tick(World *w, float dt);
// Host: lag-compensation history for a character (allocated on first use; horde zombies only
// keep one while near a player).
void chars_ensure_history(CharacterInst *c);
// Host stats of the last tick: characters evaluated (animation + hitboxes), ms spent.
void chars_host_stats(int *evaluated, float *ms);
// Gameplay notifications (host + local prediction).
void chars_notify_attack(World *w, int player_slot, bool melee, bool first_predict);
// Clients: replicated state from snapshots, then one tick of every replicated character.
void chars_client_receive(World *w, EntityId e, const AnimNetState *s, u32 tick);
void chars_client_tick(World *w, double render_tick, float dt);
// Local predicted player (client): controller follows the predicted PlayerSim.
void chars_local_predict(World *w, int slot, bool first);
// Footsteps since the last call within radius of near (main thread; farther ones are dropped).
// Horde zombies step by their speed (their animation is evaluated every few ticks only).
typedef struct CharStep { EntityId ent; vec3 pos; float speed; bool crouch, player, zombie, left; } CharStep;
int chars_take_steps(World *w, vec3 near, float radius, float dt, CharStep *out, int max);
// Zombies near a point for their voices: head position, speed, attacking, dead, health 0..1.
typedef struct CharVoiceInfo { EntityId ent; vec3 head; float speed, health; bool attacking, dead; } CharVoiceInfo;
int chars_zombies_near(World *w, vec3 p, float radius, CharVoiceInfo *out, int max);
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
// Character skins (assets/human/skins.json + assets/skins/): the local player picks one with cl_skin
// (sent to the host as its body part, replicated to everyone).
int chars_skin_count(void);
const char *chars_skin_id(int i);
const char *chars_skin_name(int i);
// Modular parts: host sets a slot's option (index into the character's slot options, -1 = empty);
// clients see it through the replicated part selection. Returns false if the option is invalid.
bool chars_set_part(World *w, EntityId e, int slot, int option);
// Host: a player's part request (RPC_SET_PART); players may only change their body to a skin
// and their own optional slots.
void chars_sv_player_part(World *w, int player_slot, int slot, int option);
// Inspector / tests: built GPU parts and how many draws the last frame submitted per part.
typedef struct CharPartInfo {
    int slot, option, mesh, bones, mesh_only, tris_lod0, gpu;
    const char *id;
} CharPartInfo;
int chars_part_info(World *w, EntityId e, CharPartInfo *out, int max);
int chars_gpu_part_count(void);
float chars_fp_hold_weight(void);
vec3 chars_fp_sight_world(void);  // the rendered item's sight (reticle anchor) in world / viewmodel space
vec3 chars_fp_sight_cam(void);    // rendered sight socket in camera space (ADS: (0, 0, -eye_relief))
float chars_fp_reach_pull(void);  // first-person reach pull this frame (m)
bool chars_fp_item_cam(Xform *item_off, int *state, float *blend);   // rendered item relative to its hold point (false: no hold)                 // 1 = the first-person hold IK owns both hands this frame   // this frame's first-person pose (tests)
bool chars_fp_camera(World *w, float alpha, int local_slot, float yaw, float pitch, Xform *socket_world);
// Events (reliable, compact): host queues + applies, clients apply what arrives.
// Death / revive notifications on every machine (flesh effects: kill burst, pools, bleeding).
typedef void (*CharLifeFn)(World *w, CharacterInst *c, bool died);
void chars_set_life_hook(CharLifeFn fn);
// Bone world transform as drawn this frame: 1 ok, 0 not drawn this frame (culled), -1 gone.
int chars_bone_draw(World *w, EntityId e, int bone, Xform *out);
void chars_push_event(World *w, const AnimEvent *e);
void chars_on_event(World *w, const AnimEvent *e);
const AnimNetState *chars_net_state(const World *w, int ent_index);
// Host damage hook: limb status, hit / death events. dir = shot direction (world).
void chars_on_damage(World *w, EntityId target, float dealt, int part, vec3 dir, vec3 point, bool killed, int damage_type);
// Hit tests against characters (host: rewound to view_tick256 when > 0; clients: current pose).
bool chars_raycast(World *w, EntityId ignore, vec3 origin, vec3 dir, float range, u32 view_tick256, EntityId *ent, HitboxRayHit *hit);
// One character (ballistics broadphase: gameplay/ballistics/bal_world.c). chord: the path length
// inside the hit capsule (flesh thickness for penetration), NULL to skip.
bool chars_raycast_one(World *w, CharacterInst *c, vec3 o, vec3 d, float range, u32 view_tick256, HitboxRayHit *out, float *chord);
bool chars_sweep(World *w, EntityId ignore, vec3 a, vec3 b, float radius, EntityId *ent, HitboxRayHit *hit);
// Client-side predicted (unconfirmed) reaction on a target the local player hit.
void chars_predict_hit(World *w, EntityId target, int part, vec3 dir);
// Zombies (host).
EntityId spawn_zombie(World *w, vec3 feet, float yaw, u8 character_id);
void zombies_attack(World *w);
// Host: an NPC swings (melee animation + replicated event); the caller applies the damage.
float chars_npc_melee(World *w, CharacterInst *c);   // starts a zombie attack; returns its hit delay (s)
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

// Vehicles (src/game/game_vehicles.c): seated pose for players (every machine: from the replicated
// seat occupancy), hands glued to the steering wheel / handlebar, weapons holstered while driving.
typedef struct VehiclePoseHooks {
    bool (*seat_pose)(World *w, int slot, vec3 *hip, vec3 feet[2], float *body_yaw);
    // Hands on the controls (world): use[h] false = that hand is free (one-handed shooting).
    bool (*seat_hands)(World *w, int slot, vec3 pos[2], quat rot[2], bool use[2]);
    bool (*holstered)(World *w, int slot);   // driving with both hands: nothing held
    bool (*vehicle_xform)(World *w, int slot, Xform *out);   // the seat's vehicle as drawn this frame
    BodyId (*vehicle_body)(World *w, int slot);              // its physics body (ignored by the hold point's wall check)
    const char *(*seat_vehicle)(World *w, int slot);         // the seat's vehicle definition name ("car"): its hand pose case
    bool (*seat_driver)(World *w, int slot);                 // the seat is the driver's (else a passenger seat)
} VehiclePoseHooks;
extern VehiclePoseHooks g_vehicle_pose;
extern bool g_chars_force_phys_step;   // client: step physics this tick (vehicle prediction)
extern bool g_chars_frozen;            // host: the world is frozen (hand pose editor): zombies / NPCs don't simulate

// TEMPORARY (branch inv-simple): the API of branch char-preview, stubbed until it is merged.
typedef enum CharWornSlot { CWORN_HEAD, CWORN_CHEST, CWORN_BACK, CWORN_COUNT } CharWornSlot;
void chars_set_worn(World *w, int player_slot, const u16 worn[CWORN_COUNT]);
typedef struct CharPreviewIn {
    int w, h;
    u16 held_item;
    u8 held_att[ATT_SLOTS];
    u16 worn[CWORN_COUNT];
    float yaw;
} CharPreviewIn;
TextureId chars_preview(World *w, int player_slot, const CharPreviewIn *in);

#endif
