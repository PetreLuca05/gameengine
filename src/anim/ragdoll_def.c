// Ragdoll definition built from the shared skeleton (pure: no physics calls). Instances live
// in physics/ragdoll.c.
#include "physics/ragdoll.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>

typedef struct BodyTemplate {
    const char *name, *bone, *end, *parent;
    int part;
    float mass_frac, radius;
    float lateral;      // > 0: capsule across the body (model X) with this half length, centred on the segment
    float start, stop;  // segment sub-range (0..1 along bone -> end)
} BodyTemplate;

// Human mass distribution (fractions are normalized to total_mass afterwards).
static const BodyTemplate k_bodies[] = {
    { "pelvis", "Hips", "Spine", NULL, BP_PELVIS, 0.12f, 0.12f, 0.08f, 0.0f, 1.0f },
    { "stomach", "Spine", "Spine2", "pelvis", BP_STOMACH, 0.12f, 0.115f, 0.07f, 0.0f, 1.0f },
    { "chest", "Spine2", "Neck", "stomach", BP_CHEST, 0.18f, 0.12f, 0.10f, 0.0f, 0.7f },
    { "neck", "Neck", "Head", "chest", BP_NECK, 0.015f, 0.05f, 0, 0.0f, 1.0f },
    { "head", "Head", "Head_end", "neck", BP_HEAD, 0.07f, 0.095f, 0, 0.3f, 0.9f },
    { "upper_arm_l", "LeftArm", "LeftForeArm", "chest", BP_UPPER_ARM_L, 0.028f, 0.05f, 0, 0.0f, 1.0f },
    { "lower_arm_l", "LeftForeArm", "LeftHand", "upper_arm_l", BP_LOWER_ARM_L, 0.016f, 0.04f, 0, 0.0f, 1.0f },
    { "hand_l", "LeftHand", "LeftHandMiddle1", "lower_arm_l", BP_HAND_L, 0.006f, 0.035f, 0, 0.0f, 1.0f },
    { "upper_arm_r", "RightArm", "RightForeArm", "chest", BP_UPPER_ARM_R, 0.028f, 0.05f, 0, 0.0f, 1.0f },
    { "lower_arm_r", "RightForeArm", "RightHand", "upper_arm_r", BP_LOWER_ARM_R, 0.016f, 0.04f, 0, 0.0f, 1.0f },
    { "hand_r", "RightHand", "RightHandMiddle1", "lower_arm_r", BP_HAND_R, 0.006f, 0.035f, 0, 0.0f, 1.0f },
    { "upper_leg_l", "LeftUpLeg", "LeftLeg", "pelvis", BP_UPPER_LEG_L, 0.10f, 0.075f, 0, 0.0f, 1.0f },
    { "lower_leg_l", "LeftLeg", "LeftFoot", "upper_leg_l", BP_LOWER_LEG_L, 0.045f, 0.055f, 0, 0.0f, 1.0f },
    { "foot_l", "LeftFoot", "LeftToeBase_end", "lower_leg_l", BP_FOOT_L, 0.014f, 0.04f, 0, 0.0f, 1.0f },
    { "upper_leg_r", "RightUpLeg", "RightLeg", "pelvis", BP_UPPER_LEG_R, 0.10f, 0.075f, 0, 0.0f, 1.0f },
    { "lower_leg_r", "RightLeg", "RightFoot", "upper_leg_r", BP_LOWER_LEG_R, 0.045f, 0.055f, 0, 0.0f, 1.0f },
    { "foot_r", "RightFoot", "RightToeBase_end", "lower_leg_r", BP_FOOT_R, 0.014f, 0.04f, 0, 0.0f, 1.0f },
};

typedef struct JointTemplate { const char *child; int type; float cone_n, cone_p, tw_min, tw_max; float h_min, h_max; vec3 bend; } JointTemplate;
static const JointTemplate k_joints[] = {
    { "stomach", RJ_SWING_TWIST, 20, 20, -15, 15, 0, 0, { 0, 0, 0 } },
    { "chest", RJ_SWING_TWIST, 20, 20, -15, 15, 0, 0, { 0, 0, 0 } },
    { "neck", RJ_SWING_TWIST, 35, 35, -35, 35, 0, 0, { 0, 0, 0 } },
    { "head", RJ_SWING_TWIST, 35, 30, -40, 40, 0, 0, { 0, 0, 0 } },
    { "upper_arm_l", RJ_SWING_TWIST, 85, 70, -60, 60, 0, 0, { 0, 0, 0 } },
    { "lower_arm_l", RJ_HINGE, 0, 0, 0, 0, -3, 145, { 0, 0, -1 } },
    { "hand_l", RJ_SWING_TWIST, 45, 30, -25, 25, 0, 0, { 0, 0, 0 } },
    { "upper_arm_r", RJ_SWING_TWIST, 85, 70, -60, 60, 0, 0, { 0, 0, 0 } },
    { "lower_arm_r", RJ_HINGE, 0, 0, 0, 0, -3, 145, { 0, 0, -1 } },
    { "hand_r", RJ_SWING_TWIST, 45, 30, -25, 25, 0, 0, { 0, 0, 0 } },
    { "upper_leg_l", RJ_SWING_TWIST, 60, 40, -25, 25, 0, 0, { 0, 0, 0 } },
    { "lower_leg_l", RJ_HINGE, 0, 0, 0, 0, -3, 150, { 0, 0, 1 } },
    { "foot_l", RJ_SWING_TWIST, 25, 15, -10, 10, 0, 0, { 0, 0, 0 } },
    { "upper_leg_r", RJ_SWING_TWIST, 60, 40, -25, 25, 0, 0, { 0, 0, 0 } },
    { "lower_leg_r", RJ_HINGE, 0, 0, 0, 0, -3, 150, { 0, 0, 1 } },
    { "foot_r", RJ_SWING_TWIST, 25, 15, -10, 10, 0, 0, { 0, 0, 0 } },
};

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static bool jpair(const cJSON *o, const char *k, float *a, float *b) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 2) return false;
    *a = (float)cJSON_GetArrayItem(v, 0)->valuedouble;
    *b = (float)cJSON_GetArrayItem(v, 1)->valuedouble;
    return true;
}

static int find_body(const RagdollDef *d, const char *name) {
    for (int i = 0; i < d->body_count; i++) if (!strcmp(d->bodies[i].name, name)) return i;
    return -1;
}

static vec3 any_perp(vec3 a) {
    vec3 p = v3_cross(a, v3(0, 0, -1));
    if (v3_len2(p) < 1e-4f) p = v3_cross(a, v3(1, 0, 0));
    return v3_norm(p);
}

bool ragdoll_def_build(RagdollDef *d, const Skeleton *s, const float *radii, const cJSON *json, char *err, int err_size) {
    memset(d, 0, sizeof(*d));
    d->total_mass = 80.0f;
    d->linear_damping = 0.05f;
    d->angular_damping = 0.6f;
    d->motor_time = 0.3f;
    d->motor_strength = 12.0f;
    d->sleep_time = 1.5f;
    d->remove_time = 45.0f;
    d->max_ragdolls = 24;
    d->friction = 0.8f;
    const cJSON *jb = json ? cJSON_GetObjectItemCaseSensitive(json, "bodies") : NULL;
    const cJSON *jj = json ? cJSON_GetObjectItemCaseSensitive(json, "joints") : NULL;
    if (json) {
        d->total_mass = jf(json, "total_mass", d->total_mass);
        jpair(json, "damping", &d->linear_damping, &d->angular_damping);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(json, "motor");
        if (m) { d->motor_time = jf(m, "time", d->motor_time); d->motor_strength = jf(m, "strength", d->motor_strength); }
        d->sleep_time = jf(json, "sleep_time", d->sleep_time);
        d->remove_time = jf(json, "remove_time", d->remove_time);
        d->max_ragdolls = (int)jf(json, "max_ragdolls", (float)d->max_ragdolls);
        d->friction = jf(json, "friction", d->friction);
    }
    float frac_sum = 0;
    float fracs[RAG_MAX_BODIES];
    for (int i = 0; i < ARRAY_COUNT(k_bodies) && d->body_count < RAG_MAX_BODIES; i++) {
        const BodyTemplate *t = &k_bodies[i];
        int bone = skel_find_suffix(s, t->bone);
        int end = skel_find_suffix(s, t->end);
        if (bone < 0) { if (err) snprintf(err, (size_t)err_size, "ragdoll: bone '%s' missing", t->bone); return false; }
        RagBodyDef *b = &d->bodies[d->body_count];
        memset(b, 0, sizeof(*b));
        dg_strcpy(b->name, sizeof(b->name), t->name);
        b->bone = (i16)bone;
        b->end_bone = (i16)end;
        b->parent = (i16)(t->parent ? find_body(d, t->parent) : -1);
        b->part = (u8)t->part;
        b->shape = RAG_CAPSULE;
        b->friction = d->friction;
        float r = t->radius;
        if (radii && radii[bone] > 0.005f) r = clampf(radii[bone], t->radius * 0.6f, t->radius * 1.4f);
        float frac = t->mass_frac;
        const cJSON *o = jb ? cJSON_GetObjectItemCaseSensitive(jb, t->name) : NULL;
        if (o) {
            r = jf(o, "radius", r);
            frac = jf(o, "mass_frac", frac);
            b->friction = jf(o, "friction", b->friction);
            b->restitution = jf(o, "restitution", b->restitution);
            const cJSON *sh = cJSON_GetObjectItemCaseSensitive(o, "shape");
            if (cJSON_IsString(sh)) b->shape = !strcmp(sh->valuestring, "box") ? RAG_BOX : (!strcmp(sh->valuestring, "sphere") ? RAG_SPHERE : RAG_CAPSULE);
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(o, "half");
            if (cJSON_IsArray(h) && cJSON_GetArraySize(h) >= 3)
                b->half = v3((float)cJSON_GetArrayItem(h, 0)->valuedouble, (float)cJSON_GetArrayItem(h, 1)->valuedouble, (float)cJSON_GetArrayItem(h, 2)->valuedouble);
        }
        vec3 a = s->bind_model[bone].t;
        vec3 e = end >= 0 ? s->bind_model[end].t : xf_point(s->bind_model[bone], v3(0, s->length[bone], 0));
        if (t->part == BP_HAND_L || t->part == BP_HAND_R) e = v3_add(a, v3_scale(v3_sub(e, a), 1.6f));
        vec3 a0 = a;
        a = v3_lerp(a0, e, t->start);
        e = v3_lerp(a0, e, t->stop);
        float len = v3_dist(a, e);
        if (len < 0.02f) { e = v3_add(a, v3(0, 0.05f, 0)); len = 0.05f; }
        vec3 dir = v3_scale(v3_sub(e, a), 1.0f / len);
        b->radius = r;
        b->half_len = fmaxf(0.01f, len * 0.5f - r * 0.5f);
        if (t->lateral > 0) {
            // Across the body: the torso is wider than it is tall.
            b->half_len = t->lateral;
            dir = v3(1, 0, 0);
        }
        if (b->shape == RAG_BOX && v3_len2(b->half) < 1e-6f) b->half = v3(r, len * 0.5f, r * 0.7f);
        b->bind = xf_make(v3_scale(v3_add(a, e), 0.5f), quat_from_to(v3(0, 1, 0), dir));
        b->local = xf_mul(xf_inverse(s->bind_model[bone]), b->bind);
        fracs[d->body_count] = frac;
        frac_sum += frac;
        d->body_count++;
    }
    for (int i = 0; i < d->body_count; i++) d->bodies[i].mass = d->total_mass * fracs[i] / fmaxf(frac_sum, 1e-3f);
    // Joints (limits relative to the bind pose: axes are derived from it in each body's frame).
    for (int i = 0; i < ARRAY_COUNT(k_joints); i++) {
        const JointTemplate *t = &k_joints[i];
        int c = find_body(d, t->child);
        if (c < 0 || d->bodies[c].parent < 0) continue;
        int p = d->bodies[c].parent;
        RagJointDef *j = &d->joints[d->joint_count++];
        memset(j, 0, sizeof(*j));
        j->parent = (i16)p;
        j->child = (i16)c;
        j->type = (u8)t->type;
        float cn = t->cone_n, cp = t->cone_p, tmin = t->tw_min, tmax = t->tw_max, hmin = t->h_min, hmax = t->h_max;
        const cJSON *o = jj ? cJSON_GetObjectItemCaseSensitive(jj, t->child) : NULL;
        if (o) { jpair(o, "cone", &cn, &cp); jpair(o, "twist", &tmin, &tmax); jpair(o, "hinge", &hmin, &hmax); }
        j->cone_normal = cn * DG_DEG2RAD;
        j->cone_plane = cp * DG_DEG2RAD;
        j->twist_min = tmin * DG_DEG2RAD;
        j->twist_max = tmax * DG_DEG2RAD;
        j->hinge_min = hmin * DG_DEG2RAD;
        j->hinge_max = hmax * DG_DEG2RAD;
        const RagBodyDef *bp = &d->bodies[p], *bc = &d->bodies[c];
        vec3 point = s->bind_model[bc->bone].t;
        vec3 cdir = quat_rotate(bc->bind.r, v3(0, 1, 0));
        vec3 axis, plane;
        if (t->type == RJ_HINGE) {
            axis = v3_cross(cdir, t->bend);
            if (v3_len2(axis) < 1e-6f) axis = any_perp(cdir);
            axis = v3_norm(axis);
            plane = cdir;      // normal axis: the child's direction (angle 0 at bind)
        } else {
            axis = cdir;
            plane = any_perp(cdir);
        }
        Xform ip = xf_inverse(bp->bind), ic = xf_inverse(bc->bind);
        j->pos_p = xf_point(ip, point);
        j->pos_c = xf_point(ic, point);
        j->axis_p = quat_rotate(ip.r, axis);
        j->axis_c = quat_rotate(ic.r, axis);
        j->plane_p = quat_rotate(ip.r, plane);
        j->plane_c = quat_rotate(ic.r, plane);
    }
    u32 h = ANIM_HASH_SEED;
    h = anim_hash(d->bodies, sizeof(RagBodyDef) * (size_t)d->body_count, h);
    h = anim_hash(d->joints, sizeof(RagJointDef) * (size_t)d->joint_count, h);
    d->hash = h;
    return true;
}

void ragdoll_body_segment(const RagBodyDef *b, Xform bone_world, float scale, vec3 *a, vec3 *bpt, float *radius) {
    Xform bw = xf_mul(bone_world, xf_make(v3_scale(b->local.t, scale), b->local.r));
    vec3 ax = quat_rotate(bw.r, v3(0, 1, 0));
    float hl = (b->shape == RAG_BOX ? b->half.y : b->half_len) * scale;
    *a = v3_madd(bw.t, ax, -hl);
    *bpt = v3_madd(bw.t, ax, hl);
    *radius = (b->shape == RAG_BOX ? fmaxf(b->half.x, b->half.z) : b->radius) * scale;
}

