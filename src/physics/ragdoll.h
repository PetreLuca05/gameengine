// Cosmetic ragdolls on the shared skeleton (Jolt). Never replicated: every peer builds its own
// from its local animated pose when the host's death event arrives.
//
// The definition (assets/human/ragdoll.json) is built once per skeleton: capsule bodies per bone
// group auto-fitted from the canonical mesh, human mass distribution (~80 kg), swing-twist joints
// (hinges for knees / elbows) with limits expressed relative to the bind pose. Hitboxes
// (gameplay/hitboxes.c) are derived from the same bodies.
#ifndef DG_RAGDOLL_H
#define DG_RAGDOLL_H

#include "anim/anim.h"
#include "anim/body_parts.h"
#include "physics.h"

struct cJSON;

#define RAG_MAX_BODIES 24

typedef enum RagShape { RAG_CAPSULE, RAG_BOX, RAG_SPHERE } RagShape;
typedef enum RagJointType { RJ_SWING_TWIST, RJ_HINGE } RagJointType;

typedef struct RagBodyDef {
    char name[16];
    i16 bone, end_bone;      // segment start / end bones
    i16 parent;              // parent body (-1 root)
    u8 shape;
    u8 part;                 // BodyPart (hitbox tag)
    float radius, half_len;  // capsule: radius + half height of the cylinder part
    vec3 half;               // box half extents
    float mass;              // kg (after normalization)
    float friction, restitution;
    Xform bind;              // body frame in model space (bind pose), capsule along local +Y
    Xform local;             // body frame relative to its bone (constant)
} RagBodyDef;

typedef struct RagJointDef {
    i16 parent, child;       // body indices
    u8 type;
    float twist_min, twist_max;  // radians
    float cone_normal, cone_plane;
    float hinge_min, hinge_max;
    vec3 pos_p, pos_c;       // joint point in each body's local frame
    vec3 axis_p, axis_c;     // twist / hinge axis
    vec3 plane_p, plane_c;   // plane / normal axis
} RagJointDef;

typedef struct RagdollDef {
    u32 hash;
    RagBodyDef bodies[RAG_MAX_BODIES];
    int body_count;
    RagJointDef joints[RAG_MAX_BODIES];
    int joint_count;
    float total_mass;
    float linear_damping, angular_damping;
    float motor_time, motor_strength;    // muscle tone after death
    float sleep_time, remove_time;       // freeze after resting, remove after
    int max_ragdolls;
    float friction;
} RagdollDef;

// radii: per-bone fitted radius (skin_fit_bone_radii) or NULL.
bool ragdoll_def_build(RagdollDef *d, const Skeleton *s, const float *radii, const struct cJSON *json, char *err, int err_size);
// Capsule endpoints of a body for a bone transform (hitboxes).
void ragdoll_body_segment(const RagBodyDef *b, Xform bone_world, float scale, vec3 *a, vec3 *bpt, float *radius);

typedef struct Ragdoll {
    bool active, frozen, settled;
    int body_count, joint_count;
    BodyId body[RAG_MAX_BODIES];
    ConstraintId joint[RAG_MAX_BODIES];
    void *group;
    float age, rest_time;
    float scale;
    Xform frozen_world[RAG_MAX_BODIES];
    quat target_rel[RAG_MAX_BODIES];     // muscle-tone targets (relative to the parent body)
    Pose death_pose;                     // local pose at death (bones not driven by bodies)
    Xform root;                          // character root at death (settled / off-screen poses)
    float remove_time;                   // seconds until removal (0 = the definition's)
} Ragdoll;

typedef struct RagdollSpawn {
    const Xform *bone_world;     // current bone world transforms
    const Xform *prev_world;     // previous tick (velocity injection), may be NULL
    float dt;
    vec3 impulse;                // killing-hit impulse (N*s)
    int hit_part;                // BodyPart of the killing hit (-1 = pelvis)
    vec3 hit_point;
    float scale;
    u32 group_id;
    bool solid;                  // collide with characters
    const Pose *pose;            // local animated pose
    Xform root;
    float remove_time;           // override (anim_ragdoll_time), 0 = definition
} RagdollSpawn;

bool ragdoll_create(Ragdoll *r, PhysicsWorld *w, const RagdollDef *d, const Skeleton *s, const RagdollSpawn *sp);
void ragdoll_destroy(Ragdoll *r, PhysicsWorld *w);
// Muscle tone, sleep detection and freezing. Returns false when the ragdoll should be removed.
bool ragdoll_step(Ragdoll *r, PhysicsWorld *w, const RagdollDef *d, float dt);
// Bone world transforms driven by the bodies (others follow their parents with the death pose).
void ragdoll_read_pose(const Ragdoll *r, PhysicsWorld *w, const RagdollDef *d, const Skeleton *s, Xform *bone_world);
// Push an active ragdoll (test stand).
void ragdoll_push(Ragdoll *r, PhysicsWorld *w, vec3 impulse);
// Settled lying pose without simulation (late joiners / off-screen), chosen by seed.
void ragdoll_settled_pose(const Skeleton *s, const Pose *base, Xform root, u8 seed, Xform *bone_world);
// Debug: shapes, joint cones, mass labels.
void ragdoll_debug_draw(const Ragdoll *r, PhysicsWorld *w, const RagdollDef *d);

#endif
