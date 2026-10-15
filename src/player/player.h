// First-person player: movement state (Jolt CharacterVirtual), mantle/vault/ledge-hang,
// weapon state. Everything in PlayerSim is deterministic given (state, cmd) so it can be
// predicted on clients and reconciled against the host.
#ifndef DG_PLAYER_H
#define DG_PLAYER_H

#include "core/base.h"
#include "core/dgmath.h"
#include "game/usercmd.h"

struct World;
struct PhysicsWorld;
struct PhysCharacter;

#define PLAYER_RADIUS 0.3f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_CROUCH_HEIGHT 1.1f
#define PLAYER_EYE_STAND 1.65f
#define PLAYER_EYE_CROUCH 0.98f

typedef enum MoveMode { MOVE_WALK, MOVE_MANTLE, MOVE_VAULT, MOVE_HANG, MOVE_NOCLIP, MOVE_DEAD } MoveMode;

typedef struct PlayerMove {
    vec3 pos;           // feet
    vec3 vel;
    float yaw, pitch;
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
    u32 land_tick;
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
} PlayerMove;

typedef struct WeaponState {
    u8 slot;            // equipped hotbar slot
    u8 prev_slot;
    u8 reloading;
    u8 swing_hit_done;  // melee: already hit something this swing
    float cooldown;     // time until next shot/swing
    float reload_t;     // reload time remaining
    float swing_t;      // melee: time since swing start (<0 = idle)
    float use_t;        // consumable use progress (s), <0 idle
    float spread;       // current bloom (degrees)
    float equip_t;      // time since equip (raise animation)
    u32 shots;          // total shots fired (seeds spread RNG)
    float recoil_kick;  // visual: decays, drives viewmodel kick
    i16 mag;            // rounds in the equipped weapon (predicted on clients, mirrored by the host)
    i16 pad;
} WeaponState;

typedef struct PlayerSim {
    PlayerMove move;
    WeaponState wpn;
    u16 last_buttons;
    u16 pad;
} PlayerSim;

// Tunables (cvars, registered by player_register_cvars).
typedef struct PlayerTuning {
    struct Cvar *walk_speed, *sprint_speed, *crouch_speed, *walk_slow_speed;
    struct Cvar *accel, *air_accel, *friction, *air_control, *gravity, *jump_speed;
    struct Cvar *coyote_time, *jump_buffer_time, *step_height, *max_slope;
    struct Cvar *mantle_min, *mantle_max, *hang_max, *air_mantle_max, *vault_max, *mantle_reach, *mantle_speed, *vault_speed;
    struct Cvar *debug_mantle, *debug_melee, *noclip_speed;
    struct Cvar *crouch_toggle, *sprint_toggle;
} PlayerTuning;

void player_register_cvars(void);
const PlayerTuning *player_tuning(void);

// Movement step for one tick. Writes the character's physics state.
// first_predict = true the first time a cmd is simulated (effects/debug allowed).
void player_move_tick(struct World *w, int slot, PlayerSim *sim, struct PhysCharacter *ch, const UserCmd *cmd, float dt, bool first_predict);
void player_move_reset(PlayerSim *sim, vec3 feet, float yaw);
float player_eye_height(const PlayerMove *m);
vec3 player_eye_pos(const PlayerMove *m);

#endif
