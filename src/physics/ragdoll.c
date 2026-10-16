#include "ragdoll.h"
#include "core/log.h"
#include "render/debug_draw.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// ------------------------------------------------------------------ instances

static int find_body(const RagdollDef *d, const char *name) {
    for (int i = 0; i < d->body_count; i++) if (!strcmp(d->bodies[i].name, name)) return i;
    return -1;
}


static Xform body_world_from_bone(const RagBodyDef *b, Xform bone_world, float scale) {
    return xf_mul(bone_world, xf_make(v3_scale(b->local.t, scale), b->local.r));
}

bool ragdoll_create(Ragdoll *r, PhysicsWorld *w, const RagdollDef *d, const Skeleton *s, const RagdollSpawn *sp) {
    memset(r, 0, sizeof(*r));
    r->scale = sp->scale > 0 ? sp->scale : 1.0f;
    r->group = phys_group_filter_create(d->body_count);
    for (int k = 0; k < d->joint_count; k++) phys_group_filter_disable(r->group, d->joints[k].parent, d->joints[k].child);
    // Torso pieces also ignore the upper legs / arms of their neighbours.
    int pelvis = find_body(d, "pelvis"), stomach = find_body(d, "stomach"), chest = find_body(d, "chest");
    for (int i = 0; i < d->body_count; i++) {
        int p = d->bodies[i].parent;
        if (p >= 0 && d->bodies[p].parent >= 0) phys_group_filter_disable(r->group, i, d->bodies[p].parent);
        if (stomach >= 0 && (d->bodies[i].part == BP_UPPER_LEG_L || d->bodies[i].part == BP_UPPER_LEG_R)) phys_group_filter_disable(r->group, i, stomach);
        if (stomach >= 0 && (d->bodies[i].part == BP_UPPER_ARM_L || d->bodies[i].part == BP_UPPER_ARM_R)) phys_group_filter_disable(r->group, i, stomach);
    }
    if (pelvis >= 0 && chest >= 0) phys_group_filter_disable(r->group, pelvis, chest);
    Xform cur[RAG_MAX_BODIES];
    for (int i = 0; i < d->body_count; i++) cur[i] = body_world_from_bone(&d->bodies[i], sp->bone_world[d->bodies[i].bone], r->scale);
    // Push-out: keep every body on the visible side of static geometry relative to the pelvis.
    if (pelvis >= 0) {
        vec3 c0 = cur[pelvis].t;
        for (int i = 0; i < d->body_count; i++) {
            if (i == pelvis) continue;
            vec3 dv = v3_sub(cur[i].t, c0);
            float len = v3_len(dv);
            if (len < 1e-3f) continue;
            RayHit rh;
            if (phys_raycast(w, c0, dv, PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) {
                float back = d->bodies[i].radius * r->scale + 0.01f;
                cur[i].t = v3_madd(c0, v3_scale(dv, 1.0f / len), fmaxf(0.0f, rh.distance - back));
            }
        }
    }
    r->body_count = d->body_count;
    for (int i = 0; i < d->body_count; i++) {
        const RagBodyDef *b = &d->bodies[i];
        BodyDesc bd;
        memset(&bd, 0, sizeof(bd));
        if (b->shape == RAG_BOX) { bd.shape.kind = SHAPE_BOX; bd.shape.half = v3_scale(b->half, r->scale); }
        else if (b->shape == RAG_SPHERE) { bd.shape.kind = SHAPE_SPHERE; bd.shape.radius = b->radius * r->scale; }
        else { bd.shape.kind = SHAPE_CAPSULE; bd.shape.radius = b->radius * r->scale; bd.shape.half_height = b->half_len * r->scale; }
        bd.pos = cur[i].t;
        bd.rot = cur[i].r;
        bd.motion = PM_DYNAMIC;
        bd.layer = sp->solid ? PL_RAGDOLL_SOLID : PL_RAGDOLL;
        bd.mass = b->mass * r->scale * r->scale * r->scale;
        bd.friction = b->friction;
        bd.restitution = b->restitution;
        bd.linear_damping = d->linear_damping;
        bd.angular_damping = d->angular_damping;
        bd.group_filter = r->group;
        bd.group_id = sp->group_id;
        bd.sub_group_id = (u32)i;
        r->body[i] = phys_add_body(w, &bd);
        // Velocity from the last two animation ticks.
        if (sp->prev_world && sp->dt > 0) {
            Xform prev = body_world_from_bone(b, sp->prev_world[b->bone], r->scale);
            vec3 lin = v3_scale(v3_sub(cur[i].t, prev.t), 1.0f / sp->dt);
            vec3 ang = v3_scale(quat_to_scaled_axis(quat_mul(cur[i].r, quat_conj(prev.r))), 1.0f / sp->dt);
            if (v3_len(lin) > 12.0f) lin = v3_scale(v3_norm(lin), 12.0f);
            if (v3_len(ang) > 20.0f) ang = v3_scale(v3_norm(ang), 20.0f);
            phys_set_velocity(w, r->body[i], lin, ang);
        }
    }
    r->joint_count = d->joint_count;
    for (int k = 0; k < d->joint_count; k++) {
        const RagJointDef *j = &d->joints[k];
        vec3 pp = v3_scale(j->pos_p, r->scale), pc = v3_scale(j->pos_c, r->scale);
        if (j->type == RJ_HINGE)
            r->joint[k] = phys_add_hinge(w, r->body[j->parent], r->body[j->child], pp, j->axis_p, j->plane_p, pc, j->axis_c, j->plane_c,
                                         j->hinge_min, j->hinge_max, 0.5f);
        else
            r->joint[k] = phys_add_swing_twist(w, r->body[j->parent], r->body[j->child], pp, j->axis_p, j->plane_p, pc, j->axis_c, j->plane_c,
                                               j->cone_normal, j->cone_plane, j->twist_min, j->twist_max, 0.5f);
        r->target_rel[j->child] = quat_mul(quat_conj(cur[j->parent].r), cur[j->child].r);
    }
    // Killing hit impulse.
    int hb = pelvis;
    for (int i = 0; i < d->body_count; i++) if (d->bodies[i].part == sp->hit_part) hb = i;
    if (hb >= 0 && v3_len2(sp->impulse) > 0) phys_add_impulse(w, r->body[hb], sp->impulse, cur[hb].t);
    if (sp->pose) pose_copy(&r->death_pose, sp->pose);
    r->root = sp->root;
    r->remove_time = sp->remove_time;
    r->active = true;
    return true;
}

void ragdoll_destroy(Ragdoll *r, PhysicsWorld *w) {
    if (!r->active) return;
    if (!r->frozen) {
        for (int k = 0; k < r->joint_count; k++) phys_remove_constraint(w, r->joint[k]);
        for (int i = 0; i < r->body_count; i++) phys_remove_body(w, r->body[i]);
    }
    phys_group_filter_destroy(r->group);
    r->group = NULL;
    r->active = false;
}

static void freeze(Ragdoll *r, PhysicsWorld *w, const RagdollDef *d) {
    for (int i = 0; i < r->body_count; i++) phys_get_transform(w, r->body[i], &r->frozen_world[i].t, &r->frozen_world[i].r);
    for (int i = 0; i < r->body_count; i++) r->frozen_world[i].s = v3(1, 1, 1);
    for (int k = 0; k < d->joint_count; k++) phys_remove_constraint(w, r->joint[k]);
    for (int i = 0; i < r->body_count; i++) phys_remove_body(w, r->body[i]);
    r->frozen = true;
}

bool ragdoll_step(Ragdoll *r, PhysicsWorld *w, const RagdollDef *d, float dt) {
    if (!r->active) return false;
    r->age += dt;
    if (r->age > (r->remove_time > 0 ? r->remove_time : d->remove_time)) return false;
    if (r->frozen) return true;
    // Muscle tone: PD drive of each child towards its death-pose orientation, fading out.
    if (r->age < d->motor_time && d->motor_strength > 0) {
        float k = d->motor_strength * (1.0f - r->age / d->motor_time);
        for (int j = 0; j < d->joint_count; j++) {
            int p = d->joints[j].parent, c = d->joints[j].child;
            quat qp, qc;
            vec3 tp, tc;
            phys_get_transform(w, r->body[p], &tp, &qp);
            phys_get_transform(w, r->body[c], &tc, &qc);
            quat target = quat_mul(qp, r->target_rel[c]);
            vec3 err = quat_to_scaled_axis(quat_mul(target, quat_conj(qc)));
            vec3 ang = v3_add(phys_get_angular_velocity(w, r->body[p]), v3_scale(err, k));
            vec3 lin = phys_get_velocity(w, r->body[c]);
            phys_set_velocity(w, r->body[c], lin, v3_lerp(phys_get_angular_velocity(w, r->body[c]), ang, 0.5f));
        }
    }
    bool any_active = false;
    for (int i = 0; i < r->body_count && !any_active; i++) any_active = phys_is_active(w, r->body[i]);
    if (!any_active) r->rest_time += dt;
    else r->rest_time = 0;
    if (r->rest_time > d->sleep_time) { freeze(r, w, d); r->settled = true; }
    return true;
}

void ragdoll_read_pose(const Ragdoll *r, PhysicsWorld *w, const RagdollDef *d, const Skeleton *s, Xform *out) {
    int driver[ANIM_MAX_BONES];
    for (int i = 0; i < s->count; i++) driver[i] = -1;
    for (int b = 0; b < d->body_count; b++) driver[d->bodies[b].bone] = b;
    for (int i = 0; i < s->count; i++) {
        int b = driver[i];
        if (b >= 0) {
            Xform bw;
            if (r->frozen) bw = r->frozen_world[b];
            else { phys_get_transform(w, r->body[b], &bw.t, &bw.r); bw.s = v3(1, 1, 1); }
            Xform loc = xf_make(v3_scale(d->bodies[b].local.t, r->scale), d->bodies[b].local.r);
            out[i] = xf_mul(bw, xf_inverse(loc));
            out[i].s = v3(r->scale, r->scale, r->scale);
            continue;
        }
        int p = s->parent[i];
        Xform loc = r->death_pose.count ? r->death_pose.local[i] : s->bind[i];
        out[i] = p >= 0 ? xf_mul(out[p], loc) : xf_mul(r->root, loc);
    }
}

void ragdoll_push(Ragdoll *r, PhysicsWorld *w, vec3 impulse) {
    if (!r->active || r->frozen) return;
    for (int i = 0; i < r->body_count; i++) {
        vec3 p;
        quat q;
        phys_get_transform(w, r->body[i], &p, &q);
        phys_add_impulse(w, r->body[i], v3_scale(impulse, 1.0f / (float)r->body_count), p);
    }
    r->rest_time = 0;
}

void ragdoll_settled_pose(const Skeleton *s, const Pose *base, Xform root, u8 seed, Xform *out) {
    // Lying on the back (even seed) or the front (odd), slightly rolled, hips near the ground.
    Pose p;
    if (base) pose_copy(&p, base);
    else pose_bind(s, &p);
    Xform model[ANIM_MAX_BONES];
    pose_to_model(s, &p, model);
    int hips = s->role[BR_HIPS];
    float hy = hips >= 0 ? model[hips].t.y : 1.0f;
    float side = (seed & 1) ? 1.0f : -1.0f;
    float roll = ((float)((seed >> 1) & 7) - 3.5f) * 0.05f;
    quat lie = quat_mul(quat_axis_angle(v3(0, 0, 1), roll), quat_axis_angle(v3(1, 0, 0), side * DG_PI * 0.5f));
    Xform shift = xf_make(v3(0, 0.13f, 0), lie);
    Xform pre = xf_make(v3(0, -hy, 0), quat_identity());
    for (int i = 0; i < s->count; i++) out[i] = xf_mul(root, xf_mul(shift, xf_mul(pre, model[i])));
}

void ragdoll_debug_draw(const Ragdoll *r, PhysicsWorld *w, const RagdollDef *d) {
    if (!r->active) return;
    for (int i = 0; i < r->body_count; i++) {
        Xform bw;
        if (r->frozen) bw = r->frozen_world[i];
        else phys_get_transform(w, r->body[i], &bw.t, &bw.r);
        const RagBodyDef *b = &d->bodies[i];
        vec3 ax = quat_rotate(bw.r, v3(0, 1, 0));
        float hl = b->half_len * r->scale;
        dbg_capsule_axis(v3_madd(bw.t, ax, -hl), v3_madd(bw.t, ax, hl), b->radius * r->scale, r->frozen ? DBG_GREY : DBG_ORANGE, 0);
        char label[32];
        snprintf(label, sizeof(label), "%.1fkg", b->mass);
        dbg_text(bw.t, label, DBG_WHITE, 0.03f, 0);
    }
    if (r->frozen) return;
    for (int k = 0; k < d->joint_count; k++) {
        const RagJointDef *j = &d->joints[k];
        Xform pw;
        phys_get_transform(w, r->body[j->parent], &pw.t, &pw.r);
        pw.s = v3(1, 1, 1);
        vec3 pt = xf_point(pw, v3_scale(j->pos_p, r->scale));
        vec3 ax = quat_rotate(pw.r, j->axis_p);
        if (j->type == RJ_HINGE) {
            dbg_line(v3_madd(pt, ax, -0.06f), v3_madd(pt, ax, 0.06f), DBG_CYAN, 0, false);
        } else {
            float cr = tanf(fminf(j->cone_normal, 1.4f)) * 0.08f;
            dbg_circle(v3_madd(pt, ax, 0.08f), ax, cr, DBG_YELLOW, 0);
            dbg_line(pt, v3_madd(pt, ax, 0.08f), DBG_YELLOW, 0, false);
        }
    }
}
