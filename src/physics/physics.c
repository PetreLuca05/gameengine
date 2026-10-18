#include "physics_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "render/debug_draw.h"
#include "joltc.h"
#include "jolt_shim.h"
#include <stdlib.h>
#include <math.h>
#include <SDL3/SDL.h>

#define MAX_LAYER_FILTERS 32
#define MAX_BODY_PAIRS 65536
#define MAX_CONTACT_CONSTRAINTS 32768

struct PhysCharacter {
    JPH_CharacterVirtual *cv;
    JPH_Shape *stand, *crouch, *inner_stand, *inner_crouch;
    CharacterParams p;
    bool crouched;
};

typedef struct PhysicsGlobal {
    JPH_JobSystem *jobs;
    JPH_TempAllocator *temp;   // PhysicsSystem::Update scratch, sized once (see phys_global_init)
    u32 update_errors;         // JPH_PhysicsUpdateError bits already logged
    bool temp_fallback_logged;
    JPH_ObjectLayerFilter *layer_filters[MAX_LAYER_FILTERS];
    u32 layer_filter_masks[MAX_LAYER_FILTERS];
    int layer_filter_count;
} PhysicsGlobal;

#define PG (g_engine.physics)

static bool JPH_API_CALL layer_filter_cb(void *user, JPH_ObjectLayer layer) {
    u32 mask = (u32)(uintptr_t)user;
    return (mask & (1u << layer)) != 0;
}

static bool JPH_API_CALL body_filter_cb(void *user, JPH_BodyID id) { return id != (JPH_BodyID)(uintptr_t)user; }
static bool JPH_API_CALL body_filter_locked_cb(void *user, const JPH_Body *body) {
    return JPH_Body_GetID(body) != (JPH_BodyID)(uintptr_t)user;
}

static void JPH_API_CALL trace_cb(const char *msg) { log_info("[jolt] %s", msg); }

static bool JPH_API_CALL assert_cb(const char *expr, const char *msg, const char *file, uint32_t line) {
    log_error("[jolt] assert %s (%s) at %s:%u", expr, msg ? msg : "", file, line);
    return false; // don't break
}

void phys_global_init(void) {
    if (!JPH_Init()) log_fatal("JPH_Init failed");
    JPH_SetTraceHandler(trace_cb);
    JPH_SetAssertFailureHandler(assert_cb);
    PhysicsGlobal *pg = (PhysicsGlobal *)calloc(1, sizeof(PhysicsGlobal));
    g_engine.physics = pg;
    JobSystemThreadPoolConfig jc = { 0 };
    jc.maxJobs = 2048;
    jc.maxBarriers = 8;
    jc.numThreads = -1;
    pg->jobs = JPH_JobSystemThreadPool_Create(&jc);
    // Every step takes maxContactConstraints worst-case constraints (15 MB at 32768) from the temp
    // allocator, plus body pairs, CCD and island scratch. joltc's built-in allocator is 8 MB, so that
    // buffer went to malloc + free on every step; Jolt never checks the result, and once the machine
    // ran out of commit the null buffer crashed in ContactConstraintManager::CreateConstraint. One
    // fixed block, committed at startup, covers it (malloc fallback only past the 8 MB headroom).
    u32 temp_bytes = (u32)MAX_CONTACT_CONSTRAINTS * dgj_contact_constraint_bytes() + 8u * 1024 * 1024;
    pg->temp = dgj_temp_allocator_create(temp_bytes);
    if (!pg->temp) log_fatal("physics: cannot allocate %u MB of Jolt temp memory", temp_bytes >> 20);

    // joltc keeps the procs pointer, so the tables must have static storage (immutable).
    static const JPH_ObjectLayerFilter_Procs olp = { layer_filter_cb };
    static const JPH_BodyFilter_Procs bfp = { body_filter_cb, body_filter_locked_cb };
    JPH_ObjectLayerFilter_SetProcs(&olp);
    JPH_BodyFilter_SetProcs(&bfp);
    log_info("physics: Jolt initialized (%d job threads, %u MB step memory)", (int)(SDL_GetNumLogicalCPUCores() - 1), temp_bytes >> 20);
}

void phys_global_shutdown(void) {
    PhysicsGlobal *pg = PG;
    if (!pg) return;
    for (int i = 0; i < pg->layer_filter_count; i++) JPH_ObjectLayerFilter_Destroy(pg->layer_filters[i]);
    JPH_JobSystem_Destroy(pg->jobs);
    dgj_temp_allocator_destroy(pg->temp);
    free(pg);
    g_engine.physics = NULL;
    JPH_Shutdown();
}

static JPH_ObjectLayerFilter *layer_filter(u32 mask) {
    PhysicsGlobal *pg = PG;
    for (int i = 0; i < pg->layer_filter_count; i++)
        if (pg->layer_filter_masks[i] == mask) return pg->layer_filters[i];
    if (pg->layer_filter_count >= MAX_LAYER_FILTERS) return pg->layer_filters[0];
    JPH_ObjectLayerFilter *f = JPH_ObjectLayerFilter_Create((void *)(uintptr_t)mask);
    pg->layer_filters[pg->layer_filter_count] = f;
    pg->layer_filter_masks[pg->layer_filter_count++] = mask;
    return f;
}

PhysicsWorld *phys_world_create(void) {
    PhysicsWorld *w = (PhysicsWorld *)calloc(1, sizeof(PhysicsWorld));
    // Layer tables are created per world: JPH_PhysicsSystem_Destroy deletes the ones it was given.
    JPH_BroadPhaseLayerInterface *bp = JPH_BroadPhaseLayerInterfaceTable_Create(PL_COUNT, 2);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_STATIC, 0);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_MOVING, 1);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_CHARACTER, 1);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_DEBRIS, 1);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_TRIGGER, 1);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_RAGDOLL, 1);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_RAGDOLL_SOLID, 1);
    JPH_BroadPhaseLayerInterfaceTable_MapObjectToBroadPhaseLayer(bp, PL_CLIP, 0);
    JPH_ObjectLayerPairFilter *pairs = JPH_ObjectLayerPairFilterTable_Create(PL_COUNT);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_STATIC, PL_MOVING);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_STATIC, PL_CHARACTER);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_STATIC, PL_DEBRIS);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_MOVING, PL_MOVING);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_MOVING, PL_CHARACTER);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_CHARACTER, PL_CHARACTER);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_TRIGGER, PL_MOVING);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_TRIGGER, PL_CHARACTER);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_RAGDOLL, PL_STATIC);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_RAGDOLL, PL_RAGDOLL);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_RAGDOLL, PL_RAGDOLL_SOLID);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_RAGDOLL_SOLID, PL_STATIC);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_RAGDOLL_SOLID, PL_RAGDOLL_SOLID);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_RAGDOLL_SOLID, PL_CHARACTER);
    JPH_ObjectLayerPairFilterTable_EnableCollision(pairs, PL_CLIP, PL_CHARACTER);
    JPH_ObjectVsBroadPhaseLayerFilter *ovb = JPH_ObjectVsBroadPhaseLayerFilterTable_Create(bp, 2, pairs, PL_COUNT);
    JPH_PhysicsSystemSettings s = { 0 };
    s.maxBodies = MAX_BODIES;
    s.maxBodyPairs = MAX_BODY_PAIRS;
    s.maxContactConstraints = MAX_CONTACT_CONSTRAINTS;
    s.broadPhaseLayerInterface = bp;
    s.objectLayerPairFilter = pairs;
    s.objectVsBroadPhaseLayerFilter = ovb;
    w->system = JPH_PhysicsSystem_Create(&s);
    JPH_Vec3 g = { 0, -9.81f, 0 };
    JPH_PhysicsSystem_SetGravity(w->system, &g);
    w->bi = JPH_PhysicsSystem_GetBodyInterface(w->system);
    w->npq = JPH_PhysicsSystem_GetNarrowPhaseQuery(w->system);
    w->locks = JPH_PhysicsSystem_GetBodyLockInterface(w->system);
    w->records = (BodyRecord *)calloc(MAX_BODIES, sizeof(BodyRecord));
    return w;
}

void phys_world_destroy(PhysicsWorld *w) {
    if (!w) return;
    for (int i = 0; i < MAX_CONSTRAINTS; i++) phys_remove_constraint(w, i);
    for (int i = 0; i < MAX_BODIES; i++)
        if (w->records[i].used) JPH_BodyInterface_RemoveAndDestroyBody(w->bi, w->records[i].id);
    JPH_PhysicsSystem_Destroy(w->system);
    free(w->records);
    free(w);
}

void phys_step(PhysicsWorld *w, float dt) {
    PhysicsGlobal *pg = PG;
    u32 err = (u32)JPH_PhysicsSystem_Update2(w->system, dt, w->collision_steps > 0 ? w->collision_steps : 1, pg->temp, pg->jobs);
    // Full caches drop contacts (bodies sink / tunnel) rather than crash: log each kind once.
    u32 fresh = err & ~pg->update_errors;
    if (fresh) {
        pg->update_errors |= fresh;
        log_warn("physics: step overflow:%s%s%s (%u bodies, %u active; limits %d pairs, %d contacts)",
                 (fresh & JPH_PhysicsUpdateError_ManifoldCacheFull) ? " manifold cache full" : "",
                 (fresh & JPH_PhysicsUpdateError_BodyPairCacheFull) ? " body pair cache full" : "",
                 (fresh & JPH_PhysicsUpdateError_ContactConstraintsFull) ? " contact constraints full" : "",
                 JPH_PhysicsSystem_GetNumBodies(w->system), JPH_PhysicsSystem_GetNumActiveBodies(w->system, JPH_BodyType_Rigid),
                 MAX_BODY_PAIRS, MAX_CONTACT_CONSTRAINTS);
    }
    if (!pg->temp_fallback_logged && dgj_temp_allocator_fallbacks(pg->temp)) {
        pg->temp_fallback_logged = true;
        log_warn("physics: step scratch outgrew its fixed block (malloc per step from now on)");
    }
}
void phys_set_collision_steps(PhysicsWorld *w, int steps) { w->collision_steps = DG_CLAMP(steps, 1, 4); }
void phys_optimize(PhysicsWorld *w) { JPH_PhysicsSystem_OptimizeBroadPhase(w->system); }

static JPH_Shape *create_shape(const ShapeDesc *d) {
    switch (d->kind) {
    case SHAPE_BOX: {
        JPH_Vec3 h = { d->half.x, d->half.y, d->half.z };
        float cr = fminf(JPH_DEFAULT_CONVEX_RADIUS, fminf(h.x, fminf(h.y, h.z)) * 0.5f);
        return (JPH_Shape *)JPH_BoxShape_Create(&h, cr);
    }
    case SHAPE_SPHERE: return (JPH_Shape *)JPH_SphereShape_Create(d->radius);
    case SHAPE_CAPSULE: return (JPH_Shape *)JPH_CapsuleShape_Create(d->half_height, d->radius);
    case SHAPE_CYLINDER: return (JPH_Shape *)JPH_CylinderShape_Create(d->half_height, d->radius);
    case SHAPE_MESH: {
        u32 tri_count = d->index_count / 3;
        JPH_IndexedTriangle *tris = (JPH_IndexedTriangle *)malloc(sizeof(JPH_IndexedTriangle) * tri_count);
        for (u32 i = 0; i < tri_count; i++) {
            tris[i].i1 = d->indices[i * 3];
            tris[i].i2 = d->indices[i * 3 + 1];
            tris[i].i3 = d->indices[i * 3 + 2];
            tris[i].materialIndex = 0;
            tris[i].userData = 0;
        }
        JPH_MeshShapeSettings *ms = JPH_MeshShapeSettings_Create2((const JPH_Vec3 *)d->verts, d->vert_count, tris, tri_count);
        JPH_Shape *shape = (JPH_Shape *)JPH_MeshShapeSettings_CreateShape(ms);
        JPH_ShapeSettings_Destroy((JPH_ShapeSettings *)ms);
        free(tris);
        return shape;
    }
    case SHAPE_HEIGHTFIELD: {
        JPH_Vec3 off = { 0, 0, 0 };
        JPH_Vec3 scale = { d->spacing, 1.0f, d->spacing };
        JPH_HeightFieldShapeSettings *hs = JPH_HeightFieldShapeSettings_Create(d->heights, &off, &scale, d->sample_count, NULL);
        JPH_Shape *shape = (JPH_Shape *)JPH_HeightFieldShapeSettings_CreateShape(hs);
        JPH_ShapeSettings_Destroy((JPH_ShapeSettings *)hs);
        return shape;
    }
    }
    return NULL;
}

BodyId phys_add_body(PhysicsWorld *w, const BodyDesc *d) {
    JPH_Shape *shape = create_shape(&d->shape);
    if (!shape) { log_error("physics: shape creation failed (kind %d)", (int)d->shape.kind); return PHYS_INVALID_BODY; }
    JPH_RVec3 pos = { d->pos.x, d->pos.y, d->pos.z };
    JPH_Quat rot = { d->rot.x, d->rot.y, d->rot.z, d->rot.w };
    if (rot.x == 0 && rot.y == 0 && rot.z == 0 && rot.w == 0) rot.w = 1.0f;
    JPH_BodyCreationSettings *bcs = JPH_BodyCreationSettings_Create3(shape, &pos, &rot, (JPH_MotionType)d->motion, (JPH_ObjectLayer)d->layer);
    JPH_BodyCreationSettings_SetUserData(bcs, d->user);
    JPH_BodyCreationSettings_SetFriction(bcs, d->friction > 0 ? d->friction : 0.6f);
    JPH_BodyCreationSettings_SetRestitution(bcs, d->restitution);
    if (d->motion == PM_DYNAMIC) {
        JPH_BodyCreationSettings_SetLinearDamping(bcs, d->linear_damping > 0 ? d->linear_damping : 0.05f);
        JPH_BodyCreationSettings_SetAngularDamping(bcs, d->angular_damping > 0 ? d->angular_damping : 0.1f);
        JPH_BodyCreationSettings_SetMotionQuality(bcs, JPH_MotionQuality_LinearCast);
        if (d->mass > 0.0f) {
            JPH_MassProperties mp;
            memset(&mp, 0, sizeof(mp));
            JPH_Shape_GetMassProperties(shape, &mp);
            JPH_MassProperties_ScaleToMass(&mp, d->mass);
            JPH_BodyCreationSettings_SetMassPropertiesOverride(bcs, &mp);
            JPH_BodyCreationSettings_SetOverrideMassProperties(bcs, JPH_OverrideMassProperties_MassAndInertiaProvided);
        }
    }
    if (d->motion == PM_KINEMATIC) JPH_BodyCreationSettings_SetAllowDynamicOrKinematic(bcs, true);
    if (d->sensor) JPH_BodyCreationSettings_SetIsSensor(bcs, true);
    if (d->group_filter) {
        JPH_CollisionGroup g;
        memset(&g, 0, sizeof(g));
        g.groupFilter = (const JPH_GroupFilter *)d->group_filter;
        g.groupID = d->group_id;
        g.subGroupID = d->sub_group_id;
        JPH_BodyCreationSettings_SetCollisionGroup(bcs, &g);
        // Grouped bodies are ragdoll parts: firmer contacts + joints (light limbs under heavy torsos).
        JPH_BodyCreationSettings_SetNumVelocityStepsOverride(bcs, 16);
        JPH_BodyCreationSettings_SetNumPositionStepsOverride(bcs, 8);
    }
    JPH_Activation act = (d->motion == PM_STATIC || d->start_asleep) ? JPH_Activation_DontActivate : JPH_Activation_Activate;
    BodyId id = JPH_BodyInterface_CreateAndAddBody(w->bi, bcs, act);
    JPH_BodyCreationSettings_Destroy(bcs);
    JPH_Shape_Destroy(shape); // body holds its own reference
    if (id == PHYS_INVALID_BODY) { log_error("physics: body creation failed"); return id; }
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    r->used = true;
    r->id = id;
    r->shape = d->shape;
    r->layer = d->layer;
    r->motion = d->motion;
    r->user = d->user;
    r->material = d->material;
    w->body_count++;
    return id;
}

void phys_record_body(PhysicsWorld *w, BodyId id, vec3 half, PhysLayer layer, PhysMotion motion, u64 user, u8 material) {
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    memset(r, 0, sizeof(*r));
    r->used = true;
    r->id = id;
    r->shape.kind = SHAPE_BOX;
    r->shape.half = half;
    r->layer = layer;
    r->motion = motion;
    r->user = user;
    r->material = material;
    w->body_count++;
}

void phys_remove_body(PhysicsWorld *w, BodyId id) {
    if (id == PHYS_INVALID_BODY) return;
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    if (!r->used || r->id != id) return;
    if (JPH_BodyInterface_IsAdded(w->bi, id)) JPH_BodyInterface_RemoveBody(w->bi, id);
    JPH_BodyInterface_DestroyBody(w->bi, id);
    r->used = false;
    w->body_count--;
}

void phys_set_body_enabled(PhysicsWorld *w, BodyId id, bool enabled) {
    if (id == PHYS_INVALID_BODY) return;
    bool added = JPH_BodyInterface_IsAdded(w->bi, id);
    if (enabled && !added) JPH_BodyInterface_AddBody(w->bi, id, JPH_Activation_Activate);
    if (!enabled && added) JPH_BodyInterface_RemoveBody(w->bi, id);
}

void phys_get_transform(PhysicsWorld *w, BodyId id, vec3 *pos, quat *rot) {
    JPH_RVec3 p;
    JPH_Quat q;
    JPH_BodyInterface_GetPositionAndRotation(w->bi, id, &p, &q);
    if (pos) *pos = v3(p.x, p.y, p.z);
    if (rot) { rot->x = q.x; rot->y = q.y; rot->z = q.z; rot->w = q.w; }
}

void phys_set_transform(PhysicsWorld *w, BodyId id, vec3 pos, quat rot, bool activate) {
    JPH_RVec3 p = { pos.x, pos.y, pos.z };
    JPH_Quat q = { rot.x, rot.y, rot.z, rot.w };
    JPH_BodyInterface_SetPositionAndRotation(w->bi, id, &p, &q, activate ? JPH_Activation_Activate : JPH_Activation_DontActivate);
}

void phys_move_kinematic(PhysicsWorld *w, BodyId id, vec3 pos, quat rot, float dt) {
    JPH_RVec3 p = { pos.x, pos.y, pos.z };
    JPH_Quat q = { rot.x, rot.y, rot.z, rot.w };
    JPH_BodyInterface_MoveKinematic(w->bi, id, &p, &q, dt);
}

vec3 phys_get_velocity(PhysicsWorld *w, BodyId id) {
    JPH_Vec3 v;
    JPH_BodyInterface_GetLinearVelocity(w->bi, id, &v);
    return v3(v.x, v.y, v.z);
}

void phys_set_velocity(PhysicsWorld *w, BodyId id, vec3 lin, vec3 ang) {
    JPH_Vec3 l = { lin.x, lin.y, lin.z }, a = { ang.x, ang.y, ang.z };
    JPH_BodyInterface_SetLinearAndAngularVelocity(w->bi, id, &l, &a);
}

void phys_add_impulse(PhysicsWorld *w, BodyId id, vec3 impulse, vec3 point) {
    if (id == PHYS_INVALID_BODY) return;
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    if (!r->used || r->motion != PM_DYNAMIC) return;
    JPH_Vec3 i = { impulse.x, impulse.y, impulse.z };
    JPH_RVec3 p = { point.x, point.y, point.z };
    JPH_BodyInterface_ActivateBody(w->bi, id);
    JPH_BodyInterface_AddImpulse2(w->bi, id, &i, &p);
}

void phys_set_motion(PhysicsWorld *w, BodyId id, PhysMotion m) {
    JPH_BodyInterface_SetMotionType(w->bi, id, (JPH_MotionType)m, JPH_Activation_Activate);
    w->records[BODY_INDEX(id)].motion = m;
}

bool phys_is_active(PhysicsWorld *w, BodyId id) { return JPH_BodyInterface_IsActive(w->bi, id); }

u64 phys_body_user(PhysicsWorld *w, BodyId id) {
    if (id == PHYS_INVALID_BODY) return 0;
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    return (r->used && r->id == id) ? r->user : 0;
}

int phys_body_count(PhysicsWorld *w) { return w->body_count; }

// ------------------------------------------------------------------ constraints

static ConstraintId store_constraint(PhysicsWorld *w, JPH_Constraint *c) {
    if (!c) return -1;
    for (int k = 0; k < MAX_CONSTRAINTS; k++) {
        int i = (w->constraint_hint + k) % MAX_CONSTRAINTS;
        if (!w->constraints[i]) {
            w->constraints[i] = c;
            w->constraint_hint = i + 1;
            JPH_PhysicsSystem_AddConstraint(w->system, c);
            return i;
        }
    }
    JPH_Constraint_Destroy(c);
    log_warn("physics: constraint table full");
    return -1;
}

static JPH_Vec3 jv3(vec3 v) { JPH_Vec3 r = { v.x, v.y, v.z }; return r; }
static JPH_RVec3 jrv3(vec3 v) { JPH_RVec3 r = { v.x, v.y, v.z }; return r; }

ConstraintId phys_add_swing_twist(PhysicsWorld *w, BodyId a, BodyId b, vec3 pa, vec3 ta, vec3 pla, vec3 pb, vec3 tb, vec3 plb,
                                  float normal_cone, float plane_cone, float twist_min, float twist_max, float friction) {
    JPH_SwingTwistConstraintSettings s;
    JPH_SwingTwistConstraintSettings_Init(&s);
    // Ragdoll joints: extra solver iterations keep light limbs / the head inside their limits on impact.
    s.base.numVelocityStepsOverride = 16;
    s.base.numPositionStepsOverride = 8;
    s.space = JPH_ConstraintSpace_LocalToBodyCOM;
    s.position1 = jrv3(pa);
    s.twistAxis1 = jv3(ta);
    s.planeAxis1 = jv3(pla);
    s.position2 = jrv3(pb);
    s.twistAxis2 = jv3(tb);
    s.planeAxis2 = jv3(plb);
    s.swingType = JPH_SwingType_Cone;
    s.normalHalfConeAngle = normal_cone;
    s.planeHalfConeAngle = plane_cone;
    s.twistMinAngle = twist_min;
    s.twistMaxAngle = twist_max;
    s.maxFrictionTorque = friction;
    JPH_BodyID ids[2] = { a, b };
    JPH_BodyLockMultiWrite *lk = JPH_BodyLockInterface_LockMultiWrite(w->locks, ids, 2);
    JPH_Body *ba = JPH_BodyLockMultiWrite_GetBody(lk, 0), *bb = JPH_BodyLockMultiWrite_GetBody(lk, 1);
    JPH_Constraint *c = (ba && bb) ? (JPH_Constraint *)JPH_SwingTwistConstraint_Create(&s, ba, bb) : NULL;
    JPH_BodyLockMultiWrite_Destroy(lk);
    return store_constraint(w, c);
}

ConstraintId phys_add_hinge(PhysicsWorld *w, BodyId a, BodyId b, vec3 pa, vec3 ha, vec3 na, vec3 pb, vec3 hb, vec3 nb,
                            float min_angle, float max_angle, float friction) {
    JPH_HingeConstraintSettings s;
    JPH_HingeConstraintSettings_Init(&s);
    s.base.numVelocityStepsOverride = 16;
    s.base.numPositionStepsOverride = 8;
    s.space = JPH_ConstraintSpace_LocalToBodyCOM;
    s.point1 = jrv3(pa);
    s.hingeAxis1 = jv3(ha);
    s.normalAxis1 = jv3(na);
    s.point2 = jrv3(pb);
    s.hingeAxis2 = jv3(hb);
    s.normalAxis2 = jv3(nb);
    s.limitsMin = min_angle;
    s.limitsMax = max_angle;
    s.maxFrictionTorque = friction;
    JPH_BodyID ids[2] = { a, b };
    JPH_BodyLockMultiWrite *lk = JPH_BodyLockInterface_LockMultiWrite(w->locks, ids, 2);
    JPH_Body *ba = JPH_BodyLockMultiWrite_GetBody(lk, 0), *bb = JPH_BodyLockMultiWrite_GetBody(lk, 1);
    JPH_Constraint *c = (ba && bb) ? (JPH_Constraint *)JPH_HingeConstraint_Create(&s, ba, bb) : NULL;
    JPH_BodyLockMultiWrite_Destroy(lk);
    return store_constraint(w, c);
}

void phys_remove_constraint(PhysicsWorld *w, ConstraintId c) {
    if (c < 0 || c >= MAX_CONSTRAINTS || !w->constraints[c]) return;
    JPH_PhysicsSystem_RemoveConstraint(w->system, w->constraints[c]);
    JPH_Constraint_Destroy(w->constraints[c]);
    w->constraints[c] = NULL;
}

void *phys_group_filter_create(int sub_groups) { return JPH_GroupFilterTable_Create((uint32_t)sub_groups); }
void phys_group_filter_disable(void *f, int a, int b) {
    JPH_GroupFilterTable_DisableCollision((JPH_GroupFilterTable *)f, (JPH_CollisionSubGroupID)a, (JPH_CollisionSubGroupID)b);
}
void phys_group_filter_destroy(void *f) { if (f) JPH_GroupFilter_Destroy((JPH_GroupFilter *)f); }

vec3 phys_get_angular_velocity(PhysicsWorld *w, BodyId id) {
    JPH_Vec3 l, v;
    JPH_BodyInterface_GetLinearAndAngularVelocity(w->bi, id, &l, &v);
    return v3(v.x, v.y, v.z);
}

bool phys_body_shape(PhysicsWorld *w, BodyId id, ShapeDesc *out) {
    if (id == PHYS_INVALID_BODY) return false;
    const BodyRecord *r = &w->records[BODY_INDEX(id)];
    if (!r->used || r->id != id) return false;
    *out = r->shape;
    return true;
}

float phys_body_mass(PhysicsWorld *w, BodyId id) {
    if (id == PHYS_INVALID_BODY) return 0.0f;
    const BodyRecord *r = &w->records[BODY_INDEX(id)];
    if (!r->used || r->id != id || r->motion != PM_DYNAMIC) return 0.0f;
    JPH_BodyLockRead lock;
    JPH_BodyLockInterface_LockRead(w->locks, id, &lock);
    float mass = 0.0f;
    if (lock.body) {
        JPH_MotionProperties *mp = JPH_Body_GetMotionProperties((JPH_Body *)lock.body);
        float inv = mp ? JPH_MotionProperties_GetInverseMassUnchecked(mp) : 0.0f;
        mass = inv > 0.0f ? 1.0f / inv : 0.0f;
    }
    JPH_BodyLockInterface_UnlockRead(w->locks, &lock);
    return mass;
}

vec3 phys_body_com(PhysicsWorld *w, BodyId id) {
    JPH_RVec3 p;
    JPH_BodyInterface_GetCenterOfMassPosition(w->bi, id, &p);
    return v3(p.x, p.y, p.z);
}

void phys_add_impulse_com(PhysicsWorld *w, BodyId id, vec3 impulse, vec3 angular_impulse) {
    if (id == PHYS_INVALID_BODY) return;
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    if (!r->used || r->motion != PM_DYNAMIC) return;
    JPH_Vec3 i = { impulse.x, impulse.y, impulse.z }, a = { angular_impulse.x, angular_impulse.y, angular_impulse.z };
    JPH_BodyInterface_ActivateBody(w->bi, id);
    JPH_BodyInterface_AddImpulse(w->bi, id, &i);
    JPH_BodyInterface_AddAngularImpulse(w->bi, id, &a);
}

// ------------------------------------------------------------------ queries

static vec3 surface_normal(PhysicsWorld *w, BodyId body, JPH_SubShapeID sub, vec3 point) {
    JPH_BodyLockRead lock;
    JPH_BodyLockInterface_LockRead(w->locks, body, &lock);
    vec3 n = v3(0, 1, 0);
    if (lock.body) {
        JPH_RVec3 p = { point.x, point.y, point.z };
        JPH_Vec3 jn;
        JPH_Body_GetWorldSpaceSurfaceNormal(lock.body, sub, &p, &jn);
        n = v3(jn.x, jn.y, jn.z);
    }
    JPH_BodyLockInterface_UnlockRead(w->locks, &lock);
    return n;
}

int phys_body_material(PhysicsWorld *w, BodyId id) {
    if (id == PHYS_INVALID_BODY) return 0;
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    return (r->used && r->id == id) ? r->material : 0;
}
void phys_set_body_material(PhysicsWorld *w, BodyId id, int material) {
    if (id == PHYS_INVALID_BODY) return;
    BodyRecord *r = &w->records[BODY_INDEX(id)];
    if (r->used && r->id == id) r->material = (u8)material;
}

bool phys_raycast(PhysicsWorld *w, vec3 origin, vec3 dir_len, u32 mask, BodyId ignore, RayHit *out) {
    JPH_RVec3 o = { origin.x, origin.y, origin.z };
    JPH_Vec3 d = { dir_len.x, dir_len.y, dir_len.z };
    JPH_RayCastResult hit;
    memset(&hit, 0, sizeof(hit));
    JPH_BodyFilter *bf = ignore != PHYS_INVALID_BODY ? JPH_BodyFilter_Create((void *)(uintptr_t)ignore) : NULL;
    bool ok = JPH_NarrowPhaseQuery_CastRay(w->npq, &o, &d, &hit, NULL, layer_filter(mask), bf);
    if (bf) JPH_BodyFilter_Destroy(bf);
    if (!ok) return false;
    if (out) {
        out->body = hit.bodyID;
        out->fraction = hit.fraction;
        out->point = v3_madd(origin, dir_len, hit.fraction);
        out->distance = v3_len(dir_len) * hit.fraction;
        out->normal = surface_normal(w, hit.bodyID, hit.subShapeID2, out->point);
        BodyRecord *r = &w->records[BODY_INDEX(hit.bodyID)];
        out->user = r->used ? r->user : 0;
        out->layer = r->layer;
        out->material = r->used ? r->material : 0;
    }
    return true;
}

typedef struct CastCtx { bool hit; JPH_ShapeCastResult best; } CastCtx;

static void JPH_API_CALL cast_cb(void *ctx_, const JPH_ShapeCastResult *r) {
    CastCtx *ctx = (CastCtx *)ctx_;
    if (!ctx->hit || r->fraction < ctx->best.fraction) { ctx->best = *r; ctx->hit = true; }
}

static JPH_RMat4 make_xf(vec3 pos, quat rot) {
    mat4 m = mat4_trs(pos, rot, v3s(1.0f));
    JPH_RMat4 r;
    memcpy(&r, &m, sizeof(r));
    return r;
}

static bool shape_cast(PhysicsWorld *w, JPH_Shape *shape, vec3 center, quat rot, vec3 dir_len, u32 mask, BodyId ignore, ShapeHit *out) {
    JPH_RMat4 xf = make_xf(center, rot);
    JPH_Vec3 d = { dir_len.x, dir_len.y, dir_len.z };
    JPH_ShapeCastSettings s;
    JPH_ShapeCastSettings_Init(&s);
    s.backFaceModeTriangles = JPH_BackFaceMode_IgnoreBackFaces;
    s.backFaceModeConvex = JPH_BackFaceMode_IgnoreBackFaces;
    JPH_RVec3 base = { 0, 0, 0 };
    CastCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    JPH_BodyFilter *bf = ignore != PHYS_INVALID_BODY ? JPH_BodyFilter_Create((void *)(uintptr_t)ignore) : NULL;
    JPH_NarrowPhaseQuery_CastShape2(w->npq, shape, &xf, &d, &s, &base, JPH_CollisionCollectorType_ClosestHit, cast_cb, &ctx,
                                    NULL, layer_filter(mask), bf, NULL);
    if (bf) JPH_BodyFilter_Destroy(bf);
    JPH_Shape_Destroy(shape);
    if (!ctx.hit) return false;
    if (out) {
        out->body = ctx.best.bodyID2;
        out->fraction = ctx.best.fraction;
        out->distance = v3_len(dir_len) * ctx.best.fraction;
        out->point = v3(ctx.best.contactPointOn2.x, ctx.best.contactPointOn2.y, ctx.best.contactPointOn2.z);
        vec3 pa = v3(ctx.best.penetrationAxis.x, ctx.best.penetrationAxis.y, ctx.best.penetrationAxis.z);
        out->normal = v3_len2(pa) > 1e-10f ? v3_neg(v3_norm(pa)) : v3_neg(v3_norm(dir_len));
        BodyRecord *r = &w->records[BODY_INDEX(ctx.best.bodyID2)];
        out->user = r->used ? r->user : 0;
    }
    return true;
}

bool phys_sphere_cast(PhysicsWorld *w, vec3 from, vec3 dir_len, float radius, u32 mask, BodyId ignore, ShapeHit *out) {
    return shape_cast(w, (JPH_Shape *)JPH_SphereShape_Create(radius), from, quat_identity(), dir_len, mask, ignore, out);
}

bool phys_capsule_cast(PhysicsWorld *w, vec3 bottom, float radius, float height, vec3 dir_len, u32 mask, BodyId ignore, ShapeHit *out) {
    float hh = fmaxf(0.01f, height * 0.5f - radius);
    vec3 c = v3_add(bottom, v3(0, height * 0.5f, 0));
    return shape_cast(w, (JPH_Shape *)JPH_CapsuleShape_Create(hh, radius), c, quat_identity(), dir_len, mask, ignore, out);
}

bool phys_box_cast(PhysicsWorld *w, vec3 center, vec3 half, quat rot, vec3 dir_len, u32 mask, BodyId ignore, ShapeHit *out) {
    JPH_Vec3 h = { half.x, half.y, half.z };
    float cr = fminf(0.02f, fminf(half.x, fminf(half.y, half.z)) * 0.5f);
    return shape_cast(w, (JPH_Shape *)JPH_BoxShape_Create(&h, cr), center, rot, dir_len, mask, ignore, out);
}

typedef struct OverlapCtx { bool hit; } OverlapCtx;
static void JPH_API_CALL overlap_cb(void *ctx_, const JPH_CollideShapeResult *r) {
    (void)r;
    ((OverlapCtx *)ctx_)->hit = true;
}

bool phys_capsule_overlap(PhysicsWorld *w, vec3 bottom, float radius, float height, u32 mask, BodyId ignore) {
    float hh = fmaxf(0.01f, height * 0.5f - radius);
    JPH_Shape *shape = (JPH_Shape *)JPH_CapsuleShape_Create(hh, radius);
    JPH_RMat4 xf = make_xf(v3_add(bottom, v3(0, height * 0.5f, 0)), quat_identity());
    JPH_Vec3 scale = { 1, 1, 1 };
    JPH_CollideShapeSettings s;
    JPH_CollideShapeSettings_Init(&s);
    JPH_RVec3 base = { 0, 0, 0 };
    OverlapCtx ctx = { false };
    JPH_BodyFilter *bf = ignore != PHYS_INVALID_BODY ? JPH_BodyFilter_Create((void *)(uintptr_t)ignore) : NULL;
    JPH_NarrowPhaseQuery_CollideShape2(w->npq, shape, &scale, &xf, &s, &base, JPH_CollisionCollectorType_AnyHit, overlap_cb, &ctx,
                                       NULL, layer_filter(mask), bf, NULL);
    if (bf) JPH_BodyFilter_Destroy(bf);
    JPH_Shape_Destroy(shape);
    return ctx.hit;
}

typedef struct BodyCollect { BodyId *out; int count, max; } BodyCollect;
static float JPH_API_CALL collect_body_cb(void *ctx_, const JPH_BodyID id) {
    BodyCollect *c = (BodyCollect *)ctx_;
    if (c->count < c->max) c->out[c->count++] = id;
    return 1.0f;
}

int phys_sphere_overlap_bodies(PhysicsWorld *w, vec3 center, float radius, u32 mask, BodyId *out, int max) {
    BodyCollect c = { out, 0, max };
    JPH_Vec3 jc = { center.x, center.y, center.z };
    const JPH_BroadPhaseQuery *bpq = JPH_PhysicsSystem_GetBroadPhaseQuery(w->system);
    JPH_BroadPhaseQuery_CollideSphere(bpq, &jc, radius, collect_body_cb, &c, NULL, layer_filter(mask));
    return c.count;
}

// ------------------------------------------------------------------ character

static JPH_Shape *character_shape(float radius, float height, float shrink) {
    float r = radius * shrink;
    float hh = fmaxf(0.01f, height * 0.5f - radius);
    JPH_Shape *cap = (JPH_Shape *)JPH_CapsuleShape_Create(hh * shrink, r);
    JPH_Vec3 off = { 0, height * 0.5f, 0 };
    JPH_Shape *rt = (JPH_Shape *)JPH_RotatedTranslatedShape_Create(&off, NULL, cap);
    JPH_Shape_Destroy(cap);
    return rt;
}

PhysCharacter *phys_character_create(PhysicsWorld *w, const CharacterParams *p, vec3 feet, u64 user) {
    PhysCharacter *c = (PhysCharacter *)calloc(1, sizeof(PhysCharacter));
    c->p = *p;
    c->stand = character_shape(p->radius, p->height_stand, 1.0f);
    c->crouch = character_shape(p->radius, p->height_crouch, 1.0f);
    c->inner_stand = character_shape(p->radius, p->height_stand, 0.9f);
    c->inner_crouch = character_shape(p->radius, p->height_crouch, 0.9f);
    JPH_CharacterVirtualSettings s;
    JPH_CharacterVirtualSettings_Init(&s);
    s.base.shape = c->stand;
    s.base.up = (JPH_Vec3){ 0, 1, 0 };
    s.base.maxSlopeAngle = p->max_slope_deg * DG_DEG2RAD;
    s.base.supportingVolume.normal = (JPH_Vec3){ 0, 1, 0 };
    s.base.supportingVolume.distance = -p->radius;
    s.base.enhancedInternalEdgeRemoval = true;
    s.mass = p->mass > 0 ? p->mass : 80.0f;
    s.maxStrength = p->max_strength > 0 ? p->max_strength : 300.0f;
    s.characterPadding = 0.02f;
    s.penetrationRecoverySpeed = 1.0f;
    s.predictiveContactDistance = 0.1f;
    s.innerBodyShape = c->inner_stand;
    s.innerBodyLayer = PL_CHARACTER;
    JPH_RVec3 pos = { feet.x, feet.y, feet.z };
    c->cv = JPH_CharacterVirtual_Create(&s, &pos, NULL, user, w->system);
    return c;
}

void phys_character_destroy(PhysicsWorld *w, PhysCharacter *c) {
    (void)w;
    if (!c) return;
    JPH_CharacterBase_Destroy((JPH_CharacterBase *)c->cv);
    JPH_Shape_Destroy(c->stand);
    JPH_Shape_Destroy(c->crouch);
    JPH_Shape_Destroy(c->inner_stand);
    JPH_Shape_Destroy(c->inner_crouch);
    free(c);
}

bool phys_character_set_crouched(PhysicsWorld *w, PhysCharacter *c, bool crouched) {
    if (c->crouched == crouched) return true;
    JPH_Shape *target = crouched ? c->crouch : c->stand;
    // Standing up may fail if there is no head room (max penetration 0).
    float max_pen = crouched ? 1e6f : 0.0f;
    if (!JPH_CharacterVirtual_SetShape(c->cv, target, max_pen, PL_CHARACTER, w->system, NULL, NULL)) return false;
    JPH_CharacterVirtual_SetInnerBodyShape(c->cv, crouched ? c->inner_crouch : c->inner_stand);
    c->crouched = crouched;
    return true;
}

bool phys_character_is_crouched(PhysCharacter *c) { return c->crouched; }

void phys_character_update(PhysicsWorld *w, PhysCharacter *c, float dt, vec3 vel, const CharacterMoveParams *mp) {
    JPH_Vec3 v = { vel.x, vel.y, vel.z };
    JPH_CharacterVirtual_SetLinearVelocity(c->cv, &v);
    JPH_ExtendedUpdateSettings s;
    memset(&s, 0, sizeof(s));
    s.stickToFloorStepDown = (JPH_Vec3){ 0, mp->allow_stick ? -mp->stick_down : 0.0f, 0 };
    s.walkStairsStepUp = (JPH_Vec3){ 0, mp->allow_stairs ? mp->step_up : 0.0f, 0 };
    s.walkStairsMinStepForward = 0.02f;
    s.walkStairsStepForwardTest = 0.15f;
    s.walkStairsCosAngleForwardContact = cosf(75.0f * DG_DEG2RAD);
    s.walkStairsStepDownExtra = (JPH_Vec3){ 0, 0, 0 };
    JPH_CharacterVirtual_ExtendedUpdate(c->cv, dt, &s, PL_CHARACTER, w->system, NULL, NULL);
}

vec3 phys_character_pos(PhysCharacter *c) {
    JPH_RVec3 p;
    JPH_CharacterVirtual_GetPosition(c->cv, &p);
    return v3(p.x, p.y, p.z);
}

vec3 phys_character_vel(PhysCharacter *c) {
    JPH_Vec3 v;
    JPH_CharacterVirtual_GetLinearVelocity(c->cv, &v);
    return v3(v.x, v.y, v.z);
}

void phys_character_set_pos(PhysCharacter *c, vec3 feet) {
    JPH_RVec3 p = { feet.x, feet.y, feet.z };
    JPH_CharacterVirtual_SetPosition(c->cv, &p);
}

void phys_character_set_vel(PhysCharacter *c, vec3 v) {
    JPH_Vec3 jv = { v.x, v.y, v.z };
    JPH_CharacterVirtual_SetLinearVelocity(c->cv, &jv);
}

GroundState phys_character_ground(PhysCharacter *c) { return (GroundState)JPH_CharacterBase_GetGroundState((JPH_CharacterBase *)c->cv); }

vec3 phys_character_ground_normal(PhysCharacter *c) {
    JPH_Vec3 n;
    JPH_CharacterBase_GetGroundNormal((JPH_CharacterBase *)c->cv, &n);
    return v3(n.x, n.y, n.z);
}

vec3 phys_character_ground_velocity(PhysCharacter *c) {
    JPH_Vec3 v;
    JPH_CharacterBase_GetGroundVelocity((JPH_CharacterBase *)c->cv, &v);
    return v3(v.x, v.y, v.z);
}

u64 phys_character_ground_user(PhysCharacter *c) { return JPH_CharacterBase_GetGroundUserData((JPH_CharacterBase *)c->cv); }
BodyId phys_character_inner_body(PhysCharacter *c) { return JPH_CharacterVirtual_GetInnerBodyID(c->cv); }

void phys_character_set_max_slope(PhysCharacter *c, float deg) {
    JPH_CharacterBase_SetMaxSlopeAngle((JPH_CharacterBase *)c->cv, deg * DG_DEG2RAD);
}

vec3 phys_character_cancel_steep(PhysCharacter *c, vec3 v) {
    JPH_Vec3 in = { v.x, v.y, v.z }, out;
    JPH_CharacterVirtual_CancelVelocityTowardsSteepSlopes(c->cv, &in, &out);
    return v3(out.x, out.y, out.z);
}

// ------------------------------------------------------------------ debug draw

void phys_debug_draw(PhysicsWorld *w, vec3 around, float radius) {
    float r2 = radius * radius;
    for (int i = 0; i < MAX_BODIES; i++) {
        BodyRecord *r = &w->records[i];
        if (!r->used) continue;
        vec3 pos;
        quat rot;
        phys_get_transform(w, r->id, &pos, &rot);
        if (r->shape.kind != SHAPE_MESH && r->shape.kind != SHAPE_HEIGHTFIELD && v3_len2(v3_sub(pos, around)) > r2) continue;
        u32 col = r->layer == PL_STATIC ? DBG_GREEN : (r->motion == PM_DYNAMIC ? (phys_is_active(w, r->id) ? DBG_ORANGE : DBG_YELLOW) : DBG_CYAN);
        if (r->layer == PL_CHARACTER) col = DBG_MAGENTA;
        if (r->layer == PL_TRIGGER) col = DBG_BLUE;
        const ShapeDesc *s = &r->shape;
        switch (s->kind) {
        case SHAPE_BOX: dbg_box(pos, s->half, rot, col, 0); break;
        case SHAPE_SPHERE: dbg_sphere(pos, s->radius, col, 0); break;
        case SHAPE_CAPSULE:
        case SHAPE_CYLINDER: {
            vec3 up = quat_rotate(rot, v3(0, s->half_height, 0));
            dbg_capsule_axis(v3_sub(pos, up), v3_add(pos, up), s->radius, col, 0);
            break;
        }
        case SHAPE_MESH:
            if (s->verts && s->indices) {
                mat4 xf = mat4_trs(pos, rot, v3s(1));
                for (u32 t = 0; t + 2 < s->index_count; t += 3) {
                    vec3 a = mat4_mul_point(xf, s->verts[s->indices[t]]);
                    if (v3_len2(v3_sub(a, around)) > r2) continue;
                    vec3 b = mat4_mul_point(xf, s->verts[s->indices[t + 1]]);
                    vec3 c = mat4_mul_point(xf, s->verts[s->indices[t + 2]]);
                    dbg_line(a, b, col, 0, true);
                    dbg_line(b, c, col, 0, true);
                    dbg_line(c, a, col, 0, true);
                }
            }
            break;
        case SHAPE_HEIGHTFIELD:
            if (s->heights) {
                u32 n = s->sample_count;
                u32 step = 2;
                for (u32 z = 0; z + step < n; z += step)
                    for (u32 x = 0; x + step < n; x += step) {
                        vec3 a = v3_add(pos, v3(x * s->spacing, s->heights[z * n + x], z * s->spacing));
                        if (v3_len2(v3_sub(a, around)) > r2) continue;
                        vec3 b = v3_add(pos, v3((x + step) * s->spacing, s->heights[z * n + x + step], z * s->spacing));
                        vec3 c = v3_add(pos, v3(x * s->spacing, s->heights[(z + step) * n + x], (z + step) * s->spacing));
                        dbg_line(a, b, col, 0, true);
                        dbg_line(a, c, col, 0, true);
                    }
            }
            break;
        }
    }
}
