// Ragdoll tests with real Jolt physics (run from the build output dir so assets/ resolves):
// ragdolls dropped in several poses / heights / pushes for 10 s each; every joint must stay
// within its limits (+ solver tolerance) every tick, nothing explodes, sinks through the floor
// or produces NaNs, and the bodies come to rest and freeze.
#include "anim/anim.h"
#include "anim/anim_library.h"
#include "physics/physics.h"
#include "physics/ragdoll.h"
#include "core/engine.h"
#include "core/log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;
static AnimLibrary *L;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// Debug draw stubs (physics.c / ragdoll.c reference them; nothing is drawn here).
void dbg_line(vec3 a, vec3 b, u32 color, float duration, bool depth) { (void)a; (void)b; (void)color; (void)duration; (void)depth; }
void dbg_circle(vec3 c, vec3 n, float r, u32 color, float duration) { (void)c; (void)n; (void)r; (void)color; (void)duration; }
void dbg_capsule_axis(vec3 a, vec3 b, float r, u32 color, float duration) { (void)a; (void)b; (void)r; (void)color; (void)duration; }
void dbg_capsule(vec3 a, float r, float h, u32 color, float duration) { (void)a; (void)r; (void)h; (void)color; (void)duration; }
void dbg_box(vec3 c, vec3 h, quat q, u32 color, float duration) { (void)c; (void)h; (void)q; (void)color; (void)duration; }
void dbg_sphere(vec3 c, float r, u32 color, float duration) { (void)c; (void)r; (void)color; (void)duration; }
void dbg_text(vec3 p, const char *t, u32 color, float size, float duration) { (void)p; (void)t; (void)color; (void)size; (void)duration; }

// Jolt's iterative solver lets joints overshoot briefly on hard impacts (a 3 m head-first drop
// drives the head into the floor under the whole body); once the body settles every joint must be
// back inside its limits.
#define TRANSIENT_TOL_DEG 20.0f
#define SETTLED_TOL_DEG 3.0f

typedef struct JointStats { float worst_over; int worst_joint; int worst_tick; } JointStats;

// Joint rotation relative to the bind pose in the child's bind frame (Jolt's convention: the
// constraint rotation is swing * twist with the twist about the child's twist axis).
static quat joint_delta(const RagdollDef *d, const RagJointDef *j, quat qp, quat qc) {
    quat rel0 = quat_mul(quat_conj(d->bodies[j->parent].bind.r), d->bodies[j->child].bind.r);
    quat rel = quat_mul(quat_conj(qp), qc);
    return quat_norm(quat_mul(quat_conj(rel0), rel));
}

static float angle_of(quat q) {
    float v = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
    return 2.0f * atan2f(v, fabsf(q.w));
}

// How far (deg) a joint is beyond its limits (0 = inside).
static float joint_violation(const RagdollDef *d, const RagJointDef *j, quat delta) {
    vec3 axis = v3_norm(j->axis_c);
    quat twist = quat_twist(delta, axis);
    quat swing = quat_norm(quat_mul(delta, quat_conj(twist)));
    float tw = angle_of(twist) * DG_RAD2DEG, sw = angle_of(swing) * DG_RAD2DEG;
    float over = 0;
    if (j->type == RJ_HINGE) {
        float lim = fmaxf(fabsf(j->hinge_min), fabsf(j->hinge_max)) * DG_RAD2DEG;
        over = fmaxf(over, tw - lim);
        over = fmaxf(over, sw);                     // a hinge allows no off-axis rotation
    } else {
        float tlim = fmaxf(fabsf(j->twist_min), fabsf(j->twist_max)) * DG_RAD2DEG;
        float slim = fmaxf(j->cone_normal, j->cone_plane) * DG_RAD2DEG;
        over = fmaxf(over, tw - tlim);
        over = fmaxf(over, sw - slim);
    }
    (void)d;
    return over;
}

typedef struct DropCase { const char *name; float height; float pitch_deg, roll_deg; vec3 impulse; const char *clip; } DropCase;

static void run_drop(PhysicsWorld *pw, const DropCase *dc) {
    const Skeleton *s = &L->skel;
    const RagdollDef *d = &L->ragdoll;
    // Pose: bind or a clip frame, rotated and lifted.
    Pose pose;
    pose_bind(s, &pose);
    if (dc->clip) {
        int k = anim_lib_find_clip(L, dc->clip);
        if (k >= 0) clip_sample(&L->clips[k], L->clips[k].duration * 0.3f, &pose);
    }
    Xform model[ANIM_MAX_BONES], world[ANIM_MAX_BONES];
    pose_to_model(s, &pose, model);
    quat rot = quat_mul(quat_axis_angle(v3(1, 0, 0), dc->pitch_deg * DG_DEG2RAD), quat_axis_angle(v3(0, 0, 1), dc->roll_deg * DG_DEG2RAD));
    Xform root = xf_make(v3(0, dc->height, 0), rot);
    for (int i = 0; i < s->count; i++) world[i] = xf_mul(root, model[i]);

    Ragdoll r;
    RagdollSpawn sp;
    memset(&sp, 0, sizeof(sp));
    sp.bone_world = world;
    sp.prev_world = NULL;
    sp.dt = 1.0f / 60.0f;
    sp.impulse = dc->impulse;
    sp.hit_part = BP_CHEST;
    sp.scale = 1.0f;
    sp.group_id = 1;
    sp.pose = &pose;
    sp.root = root;
    bool ok = ragdoll_create(&r, pw, d, s, &sp);
    CHECK(ok, "%s: ragdoll_create failed", dc->name);
    if (!ok) return;

    JointStats js = { 0, -1, -1 };
    float settled_over = 0, settled_min_y = 1e9f;
    int settled_joint = -1;
    float max_speed = 0, min_y = 1e9f;
    bool nan = false;
    int frozen_at = -1;
    const float dt = 1.0f / 60.0f;
    for (int t = 0; t < 600; t++) {
        phys_step(pw, dt);
        ragdoll_step(&r, pw, d, dt);
        if (r.frozen && frozen_at < 0) frozen_at = t;
        quat q[RAG_MAX_BODIES];
        for (int b = 0; b < r.body_count; b++) {
            vec3 p;
            if (r.frozen) { p = r.frozen_world[b].t; q[b] = r.frozen_world[b].r; }
            else phys_get_transform(pw, r.body[b], &p, &q[b]);
            if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) || !isfinite(q[b].w)) nan = true;
            // Lowest capsule surface point (bodies are capsules along local Y).
            vec3 ax = quat_rotate(q[b], v3(0, 1, 0));
            float lo = p.y - fabsf(ax.y) * d->bodies[b].half_len - d->bodies[b].radius;
            min_y = fminf(min_y, lo);
            if (r.frozen || t == 599) settled_min_y = fminf(settled_min_y, lo);
            if (!r.frozen) max_speed = fmaxf(max_speed, v3_len(phys_get_velocity(pw, r.body[b])));
        }
        const char *trace = getenv("RAG_TRACE");
        for (int k = 0; k < d->joint_count; k++) {
            const RagJointDef *j = &d->joints[k];
            quat dl = joint_delta(d, j, q[j->parent], q[j->child]);
            float over = joint_violation(d, j, dl);
            if (trace && !strcmp(trace, d->bodies[j->child].name) && (t < 3 || t % 6 == 0) && t < 120) {
                quat tw = quat_twist(dl, v3_norm(j->axis_c));
                quat swq = quat_norm(quat_mul(dl, quat_conj(tw)));
                vec3 sa = quat_to_scaled_axis(swq);
                printf("    t%3d %s twist %6.1f swing %6.1f (axis %5.2f %5.2f %5.2f) over %5.1f | lim cone %.0f/%.0f twist %.0f..%.0f hinge %.0f..%.0f type %d\n", t,
                       d->bodies[j->child].name, angle_of(tw) * DG_RAD2DEG * (quat_to_scaled_axis(tw).x * j->axis_p.x + quat_to_scaled_axis(tw).y * j->axis_p.y + quat_to_scaled_axis(tw).z * j->axis_p.z >= 0 ? 1 : -1),
                       angle_of(swq) * DG_RAD2DEG, sa.x, sa.y, sa.z, over, j->cone_normal * DG_RAD2DEG, j->cone_plane * DG_RAD2DEG, j->twist_min * DG_RAD2DEG,
                       j->twist_max * DG_RAD2DEG, j->hinge_min * DG_RAD2DEG, j->hinge_max * DG_RAD2DEG, j->type);
            }
            if (over > js.worst_over) { js.worst_over = over; js.worst_joint = k; js.worst_tick = t; }
            if ((r.frozen || t == 599) && over > settled_over) { settled_over = over; settled_joint = k; }
        }
    }
    const char *wj = js.worst_joint >= 0 ? d->bodies[d->joints[js.worst_joint].child].name : "-";
    const char *sj = settled_joint >= 0 ? d->bodies[d->joints[settled_joint].child].name : "-";
    printf("  drop %-16s transient excess %5.2f deg (%s, tick %d)  settled %4.2f deg (%s)  max speed %5.2f m/s  lowest %+.3f m  frozen at tick %d\n", dc->name,
           js.worst_over, wj, js.worst_tick, settled_over, sj, max_speed, min_y, frozen_at);
    CHECK(!nan, "%s: NaN in ragdoll state", dc->name);
    CHECK(js.worst_over <= TRANSIENT_TOL_DEG, "%s: joint %s overshot its limit by %.2f deg (tick %d)", dc->name, wj, js.worst_over, js.worst_tick);
    CHECK(settled_over <= SETTLED_TOL_DEG, "%s: settled joint %s is %.2f deg outside its limit", dc->name, sj, settled_over);
    CHECK(max_speed < 25.0f, "%s: exploded (max body speed %.1f m/s)", dc->name, max_speed);
    CHECK(min_y > -0.07f, "%s: tunneled into the floor (lowest %.3f m)", dc->name, min_y);
    // Jolt lets resting contacts sit inside by its penetration slop (2 cm) + solver residue; the
    // skinned mesh is wider than the capsules, so < 5 cm is invisible.
    CHECK(settled_min_y > -0.05f, "%s: settled body %.3f m inside the floor", dc->name, settled_min_y);
    CHECK(frozen_at >= 0, "%s: never came to rest within 10 s", dc->name);
    ragdoll_destroy(&r, pw);
}

static void test_ragdoll_def(void) {
    const RagdollDef *d = &L->ragdoll;
    CHECK(d->body_count >= 15, "ragdoll bodies %d", d->body_count);
    CHECK(d->joint_count == d->body_count - 1, "joints %d for %d bodies", d->joint_count, d->body_count);
    CHECK(fabsf(d->total_mass - 80.0f) < 0.5f, "total mass %.1f", d->total_mass);
    for (int k = 0; k < d->joint_count; k++) {
        const RagJointDef *j = &d->joints[k];
        CHECK(j->parent >= 0 && j->parent < d->body_count && j->child > 0 && j->child < d->body_count, "joint %d bodies %d -> %d", k, j->parent, j->child);
        CHECK(fabsf(v3_len(j->axis_p) - 1.0f) < 1e-3f, "joint %d axis not normalized", k);
        if (j->type == RJ_HINGE) CHECK(j->hinge_max > j->hinge_min, "joint %d hinge range", k);
        else CHECK(j->cone_normal > 0 && j->cone_plane > 0 && j->twist_max >= j->twist_min, "joint %d swing-twist limits", k);
    }
    // Every body part has a body (hitboxes + reactions).
    for (int p = 0; p < BP_COUNT; p++) {
        bool found = false;
        for (int b = 0; b < d->body_count; b++) found |= d->bodies[b].part == p;
        CHECK(found, "no ragdoll body for part %s", body_part_name(p));
    }
}

int main(void) {
    log_init(NULL);
    L = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    if (!anim_lib_load(L, 30.0f)) { printf("FAIL: animation library failed to load (run from the build output dir)\n"); return 1; }
    test_ragdoll_def();

    phys_global_init();
    PhysicsWorld *pw = phys_world_create();
    BodyDesc floor;
    memset(&floor, 0, sizeof(floor));
    floor.shape.kind = SHAPE_BOX;
    floor.shape.half = v3(50, 0.5f, 50);
    floor.pos = v3(0, -0.5f, 0);
    floor.rot = quat_identity();
    floor.motion = PM_STATIC;
    floor.layer = PL_STATIC;
    floor.friction = 0.8f;
    phys_add_body(pw, &floor);
    phys_optimize(pw);
    phys_set_collision_steps(pw, 2);   // as the game does while ragdolls simulate

    static const DropCase cases[] = {
        { "bind T-pose 1m", 1.0f, 0, 0, { 0, 0, 0 }, NULL },
        { "idle fall back", 0.05f, 0, 0, { 0, 0, 90 }, "idle" },
        { "walk push side", 0.05f, 0, 0, { 160, 0, 0 }, "jog_forward" },
        { "head first 3m", 3.0f, 170, 0, { 0, 0, 0 }, NULL },
        { "rolled 2m spin", 2.0f, 40, 70, { 60, 40, -60 }, "sprint" },
    };
    for (int c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) run_drop(pw, &cases[c]);

    phys_world_destroy(pw);
    phys_global_shutdown();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
