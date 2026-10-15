// First-person hands + arms rig with two-bone IK.
//
// Placeholder visuals: each hand is a cube, each arm two boxes (upper arm, forearm).
// The system is the foundation for real hand models later:
//   1. Base layer   : keyframed hand-pose clips (data/hand_poses.json), camera-space targets + grip type.
//   2. Procedural   : bob, look sway, velocity lag, landing dip, breathing, recoil.
//   3. World IK     : per-hand world-space targets with blend weights (door knobs, levers, switches,
//                     container lids, pickups, ledge anchors during hang/mantle/vault, weapon support grip).
//   4. Solve        : analytic two-bone IK shoulder -> elbow -> wrist with an elbow pole vector.
// Swap the box rendering in hands_render() for skinned meshes driven by the solved chain.
#ifndef DG_HANDS_H
#define DG_HANDS_H

#include "core/base.h"
#include "core/dgmath.h"
#include "world/world.h"

typedef enum GripType {
    GRIP_RELAXED, GRIP_FIST, GRIP_POINT, GRIP_GRAB, GRIP_PISTOL, GRIP_RIFLE, GRIP_MELEE, GRIP_PALM, GRIP_COUNT
} GripType;

#define HAND_LEFT 0
#define HAND_RIGHT 1

typedef struct HandsInput {
    World *world;
    int player;
    vec3 eye;                 // camera position (after bob)
    float yaw, pitch, roll;   // camera angles
    vec2 look_delta;          // view rotation this frame (radians)
    float dt;
    EntityId focus;           // interactable under the crosshair
    bool use_pressed;         // use edge this frame (local prediction of the interaction)
    bool use_held;
    bool ads;
    vec3 vm_kick_pos;         // viewmodel recoil spring: camera space offset (m, +z = back)
    vec3 vm_kick_rot;         // pitch (muzzle up), yaw (right), roll in degrees
} HandsInput;

typedef struct ArmChain {
    vec3 shoulder, elbow, wrist;
    vec3 hand_pos;
    quat hand_rot;
    u8 grip;
    bool reach_limited;       // target was out of reach
} ArmChain;

bool hands_init(void);
void hands_shutdown(void);
bool hand_poses_load(const char *path);
void hands_update(const HandsInput *in);
void hands_render(void);                     // local player's arms/hands/weapon (viewmodel pass)
// Third-person rig for remote players (stateless; base pose + ledge anchors only).
void hands_render_remote(World *w, int slot, vec3 eye, float yaw, float pitch);
const ArmChain *hands_chain(int hand);
// Third-person held item at a grip transform (item origin = right grip), e.g. a character's hand.
void hands_draw_held(const struct WeaponDef *wd, const struct ItemDef *it, vec3 pos, quat rot, u32 material_flags);
// World-space barrel tip of the local viewmodel weapon (last hands_update). False if no gun.
bool hands_muzzle(vec3 *pos, vec3 *fwd);
void hands_reload_poses(void);

#endif
