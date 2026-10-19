// Data-driven animation controller (assets/human/controller.json).
//
// Layers (fixed order, per-bone masks, override / additive) each run a state machine whose
// states play a clip or a blend tree (1D, 2D cartesian, 2D freeform-directional, nested).
// Transitions test parameters (float / int / bool / trigger), with blend durations, exit
// times, interrupt rules and "any state" sources; optional inertialization.
//
// Determinism: ctrl_tick() is a pure function of (definition, binding, parameter history,
// tick count). Time advances in whole ticks; no wall clock, no frame dt, no RNG except the
// seed delivered with triggers. ctrl_evaluate() samples the pose for the current tick and
// may be called less often (render LOD) without changing the state machine.
#ifndef DG_CONTROLLER_H
#define DG_CONTROLLER_H

#include "anim.h"

#define AC_MAX_PARAMS 48
#define AC_MAX_LAYERS 6
#define AC_MAX_STATES 48
#define AC_MAX_TRANS 96
#define AC_MAX_NODES 160
#define AC_MAX_CHILDREN 12
#define AC_MAX_COND 4
#define AC_MAX_SLOTS 128
#define AC_MAX_MASKS 12
#define AC_MAX_VARIANTS 6
#define AC_HISTORY 16

// Standard parameters (gameplay writes these by id; the JSON may add more by name).
typedef enum AnimParam {
    AP_SPEED, AP_MOVE_X, AP_MOVE_Y, AP_GROUNDED, AP_CROUCH, AP_PRONE, AP_SPRINT, AP_ADS, AP_MANTLE, AP_MANTLE_T,
    AP_HANG, AP_VAULT, AP_VERTICAL_VEL, AP_TURN_ANGLE, AP_AIM_PITCH, AP_AIM_YAW, AP_WEAPON_TYPE, AP_WEAPON_ID,
    AP_HEALTH, AP_LIMB_STATUS, AP_DEAD, AP_ATTACKING,
    // triggers
    AP_T_JUMP, AP_T_LAND, AP_T_HIT, AP_T_FIRE, AP_T_RELOAD, AP_T_MELEE, AP_T_DIE, AP_T_EQUIP,
    // trigger payloads
    AP_HIT_PART, AP_HIT_DIR, AP_HIT_STRENGTH, AP_SEED,
    AP_STD_COUNT
} AnimParam;

typedef enum ParamType { PT_FLOAT, PT_INT, PT_BOOL, PT_TRIGGER } ParamType;
typedef enum NodeKind { NK_CLIP, NK_BLEND1D, NK_BLEND2D_CART, NK_BLEND2D_DIR } NodeKind;
typedef enum CondOp { CO_GT, CO_LT, CO_GE, CO_LE, CO_EQ, CO_NE, CO_TRUE, CO_FALSE, CO_TRIGGER } CondOp;
typedef enum LayerBlend { LB_OVERRIDE, LB_ADDITIVE } LayerBlend;

typedef struct ParamDef { char name[24]; u8 type; float def; } ParamDef;

typedef struct BlendNode {
    u8 kind;
    i16 slot;                      // NK_CLIP: clip slot
    char slot_pattern[32];         // NK_CLIP: "reload_{weapon}" resolved per weapon type
    float speed;
    float hand_ik;                 // NK_CLIP: 1 = the procedural hold owns the hands, 0 = the clip does ("hand_ik": false)
    i16 param_x, param_y;
    int child_count;
    i16 child[AC_MAX_CHILDREN];
    float threshold[AC_MAX_CHILDREN];  // 1D
    vec2 pos[AC_MAX_CHILDREN];         // 2D
} BlendNode;

typedef struct StateDef {
    char name[24];
    i16 node;                      // root blend node, -1 = empty (layer weight 0)
    i16 variants[AC_MAX_VARIANTS]; // alternative root nodes picked by seed
    int variant_count;
    i16 time_param;                // drives normalized time directly (mantle_t), -1 = clock
    i16 speed_param;               // playback multiplier param, -1
    float speed;
    bool loop;                     // false: clamps at the end (state "finished")
    bool hand_ik;                  // procedural weapon hold active in this state
    bool sync;                     // keep phase when transitioning between sync states
    bool speed_match;              // playback rate follows `speed` / the clips' root speed (no foot sliding)
    bool clip_root_y;              // "root_y": "clip": the clip carries the vertical travel (a climb); the
                                   // rendered root keeps its height from the state's start (characters.c)
} StateDef;

typedef struct Condition { i16 param; u8 op; float value; } Condition;

typedef struct TransitionDef {
    i16 from;                      // -1 = any state
    i16 to;
    Condition cond[AC_MAX_COND];
    int cond_count;
    float duration;                // seconds
    float exit_time;               // normalized time required (<0 none)
    bool inertialize;
    bool interruptible;            // may be interrupted by another transition while blending
    bool allow_self;               // any-state transition may re-enter the current state
} TransitionDef;

typedef struct LayerDef {
    char name[24];
    i16 mask;                      // -1 = all bones
    u8 blend;
    float weight;
    i16 weight_param;              // optional multiplier param
    StateDef states[AC_MAX_STATES];
    int state_count;
    int default_state;
    TransitionDef trans[AC_MAX_TRANS];
    int trans_count;
} LayerDef;

typedef struct ControllerDef {
    u32 hash;
    char source[128];
    ParamDef params[AC_MAX_PARAMS];
    int param_count;
    LayerDef layers[AC_MAX_LAYERS];
    int layer_count;
    BlendNode nodes[AC_MAX_NODES];
    int node_count;
    char slot_names[AC_MAX_SLOTS][32];
    int slot_count;
    char mask_names[AC_MAX_MASKS][24];
    BoneMask masks[AC_MAX_MASKS];
    int mask_count;
    char weapon_types[16][16];     // values of the weapon_type parameter ("{weapon}" substitution)
    int weapon_type_count;
} ControllerDef;

// Anim set binding: controller slot -> clip (+ playback modifiers). Built per (controller, anim set).
typedef struct SlotBinding { i16 clip; bool mirror; bool reverse; float r0, r1; } SlotBinding;   // r0..r1: normalized sub-range
typedef struct AnimBinding {
    u32 hash;                      // anim set hash
    char name[32];
    SlotBinding slots[AC_MAX_SLOTS];
    // pattern slots resolved per weapon type: [node][weapon_type] -> slot
    i16 pattern_slot[AC_MAX_NODES][16];
    i16 fallback_clip;             // idle
} AnimBinding;

typedef struct LayerState {
    i16 state, prev_state;
    i16 variant, prev_variant;     // chosen root node
    u32 state_ticks;
    float phase, prev_phase;       // normalized 0..1 (loops wrap)
    u16 blend_ticks, blend_total;  // crossfade progress (0 total = none)
    bool interruptible;
    float prev_weight_scale;
} LayerState;

typedef struct TransitionLog { u32 tick; u8 layer; i16 from, to; } TransitionLog;

typedef struct ControllerInst {
    const ControllerDef *def;
    const AnimBinding *bind;
    float params[AC_MAX_PARAMS];
    u64 triggers;                  // trigger bits by param index (consumed per tick)
    LayerState layer[AC_MAX_LAYERS];
    u32 tick;
    // debug / tools
    TransitionLog history[AC_HISTORY];
    int history_head;
    bool mute[AC_MAX_LAYERS], solo[AC_MAX_LAYERS];
    // inertialization (applied in evaluate)
    bool inert_pending;
    u32 inert_tick;
    float inert_duration;
    vec3 inert_rot_x0[ANIM_MAX_BONES], inert_rot_v0[ANIM_MAX_BONES];
    vec3 inert_pos_x0, inert_pos_v0;
    Pose last_out, prev_out;
    bool have_last;
    // events emitted by the last tick (cosmetic)
    u8 events[8];
    int event_count;
} ControllerInst;

// Loading ----------------------------------------------------------------
bool ctrl_def_load(ControllerDef *def, const char *json_text, const Skeleton *skel, char *err, int err_size);
int ctrl_param_index(const ControllerDef *def, const char *name);
int ctrl_slot_index(const ControllerDef *def, const char *name);
int ctrl_state_index(const ControllerDef *def, int layer, const char *name);
const char *ctrl_param_name(int std_param);
// Binds an anim set (JSON {"slots": {"walk_f": "walking", "walk_b": "walking@reverse"}}) to clips.
typedef struct ClipLookup { const Clip *clips; int count; } ClipLookup;
bool ctrl_bind(AnimBinding *b, const ControllerDef *def, const char *set_json, const ClipLookup *clips, char *err, int err_size);

// Runtime -----------------------------------------------------------------
void ctrl_init(ControllerInst *c, const ControllerDef *def, const AnimBinding *bind);
DG_INLINE void ctrl_set(ControllerInst *c, int p, float v) { if (p >= 0 && p < AC_MAX_PARAMS) c->params[p] = v; }
DG_INLINE float ctrl_get(const ControllerInst *c, int p) { return (p >= 0 && p < AC_MAX_PARAMS) ? c->params[p] : 0.0f; }
DG_INLINE void ctrl_trigger(ControllerInst *c, int p) { if (p >= 0 && p < 64) c->triggers |= (1ull << p); }
// Advances the state machines one tick (deterministic).
void ctrl_tick(ControllerInst *c, const ClipLookup *clips, float dt);
// Samples the pose for the current state (no procedural passes). model may be NULL.
void ctrl_evaluate(ControllerInst *c, const Skeleton *skel, const ClipLookup *clips, Pose *out);
// Forces the replicated state (snap on divergence / late join).
void ctrl_force_state(ControllerInst *c, int layer, int state, u32 state_ticks, float phase);
// Normalized time of the layer's current state, and whether a clamped state finished.
float ctrl_state_time(const ControllerInst *c, int layer);
bool ctrl_hand_ik_weight(const ControllerInst *c, float *w);   // procedural hold weight from upper layer
// Root motion of the base layer for this tick (model space XZ + yaw), only clips flagged root_motion.
void ctrl_root_delta(const ControllerInst *c, const ClipLookup *clips, float dt, vec3 *dpos, float *dyaw);
// Height of the root bone (the hips) in the first frame a state plays (its heaviest clip at the
// start of its range); false for an empty state.
bool ctrl_state_start_hips_y(const ControllerInst *c, const ClipLookup *clips, int layer, int state, float *y);
const char *ctrl_state_name(const ControllerInst *c, int layer, int state);

#endif
