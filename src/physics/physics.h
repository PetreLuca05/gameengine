// Jolt physics wrapper (via joltc). One PhysicsWorld per simulated world
// (the host's authoritative world, or a client's replica world).
#ifndef DG_PHYSICS_H
#define DG_PHYSICS_H

#include "core/base.h"
#include "core/dgmath.h"

typedef u32 BodyId;           // Jolt body id; PHYS_INVALID_BODY = none
#define PHYS_INVALID_BODY 0xFFFFFFFFu

typedef enum PhysLayer {
    PL_STATIC = 0,     // level geometry
    PL_MOVING,         // dynamic + kinematic props, doors
    PL_CHARACTER,      // character inner bodies, hitbox proxies
    PL_DEBRIS,         // small client-side bits (collide with static only)
    PL_TRIGGER,        // sensors
    PL_RAGDOLL,        // cosmetic ragdoll bodies: static + other ragdolls only (never pushes gameplay props)
    PL_RAGDOLL_SOLID,  // ragdolls that also block characters (anim_corpse_collision 1)
    PL_COUNT
} PhysLayer;

#define PL_MASK(l) (1u << (l))
#define PL_MASK_WORLD (PL_MASK(PL_STATIC) | PL_MASK(PL_MOVING))
#define PL_MASK_ALL (PL_MASK(PL_STATIC) | PL_MASK(PL_MOVING) | PL_MASK(PL_CHARACTER))

typedef enum PhysMotion { PM_STATIC = 0, PM_KINEMATIC = 1, PM_DYNAMIC = 2 } PhysMotion;

typedef enum ShapeKind { SHAPE_BOX, SHAPE_SPHERE, SHAPE_CAPSULE, SHAPE_CYLINDER, SHAPE_MESH, SHAPE_HEIGHTFIELD } ShapeKind;

typedef struct ShapeDesc {
    ShapeKind kind;
    vec3 half;            // box half extents
    float radius;         // sphere/capsule/cylinder
    float half_height;    // capsule/cylinder: half height of the cylinder part (Y axis)
    // mesh (vertices in body space). Data is copied by Jolt; debug draw keeps the pointers if keep_data.
    const vec3 *verts;
    u32 vert_count;
    const u32 *indices;
    u32 index_count;
    // heightfield: sample_count x sample_count heights, spacing in meters, origin at body position
    const float *heights;
    u32 sample_count;
    float spacing;
} ShapeDesc;

typedef struct BodyDesc {
    ShapeDesc shape;
    vec3 pos;
    quat rot;
    PhysMotion motion;
    PhysLayer layer;
    u64 user;            // entity id (0 = level static)
    float mass;          // dynamic only (0 = from density)
    float friction;
    float restitution;
    float linear_damping, angular_damping;
    bool sensor;
    bool start_asleep;
    // optional collision group (ragdolls: no collisions between joined bodies)
    void *group_filter;      // from phys_group_filter_create
    u32 group_id, sub_group_id;
    u8 material;             // SurfaceMaterial (src/vfx/vfx_types.h): impact VFX / sounds
} BodyDesc;

typedef struct RayHit {
    BodyId body;
    u64 user;
    vec3 point;
    vec3 normal;
    float fraction;
    float distance;
    PhysLayer layer;
    u8 material;
} RayHit;

typedef struct ShapeHit {
    BodyId body;
    u64 user;
    vec3 point;      // contact point on the hit body
    vec3 normal;     // surface normal pointing towards the caster
    float fraction;
    float distance;
} ShapeHit;

typedef enum GroundState { GROUND_ON = 0, GROUND_STEEP = 1, GROUND_UNSUPPORTED = 2, GROUND_AIR = 3 } GroundState;

typedef struct PhysicsWorld PhysicsWorld;
typedef struct PhysCharacter PhysCharacter;

typedef struct CharacterParams {
    float radius;
    float height_stand;
    float height_crouch;
    float max_slope_deg;
    float mass;
    float max_strength;   // push force on dynamic bodies (N)
} CharacterParams;

typedef struct CharacterMoveParams {
    float step_up;        // stair step height
    float stick_down;     // how far to snap down to the floor
    bool allow_stairs;
    bool allow_stick;
} CharacterMoveParams;

void phys_global_init(void);
void phys_global_shutdown(void);

PhysicsWorld *phys_world_create(void);
void phys_world_destroy(PhysicsWorld *w);
void phys_step(PhysicsWorld *w, float dt);
// Collision substeps per phys_step (default 1). Ragdolls need 2 to keep joints / contacts firm on impact.
void phys_set_collision_steps(PhysicsWorld *w, int steps);
void phys_optimize(PhysicsWorld *w);

BodyId phys_add_body(PhysicsWorld *w, const BodyDesc *desc);
void phys_remove_body(PhysicsWorld *w, BodyId id);
void phys_get_transform(PhysicsWorld *w, BodyId id, vec3 *pos, quat *rot);
void phys_set_transform(PhysicsWorld *w, BodyId id, vec3 pos, quat rot, bool activate);
void phys_move_kinematic(PhysicsWorld *w, BodyId id, vec3 pos, quat rot, float dt);
vec3 phys_get_velocity(PhysicsWorld *w, BodyId id);
void phys_set_velocity(PhysicsWorld *w, BodyId id, vec3 lin, vec3 ang);
void phys_add_impulse(PhysicsWorld *w, BodyId id, vec3 impulse, vec3 point);
void phys_set_motion(PhysicsWorld *w, BodyId id, PhysMotion m);
bool phys_is_active(PhysicsWorld *w, BodyId id);
u64 phys_body_user(PhysicsWorld *w, BodyId id);
void phys_set_body_enabled(PhysicsWorld *w, BodyId id, bool enabled); // add/remove from broadphase

// Queries. mask = PL_MASK(...) bits; ignore = body to skip (or PHYS_INVALID_BODY).
int phys_body_material(PhysicsWorld *w, BodyId id);
void phys_set_body_material(PhysicsWorld *w, BodyId id, int material);
bool phys_raycast(PhysicsWorld *w, vec3 origin, vec3 dir_len, u32 mask, BodyId ignore, RayHit *out);
bool phys_sphere_cast(PhysicsWorld *w, vec3 from, vec3 dir_len, float radius, u32 mask, BodyId ignore, ShapeHit *out);
bool phys_capsule_cast(PhysicsWorld *w, vec3 bottom_center, float radius, float total_height, vec3 dir_len, u32 mask, BodyId ignore, ShapeHit *out);
bool phys_box_cast(PhysicsWorld *w, vec3 center, vec3 half, quat rot, vec3 dir_len, u32 mask, BodyId ignore, ShapeHit *out);
// True if a capsule standing at bottom_center overlaps anything (clearance test).
bool phys_capsule_overlap(PhysicsWorld *w, vec3 bottom_center, float radius, float total_height, u32 mask, BodyId ignore);
int phys_sphere_overlap_bodies(PhysicsWorld *w, vec3 center, float radius, u32 mask, BodyId *out, int max);

// Character (Jolt CharacterVirtual). Position is at the feet.
PhysCharacter *phys_character_create(PhysicsWorld *w, const CharacterParams *p, vec3 feet, u64 user);
void phys_character_destroy(PhysicsWorld *w, PhysCharacter *c);
bool phys_character_set_crouched(PhysicsWorld *w, PhysCharacter *c, bool crouched); // false = blocked
bool phys_character_is_crouched(PhysCharacter *c);
void phys_character_update(PhysicsWorld *w, PhysCharacter *c, float dt, vec3 velocity, const CharacterMoveParams *mp);
vec3 phys_character_pos(PhysCharacter *c);
vec3 phys_character_vel(PhysCharacter *c);
void phys_character_set_pos(PhysCharacter *c, vec3 feet);
void phys_character_set_vel(PhysCharacter *c, vec3 v);
GroundState phys_character_ground(PhysCharacter *c);
vec3 phys_character_ground_normal(PhysCharacter *c);
vec3 phys_character_ground_velocity(PhysCharacter *c);
u64 phys_character_ground_user(PhysCharacter *c);
BodyId phys_character_inner_body(PhysCharacter *c);
void phys_character_set_max_slope(PhysCharacter *c, float deg);
vec3 phys_character_cancel_steep(PhysCharacter *c, vec3 desired_velocity);

// Constraints (ragdolls). Positions / axes are in each body's local (center of mass) space.
typedef int ConstraintId;   // -1 = invalid
ConstraintId phys_add_swing_twist(PhysicsWorld *w, BodyId a, BodyId b, vec3 pos_a, vec3 twist_a, vec3 plane_a, vec3 pos_b, vec3 twist_b,
                                  vec3 plane_b, float normal_cone, float plane_cone, float twist_min, float twist_max, float friction);
ConstraintId phys_add_hinge(PhysicsWorld *w, BodyId a, BodyId b, vec3 pos_a, vec3 hinge_a, vec3 normal_a, vec3 pos_b, vec3 hinge_b,
                            vec3 normal_b, float min_angle, float max_angle, float friction);
void phys_remove_constraint(PhysicsWorld *w, ConstraintId c);
// Group filters: bodies sharing group_id collide unless their sub groups were disabled.
void *phys_group_filter_create(int sub_groups);
void phys_group_filter_disable(void *filter, int sub_a, int sub_b);
void phys_group_filter_destroy(void *filter);
vec3 phys_get_angular_velocity(PhysicsWorld *w, BodyId id);

// Debug: draw collision shapes via debug lines.
void phys_debug_draw(PhysicsWorld *w, vec3 around, float radius);
int phys_body_count(PhysicsWorld *w);

#endif
