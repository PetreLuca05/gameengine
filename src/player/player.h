// First-person player: movement state (Jolt CharacterVirtual), mantle/vault/ledge-hang,
// weapon state. Everything in PlayerSim is deterministic given (state, cmd) so it can be
// predicted on clients and reconciled against the host.
#ifndef DG_PLAYER_H
#define DG_PLAYER_H

#include "core/base.h"
#include "core/dgmath.h"
#include "game/usercmd.h"
#include "gameplay/recoil.h"
#include "gameplay/revive/revive_core.h"

struct World;
struct PhysicsWorld;
struct PhysCharacter;

#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_CROUCH_HEIGHT 1.1f
#define PLAYER_EYE_STAND 1.65f
#define PLAYER_EYE_CROUCH 0.98f

typedef enum MoveMode { MOVE_WALK, MOVE_MANTLE, MOVE_VAULT, MOVE_HANG, MOVE_NOCLIP, MOVE_DEAD, MOVE_SEATED, MOVE_SWIM } MoveMode;

// Water state of a player (docs/WATER.md "Swimming"): set every tick from the water at the feet.
typedef enum WaterState {
    WATER_DRY,          // no water at the feet
    WATER_WADE,         // walking in water (slowed by depth)
    WATER_SURFACE,      // MOVE_SWIM at the surface (head above water)
    WATER_UNDER,        // MOVE_SWIM diving (head under water: oxygen runs down)
} WaterState;

typedef struct PlayerMove {
    vec3 pos;           // feet
    vec3 vel;
    float yaw, pitch;
    float body_yaw;     // lower-body facing (turn in place); host-owned, replicated quantized
    u8 body_turning;
    u8 pad_body[3];
    u8 mode;
    u8 on_ground;
    u8 crouched;        // collision shape is crouched
    u8 sprinting;
    u8 crouch_toggled;  // toggle-crouch latch
    u8 sprint_toggled;
    u8 ground_steep;
    u8 pad0;
    float coyote;       // time left to still jump after leaving ground
    float jump_buffer;  // time left a jump press stays queued
    float crouch_amount;// 0 = stand, 1 = crouched (smoothed, drives eye height)
    float air_time;
    float land_speed;   // vertical speed at last landing (camera dip)
    u32 land_tick;      // command seq of the last landing (a change = a landing)
    float fall_start_y;
    // mantle / vault (scripted traversal)
    vec3 m_start, m_mid, m_end;
    float m_t, m_dur;
    u8 m_end_crouched;
    u8 m_kind;          // MOVE_MANTLE / MOVE_VAULT
    u8 pad1[2];
    // ledge hang
    vec3 hang_pos;      // feet position while hanging
    vec3 hang_normal;   // wall normal (towards player)
    vec3 ledge_point;   // top edge point in front of the player
    float hang_time;
    float regrab_cooldown;
    // hand IK anchors in world space (valid during mantle / vault / hang)
    vec3 hand_anchor[2];
    u8 hand_anchor_valid;
    u8 pad2[3];
    // vehicles (MOVE_SEATED, src/game/game_vehicles.c): host-owned, predicted positions only
    u32 vehicle;        // EntityId of the vehicle
    u8 seat;            // seat index
    u8 veh_onehand;     // driver holding the gun out one-handed (hold point HP_VEHICLE_ONEHAND, steering clamp)
    u8 veh_raise;       // ticks the gun stays out after a shot (fire raises it by itself)
    u8 pad3;
    float veh_stun;     // s left after being thrown out at speed (no control)
    float stride;       // walk-cycle phase (rad) for the item / camera bob (deterministic, spectators)
    // water (docs/WATER.md "Swimming"): predicted like the rest; oxygen / drowning are host-authoritative
    // through reconciliation (the damage itself is dealt by the host only)
    u8 water;           // WaterState
    u8 head_under;      // the eye is under the surface
    u8 swim_sprint;     // fast swim (drains stamina)
    u8 pad4;
    float water_depth;  // surface - feet (m), 0 when dry
    float water_surface;// surface height at the player (valid when water != WATER_DRY)
    float water_column; // surface - bottom at the player (m)
    float oxygen;       // s of breath left (pl_swim_breath at most)
    float stamina;      // 0..1 for the fast swim
    float wet;          // 0..1 character wetness (shading, drips): soaks in water, dries over time
    float swim_phase;   // stroke cycle (rad): procedural swim animation, deterministic
} PlayerMove;

typedef struct WeaponState {
    u8 slot;            // equipped hotbar slot
    u8 prev_slot;
    u8 reloading;
    u8 swing_hit_done;  // melee: already hit something this swing
    float cooldown;     // time until next shot/swing; guns carry the sub-tick remainder (exact RPM)
    float reload_t;     // reload time remaining
    float swing_t;      // melee: time since swing start (<0 = idle)
    float use_t;        // consumable use progress (s), <0 idle
    float equip_t;      // time since equip (raise animation)
    u32 shots;          // total shots fired (seeds spread RNG)
    float recoil_kick;  // 1 on a shot, decays (third-person firing flag)
    i16 mag;            // rounds in the equipped weapon (predicted on clients, mirrored by the host)
    u8 fire_mode;       // FireMode
    u8 burst_left;      // shots left in the current burst
    float ads_t;        // 0 hip .. 1 fully aimed (deterministic, drives recoil/spread/sway)
    float since_sprint; // s since the player stopped sprinting
    float breath;       // breath hold used (s)
    u8 breath_holding;
    u8 breath_exhausted;
    u8 ads_on;          // aim requested this tick (the hold point starts moving at once)
    u16 item;           // item in the equipped slot (a change counts as an equip)
    float sway_k;       // smoothed sway amplitude multiplier (hold breath)
    RecoilState rc;     // aim recoil + bloom (gameplay/recoil.h)
} WeaponState;

typedef struct PlayerSim {
    PlayerMove move;
    WeaponState wpn;
    DownState down;     // knocked down / bleed-out / revive progress (docs/REVIVE.md); host-written parts noted there
    ActionState act;    // predicted timed action (revive / extract DNA / insert into a fabricator)
    u16 last_buttons;
    u16 frozen_ticks;   // forced idle after a fabricator respawn (no input)
} PlayerSim;

// Tunables (cvars, registered by player_register_cvars).
typedef struct PlayerTuning {
    struct Cvar *walk_speed, *sprint_speed, *crouch_speed, *walk_slow_speed;
    struct Cvar *accel, *air_accel, *friction, *air_control, *gravity, *jump_speed;
    struct Cvar *coyote_time, *jump_buffer_time, *step_height, *max_slope;
    struct Cvar *mantle_min, *mantle_max, *hang_max, *air_mantle_max, *vault_max, *mantle_reach, *mantle_speed, *vault_speed;
    struct Cvar *hang_yaw_limit;
    struct Cvar *debug_mantle, *debug_melee, *noclip_speed;
    struct Cvar *crouch_toggle, *sprint_toggle;
    // water (pl_swim_*)
    struct Cvar *swim_speed, *swim_sprint_speed, *swim_dive_speed, *swim_accel, *swim_enter, *swim_exit, *swim_eye;
    struct Cvar *swim_spring, *swim_breath, *swim_stamina, *swim_rise, *wade_slow, *wade_crouch_max, *wade_sprint_max;
} PlayerTuning;

void player_register_cvars(void);
const PlayerTuning *player_tuning(void);

// Movement step for one tick. Writes the character's physics state.
// first_predict = true the first time a cmd is simulated (effects/debug allowed).
void player_move_tick(struct World *w, int slot, PlayerSim *sim, struct PhysCharacter *ch, const UserCmd *cmd, float dt, bool first_predict);
void player_move_reset(PlayerSim *sim, vec3 feet, float yaw);
float player_eye_height(const PlayerMove *m);
vec3 player_eye_pos(const PlayerMove *m);
// Ledge hang: the yaw facing the wall (the body and both ledge grips stay on it), and a view yaw
// kept within pl_hang_yaw_limit of it (unwrapped: only the excess is removed).
float player_hang_wall_yaw(const PlayerMove *m);
float player_hang_clamp_yaw(const PlayerMove *m, float yaw);
// The ledge grips (left, right) of a hang from the hanging feet and the wall yaw alone (the hang
// puts the feet a fixed distance under and out from the edge): remote players' grips from their
// replicated position and body yaw, identical to the simulated ones.
void player_hang_grips(vec3 feet, float wall_yaw, vec3 grip[2]);
// Spine-bend eye offset (set by the animation system; zero without it).
void movement_set_eye_offset(vec3 (*fn)(float yaw, float pitch));
// Seated players follow their seat (game_vehicles.c): feet position (so the standing eye height
// lands on the seat's eye) and the vehicle velocity. False = the vehicle is gone.
typedef bool (*SeatPoseFn)(struct World *w, u32 vehicle, int seat, vec3 *feet, vec3 *vel);
void movement_set_seat_pose(SeatPoseFn fn);
// Water at (x, z) at server tick `tick` (the command's view tick: identical on the host and the
// predicting client): surface height, water column depth (surface - bottom), water velocity (flow +
// the waves' orbital motion, y = the surface rising / falling). False = no water. Set by the game
// (game_water.c); without it nobody swims.
typedef bool (*MoveWaterFn)(struct World *w, float x, float z, double tick, float *surface, float *column, vec3 *flow);
void movement_set_water(MoveWaterFn fn);
// Fall damage multiplier for a landing in water of `depth` m (1 on land, 0 from ~2.5 m down): the
// hook for a future player fall damage rule (there is none yet).
float player_fall_water_scale(float depth);
float player_breath_max(void);
vec3 player_eye_offset(float yaw, float pitch);

#endif
