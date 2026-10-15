// Foot IK + ground lean (cosmetic: never moves the capsule).
//   pelvis pass: per-foot downward casts against the local physics world; the pelvis lowers
//                to the lower foot so both can reach (smoothed);
//   leg pass:    two-bone leg IK onto the ground, foot aligned to the surface normal, foot
//                locking while planted (hides slide during turn in place).
#include "procedural_internal.h"
#include <math.h>

static int leg_bones[2][3] = { { BR_L_UPLEG, BR_L_LEG, BR_L_FOOT }, { BR_R_UPLEG, BR_R_LEG, BR_R_FOOT } };

static bool ground_at(const ProcInput *in, vec3 world_xz, float *y, vec3 *n) {
    if (!in->raycast) return false;
    vec3 from = v3(world_xz.x, in->root.t.y + in->params->foot_ray_up, world_xz.z);
    float len = in->params->foot_ray_up + in->params->foot_ray_down;
    float frac;
    vec3 p;
    if (!in->raycast(in->ray_ctx, from, v3(0, -len, 0), &frac, &p, n)) return false;
    *y = p.y;
    return true;
}

void proc_foot_pelvis(ProcState *st, const ProcInput *in, Pose *pose, Xform *model, float off[2], vec3 anim_foot[2]) {
    const Skeleton *s = in->skel;
    int hips = s->role[BR_HIPS];
    float target = 0;
    for (int f = 0; f < 2; f++) {
        int fb = s->role[leg_bones[f][2]];
        off[f] = 0;
        if (fb < 0) continue;
        anim_foot[f] = model[fb].t;
        FootState *fs = &st->foot[f];
        float gy;
        vec3 gn;
        vec3 w = xf_point(in->root, model[fb].t);
        if (in->grounded && ground_at(in, w, &gy, &gn)) {
            fs->ground_y = gy;
            fs->ground_n = gn;
        } else {
            fs->ground_y = in->root.t.y;
            fs->ground_n = v3(0, 1, 0);
        }
        float o = clampf(fs->ground_y - in->root.t.y, -in->params->pelvis_max_drop, 0.45f);
        float k = 1.0f - expf(-in->dt * in->params->foot_smooth);
        fs->prev_offset = lerpf(fs->prev_offset, in->grounded ? o : 0.0f, k);
        off[f] = fs->prev_offset;
        if (off[f] < target) target = off[f];
    }
    float k = 1.0f - expf(-in->dt * in->params->foot_smooth);
    float want = in->grounded ? target - in->crouch_drop : 0.0f;
    st->pelvis_offset = lerpf(st->pelvis_offset, want, k * 0.6f);
    if (hips >= 0 && st->pelvis_offset < 0) {
        Xform h = model[hips];
        h.t.y += st->pelvis_offset;
        pose_set_model(s, pose, model, hips, h);
        pose_update_children(s, pose, model, hips);
    }
}

void proc_lean(ProcState *st, const ProcInput *in, Pose *pose, Xform *model) {
    const Skeleton *s = in->skel;
    int hips = s->role[BR_HIPS];
    if (hips < 0 || in->dt <= 0) return;
    // Acceleration lean (model space) + slope lean from the average foot normal.
    quat inv_yaw = quat_conj(in->root.r);
    vec3 acc = quat_rotate(inv_yaw, v3_scale(v3_sub(in->velocity, st->prev_vel), 1.0f / in->dt));
    vec3 n = v3_norm(v3_add(st->foot[0].ground_n, st->foot[1].ground_n));
    if (v3_len2(n) < 0.5f) n = v3(0, 1, 0);
    vec3 nm = quat_rotate(inv_yaw, n);
    float pitch_t = clampf(-acc.z * in->params->accel_lean, -0.2f, 0.2f) + clampf(-nm.z * in->params->slope_lean, -0.15f, 0.15f);
    float roll_t = clampf(acc.x * in->params->accel_lean, -0.15f, 0.15f);
    if (!in->grounded) { pitch_t = 0; roll_t = 0; }
    float k = 1.0f - expf(-in->dt * 6.0f);
    st->lean.x = lerpf(st->lean.x, pitch_t, k);
    st->lean.z = lerpf(st->lean.z, roll_t, k);
    int spine = s->role[BR_SPINE];
    quat q = quat_mul(quat_axis_angle(v3(1, 0, 0), st->lean.x), quat_axis_angle(v3(0, 0, 1), st->lean.z));
    proc_rotate_bone(s, pose, model, spine >= 0 ? spine : hips, q);
}

void proc_foot_legs(ProcState *st, const ProcInput *in, Pose *pose, Xform *model, const float off[2], const vec3 anim_foot[2]) {
    const Skeleton *s = in->skel;
    if (!in->grounded) { st->foot[0].locked = st->foot[1].locked = false; return; }
    bool planted = !in->moving;
    Xform inv_root = xf_inverse(in->root);
    for (int f = 0; f < 2; f++) {
        int a = s->role[leg_bones[f][0]], b = s->role[leg_bones[f][1]], c = s->role[leg_bones[f][2]];
        if (a < 0 || b < 0 || c < 0) continue;
        FootState *fs = &st->foot[f];
        vec3 target = anim_foot[f];
        target.y += off[f];
        // Foot locking while planted (turn in place): keep the world position until it drifts.
        vec3 tw = xf_point(in->root, target);
        if (planted) {
            if (!fs->locked) { fs->locked = true; fs->lock_pos = tw; }
            vec3 d = v3_sub(tw, fs->lock_pos);
            d.y = 0;
            if (v3_len(d) > in->params->foot_lock_release) fs->lock_pos = tw;   // step
            vec3 lp = xf_point(inv_root, fs->lock_pos);
            target.x = lp.x;
            target.z = lp.z;
        } else {
            fs->locked = false;
        }
        // Pole: keep the knee where the animation put it.
        vec3 mid = v3_scale(v3_add(model[a].t, model[c].t), 0.5f);
        vec3 pole = v3_sub(model[b].t, mid);
        if (v3_len2(pole) < 1e-6f) pole = v3(0, 0, -1);
        quat keep = model[c].r;
        anim_ik_two_bone_pose(s, pose, model, a, b, c, target, &keep, v3_norm(pole), 1.0f);
        // Align the foot to the ground normal (model space).
        vec3 n = quat_rotate(quat_conj(in->root.r), fs->ground_n);
        quat al = quat_from_to(v3(0, 1, 0), v3_norm(n));
        al = quat_nlerp(quat_identity(), al, in->params->foot_align);
        if (quat_angle_between(al, quat_identity()) < 0.6f) proc_rotate_bone(s, pose, model, c, al);
    }
}
