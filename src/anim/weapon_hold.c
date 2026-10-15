// Procedural weapon holding: hold style -> desired item transform -> right-arm IK -> item
// finalized from the solved right hand -> left hand IK onto grip_l (position + rotation) ->
// finger curl. Hands stay glued to the item by construction:
//   * the item is parented to the right hand (its transform is derived from the solved hand),
//   * the left-hand target is computed from that final item transform, this tick,
//   * the left arm never stretches: if grip_l is out of reach the item / right arm move towards
//     the body (up to max_shift) until it reaches, and the style is reported once.
#include "procedural_internal.h"
#include "core/log.h"
#include <math.h>

static HoldStyle blend_style(const HoldStyle *a, const HoldStyle *b, float t) {
    if (t >= 1.0f || a == b) return *b;
    HoldStyle h = *b;
    h.hip_offset = v3_lerp(a->hip_offset, b->hip_offset, t);
    h.hip_rot = v3_lerp(a->hip_rot, b->hip_rot, t);
    h.ads_distance = lerpf(a->ads_distance, b->ads_distance, t);
    h.sprint_offset = v3_lerp(a->sprint_offset, b->sprint_offset, t);
    h.sprint_rot = v3_lerp(a->sprint_rot, b->sprint_rot, t);
    h.crouch_offset = v3_lerp(a->crouch_offset, b->crouch_offset, t);
    h.elbow_r = v3_lerp(a->elbow_r, b->elbow_r, t);
    h.elbow_l = v3_lerp(a->elbow_l, b->elbow_l, t);
    h.shoulder_r = v3_lerp(a->shoulder_r, b->shoulder_r, t);
    h.shoulder_l = v3_lerp(a->shoulder_l, b->shoulder_l, t);
    h.chest_offset = v3_lerp(a->chest_offset, b->chest_offset, t);
    for (int i = 0; i < 5; i++) { h.curl_r[i] = lerpf(a->curl_r[i], b->curl_r[i], t); h.curl_l[i] = lerpf(a->curl_l[i], b->curl_l[i], t); }
    h.aim_pitch_scale = lerpf(a->aim_pitch_scale, b->aim_pitch_scale, t);
    h.ads_weight = lerpf(a->ads_weight, b->ads_weight, t);
    h.fp_hip = v3_lerp(a->fp_hip, b->fp_hip, t);
    h.fp_weight = lerpf(a->fp_weight, b->fp_weight, t);
    return h;
}

void proc_curl_fingers(const Skeleton *s, Pose *pose, Xform *model, int side, const float *curl, const HandFrames *hf, float weight) {
    static const BoneRole first[2][5] = {
        { BR_L_THUMB1, BR_L_INDEX1, BR_L_MIDDLE1, BR_L_RING1, BR_L_PINKY1 },
        { BR_R_THUMB1, BR_R_INDEX1, BR_R_MIDDLE1, BR_R_RING1, BR_R_PINKY1 },
    };
    int hand = s->role[side ? BR_R_HAND : BR_L_HAND];
    if (hand < 0 || weight <= 0) return;
    for (int f = 0; f < 5; f++) {
        int b = s->role[first[side][f]];
        float ang = curl[f] * DG_DEG2RAD * weight;
        // Three joints per finger: proximal gets less, distal more.
        static const float split[3] = { 0.30f, 0.38f, 0.32f };
        for (int j = 0; j < 3 && b >= 0; j++) {
            // Axis: the palm frame's curl axis, in model space of the current hand.
            Xform palm = xf_mul(model[hand], hf->palm[side]);
            vec3 axis = quat_rotate(palm.r, hf->curl_axis[side]);
            if (f == 0) axis = quat_rotate(palm.r, v3(0, 0, side ? 1.0f : -1.0f));   // thumb folds across the palm
            proc_rotate_bone(s, pose, model, b, quat_axis_angle(axis, ang * split[j] * 3.0f / 3.0f * (f == 0 ? 0.6f : 1.0f)));
            // Next joint = first child.
            int next = -1;
            for (int c = b + 1; c < s->count; c++) if (s->parent[c] == b) { next = c; break; }
            b = next;
        }
    }
}

static float socket_error_angle(quat a, quat b) { return quat_angle_between(a, b) * DG_RAD2DEG; }

void proc_hold(ProcState *st, const ProcInput *in, Pose *pose, Xform *model) {
    const Skeleton *s = in->skel;
    const ProcParams *p = in->params;
    const HandFrames *hf = in->hands;
    const ItemSockets *it = in->item;
    int sp2 = s->role[BR_SPINE2], head = s->role[BR_HEAD];
    int ra = s->role[BR_R_ARM], rf = s->role[BR_R_FOREARM], rh = s->role[BR_R_HAND];
    int la = s->role[BR_L_ARM], lf = s->role[BR_L_FOREARM], lh = s->role[BR_L_HAND];
    int rsh = s->role[BR_R_SHOULDER], lsh = s->role[BR_L_SHOULDER];
    if (sp2 < 0 || ra < 0 || rf < 0 || rh < 0) { st->has_item = false; return; }

    // Style switch blend (~0.2 s).
    if (in->hold_style != st->style) {
        st->prev_style = st->style >= 0 ? st->style : in->hold_style;
        st->style = in->hold_style;
        st->style_t = 0;
    }
    st->style_t = fminf(1.0f, st->style_t + in->dt / fmaxf(0.01f, p->style_blend_time));
    HoldStyle hs = blend_style(proc_hold_style(p, st->prev_style), proc_hold_style(p, st->style), smoothstepf(0, 1, st->style_t));
    float w = saturate(in->hand_ik_weight);

    // Aim frame (model space, relative to the rendered body yaw).
    float yaw = clampf(wrap_angle(in->aim_yaw - st->render_body_yaw), -p->yaw_limit * DG_DEG2RAD, p->yaw_limit * DG_DEG2RAD);
    float pitch = clampf(in->aim_pitch, -p->pitch_limit * DG_DEG2RAD, p->pitch_limit * DG_DEG2RAD) * hs.aim_pitch_scale;
    quat yaw_q = quat_yaw(yaw);
    quat aim = quat_mul(yaw_q, quat_axis_angle(v3(1, 0, 0), pitch));
    vec3 anchor = v3_add(model[sp2].t, quat_rotate(yaw_q, hs.chest_offset));

    // Desired item transform: hip -> ADS -> sprint, plus crouch, kick.
    Xform hip = xf_make(v3_add(anchor, quat_rotate(aim, hs.hip_offset)), quat_mul(aim, euler_deg(hs.hip_rot)));
    vec3 eye = head >= 0 ? v3_add(model[head].t, quat_rotate(yaw_q, v3(0, 0.075f, -0.09f))) : v3_add(anchor, v3(0, 0.3f, 0));
    Xform ads = xf_make(v3_add(eye, quat_rotate(aim, v3(0, -it->sight_height - 0.012f, -hs.ads_distance))), aim);
    // Owner's first-person body: the hip pose sits in view below the eyes (like an owner-only aim
    // pose); everyone else sees the normal hip pose. Reach / glue rules are the same.
    if (in->first_person && hs.fp_weight > 0) {
        Xform fp = xf_make(v3_add(eye, quat_rotate(aim, hs.fp_hip)), hip.r);
        hip = xf_lerp(hip, fp, saturate(hs.fp_weight));
    }
    float ads_t = saturate(in->ads_t) * hs.ads_weight;
    Xform desired = xf_lerp(hip, ads, ads_t);
    if (in->sprinting && ads_t < 0.5f) {
        Xform spr = xf_make(v3_add(anchor, quat_rotate(yaw_q, hs.sprint_offset)), quat_mul(yaw_q, euler_deg(hs.sprint_rot)));
        desired = xf_lerp(desired, spr, 0.85f);
    }
    if (in->crouched) desired.t = v3_add(desired.t, quat_rotate(aim, hs.crouch_offset));
    desired.t = v3_add(desired.t, quat_rotate(aim, in->item_kick_pos));
    desired.r = quat_norm(quat_mul(desired.r, euler_deg(in->item_kick_rot)));

    // Muzzle wall pull-back (local raycast, cosmetic: never changes where bullets go).
    if (in->raycast && it->length > 0.05f) {
        vec3 base_w = xf_point(in->root, anchor);
        vec3 muzzle_w = xf_point(in->root, xf_point(desired, it->muzzle));
        vec3 d = v3_sub(muzzle_w, base_w);
        float frac;
        vec3 hp, hn;
        if (in->raycast(in->ray_ctx, base_w, d, &frac, &hp, &hn) && frac < 1.0f) {
            float pull = (1.0f - frac) * v3_len(d);
            pull = fminf(pull, it->length * 0.7f);
            vec3 fwd = quat_rotate(aim, v3(0, 0, -1));
            desired.t = v3_madd(desired.t, fwd, -pull);
            desired.r = quat_norm(quat_mul(quat_axis_angle(quat_rotate(aim, v3(1, 0, 0)), -pull * 1.6f), desired.r));
        }
    }

    // Clavicles.
    if (rsh >= 0 && v3_len2(hs.shoulder_r) > 0) proc_rotate_bone(s, pose, model, rsh, euler_deg(v3_scale(hs.shoulder_r, w)));
    if (lsh >= 0 && v3_len2(hs.shoulder_l) > 0) proc_rotate_bone(s, pose, model, lsh, euler_deg(v3_scale(hs.shoulder_l, w)));

    // Solve right then left. If grip_l is out of reach, slide the item back along its own
    // barrel axis (stock into the shoulder, sights stay on the aim line) by exactly the
    // shortfall, up to max_shift; the left arm is never stretched.
    Pose base_pose;
    Xform base_model[ANIM_MAX_BONES];
    pose_copy(&base_pose, pose);
    memcpy(base_model, model, sizeof(Xform) * (size_t)s->count);
    float shift = 0;
    st->reach_shifted = false;
    Xform item = desired;
    bool left_ok = true;
    vec3 back = quat_rotate(desired.r, v3(0, 0, 1));   // towards the body
    for (int iter = 0; iter < 10; iter++) {
        if (iter > 0) {
            pose_copy(pose, &base_pose);
            memcpy(model, base_model, sizeof(Xform) * (size_t)s->count);
        }
        Xform want = desired;
        want.t = v3_madd(want.t, back, shift);
        // Right hand: the palm on grip_r.
        Xform hand_r = proc_hand_for_socket(xf_mul(want, it->grip_r), hf->palm[1]);
        vec3 pole_r = quat_rotate(yaw_q, hs.elbow_r);
        anim_ik_two_bone_pose(s, pose, model, ra, rf, rh, hand_r.t, &hand_r.r, pole_r, w);
        // Item finalized from the solved right hand (this is what gets rendered).
        Xform palm_r = xf_mul(model[rh], hf->palm[1]);
        item = xf_mul(palm_r, xf_inverse(it->grip_r));
        st->right_err_pos = v3_dist(palm_r.t, xf_mul(want, it->grip_r).t);
        if (!hs.left_hand || !it->has_left || la < 0 || lf < 0 || lh < 0) { left_ok = true; break; }
        // Left hand: item-space socket -> model-space target, full rotation.
        Xform sock_l = xf_mul(item, it->grip_l);
        Xform hand_l = proc_hand_for_socket(sock_l, hf->palm[0]);
        float reach = v3_dist(model[la].t, model[lf].t) + v3_dist(model[lf].t, model[lh].t);
        float need = v3_dist(model[la].t, hand_l.t);
        float margin = 0.004f;
        if (need > reach - margin * 0.5f && iter < 9 && shift < hs.max_shift) {
            // How much does moving along `back` shorten the shoulder -> hand distance?
            vec3 to_sh = v3_norm(v3_sub(model[la].t, hand_l.t));
            float gain = v3_dot(to_sh, back);
            if (gain < 0.5f) {
                // Long axis isn't the barrel (melee) or points away: slide towards the shoulder instead.
                back = v3_norm(v3_add(v3_scale(back, fmaxf(gain, 0.0f)), to_sh));
                gain = fmaxf(0.5f, v3_dot(to_sh, back));
            }
            shift = fminf(hs.max_shift, shift + (need - reach + margin) / gain);
            st->reach_shifted = true;
            continue;
        }
        vec3 pole_l = quat_rotate(yaw_q, hs.elbow_l);
        left_ok = anim_ik_two_bone_pose(s, pose, model, la, lf, lh, hand_l.t, &hand_l.r, pole_l, w);
        Xform palm_l = xf_mul(model[lh], hf->palm[0]);
        st->glue_err_pos = v3_dist(palm_l.t, sock_l.t);
        st->glue_err_ang = socket_error_angle(palm_l.r, sock_l.r);
        break;
    }
    // Only meaningful at full IK weight (equip / reload blends move the hands on purpose).
    if (!left_ok && w > 0.99f && !(st->warned_styles & (1u << (st->style & 31)))) {
        st->warned_styles |= 1u << (st->style & 31);
        log_warn("hold style '%s': grip_l out of reach even after shifting %.0f mm (fix the socket)", hs.name, shift * 1000.0f);
    }
    // Fingers last: they never move the wrist.
    proc_curl_fingers(s, pose, model, 1, hs.curl_r, hf, w);
    if (hs.left_hand && it->has_left) proc_curl_fingers(s, pose, model, 0, hs.curl_l, hf, w);
    st->item_model = item;
    st->item_world = xf_mul(in->root, item);
    st->has_item = true;
}

// Hands to world anchors (ledge grab during mantle / hang). Runs after the hold so it wins.
void proc_hand_anchors(ProcState *st, const ProcInput *in, Pose *pose, Xform *model) {
    (void)st;
    const Skeleton *s = in->skel;
    float w = saturate(in->hand_anchor_w);
    if (w <= 0) return;
    Xform inv_root = xf_inverse(in->root);
    static const BoneRole chain[2][3] = { { BR_L_ARM, BR_L_FOREARM, BR_L_HAND }, { BR_R_ARM, BR_R_FOREARM, BR_R_HAND } };
    for (int h = 0; h < 2; h++) {
        int a = s->role[chain[h][0]], b = s->role[chain[h][1]], c = s->role[chain[h][2]];
        if (a < 0 || b < 0 || c < 0) continue;
        vec3 target = xf_point(inv_root, in->hand_anchor[h]);
        // Palm down on the ledge: back of the hand up, fingers forward.
        vec3 fwd = v3(0, 0, -1), up = v3(0, 1, 0), x = v3_cross(up, v3_neg(fwd));
        (void)x;
        Xform palm = xf_make(target, quat_identity());
        Xform hand = proc_hand_for_socket(palm, in->hands->palm[h]);
        vec3 pole = v3(h ? 1.0f : -1.0f, -0.6f, 0.4f);
        anim_ik_two_bone_pose(s, pose, model, a, b, c, hand.t, &hand.r, pole, w);
    }
}
