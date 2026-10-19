// Animation system tests (run from the build output dir so assets/ and data/ resolve):
// FBX load, rig validation, pose math, IK, hold styles + hand glue, body-part hitboxes and
// damage multipliers, damage reactions, controller determinism, quantization, lag-comp
// rewind and LOD hysteresis.
#include "gameplay/phone/phone_orient.h"
#include "player/camera_fp.h"
#include "anim/eye_nominal.h"
#include "cJSON.h"
#include "anim/anim.h"
#include "anim/anim_library.h"
#include "anim/character_rt.h"
#include "anim/anim_net.h"
#include "anim/anim_lod.h"
#include "assets/character.h"
#include "assets/fbx_loader.h"
#include "gameplay/hitboxes.h"
#include "gameplay/items.h"
#include "net/bitbuf.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
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

static bool xf_near(Xform a, Xform b, float ep, float eq) {
    return v3_dist(a.t, b.t) <= ep && quat_angle_between(a.r, b.r) <= eq;
}

// ------------------------------------------------------------------ assets

// Every clip faces the canonical forward (-Z): the chest's horizontal forward averaged over the clip
// (frames where it points mostly down, a crawl or a swim stroke, count little) within 50 deg. Clips
// keep their authored facing: a bladed rifle / pistol idle turns the chest up to 40 deg; a rotated
// armature in the source file would be 90 or 180 off.
static void test_clip_facing(void) {
    const Skeleton *s = &L->skel;
    int chest = s->role[BR_SPINE2], hips = s->role[BR_HIPS];
    for (int i = 0; i < L->clip_count; i++) {
        const Clip *c = &L->clips[i];
        vec3 fc = v3(0, 0, 0), fh = v3(0, 0, 0);
        for (int f = 0; f < c->frames; f++) {
            Pose p;
            Xform m[ANIM_MAX_BONES];
            clip_sample_phase(c, c->frames > 1 ? (float)f / (float)(c->frames - 1) : 0.0f, &p);
            pose_to_model(s, &p, m);
            // The chest's front, plus its up (the spine) for a body lying in the clip (crawl, swim):
            // upright the spine adds nothing horizontal, flat it points where the head goes.
            quat dc = quat_mul(m[chest].r, quat_conj(s->bind_model[chest].r));
            fc = v3_add(fc, v3_add(v3_flat(quat_rotate(dc, v3(0, 0, -1))), v3_flat(quat_rotate(dc, v3(0, 1, 0)))));
            fh = v3_add(fh, v3_flat(quat_rotate(quat_mul(m[hips].r, quat_conj(s->bind_model[hips].r)), v3(0, 0, -1))));
        }
        float chest_deg = atan2f(-fc.x, -fc.z) * DG_RAD2DEG, hips_deg = atan2f(-fh.x, -fh.z) * DG_RAD2DEG;
        printf("  facing %-24s hips %+7.1f chest %+7.1f deg (clip average, + = left)\n", c->name, hips_deg, chest_deg);
        CHECK(fabsf(chest_deg) < 50.0f, "clip %s: chest faces %.0f deg off forward", c->name, chest_deg);
    }
}

// The base pose's planted toes over `ticks` ticks of steady input: their mean slide, reversed, is
// the direction the feet carry the body (degrees, 0 forward, + right) and its speed.
static float toe_travel(CharRuntime *rt, const CharInputs *in, int ticks, float *speed) {
    const Skeleton *s = &L->skel;
    int toe[2] = { s->role[BR_L_TOE], s->role[BR_R_TOE] };
    vec3 *tp = (vec3 *)malloc(sizeof(vec3) * 2 * (size_t)ticks);
    float lo[2] = { 1e9f, 1e9f };
    for (int t = 0; t < ticks; t++) {
        charrt_set_inputs(rt, in);
        charrt_tick(rt, 1.0f / 60.0f);
        charrt_eval_base(rt);
        for (int k = 0; k < 2; k++) {
            tp[t * 2 + k] = rt->base_model[toe[k]].t;
            lo[k] = fminf(lo[k], tp[t * 2 + k].y);
        }
    }
    vec3 v = v3(0, 0, 0);
    int n = 0;
    for (int t = ticks / 2; t + 1 < ticks; t++)
        for (int k = 0; k < 2; k++) {
            vec3 a = tp[t * 2 + k], b = tp[(t + 1) * 2 + k];
            if (a.y > lo[k] + 0.03f || b.y > lo[k] + 0.03f) continue;
            v = v3_add(v, v3_scale(v3_flat(v3_sub(b, a)), 60.0f));
            n++;
        }
    free(tp);
    if (n) v = v3_scale(v, 1.0f / (float)n);
    *speed = v3_len(v);
    return atan2f(-v.x, v.z) * DG_RAD2DEG;
}

// 8-way locomotion with diagonal clips (no hip twist): the blend tree gets the move direction
// itself, and the planted feet carry the body that way at the move speed. A direction whose slot
// plays idle (a missing clip: the strafes until they exist) is only reported.
static bool slot_plays_idle(const CharRuntime *rt, const char *name) {
    int slot = ctrl_slot_index(rt->ctrl.def, name);
    return slot < 0 || rt->ctrl.bind->slots[slot].clip < 0 || rt->ctrl.bind->slots[slot].clip == rt->ctrl.bind->fallback_clip;
}

static void test_diagonal_clips(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    static const struct { float x, y; const char *name, *slot; } dirs[] = {
        { 0.707f, 0.707f, "forward-right", "walk_fr" }, { -0.707f, 0.707f, "forward-left", "walk_fl" }, { 0.707f, -0.707f, "back-right", "walk_br" },
        { -0.707f, -0.707f, "back-left", "walk_bl" }, { 0, 1, "forward", "walk_f" }, { 0, -1, "back", "walk_b" }, { 1, 0, "right", "walk_r" }, { -1, 0, "left", "walk_l" },
    };
    for (int k = 0; k < (int)(sizeof(dirs) / sizeof(dirs[0])); k++) {
        charrt_init(rt, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.speed = 3.0f;
        in.move_local = v2(dirs[k].x, dirs[k].y);
        float sp, dir = toe_travel(rt, &in, 240, &sp);
        float move_deg = atan2f(dirs[k].x, dirs[k].y) * DG_RAD2DEG;
        vec2 bd = v2(ctrl_get(&rt->ctrl, AP_MOVE_X), ctrl_get(&rt->ctrl, AP_MOVE_Y));
        float bdeg = atan2f(bd.x, bd.y) * DG_RAD2DEG;
        bool missing = slot_plays_idle(rt, dirs[k].slot);
        printf("  direction %-13s feet travel %+7.1f deg (want %+7.1f) at %.2f m/s, blend dir %+6.1f deg%s\n", dirs[k].name, dir, move_deg, sp, bdeg,
               missing ? "  (no clip: idle)" : "");
        CHECK(fabsf(wrap_angle((bdeg - move_deg) * DG_DEG2RAD)) < 2.0f * DG_DEG2RAD, "%s: blend tree direction %.0f deg, want %.0f", dirs[k].name, bdeg, move_deg);
        if (missing) continue;
        CHECK(fabsf(wrap_angle((dir - move_deg) * DG_DEG2RAD)) < 8.0f * DG_DEG2RAD, "%s: the feet carry the body %.0f deg, want %.0f", dirs[k].name, dir, move_deg);
        // Speed matching: the feet move at the move speed (no sliding), within the clips' estimate.
        CHECK(fabsf(sp - in.speed) < 0.6f, "%s: feet at %.2f m/s, moving at %.2f", dirs[k].name, sp, in.speed);
    }
    free(rt);
}

// Locomotion smoothness: a player-like input sequence (accelerate, diagonals, strafe, back, stop)
// at 60 Hz through the full runtime (controller + post-pass); the per-tick acceleration of key
// bones (model space) must not spike - spikes are pops / jitter.
// Looping clips must wrap seamlessly: the jump from the last to the first frame is no bigger
// than a normal frame step (feet / hands, model space).
static void test_loop_seams(void) {
    const Skeleton *s = &L->skel;
    int bones[3] = { s->role[BR_L_FOOT], s->role[BR_R_HAND], s->role[BR_HIPS] };
    for (int i = 0; i < L->clip_count; i++) {
        const Clip *c = &L->clips[i];
        if (!c->loop || c->frames < 4) continue;
        Pose p;
        Xform m[ANIM_MAX_BONES];
        vec3 pos[4][3];
        float ph[4] = { (float)(c->frames - 3) / (float)(c->frames - 1), (float)(c->frames - 2) / (float)(c->frames - 1), 0.0f, 1.0f / (float)(c->frames - 1) };
        for (int k = 0; k < 4; k++) {
            clip_sample_phase(c, ph[k], &p);
            pose_to_model(s, &p, m);
            for (int b = 0; b < 3; b++) pos[k][b] = m[bones[b]].t;
        }
        float step = 0, seam = 0;
        for (int b = 0; b < 3; b++) {
            step = fmaxf(step, fmaxf(v3_dist(pos[0][b], pos[1][b]), v3_dist(pos[2][b], pos[3][b])));
            seam = fmaxf(seam, v3_dist(pos[1][b], pos[2][b]));
        }
        printf("  seam %-22s frame step %5.1f mm, wrap %5.1f mm\n", c->name, step * 1000, seam * 1000);
        CHECK(seam < step * 2.5f + 0.004f, "clip %s: loop seam %.1f mm (frame step %.1f mm)", c->name, seam * 1000, step * 1000);
    }
}

static void test_locomotion_smooth(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    const Skeleton *s = &L->skel;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    charrt_init(rt, L, def);
    const float dt = 1.0f / 60.0f;
    CharInputs in;
    memset(&in, 0, sizeof(in));
    in.grounded = true;
    vec2 vel = v2(0, 0);
    int bones[4] = { s->role[BR_HIPS], s->role[BR_HEAD], s->role[BR_L_FOOT], s->role[BR_R_HAND] };
    const char *bn[4] = { "hips", "head", "foot_l", "hand_r" };
    vec3 hist[4][3];
    float worst[4] = { 0 }, avg[4] = { 0 };
    int worst_t[4] = { 0 }, n = 0;
    // Strafe phases (3, 7) that play idle (no strafe clip yet) slide still feet: they stay in the
    // run (their blends in and out are checked) but would lower the mean the limits scale with.
    bool idle_strafe = slot_plays_idle(rt, "walk_r") && slot_plays_idle(rt, "walk_l") && slot_plays_idle(rt, "run_r") && slot_plays_idle(rt, "run_l");
    for (int t = 0; t < 900; t++) {
        // Input: wish direction (local x right, y forward), acceleration like the player controller.
        vec2 wish = v2(0, 0);
        int ph = t / 90;
        static const float W[10][2] = { {0,0},{0,1},{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1},{0,0} };
        wish = v2(W[ph][0], W[ph][1]);
        if (getenv("JERK_CONST")) wish = v2((float)atof(getenv("JERK_CONST")), 1.0f);
        if (v2_len(wish) > 0) wish = v2_scale(wish, 4.2f / v2_len(wish));
        vec2 dv = v2_sub(wish, vel);
        float dl = v2_len(dv), acc = 40.0f * dt;
        vel = dl > acc ? v2_add(vel, v2_scale(dv, acc / dl)) : wish;
        float sp = v2_len(vel);
        in.speed = sp;
        in.move_local = sp > 0.05f ? v2_scale(vel, 1.0f / sp) : v2(0, 1);
        charrt_set_inputs(rt, &in);
        charrt_tick(rt, dt);
        charrt_eval_base(rt);
        ProcInput pi;
        charrt_proc_input(rt, &pi, xf_make(v3(0, 0, 0), quat_identity()), dt);
        charrt_post(rt, &pi);
        for (int k = 0; k < 4; k++) {
            hist[k][0] = hist[k][1];
            hist[k][1] = hist[k][2];
            hist[k][2] = rt->final_model[bones[k]].t;
            if (t < 2) continue;
            float a = v3_len(v3_add(v3_sub(hist[k][2], v3_scale(hist[k][1], 2.0f)), hist[k][0]));
            bool still = idle_strafe && (ph == 3 || ph == 7);
            if (!still) avg[k] += a;
            if (a > worst[k]) { worst[k] = a; worst_t[k] = t; }
            if (getenv("JERK_TRACE") && k == 2 && t >= atoi(getenv("JERK_TRACE")) && t < atoi(getenv("JERK_TRACE")) + 20)
                printf("    t%4d foot accel %6.1f mm  speed %.2f bspeed %.2f phase %.3f st %d blend %u/%u dir %.2f,%.2f\n", t, a * 1000, in.speed,
                       rt->blend_speed, rt->ctrl.layer[0].phase, rt->ctrl.layer[0].state, rt->ctrl.layer[0].blend_ticks, rt->ctrl.layer[0].blend_total,
                       rt->blend_dir.x, rt->blend_dir.y);
        }
        if (t >= 2 && !(idle_strafe && (ph == 3 || ph == 7))) n++;
    }
    for (int k = 0; k < 4; k++) {
        avg[k] /= (float)n;
        printf("  smooth %-7s worst accel %6.2f mm/tick^2 (tick %d, phase %d)  mean %5.2f  ratio %.1f\n", bn[k], worst[k] * 1000, worst_t[k], worst_t[k] / 90,
               avg[k] * 1000, worst[k] / fmaxf(avg[k], 1e-6f));
        // Feet have sharp ground contacts in the clips themselves; transition pops are far larger.
        float lim = k == 2 ? 8.0f : 6.0f;
        CHECK(worst[k] < avg[k] * lim + 0.002f, "%s pops: worst %.2f mm/tick^2 vs mean %.2f (tick %d)", bn[k], worst[k] * 1000, avg[k] * 1000, worst_t[k]);
    }
    free(rt);
}

static void test_load(void) {
    CHECK(L->loaded, "library failed to load");
    CHECK(L->skel.count == 65, "skeleton bones %d", L->skel.count);
    CHECK(L->skel.role[BR_HIPS] == 0, "hips is not the root (%d)", L->skel.role[BR_HIPS]);
    for (int r = 0; r < BR_COUNT; r++) CHECK(L->skel.role[r] >= 0, "bone role %s missing", bone_role_name((BoneRole)r));
    CHECK(fabsf(L->skel.height - 1.797f) < 0.02f, "height %.3f (units/axis conversion)", L->skel.height);
    // Forward is -Z: the toes are in front of the feet.
    vec3 toe = L->skel.bind_model[L->skel.role[BR_L_TOE]].t, foot = L->skel.bind_model[L->skel.role[BR_L_FOOT]].t;
    CHECK(toe.z < foot.z, "character does not face -Z (toe z %.3f foot z %.3f)", toe.z, foot.z);
    CHECK(L->clip_count >= 10, "clips %d", L->clip_count);
    for (int i = 0; i < L->clip_count; i++) {
        const Clip *c = &L->clips[i];
        CHECK(c->frames > 2 && c->bones == L->skel.count, "clip %s frames %d bones %d", c->name, c->frames, c->bones);
        bool finite = true;
        for (int k = 0; k < c->frames * c->bones && finite; k++) finite = isfinite(c->data[k].t.x) && isfinite(c->data[k].r.w);
        CHECK(finite, "clip %s has non-finite data", c->name);
    }
    CHECK(anim_lib_find_clip(L, "jog_f") >= 0 && anim_lib_find_clip(L, "hang_idle") >= 0, "expected clips missing");
    CHECK(L->ctrl.layer_count == 3, "controller layers %d", L->ctrl.layer_count);
    CHECK(L->set_count >= 2, "anim sets %d", L->set_count);
    CHECK(L->char_count >= 5, "characters %d", L->char_count);
    CHECK(L->ragdoll.body_count == 17, "ragdoll bodies %d", L->ragdoll.body_count);
    float mass = 0;
    for (int i = 0; i < L->ragdoll.body_count; i++) mass += L->ragdoll.bodies[i].mass;
    CHECK(fabsf(mass - 80.0f) < 0.01f, "ragdoll mass %.2f", mass);
    CHECK(L->meshes[0].data.lod_count == 4, "mesh LODs %d", L->meshes[0].data.lod_count);
    for (int c = 0; c < AC_CATEGORY_COUNT; c++) CHECK(L->manifest.hash[c] != 0, "manifest %s hash 0", asset_category_name(c));
}

// Builds a copy of `src` keeping bones for which keep[i] is true (parents remapped).
static void skel_subset(const Skeleton *src, const bool *keep, Skeleton *out) {
    memset(out, 0, sizeof(*out));
    int map[ANIM_MAX_BONES];
    for (int i = 0; i < src->count; i++) {
        map[i] = -1;
        if (!keep[i]) continue;
        int j = out->count++;
        map[i] = j;
        dg_strcpy(out->names[j], ANIM_NAME_MAX, src->names[i]);
        int p = src->parent[i];
        while (p >= 0 && map[p] < 0) p = src->parent[p];
        out->parent[j] = (i16)(p >= 0 ? map[p] : -1);
        out->bind[j] = src->bind[i];
    }
    skel_finalize(out);
}

static void test_rig_validation(void) {
    RigReport *r = (RigReport *)malloc(sizeof(RigReport));
    Skeleton *s = (Skeleton *)malloc(sizeof(Skeleton));
    // Identical.
    CHECK(rig_validate(&L->skel, &L->skel, 0.05f, r) && r->identical, "identical rig rejected:\n%s", r->text);
    // Subset: no finger bones.
    bool keep[ANIM_MAX_BONES];
    for (int i = 0; i < L->skel.count; i++) keep[i] = strstr(L->skel.names[i], "Hand") == NULL || strstr(L->skel.names[i], "Hand_") != NULL ||
                                                     !strcmp(strrchr(L->skel.names[i], ':') + 1, "LeftHand") || !strcmp(strrchr(L->skel.names[i], ':') + 1, "RightHand");
    skel_subset(&L->skel, keep, s);
    CHECK(rig_validate(&L->skel, s, 0.05f, r) && !r->identical && r->missing == 40, "subset rig: ok %d missing %d\n%s", r->ok, r->missing, r->text);
    // Extra leaf bone (hair) under the head.
    *s = L->skel;
    int head = L->skel.role[BR_HEAD];
    int n = s->count++;
    dg_strcpy(s->names[n], ANIM_NAME_MAX, "mixamorig1:HairTip");
    s->parent[n] = (i16)head;
    s->bind[n] = xf_make(v3(0, 0.1f, -0.05f), quat_identity());
    skel_finalize(s);
    CHECK(rig_validate(&L->skel, s, 0.05f, r) && r->extra == 1, "extra-bone rig: ok %d extra %d\n%s", r->ok, r->extra, r->text);
    // Renamed bone.
    *s = L->skel;
    int fa = L->skel.role[BR_L_FOREARM];
    dg_strcpy(s->names[fa], ANIM_NAME_MAX, "mixamorig1:LeftLowerArm");
    skel_finalize(s);
    CHECK(!rig_validate(&L->skel, s, 0.05f, r) && strstr(r->text, "LeftLowerArm"), "renamed bone not rejected:\n%s", r->text);
    // Proportions: a longer leg warns.
    *s = L->skel;
    int leg = L->skel.role[BR_L_LEG];
    s->bind[leg].t = v3_scale(s->bind[leg].t, 1.2f);
    skel_finalize(s);
    CHECK(rig_validate(&L->skel, s, 0.05f, r) && r->proportion_warnings >= 1, "proportion warning missing");
    free(r);
    free(s);
}

// ------------------------------------------------------------------ poses / IK

static void test_pose_math(void) {
    const Skeleton *s = &L->skel;
    Pose a, b, out;
    clip_sample(&L->clips[anim_lib_find_clip(L, "jog_f")], 0.37f, &a);
    pose_blend(&a, &a, 0.37f, &out);
    CHECK(!memcmp(&a.local, &out.local, sizeof(Xform) * (size_t)s->count), "blend(a, a, t) != a");
    clip_sample(&L->clips[anim_lib_find_clip(L, "sprint")], 0.2f, &b);
    pose_additive(&a, &b, &a, 0.0f, NULL, &out);
    CHECK(!memcmp(&a.local, &out.local, sizeof(Xform) * (size_t)s->count), "zero-weight additive is not identity");
    // add == ref gives the base back (within float noise).
    pose_additive(&a, &b, &b, 1.0f, NULL, &out);
    float err = 0;
    for (int i = 0; i < s->count; i++) err = fmaxf(err, quat_angle_between(a.local[i].r, out.local[i].r));
    CHECK(err < 1e-4f, "additive(add == ref) changed the pose (%f)", err);
    // Mirror twice = identity.
    Pose m1, m2;
    pose_mirror(s, &a, &m1);
    pose_mirror(s, &m1, &m2);
    err = 0;
    for (int i = 0; i < s->count; i++) err = fmaxf(err, quat_angle_between(a.local[i].r, m2.local[i].r));
    CHECK(err < 2e-3f, "mirror(mirror(p)) != p (%f rad)", err);
}

static void test_ik(void) {
    const Skeleton *s = &L->skel;
    int a = s->role[BR_R_ARM], b = s->role[BR_R_FOREARM], c = s->role[BR_R_HAND];
    u32 seed = 12345;
    int reached = 0;
    for (int k = 0; k < 200; k++) {
        Pose p;
        Xform model[ANIM_MAX_BONES];
        pose_bind(s, &p);
        pose_to_model(s, &p, model);
        float la = v3_dist(model[a].t, model[b].t), lb = v3_dist(model[b].t, model[c].t);
        seed = seed * 1664525u + 1013904223u;
        float u = (float)(seed >> 8) / 16777216.0f;
        seed = seed * 1664525u + 1013904223u;
        float v = (float)(seed >> 8) / 16777216.0f;
        seed = seed * 1664525u + 1013904223u;
        float w = (float)(seed >> 8) / 16777216.0f;
        vec3 dir = v3_norm(v3(u * 2 - 1, v * 2 - 1, w * 2 - 1));
        float dist = (la + lb) * (0.25f + 0.7f * w);
        vec3 target = v3_madd(model[a].t, dir, dist);
        quat rot = quat_norm(quat_euler(u * 6, v * 3, w * 6));
        bool ok = anim_ik_two_bone_pose(s, &p, model, a, b, c, target, &rot, v3(0, -1, 0.3f), 1.0f);
        Xform check[ANIM_MAX_BONES];
        pose_to_model(s, &p, check);
        float perr = v3_dist(check[c].t, target), aerr = quat_angle_between(check[c].r, rot);
        CHECK(ok && perr < 1e-4f && aerr < 1e-3f, "IK miss %d: pos %.6f m ang %.5f rad", k, perr, aerr);
        reached += ok;
        // Bone lengths preserved.
        CHECK(fabsf(v3_dist(check[a].t, check[b].t) - la) < 1e-4f && fabsf(v3_dist(check[b].t, check[c].t) - lb) < 1e-4f, "IK stretched the arm");
    }
    CHECK(reached == 200, "IK reached %d / 200", reached);
}

// ------------------------------------------------------------------ hold + glue

typedef struct GlueStats { float max_pos, max_ang, max_item; int ticks; int worst_tick, worst_flags; float worst_pitch, worst_yaw; char worst_what[24]; } GlueStats;

static Xform root_at(vec3 pos, float yaw) { return xf_make(pos, quat_yaw(yaw)); }

// A hand's grip-resolved palm target in item space: the IK target of the item's universal grip
// (assets/handposes/items, in item space, root scale applied) or the item's own grip_r / grip_l
// when the grip leaves it unset. This is what the runtime solves onto, what the editor draws as
// IK_hand_L / IK_hand_R and what its save-time glue check measures (docs/HANDPOSE.md).
static Xform grip_target(const ItemSockets *k, int style, int hand) {
    const HandPoseDB *db = &L->handposes;
    float rs = handpose_root(db, handpose_find_hash(db, k->pose_id[1])).s.x;
    HandHold hh;
    proc_hand_grip(db, proc_hold_style(&L->proc, style), k, hand, rs, &hh);
    return hh.sock;
}

static void glue_step(CharRuntime *rt, const ItemSockets *k, CharInputs *in, float dt, vec3 kick_pos, vec3 kick_rot, GlueStats *g, const char *what,
                      const char *weapon) {
    charrt_set_inputs(rt, in);
    charrt_tick(rt, dt);
    charrt_eval_base(rt);
    ProcInput pi;
    charrt_proc_input(rt, &pi, root_at(v3(0, 0, 0), in->body_yaw), dt);
    pi.item = k;
    pi.item_kick_pos = kick_pos;
    pi.item_kick_rot = kick_rot;
    pi.hand_ik_weight = 1.0f;
    charrt_post(rt, &pi);
    const Skeleton *s = &L->skel;
    // Item never moves between the right-hand solve and the end of the frame (the item hangs off
    // the right palm through the grip's right IK target).
    Xform palm_r = xf_mul(rt->final_model[s->role[BR_R_HAND]], L->hands.palm[1]);
    Xform item_now = xf_mul(palm_r, xf_inverse(grip_target(k, in->weapon_type, 1)));
    float item_move = v3_dist(item_now.t, rt->proc.item_model.t) + quat_angle_between(item_now.r, rt->proc.item_model.r);
    g->max_item = fmaxf(g->max_item, item_move);
    if (k->has_left && proc_hold_style(&L->proc, in->weapon_type)->left_hand) {
        // Left palm <-> its grip target from the FINAL pose.
        Xform palm_l = xf_mul(rt->final_model[s->role[BR_L_HAND]], L->hands.palm[0]);
        // ADS may slide the support hand back along the handguard (keeps the exact ADS transform).
        Xform gl = grip_target(k, in->weapon_type, 0);
        gl.t.z += rt->proc.grip_l_slide;
        Xform sock = xf_mul(rt->proc.item_model, gl);
        float pe = v3_dist(palm_l.t, sock.t), ae = quat_angle_between(palm_l.r, sock.r) * DG_RAD2DEG;
        if ((pe > 0.002f || ae > 1.0f) && g->max_pos <= 0.002f && g->max_ang <= 1.0f)
            printf("  glue %s/%s tick %d: %.2f mm %.2f deg (pitch %.2f ads %d crouch %d sprint %d, right err %.1f mm)\n", weapon, what, g->ticks, pe * 1000, ae,
                   in->aim_pitch, in->ads, in->crouch, in->sprint, rt->proc.right_err_pos * 1000);
        if (pe > g->max_pos && pe > 0.002f) {
            g->worst_tick = g->ticks;
            g->worst_pitch = in->aim_pitch;
            g->worst_flags = (in->ads ? 1 : 0) | (in->crouch ? 2 : 0) | (in->sprint ? 4 : 0);
            g->worst_yaw = wrap_angle(in->aim_yaw - in->body_yaw);
            snprintf(g->worst_what, sizeof(g->worst_what), "%s", what);
        }
        g->max_pos = fmaxf(g->max_pos, pe);
        g->max_ang = fmaxf(g->max_ang, ae);
    }
    g->ticks++;
}

// Reach report: shoulder positions relative to the eye (camera space at the bind pose) and arm
// lengths - the envelope every hold point state has to stay inside.
static void report_reach(void) {
    const Skeleton *s = &L->skel;
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    const CharSocket *eye = def ? character_socket(def, "camera") : NULL;
    if (!eye || eye->bone < 0) return;
    vec3 e = xf_point(s->bind_model[eye->bone], eye->rel.t);
    for (int h = 0; h < 2; h++) {
        int a = s->role[h ? BR_R_ARM : BR_L_ARM], f = s->role[h ? BR_R_FOREARM : BR_L_FOREARM], w = s->role[h ? BR_R_HAND : BR_L_HAND];
        if (a < 0 || f < 0 || w < 0) continue;
        vec3 sh = v3_sub(s->bind_model[a].t, e);
        float len = v3_dist(s->bind_model[a].t, s->bind_model[f].t) + v3_dist(s->bind_model[f].t, s->bind_model[w].t);
        Xform palm = xf_mul(s->bind_model[w], L->hands.palm[h]);
        printf("  reach %s shoulder (right %.3f, up %.3f, fwd %.3f) from the eye, arm %.3f m, palm %.3f from the wrist\n", h ? "R" : "L", sh.x, sh.y, -sh.z, len,
               v3_dist(palm.t, s->bind_model[w].t));
    }
}

static void test_hold_and_glue(const ItemDB *db) {
    report_reach();
    const float dt = 1.0f / 60.0f;
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    CHECK(def != NULL, "player character missing");
    if (!def) return;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    for (int wi = 0; wi < db->weapon_count; wi++) {
        const WeaponDef *wd = &db->weapons[wi];
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        CHECK(style >= 0, "weapon %s: unknown hold style %s", wd->id, wd->hold_style);
        if (style < 0) continue;
        // 1) Static hip / ADS placement: both hands within 1 cm of their grip targets.
        for (int ads = 0; ads < 2; ads++) {
            charrt_init(rt, L, def);
            CharInputs in;
            memset(&in, 0, sizeof(in));
            in.grounded = true;
            in.weapon_type = style;
            in.ads = ads != 0;
            GlueStats g = { 0 };
            for (int t = 0; t < 20; t++) glue_step(rt, &wd->sockets, &in, dt, v3(0, 0, 0), v3(0, 0, 0), &g, ads ? "ads" : "hip", wd->id);
            const Skeleton *s = &L->skel;
            Xform palm_r = xf_mul(rt->final_model[s->role[BR_R_HAND]], L->hands.palm[1]);
            Xform want_r = xf_mul(rt->proc.item_model, grip_target(&wd->sockets, style, 1));
            CHECK(v3_dist(palm_r.t, want_r.t) < 0.01f, "%s %s: right hand %.1f mm off its grip target", wd->id, ads ? "ADS" : "hip", v3_dist(palm_r.t, want_r.t) * 1000);
            CHECK(rt->proc.right_err_pos < 0.01f, "%s %s: right hand could not reach the desired item placement (%.1f mm)", wd->id,
                  ads ? "ADS" : "hip", rt->proc.right_err_pos * 1000);
            if (wd->sockets.has_left && proc_hold_style(&L->proc, style)->left_hand)
                CHECK(g.max_pos < 0.01f, "%s %s: left hand %.1f mm off its grip target", wd->id, ads ? "ADS" : "hip", g.max_pos * 1000);
        }
        // 2) Glue: 600 ticks through locomotion states, ADS/hip/sprint, a recoil mag dump, every body-part hit, a full turn in place.
        charrt_init(rt, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.weapon_type = style;
        GlueStats g = { 0 };
        for (int t = 0; t < 600; t++) {
            int phase = t / 60;
            in.speed = 0;
            in.move_local = v2(0, 1);
            in.ads = false;
            in.sprint = false;
            in.crouch = false;
            vec3 kick_p = v3(0, 0, 0), kick_r = v3(0, 0, 0);
            switch (phase) {
            case 0: break;                                                    // idle
            case 1: in.speed = 1.6f; break;                                   // walk
            case 2: in.speed = 4.2f; in.move_local = v2(0.7f, 0.7f); break;   // run diagonal
            case 3: in.speed = 1.6f; in.move_local = v2(-1, 0); in.ads = true; break;   // strafe ADS
            case 4: in.speed = 6.5f; in.sprint = true; break;                 // sprint (lowered)
            case 5: in.crouch = true; in.speed = 1.2f; break;                 // crouch walk
            case 6: in.ads = true;                                            // recoil mag dump
                kick_p = v3(0, 0.006f * sinf((float)t * 1.7f), 0.03f * fabsf(sinf((float)t * 0.9f)));
                kick_r = v3(4.0f * fabsf(sinf((float)t * 0.9f)), 1.5f * sinf((float)t * 1.3f), 2.0f * sinf((float)t * 0.7f));
                break;
            case 7:                                                           // a hit on every body part
                if (t % 3 == 0) {
                    int part = (t / 3) % BP_COUNT;
                    int body = -1;
                    for (int b = 0; b < L->ragdoll.body_count; b++) if (L->ragdoll.bodies[b].part == part) body = b;
                    if (body >= 0) proc_damage_hit(&rt->proc, &L->proc, &L->skel, part, L->ragdoll.bodies[body].bone, v3(0.3f, 0, -1), 1.5f);
                }
                break;
            default:                                                          // turn in place: full 360 over 3 s
                in.aim_yaw = wrap_angle((float)(t - 480) / 180.0f * 2.0f * DG_PI);
                break;
            }
            in.aim_pitch = 0.35f * sinf((float)t * 0.02f);
            u8 turning = 0;
            in.body_yaw = anim_body_yaw_step(in.body_yaw, in.aim_yaw, in.speed > 0.3f, &L->proc, dt, &turning);
            glue_step(rt, &wd->sockets, &in, dt, kick_p, kick_r, &g, "sequence", wd->id);
        }
        // 3) Extreme view pitch (players look +-89 deg) x stance x ADS x sprint.
        for (int combo = 0; combo < 8; combo++) {
            in.ads = combo & 1;
            in.crouch = (combo & 2) != 0;
            in.sprint = (combo & 4) != 0;
            in.speed = in.sprint ? 6.5f : 0.0f;
            for (int t = 0; t < 60; t++) {
                in.aim_pitch = 1.55f * sinf((float)t / 60.0f * 2.0f * DG_PI);
                u8 turning = 0;
                in.body_yaw = anim_body_yaw_step(in.body_yaw, in.aim_yaw, in.speed > 0.3f, &L->proc, dt, &turning);
                glue_step(rt, &wd->sockets, &in, dt, v3(0, 0, 0), v3(0, 0, 0), &g, "pitch sweep", wd->id);
            }
        }
        // 4) Aim yaw offset from a body that has not turned yet (+-yaw_limit) x pitch.
        in.ads = false; in.crouch = false; in.sprint = false; in.speed = 0;
        for (int t = 0; t < 240; t++) {
            in.body_yaw = 0;
            in.aim_yaw = L->proc.yaw_limit * DG_DEG2RAD * sinf((float)t / 240.0f * 2.0f * DG_PI);
            in.aim_pitch = 1.3f * sinf((float)t / 40.0f * 2.0f * DG_PI);
            in.ads = (t / 60) & 1;
            glue_step(rt, &wd->sockets, &in, dt, v3(0, 0, 0), v3(0, 0, 0), &g, "yaw sweep", wd->id);
        }
        bool has_left = wd->sockets.has_left && proc_hold_style(&L->proc, style)->left_hand;
        if (has_left) CHECK(g.max_pos < 0.002f && g.max_ang < 1.0f, "%s glue: max %.2f mm / %.2f deg over 600 ticks + pitch sweep", wd->id, g.max_pos * 1000, g.max_ang);
        CHECK(g.max_item < 1e-4f, "%s: item moved %.6f after the right-hand solve", wd->id, g.max_item);
        printf("  hold %-9s style %-9s glue max %.3f mm / %.3f deg, item drift %.2e", wd->id, wd->hold_style, g.max_pos * 1000, g.max_ang, g.max_item);
        if (g.max_pos > 0.002f)
            printf("  (worst: %s tick %d pitch %.2f rel yaw %.2f ads %d crouch %d sprint %d)", g.worst_what, g.worst_tick, g.worst_pitch, g.worst_yaw,
                   g.worst_flags & 1, (g.worst_flags >> 1) & 1, (g.worst_flags >> 2) & 1);
        printf("\n");
    }
    free(rt);
}

// Phone driven by a real phone (docs/PHONE.md): random device poses (and absurd ones) through the
// reach cone clamp, portrait (one hand) and landscape (two hands): the right hand reaches the
// phone, the left hand stays glued in landscape, the item never moves after the solve, and the
// phone's camera-space rotation stays inside the reach cone.
static void test_phone_hold(const ItemDB *db) {
    const ItemDef *it = item_def(db, db->builtin_phone);
    CHECK(db->builtin_phone && it->camera, "built-in phone item present");
    if (!db->builtin_phone) return;
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    const float dt = 1.0f / 60.0f;
    PhoneReach reach = phone_reach_default();
    for (int land = 0; land < 2; land++) {
        int style = proc_hold_style_index(&L->proc, land ? it->hold_style_alt : it->hold_style);
        CHECK(style >= 0, "phone hold style %s", land ? it->hold_style_alt : it->hold_style);
        if (style < 0) continue;
        charrt_init(rt, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.ads = true;
        in.weapon_type = style;
        in.weapon_id = db->builtin_phone;
        GlueStats g = { 0 };
        float worst_right = 0, worst_cone = 0;
        Rng rg = rng_make(77 + (u64)land);
        quat target = quat_identity();
        for (int t = 0; t < 900; t++) {
            // A new device pose every 0.25 s (smoothly approached, like filtered samples), some far
            // outside the reach cone; the view pitch sweeps too.
            if (t % 15 == 0) {
                float k = (t / 15) % 4 == 3 ? 3.0f : 1.0f;
                target = phone_from_ypr(v3(rng_range(&rg, -1.0f, 1.0f) * k, rng_range(&rg, -0.9f, 0.9f) * k, (land ? 1.57f : 0.0f) + rng_range(&rg, -0.4f, 0.4f) * k));
            }
            quat cur = t ? phone_quat_unpack(in.phone_rot) : quat_identity();
            in.phone_rot = phone_quat_pack(quat_nlerp(cur, target, 0.2f));
            in.phone_flags = (u8)(CHAR_PHONE_CAMERA | CHAR_PHONE_DRIVEN | (land ? CHAR_PHONE_LANDSCAPE : 0));
            in.aim_pitch = 0.9f * sinf((float)t * 0.013f);
            glue_step(rt, &it->sockets, &in, dt, v3(0, 0, 0), v3(0, 0, 0), &g, "phone", it->id);
            if (t < 30) continue;   // the 0.3 s blend from the mouse aim
            worst_right = fmaxf(worst_right, rt->proc.right_err_pos);
            quat item_cam = quat_mul(quat_conj(rt->proc.cam_model.r), rt->proc.item_model.r);
            vec3 a = v3_scale(phone_euler_ypr(item_cam), DG_RAD2DEG);
            float over = fmaxf(fmaxf(fabsf(a.x) - reach.yaw, fabsf(a.z) - reach.roll), fmaxf(a.y - reach.pitch_up, -a.y - reach.pitch_down));
            worst_cone = fmaxf(worst_cone, over);
        }
        printf("  phone %-9s right hand %.2f mm from the phone, left glue %.3f mm / %.3f deg, item drift %.2e, cone overshoot %.2f deg\n", land ? "landscape" : "portrait",
               worst_right * 1000, g.max_pos * 1000, g.max_ang, g.max_item, worst_cone);
        CHECK(worst_right < 0.01f, "phone %s: right hand %.1f mm from the driven phone", land ? "landscape" : "portrait", worst_right * 1000);
        CHECK(g.max_item < 1e-4f, "phone %s: item moved %.6f after the right-hand solve", land ? "landscape" : "portrait", g.max_item);
        if (land) CHECK(g.max_pos < 0.002f && g.max_ang < 1.0f, "phone landscape: left hand %.2f mm / %.2f deg off", g.max_pos * 1000, g.max_ang);
        CHECK(worst_cone < 1.0f, "phone %s: camera-space rotation %.2f deg outside the reach cone", land ? "landscape" : "portrait", worst_cone);
    }
    free(rt);
}

// ------------------------------------------------------------------ hold point / first person camera

// One tick with a held item. `view`: the presentation hold point too (players).
static void hp_step(CharRuntime *rt, const ItemSockets *k, CharInputs *in, float dt, bool static_pose, bool view, vec3 kick_p, vec3 kick_r) {
    charrt_set_inputs(rt, in);
    charrt_tick(rt, dt);
    charrt_eval_base(rt);
    ProcInput pi;
    charrt_proc_input(rt, &pi, root_at(v3(0, 0, 0), in->body_yaw), dt);
    pi.item = k;
    pi.hand_ik_weight = 1.0f;
    pi.hp_static = static_pose;
    pi.hp_view = view;
    pi.item_kick_pos = kick_p;
    pi.item_kick_rot = kick_r;
    charrt_post(rt, &pi);
}
// The clavicles (LeftShoulder / RightShoulder) keep the animation clip's local rotation after
// every procedural layer: the relaxed held-arm pose, damage reactions, spine, the hold point and
// the hand IK (anim_ik_shoulders off). Rifle and pistol, hip and ADS, standing and running, with
// an arm hit mid-way; the hands still reach their grips.
static void test_clavicles_keep_clip(const ItemDB *db) {
    const float dt = 1.0f / 60.0f;
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    const Skeleton *s = &L->skel;
    const int clav[2] = { s->role[BR_L_SHOULDER], s->role[BR_R_SHOULDER] };
    CHECK(clav[0] >= 0 && clav[1] >= 0, "rig has no LeftShoulder / RightShoulder roles");
    if (clav[0] < 0 || clav[1] < 0) return;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    ControllerInst *cc = (ControllerInst *)malloc(sizeof(ControllerInst));
    static Pose clip;
    static const char *guns[2] = { "pistol", "rifle" };
    for (int g = 0; g < 2; g++) {
        int wi = -1;
        for (int i = 0; i < db->weapon_count; i++) if (!strcmp(db->weapons[i].id, guns[g])) wi = i;
        CHECK(wi >= 0, "no weapon '%s'", guns[g]);
        if (wi < 0) continue;
        const WeaponDef *wd = &db->weapons[wi];
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        for (int mode = 0; mode < 4; mode++) {
            bool ads = mode & 1, run = mode & 2;
            charrt_init(rt, L, def);
            CharInputs in;
            memset(&in, 0, sizeof(in));
            in.grounded = true;
            in.weapon_type = style;
            in.weapon_id = (u16)(wi + 1);
            in.ads = ads;
            in.speed = run ? 3.0f : 0.0f;
            in.move_local = v2(0, 1);
            float worst = 0, worst_glue = 0, moved = 0;
            for (int t = 0; t < 120; t++) {
                in.aim_pitch = 0.5f * sinf((float)t * 0.05f);
                in.aim_yaw = 0.3f * sinf((float)t * 0.03f);
                charrt_set_inputs(rt, &in);
                charrt_tick(rt, dt);
                // The clip pose this tick: the controller's own output (a copy: evaluating advances
                // the inertialization state).
                *cc = rt->ctrl;
                ctrl_evaluate(cc, s, &L->lookup, &clip);
                charrt_eval_base(rt);
                ProcInput pi;
                charrt_proc_input(rt, &pi, root_at(v3(0, 0, 0), in.body_yaw), dt);
                pi.item = &wd->sockets;
                pi.hand_ik_weight = 1.0f;
                if (t == 40) proc_damage_hit(&rt->proc, &L->proc, s, 0, s->role[BR_L_FOREARM], v3(0.3f, 0, -1), 2.0f);
                if (t == 70) proc_damage_hit(&rt->proc, &L->proc, s, 0, s->role[BR_R_ARM], v3(-0.3f, 0, -1), 2.0f);
                charrt_post(rt, &pi);
                if (t < 30) continue;   // style / ADS blends settle
                for (int k = 0; k < 2; k++) {
                    float d = quat_angle_between(rt->final.local[clav[k]].r, clip.local[clav[k]].r) * DG_RAD2DEG;
                    worst = fmaxf(worst, d);
                    // The clavicle model rotation does change (the spine turns it): the check is local.
                    moved = fmaxf(moved, quat_angle_between(rt->final_model[clav[k]].r, rt->base_model[clav[k]].r) * DG_RAD2DEG);
                }
                if (proc_hold_style(&L->proc, style)->left_hand && wd->sockets.has_left) worst_glue = fmaxf(worst_glue, rt->proc.glue_err_pos);
            }
            printf("  clavicles %-6s %-3s %-5s local %.5f deg from the clip (model turned up to %.1f deg by the spine), left glue %.3f mm\n", guns[g],
                   ads ? "ads" : "hip", run ? "run" : "stand", worst, moved, worst_glue * 1000);
            CHECK(worst < 0.01f, "%s %s %s: clavicle local rotation %.4f deg off the clip", guns[g], ads ? "ads" : "hip", run ? "run" : "stand", worst);
            CHECK(worst_glue < 0.002f, "%s %s %s: left hand %.2f mm off grip_l", guns[g], ads ? "ads" : "hip", run ? "run" : "stand", worst_glue * 1000);
        }
    }
    free(cc);
    free(rt);
}

// Rule 1 (docs/CAMERA_HOLDPOINT.md): the first-person frame re-solve - only spine look and IK on stage
// A - given the tick's own inputs reproduces the tick's pose bit for bit, through locomotion, ADS,
// crouch, recoil kick with the hands trailing and the trigger pull, hits and a full turn; and a sprint
// hands the arms to its clip (hand IK weight 0), a run keeps the hold (1).
static void test_frame_solve(const ItemDB *db) {
    const float dt = 1.0f / 60.0f;
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    const Skeleton *s = &L->skel;
    CharRuntime *a = (CharRuntime *)malloc(sizeof(CharRuntime));
    static ProcStageA cap;
    static ProcState st;
    static Pose pose;
    static Xform model[ANIM_MAX_BONES];
    for (int wi = 0; wi < db->weapon_count; wi++) {
        const WeaponDef *wd = &db->weapons[wi];
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        charrt_init(a, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.weapon_type = style;
        in.weapon_id = (u16)(wi + 1);
        int checks = 0, fails = 0;
        u32 passes_all = 0;
        for (int t = 0; t < 360; t++) {
            int ph = t / 60;
            in.ads = ph == 1 || ph == 4;
            in.crouch = ph == 3;
            in.speed = ph == 2 ? 3.5f : (ph == 3 ? 1.2f : 0.0f);
            in.move_local = v2(0, 1);
            in.aim_yaw = ph == 5 ? (float)t * 0.11f : 0.6f * sinf((float)t * 0.05f);
            in.aim_pitch = 0.5f * sinf((float)t * 0.031f);
            u8 turning = 0;
            in.body_yaw = anim_body_yaw_step(in.body_yaw, in.aim_yaw, in.speed > 0.3f, &L->proc, dt, &turning);
            if (t == 100) proc_damage_hit(&a->proc, &L->proc, s, 0, s->role[BR_SPINE1], v3(0.3f, 0, -1), 2.0f);
            ProcInput pi;
            charrt_set_inputs(a, &in);
            charrt_tick(a, dt);
            charrt_eval_base(a);
            charrt_proc_input(a, &pi, root_at(v3(0, 0, 0), in.body_yaw), dt);
            pi.item = &wd->sockets;
            pi.hand_ik_weight = 1.0f;
            pi.hp_view = true;
            pi.capture = &cap;
            if (ph == 4) {   // a recoil string: kick, the hands trailing it, the trigger
                float k = fabsf(sinf((float)t * 0.7f));
                pi.item_kick_pos = v3(0, 0.004f * k, 0.03f * k);
                pi.item_kick_rot = v3(4.0f * k, 0.6f, 1.0f * k);
                pi.hand_lag_pos = v3(0, -0.003f * k, 0.008f * k);
                pi.hand_lag_rot = v3(-1.5f * k, 0.2f, 0);
                pi.trigger = k;
            }
            cap.valid = false;
            charrt_post(a, &pi);
            CHECK(cap.valid, "%s: stage A not captured", wd->id);
            st = a->proc;
            pose_copy(&pose, &cap.pose);
            memcpy(model, cap.model, sizeof(Xform) * (size_t)s->count);
            ProcInput fi = pi;
            fi.hp_given = true;
            u32 passes = proc_frame_solve(&st, &fi, cap.clav_clip, &a->final, a->final_model, &pose, model);
            passes_all |= passes;
            checks++;
            if (memcmp(pose.local, a->final.local, sizeof(Xform) * (size_t)s->count) || memcmp(model, a->final_model, sizeof(Xform) * (size_t)s->count) ||
                memcmp(&st.item_model, &a->proc.item_model, sizeof(Xform)))
                fails++;
        }
        CHECK(fails == 0, "%s: the frame re-solve differs from the tick pose in %d of %d ticks", wd->id, fails, checks);
        CHECK((passes_all & ~(PROC_PASS_SPINE | PROC_PASS_IK)) == 0, "%s: a pass outside the whitelist wrote bones (0x%x)", wd->id, passes_all);
        if (wi < 3) printf("  frame re-solve %-9s %d ticks bit-identical to the tick pose (passes 0x%x)\n", wd->id, checks - fails, passes_all);
    }
    // Sprint: the clip owns the arms; run: the hold.
    {
        charrt_init(a, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.weapon_type = proc_hold_style_index(&L->proc, db->weapons[1].hold_style);
        in.weapon_id = 2;
        in.move_local = v2(0, 1);
        float w_run = 0, w_sprint = 1;
        for (int t = 0; t < 240; t++) {
            in.sprint = t >= 120;
            in.speed = in.sprint ? 7.0f : 4.2f;
            charrt_set_inputs(a, &in);
            charrt_tick(a, dt);
            float w = 1;
            ctrl_hand_ik_weight(&a->ctrl, &w);
            if (t == 110) w_run = w;
            if (t == 230) w_sprint = w;
        }
        CHECK(w_run > 0.99f, "running: hand IK weight %.2f (the hold should own the hands)", w_run);
        CHECK(w_sprint < 0.01f, "sprinting: hand IK weight %.2f (the sprint clip should own the arms)", w_sprint);
        printf("  hand IK: run %.2f, sprint %.2f (the sprint clip carries the arms)\n", w_run, w_sprint);
    }
    free(a);
}

static void test_holdpoint(const ItemDB *db) {
    const float dt = 1.0f / 60.0f;
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    CharRuntime *a = (CharRuntime *)malloc(sizeof(CharRuntime)), *b = (CharRuntime *)malloc(sizeof(CharRuntime));
    // 1) ADS (docs/CAMERA_HOLDPOINT.md): the presentation hold point puts the sight socket on the view
    //    axis within 0.5 mm / 0.1 deg for every gun at several view pitches, and the body ignores ADS:
    //    the pose's item (camera space) is the hip one to 0.01 mm.
    for (int wi = 0; wi < db->weapon_count; wi++) {
        const WeaponDef *wd = &db->weapons[wi];
        if (wd->kind == WPN_MELEE) continue;
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        float worst_p = 0, worst_a = 0, worst_body = 0;
        for (int pt = 0; pt < 5; pt++) {
            charrt_init(a, L, def);
            charrt_init(b, L, def);
            CharInputs in;
            memset(&in, 0, sizeof(in));
            in.grounded = true;
            in.weapon_type = style;
            in.weapon_id = (u16)(wi + 1);
            in.aim_pitch = (-0.6f + 0.3f * (float)pt);
            CharInputs hip = in;
            in.ads = true;
            for (int t = 0; t < 40; t++) {
                hp_step(a, &wd->sockets, &in, dt, true, true, v3(0, 0, 0), v3(0, 0, 0));
                hp_step(b, &wd->sockets, &hip, dt, true, true, v3(0, 0, 0), v3(0, 0, 0));
            }
            CHECK(a->proc.hp_view_valid, "%s: no presentation hold point", wd->id);
            Xform ic = a->proc.view_item_cam;
            vec3 sc = xf_point(ic, wd->sockets.sight);
            float pe = sqrtf(sc.x * sc.x + sc.y * sc.y), ae = quat_angle_between(ic.r, quat_identity()) * DG_RAD2DEG;
            worst_p = fmaxf(worst_p, pe);
            worst_a = fmaxf(worst_a, ae);
            Xform pa = xf_mul(xf_inverse(a->proc.cam_model), a->proc.item_model), pb = xf_mul(xf_inverse(b->proc.cam_model), b->proc.item_model);
            worst_body = fmaxf(worst_body, v3_dist(pa.t, pb.t));
        }
        CHECK(worst_p < 0.0005f && worst_a < 0.1f, "%s: ADS sight %.3f mm / %.3f deg off the view axis", wd->id, worst_p * 1000, worst_a);
        CHECK(worst_body < 0.00001f, "%s: ADS moved the item of the body %.3f mm (the model must stay unchanged)", wd->id, worst_body * 1000);
        printf("  ads %-9s presented sight on axis within %.4f mm / %.4f deg, body item %.4f mm from hip\n", wd->id, worst_p * 1000, worst_a, worst_body * 1000);
    }
    // 2) The owner and a remote copy solve bit-identical poses from the same inputs (no
    //    first-person-only pose code - the owner also evaluates the presentation hold point, which
    //    nothing in the pose follows): 300 ticks through states, look, recoil.
    for (int wi = 0; wi < db->weapon_count; wi++) {
        const WeaponDef *wd = &db->weapons[wi];
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        charrt_init(a, L, def);
        charrt_init(b, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.weapon_type = style;
        in.weapon_id = (u16)(wi + 1);
        bool same = true;
        for (int t = 0; t < 300; t++) {
            int ph = t / 50;
            in.ads = ph == 1 || ph == 4;
            in.sprint = ph == 2;
            in.crouch = ph == 3;
            in.speed = ph == 2 ? 6.0f : (ph == 3 ? 1.2f : 0.0f);
            in.aim_yaw = 0.6f * sinf((float)t * 0.05f);
            in.aim_pitch = 0.4f * sinf((float)t * 0.031f);
            vec3 kp = ph == 4 ? v3(0, 0.004f, 0.02f * fabsf(sinf((float)t))) : v3(0, 0, 0);
            vec3 kr = ph == 4 ? v3(3.0f * fabsf(sinf((float)t)), 0.5f, 0.8f) : v3(0, 0, 0);
            u8 turning = 0;
            in.body_yaw = anim_body_yaw_step(in.body_yaw, in.aim_yaw, in.speed > 0.3f, &L->proc, dt, &turning);
            hp_step(a, &wd->sockets, &in, dt, false, false, kp, kr);
            hp_step(b, &wd->sockets, &in, dt, false, true, kp, kr);   // the owner: + presentation hold point
            if (memcmp(a->final_model, b->final_model, sizeof(Xform) * (size_t)L->skel.count) != 0 ||
                memcmp(&a->proc.item_model, &b->proc.item_model, sizeof(Xform)) != 0) {
                same = false;
                break;
            }
        }
        CHECK(same, "%s: owner and remote hold point poses differ", wd->id);
    }
    // 3) State switches are continuous: the hold point never jumps (inertialized blends).
    {
        const WeaponDef *wd = &db->weapons[1];
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        charrt_init(a, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.weapon_type = style;
        in.weapon_id = 2;
        // A snap would move the full state distance (~20 cm) in one tick. Velocity continuity: a
        // switch that interrupts a running blend must not change the velocity more than an
        // uninterrupted blend itself does (a kink shows up as a much larger per-tick change).
        float step[2] = { 0, 0 }, dvm[2] = { 0, 0 };
        const int period[2] = { 60, 5 };   // calm switches (baseline) / switches every 83 ms (mid-blend)
        for (int run = 0; run < 2; run++) {
            charrt_init(a, L, def);
            Xform prev = xf_identity();
            vec3 prev_v = v3(0, 0, 0);
            for (int t = 0; t < 480; t++) {
                int ph = (t / period[run]) % 6;
                in.ads = ph == 1 || ph == 4;
                in.sprint = ph == 2;
                in.crouch = ph == 3 || ph == 4;
                hp_step(a, &wd->sockets, &in, dt, true, false, v3(0, 0, 0), v3(0, 0, 0));
                Xform h = a->proc.hp_out.hold;
                if (t > 0) {
                    vec3 v = v3_sub(h.t, prev.t);
                    step[run] = fmaxf(step[run], v3_len(v));
                    if (t > 1) dvm[run] = fmaxf(dvm[run], v3_dist(v, prev_v));
                    prev_v = v;
                }
                prev = h;
            }
        }
        CHECK(step[0] < 0.05f && step[1] < 0.05f, "hold point state blend jumps %.1f / %.1f mm in one tick", step[0] * 1000, step[1] * 1000);
        CHECK(dvm[1] < dvm[0] * 1.6f, "interrupted blends change velocity %.2f mm/tick vs %.2f uninterrupted (kink)", dvm[1] * 1000, dvm[0] * 1000);
        printf("  hold point blend: max %.1f mm/tick; velocity change %.2f mm/tick interrupted every 83 ms vs %.2f uninterrupted\n",
               fmaxf(step[0], step[1]) * 1000, dvm[1] * 1000, dvm[0] * 1000);
    }
    free(a);
    free(b);
}

// Rule 2 (docs/CAMERA_HOLDPOINT.md): the gliding camera.
//  1) The shipped nominal eye table (assets/player/eye_nominal.json) is the animation's: a fresh
//     calibration moves no entry by more than 5 mm (else: cam_calibrate in the game).
//  2) Animation-only motion never moves the camera (idle, walk / run in place, crouch, hits: the
//     camera delta is exactly 0 while the head eye moves), and the head eye stays within the anchor
//     radius of it (hits reported).
//  3) Stance changes glide (critically damped: no overshoot, no jump); a step-up snap glides out; a
//     slope or a jump follows the capsule at once.
static void test_camera_glide(const ItemDB *db) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    static EyeNominalTable fresh, shipped;
    // The game bakes the clips at its simulation rate (anim_sample_rate = SIM_HZ): measure that library.
    AnimLibrary *gl = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    CHECK(anim_lib_load(gl, (float)SIM_HZ), "60 Hz library load failed");
    const CharacterDef *gdef = anim_lib_char_by_id(gl, "player");
    CHECK(gdef && eye_nominal_calibrate(gl, gdef, db, &fresh, 0, 0), "nominal eye calibration failed");
    anim_lib_free(gl);
    free(gl);
    size_t size = 0;
    char *txt = platform_read_file("assets/player/eye_nominal.json", &size);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    eye_table_parse(&shipped, root);
    cJSON_Delete(root);
    free(txt);
    CHECK(shipped.valid, "assets/player/eye_nominal.json missing (run cam_calibrate)");
    CHECK(shipped.anim_hash == fresh.anim_hash, "eye table measured from another animation library (%08x, now %08x): run cam_calibrate", shipped.anim_hash, fresh.anim_hash);
    float worst = 0;
    int wp = 0, ws = 0;
    for (int pp = 0; pp < EYE_POSTURES; pp++)
        for (int st = 0; st < EYE_STYLES; st++) {
            if (!fresh.style_valid[st]) continue;
            CHECK(shipped.style_valid[st], "eye table: hold style %d missing (run cam_calibrate)", st);
            for (int i = 0; i < fresh.speed_n[pp]; i++)
                for (int k = 0; k < EYE_PITCH_N; k++) {
                    float d = v3_dist(fresh.off[pp][st][i][k], shipped.off[pp][st][i][k]);
                    if (d > worst) { worst = d; wp = pp; ws = st; }
                }
        }
    CHECK(worst < 0.005f, "eye table is stale: %s / style %d moved %.1f mm (run cam_calibrate)", eye_posture_name(wp), ws, worst * 1000);
    printf("  eye table: fresh calibration within %.2f mm of the shipped one; residuals (mm):", worst * 1000);
    for (int pp = 0; pp < EYE_POSTURES; pp++) printf(" %s %.0f", eye_posture_name(pp), fresh.residual[pp] * 1000);
    printf("\n");

    // 2) A character animating while its camera inputs stay the same.
    {
        const float dt = 1.0f / 60.0f;
        CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
        const WeaponDef *rifle = NULL;
        for (int i = 0; i < db->weapon_count; i++) if (!strcmp(db->weapons[i].id, "rifle")) rifle = &db->weapons[i];
        static const char *names[5] = { "idle", "walk in place", "run in place", "crouch", "hits" };
        for (int mode = 0; mode < 5; mode++) {
            charrt_init(rt, L, def);
            CharInputs in;
            memset(&in, 0, sizeof(in));
            in.grounded = true;
            in.weapon_type = rifle ? proc_hold_style_index(&L->proc, rifle->hold_style) : 0;
            in.weapon_id = rifle ? (int)(rifle - db->weapons) + 1 : 0;
            in.move_local = v2(0, 1);
            in.health = 100;
            in.speed = mode == 1 ? 1.6f : mode == 2 ? 4.2f : 0.0f;
            in.crouch = mode == 3;
            CamGlide g;
            cam_glide_reset(&g);
            CamGlideParams gp = cam_glide_params_default();
            CamNominalIn ni;
            memset(&ni, 0, sizeof(ni));
            ni.posture = -1;
            ni.crouch = in.crouch ? 1.0f : 0.0f;
            ni.speed = in.speed;   // "in place": the clip plays, the capsule doesn't move
            ni.style = in.weapon_type;
            vec3 cam0 = v3(0, 0, 0), head_min = v3(1e9f, 1e9f, 1e9f), head_max = v3(-1e9f, -1e9f, -1e9f);
            float cam_moved = 0, anchor = 0;
            for (int t = 0; t < 240; t++) {
                if (mode == 4 && t % 40 == 20) proc_damage_hit(&rt->proc, &L->proc, &L->skel, t / 40 % 3, L->skel.role[t / 40 % 2 ? BR_SPINE1 : BR_HEAD], v3(0.4f, 0, -1), 2.5f);
                charrt_set_inputs(rt, &in);
                charrt_tick(rt, dt);
                charrt_eval_base(rt);
                ProcInput pi;
                charrt_proc_input(rt, &pi, root_at(v3(0, 0, 0), 0.0f), dt);
                pi.item = rifle ? &rifle->sockets : NULL;
                if (in.crouch) pi.crouch_drop = 0.38f;
                charrt_post(rt, &pi);
                vec3 off = cam_glide_offset(&g, &shipped, &ni, dt, &gp);
                float step = cam_glide_step(&g, 0.0f, 0.0f, true, dt, &gp);
                vec3 cam = v3(off.x, off.y + step, -off.z);   // model space: forward is -Z
                Xform eye;
                if (!character_socket_world(def, "camera", rt->final_model, &eye)) continue;
                if (t < 60) { cam0 = cam; continue; }
                cam_moved = fmaxf(cam_moved, v3_dist(cam, cam0));
                anchor = fmaxf(anchor, v3_dist(cam, eye.t));
                head_min = v3_min(head_min, eye.t);
                head_max = v3_max(head_max, eye.t);
            }
            vec3 range = v3_sub(head_max, head_min);
            printf("  camera %-14s: camera moved %.3f mm while the head eye moved %.0f x %.0f x %.0f mm; head eye within %.0f mm of it\n", names[mode], cam_moved * 1000,
                   range.x * 1000, range.y * 1000, range.z * 1000, anchor * 1000);
            CHECK(cam_moved == 0.0f, "%s: the camera moved %.3f mm with animation-only motion", names[mode], cam_moved * 1000);
            if (mode < 4) CHECK(anchor < 0.08f, "%s: the head eye strays %.0f mm from the nominal eye (anchor 80 mm)", names[mode], anchor * 1000);
        }
        free(rt);
    }
    // 3) The glide: stand -> crouch and back, a 30 cm step-up, a slope, a jump.
    {
        CamGlide g;
        cam_glide_reset(&g);
        CamGlideParams gp = cam_glide_params_default();
        CamNominalIn ni;
        memset(&ni, 0, sizeof(ni));
        ni.posture = -1;
        const float dt = 1.0f / 144.0f;
        vec3 a = eye_table_sample(&shipped, EYE_STAND, 0, 0, 0), b = eye_table_sample(&shipped, EYE_CROUCH, 0, 0, 0);
        float overshoot = 0, jump = 0, settle_t = -1;
        vec3 prev = cam_glide_offset(&g, &shipped, &ni, dt, &gp);
        for (int f = 1; f < 288; f++) {
            ni.crouch = 1.0f;
            vec3 o = cam_glide_offset(&g, &shipped, &ni, dt, &gp);
            jump = fmaxf(jump, v3_dist(o, prev));
            overshoot = fmaxf(overshoot, b.y - o.y);   // below the crouch target
            if (settle_t < 0 && v3_dist(o, b) < 0.001f) settle_t = (float)f * dt;
            prev = o;
        }
        printf("  glide stand -> crouch (%.0f mm): settles within 1 mm at %.2f s, overshoot %.2f mm, largest 144 Hz step %.1f mm\n", (a.y - b.y) * 1000, settle_t,
               overshoot * 1000, jump * 1000);
        CHECK(overshoot < 0.001f, "glide overshoots the crouch eye by %.1f mm", overshoot * 1000);
        CHECK(settle_t > 0 && settle_t < 0.8f, "glide settles in %.2f s", settle_t);
        CHECK(jump < 0.02f, "glide jumps %.1f mm in one 144 Hz frame", jump * 1000);
        // Step: the capsule snaps up 25 cm while grounded; the camera glides up instead.
        cam_glide_reset(&g);
        float y = 0, worst_step = 0, cam_prev = 0, s_end = 0;
        for (int f = 0; f < 288; f++) {
            if (f == 20) y += 0.25f;
            float s = cam_glide_step(&g, y, 1.5f, true, dt, &gp);
            float cy = y + s;
            if (f > 0) worst_step = fmaxf(worst_step, fabsf(cy - cam_prev));
            cam_prev = cy;
            s_end = s;
        }
        printf("  glide step-up 25 cm: largest 144 Hz camera step %.1f mm, left %.2f mm after 2 s\n", worst_step * 1000, fabsf(s_end) * 1000);
        CHECK(worst_step < 0.02f && fabsf(s_end) < 0.001f, "step-up: camera step %.1f mm, left %.1f mm", worst_step * 1000, fabsf(s_end) * 1000);
        // Stairs (18 cm risers, 28 cm treads) at walk / run / sprint, each snap spread over a 60 Hz
        // tick's frames like the interpolated position: the eye never sinks more than 30 cm behind the
        // capsule, and the absorber works at 60 and 240 Hz alike.
        for (int fr = 0; fr < 2; fr++) {
            const float fdt = fr ? 1.0f / 240.0f : 1.0f / 60.0f;
            for (int sp = 0; sp < 3; sp++) {
                const float speed = sp == 0 ? 1.6f : (sp == 1 ? 4.2f : 6.8f);
                cam_glide_reset(&g);
                float x = 0, worst = 0;
                for (int f = 0; f < (int)(3.0f / fdt); f++) {
                    x += speed * fdt;
                    float n = floorf(x / 0.28f);   // treads passed: the capsule steps up at each edge ...
                    float frac = saturate((x - n * 0.28f) / (speed / 60.0f));   // ... spread over one tick
                    float s = cam_glide_step(&g, 0.18f * (n - 1.0f + frac), speed, true, fdt, &gp);
                    worst = fmaxf(worst, fabsf(s));
                }
                CHECK(worst <= 0.3f + 1e-4f, "stairs at %.1f m/s, %.0f Hz: the eye sinks %.0f mm", speed, 1.0f / fdt, worst * 1000);
                CHECK(worst > 0.02f, "stairs at %.1f m/s, %.0f Hz: no step absorbed", speed, 1.0f / fdt);
                if (fr) printf("  stairs %.1f m/s: the eye at most %.0f mm behind the capsule\n", speed, worst * 1000);
            }
        }
        // Slope: 0.5 m/s up at 4 m/s, and a jump: the camera follows the capsule exactly.
        cam_glide_reset(&g);
        float lag = 0;
        y = 0;
        for (int f = 0; f < 288; f++) {
            y += (f < 144 ? 0.5f : 3.0f) * dt;
            float s = cam_glide_step(&g, y, 4.0f, f < 144, dt, &gp);
            lag = fmaxf(lag, fabsf(s));
        }
        CHECK(lag < 1e-6f, "slope / jump: the camera lags the capsule by %.1f mm", lag * 1000);
    }
}

// ------------------------------------------------------------------ hitboxes / damage

static void test_body_parts(const ItemDB *db) {
    const Skeleton *s = &L->skel;
    Xform world[ANIM_MAX_BONES];
    Pose p;
    pose_bind(s, &p);
    pose_to_model(s, &p, world);
    HitboxSet set;
    hitbox_build(&L->hitboxes, &L->ragdoll, world, 1.0f, &set);
    CHECK(set.count == 17, "hitbox count %d", set.count);
    const CharacterDef *player = anim_lib_char_by_id(L, "player");
    const WeaponDef *sniper = NULL, *rifle = NULL;
    for (int i = 0; i < db->weapon_count; i++) {
        if (!strcmp(db->weapons[i].id, "sniper")) sniper = &db->weapons[i];
        if (!strcmp(db->weapons[i].id, "rifle")) rifle = &db->weapons[i];
    }
    vec3 dirs[14] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    for (int i = 6; i < 14; i++) dirs[i] = v3_norm(v3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f));
    for (int k = 0; k < set.count; k++) {
        const HitCapsule *c = &set.c[k];
        vec3 center = v3_scale(v3_add(c->a, c->b), 0.5f);
        bool found = false;
        for (int d = 0; d < 14 && !found; d++) {
            vec3 o = v3_madd(center, dirs[d], -3.0f);
            HitboxRayHit h;
            if (hitbox_raycast(&set, o, dirs[d], 10.0f, &h) && h.part == c->part) {
                found = true;
                float base = 40.0f;
                float dmg = damage_resolve(base, player->part_mult, sniper ? sniper->part_mult : NULL, h.part, 1.0f);
                float expect = base * player->part_mult[h.part] * (sniper ? sniper->part_mult[h.part] : 1.0f);
                CHECK(dmg == expect, "part %s: damage %.4f != %.4f", body_part_name(h.part), dmg, expect);
            }
        }
        CHECK(found, "no ray isolates body part %s", body_part_name(c->part));
    }
    // Exact multipliers: player head 2.5, sniper head override 1.6 -> 4.0x; unlisted parts default to 1.0.
    CHECK(fabsf(player->part_mult[BP_HEAD] - 2.5f) < 1e-6f, "player head mult %.3f", player->part_mult[BP_HEAD]);
    if (sniper) CHECK(sniper->part_mult[BP_HEAD] == 1.6f && sniper->part_mult[BP_CHEST] == 1.0f, "sniper part_mult override / default wrong");
    if (rifle) for (int pp = 0; pp < BP_COUNT; pp++) CHECK(rifle->part_mult[pp] == 1.0f, "rifle part %s not 1.0", body_part_name(pp));
    CHECK(damage_resolve(30.0f, player->part_mult, sniper->part_mult, BP_HEAD, 1.0f) == 30.0f * 2.5f * 1.6f, "head x sniper resolution");
    // A ray straight at the face from the front hits the head (nearest to the origin wins).
    vec3 hc = v3_scale(v3_add(set.c[4].a, set.c[4].b), 0.5f);
    HitboxRayHit h;
    CHECK(hitbox_raycast(&set, v3_add(hc, v3(0, 0, -2)), v3(0, 0, 1), 5, &h) && h.part == BP_HEAD, "front ray at the face hit %s", body_part_name(h.part));
}

static void test_damage_reaction(void) {
    ProcState st;
    proc_reset(&st);
    Pose p;
    Xform model[ANIM_MAX_BONES];
    pose_bind(&L->skel, &p);
    pose_to_model(&L->skel, &p, model);
    ProcInput in;
    memset(&in, 0, sizeof(in));
    in.skel = &L->skel;
    in.params = &L->proc;
    in.dt = 1.0f / 60.0f;
    in.damage = true;
    for (int part = 0; part < BP_COUNT; part++) {
        int bone = -1;
        for (int b = 0; b < L->ragdoll.body_count; b++) if (L->ragdoll.bodies[b].part == part) bone = L->ragdoll.bodies[b].bone;
        proc_damage_hit(&st, &L->proc, &L->skel, part, bone, v3(0, 0, 1), 2.0f);
        CHECK(proc_damage_active(&st), "no reaction for %s", body_part_name(part));
        int ticks = 0;
        while (proc_damage_active(&st) && ticks < 120) {
            Pose q = p;
            Xform m[ANIM_MAX_BONES];
            memcpy(m, model, sizeof(m));
            proc_damage_apply(&st, &in, &q, m);
            ticks++;
        }
        CHECK(ticks <= 60, "reaction for %s took %d ticks to return to zero (> 1.0 s)", body_part_name(part), ticks);
    }
}

// ------------------------------------------------------------------ determinism / net

static float prand(u32 *s) { *s = *s * 1664525u + 1013904223u; return (float)(*s >> 8) / 16777216.0f; }

static void feed(ControllerInst *c, int t, u32 *seed) {
    float r = prand(seed);
    ctrl_set(c, AP_SPEED, (t / 200) % 3 == 0 ? 0 : 1.0f + 4.0f * prand(seed));
    float a = (float)t * 0.01f;
    ctrl_set(c, AP_MOVE_X, sinf(a));
    ctrl_set(c, AP_MOVE_Y, cosf(a));
    ctrl_set(c, AP_GROUNDED, (t % 500) < 470 ? 1.0f : 0.0f);
    ctrl_set(c, AP_CROUCH, (t % 900) > 700 ? 1.0f : 0.0f);
    ctrl_set(c, AP_VERTICAL_VEL, (t % 500) >= 470 ? -8.0f : 0.0f);
    ctrl_set(c, AP_WEAPON_TYPE, (float)((t / 1000) % 4));
    if (r < 0.01f) ctrl_trigger(c, AP_T_JUMP);
    if (t % 500 == 470 + 29) ctrl_trigger(c, AP_T_LAND);
    if (r > 0.995f) ctrl_trigger(c, AP_T_RELOAD);
    if ((t % 37) == 0) ctrl_trigger(c, AP_T_FIRE);
}

// Upper-body one-shots (equip / reload / fire / melee) must return to "none" and give the hands
// back to the weapon hold, also when their clips are unbound (placeholder anim sets).
static void test_upper_oneshots(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    static const int trig[4] = { AP_T_EQUIP, AP_T_RELOAD, AP_T_FIRE, AP_T_MELEE };
    static const char *names[4] = { "equip", "reload", "fire", "melee" };
    for (int k = 0; k < 4; k++) {
        charrt_init(rt, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.weapon_type = proc_hold_style_index(&L->proc, "rifle");
        in.weapon_id = 2;
        charrt_set_inputs(rt, &in);
        charrt_tick(rt, 1.0f / 60.0f);
        charrt_trigger(rt, trig[k]);
        int back = -1;
        for (int t = 0; t < 600 && back < 0; t++) {
            charrt_set_inputs(rt, &in);
            charrt_tick(rt, 1.0f / 60.0f);
            if (t > 0 && !strcmp(ctrl_state_name(&rt->ctrl, 1, rt->ctrl.layer[1].state), "none")) back = t;
        }
        float w = 0;
        bool ik = ctrl_hand_ik_weight(&rt->ctrl, &w);
        CHECK(back >= 0, "upper %s never returned to none (stuck in %s)", names[k], ctrl_state_name(&rt->ctrl, 1, rt->ctrl.layer[1].state));
        for (int t = 0; t < 30; t++) { charrt_set_inputs(rt, &in); charrt_tick(rt, 1.0f / 60.0f); }
        ik = ctrl_hand_ik_weight(&rt->ctrl, &w);
        CHECK(ik && w > 0.99f, "upper %s: hand IK weight %.2f after returning", names[k], w);
        printf("  upper %-6s back to none after %d ticks, hand ik %.2f\n", names[k], back, w);
    }
    free(rt);
}

static void test_determinism(void) {
    ControllerInst *a = (ControllerInst *)malloc(sizeof(ControllerInst)), *b = (ControllerInst *)malloc(sizeof(ControllerInst));
    ctrl_init(a, &L->ctrl, &L->sets[0]);
    ctrl_init(b, &L->ctrl, &L->sets[0]);
    u32 sa = 99, sb = 99;
    Pose pa, pb;
    int mismatch = 0, transitions = 0;
    for (int t = 0; t < 10000; t++) {
        feed(a, t, &sa);
        feed(b, t, &sb);
        int before = a->layer[0].state;
        ctrl_tick(a, &L->lookup, 1.0f / 60.0f);
        ctrl_tick(b, &L->lookup, 1.0f / 60.0f);
        if (a->layer[0].state != before) transitions++;
        for (int l = 0; l < L->ctrl.layer_count; l++)
            if (a->layer[l].state != b->layer[l].state || memcmp(&a->layer[l].phase, &b->layer[l].phase, sizeof(float))) mismatch++;
        if (t % 25 == 0) {
            ctrl_evaluate(a, &L->skel, &L->lookup, &pa);
            ctrl_evaluate(b, &L->skel, &L->lookup, &pb);
            if (memcmp(pa.local, pb.local, sizeof(Xform) * (size_t)L->skel.count)) mismatch++;
        }
    }
    CHECK(mismatch == 0, "controller diverged %d times over 10000 ticks", mismatch);
    CHECK(transitions > 20, "parameter stream exercised only %d base transitions", transitions);
    printf("  determinism: 10000 ticks, %d base-layer transitions, bit-identical state ids + poses\n", transitions);
    free(a);
    free(b);
}

static void test_quantization(void) {
    float worst[6] = { 0 };
    for (int i = 0; i <= 2000; i++) {
        float u = (float)i / 2000.0f;
        float sp = u * 12.75f;
        worst[0] = fmaxf(worst[0], fabsf(and_speed(anq_speed(sp)) - sp));
        float mv = u * 2 - 1;
        worst[1] = fmaxf(worst[1], fabsf(and_unit(anq_unit(mv)) - mv));
        float ang = (u * 2 - 1) * DG_PI * 0.999f;
        worst[2] = fmaxf(worst[2], fabsf(wrap_angle(and_angle(anq_angle(ang)) - ang)));
        float pit = (u * 2 - 1) * 1.55f;
        worst[3] = fmaxf(worst[3], fabsf(and_pitch(anq_pitch(pit)) - pit));
        float ph = u * 0.999f;
        float dph = fabsf(and_phase(anq_phase(ph)) - ph);
        worst[4] = fmaxf(worst[4], fminf(dph, 1.0f - dph));
        vec3 d = v3_norm(v3(sinf(u * 40.0f), cosf(u * 23.0f), sinf(u * 7.0f + 1)));
        worst[5] = fmaxf(worst[5], acosf(fminf(1.0f, v3_dot(d, and_dir(anq_dir(d))))) * DG_RAD2DEG);
    }
    CHECK(worst[0] <= 0.0251f, "speed err %f", worst[0]);
    CHECK(worst[1] <= 0.0040f, "move err %f", worst[1]);
    CHECK(worst[2] <= 0.0028f * DG_DEG2RAD * 1.01f, "angle err %f deg", worst[2] * DG_RAD2DEG);
    CHECK(worst[3] <= 0.0000501f, "pitch err %f", worst[3]);
    CHECK(worst[4] <= 0.002f, "phase err %f", worst[4]);
    CHECK(worst[5] <= 1.2f, "dir err %f deg", worst[5]);
    // Snapshot field round trip + size.
    AnimNetState s0, s1, out;
    memset(&s0, 0, sizeof(s0));
    s1 = s0;
    s1.phase = 77;
    s1.aim_yaw = anq_angle(1.0f);
    s1.aim_pitch = anq_pitch(-0.2f);
    s1.body_yaw = anq_angle(0.9f);
    s1.speed = anq_speed(1.6f);
    s1.move_y = 127;
    u8 buf[64];
    Writer w = writer_make(buf, sizeof(buf));
    u16 f = anim_net_diff(&s0, &s1);
    anim_net_write(&w, &s1, f);
    out = s0;
    Reader r = reader_make(buf, w.len);
    anim_net_read(&r, &out, NULL);
    CHECK(!memcmp(&out, &s1, sizeof(out)) && !r.overflow, "anim state round trip");
    CHECK((int)w.len == 2 + anim_net_field_bytes(f), "anim state size %u", w.len);
    printf("  anim state, walking + aiming: %u bytes (fields %04x)\n", w.len, f);
    AnimEvent e = { AEV_HIT, 0x1234, 777, 9, BP_HEAD, anq_dir(v3(0, 0, 1)), anq_strength(1.5f), 3, 0 }, e2;
    w = writer_make(buf, sizeof(buf));
    anim_event_write(&w, &e);
    r = reader_make(buf, w.len);
    bool rd = anim_event_read(&r, &e2);
    CHECK(rd && e2.type == e.type && e2.ent == e.ent && e2.tick == e.tick && e2.seed == e.seed && e2.body_part == e.body_part && e2.dir == e.dir &&
              e2.strength == e.strength && e2.damage_type == e.damage_type && (int)w.len == anim_event_size(&e),
          "hit event round trip");
}

static void test_lagcomp(void) {
    HitboxHistory *h = (HitboxHistory *)calloc(1, sizeof(HitboxHistory));
    Pose p;
    Xform world[ANIM_MAX_BONES];
    const Clip *walk = &L->clips[anim_lib_find_clip(L, "jog_f")];
    HitboxSet sets[40];
    for (int t = 0; t < 40; t++) {
        clip_sample(walk, (float)t / 60.0f, &p);
        pose_to_model(&L->skel, &p, world);
        for (int i = 0; i < L->skel.count; i++) world[i].t = v3_add(world[i].t, v3(0, 0, -0.03f * (float)t));
        hitbox_build(&L->hitboxes, &L->ragdoll, world, 1.0f, &sets[t]);
        sets[t].tick = 1000 + (u32)t;
        hitbox_record(h, &sets[t]);
    }
    int exact = 0;
    for (int t = 40 - HB_HISTORY; t < 40; t++) {
        HitboxSet out;
        bool ok = hitbox_rewind(h, (1000u + (u32)t) << 8, &out);
        exact += ok && !memcmp(&out, &sets[t], sizeof(HitboxSet));
    }
    CHECK(exact == HB_HISTORY, "lag-comp rewind exact for %d / %d ticks", exact, HB_HISTORY);
    HitboxSet mid;
    hitbox_rewind(h, (1030u << 8) + 128, &mid);
    CHECK(fabsf(mid.c[0].a.z - (sets[30].c[0].a.z + sets[31].c[0].a.z) * 0.5f) < 1e-5f, "sub-tick rewind not interpolated");
    HitboxSet old;
    CHECK(!hitbox_rewind(h, 900u << 8, &old), "rewind outside the window must fail");
    free(h);
}

static void test_lod(void) {
    LodConfig c;
    lod_config_default(&c);
    int lod = 0, changes = 0;
    for (int i = 0; i < 2000; i++) {
        float px = c.threshold[0] + sinf((float)i * 0.37f) * c.threshold[0] * 0.1f;   // jitter +-10% around the boundary
        int n = lod_select(&c, lod, px);
        if (n != lod) changes++;
        lod = n;
    }
    CHECK(changes <= 1, "LOD oscillated %d times around a threshold", changes);
    // Monotonic sweep visits every LOD.
    lod = 0;
    int seen = 1;
    for (float px = 600; px > 5; px *= 0.97f) { int n = lod_select(&c, lod, px); if (n != lod) seen++; lod = n; }
    CHECK(seen == 4 && lod == 3, "sweep visited %d LODs, ended at %d", seen, lod);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    L = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    if (!anim_lib_load(L, 30.0f)) { printf("FAIL: library load\n"); return 1; }
    ItemDB *db = (ItemDB *)calloc(1, sizeof(ItemDB));
    items_load(db, "data/items.json", "data/weapons");
    test_load();
    test_rig_validation();
    test_pose_math();
    test_ik();
    test_hold_and_glue(db);
    test_phone_hold(db);
    test_holdpoint(db);
    test_frame_solve(db);
    test_camera_glide(db);
    test_clavicles_keep_clip(db);
    test_body_parts(db);
    test_damage_reaction();
    test_clip_facing();
    test_diagonal_clips();
    test_loop_seams();
    test_locomotion_smooth();
    test_upper_oneshots();
    test_determinism();
    test_quantization();
    test_lagcomp();
    test_lod();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
