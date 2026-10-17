// Hand pose control rig: controls, hierarchy, world transforms, transforms -> data (docs/HANDPOSE.md).
#include "hp_rig.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static HpCtrl g_ctrls[HP_RIG_CTRLS];
static bool g_built;

static void build(void) {
    if (g_built) return;
    static const char *side[2] = { "L", "R" };
    HpCtrl *c = &g_ctrls[HP_RIG_ROOT];
    *c = (HpCtrl){ HPK_ROOT, -1, -1, -1, -1, HPA_T | HPA_R | HPA_S, "Root" };
    for (int h = 0; h < 2; h++) {
        c = &g_ctrls[HP_RIG_IK(h)];
        *c = (HpCtrl){ HPK_IK, (i8)h, -1, -1, HP_RIG_ROOT, HPA_T | HPA_R, "" };
        snprintf(c->name, sizeof(c->name), "IK_hand_%s", side[h]);
        c = &g_ctrls[HP_RIG_POLE(h)];
        *c = (HpCtrl){ HPK_POLE, (i8)h, -1, -1, HP_RIG_ROOT, HPA_T, "" };
        snprintf(c->name, sizeof(c->name), "Pole_%s", side[h]);
        for (int f = 0; f < HAND_FINGERS; f++)
            for (int j = 0; j < HAND_FINGER_JOINTS; j++) {
                c = &g_ctrls[HP_RIG_FINGER(h, f, j)];
                *c = (HpCtrl){ HPK_FINGER, (i8)h, (i8)f, (i8)j, (i16)(j ? HP_RIG_FINGER(h, f, j - 1) : HP_RIG_IK(h)), HPA_R, "" };
                snprintf(c->name, sizeof(c->name), "%s_%s_%d", side[h], handpose_finger_name(f), j + 1);
            }
    }
    g_built = true;
}

const HpCtrl *hp_rig_ctrl(int i) {
    build();
    return i >= 0 && i < HP_RIG_CTRLS ? &g_ctrls[i] : NULL;
}

int hp_rig_find(const char *name) {
    build();
    for (int i = 0; name && i < HP_RIG_CTRLS; i++) if (!strcmp(g_ctrls[i].name, name)) return i;
    return -1;
}

int hp_rig_parent(int i) { const HpCtrl *c = hp_rig_ctrl(i); return c ? c->parent : -1; }

int hp_rig_children(int i, int *out, int max) {
    build();
    int n = 0;
    for (int k = 0; k < HP_RIG_CTRLS && n < max; k++) if (g_ctrls[k].parent == i && i >= 0) out[n++] = k;
    return n;
}

int hp_rig_first_child(int i) {
    int c;
    return hp_rig_children(i, &c, 1) ? c : -1;
}

int hp_rig_siblings(int i, int *out, int max) {
    build();
    int p = hp_rig_parent(i), n = 0;
    if (i < 0 || i >= HP_RIG_CTRLS) return 0;
    for (int k = 0; k < HP_RIG_CTRLS && n < max; k++) if (k != i && g_ctrls[k].parent == p && p >= 0) out[n++] = k;
    return n;
}

int hp_rig_mirror(int i) {
    const HpCtrl *c = hp_rig_ctrl(i);
    if (!c) return -1;
    switch (c->kind) {
    case HPK_IK: return HP_RIG_IK(!c->hand);
    case HPK_POLE: return HP_RIG_POLE(!c->hand);
    case HPK_FINGER: return HP_RIG_FINGER(!c->hand, c->finger, c->joint);
    default: return i;
    }
}

bool hp_rig_is_ancestor(int a, int i) {
    for (int p = hp_rig_parent(i), n = 0; p >= 0 && n < 8; p = hp_rig_parent(p), n++)
        if (p == a) return true;
    return false;
}

void hp_rig_select_finger(bool *sel, int hand, int finger, bool on) {
    for (int j = 0; j < HAND_FINGER_JOINTS; j++) sel[HP_RIG_FINGER(hand, finger, j)] = on;
}

void hp_rig_select_hand(bool *sel, int hand, bool on) {
    sel[HP_RIG_IK(hand)] = on;
    for (int f = 0; f < HAND_FINGERS; f++) hp_rig_select_finger(sel, hand, f, on);
}

int hp_rig_top_level(const bool *sel, int *out, int max) {
    int n = 0;
    for (int i = 0; i < HP_RIG_CTRLS && n < max; i++) {
        if (!sel[i]) continue;
        bool carried = false;
        for (int p = hp_rig_parent(i), d = 0; p >= 0 && d < 8 && !carried; p = hp_rig_parent(p), d++) carried = sel[p];
        if (!carried) out[n++] = i;
    }
    return n;
}

// ------------------------------------------------------------------ world transforms

// Xform with a uniform scale on its own frame: the point p of the frame's space -> world.
static vec3 scaled_point(Xform x, vec3 p) { return v3_add(x.t, quat_rotate(x.r, v3_scale(p, x.s.x > 0 ? x.s.x : 1.0f))); }

static Xform root_world(const HandPoseDB *db, HpTarget t, const HpRigCtx *ctx) {
    Xform r = hp_target_root(db, t);
    Xform w = xf_make(xf_point(ctx->root_parent, r.t), quat_norm(quat_mul(ctx->root_parent.r, r.r)));
    float s = ctx->grip_scale > 0 ? ctx->grip_scale : (r.s.x > 0 ? r.s.x : 1.0f);
    w.s = t.entry >= 0 ? v3s(s) : v3(1, 1, 1);
    return w;
}

// The frame a hand's IK target is expressed in (world): grips: the item (root, scaled); case
// keys: the hand's base target (with the anchor unless the base has it).
static Xform ik_space(const HandPoseDB *db, HpTarget t, const HpRigCtx *ctx, int h) {
    if (t.entry >= 0) {
        Xform r = root_world(db, t, ctx), gs = ctx->grip_space[h];
        if (gs.r.w == 0 && gs.r.x == 0 && gs.r.y == 0 && gs.r.z == 0) gs = xf_identity();
        Xform o = xf_make(scaled_point(r, gs.t), quat_norm(quat_mul(r.r, gs.r)));
        o.s = r.s;
        return o;
    }
    Xform b = ctx->hand_base[h];
    if (ctx->anchor_applied) b = xf_mul(b, hp_target_root(db, t));
    b.s = v3(1, 1, 1);
    return b;
}

static Xform sock_to_world(Xform space, Xform sock) {
    Xform w = xf_make(scaled_point(space, sock.t), quat_norm(quat_mul(space.r, sock.r)));
    return w;
}

static Xform world_to_sock(Xform space, Xform world) {
    float s = space.s.x > 0 ? space.s.x : 1.0f;
    quat ic = quat_conj(space.r);
    Xform o = xf_make(v3_scale(quat_rotate(ic, v3_sub(world.t, space.t)), 1.0f / s), quat_norm(quat_mul(ic, world.r)));
    return o;
}

// The target's resolved IK socket of a hand in its own space (wrist folded; grips: the item's
// own socket when nothing sets it).
static Xform resolved_sock(const HandPoseDB *db, HpTarget t, int h, const HpEditDefaults *def) {
    HandPose p;
    hp_target_resolve(db, t, h, &p);
    Xform s = (t.entry >= 0 && !p.has_socket) ? (def ? def->socket[h] : xf_identity()) : p.socket;
    s = xf_mul(s, p.wrist);
    s.s = v3(1, 1, 1);
    return s;
}

static vec3 resolved_pole(const HandPoseDB *db, HpTarget t, int h, const HpEditDefaults *def) {
    HandPose p;
    hp_target_resolve(db, t, h, &p);
    return p.has_pole ? p.pole : (def ? def->pole[h] : v3(h ? 0.7f : -0.7f, -1, 0));
}

void hp_rig_world(const HandPoseDB *db, HpTarget t, const HpRigCtx *ctx, const HpEditDefaults *def, Xform *out) {
    for (int i = 0; i < HP_RIG_CTRLS; i++) out[i] = xf_identity();
    out[HP_RIG_ROOT] = root_world(db, t, ctx);
    bool layers = hp_target_has_layers(db, t);
    for (int h = 0; h < 2; h++) {
        Xform sock = layers ? resolved_sock(db, t, h, def) : xf_identity();
        out[HP_RIG_IK(h)] = sock_to_world(ik_space(db, t, ctx, h), sock);
        vec3 pole = layers ? resolved_pole(db, t, h, def) : (def ? def->pole[h] : v3(0, -1, 0));
        float l = v3_len(pole);
        vec3 d = l > 1e-6f ? v3_scale(pole, 1.0f / l) : v3(0, -1, 0);
        out[HP_RIG_POLE(h)] = xf_make(v3_madd(ctx->elbow[h], quat_rotate(ctx->cam_rot, d), ctx->pole_len > 0 ? ctx->pole_len : 0.25f), ctx->cam_rot);
        for (int f = 0; f < HAND_FINGERS; f++)
            for (int j = 0; j < HAND_FINGER_JOINTS; j++) out[HP_RIG_FINGER(h, f, j)] = ctx->finger_bone[h][f][j];
    }
}

void hp_rig_finger_axes(const HpRigCtx *ctx, int h, int f, int j, vec3 axes[3]) {
    h = h ? 1 : 0;
    quat pr = ctx->palm_frame[h].r;
    vec3 up = quat_rotate(pr, v3(0, 1, 0));
    vec3 a = ctx->finger_bone[h][f][j].t;
    vec3 b = j + 1 < HAND_FINGER_JOINTS ? ctx->finger_bone[h][f][j + 1].t : ctx->finger_tip[h][f];
    vec3 bone = v3_sub(b, a);
    float bl = v3_len(bone);
    bone = bl > 1e-6f ? v3_scale(bone, 1.0f / bl) : quat_rotate(pr, v3(0, 0, -1));
    // Signed so that +angle about axes[i] is +curl / +spread / +twist (handpose_apply_fingers).
    axes[0] = f == HF_THUMB ? quat_rotate(pr, v3(0, 0, h ? 1.0f : -1.0f)) : quat_rotate(pr, ctx->curl_axis[h]);
    if (f == HF_THUMB) {
        vec3 ax = v3_cross(bone, up);
        axes[1] = v3_len2(ax) > 1e-8f ? v3_scale(v3_norm(ax), -1.0f) : up;
    } else {
        axes[1] = v3_scale(up, -(ctx->thumb_side[h] != 0 ? ctx->thumb_side[h] : 1.0f));
    }
    axes[2] = v3_scale(bone, h ? 1.0f : -1.0f);
}

// ------------------------------------------------------------------ apply

static vec3 keep_locked3(vec3 now, vec3 old, u8 bits) {
    if (bits & 1) now.x = old.x;
    if (bits & 2) now.y = old.y;
    if (bits & 4) now.z = old.z;
    return now;
}

static Xform apply_locks(Xform now, Xform old, u8 locks) {
    now.t = keep_locked3(now.t, old.t, locks & 7);
    if (locks & HPL_LOCK_ROT) {
        vec3 en = handpose_quat_to_euler_deg(now.r), eo = handpose_quat_to_euler_deg(old.r);
        now.r = quat_norm(handpose_euler_deg_to_quat(keep_locked3(en, eo, (u8)((locks >> 3) & 7))));
    }
    return now;
}

static HandPoseLayer *set_socket(HandPoseDB *db, HpTarget t, int h, const HpEditDefaults *def, Xform sock) {
    HandPoseLayer *l = hp_edit_touch(db, t, h, HPL_SOCKET, def);
    if (!l) return NULL;
    // The wrist offset is folded into the IK target (one control).
    l->socket = sock;
    l->socket.r = quat_norm(l->socket.r);
    l->socket.s = v3(1, 1, 1);
    l->wrist = xf_identity();
    l->set = (u16)(l->set & ~HPL_WRIST);
    return l;
}

bool hp_rig_apply(HandPoseDB *db, HpTarget t, const HpRigCtx *ctx, const HpEditDefaults *def, int ctrl, Xform start, Xform world, const HandJoint *j0, u8 locks) {
    const HpCtrl *c = hp_rig_ctrl(ctrl);
    if (!c || !hp_target_valid(db, t)) return false;
    switch (c->kind) {
    case HPK_ROOT: {
        Xform old = hp_target_root(db, t);
        quat ic = quat_conj(ctx->root_parent.r);
        Xform nl = xf_make(quat_rotate(ic, v3_sub(world.t, ctx->root_parent.t)), quat_norm(quat_mul(ic, world.r)));
        nl = apply_locks(nl, old, locks);
        float s = t.entry >= 0 ? (world.s.x > 0 ? world.s.x : 1.0f) : 1.0f;
        if (locks & HPL_LOCK_SCALE) s = old.s.x > 0 ? old.s.x : 1.0f;
        nl.s = v3s(s);
        hp_target_set_root(db, t, nl);
        return true;
    }
    case HPK_IK: {
        if (!hp_target_has_layers(db, t)) return false;
        Xform old = resolved_sock(db, t, c->hand, def);
        Xform sock = apply_locks(world_to_sock(ik_space(db, t, ctx, c->hand), world), old, locks);
        return set_socket(db, t, c->hand, def, sock) != NULL;
    }
    case HPK_POLE: {
        if (!hp_target_has_layers(db, t)) return false;
        vec3 old = resolved_pole(db, t, c->hand, def);
        vec3 d = quat_rotate(quat_conj(ctx->cam_rot), v3_sub(world.t, ctx->elbow[c->hand]));
        float dl = v3_len(d), ol = v3_len(old);
        if (dl < 1e-6f) return false;
        vec3 np = keep_locked3(v3_scale(d, (ol > 1e-6f ? ol : 1.0f) / dl), old, locks & 7);
        HandPoseLayer *l = hp_edit_touch(db, t, c->hand, HPL_POLE, def);
        if (!l) return false;
        l->pole = np;
        return true;
    }
    case HPK_FINGER: {
        if (!hp_target_has_layers(db, t) || !j0) return false;
        // Rotation from the start frame to the new one, decomposed on the joint's axes.
        quat dq = quat_dot_fix(quat_identity(), quat_norm(quat_mul(world.r, quat_conj(start.r))));
        float sn = sqrtf(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
        float ang = 2.0f * atan2f(sn, dq.w);
        vec3 axis = sn > 1e-7f ? v3(dq.x / sn, dq.y / sn, dq.z / sn) : v3(0, 0, 0);
        vec3 ax[3];
        hp_rig_finger_axes(ctx, c->hand, c->finger, c->joint, ax);
        HandJoint v = *j0;
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = ang * v3_dot(axis, ax[i]) * DG_RAD2DEG;
        if (!(locks & 0x08)) v.curl += d[0];
        if (!(locks & 0x10)) v.spread += d[1];
        if (!(locks & 0x20)) v.twist += d[2];
        hp_edit_set_joint(db, t, c->hand, c->finger, c->joint, v);
        return true;
    }
    }
    return false;
}

// ------------------------------------------------------------------ local values (N-panel)

HpLocal hp_rig_local(const HandPoseDB *db, HpTarget t, int ctrl, const HpEditDefaults *def) {
    HpLocal o = { v3(0, 0, 0), v3(0, 0, 0), quat_identity(), 1.0f };
    const HpCtrl *c = hp_rig_ctrl(ctrl);
    if (!c || !hp_target_valid(db, t)) return o;
    switch (c->kind) {
    case HPK_ROOT: {
        Xform r = hp_target_root(db, t);
        o.loc = r.t;
        o.rot = r.r;
        o.scale = r.s.x > 0 ? r.s.x : 1.0f;
        break;
    }
    case HPK_IK:
        if (hp_target_has_layers(db, t)) {
            Xform s = resolved_sock(db, t, c->hand, def);
            o.loc = s.t;
            o.rot = s.r;
        }
        break;
    case HPK_POLE:
        if (hp_target_has_layers(db, t)) o.loc = resolved_pole(db, t, c->hand, def);
        break;
    case HPK_FINGER:
        if (hp_target_has_layers(db, t)) {
            HandPose p;
            hp_target_resolve(db, t, c->hand, &p);
            HandJoint jt = p.j[c->finger][c->joint];
            o.rot_deg = v3(jt.curl, jt.spread, jt.twist);
            return o;
        }
        break;
    }
    o.rot_deg = handpose_quat_to_euler_deg(o.rot);
    return o;
}

bool hp_rig_set_local(HandPoseDB *db, HpTarget t, int ctrl, const HpEditDefaults *def, HpLocal v) {
    const HpCtrl *c = hp_rig_ctrl(ctrl);
    if (!c || !hp_target_valid(db, t)) return false;
    switch (c->kind) {
    case HPK_ROOT: {
        Xform r = xf_make(v.loc, v.rot);
        r.s = v3s(v.scale);
        hp_target_set_root(db, t, r);
        return true;
    }
    case HPK_IK: return hp_target_has_layers(db, t) && set_socket(db, t, c->hand, def, xf_make(v.loc, v.rot)) != NULL;
    case HPK_POLE: {
        HandPoseLayer *l = hp_target_has_layers(db, t) ? hp_edit_touch(db, t, c->hand, HPL_POLE, def) : NULL;
        if (l) l->pole = v.loc;
        return l != NULL;
    }
    case HPK_FINGER:
        if (!hp_target_has_layers(db, t)) return false;
        hp_edit_set_joint(db, t, c->hand, c->finger, c->joint, (HandJoint){ v.rot_deg.x, v.rot_deg.y, v.rot_deg.z });
        return true;
    }
    return false;
}

// The joint value the target inherits (its chain without its own layer).
static HandJoint inherited_joint(const HandPoseDB *db, HpTarget t, int h, int f, int j) {
    HandPose p;
    int parent = t.entry >= 0 ? db->e[t.entry].parent : db->c[t.ci].parent;
    handpose_resolve(db, parent, h, &p);
    return p.j[f][j];
}

bool hp_rig_clear(HandPoseDB *db, HpTarget t, int ctrl, int channel) {
    const HpCtrl *c = hp_rig_ctrl(ctrl);
    if (!c || !hp_target_valid(db, t)) return false;
    switch (c->kind) {
    case HPK_ROOT: {
        Xform r = hp_target_root(db, t);
        if (channel & HPA_T) r.t = v3(0, 0, 0);
        if (channel & HPA_R) r.r = quat_identity();
        if (channel & HPA_S) r.s = v3(1, 1, 1);
        if (r.t.x == 0 && r.t.y == 0 && r.t.z == 0 && r.r.w == 1 && r.s.x == 1) return hp_edit_reset_root(db, t) || true;
        hp_target_set_root(db, t, r);
        return true;
    }
    case HPK_IK: return hp_edit_reset(db, t, c->hand, HPL_SOCKET) | hp_edit_reset(db, t, c->hand, HPL_WRIST);
    case HPK_POLE: return hp_edit_reset(db, t, c->hand, HPL_POLE);
    case HPK_FINGER:
        if (!(channel & HPA_R) || !hp_field_overridden(db, t, c->hand, (u16)HPL_FINGER(c->finger))) return false;
        hp_edit_set_joint(db, t, c->hand, c->finger, c->joint, inherited_joint(db, t, c->hand, c->finger, c->joint));
        return true;
    }
    return false;
}
