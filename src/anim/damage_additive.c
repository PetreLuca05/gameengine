// Procedural additive damage reactions: an impulse on the hit bone's chain (propagated up
// the parents with falloff) drives damped springs that return to zero in ~0.3-0.6 s.
// Purely additive and cosmetic: never touches hitboxes, the capsule or the aim.
#include "procedural_internal.h"
#include <math.h>

#define SPRING_SUBSTEPS 4
#define MAX_ANGLE 0.7f

static DamageSpring *spring_for(ProcState *s, int bone) {
    for (int i = 0; i < s->spring_count; i++) if (s->springs[i].bone == bone) return &s->springs[i];
    if (s->spring_count >= 16) {
        // Replace the calmest spring.
        int best = 0;
        float bm = 1e9f;
        for (int i = 0; i < s->spring_count; i++) {
            float m = v3_len2(s->springs[i].x) + v3_len2(s->springs[i].v) * 0.01f;
            if (m < bm) { bm = m; best = i; }
        }
        memset(&s->springs[best], 0, sizeof(DamageSpring));
        s->springs[best].bone = (i16)bone;
        return &s->springs[best];
    }
    DamageSpring *d = &s->springs[s->spring_count++];
    memset(d, 0, sizeof(*d));
    d->bone = (i16)bone;
    return d;
}

void proc_damage_hit(ProcState *s, const ProcParams *p, const Skeleton *skel, int part, int bone, vec3 dir_model, float strength) {
    if (bone < 0 || bone >= skel->count) return;
    int pi = (part >= 0 && part < 32) ? p->part_preset[part] : 1;
    const DamagePreset *pr = &p->presets[DG_CLAMP(pi, 0, p->preset_count - 1)];
    vec3 dir = v3_len2(dir_model) > 1e-8f ? v3_norm(dir_model) : v3(0, 0, 1);
    // Bend away from the shot: rotation axis perpendicular to up and the hit direction.
    vec3 axis = v3_cross(v3(0, 1, 0), dir);
    if (v3_len2(axis) < 1e-6f) axis = v3(1, 0, 0);
    axis = v3_norm(axis);
    float mag = pr->strength * DG_DEG2RAD * strength;
    int b = bone;
    for (int depth = 0; depth < pr->chain_depth && b >= 0; depth++) {
        DamageSpring *sp = spring_for(s, b);
        sp->k = pr->stiffness;
        sp->c = pr->damping;
        // Velocity impulse sized so the peak displacement is about `mag`.
        float wn = sqrtf(fmaxf(pr->stiffness, 1.0f));
        sp->v = v3_madd(sp->v, axis, mag * wn * 1.4f);
        mag *= pr->falloff;
        if (b == skel->role[BR_HIPS]) break;
        b = skel->parent[b];
    }
    float wn = sqrtf(150.0f);
    if (pr->pelvis_drop > 0) s->pelvis_drop_v += pr->pelvis_drop * strength * wn * 1.4f;
    if (pr->translate > 0) s->hips_push_v = v3_madd(s->hips_push_v, v3_flat(dir), pr->translate * strength * wn * 1.4f);
}

// "At rest" = below 0.006 deg / 0.06 deg/s (rotations) and 0.1 mm / 1 mm/s (translations).
#define REST_X 1e-4f
#define REST_V 1e-3f

static bool spring_moving(vec3 x, vec3 v) { return v3_len2(x) > REST_X * REST_X || v3_len2(v) > REST_V * REST_V; }

bool proc_damage_active(const ProcState *s) {
    for (int i = 0; i < s->spring_count; i++)
        if (spring_moving(s->springs[i].x, s->springs[i].v)) return true;
    return spring_moving(v3(s->pelvis_drop_x, 0, 0), v3(s->pelvis_drop_v, 0, 0)) || spring_moving(s->hips_push_x, s->hips_push_v);
}

static void step_vec(vec3 *x, vec3 *v, float k, float c, float dt) {
    float h = dt / SPRING_SUBSTEPS;
    for (int i = 0; i < SPRING_SUBSTEPS; i++) {
        vec3 a = v3_sub(v3_scale(*x, -k), v3_scale(*v, c));
        *v = v3_madd(*v, a, h);
        *x = v3_madd(*x, *v, h);
    }
}

void proc_damage_apply(ProcState *s, const ProcInput *in, Pose *pose, Xform *model) {
    const Skeleton *sk = in->skel;
    // Advance.
    for (int i = 0; i < s->spring_count; i++) {
        DamageSpring *d = &s->springs[i];
        step_vec(&d->x, &d->v, d->k, d->c, in->dt);
        float l = v3_len(d->x);
        if (l > MAX_ANGLE) d->x = v3_scale(d->x, MAX_ANGLE / l);   // stacking clamp
    }
    {
        vec3 x = v3(s->pelvis_drop_x, 0, 0), v = v3(s->pelvis_drop_v, 0, 0);
        step_vec(&x, &v, 150.0f, 22.0f, in->dt);
        s->pelvis_drop_x = clampf(x.x, -0.25f, 0.25f);
        s->pelvis_drop_v = v.x;
        if (!spring_moving(x, v)) s->pelvis_drop_x = s->pelvis_drop_v = 0;
        step_vec(&s->hips_push_x, &s->hips_push_v, 150.0f, 22.0f, in->dt);
        if (!spring_moving(s->hips_push_x, s->hips_push_v)) s->hips_push_x = s->hips_push_v = v3(0, 0, 0);
    }
    // Drop springs that settled.
    int m = 0;
    for (int i = 0; i < s->spring_count; i++)
        if (spring_moving(s->springs[i].x, s->springs[i].v)) s->springs[m++] = s->springs[i];
    s->spring_count = m;
    if (!in->damage) return;
    // Apply parents first (bone indices are topologically sorted).
    for (int pass = 0; pass < sk->count && s->spring_count; pass++) {
        for (int i = 0; i < s->spring_count; i++) {
            if (s->springs[i].bone != pass) continue;
            proc_rotate_bone(sk, pose, model, pass, quat_from_scaled_axis(s->springs[i].x));
        }
    }
    int hips = sk->role[BR_HIPS];
    if (hips >= 0 && (s->pelvis_drop_x != 0 || v3_len2(s->hips_push_x) > 0)) {
        Xform h = model[hips];
        h.t = v3_add(h.t, v3(s->hips_push_x.x, -fmaxf(0.0f, s->pelvis_drop_x), s->hips_push_x.z));
        pose_set_model(sk, pose, model, hips, h);
        pose_update_children(sk, pose, model, hips);
    }
}
