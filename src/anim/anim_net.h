// Animation replication: quantized per-character state (delta-compressed inside snapshots),
// compact reliable animation events, and quantization helpers.
//
// What crosses the wire (see docs/ANIMATION_NET.md):
//   snapshot field (per character, only changed sub-fields, 2-byte sub-mask):
//     state ids (base, upper), base phase u8, state start tick u16, speed u8, move dir 2 x i8,
//     aim pitch i16, aim yaw u16, body yaw u16, flags u8, limb status u8, weapon u8,
//     mantle t u8, character id u8, death tick u32 + seed u8 (only when dead)
//   events (reliable, ordered): jump, land, fire, reload, melee, hit, die - each with tick + seed
// Everything else (pose, IK, foot IK, damage reaction springs, ragdolls) is derived locally.
#ifndef DG_ANIM_NET_H
#define DG_ANIM_NET_H

#include "core/base.h"
#include "core/dgmath.h"

struct Writer;
struct Reader;

enum AnimNetFlags {
    ANF_GROUNDED = 1 << 0, ANF_CROUCH = 1 << 1, ANF_PRONE = 1 << 2, ANF_SPRINT = 1 << 3,
    ANF_ADS = 1 << 4, ANF_MANTLE = 1 << 5, ANF_HANG = 1 << 6, ANF_DEAD = 1 << 7,
};

enum AnimNetFields {
    ANS_STATE = 1 << 0, ANS_PHASE = 1 << 1, ANS_START = 1 << 2, ANS_SPEED = 1 << 3, ANS_MOVE = 1 << 4,
    ANS_PITCH = 1 << 5, ANS_YAW = 1 << 6, ANS_BODY = 1 << 7, ANS_FLAGS = 1 << 8, ANS_LIMB = 1 << 9,
    ANS_WEAPON = 1 << 10, ANS_MANTLE = 1 << 11, ANS_CHAR = 1 << 12, ANS_DEATH = 1 << 13,
    ANS_ALL = 0x3FFF,
};

typedef struct AnimNetState {
    u8 state[2];
    u8 phase;
    u8 speed;
    u16 state_start;
    i8 move_x, move_y;
    i16 aim_pitch;
    u16 aim_yaw;
    u16 body_yaw;
    u8 flags;
    u8 limb_status;
    u8 weapon;
    u8 mantle_t;
    u8 character_id;
    u8 death_seed;
    u32 death_tick;
} AnimNetState;

// Quantization (documented error bounds, tested by tests/test_anim.c):
//   speed   0..12.75 m/s, step 0.05            -> |err| <= 0.025
//   move    -1..1, step 1/127                  -> |err| <= 0.004
//   angles  u16 over 2pi (0.0055 deg)          -> |err| <= 0.0028 deg
//   pitch   i16, rad * 10000                   -> |err| <= 0.00005 rad
//   phase   u8, 1/256                          -> |err| <= 0.002
DG_INLINE u8 anq_speed(float v) { int q = (int)lrintf(v * 20.0f); return (u8)DG_CLAMP(q, 0, 255); }
DG_INLINE float and_speed(u8 q) { return (float)q / 20.0f; }
DG_INLINE i8 anq_unit(float v) { int q = (int)lrintf(v * 127.0f); return (i8)DG_CLAMP(q, -127, 127); }
DG_INLINE float and_unit(i8 q) { return (float)q / 127.0f; }
DG_INLINE u16 anq_angle(float a) {
    float t = a / (2.0f * DG_PI);
    t -= floorf(t);
    return (u16)((u32)lrintf(t * 65536.0f) & 0xFFFFu);
}
DG_INLINE float and_angle(u16 q) { return wrap_angle((float)q * (2.0f * DG_PI / 65536.0f)); }
DG_INLINE i16 anq_pitch(float a) { int q = (int)lrintf(a * 10000.0f); return (i16)DG_CLAMP(q, -32767, 32767); }
DG_INLINE float and_pitch(i16 q) { return (float)q / 10000.0f; }
DG_INLINE u8 anq_phase(float p) { p -= floorf(p); return (u8)((u32)lrintf(p * 256.0f) & 0xFF); }
DG_INLINE float and_phase(u8 q) { return (float)q / 256.0f; }
// Octahedral unit vector in 16 bits (8 + 8).
u16 anq_dir(vec3 d);
vec3 and_dir(u16 q);

u16 anim_net_diff(const AnimNetState *base, const AnimNetState *cur);   // base NULL = all fields
void anim_net_write(struct Writer *w, const AnimNetState *s, u16 fields);
void anim_net_read(struct Reader *r, AnimNetState *s, u16 *fields);
int anim_net_field_bytes(u16 fields);    // payload size for a mask (excluding the mask itself)

// Reliable animation events --------------------------------------------------
typedef enum AnimEventType {
    AEV_NONE, AEV_JUMP, AEV_LAND, AEV_FIRE, AEV_RELOAD, AEV_MELEE, AEV_HIT, AEV_DIE, AEV_EQUIP, AEV_COUNT
} AnimEventType;

typedef struct AnimEvent {
    u8 type;
    u32 ent;            // entity id
    u32 tick;           // host tick
    u8 seed;
    // hit / die payload
    u8 body_part;
    u16 dir;            // octahedral
    u8 strength;        // 0..255 -> 0..4
    u8 damage_type;     // 4 bits used
    u8 extra;           // land: vertical speed (m/s * 8), fire: weapon
} AnimEvent;

// Wire size per event type (bytes), excluding the message header.
int anim_event_size(const AnimEvent *e);
void anim_event_write(struct Writer *w, const AnimEvent *e);
bool anim_event_read(struct Reader *r, AnimEvent *e);
DG_INLINE u8 anq_strength(float s) { int q = (int)lrintf(s * 63.75f); return (u8)DG_CLAMP(q, 0, 255); }
DG_INLINE float and_strength(u8 q) { return (float)q / 63.75f; }

#endif
