// Private state shared by src/physics/*.c (physics.c, phys_vehicle.c).
#ifndef DG_PHYSICS_INTERNAL_H
#define DG_PHYSICS_INTERNAL_H

#include "physics.h"
#include "joltc.h"

#define MAX_BODIES 16384
#define BODY_INDEX(id) ((id) & 0x7FFFFFu)
#define MAX_CONSTRAINTS 4096

typedef struct BodyRecord {
    bool used;
    BodyId id;
    ShapeDesc shape;
    PhysLayer layer;
    PhysMotion motion;
    u64 user;
    u8 material;
} BodyRecord;

struct PhysicsWorld {
    JPH_Constraint *constraints[MAX_CONSTRAINTS];
    int constraint_hint;
    JPH_PhysicsSystem *system;
    JPH_BodyInterface *bi;
    const JPH_NarrowPhaseQuery *npq;
    const JPH_BodyLockInterface *locks;
    BodyRecord *records;
    int body_count;
    int collision_steps;     // substeps per phys_step (2 while ragdolls simulate)
};

// Registers a body created outside phys_add_body (bookkeeping: user data, material, queries).
void phys_record_body(PhysicsWorld *w, BodyId id, vec3 half, PhysLayer layer, PhysMotion motion, u64 user, u8 material);

#endif
