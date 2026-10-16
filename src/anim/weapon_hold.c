// Procedural weapon holding: hold style -> desired item transform -> right-arm IK -> item
// finalized from the solved right hand -> left hand IK onto grip_l (position + rotation) ->
// fingers. Each hand's socket, wrist offset, pole hint, IK weight and finger joints come from its
// hand pose (src/anim/handpose.c, docs/HANDPOSE.md). Hands stay glued to the item by construction:
//   * the item is parented to the right hand (its transform is derived from the solved hand),
//   * the left-hand target is computed from that final item transform, this tick,
//   * the left arm never stretches: if grip_l is out of reach the item / right arm move towards
//     the body (up to max_shift) until it reaches, and the style is reported once.
#include "procedural_internal.h"
#include "core/log.h"
#include "gameplay/phone/phone_orient.h"
#include <math.h>

static HoldStyle blend_style(const HoldStyle *a, const HoldStyle *b, float t) {
    if (t >= 1.0f || a == b) return *b;
    HoldStyle h = *b;   // hold point data: the current style's (the hold point blends itself)
    h.elbow_r = v3_lerp(a->elbow_r, b->elbow_r, t);
    h.elbow_l = v3_lerp(a->elbow_l, b->elbow_l, t);
    h.shoulder_r = v3_lerp(a->shoulder_r, b->shoulder_r, t);
    h.shoulder_l = v3_lerp(a->shoulder_l, b->shoulder_l, t);
    for (int i = 0; i < 5; i++) { h.curl_r[i] = lerpf(a->curl_r[i], b->curl_r[i], t); h.curl_l[i] = lerpf(a->curl_l[i], b->curl_l[i], t); }
    return h;
}

void proc_curl_fingers(const Skeleton *s, Pose *pose, Xform *model, int side, const float *curl, const HandFrames *hf, float weight) {
    HandFingers j;
    handpose_from_curl(curl, j);
    handpose_apply_fingers(s, pose, model, side, j, hf, weight);
}

// ------------------------------------------------------------------ hand poses on the item

static bool xf_is_identity(Xform x) { return x.t.x == 0 && x.t.y == 0 && x.t.z == 0 && x.r.x == 0 && x.r.y == 0 && x.r.z == 0 && x.r.w == 1; }

// One hand's target this tick: the hold style's pose (base) with the item's entry on top. The
// socket is the item's own (grip_r / grip_l) unless the pose overrides it; the wrist offset is
// folded in so the IK, the glue check and the item finalization all use the same palm target.
static u32 hand_target(const ProcInput *in, const HoldStyle *hs, const ItemSockets *it, int hand, int state, HandHold *o) {
    memset(o, 0, sizeof(*o));
    o->sock = hand ? it->grip_r : it->grip_l;
    o->pole = hand ? hs->elbow_r : hs->elbow_l;
    o->ikw = 1.0f;
    const HandPoseDB *db = in->handposes;
    int base = db ? handpose_find_hash(db, handpose_style_hash(hs->name)) : -1;
    int entry = db ? handpose_find_hash(db, it->pose_id[hand]) : -1;
    if (base < 0 && entry < 0) {
        handpose_from_curl(hand ? hs->curl_r : hs->curl_l, o->j);   // no data: the style's curl
        return dg_hash_u32(hs->hash ^ (u32)hand);
    }
    HandPose p;
    handpose_resolve_over(db, base, entry, hand, state, &p);
    if (p.has_socket) o->sock = it->has_pose_space[hand] ? xf_mul(it->pose_space[hand], p.socket) : p.socket;
    if (!xf_is_identity(p.wrist)) o->sock = xf_mul(o->sock, p.wrist);
    if (p.has_pole) o->pole = p.pole;
    o->ikw = p.ik_weight;
    memcpy(o->j, p.j, sizeof(o->j));
    // Edits to the data apply at once (the editor drags them); entry / style / state changes blend.
    return dg_hash_u32((u32)(base + 1) * 2654435761u ^ (u32)(entry + 1) * 40503u ^ (u32)state * 97u ^ (u32)hand);
}

static float decay5(float x) {
    x = saturate(x);
    return 1.0f - x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

// Inertialization: on a change the offset (current output - new target) is captured and decays
// to zero over `dur` (quintic); settled, the output is the target exactly.
static void hand_hold_update(HandHold *h, const HandHold *tg, u32 key, float dt, float dur) {
    if (!h->init) {
        *h = *tg;
        h->init = true;
        h->key = key;
        h->t = h->dur = 0;
        return;
    }
    if (key != h->key) {
        h->off_t = v3_sub(h->sock.t, tg->sock.t);
        h->off_r = quat_dot_fix(quat_identity(), quat_mul(h->sock.r, quat_conj(tg->sock.r)));
        h->off_pole = v3_sub(h->pole, tg->pole);
        h->off_ikw = h->ikw - tg->ikw;
        for (int f = 0; f < HAND_FINGERS; f++)
            for (int k = 0; k < HAND_FINGER_JOINTS; k++) {
                h->off_j[f][k].curl = h->j[f][k].curl - tg->j[f][k].curl;
                h->off_j[f][k].spread = h->j[f][k].spread - tg->j[f][k].spread;
                h->off_j[f][k].twist = h->j[f][k].twist - tg->j[f][k].twist;
            }
        h->key = key;
        h->t = 0;
        h->dur = dur;
    } else {
        h->t += dt;
    }
    h->sock = tg->sock;
    h->pole = tg->pole;
    h->ikw = tg->ikw;
    memcpy(h->j, tg->j, sizeof(h->j));
    if (h->t >= h->dur) return;
    float k = decay5(h->t / h->dur);
    h->sock.t = v3_madd(h->sock.t, h->off_t, k);
    h->sock.r = quat_norm(quat_mul(quat_nlerp(quat_identity(), h->off_r, k), h->sock.r));
    h->pole = v3_madd(h->pole, h->off_pole, k);
    h->ikw = saturate(h->ikw + h->off_ikw * k);
    for (int f = 0; f < HAND_FINGERS; f++)
        for (int q = 0; q < HAND_FINGER_JOINTS; q++) {
            h->j[f][q].curl += h->off_j[f][q].curl * k;
            h->j[f][q].spread += h->off_j[f][q].spread * k;
            h->j[f][q].twist += h->off_j[f][q].twist * k;
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

    // Bladed stance (before the camera / arms): the chest yaws so the support shoulder comes
    // forward, the neck counter-rotates so the head (and the camera on it) keeps facing the view;
    // clavicles protract. Scaled by the hand IK weight (clips own the body otherwise).
    {
        float tw = hs.hp.twist * DG_DEG2RAD * w;
        int sp1 = s->role[BR_SPINE1], neck = s->role[BR_NECK];
        if (fabsf(tw) > 1e-5f && sp1 >= 0 && neck >= 0) {
            float base_yaw = wrap_angle(in->aim_yaw - st->render_body_yaw);
            (void)base_yaw;
            proc_rotate_bone(s, pose, model, sp1, quat_yaw(-tw * 0.45f));
            proc_rotate_bone(s, pose, model, sp2, quat_yaw(-tw * 0.55f));
            proc_rotate_bone(s, pose, model, neck, quat_yaw(tw));
        }
        if (lsh >= 0 && v3_len2(hs.hp.clav_l) > 0 && hs.left_hand && it->has_left) proc_rotate_bone(s, pose, model, lsh, euler_deg(v3_scale(hs.hp.clav_l, w)));
        if (rsh >= 0 && v3_len2(hs.hp.clav_r) > 0) proc_rotate_bone(s, pose, model, rsh, euler_deg(v3_scale(hs.hp.clav_r, w)));
    }

    // Camera (model space): the head bone + eye socket after the spine look, rotated by the view
    // (relative to the rendered body yaw). Same for the owner and every remote copy.
    float rel_yaw = wrap_angle(in->aim_yaw - st->render_body_yaw);
    float pitch = clampf(in->aim_pitch, -89.0f * DG_DEG2RAD, 89.0f * DG_DEG2RAD);
    quat yaw_q = quat_yaw(rel_yaw);
    quat aim = quat_mul(yaw_q, quat_axis_angle(v3(1, 0, 0), pitch));
    vec3 eye = (in->eye_bone >= 0 && in->eye_bone < s->count) ? xf_point(model[in->eye_bone], in->eye_local.t)
                                                                : (head >= 0 ? v3_add(model[head].t, quat_rotate(yaw_q, v3(0, 0.08f, -0.09f))) : v3_add(model[sp2].t, v3(0, 0.3f, 0)));
    Xform cam = xf_make(eye, aim);
    st->cam_model = cam;

    // Hold point: camera-space item placement (state, ADS from the sight socket, lag / sway / bob /
    // kick around the pivot, wall collision).
    HoldPointInput hi;
    memset(&hi, 0, sizeof(hi));
    hi.style = &proc_hold_style(p, st->style)->hp;
    hi.ovr = &it->hp;
    hi.style_key = in->item_key * 64 + st->style;
    hi.anchor = xf_make(it->anchor.t, quat_identity());   // the anchor is a point: the hold point's rotation orients the item
    hi.sight = it->sight;
    hi.pivot = it->pivot;
    hi.length = it->length;
    hi.has_item = true;
    hi.ads = in->ads_t > 0.5f;
    hi.sprint = in->sprinting;
    hi.crouch = in->crouched;
    hi.prone = in->prone;
    hi.inspect = in->inspect;
    hi.empty = in->empty_hands;
    hi.vehicle_onehand = in->vehicle_onehand;
    hi.phone_mode = in->phone_mode;
    // Phone driven by a real phone: its rotation relative to the view yaw, in camera space (the
    // view pitch removed), clamped to the arm's reach cone; eased in / out over 0.3 s.
    if (in->phone_driven) st->phone_rot = quat_norm(in->phone_rot);
    if (st->phone_rot.w == 0 && st->phone_rot.x == 0 && st->phone_rot.y == 0 && st->phone_rot.z == 0) st->phone_rot = quat_identity();
    st->phone_w = approachf(st->phone_w, in->phone_driven && in->phone_mode ? 1.0f : 0.0f, in->dt / 0.3f);
    if (st->phone_w > 0.0f) {
        PhoneReach reach = phone_reach_default();
        hi.rot_cam = phone_clamp_reach(phone_to_camera(st->phone_rot, pitch), &reach);
        hi.rot_w = smoothstepf(0.0f, 1.0f, st->phone_w);
    }
    hi.dt = in->dt;
    hi.aim_yaw = in->aim_yaw;
    hi.aim_pitch = pitch;
    Xform cam_world = xf_mul(in->root, cam);
    cam_world.s = v3(1, 1, 1);
    hi.vel_cam = quat_rotate(quat_conj(cam_world.r), in->velocity);
    hi.grounded = in->grounded;
    hi.kick_pos = in->item_kick_pos;
    hi.kick_rot = in->item_kick_rot;
    hi.cam_world = cam_world;
    hi.raycast = (HoldPointRayFn)in->raycast;
    hi.ray_ctx = in->ray_ctx;
    hi.static_pose = in->hp_static;
    hi.sway_scale = in->hp_sway_scale;
    hi.bob_scale = in->hp_bob_scale;
    hi.kick_params = in->hold_kick;
    holdpoint_eval(&st->hp, &hi, &st->hp_out);
    st->item_cam = st->hp_out.item;
    // Hand poses for this state (sockets in item space incl. the wrist offset, poles, fingers).
    for (int h = 0; h < 2; h++) {
        HandHold tg;
        u32 key = hand_target(in, &hs, it, h, HPS_FROM_HOLD(st->hp_out.state), &tg);
        hand_hold_update(&st->hand[h], &tg, key, in->dt, fmaxf(0.01f, p->style_blend_time));
    }
    const Xform sock_r = st->hand[1].sock, sock_l0 = st->hand[0].sock;
    Xform desired = xf_mul(cam, st->hp_out.item);
    // Upper body follows the hold point: clavicle additive from its offset to the hip pose.
    vec3 off = st->hp_out.offset;
    float cg = hs.hp.clavicle_gain;
    vec3 add_r = v3(0, clampf(-off.x * cg * 0.5f, -8, 8), clampf(off.y * cg, -10, 10));
    vec3 add_l = v3(0, clampf(-off.x * cg * 0.5f, -8, 8), clampf(-off.y * cg, -10, 10));
    hs.shoulder_r = v3_add(hs.shoulder_r, add_r);
    if (hs.left_hand && it->has_left) hs.shoulder_l = v3_add(hs.shoulder_l, add_l);

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
    float max_shift = fminf(hs.max_shift, hs.hp.reach_pull > 0 ? hs.hp.reach_pull : hs.max_shift);
    vec3 extra = v3(0, 0, 0);   // last resort (near-vertical views): pull straight towards the support shoulder
    bool emergency = false;
    // ADS: the item must stay exactly on its hold point (sight on the view axis at the eye
    // relief), so a short support arm slides the left hand back along the handguard first
    // (towards the receiver, as a shooter shifts the grip) and only then moves the item.
    bool ads_exact = st->hp_out.state == HP_ADS;
    float slide = 0, max_slide = ads_exact && it->has_left ? fmaxf(0.0f, -sock_l0.t.z - 0.08f) : 0.0f;
    for (int iter = 0; iter < 16; iter++) {
        if (iter > 0) {
            pose_copy(pose, &base_pose);
            memcpy(model, base_model, sizeof(Xform) * (size_t)s->count);
        }
        Xform want = desired;
        want.t = v3_add(v3_madd(want.t, back, shift), extra);
        // Right hand: the palm on grip_r (the shoulder reaches first, the arm never stretches).
        Xform hand_r = proc_hand_for_socket(xf_mul(want, sock_r), hf->palm[1]);
        {
            float reach_r = v3_dist(model[ra].t, model[rf].t) + v3_dist(model[rf].t, model[rh].t);
            proc_clavicle_reach(s, pose, model, rsh, ra, hand_r.t, reach_r, 0.003f, 45.0f * w);
        }
        vec3 pole_r = quat_rotate(yaw_q, st->hand[1].pole);
        anim_ik_two_bone_pose(s, pose, model, ra, rf, rh, hand_r.t, &hand_r.r, pole_r, w * st->hand[1].ikw);
        // Item finalized from the solved right hand (this is what gets rendered).
        Xform palm_r = xf_mul(model[rh], hf->palm[1]);
        item = xf_mul(palm_r, xf_inverse(sock_r));
        st->right_err_pos = v3_dist(palm_r.t, xf_mul(want, sock_r).t);
        if (!hs.left_hand || !it->has_left || la < 0 || lf < 0 || lh < 0) { left_ok = true; break; }
        // Left hand: item-space socket -> model-space target, full rotation.
        Xform grip_l = sock_l0;
        grip_l.t.z += slide;
        Xform sock_l = xf_mul(item, grip_l);
        Xform hand_l = proc_hand_for_socket(sock_l, hf->palm[0]);
        float reach = v3_dist(model[la].t, model[lf].t) + v3_dist(model[lf].t, model[lh].t);
        float margin = 0.004f;
        // The shoulder reaches first (clavicle protraction, up to 45 deg): the item stays on its
        // hold point; only what the shoulder can't cover moves the item.
        proc_clavicle_reach(s, pose, model, lsh, la, hand_l.t, reach, margin * 1.5f, 75.0f * w);
        float need = v3_dist(model[la].t, hand_l.t);
        if (need > reach - margin * 0.5f && iter < 15 && slide < max_slide - 1e-5f) {
            vec3 to_sh = v3_norm(v3_sub(model[la].t, hand_l.t));
            float g = v3_dot(to_sh, quat_rotate(item.r, v3(0, 0, 1)));
            if (g > 0.2f) {
                slide = fminf(max_slide, slide + (need - reach + margin) / g);
                continue;
            }
        }
        if (need > reach - margin * 0.5f && iter < 15 && shift >= max_shift - 1e-5f) {
            // The barrel-axis pull is exhausted: move the hold point towards the shoulder by the
            // remaining shortfall (never stretch the arm, never leave a gap).
            vec3 to_sh = v3_norm(v3_sub(model[la].t, hand_l.t));
            extra = v3_madd(extra, to_sh, need - reach + margin);
            emergency = true;
            st->reach_shifted = true;
            continue;
        }
        if (need > reach - margin * 0.5f && iter < 15 && shift < max_shift) {
            // How much does moving along `back` shorten the shoulder -> hand distance?
            vec3 to_sh = v3_norm(v3_sub(model[la].t, hand_l.t));
            float gain = v3_dot(to_sh, back);
            if (gain < 0.5f) {
                // Long axis isn't the barrel (melee) or points away: slide towards the shoulder instead.
                back = v3_norm(v3_add(v3_scale(back, fmaxf(gain, 0.0f)), to_sh));
                gain = fmaxf(0.5f, v3_dot(to_sh, back));
            }
            shift = fminf(max_shift, shift + (need - reach + margin) / gain);
            st->reach_shifted = true;
            continue;
        }
        vec3 pole_l = quat_rotate(yaw_q, st->hand[0].pole);
        left_ok = anim_ik_two_bone_pose(s, pose, model, la, lf, lh, hand_l.t, &hand_l.r, pole_l, w * st->hand[0].ikw);
        Xform palm_l = xf_mul(model[lh], hf->palm[0]);
        st->glue_err_pos = v3_dist(palm_l.t, sock_l.t);
        st->glue_err_ang = socket_error_angle(palm_l.r, sock_l.r);
        break;
    }
    // Only meaningful at full IK weight (equip / reload blends move the hands on purpose).
    if ((!left_ok || emergency) && w > 0.99f && !(st->warned_styles & (1u << (st->style & 31)))) {
        st->warned_styles |= 1u << (st->style & 31);
        log_warn("hold point '%s' / %s: grip_l out of reach (pitch %.0f deg): hold point pulled %.0f mm along the item + %.0f mm towards the shoulder",
                 hs.name, holdpoint_state_name(st->hp_out.state), pitch * DG_RAD2DEG, shift * 1000.0f, v3_len(extra) * 1000.0f);
    }
    // Fingers last: they never move the wrist.
    if (!in->no_fingers) {
        handpose_apply_fingers(s, pose, model, 1, st->hand[1].j, hf, w);
        if (hs.left_hand && it->has_left) handpose_apply_fingers(s, pose, model, 0, st->hand[0].j, hf, w);
    }
    st->reach_shift = shift;
    st->grip_l_slide = slide;
    // Camera-space target including the reach pull (the owner re-places the item per frame).
    st->item_cam = xf_mul(xf_inverse(cam), xf_make(v3_add(v3_madd(desired.t, back, shift), extra), desired.r));
    st->item_model = item;
    st->item_world = xf_mul(in->root, item);
    st->has_item = true;
}

float proc_grip_l_slide_max(const ItemSockets *k) {
    if (!k || !k->has_left) return 0.0f;
    return fmaxf(0.0f, -k->grip_l.t.z - 0.08f);
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
        if (a < 0 || b < 0 || c < 0 || (in->hand_anchor_skip & (1u << h))) continue;
        vec3 target = xf_point(inv_root, in->hand_anchor[h]);
        // Palm down on the ledge: back of the hand up, fingers forward.
        vec3 fwd = v3(0, 0, -1), up = v3(0, 1, 0), x = v3_cross(up, v3_neg(fwd));
        (void)x;
        quat pr = in->hand_anchor_rot_valid ? quat_mul(inv_root.r, in->hand_anchor_rot[h]) : quat_identity();
        Xform palm = xf_make(target, pr);
        // Vehicle controls: the hand pose's socket is an offset from the anchor (relative entry).
        HandPose hp;
        bool posed = false;
        if (in->handposes && (in->anchor_pose[h] || in->anchor_pose_base[h])) {
            int base = handpose_find_hash(in->handposes, in->anchor_pose_base[h]), entry = handpose_find_hash(in->handposes, in->anchor_pose[h]);
            if (base >= 0 || entry >= 0) {
                handpose_resolve_over(in->handposes, base, entry, h, in->anchor_pose_state[h], &hp);
                if (hp.has_socket) palm = xf_mul(palm, hp.socket);
                palm = xf_mul(palm, hp.wrist);
                posed = true;
            }
        }
        Xform hand = proc_hand_for_socket(palm, in->hands->palm[h]);
        vec3 pole = v3(h ? 1.0f : -1.0f, -0.6f, 0.4f);
        if (posed && hp.has_pole) pole = quat_rotate(quat_yaw(wrap_angle(in->aim_yaw - st->render_body_yaw)), hp.pole);   // aim space, like the styles' poles
        anim_ik_two_bone_pose(s, pose, model, a, b, c, hand.t, &hand.r, pole, w * (posed ? hp.ik_weight : 1.0f));
        if (posed && !in->no_fingers) handpose_apply_fingers(s, pose, model, h, hp.j, in->hands, w);
    }
}
