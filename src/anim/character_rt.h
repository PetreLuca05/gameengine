// Per-character animation runtime (pure, shared by the game and the tests):
//   inputs (gameplay / replicated state) -> controller tick -> base pose (sim-only: hitboxes)
//   -> procedural post-pass (render side: damage additive, spine, hold + glue, foot IK).
#ifndef DG_CHARACTER_RT_H
#define DG_CHARACTER_RT_H

#include "anim_library.h"

typedef struct CharInputs {
    float speed;            // horizontal m/s
    vec2 move_local;        // unit direction in body space (x right, y forward)
    bool grounded, crouch, prone, sprint, ads, mantling, vaulting, hanging, dead, attacking;
    bool downed;            // knocked down (docs/REVIVE.md): controller state 'downed' + the procedural kneel
    bool swimming;          // at the surface or under water (EF_SWIM / EF_UNDERWATER): controller state 'swim'
    float mantle_t;
    float vertical_vel;
    float aim_pitch, aim_yaw, body_yaw;   // radians (world yaw)
    int weapon_type;        // HoldStyleId
    int weapon_id;
    float health;
    u8 limb_status;
    // Phone (docs/PHONE.md): CHAR_PHONE_* flags and, while driven by a real phone, its rotation
    // relative to the view yaw (phone_quat_pack). Replicated (ANS_PHONE).
    u8 phone_flags;
    u32 phone_rot;
} CharInputs;

enum CharPhoneFlags { CHAR_PHONE_CAMERA = 1 << 0, CHAR_PHONE_LANDSCAPE = 1 << 1, CHAR_PHONE_DRIVEN = 1 << 2, CHAR_PHONE_REC = 1 << 3, CHAR_PHONE_FRONT = 1 << 4 };

typedef struct CharRuntime {
    const AnimLibrary *lib;
    const CharacterDef *def;
    ControllerInst ctrl;
    ProcState proc;
    CharInputs in;
    Pose base, final;
    Xform base_model[ANIM_MAX_BONES];
    Xform final_model[ANIM_MAX_BONES];
    bool base_valid;
    u32 evals;
    // Smoothed move direction (spring) -> blend-tree direction (the diagonal clips cover the
    // diagonals); smoothed speed.
    float dir_ang, dir_ang_v, dir_target;
    bool dir_init;
    vec2 blend_dir;
    float blend_speed, blend_speed_v;
    // Tools (test stand): replaces the controller output with one clip at a fixed time.
    i16 override_clip;                // -1 = off
    float override_time;
    bool override_mirror;
} CharRuntime;

void charrt_init(CharRuntime *r, const AnimLibrary *lib, const CharacterDef *def);
// Writes the controller parameters from gameplay inputs (deterministic).
void charrt_set_inputs(CharRuntime *r, const CharInputs *in);
void charrt_trigger(CharRuntime *r, int trigger_param);
void charrt_tick(CharRuntime *r, float dt);
void charrt_eval_base(CharRuntime *r);
// Bone world transforms from a model-space pose (root = feet + body yaw, uniform scale).
void charrt_to_world(const CharRuntime *r, const Xform *model, Xform root, float scale, Xform *world);
// Copies base -> final and runs the damage additive + procedural post-pass with `in`.
void charrt_post(CharRuntime *r, ProcInput *in);
// Fills a ProcInput with the runtime's library data and inputs (caller adds world bits).
void charrt_proc_input(const CharRuntime *r, ProcInput *in, Xform root, float dt);

#endif
