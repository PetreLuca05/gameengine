// Hand pose control rig + transform tool (src/editor/handpose/hp_rig.c, hp_xform.c,
// docs/HANDPOSE.md "Control rig"): the hierarchy (Root -> IK / poles -> fingers), moving the root
// moves both hands' IK targets by the same transform (grips and case keys), the root's scale
// scales the hands' offsets, fingers only rotate, freely on every axis (curl / spread / twist far
// past the old joint limits, gizmo and typed values; kept within +-180), nothing but the root
// moves the item, mirror, undo / redo round trips every rig operation, and the modal
// transforms (constraints, typed values, snapping, pivots, cancel, view-plane grab).
#include "editor/handpose/hp_rig.h"
#include "editor/handpose/hp_xform.h"
#include "core/engine.h"
#include "core/log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;

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

static bool xf_near(Xform a, Xform b, float ep, float er) { return v3_dist(a.t, b.t) <= ep && quat_angle_between(a.r, b.r) <= er; }

// A grip (item space sockets on both hands, fingers) inheriting a style, and a door case.
static void build(HandPoseDB *db) {
    handpose_db_init(db);
    int s = handpose_add(db, "style/rifle", HPC_STYLE);
    for (int h = 0; h < 2; h++) {
        handpose_preset("relaxed", db->e[s].grip[h].j);
        db->e[s].grip[h].set = HPL_FINGERS_ALL;
    }
    int r = handpose_add(db, "rifle", HPC_WEAPON);
    dg_strcpy(db->e[r].inherits, sizeof(db->e[r].inherits), "style/rifle");
    db->e[r].hands = 3;
    db->e[r].grip[1].set = HPL_SOCKET;
    db->e[r].grip[1].socket = xf_make(v3(0, 0, 0), quat_identity());
    db->e[r].grip[0].set = HPL_SOCKET | HPL_POLE;
    db->e[r].grip[0].socket = xf_make(v3(-0.01f, 0.02f, -0.30f), quat_axis_angle(v3(0, 0, 1), 0.4f));
    db->e[r].grip[0].pole = v3(-0.7f, -1.0f, 0.2f);
    int c = handpose_case_add(db, "door_knob", HCK_INTERACT);
    dg_strcpy(db->c[c].bind, sizeof(db->c[c].bind), "interactable:door");
    db->c[c].bind_hash = dg_hash_str(db->c[c].bind);
    dg_strcpy(db->c[c].inherits, sizeof(db->c[c].inherits), "style/rifle");
    db->c[c].hands = 3;
    handpose_link(db);
    handpose_case_key_at(db, c, db->c[c].contact);
    db->c[c].keys[0].hand[0].socket = xf_make(v3(-0.12f, 0.0f, 0.0f), quat_identity());
    db->c[c].keys[0].hand[1].socket = xf_make(v3(0.03f, 0.01f, 0.0f), quat_axis_angle(v3(1, 0, 0), 0.3f));
    handpose_rehash(db);
}

// A plausible context: the hold frame / case base somewhere in the world, a hand of finger bones
// along -Z of the palm, the curl axis x.
static void make_ctx(HpRigCtx *ctx, bool is_case) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->root_parent = xf_make(v3(1.0f, 1.4f, -2.0f), quat_norm(quat_mul(quat_yaw(0.7f), quat_axis_angle(v3(1, 0, 0), -0.2f))));
    ctx->hand_base[0] = ctx->hand_base[1] = ctx->root_parent;
    ctx->anchor_applied = true;
    ctx->is_case = is_case;
    ctx->cam_rot = quat_yaw(0.7f);
    ctx->pole_len = 0.25f;
    for (int h = 0; h < 2; h++) {
        ctx->grip_space[h] = xf_identity();
        ctx->palm_frame[h] = xf_make(v3(h ? 0.2f : -0.2f, 1.2f, -2.2f), quat_yaw(0.7f));
        ctx->curl_axis[h] = v3(1, 0, 0);
        ctx->thumb_side[h] = h ? -1.0f : 1.0f;
        ctx->shoulder[h] = v3(h ? 0.2f : -0.2f, 1.45f, -1.9f);
        ctx->elbow[h] = v3(h ? 0.3f : -0.3f, 1.2f, -2.0f);
        ctx->wrist[h] = ctx->palm_frame[h].t;
        for (int f = 0; f < HAND_FINGERS; f++) {
            for (int j = 0; j < HAND_FINGER_JOINTS; j++)
                ctx->finger_bone[h][f][j] = xf_make(xf_point(ctx->palm_frame[h], v3(0.02f * (float)(f - 2), 0, -0.05f - 0.025f * (float)j)), ctx->palm_frame[h].r);
            ctx->finger_tip[h][f] = xf_point(ctx->palm_frame[h], v3(0.02f * (float)(f - 2), 0, -0.13f));
        }
    }
}

static void test_hierarchy(void) {
    CHECK(hp_rig_ctrl(HP_RIG_ROOT)->kind == HPK_ROOT && hp_rig_parent(HP_RIG_ROOT) < 0, "root");
    for (int h = 0; h < 2; h++) {
        CHECK(hp_rig_parent(HP_RIG_IK(h)) == HP_RIG_ROOT && hp_rig_parent(HP_RIG_POLE(h)) == HP_RIG_ROOT, "IK / pole parent = root");
        CHECK(hp_rig_ctrl(HP_RIG_IK(h))->allow == (HPA_T | HPA_R) && hp_rig_ctrl(HP_RIG_POLE(h))->allow == HPA_T, "IK: T+R, pole: T");
        for (int f = 0; f < HAND_FINGERS; f++)
            for (int j = 0; j < HAND_FINGER_JOINTS; j++) {
                int i = HP_RIG_FINGER(h, f, j);
                CHECK(hp_rig_ctrl(i)->allow == HPA_R, "fingers rotate only");
                CHECK(hp_rig_is_ancestor(HP_RIG_IK(h), i) && hp_rig_is_ancestor(HP_RIG_ROOT, i) && !hp_rig_is_ancestor(HP_RIG_IK(!h), i), "finger %s under its hand", hp_rig_ctrl(i)->name);
                CHECK(hp_rig_mirror(i) == HP_RIG_FINGER(!h, f, j) && hp_rig_mirror(hp_rig_mirror(i)) == i, "finger mirror");
            }
        CHECK(hp_rig_mirror(HP_RIG_IK(h)) == HP_RIG_IK(!h) && hp_rig_mirror(HP_RIG_POLE(h)) == HP_RIG_POLE(!h), "IK / pole mirror");
    }
    CHECK(hp_rig_find("IK_hand_R") == HP_RIG_IK(1) && hp_rig_find("L_index_2") == HP_RIG_FINGER(0, HF_INDEX, 1) && hp_rig_find("Pole_L") == HP_RIG_POLE(0), "names");
    // The item mesh is not a control: only the root scales, nothing named after the item.
    int scalers = 0;
    for (int i = 0; i < HP_RIG_CTRLS; i++) scalers += (hp_rig_ctrl(i)->allow & HPA_S) != 0;
    CHECK(scalers == 1 && hp_rig_find("Item") < 0 && hp_rig_find("item") < 0, "the item is not a control (%d scaling controls)", scalers);
    int out[HP_RIG_CTRLS], n = hp_rig_siblings(HP_RIG_IK(0), out, HP_RIG_CTRLS);
    CHECK(n == 3, "IK_L siblings: IK_R + 2 poles (%d)", n);
    n = hp_rig_children(HP_RIG_IK(1), out, HP_RIG_CTRLS);
    CHECK(n == HAND_FINGERS, "IK_R children: the five first joints (%d)", n);
    bool sel[HP_RIG_CTRLS] = { 0 };
    sel[HP_RIG_ROOT] = sel[HP_RIG_IK(0)] = sel[HP_RIG_FINGER(1, 2, 1)] = true;
    n = hp_rig_top_level(sel, out, HP_RIG_CTRLS);
    CHECK(n == 1 && out[0] == HP_RIG_ROOT, "the root carries the selected children (%d)", n);
    memset(sel, 0, sizeof(sel));
    hp_rig_select_hand(sel, 1, true);
    n = hp_rig_top_level(sel, out, HP_RIG_CTRLS);
    CHECK(n == 1 && out[0] == HP_RIG_IK(1), "a whole hand moves through its IK control");
}

// Moving the root by T moves both IK targets (and the item) by T: grips and case keys.
static void test_root_carries(void) {
    HandPoseDB db;
    build(&db);
    HpRigCtx ctx;
    HpEditDefaults def;
    for (int h = 0; h < 2; h++) { def.socket[h] = xf_identity(); def.pole[h] = v3(h ? 0.7f : -0.7f, -1, 0); }
    HpTarget targets[2] = { hp_target_grip(handpose_find(&db, "rifle")), hp_target_case(handpose_case_find(&db, "door_knob"), 0) };
    Xform T = xf_make(v3(0.03f, -0.02f, 0.05f), quat_norm(quat_mul(quat_axis_angle(v3(0, 1, 0), 0.35f), quat_axis_angle(v3(1, 0, 0), -0.2f))));
    for (int k = 0; k < 2; k++) {
        make_ctx(&ctx, k == 1);
        Xform before[HP_RIG_CTRLS], after[HP_RIG_CTRLS];
        hp_rig_world(&db, targets[k], &ctx, &def, before);
        // The new root world = T * old (T applied in world space about the origin).
        Xform nr = xf_mul(T, before[HP_RIG_ROOT]);
        nr.s = before[HP_RIG_ROOT].s;
        CHECK(hp_rig_apply(&db, targets[k], &ctx, &def, HP_RIG_ROOT, before[HP_RIG_ROOT], nr, NULL, 0), "root apply");
        hp_rig_world(&db, targets[k], &ctx, &def, after);
        for (int h = 0; h < 2; h++) {
            Xform want = xf_mul(T, before[HP_RIG_IK(h)]);
            CHECK(xf_near(after[HP_RIG_IK(h)], want, 1e-5f, 1e-4f), "%s: IK_%s not carried by the root (%.6f m off)", k ? "case" : "grip", h ? "R" : "L",
                  v3_dist(after[HP_RIG_IK(h)].t, want.t));
        }
        // The hands' offsets from the root are unchanged in the root's frame.
        for (int h = 0; h < 2; h++) {
            Xform rb = xf_mul(xf_inverse(xf_make(before[HP_RIG_ROOT].t, before[HP_RIG_ROOT].r)), before[HP_RIG_IK(h)]);
            Xform ra = xf_mul(xf_inverse(xf_make(after[HP_RIG_ROOT].t, after[HP_RIG_ROOT].r)), after[HP_RIG_IK(h)]);
            CHECK(xf_near(ra, rb, 1e-5f, 1e-4f), "IK_%s relative to the root changed", h ? "R" : "L");
        }
        // Moving an IK control never moves the root (the item) or the other hand.
        Xform ik = after[HP_RIG_IK(0)];
        ik.t = v3_add(ik.t, v3(0.01f, 0.0f, -0.02f));
        hp_rig_apply(&db, targets[k], &ctx, &def, HP_RIG_IK(0), after[HP_RIG_IK(0)], ik, NULL, 0);
        Xform again[HP_RIG_CTRLS];
        hp_rig_world(&db, targets[k], &ctx, &def, again);
        CHECK(xf_near(again[HP_RIG_ROOT], after[HP_RIG_ROOT], 1e-6f, 1e-6f) && xf_near(again[HP_RIG_IK(1)], after[HP_RIG_IK(1)], 1e-6f, 1e-6f), "an IK move moved the root / the other hand");
        CHECK(v3_dist(again[HP_RIG_IK(0)].t, ik.t) < 1e-5f, "IK_L didn't land where it was put (%.6f)", v3_dist(again[HP_RIG_IK(0)].t, ik.t));
    }
    // Scale (grips): the hands' offsets from the root scale with it; clamped.
    make_ctx(&ctx, false);
    Xform w0[HP_RIG_CTRLS], w1[HP_RIG_CTRLS];
    hp_rig_world(&db, targets[0], &ctx, &def, w0);
    Xform sr = w0[HP_RIG_ROOT];
    sr.s = v3s(1.5f);
    hp_rig_apply(&db, targets[0], &ctx, &def, HP_RIG_ROOT, w0[HP_RIG_ROOT], sr, NULL, 0);
    hp_rig_world(&db, targets[0], &ctx, &def, w1);
    float d0 = v3_dist(w0[HP_RIG_IK(0)].t, w0[HP_RIG_ROOT].t), d1 = v3_dist(w1[HP_RIG_IK(0)].t, w1[HP_RIG_ROOT].t);
    CHECK(fabsf(d1 - 1.5f * d0) < 1e-5f, "root scale: offset %.4f -> %.4f (want x1.5)", d0, d1);
    sr.s = v3s(9.0f);
    hp_rig_apply(&db, targets[0], &ctx, &def, HP_RIG_ROOT, w0[HP_RIG_ROOT], sr, NULL, 0);
    CHECK(handpose_root(&db, targets[0].entry).s.x == HANDPOSE_ROOT_SCALE_MAX, "scale clamped");
    // Case anchors don't scale.
    make_ctx(&ctx, true);
    hp_rig_world(&db, targets[1], &ctx, &def, w0);
    sr = w0[HP_RIG_ROOT];
    sr.s = v3s(1.5f);
    hp_rig_apply(&db, targets[1], &ctx, &def, HP_RIG_ROOT, w0[HP_RIG_ROOT], sr, NULL, 0);
    CHECK(db.c[targets[1].ci].anchor.s.x == 1.0f, "a case anchor took a scale");
    handpose_db_free(&db);
}

static void test_fingers(void) {
    HandPoseDB db;
    build(&db);
    HpRigCtx ctx;
    make_ctx(&ctx, false);
    HpEditDefaults def;
    for (int h = 0; h < 2; h++) { def.socket[h] = xf_identity(); def.pole[h] = v3(0, -1, 0); }
    HpTarget t = hp_target_grip(handpose_find(&db, "rifle"));
    Xform w[HP_RIG_CTRLS];
    hp_rig_world(&db, t, &ctx, &def, w);
    int fi = HP_RIG_FINGER(1, HF_INDEX, 1);
    HpLocal l0 = hp_rig_local(&db, t, fi, &def);
    HandJoint j0 = { l0.rot_deg.x, l0.rot_deg.y, l0.rot_deg.z };
    // +20 deg about the joint's curl axis -> +20 curl, nothing else.
    vec3 ax[3];
    hp_rig_finger_axes(&ctx, 1, HF_INDEX, 1, ax);
    Xform nw = w[fi];
    nw.r = quat_norm(quat_mul(quat_axis_angle(ax[0], 20.0f * DG_DEG2RAD), w[fi].r));
    hp_rig_apply(&db, t, &ctx, &def, fi, w[fi], nw, &j0, 0);
    HpLocal l1 = hp_rig_local(&db, t, fi, &def);
    CHECK(fabsf(l1.rot_deg.x - (j0.curl + 20.0f)) < 0.01f && fabsf(l1.rot_deg.y - j0.spread) < 0.01f && fabsf(l1.rot_deg.z - j0.twist) < 0.01f,
          "curl rotation: %.2f %.2f %.2f (from %.2f)", l1.rot_deg.x, l1.rot_deg.y, l1.rot_deg.z, j0.curl);
    // Spread / twist axes: free, far past the old limits (PIP spread 5, twist 10).
    nw.r = quat_norm(quat_mul(quat_axis_angle(ax[1], 40.0f * DG_DEG2RAD), w[fi].r));
    hp_rig_apply(&db, t, &ctx, &def, fi, w[fi], nw, &j0, 0);
    l1 = hp_rig_local(&db, t, fi, &def);
    CHECK(fabsf(l1.rot_deg.y - (j0.spread + 40.0f)) < 0.01f && fabsf(l1.rot_deg.x - j0.curl) < 0.01f && fabsf(l1.rot_deg.z - j0.twist) < 0.01f, "spread rotation (%.2f)", l1.rot_deg.y);
    nw.r = quat_norm(quat_mul(quat_axis_angle(ax[2], -70.0f * DG_DEG2RAD), w[fi].r));
    hp_rig_apply(&db, t, &ctx, &def, fi, w[fi], nw, &j0, 0);
    l1 = hp_rig_local(&db, t, fi, &def);
    CHECK(fabsf(l1.rot_deg.z - (j0.twist - 70.0f)) < 0.01f && fabsf(l1.rot_deg.x - j0.curl) < 0.01f && fabsf(l1.rot_deg.y - j0.spread) < 0.01f, "twist rotation (%.2f)", l1.rot_deg.z);
    // A large curl (old limit 110): taken as is.
    nw.r = quat_norm(quat_mul(quat_axis_angle(ax[0], 150.0f * DG_DEG2RAD), w[fi].r));
    hp_rig_apply(&db, t, &ctx, &def, fi, w[fi], nw, &j0, 0);
    l1 = hp_rig_local(&db, t, fi, &def);
    CHECK(fabsf(l1.rot_deg.x - (j0.curl + 150.0f)) < 0.01f, "large curl rotation clamped (%.2f, from %.2f)", l1.rot_deg.x, j0.curl);
    // Typed values: any value within a full turn is taken exactly, beyond it kept at +-180.
    for (int f = 0; f < HAND_FINGERS; f++)
        for (int j = 0; j < HAND_FINGER_JOINTS; j++) {
            HpLocal v = { v3(0, 0, 0), v3(150, -120, 170), quat_identity(), 1 };
            hp_rig_set_local(&db, t, HP_RIG_FINGER(0, f, j), &def, v);
            HpLocal b = hp_rig_local(&db, t, HP_RIG_FINGER(0, f, j), &def);
            CHECK(!memcmp(&b.rot_deg, &v.rot_deg, sizeof(vec3)), "typed finger values clamped (%s %d: %.1f %.1f %.1f)", handpose_finger_name(f), j, b.rot_deg.x, b.rot_deg.y, b.rot_deg.z);
            v.rot_deg = v3(999, -999, 999);
            hp_rig_set_local(&db, t, HP_RIG_FINGER(0, f, j), &def, v);
            b = hp_rig_local(&db, t, HP_RIG_FINGER(0, f, j), &def);
            CHECK(b.rot_deg.x == 180 && b.rot_deg.y == -180 && b.rot_deg.z == 180, "typed finger values not kept within +-180 (%s %d)", handpose_finger_name(f), j);
        }
    // Fingers don't translate: a moved (not rotated) finger frame changes nothing.
    HpLocal before = hp_rig_local(&db, t, fi, &def);
    HandJoint jb = { before.rot_deg.x, before.rot_deg.y, before.rot_deg.z };
    hp_rig_world(&db, t, &ctx, &def, w);
    nw = w[fi];
    nw.t = v3_add(nw.t, v3(0.1f, 0.2f, 0.3f));
    hp_rig_apply(&db, t, &ctx, &def, fi, w[fi], nw, &jb, 0);
    HpLocal after = hp_rig_local(&db, t, fi, &def);
    CHECK(!memcmp(&before.rot_deg, &after.rot_deg, sizeof(vec3)), "a finger translation changed the joint");
    // Locks: a locked curl keeps its value.
    nw = w[fi];
    nw.r = quat_norm(quat_mul(quat_axis_angle(ax[0], 10.0f * DG_DEG2RAD), w[fi].r));
    hp_rig_apply(&db, t, &ctx, &def, fi, w[fi], nw, &jb, 0x08);
    after = hp_rig_local(&db, t, fi, &def);
    CHECK(after.rot_deg.x == before.rot_deg.x, "locked curl moved");
    // The curl shortcut drives the three joints of a finger.
    hp_edit_set_curl(&db, t, 1, HF_MIDDLE, 60.0f);
    HandPose p;
    handpose_resolve(&db, t.entry, 1, &p);
    CHECK(p.j[HF_MIDDLE][0].curl > 0 && p.j[HF_MIDDLE][1].curl > p.j[HF_MIDDLE][0].curl && fabsf(handpose_finger_curl(p.j, HF_MIDDLE) - 60.0f) < 1e-3f, "curl slider");
    handpose_db_free(&db);
}

// Mirror: the left grip becomes the right one reflected (x negated in item space).
static void test_mirror(void) {
    HandPoseDB db;
    build(&db);
    HpRigCtx ctx;
    make_ctx(&ctx, false);
    HpEditDefaults def;
    for (int h = 0; h < 2; h++) { def.socket[h] = xf_identity(); def.pole[h] = v3(h ? 0.7f : -0.7f, -1, 0); }
    HpTarget t = hp_target_grip(handpose_find(&db, "rifle"));
    hp_edit_set_curl(&db, t, 0, HF_RING, 55.0f);
    hp_edit_mirror(&db, t, 0);
    const HandPoseEntry *e = &db.e[t.entry];
    Xform m = handpose_mirror_xform(e->grip[0].socket);
    CHECK(!memcmp(&e->grip[1].socket, &m, sizeof(Xform)) && e->grip[1].pole.x == -e->grip[0].pole.x && !memcmp(e->grip[1].j, e->grip[0].j, sizeof(HandFingers)),
          "mirror L -> R");
    // In the item's frame the two IK targets are reflections of each other.
    Xform w[HP_RIG_CTRLS];
    hp_rig_world(&db, t, &ctx, &def, w);
    Xform ir = xf_inverse(xf_make(w[HP_RIG_ROOT].t, w[HP_RIG_ROOT].r));
    vec3 pl = xf_point(ir, w[HP_RIG_IK(0)].t), pr = xf_point(ir, w[HP_RIG_IK(1)].t);
    CHECK(fabsf(pl.x + pr.x) < 1e-5f && fabsf(pl.y - pr.y) < 1e-5f && fabsf(pl.z - pr.z) < 1e-5f, "mirrored IK targets (%.4f %.4f)", pl.x, pr.x);
    handpose_db_free(&db);
}

// Undo / redo round trips every rig operation (the canonical text before / after).
static char *text_of(const HandPoseDB *db, HpTarget t) { return t.entry >= 0 ? handpose_entry_write(&db->e[t.entry]) : handpose_case_write(&db->c[t.ci]); }

static void test_undo_all(void) {
    HandPoseDB db;
    build(&db);
    HpRigCtx ctx;
    HpEditDefaults def;
    for (int h = 0; h < 2; h++) { def.socket[h] = xf_identity(); def.pole[h] = v3(h ? 0.7f : -0.7f, -1, 0); }
    HpUndo u;
    memset(&u, 0, sizeof(u));
    int ops = 0, bad = 0;
    for (int k = 0; k < 2; k++) {
        HpTarget t = k == 0 ? hp_target_grip(handpose_find(&db, "rifle")) : hp_target_case(handpose_case_find(&db, "door_knob"), 0);
        make_ctx(&ctx, k == 1);
        for (int op = 0; op < 14; op++) {
            Xform w[HP_RIG_CTRLS];
            hp_rig_world(&db, t, &ctx, &def, w);
            char *before = text_of(&db, t);
            hp_undo_push(&u, &db, t, 0);
            Xform nw;
            HpLocal l;
            HandJoint j0 = { 10, 0, 0 };
            switch (op) {
            case 0: nw = w[HP_RIG_ROOT]; nw.t.y += 0.02f; hp_rig_apply(&db, t, &ctx, &def, HP_RIG_ROOT, w[HP_RIG_ROOT], nw, NULL, 0); break;
            case 1: nw = w[HP_RIG_ROOT]; nw.r = quat_mul(quat_yaw(0.2f), nw.r); hp_rig_apply(&db, t, &ctx, &def, HP_RIG_ROOT, w[HP_RIG_ROOT], nw, NULL, 0); break;
            case 2: nw = w[HP_RIG_IK(1)]; nw.t.x += 0.01f; hp_rig_apply(&db, t, &ctx, &def, HP_RIG_IK(1), w[HP_RIG_IK(1)], nw, NULL, 0); break;
            case 3: nw = w[HP_RIG_IK(0)]; nw.r = quat_mul(quat_axis_angle(v3(0, 0, 1), 0.3f), nw.r); hp_rig_apply(&db, t, &ctx, &def, HP_RIG_IK(0), w[HP_RIG_IK(0)], nw, NULL, 0); break;
            case 4: nw = w[HP_RIG_POLE(0)]; nw.t.y -= 0.05f; hp_rig_apply(&db, t, &ctx, &def, HP_RIG_POLE(0), w[HP_RIG_POLE(0)], nw, NULL, 0); break;
            case 5: nw = w[HP_RIG_FINGER(1, 1, 0)]; nw.r = quat_mul(quat_axis_angle(v3(1, 0, 0), 0.2f), nw.r); hp_rig_apply(&db, t, &ctx, &def, HP_RIG_FINGER(1, 1, 0), w[HP_RIG_FINGER(1, 1, 0)], nw, &j0, 0); break;
            case 6: hp_edit_set_curl(&db, t, 1, HF_PINKY, 70); break;
            case 7: hp_edit_preset(&db, t, 0, "fist"); break;
            case 8: hp_edit_mirror(&db, t, 0); break;
            case 9: hp_rig_clear(&db, t, HP_RIG_IK(1), HPA_T); break;
            case 10: l = hp_rig_local(&db, t, HP_RIG_IK(0), &def); l.loc.z -= 0.03f; hp_rig_set_local(&db, t, HP_RIG_IK(0), &def, l); break;
            case 11: hp_rig_clear(&db, t, HP_RIG_ROOT, HPA_T | HPA_R | HPA_S); break;
            case 12: if (t.ci >= 0) handpose_case_key_at(&db, t.ci, db.c[t.ci].release); else hp_edit_set_joint(&db, t, 1, HF_INDEX, 2, (HandJoint){ 150, -60, 90 }); break;
            case 13: if (t.ci >= 0) handpose_case_key_remove(&db.c[t.ci], db.c[t.ci].key_count - 1); else hp_edit_reset(&db, t, 1, HPL_FINGER(HF_INDEX)); break;
            }
            char *after = text_of(&db, t);
            HpTarget r = hp_undo(&u, &db);
            char *undone = text_of(&db, t);
            hp_redo(&u, &db);
            char *redone = text_of(&db, t);
            bool ok = (r.entry >= 0 || r.ci >= 0) && !strcmp(before, undone) && !strcmp(after, redone);
            if (!ok) { bad++; printf("  undo / redo op %d on the %s lossy\n", op, k ? "case" : "grip"); }
            ops++;
            free(before); free(after); free(undone); free(redone);
        }
    }
    CHECK(bad == 0, "undo / redo round trip: %d of %d operations lossy", bad, ops);
    printf("  rig undo / redo: %d operations round trip (grip + case)\n", ops);
    hp_undo_free(&u);
    handpose_db_free(&db);
}

// ------------------------------------------------------------------ transform tool

// A simple pinhole camera at (0, 0, 2) looking down -Z, 1000 px focal length, 800x600.
static bool proj(void *ctx, vec3 p, vec2 *px) {
    (void)ctx;
    float z = 2.0f - p.z;
    if (z <= 0.01f) return false;
    *px = v2(400.0f + 1000.0f * p.x / z, 300.0f - 1000.0f * p.y / z);
    return true;
}

static void test_xform(void) {
    HpxView view = { proj, NULL, v3(0, 0, 2) };
    quat basis[HPX_ORIENT_COUNT] = { quat_identity(), quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), quat_identity(), quat_identity() };
    Xform st[2] = { xf_make(v3(0, 0, 0), quat_identity()), xf_make(v3(0.1f, 0, 0), quat_identity()) };
    int ids[2] = { 7, 9 };
    HpXform x;
    // Grab, Z typed 0.25.
    hpx_begin(&x, HPX_GRAB, v2(400, 300), 2, ids, st, 0, basis, HPX_GLOBAL, HPX_PIVOT_MEDIAN);
    hpx_axis(&x, 2, false);
    for (const char *p = "0.25"; *p; p++) hpx_char(&x, *p);
    hpx_update(&x, v2(400, 300), &view, false);
    CHECK(fabsf(x.out[0].t.z - 0.25f) < 1e-6f && fabsf(x.out[1].t.x - 0.1f) < 1e-6f && fabsf(x.out[1].t.z - 0.25f) < 1e-6f, "grab z typed");
    hpx_char(&x, '-');
    hpx_update(&x, v2(400, 300), &view, false);
    CHECK(fabsf(x.out[0].t.z + 0.25f) < 1e-6f, "typed '-' negates");
    hpx_cancel(&x);
    CHECK(!hpx_active(&x) && !memcmp(&x.out[0], &st[0], sizeof(Xform)), "cancel restores");
    // Free grab: the pivot (the median, at 425 px) follows the mouse on screen (view plane).
    hpx_begin(&x, HPX_GRAB, v2(400, 300), 2, ids, st, 0, basis, HPX_GLOBAL, HPX_PIVOT_MEDIAN);
    hpx_update(&x, v2(450, 280), &view, false);
    vec2 s;
    proj(NULL, v3_add(x.pivot, x.delta), &s);
    CHECK(fabsf(s.x - 475.0f) < 0.5f && fabsf(s.y - 280.0f) < 0.5f && fabsf(x.delta.z) < 0.01f, "view plane grab (%.1f %.1f, dz %.5f)", s.x, s.y, x.delta.z);
    // X constraint: only x moves; snapping rounds to 5 mm.
    hpx_axis(&x, 0, false);
    hpx_update(&x, v2(463, 290), &view, true);
    CHECK(x.delta.y == 0 && x.delta.z == 0 && fabsf(x.delta.x / 0.005f - roundf(x.delta.x / 0.005f)) < 1e-3f, "x constraint + snap (%.5f)", x.delta.x);
    // Shift+Z: the XY plane.
    hpx_axis(&x, 2, true);
    hpx_update(&x, v2(463, 290), &view, false);
    CHECK(fabsf(x.delta.z) < 1e-6f && fabsf(x.delta.x) > 0 && fabsf(x.delta.y) > 0, "plane constraint");
    // Second press of the same axis: the other orientation (local: rotated 90 deg about z).
    hpx_axis(&x, 0, false);
    hpx_axis(&x, 0, false);
    CHECK(x.orient == HPX_LOCAL && v3_dist(hpx_axis_world(&x, 0), v3(0, 1, 0)) < 1e-5f, "x x: local axis");
    hpx_axis(&x, 0, false);
    CHECK(x.axis == -1, "third press: free");
    hpx_end(&x);
    // Precision (Shift held): the motion while it is on counts a tenth, no jump when it ends;
    // with Ctrl the snap is 1 mm.
    hpx_begin(&x, HPX_GRAB, v2(400, 300), 2, ids, st, 0, basis, HPX_GLOBAL, HPX_PIVOT_MEDIAN);
    hpx_axis(&x, 0, false);
    hpx_update(&x, v2(500, 300), &view, false);
    float full = x.delta.x;
    x.precise = true;
    hpx_update(&x, v2(600, 300), &view, false);
    CHECK(full > 0.1f && fabsf(x.delta.x - full * 1.1f) < 1e-4f, "precision: a tenth (%.5f, %.5f)", x.delta.x, full);
    x.precise = false;
    hpx_update(&x, v2(600, 300), &view, false);
    CHECK(fabsf(x.delta.x - full * 1.1f) < 1e-4f, "precision released: no jump (%.5f)", x.delta.x);
    x.precise = true;
    hpx_update(&x, v2(613, 300), &view, true);
    CHECK(fabsf(x.delta.x / 0.001f - roundf(x.delta.x / 0.001f)) < 1e-3f && fabsf(x.delta.x / 0.005f - roundf(x.delta.x / 0.005f)) > 1e-3f, "Ctrl+Shift snaps 1 mm (%.5f)", x.delta.x);
    hpx_end(&x);
    // Rotate z 90 typed about the median pivot vs individual origins.
    hpx_begin(&x, HPX_ROTATE, v2(400, 300), 2, ids, st, 0, basis, HPX_GLOBAL, HPX_PIVOT_MEDIAN);
    hpx_axis(&x, 2, false);
    for (const char *p = "90"; *p; p++) hpx_char(&x, *p);
    hpx_update(&x, v2(400, 300), &view, false);
    vec3 piv = v3(0.05f, 0, 0);
    vec3 want1 = v3_add(piv, quat_rotate(quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f), v3_sub(st[1].t, piv)));
    CHECK(v3_dist(x.out[1].t, want1) < 1e-5f && quat_angle_between(x.out[1].r, quat_axis_angle(v3(0, 0, 1), DG_PI * 0.5f)) < 1e-4f, "rotate about the median");
    hpx_end(&x);
    hpx_begin(&x, HPX_ROTATE, v2(400, 300), 2, ids, st, 0, basis, HPX_GLOBAL, HPX_PIVOT_INDIVIDUAL);
    hpx_axis(&x, 2, false);
    for (const char *p = "90"; *p; p++) hpx_char(&x, *p);
    hpx_update(&x, v2(400, 300), &view, false);
    CHECK(v3_dist(x.out[1].t, st[1].t) < 1e-6f, "individual origins: positions stay");
    hpx_end(&x);
    // Mouse rotation about the view axis: a quarter turn on screen = 90 deg about the axis to the eye.
    hpx_begin(&x, HPX_ROTATE, v2(500, 300), 1, ids, st, 0, basis, HPX_GLOBAL, HPX_PIVOT_ACTIVE);
    hpx_update(&x, v2(400, 200), &view, false);   // from the right of the pivot to above it: counter-clockwise
    CHECK(fabsf(x.angle_deg - 90.0f) < 0.5f, "screen rotation %.2f deg", x.angle_deg);
    vec3 r = quat_rotate(x.out[0].r, v3(1, 0, 0));
    CHECK(r.y > 0.99f, "counter-clockwise on screen rotates +x towards +y (%.2f %.2f)", r.x, r.y);
    hpx_end(&x);
    // Scale: the distance ratio, snap, uniform.
    hpx_begin(&x, HPX_SCALE, v2(450, 300), 2, ids, st, 0, basis, HPX_GLOBAL, HPX_PIVOT_MEDIAN);
    hpx_update(&x, v2(500, 300), &view, false);   // pivot at (425, 300): 25 -> 75 px
    CHECK(fabsf(x.factor - 3.0f) < 0.01f && fabsf(x.out[0].s.x - 3.0f) < 0.01f, "scale factor %.3f", x.factor);
    hpx_update(&x, v2(461, 300), &view, true);
    CHECK(fabsf(x.factor / 0.05f - roundf(x.factor / 0.05f)) < 1e-3f, "scale snap %.3f", x.factor);
    char b[160];
    hpx_status(&x, b, sizeof(b));
    CHECK(strstr(b, "Scale") != NULL, "status '%s'", b);
    hpx_end(&x);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    test_hierarchy();
    test_root_carries();
    test_fingers();
    test_mirror();
    test_undo_all();
    test_xform();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
