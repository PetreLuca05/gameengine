// Procedural swimming (docs/WATER.md "Swim animation"): no clips. The body tips from upright
// (treading water) towards prone with the swim speed; under water it follows the dive direction.
// Arms: front crawl at the surface (hand targets on an ellipse around the shoulder, recovery arc held
// above the waterline, the pull below it: the hands meet the surface), breaststroke under water,
// sculling when idle. Legs: flutter kick, frog kick under water, a slow bicycle kick when treading.
// Everything scales with a smoothed weight, so entering / leaving the water blends.
#include "procedural_internal.h"
#include <math.h>

static float approach_f(float cur, float target, float step) { return cur < target ? fminf(cur + step, target) : fmaxf(cur - step, target); }

void proc_swim(ProcState *st, const ProcInput *in, Pose *pose, Xform *model) {
    st->swim_w = approach_f(st->swim_w, in->swim ? 1.0f : 0.0f, in->dt / 0.35f);
    float w = st->swim_w;
    if (w <= 0.001f) return;
    const Skeleton *s = in->skel;
    Xform inv = xf_inverse(in->root);
    bool under = in->swim == 2;
    float sp = clampf(in->swim_speed / 1.7f, 0.0f, 1.5f);
    float moving = smoothstepf(0.15f, 0.5f, sp);
    // Phase: the simulation's stroke phase when known, else a local one.
    if (in->swim_phase_valid) st->swim_phase = in->swim_phase;
    else st->swim_phase = fmodf(st->swim_phase + in->dt * (1.6f + in->swim_speed * 1.9f), 2.0f * DG_PI * 1024.0f);
    float ph = st->swim_phase;

    // ---- body tilt (degrees forward from upright), smoothed
    float tilt;
    if (under) tilt = moving > 0.0f ? clampf(85.0f - in->aim_pitch * DG_RAD2DEG, 10.0f, 165.0f) * moving + 15.0f * (1.0f - moving) : 15.0f;
    else tilt = lerpf(12.0f, 72.0f, moving);
    st->swim_tilt = approach_f(st->swim_tilt, tilt, in->dt * 140.0f);
    float t = st->swim_tilt * w;

    int hips = s->role[BR_HIPS];
    if (hips < 0) return;
    // At the surface the pelvis rides just under the water (the head above it).
    if (!under && in->water_y > -1e8f) {
        Xform n = model[hips];
        float want = in->water_y - 0.22f - in->root.t.y;
        n.t.y = lerpf(n.t.y, want, w * smoothstepf(10.0f, 40.0f, st->swim_tilt));
        pose_set_model(s, pose, model, hips, n);
        pose_update_children(s, pose, model, hips);
    }
    proc_rotate_bone(s, pose, model, hips, quat_axis_angle(v3(1, 0, 0), -t * DG_DEG2RAD));
    // Head up: the face out of the water at the surface, along the travel under it.
    int neck = s->role[BR_NECK], head = s->role[BR_HEAD];
    float lift = under ? 0.3f : 0.55f;
    if (neck >= 0) proc_rotate_bone(s, pose, model, neck, quat_axis_angle(v3(1, 0, 0), t * lift * 0.6f * DG_DEG2RAD));
    if (head >= 0) proc_rotate_bone(s, pose, model, head, quat_axis_angle(v3(1, 0, 0), t * lift * 0.4f * DG_DEG2RAD));

    // Body frame (model space): along the spine, out of the chest, lateral.
    float tr = st->swim_tilt * DG_DEG2RAD;
    vec3 up_b = v3(0.0f, cosf(tr), -sinf(tr));
    vec3 fwd_b = v3(0.0f, -sinf(tr), -cosf(tr));
    float water_m = in->water_y > -1e8f ? in->water_y - in->root.t.y : 1e9f;   // the surface in model space

    // ---- arms
    static const BoneRole arm[2][3] = { { BR_L_ARM, BR_L_FOREARM, BR_L_HAND }, { BR_R_ARM, BR_R_FOREARM, BR_R_HAND } };
    for (int h = 0; h < 2; h++) {
        int a = s->role[arm[h][0]], b = s->role[arm[h][1]], c = s->role[arm[h][2]];
        if (a < 0 || b < 0 || c < 0 || (in->swim_hold_mask & (1u << h))) continue;
        float sgn = h ? 1.0f : -1.0f;   // +X right
        vec3 lat = v3(sgn, 0, 0);
        float L = v3_dist(s->bind_model[a].t, s->bind_model[b].t) + v3_dist(s->bind_model[b].t, s->bind_model[c].t);
        vec3 sh = model[a].t;
        vec3 target;
        float aw = w;
        if (!under && moving > 0.0f) {
            // Front crawl: the arms half a cycle apart.
            float th = ph + (h ? DG_PI : 0.0f);
            float cth = cosf(th), sth = sinf(th);
            target = v3_add(sh, v3_add(v3_scale(up_b, L * 0.82f * cth), v3_scale(fwd_b, L * 0.55f * sth)));
            target = v3_add(target, v3_scale(lat, 0.1f + 0.12f * fmaxf(0.0f, -sth)));
            // Recovery over the water, the pull through it.
            if (sth < 0.0f) target.y = fmaxf(target.y, water_m + 0.06f);
            else target.y = fminf(target.y, water_m - 0.05f);
            aw *= moving;
            vec3 tread = v3_add(sh, v3_add(v3_scale(lat, 0.32f), v3_sub(v3_scale(fwd_b, 0.3f), v3_scale(up_b, 0.25f))));
            target = v3_lerp(tread, target, moving);
        } else if (under && moving > 0.0f) {
            // Breaststroke: reach forward together, sweep out and back, hands together to the chest.
            float th = fmodf(ph * 0.6f, 2.0f * DG_PI);
            float sweep = th < DG_PI ? sinf(th) : 0.0f;
            float reach = cosf(th < DG_PI ? th * 0.5f : DG_PI * 0.5f + (th - DG_PI) * 0.5f);
            target = v3_add(sh, v3_add(v3_scale(up_b, L * 0.8f * reach), v3_scale(lat, 0.12f + L * 0.55f * sweep)));
            target = v3_add(target, v3_scale(fwd_b, 0.18f));
        } else {
            // Sculling / treading: small circles out to the side, below the surface.
            float th = ph * 1.4f + (h ? 0.0f : DG_PI * 0.5f);
            target = v3_add(sh, v3_add(v3_scale(lat, 0.34f + 0.07f * sinf(th)), v3_add(v3_scale(fwd_b, 0.28f + 0.08f * cosf(th)), v3_scale(up_b, -0.28f))));
            if (!under) target.y = fminf(target.y, water_m - 0.08f);
        }
        vec3 pole = v3_add(v3_scale(fwd_b, -1.0f), v3_scale(lat, 0.6f));
        anim_ik_two_bone_pose(s, pose, model, a, b, c, target, NULL, pole, aw);
    }

    // ---- legs
    static const BoneRole leg[2][2] = { { BR_L_UPLEG, BR_L_LEG }, { BR_R_UPLEG, BR_R_LEG } };
    for (int k = 0; k < 2; k++) {
        int a = s->role[leg[k][0]], b = s->role[leg[k][1]];
        if (a < 0 || b < 0) continue;
        float hip, knee;
        if (under && moving > 0.0f) {
            float th = fmodf(ph * 0.6f, 2.0f * DG_PI);
            float draw = th < DG_PI ? 0.0f : sinf(th - DG_PI);   // the frog kick draws the knees in, then snaps back
            hip = 35.0f * draw;
            knee = -95.0f * draw;
        } else if (moving > 0.0f) {
            float s2 = sinf(ph * 2.0f + (k ? DG_PI : 0.0f));
            hip = 16.0f * s2 * moving + 22.0f * (1.0f - moving) * (0.5f + 0.5f * s2);
            knee = -(10.0f + 12.0f * (0.5f + 0.5f * s2)) * moving - 45.0f * (1.0f - moving) * (0.5f + 0.5f * s2);
        } else {
            float s2 = 0.5f + 0.5f * sinf(ph + (k ? DG_PI : 0.0f));
            hip = 28.0f * s2;
            knee = -55.0f * s2;
        }
        proc_rotate_bone(s, pose, model, a, quat_axis_angle(v3(1, 0, 0), hip * w * DG_DEG2RAD));
        proc_rotate_bone(s, pose, model, b, quat_axis_angle(v3(1, 0, 0), knee * w * DG_DEG2RAD));
    }
    (void)inv;
}
