// Animation system tests (run from the build output dir so assets/ and data/ resolve):
// FBX load, rig validation, pose math, IK, hold styles + hand glue, body-part hitboxes and
// damage multipliers, damage reactions, controller determinism, quantization, lag-comp
// rewind and LOD hysteresis.
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

// Every clip must face the canonical forward (-Z): the hips' and chest's forward at the first
// frame, compared with the bind pose (a rotated armature in the source file would turn the body
// against the hold / aim frame).
static float bone_facing_deg(const Skeleton *s, const Xform *model, int b) {
    quat d = quat_mul(model[b].r, quat_conj(s->bind_model[b].r));
    vec3 f = quat_rotate(d, v3(0, 0, -1));
    return atan2f(-f.x, -f.z) * DG_RAD2DEG;
}

static void test_clip_facing(void) {
    const Skeleton *s = &L->skel;
    for (int i = 0; i < L->clip_count; i++) {
        Pose p;
        Xform m[ANIM_MAX_BONES];
        clip_sample(&L->clips[i], 0, &p);
        pose_to_model(s, &p, m);
        float hips = bone_facing_deg(s, m, s->role[BR_HIPS]), chest = bone_facing_deg(s, m, s->role[BR_SPINE2]);
        printf("  facing %-24s hips %+7.1f chest %+7.1f deg\n", L->clips[i].name, hips, chest);
        CHECK(fabsf(chest) < 35.0f, "clip %s: chest faces %.0f deg off forward", L->clips[i].name, chest);
    }
}

// Diagonal movement: forward / backward clip with the hips turned towards the move direction,
// the chest still facing the aim.
static void test_diagonal_legs(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    const Skeleton *s = &L->skel;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    static const struct { float x, y; const char *name; } dirs[] = {
        { 0.707f, 0.707f, "forward-right" }, { -0.707f, 0.707f, "forward-left" },
        { 0.707f, -0.707f, "back-right" }, { -0.707f, -0.707f, "back-left" }, { 1, 0, "right" }, { 0, 1, "forward" },
    };
    for (int k = 0; k < (int)(sizeof(dirs) / sizeof(dirs[0])); k++) {
        charrt_init(rt, L, def);
        CharInputs in;
        memset(&in, 0, sizeof(in));
        in.grounded = true;
        in.speed = 3.0f;
        in.move_local = v2(dirs[k].x, dirs[k].y);
        for (int t = 0; t < 60; t++) { charrt_set_inputs(rt, &in); charrt_tick(rt, 1.0f / 60.0f); }
        charrt_eval_base(rt);
        int hips = s->role[BR_HIPS], chest = s->role[BR_SPINE2];
        // Forward of the hips / chest in model space (x right, -z forward).
        quat dh = quat_mul(rt->base_model[hips].r, quat_conj(s->bind_model[hips].r));
        quat dc = quat_mul(rt->base_model[chest].r, quat_conj(s->bind_model[chest].r));
        vec3 fh = quat_rotate(dh, v3(0, 0, -1)), fc = quat_rotate(dc, v3(0, 0, -1));
        float hip_deg = atan2f(fh.x, -fh.z) * DG_RAD2DEG, chest_deg = atan2f(fc.x, -fc.z) * DG_RAD2DEG;
        float move_deg = atan2f(dirs[k].x, dirs[k].y) * DG_RAD2DEG;
        float want = fabsf(move_deg) <= 60 ? move_deg : (fabsf(move_deg) >= 120 ? move_deg - (move_deg > 0 ? 180 : -180) : 0);
        printf("  diagonal %-13s hips %+6.1f (want %+6.1f) chest %+6.1f  blend dir %.2f,%.2f\n", dirs[k].name, hip_deg, want, chest_deg,
               ctrl_get(&rt->ctrl, AP_MOVE_X), ctrl_get(&rt->ctrl, AP_MOVE_Y));
        CHECK(fabsf(hip_deg - want) < 12.0f, "%s: hips face %.1f deg, want %.1f", dirs[k].name, hip_deg, want);
        CHECK(fabsf(chest_deg) < 20.0f, "%s: chest turned %.1f deg away from the aim", dirs[k].name, chest_deg);
        // The blend tree must see the clip direction: forward / backward on diagonals.
        vec2 bd = v2(ctrl_get(&rt->ctrl, AP_MOVE_X), ctrl_get(&rt->ctrl, AP_MOVE_Y));
        float bdeg = atan2f(bd.x, bd.y) * DG_RAD2DEG;
        CHECK(fabsf(wrap_angle((bdeg - (move_deg - want)) * DG_DEG2RAD)) < 6.0f * DG_DEG2RAD, "%s: blend tree direction %.0f deg, want %.0f", dirs[k].name, bdeg,
              move_deg - want);
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
    CHECK(anim_lib_find_clip(L, "jog_forward") >= 0 && anim_lib_find_clip(L, "Hanging Idle") >= 0, "expected clips missing");
    CHECK(L->ctrl.layer_count == 3, "controller layers %d", L->ctrl.layer_count);
    CHECK(L->set_count >= 2, "anim sets %d", L->set_count);
    CHECK(L->char_count >= 5, "characters %d", L->char_count);
    CHECK(L->ragdoll.body_count == 17, "ragdoll bodies %d", L->ragdoll.body_count);
    float mass = 0;
    for (int i = 0; i < L->ragdoll.body_count; i++) mass += L->ragdoll.bodies[i].mass;
    CHECK(fabsf(mass - 80.0f) < 0.01f, "ragdoll mass %.2f", mass);
    CHECK(L->meshes[0].lod_count == 4, "mesh LODs %d", L->meshes[0].lod_count);
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
    clip_sample(&L->clips[anim_lib_find_clip(L, "jog_forward")], 0.37f, &a);
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

typedef struct GlueStats { float max_pos, max_ang, max_item; int ticks; } GlueStats;

static Xform root_at(vec3 pos, float yaw) { return xf_make(pos, quat_yaw(yaw)); }

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
    // Item never moves between the right-hand solve and the end of the frame.
    Xform palm_r = xf_mul(rt->final_model[s->role[BR_R_HAND]], L->hands.palm[1]);
    Xform item_now = xf_mul(palm_r, xf_inverse(k->grip_r));
    float item_move = v3_dist(item_now.t, rt->proc.item_model.t) + quat_angle_between(item_now.r, rt->proc.item_model.r);
    g->max_item = fmaxf(g->max_item, item_move);
    if (k->has_left && proc_hold_style(&L->proc, in->weapon_type)->left_hand) {
        // Left palm <-> grip_l from the FINAL pose.
        Xform palm_l = xf_mul(rt->final_model[s->role[BR_L_HAND]], L->hands.palm[0]);
        Xform sock = xf_mul(rt->proc.item_model, k->grip_l);
        float pe = v3_dist(palm_l.t, sock.t), ae = quat_angle_between(palm_l.r, sock.r) * DG_RAD2DEG;
        if ((pe > 0.002f || ae > 1.0f) && g->max_pos <= 0.002f && g->max_ang <= 1.0f)
            printf("  glue %s/%s tick %d: %.2f mm %.2f deg (pitch %.2f ads %d crouch %d sprint %d, right err %.1f mm)\n", weapon, what, g->ticks, pe * 1000, ae,
                   in->aim_pitch, in->ads, in->crouch, in->sprint, rt->proc.right_err_pos * 1000);
        g->max_pos = fmaxf(g->max_pos, pe);
        g->max_ang = fmaxf(g->max_ang, ae);
    }
    g->ticks++;
}

static void test_hold_and_glue(const ItemDB *db) {
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
        // 1) Static hip / ADS placement: both hands within 1 cm of their sockets.
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
            Xform want_r = xf_mul(rt->proc.item_model, wd->sockets.grip_r);
            CHECK(v3_dist(palm_r.t, want_r.t) < 0.01f, "%s %s: right hand %.1f mm off grip_r", wd->id, ads ? "ADS" : "hip", v3_dist(palm_r.t, want_r.t) * 1000);
            CHECK(rt->proc.right_err_pos < 0.01f, "%s %s: right hand could not reach the desired item placement (%.1f mm)", wd->id,
                  ads ? "ADS" : "hip", rt->proc.right_err_pos * 1000);
            if (wd->sockets.has_left && proc_hold_style(&L->proc, style)->left_hand)
                CHECK(g.max_pos < 0.01f, "%s %s: left hand %.1f mm off grip_l", wd->id, ads ? "ADS" : "hip", g.max_pos * 1000);
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
        printf("  hold %-9s style %-9s glue max %.3f mm / %.3f deg, item drift %.2e\n", wd->id, wd->hold_style, g.max_pos * 1000, g.max_ang, g.max_item);
    }
    free(rt);
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
    const Clip *walk = &L->clips[anim_lib_find_clip(L, "jog_forward")];
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
    test_body_parts(db);
    test_damage_reaction();
    test_clip_facing();
    test_diagonal_legs();
    test_upper_oneshots();
    test_determinism();
    test_quantization();
    test_lagcomp();
    test_lod();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
