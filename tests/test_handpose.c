// Hand pose tests (docs/HANDPOSE.md), run from the build output dir (assets/ and data/ resolve):
// generated style defaults reproduce the old curl tables (bone for bone), inheritance and
// override resolution, lossless save / load, schema migration, mirror, joint limits, the
// contact solver and hand glue for every weapon with randomly tweaked poses.
#include "anim/anim_library.h"
#include "anim/character_rt.h"
#include "anim/handpose.h"
#include "editor/handpose/hp_discover.h"
#include "editor/handpose/hp_gizmo.h"
#include "editor/handpose/hp_undo.h"
#include "gameplay/items.h"
#include "core/engine.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include "core/log.h"
#include "cJSON.h"
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

static float frand(Rng *r, float lo, float hi) { return lo + (hi - lo) * (float)(rng_next(r) & 0xFFFFFF) / (float)0xFFFFFF; }

// ------------------------------------------------------------------ legacy reference

// The finger curl as it was before hand poses (src/anim/weapon_hold.c proc_curl_fingers).
static void legacy_curl(const Skeleton *s, Pose *pose, Xform *model, int side, const float *curl, const HandFrames *hf, float weight) {
    static const BoneRole first[2][5] = {
        { BR_L_THUMB1, BR_L_INDEX1, BR_L_MIDDLE1, BR_L_RING1, BR_L_PINKY1 },
        { BR_R_THUMB1, BR_R_INDEX1, BR_R_MIDDLE1, BR_R_RING1, BR_R_PINKY1 },
    };
    int hand = s->role[side ? BR_R_HAND : BR_L_HAND];
    if (hand < 0 || weight <= 0) return;
    for (int f = 0; f < 5; f++) {
        int b = s->role[first[side][f]];
        float ang = curl[f] * DG_DEG2RAD * weight;
        static const float split[3] = { 0.30f, 0.38f, 0.32f };
        for (int j = 0; j < 3 && b >= 0; j++) {
            Xform palm = xf_mul(model[hand], hf->palm[side]);
            vec3 axis = quat_rotate(palm.r, hf->curl_axis[side]);
            if (f == 0) axis = quat_rotate(palm.r, v3(0, 0, side ? 1.0f : -1.0f));
            quat q = quat_axis_angle(axis, ang * split[j] * 3.0f / 3.0f * (f == 0 ? 0.6f : 1.0f));
            Xform n = model[b];
            n.r = quat_norm(quat_mul(q, n.r));
            pose_set_model(s, pose, model, b, n);
            pose_update_children(s, pose, model, b);
            int next = -1;
            for (int c = b + 1; c < s->count; c++) if (s->parent[c] == b) { next = c; break; }
            b = next;
        }
    }
}

static float max_bone_diff(const Skeleton *s, const Xform *a, const Xform *b) {
    float m = 0;
    for (int i = 0; i < s->count; i++) m = fmaxf(m, v3_dist(a[i].t, b[i].t));
    return m;
}

static Pose g_pa, g_pb;
static Xform g_ma[ANIM_MAX_BONES], g_mb[ANIM_MAX_BONES];

// Every style's generated default resolves to its old curl, and applying it moves the bones as
// the old code did (within float rounding: the angle products are associated differently).
static void test_style_defaults(void) {
    const Skeleton *s = &L->skel;
    const HandPoseDB *db = &L->handposes;
    for (int i = 0; i < L->proc.style_count; i++) {
        const HoldStyle *hs = &L->proc.styles[i];
        int e = handpose_find_hash(db, handpose_style_hash(hs->name));
        CHECK(e >= 0, "style %s: no hand pose entry", hs->name);
        if (e < 0) continue;
        for (int h = 0; h < 2; h++) {
            HandPose p;
            handpose_resolve(db, e, h, HPS_HIP, &p);
            HandFingers want;
            handpose_from_curl(h ? hs->curl_r : hs->curl_l, want);
            float d = 0;
            for (int f = 0; f < 5; f++)
                for (int k = 0; k < 3; k++) d = fmaxf(d, fabsf(p.j[f][k].curl - want[f][k].curl) + fabsf(p.j[f][k].spread) + fabsf(p.j[f][k].twist));
            CHECK(d < 1e-4f, "style %s hand %d: resolved fingers differ from the curl table by %.5f deg", hs->name, h, d);
            CHECK(!p.has_socket && !p.has_pole, "style %s: generated default overrides a socket / pole", hs->name);
            pose_bind(s, &g_pa);
            pose_to_model(s, &g_pa, g_ma);
            pose_copy(&g_pb, &g_pa);
            memcpy(g_mb, g_ma, sizeof(Xform) * (size_t)s->count);
            legacy_curl(s, &g_pa, g_ma, h, h ? hs->curl_r : hs->curl_l, &L->hands, 1.0f);
            handpose_apply_fingers(s, &g_pb, g_mb, h, p.j, &L->hands, 1.0f);
            float bd = max_bone_diff(s, g_ma, g_mb);
            CHECK(bd < 1e-5f, "style %s hand %d: finger bones %.4f mm off the legacy curl", hs->name, h, bd * 1000);
        }
    }
    for (int g = 0; g < handpose_grip_count(); g++) CHECK(handpose_find_hash(db, handpose_grip_style_hash(g)) >= 0, "grip style %s missing", handpose_grip_name(g));
    printf("  style defaults: %d hold styles + %d grips reproduce the curl tables\n", L->proc.style_count, handpose_grip_count());
}

// ------------------------------------------------------------------ inheritance

static void test_inheritance(void) {
    HandPoseDB db;
    handpose_db_init(&db);
    int a = handpose_add(&db, "style/t_a", HPC_STYLE);
    db.e[a].layer[1][HPS_DEFAULT].set = HPL_FINGER(HF_INDEX) | HPL_FINGER(HF_MIDDLE);
    db.e[a].layer[1][HPS_DEFAULT].j[HF_INDEX][0].curl = 10;
    db.e[a].layer[1][HPS_DEFAULT].j[HF_MIDDLE][0].curl = 11;
    db.e[a].layer[1][HPS_ADS].set = HPL_FINGER(HF_INDEX) | HPL_POLE;
    db.e[a].layer[1][HPS_ADS].j[HF_INDEX][0].curl = 20;
    db.e[a].layer[1][HPS_ADS].pole = v3(1, 2, 3);
    int b = handpose_add(&db, "t_b", HPC_WEAPON);
    dg_strcpy(db.e[b].inherits, sizeof(db.e[b].inherits), "style/t_a");
    db.e[b].layer[1][HPS_DEFAULT].set = HPL_FINGER(HF_INDEX) | HPL_SOCKET;
    db.e[b].layer[1][HPS_DEFAULT].j[HF_INDEX][0].curl = 30;
    db.e[b].layer[1][HPS_DEFAULT].socket = xf_make(v3(0.01f, 0, 0), quat_identity());
    db.e[b].layer[1][HPS_SPRINT].set = HPL_FINGER(HF_MIDDLE);
    db.e[b].layer[1][HPS_SPRINT].j[HF_MIDDLE][0].curl = 40;
    int c = handpose_add(&db, "t_c", HPC_ATTACHMENT);   // no inherits: the runtime base supplies the style
    db.e[c].layer[0][HPS_DEFAULT].set = HPL_FINGER(HF_RING);
    db.e[c].layer[0][HPS_DEFAULT].j[HF_RING][1].curl = 50;
    db.e[a].layer[0][HPS_DEFAULT].set = HPL_FINGER(HF_PINKY);
    db.e[a].layer[0][HPS_DEFAULT].j[HF_PINKY][2].curl = 5;
    handpose_link(&db);
    HandPose p;
    handpose_resolve(&db, b, 1, HPS_HIP, &p);
    CHECK(p.j[HF_INDEX][0].curl == 30 && p.j[HF_MIDDLE][0].curl == 11, "hip: entry default over style default (%.0f, %.0f)", p.j[HF_INDEX][0].curl,
          p.j[HF_MIDDLE][0].curl);
    CHECK(p.has_socket && p.socket.t.x == 0.01f && !p.has_pole, "hip: socket override, no pole");
    handpose_resolve(&db, b, 1, HPS_ADS, &p);
    CHECK(p.j[HF_INDEX][0].curl == 30, "ads: the entry's default beats the style's per-state value (%.0f)", p.j[HF_INDEX][0].curl);
    CHECK(p.has_pole && p.pole.y == 2, "ads: style per-state pole inherited");
    handpose_resolve(&db, b, 1, HPS_SPRINT, &p);
    CHECK(p.j[HF_MIDDLE][0].curl == 40 && p.j[HF_INDEX][0].curl == 30, "sprint: entry per-state finger");
    handpose_resolve(&db, a, 1, HPS_ADS, &p);
    CHECK(p.j[HF_INDEX][0].curl == 20, "style alone: per-state value");
    handpose_resolve_over(&db, a, c, 0, HPS_HIP, &p);
    CHECK(p.j[HF_RING][1].curl == 50 && p.j[HF_PINKY][2].curl == 5, "resolve_over: base style under an entry without inherits");
    handpose_resolve_over(&db, a, b, 1, HPS_SPRINT, &p);
    CHECK(p.j[HF_MIDDLE][0].curl == 40 && p.j[HF_INDEX][0].curl == 30, "resolve_over: base already in the chain applies once");
    handpose_resolve(&db, -1, 1, HPS_HIP, &p);
    CHECK(!p.has_socket && p.j[HF_INDEX][0].curl == 0 && p.ik_weight == 1 && p.wrist.r.w == 1, "no entry: open hand, identity");
    // Cycle: a -> b -> a is cut, resolution terminates.
    dg_strcpy(db.e[a].inherits, sizeof(db.e[a].inherits), "t_b");
    handpose_link(&db);
    handpose_resolve(&db, b, 1, HPS_HIP, &p);
    CHECK(db.e[a].parent < 0 || db.e[b].parent < 0, "inheritance cycle cut");
    // Hash: stubs don't change it, overrides do.
    dg_strcpy(db.e[a].inherits, sizeof(db.e[a].inherits), "");
    handpose_link(&db);
    handpose_rehash(&db);
    u32 h0 = db.hash;
    int stub = handpose_add(&db, "t_stub", HPC_ITEM);
    dg_strcpy(db.e[stub].inherits, sizeof(db.e[stub].inherits), "style/t_a");
    db.e[stub].reviewed = true;
    handpose_link(&db);
    handpose_rehash(&db);
    CHECK(db.hash == h0, "a stub (no overrides) changes the net hash");
    db.e[stub].layer[1][HPS_DEFAULT].set = HPL_IKW;
    db.e[stub].layer[1][HPS_DEFAULT].ik_weight = 0.5f;
    handpose_rehash(&db);
    CHECK(db.hash != h0, "an override doesn't change the net hash");
    handpose_db_free(&db);
}

// ------------------------------------------------------------------ files

static void random_layer(Rng *r, HandPoseLayer *l) {
    handpose_layer_clear(l);
    l->set = (u16)(rng_next(r) & 0x3FF);
    l->socket = xf_make(v3(frand(r, -0.2f, 0.2f), frand(r, -0.2f, 0.2f), frand(r, -0.4f, 0.1f)),
                        quat_norm((quat){ frand(r, -1, 1), frand(r, -1, 1), frand(r, -1, 1), frand(r, 0.1f, 1) }));
    l->wrist = xf_make(v3(frand(r, -0.02f, 0.02f), frand(r, -0.02f, 0.02f), frand(r, -0.02f, 0.02f)), handpose_euler_deg_to_quat(v3(frand(r, -20, 20), frand(r, -20, 20), 0)));
    l->pole = v3(frand(r, -1, 1), frand(r, -1, 0), frand(r, -1, 1));
    l->ik_weight = frand(r, 0, 1);
    for (int f = 0; f < 5; f++)
        for (int k = 0; k < 3; k++) l->j[f][k] = handpose_clamp_joint(f, k, (HandJoint){ frand(r, -30, 110), frand(r, -40, 40), frand(r, -30, 30) });
    if (l->set & HPL_CONTACTS) {
        l->contact_count = (u8)(1 + rng_next(r) % 4);
        for (int c = 0; c < l->contact_count; c++) l->contacts[c] = (HandContact){ (u8)(c + 1), v3(frand(r, -0.05f, 0.05f), frand(r, -0.05f, 0.05f), frand(r, -0.3f, 0)) };
    }
}

static bool layer_equal(const HandPoseLayer *a, const HandPoseLayer *b) {
    if (a->set != b->set) return false;
#define XEQ(P, Q) (P.t.x == Q.t.x && P.t.y == Q.t.y && P.t.z == Q.t.z && P.r.x == Q.r.x && P.r.y == Q.r.y && P.r.z == Q.r.z && P.r.w == Q.r.w)
    if ((a->set & HPL_SOCKET) && !XEQ(a->socket, b->socket)) return false;
    if ((a->set & HPL_WRIST) && !XEQ(a->wrist, b->wrist)) return false;
#undef XEQ
    if ((a->set & HPL_POLE) && (a->pole.x != b->pole.x || a->pole.y != b->pole.y || a->pole.z != b->pole.z)) return false;
    if ((a->set & HPL_IKW) && a->ik_weight != b->ik_weight) return false;
    for (int f = 0; f < 5; f++)
        if ((a->set & HPL_FINGER(f)) && memcmp(a->j[f], b->j[f], sizeof(a->j[f]))) return false;
    if (a->set & HPL_CONTACTS) {
        if (a->contact_count != b->contact_count) return false;
        for (int c = 0; c < a->contact_count; c++)
            if (a->contacts[c].finger != b->contacts[c].finger || memcmp(&a->contacts[c].point, &b->contacts[c].point, sizeof(vec3))) return false;
    }
    return true;
}

static void test_roundtrip(void) {
    Rng r = { 0x1234, 0x77 };
    HandPoseDB db;
    handpose_db_init(&db);
    int bad = 0;
    for (int trial = 0; trial < 50; trial++) {
        int i = handpose_add(&db, "rt_entry", HPC_WEAPON);
        HandPoseEntry *e = &db.e[i];
        dg_strcpy(e->label, sizeof(e->label), "Round \"trip\"");
        dg_strcpy(e->source, sizeof(e->source), "data/weapons/x.json");
        dg_strcpy(e->inherits, sizeof(e->inherits), "style/rifle");
        e->hands = 3;
        e->states = (u32)(rng_next(&r) & ((1u << HPS_COUNT) - 1)) | 1u;
        e->reviewed = trial & 1;
        e->auto_placed = trial & 2;
        e->relative = trial & 4;
        for (int h = 0; h < 2; h++)
            for (int s = 0; s < HPS_COUNT; s++) {
                random_layer(&r, &e->layer[h][s]);
                if (rng_next(&r) % 3 == 0) e->layer[h][s].set = 0;
            }
        char *txt = handpose_entry_write(e);
        cJSON *root = cJSON_Parse(txt);
        HandPoseEntry back;
        memset(&back, 0, sizeof(back));
        dg_strcpy(back.id, sizeof(back.id), "rt_entry");
        char err[128];
        bool ok = root && handpose_entry_parse(&back, root, err, sizeof(err));
        char *txt2 = ok ? handpose_entry_write(&back) : NULL;
        bool same = ok && txt2 && !strcmp(txt, txt2) && back.states == e->states && back.reviewed == e->reviewed && back.relative == e->relative &&
                    !strcmp(back.label, e->label) && !strcmp(back.inherits, e->inherits);
        for (int h = 0; h < 2 && same; h++)
            for (int s = 0; s < HPS_COUNT && same; s++) same = layer_equal(&e->layer[h][s], &back.layer[h][s]);
        if (!same && !bad++) printf("  round trip mismatch:\n%s\n---\n%s\n", txt, txt2 ? txt2 : "(parse failed)");
        cJSON_Delete(root);
        free(txt);
        free(txt2);
    }
    CHECK(bad == 0, "save / load round trip lossy in %d of 50 random entries", bad);
    handpose_db_free(&db);
}

static void test_migration(void) {
    const char *v0 = "{ \"inherits\": \"style/rifle\", \"curl_r\": [10, 20, 30, 40, 50], \"curl_l\": [1, 2, 3, 4, 5], \"left\": { \"ads\": { \"ik_weight\": 0.5 } } }";
    cJSON *root = cJSON_Parse(v0);
    HandPoseEntry e;
    memset(&e, 0, sizeof(e));
    dg_strcpy(e.id, sizeof(e.id), "legacy");
    char err[128];
    CHECK(handpose_entry_parse(&e, root, err, sizeof(err)), "schema 0 parse: %s", err);
    float cr[5] = { 10, 20, 30, 40, 50 };
    HandFingers want;
    handpose_from_curl(cr, want);
    CHECK((e.layer[1][HPS_DEFAULT].set & HPL_FINGERS_ALL) == HPL_FINGERS_ALL && !memcmp(e.layer[1][HPS_DEFAULT].j, want, sizeof(want)), "schema 0 curl_r -> right default fingers");
    CHECK(e.layer[0][HPS_ADS].set == HPL_IKW && e.layer[0][HPS_ADS].ik_weight == 0.5f, "schema 0: existing hand blocks kept");
    char *txt = handpose_entry_write(&e);
    CHECK(strstr(txt, "\"schema\": 1") != NULL, "migrated entry written at the current schema");
    free(txt);
    cJSON_Delete(root);
    cJSON *future = cJSON_Parse("{ \"schema\": 99 }");
    CHECK(!handpose_entry_parse(&e, future, err, sizeof(err)), "a newer schema is refused");
    cJSON_Delete(future);
}

// ------------------------------------------------------------------ mirror / limits

static void test_mirror_limits(void) {
    Rng r = { 99, 3 };
    for (int i = 0; i < 100; i++) {
        HandPoseLayer l, m, mm;
        random_layer(&r, &l);
        handpose_mirror_layer(&l, &m);
        handpose_mirror_layer(&m, &mm);
        vec3 p = v3(0.03f, -0.02f, -0.05f);
        vec3 a = xf_point(l.socket, p), b = xf_point(m.socket, v3(-p.x, p.y, p.z));
        CHECK(fabsf(a.x + b.x) < 1e-5f && fabsf(a.y - b.y) < 1e-5f && fabsf(a.z - b.z) < 1e-5f, "mirror: a mirrored point doesn't land mirrored");
        vec3 x = quat_rotate(m.socket.r, v3(1, 0, 0)), y = quat_rotate(m.socket.r, v3(0, 1, 0)), z = quat_rotate(m.socket.r, v3(0, 0, 1));
        CHECK(v3_dot(v3_cross(x, y), z) > 0.999f, "mirror: frame not right-handed");
        CHECK(v3_dist(mm.socket.t, l.socket.t) < 1e-6f && quat_angle_between(mm.socket.r, l.socket.r) < 1e-4f && mm.pole.x == l.pole.x,
              "mirror twice != identity");
        CHECK(!memcmp(m.j, l.j, sizeof(l.j)), "mirror: finger values are hand-relative (unchanged)");
    }
    // Mirrored finger pose on the skeleton: the left fingertips mirror the right ones.
    const Skeleton *s = &L->skel;
    HandFingers j;
    handpose_preset("pinch", j);
    j[HF_INDEX][0].spread = 12;
    j[HF_MIDDLE][1].twist = 6;
    pose_bind(s, &g_pa);
    pose_to_model(s, &g_pa, g_ma);
    vec3 rh = g_ma[s->role[BR_R_HAND]].t, lh = g_ma[s->role[BR_L_HAND]].t;
    handpose_apply_fingers(s, &g_pa, g_ma, 1, j, &L->hands, 1.0f);
    handpose_apply_fingers(s, &g_pa, g_ma, 0, j, &L->hands, 1.0f);
    float worst = 0;
    for (int f = 0; f < 5; f++) {
        vec3 tr = v3_sub(handpose_fingertip(s, g_ma, 1, f), rh), tl = v3_sub(handpose_fingertip(s, g_ma, 0, f), lh);
        worst = fmaxf(worst, v3_dist(tr, v3(-tl.x, tl.y, tl.z)));
    }
    printf("  mirror: L/R fingertips with the same pose differ by %.2f mm from mirrored (rig asymmetry)\n", worst * 1000);
    CHECK(worst < 0.006f, "mirror on the rig: fingertips %.1f mm from mirrored", worst * 1000);
    // Limits.
    HandJoint big = handpose_clamp_joint(HF_INDEX, 1, (HandJoint){ 500, 90, -90 });
    HandJoint lo, hi;
    handpose_joint_limits(HF_INDEX, 1, &lo, &hi);
    CHECK(big.curl == hi.curl && big.spread == hi.spread && big.twist == lo.twist, "joint limits not enforced");
    HandPoseLayer l;
    handpose_layer_clear(&l);
    cJSON *o = cJSON_Parse("{ \"right\": { \"default\": { \"fingers\": { \"thumb\": [[999, -999, 999], [0, 0, 0], [0, 0, 0]] } } } }");
    HandPoseEntry e;
    memset(&e, 0, sizeof(e));
    char err[64];
    handpose_entry_parse(&e, o, err, sizeof(err));
    handpose_joint_limits(HF_THUMB, 0, &lo, &hi);
    CHECK(e.layer[1][HPS_DEFAULT].j[HF_THUMB][0].curl == hi.curl && e.layer[1][HPS_DEFAULT].j[HF_THUMB][0].spread == lo.spread, "limits not enforced on load");
    cJSON_Delete(o);
    for (int i = 0; i < handpose_preset_count(); i++) CHECK(handpose_preset(handpose_preset_name(i), j), "preset %s", handpose_preset_name(i));
}

// ------------------------------------------------------------------ contact solver

static void test_contact(void) {
    const Skeleton *s = &L->skel;
    pose_bind(s, &g_pa);
    pose_to_model(s, &g_pa, g_ma);
    int worst_f = -1;
    float worst = 0;
    for (int side = 0; side < 2; side++)
        for (int f = 1; f < 5; f++) {
            // A bar under the finger, in the palm frame (palm side = -Y), 2.5 cm below the knuckles.
            Xform palm = xf_mul(g_ma[s->role[side ? BR_R_HAND : BR_L_HAND]], L->hands.palm[side]);
            vec3 tip = handpose_fingertip(s, g_ma, side, f);
            HandBox box = { v3_add(tip, quat_rotate(palm.r, v3(0, -0.035f, 0.01f))), v3(0.05f, 0.012f, 0.012f), palm.r };
            HandFingers j;
            memset(j, 0, sizeof(j));
            HandContactResult r = handpose_contact_solve(s, &g_pa, g_ma, side, f, j, &L->hands, &box, 1, NULL, 0.008f, 0.0005f);
            CHECK(r.touched, "contact %s %s: never touched the bar", side ? "R" : "L", handpose_finger_name(f));
            CHECK(r.tip_dist >= 0 && r.tip_dist <= 0.001f, "contact %s %s: stopped %.2f mm from the surface", side ? "R" : "L", handpose_finger_name(f), r.tip_dist * 1000);
            CHECK(j[f][0].curl > 0, "contact: finger didn't close");
            if (r.tip_dist > worst) { worst = r.tip_dist; worst_f = f; }
            // Curled 25 deg past the contact: the solver starts from open again and ends on the surface.
            HandFingers k;
            memcpy(k, j, sizeof(k));
            for (int q = 0; q < 3; q++) k[f][q].curl = fminf(k[f][q].curl + 25.0f * (q == 0 ? 0.30f : q == 1 ? 0.38f : 0.32f), 90.0f);
            HandFingers probe;
            memcpy(probe, k, sizeof(probe));
            HandContactResult in0 = handpose_contact_solve(s, &g_pa, g_ma, side, f, probe, &L->hands, &box, 1, NULL, 0.008f, 0.0005f);
            r = in0;
            CHECK(r.touched && !r.blocked, "contact from a curled finger %s %s: no contact", side ? "R" : "L", handpose_finger_name(f));
            CHECK(r.tip_dist >= 0 && r.tip_dist <= 0.001f, "contact from inside %s %s: %.2f mm", side ? "R" : "L", handpose_finger_name(f), r.tip_dist * 1000);
        }
    printf("  contact solver: worst surface gap %.3f mm (%s)\n", worst * 1000, handpose_finger_name(worst_f));
    // Inside the geometry even when open: blocked, joints untouched.
    {
        Xform palm = xf_mul(g_ma[s->role[BR_R_HAND]], L->hands.palm[1]);
        HandBox inside = { palm.t, v3(0.2f, 0.2f, 0.2f), palm.r };
        HandFingers j0;
        memset(j0, 0, sizeof(j0));
        j0[HF_INDEX][1].curl = 33;
        HandContactResult rb = handpose_contact_solve(s, &g_pa, g_ma, 1, HF_INDEX, j0, &L->hands, &inside, 1, NULL, 0.008f, 0.0005f);
        CHECK(rb.blocked && !rb.touched && j0[HF_INDEX][1].curl == 33, "finger inside the item when open: blocked, unchanged");
    }
    // Nothing to touch + a target: the closest curl to the target.
    HandFingers j;
    memset(j, 0, sizeof(j));
    HandBox far = { v3(10, 10, 10), v3(0.01f, 0.01f, 0.01f), quat_identity() };
    vec3 tgt = handpose_fingertip(s, g_ma, 1, HF_INDEX);
    tgt = v3_add(tgt, v3(0, -0.03f, 0));
    HandContactResult r = handpose_contact_solve(s, &g_pa, g_ma, 1, HF_INDEX, j, &L->hands, &far, 1, &tgt, 0.008f, 0.0005f);
    CHECK(!r.touched && j[HF_INDEX][0].curl > 0, "target without contact: the finger moves towards it");
}

// ------------------------------------------------------------------ glue with tweaks

// Every weapon / item at hip and ADS with random finger, wrist and socket tweaks: the hands stay
// glued to the (tweaked) sockets - the guarantee doesn't depend on the data.
static void test_glue_tweaks(const ItemDB *idb) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) { CHECK(false, "player character missing"); return; }
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    HandPoseDB *db = &L->handposes;
    Rng r = { 42, 7 };
    const Skeleton *s = &L->skel;
    float worst = 0;
    int tested = 0;
    // Every weapon and every held item (with its landscape style too: the phone's two-handed hold).
    for (int n = 0; n < idb->weapon_count + idb->item_count; n++) {
        const ItemSockets *k;
        const char *id, *style_name;
        int cat;
        if (n < idb->weapon_count) {
            k = &idb->weapons[n].sockets;
            id = idb->weapons[n].id;
            style_name = idb->weapons[n].hold_style;
            cat = HPC_WEAPON;
        } else {
            const ItemDef *it = &idb->items[n - idb->weapon_count];
            if (n - idb->weapon_count < 1 || !it->has_hold || it->weapon >= 0) continue;
            k = &it->sockets;
            id = it->id;
            style_name = it->hold_style_alt[0] ? it->hold_style_alt : it->hold_style;
            cat = HPC_ITEM;
        }
        int style = proc_hold_style_index(&L->proc, style_name);
        if (style < 0) continue;
        char eid[96];
        handpose_entry_id(eid, sizeof(eid), cat, id);
        int e = handpose_add(db, eid, cat);
        HandPoseEntry *en = &db->e[e];
        handpose_entry_clear_layers(en);
        snprintf(en->inherits, sizeof(en->inherits), "style/%s", style_name);
        for (int h = 0; h < 2; h++) {
            HandPoseLayer *l = &en->layer[h][HPS_DEFAULT];
            l->set = HPL_FINGERS_ALL | HPL_WRIST | (h == 0 ? HPL_SOCKET : 0);
            for (int f = 0; f < 5; f++)
                for (int q = 0; q < 3; q++) l->j[f][q] = handpose_clamp_joint(f, q, (HandJoint){ frand(&r, 0, 90), frand(&r, -10, 10), frand(&r, -8, 8) });
            l->wrist = xf_make(v3(frand(&r, -0.005f, 0.005f), frand(&r, -0.005f, 0.005f), frand(&r, -0.005f, 0.005f)), handpose_euler_deg_to_quat(v3(frand(&r, -8, 8), frand(&r, -8, 8), frand(&r, -8, 8))));
            l->socket = xf_mul(k->grip_l, xf_make(v3(frand(&r, -0.01f, 0.01f), frand(&r, -0.01f, 0.01f), frand(&r, -0.02f, 0.02f)), handpose_euler_deg_to_quat(v3(frand(&r, -10, 10), frand(&r, -10, 10), 0))));
        }
        handpose_link(db);
        handpose_rehash(db);
        for (int ads = 0; ads < 2; ads++) {
            charrt_init(rt, L, def);
            CharInputs in;
            memset(&in, 0, sizeof(in));
            in.grounded = true;
            in.weapon_type = style;
            in.ads = ads != 0;
            for (int t = 0; t < 30; t++) {
                in.aim_pitch = frand(&r, -0.6f, 0.6f) * (t > 20 ? 1.0f : 0.0f);
                charrt_set_inputs(rt, &in);
                charrt_tick(rt, 1.0f / 60.0f);
                charrt_eval_base(rt);
                ProcInput pi;
                charrt_proc_input(rt, &pi, xf_identity(), 1.0f / 60.0f);
                pi.item = k;
                pi.hand_ik_weight = 1.0f;
                charrt_post(rt, &pi);
            }
            // Right hand: the item is finalized from it (exact by construction); left: on its socket.
            Xform palm_r = xf_mul(rt->final_model[s->role[BR_R_HAND]], L->hands.palm[1]);
            float er = v3_dist(xf_mul(palm_r, xf_inverse(rt->proc.hand[1].sock)).t, rt->proc.item_model.t);
            CHECK(er < 1e-4f, "%s: item not finalized from the tweaked right socket (%.2f mm)", id, er * 1000);
            if (k->has_left && proc_hold_style(&L->proc, style)->left_hand) {
                Xform sock = rt->proc.hand[0].sock;
                sock.t.z += rt->proc.grip_l_slide;
                sock = xf_mul(rt->proc.item_model, sock);
                Xform palm_l = xf_mul(rt->final_model[s->role[BR_L_HAND]], L->hands.palm[0]);
                float pe = v3_dist(palm_l.t, sock.t), ae = quat_angle_between(palm_l.r, sock.r) * DG_RAD2DEG;
                CHECK(pe < 0.002f && ae < 1.0f, "%s %s: left hand %.2f mm / %.2f deg off its tweaked socket", id, ads ? "ADS" : "hip", pe * 1000, ae);
                worst = fmaxf(worst, pe);
            }
            tested++;
        }
        handpose_entry_clear_layers(&db->e[e]);
    }
    anim_lib_reload_handposes(L);
    printf("  glue with random pose tweaks: %d weapon / item x state runs, worst left hand %.3f mm\n", tested, worst * 1000);
    free(rt);
}

// ------------------------------------------------------------------ discovery

// A plain text scan of the asset files (what `grep` sees), independent of the parsers.
static int grep_sources(char ids[][64], int max) {
    int n = 0;
    // items.json: one chunk per "id" (an item may span several lines).
    char *txt = platform_read_file("data/items.json", NULL);
    for (char *id = txt ? strstr(txt, "\"id\":") : NULL; id && n < max;) {
        char *next = strstr(id + 5, "\"id\":");
        char save = 0;
        if (next) { save = *next; *next = 0; }
        char v[48];
        if (strstr(id, "\"hold\"") && !strstr(id, "\"kind\": \"weapon\"") && sscanf(id, "\"id\": \"%47[^\"]\"", v) == 1) snprintf(ids[n++], 64, "%s", v);
        if (next) *next = save;
        id = next;
    }
    free(txt);
    static const struct { const char *dir, *needle, *prefix; } dirs[] = {
        { "data/weapons", "\"hold\"", "" }, { "assets/attachments", "\"grip_l\"", "att_" }, { "assets/vehicles", "\"driver\": true", "veh_" },
    };
    for (int d = 0; d < 3; d++) {
        int count = 0;
        char base[512];
        platform_data_path(dirs[d].dir, base, sizeof(base));
        char **files = SDL_GlobDirectory(base, "*.json", 0, &count);
        for (int i = 0; files && i < count && n < max; i++) {
            char path[800];
            snprintf(path, sizeof(path), "%s/%s", base, files[i]);
            char *t = platform_read_file(path, NULL);
            if (t && strstr(t, dirs[d].needle)) {
                char v[48] = "";
                const char *key = d == 2 ? "\"name\":" : "\"id\":";
                char *k = strstr(t, key);
                if (k && sscanf(k + strlen(key), " \"%47[^\"]\"", v) == 1) snprintf(ids[n++], 64, "%s%s", dirs[d].prefix, v);
            }
            free(t);
        }
        SDL_free(files);
    }
    return n;
}

static int count_files(const char *dir) {
    int count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    SDL_free(files);
    return files ? count : 0;
}

// Removes every *.json in dir (and dir/styles), then the directories.
static void clear_dir(const char *dir) {
    const char *subs[2] = { "/styles", "" };
    for (int k = 0; k < 2; k++) {
        char d[400];
        snprintf(d, sizeof(d), "%s%s", dir, subs[k]);
        int count = 0;
        char **files = SDL_GlobDirectory(d, "*.json", 0, &count);
        for (int i = 0; files && i < count; i++) {
            char p[600];
            snprintf(p, sizeof(p), "%s/%s", d, files[i]);
            remove(p);
        }
        SDL_free(files);
        SDL_RemovePath(d);
    }
}

static void test_discovery(const ItemDB *idb) {
    // 1) Discovery == the text scan of the assets.
    HandPoseSourceList src = { 0 };
    hp_sources_from_items(&src, idb, &L->proc);
    hp_sources_from_vehicle_dir(&src, "assets/vehicles");
    static char grep_ids[128][64];
    int gn = grep_sources(grep_ids, 128);
    int matched = 0;
    for (int g = 0; g < gn; g++) {
        bool found = false;
        for (int k = 0; k < src.count && !found; k++) {
            char id[96];
            handpose_entry_id(id, sizeof(id), src.v[k].category, src.v[k].name);
            found = !strcmp(id, grep_ids[g]);
        }
        CHECK(found, "discovery missed %s (found by the text scan)", grep_ids[g]);
        matched += found;
    }
    CHECK(gn == src.count, "discovery found %d sources, the text scan %d", src.count, gn);
    printf("  discovery: %d sources, text scan %d, matched %d\n", src.count, gn, matched);
    // Hands / states of a few known sources.
    for (int k = 0; k < src.count; k++) {
        const HandPoseSource *s = &src.v[k];
        if (s->category == HPC_WEAPON && !strcmp(s->name, "rifle"))
            CHECK(s->hands == 3 && (s->states & (1u << HPS_ADS)) && (s->states & (1u << HPS_SPRINT)) && !strcmp(s->inherits, "style/rifle"), "rifle source: hands %d states %x %s", s->hands, s->states, s->inherits);
        if (s->category == HPC_WEAPON && !strcmp(s->name, "knife")) CHECK(s->hands == 2, "knife: one hand (%d)", s->hands);
        if (s->category == HPC_ITEM && !strcmp(s->name, "phone"))
            CHECK((s->states & (1u << HPS_PHONE_PORTRAIT)) && (s->states & (1u << HPS_PHONE_LANDSCAPE)) && (s->hands & 1), "phone: portrait + landscape, both hands (%x, %d)", s->states, s->hands);
        if (s->category == HPC_ATTACHMENT) CHECK(s->hands == 1 && s->inherits[0], "attachment grip %s: left hand, inherits a host style", s->name);
        if (s->category == HPC_VEHICLE) CHECK(s->relative && (s->states & (1u << HPS_DRIVE)) && s->hands == 3, "vehicle %s: relative, drive, both hands", s->name);
    }

    // 2) New sources: an item added to items.json and an interactable placed in the dev room
    // (a type with no hand target in code) -> exactly one stub each, nothing else written.
    const char *tmp = "test_handpose_tmp";
    char dir[300], rel[300];
    snprintf(dir, sizeof(dir), "%s/handposes", tmp);
    clear_dir(dir);
    SDL_CreateDirectory(dir);
    char *items = platform_read_file("data/items.json", NULL);
    char *at = items ? strstr(items, "\"items\": [") : NULL;
    CHECK(at != NULL, "items.json layout");
    if (!at) return;
    at += strlen("\"items\": [");
    char *mod = (char *)malloc(strlen(items) + 512);
    size_t pre = (size_t)(at - items);
    memcpy(mod, items, pre);
    sprintf(mod + pre, "\n    { \"id\": \"test_crowbar\", \"name\": \"Crowbar\", \"kind\": \"misc\", \"size\": [0.02, 0.3, 0.02], \"hold\": { \"style\": \"item_large\" } },%s", at);
    snprintf(rel, sizeof(rel), "%s/items_plus.json", tmp);
    platform_write_file(rel, mod, strlen(mod));
    ItemDB *db2 = (ItemDB *)calloc(1, sizeof(ItemDB));
    items_load(db2, rel, "data/weapons");
    HandPoseSourceList src2 = { 0 };
    hp_sources_from_items(&src2, db2, &L->proc);
    hp_sources_from_vehicle_dir(&src2, "assets/vehicles");
    hp_source_from_interactable(&src2, "test_crank", 1 /* both hands */, 1.5f, false, "interactable:test_crank (level:devroom)");
    HandPoseDB db;
    handpose_db_init(&db);
    handpose_add_style_defaults(&db, &L->proc);
    HandPoseScanOpts o = { dir, NULL, false };
    HandPoseScanReport r;
    hp_scan(&db, &src, &o, &r);   // the shipped assets first
    int base_files = count_files(dir);
    CHECK(r.created == src.count && base_files == src.count, "first scan: %d created, %d files for %d sources", r.created, base_files, src.count);
    hp_scan(&db, &src2, &o, &r);
    CHECK(r.created == 2 && r.written == 2 && count_files(dir) == base_files + 2, "new item + interactable: %d created, %d written (want 2)", r.created, r.written);
    int ci = handpose_find(&db, "test_crowbar"), ii = handpose_find(&db, "use_test_crank");
    CHECK(ci >= 0 && ii >= 0, "stub entries missing");
    if (ci >= 0) {
        const HandPoseEntry *e = &db.e[ci];
        CHECK(!strcmp(e->inherits, "style/item_large") && e->hands == (2 | (proc_hold_style(&L->proc, proc_hold_style_index(&L->proc, "item_large"))->left_hand && db2->items[1].sockets.has_left ? 1 : 0)) &&
              !e->reviewed && !handpose_entry_has_overrides(e) && e->category == HPC_ITEM,
              "crowbar stub: inherits %s hands %d reviewed %d", e->inherits, e->hands, e->reviewed);
        CHECK(e->states == hp_style_states(&L->proc, "item_large"), "crowbar stub states %x", e->states);
    }
    if (ii >= 0) {
        const HandPoseEntry *e = &db.e[ii];
        CHECK(e->hands == 3 && (e->states & (1u << HPS_USE)) && (e->states & (1u << HPS_HOLD)) && e->relative && e->auto_placed && !e->reviewed &&
              !strcmp(e->inherits, "style/grip_grab"),
              "crank stub: hands %d states %x relative %d auto %d", e->hands, e->states, e->relative, e->auto_placed);
        CHECK((e->layer[0][HPS_DEFAULT].set & HPL_SOCKET) && (e->layer[1][HPS_DEFAULT].set & HPL_SOCKET) && e->layer[0][HPS_DEFAULT].socket.t.x < 0,
              "auto-placed sockets on both hands, apart");
    }
    // The stub files read back as the same entries.
    HandPoseDB back;
    handpose_db_init(&back);
    handpose_db_load_dir(&back, dir);
    int bi = handpose_find(&back, "use_test_crank");
    CHECK(bi >= 0 && back.e[bi].auto_placed && back.e[bi].hands == 3 && back.e[bi].states == db.e[ii].states, "stub file round trip");
    handpose_db_free(&back);
    // Idempotent: a rescan creates nothing.
    hp_scan(&db, &src2, &o, &r);
    CHECK(r.created == 0 && r.written == 0, "rescan created %d", r.created);
    // Never over a file: an existing file for a source the DB doesn't know stays untouched.
    char path[400];
    snprintf(path, sizeof(path), "%s/test_kept.json", dir);
    const char *mine = "{ \"schema\": 1, \"reviewed\": true, \"note\": \"hand-made\" }\n";
    platform_write_file(path, mine, strlen(mine));
    hp_source_from_interactable(&src2, "test_kept_src", 0, 0, true, "interactable:x");
    snprintf(src2.v[src2.count - 1].name, sizeof(src2.v[0].name), "kept");
    src2.v[src2.count - 1].category = HPC_ITEM;
    snprintf(src2.v[src2.count - 1].name, sizeof(src2.v[0].name), "test_kept");
    hp_scan(&db, &src2, &o, &r);
    char *now = platform_read_file(path, NULL);
    CHECK(now && !strcmp(now, mine), "an existing file was overwritten by a stub");
    free(now);
    src2.count--;
    remove(path);
    // 3) Remove both sources: their stubs are orphaned, reported, removed only on request.
    src.categories |= 1u << HPC_INTERACT;   // the game's list always covers the registered interactables
    hp_scan(&db, &src, &o, &r);
    ci = handpose_find(&db, "test_crowbar");
    ii = handpose_find(&db, "use_test_crank");
    CHECK(ci >= 0 && db.e[ci].orphan && ii >= 0 && db.e[ii].orphan, "removed sources not orphaned");
    // + the never-overwrite case's entry (in memory only, its source removed too).
    CHECK(r.orphans == 3, "orphan count %d", r.orphans);
    char *rep = (char *)malloc(65536);
    hp_report_text(&db, rep, 65536);
    CHECK(strstr(rep, "orphaned") && strstr(rep, "test_crowbar") && strstr(rep, "use_test_crank"), "report doesn't list the orphans");
    free(rep);
    CHECK(count_files(dir) == base_files + 2, "orphans deleted without confirmation");
    CHECK(!hp_remove_orphan(&db, handpose_find(&db, "rifle"), dir, NULL), "a live entry was removable");
    hp_remove_orphan(&db, handpose_find(&db, "test_crowbar"), dir, NULL);
    hp_remove_orphan(&db, handpose_find(&db, "use_test_crank"), dir, NULL);
    CHECK(count_files(dir) == base_files && handpose_find(&db, "test_crowbar") < 0, "orphan removal");
    handpose_db_free(&db);
    hp_sources_free(&src);
    hp_sources_free(&src2);
    free(items);
    free(mod);
    free(db2);
    SDL_RemovePath(rel);
    clear_dir(dir);
    SDL_RemovePath(tmp);
}


// ------------------------------------------------------------------ editor == gameplay

// The editor poses the character with the gameplay pipeline; what it adds is the forced hold point
// state (proc_force_hold_state). Forcing a state must give the same bones, bit for bit, as reaching
// it through gameplay inputs - with edited hand pose data in both.
static void test_editor_equals_gameplay(const ItemDB *idb) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    CharRuntime *a = (CharRuntime *)malloc(sizeof(CharRuntime)), *b = (CharRuntime *)malloc(sizeof(CharRuntime));
    HandPoseDB *db = &L->handposes;
    Rng r = { 5, 9 };
    static const struct { int hp; bool ads, sprint, crouch; } states[] = { { HP_HIP, false, false, false }, { HP_ADS, true, false, false },
                                                                          { HP_SPRINT, false, true, false }, { HP_CROUCH, false, false, true } };
    int runs = 0, diffs = 0;
    for (int wi = 0; wi < idb->weapon_count; wi++) {
        const WeaponDef *wd = &idb->weapons[wi];
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        if (style < 0) continue;
        // Edited data (as the editor leaves it): fingers + a wrist offset on both hands.
        int e = handpose_add(db, wd->id, HPC_WEAPON);
        snprintf(db->e[e].inherits, sizeof(db->e[e].inherits), "style/%s", wd->hold_style);
        for (int h = 0; h < 2; h++) {
            HandPoseLayer *l = &db->e[e].layer[h][HPS_DEFAULT];
            l->set = HPL_FINGERS_ALL | HPL_WRIST;
            for (int f = 0; f < 5; f++)
                for (int k = 0; k < 3; k++) l->j[f][k] = handpose_clamp_joint(f, k, (HandJoint){ frand(&r, 0, 80), frand(&r, -5, 5), 0 });
            l->wrist = xf_make(v3(0.002f, 0, 0), handpose_euler_deg_to_quat(v3(3, 0, 0)));
        }
        handpose_link(db);
        handpose_rehash(db);
        for (int si = 0; si < (int)ARRAY_COUNT(states); si++) {
            charrt_init(a, L, def);
            charrt_init(b, L, def);
            CharInputs in;
            memset(&in, 0, sizeof(in));
            in.grounded = true;
            in.weapon_type = style;
            in.ads = states[si].ads;
            in.sprint = states[si].sprint;
            in.crouch = states[si].crouch;
            for (int t = 0; t < 30; t++) {
                CharRuntime *rt[2] = { a, b };
                for (int k = 0; k < 2; k++) {
                    charrt_set_inputs(rt[k], &in);
                    charrt_tick(rt[k], 1.0f / 60.0f);
                    charrt_eval_base(rt[k]);
                    ProcInput pi;
                    charrt_proc_input(rt[k], &pi, xf_identity(), 1.0f / 60.0f);
                    pi.item = &wd->sockets;
                    pi.hand_ik_weight = 1.0f;
                    if (k == 1) proc_force_hold_state(&pi, states[si].hp);   // the editor's preview
                    charrt_post(rt[k], &pi);
                }
            }
            runs++;
            bool same = !memcmp(a->final_model, b->final_model, sizeof(Xform) * (size_t)L->skel.count) &&
                        !memcmp(&a->proc.item_model, &b->proc.item_model, sizeof(Xform));
            if (!same) diffs++;
            CHECK(same, "%s %s: editor-forced state differs from the gameplay pose", wd->id, holdpoint_state_name(states[si].hp));
            CHECK(a->proc.hp_out.state == states[si].hp, "%s: gameplay inputs reached %s, not %s", wd->id, holdpoint_state_name(a->proc.hp_out.state),
                  holdpoint_state_name(states[si].hp));
        }
        handpose_entry_clear_layers(&db->e[e]);
    }
    anim_lib_reload_handposes(L);
    printf("  editor == gameplay: %d weapon x state runs, %d differ (bit-identical bones and item)\n", runs, diffs);
    free(a);
    free(b);
}

// ------------------------------------------------------------------ gizmo / undo / manifest

static void test_gizmo(void) {
    HpCamera c = { v3(0, 0, 1), quat_identity(), 60, 1280, 720 };
    // Projection round trip.
    for (int i = 0; i < 20; i++) {
        vec3 p = v3(-0.3f + 0.03f * i, 0.2f - 0.02f * i, -0.5f - 0.05f * i);
        vec2 px;
        CHECK(hpg_project(&c, p, &px), "projection failed");
        vec3 o, d;
        hpg_ray(&c, px, &o, &d);
        float t = v3_dot(v3_sub(p, o), d);
        CHECK(v3_dist(v3_madd(o, d, t), p) < 1e-4f, "ray through the projected point misses it");
    }
    HpGizmo g;
    hpg_init(&g);
    g.local = false;
    g.size = 0.1f;
    Xform ed = xf_make(v3(0, 0, 0), quat_identity());
    // Translate along X: mouse on the X axis, dragged to the projection of x = 0.05.
    vec2 m0, m1;
    hpg_project(&c, v3(0.06f, 0, 0), &m0);
    CHECK(hpg_pick(&g, &c, hpg_frame(&g, ed), m0, 6) == HPG_X, "pick the X axis");
    CHECK(hpg_begin(&g, &c, ed, HPG_X, m0), "begin X drag");
    hpg_project(&c, v3(0.11f, 0.01f, 0), &m1);
    Xform x = hpg_drag(&g, &c, m1);
    CHECK(fabsf(x.t.x - 0.05f) < 1e-4f && fabsf(x.t.y) < 1e-6f && fabsf(x.t.z) < 1e-6f, "X drag moved to (%.4f %.4f %.4f), want (0.05 0 0)", x.t.x, x.t.y, x.t.z);
    g.snap = true;
    g.snap_move = 0.03f;   // 0.05 -> the 0.06 grid line
    x = hpg_drag(&g, &c, m1);
    CHECK(fabsf(x.t.x - 0.06f) < 1e-5f, "snapped X drag %.4f, want 0.06", x.t.x);
    hpg_end(&g);
    g.snap = false;
    // Rotate about Z (the camera looks down -Z at the ring; the X / Y rings are edge-on lines
    // through the centre, so grab the Z ring at 45 deg): a quarter turn of the mouse.
    g.mode = HPG_ROTATE;
    float r = g.size * 0.8f;
    hpg_project(&c, v3(r * 0.7071f, r * 0.7071f, 0), &m0);
    CHECK(hpg_pick(&g, &c, hpg_frame(&g, ed), m0, 6) == HPG_RZ, "pick the Z ring");
    CHECK(hpg_begin(&g, &c, ed, HPG_RZ, m0), "begin Z rotation");
    hpg_project(&c, v3(-r * 0.7071f, r * 0.7071f, 0), &m1);
    x = hpg_drag(&g, &c, m1);
    vec3 xa = quat_rotate(x.r, v3(1, 0, 0));
    CHECK(fabsf(xa.y - 1.0f) < 1e-3f, "Z ring quarter turn: x axis -> (%.3f %.3f %.3f), want +Y", xa.x, xa.y, xa.z);
    // Joint picking.
    vec3 pts[3] = { v3(-0.1f, 0, 0), v3(0, 0, 0), v3(0.1f, 0, 0) };
    vec2 mid;
    hpg_project(&c, pts[2], &mid);
    CHECK(hpg_pick_point(&c, pts, 3, mid, 10) == 2, "joint pick");
    float t;
    CHECK(hpg_ray_box(v3(0, 0, 1), v3(0, 0, -1), v3(0, 0, 0), quat_identity(), v3(0.1f, 0.1f, 0.1f), &t) && fabsf(t - 0.9f) < 1e-5f, "ray / box");
}

static void test_undo(void) {
    HandPoseDB db;
    handpose_db_init(&db);
    int i = handpose_add(&db, "u_entry", HPC_ITEM);
    HpUndo u = { 0 };
    for (int k = 1; k <= 5; k++) {
        hpu_push(&u, &db.e[i]);
        db.e[i].layer[1][HPS_DEFAULT].set = HPL_IKW;
        db.e[i].layer[1][HPS_DEFAULT].ik_weight = 0.1f * (float)k;
    }
    CHECK(hpu_undo_count(&u) == 5, "undo count");
    for (int k = 4; k >= 0; k--) {
        int r = hpu_undo(&u, &db);
        const HandPoseLayer *l = &db.e[i].layer[1][HPS_DEFAULT];
        bool ok = k ? fabsf(l->ik_weight - 0.1f * (float)k) < 1e-6f : l->set == 0;   // step 0: the untouched layer
        CHECK(r == i && ok, "undo step %d: ik %.2f set %x", k, l->ik_weight, l->set);
    }
    CHECK(hpu_undo(&u, &db) < 0 && db.e[i].layer[1][HPS_DEFAULT].set == 0, "undo past the start");
    for (int k = 1; k <= 5; k++) hpu_redo(&u, &db);
    CHECK(fabsf(db.e[i].layer[1][HPS_DEFAULT].ik_weight - 0.5f) < 1e-6f && hpu_redo_count(&u) == 0, "redo to the end");
    hpu_undo(&u, &db);
    hpu_push(&u, &db.e[i]);
    CHECK(hpu_redo_count(&u) == 0, "a new edit clears redo");
    hpu_free(&u);
    handpose_db_free(&db);
}

static void test_manifest(void) {
    CHECK(L->manifest.hash[AC_HAND_POSES] == L->handposes.hash && L->handposes.hash != 0, "manifest hand_poses %08x vs DB %08x", L->manifest.hash[AC_HAND_POSES],
          L->handposes.hash);
    CHECK(!strcmp(asset_category_name(AC_HAND_POSES), "hand_poses"), "category name");
    // Same files -> same hash (every client); a changed pose -> a different one.
    u32 h0 = L->handposes.hash;
    anim_lib_reload_handposes(L);
    CHECK(L->handposes.hash == h0, "reload changed the hash");
    int e = handpose_find(&L->handposes, "style/rifle");
    if (e >= 0) {
        L->handposes.e[e].layer[1][HPS_DEFAULT].j[HF_INDEX][0].curl += 1;
        handpose_rehash(&L->handposes);
        CHECK(L->handposes.hash != h0, "a pose edit didn't change the hash");
        anim_lib_reload_handposes(L);
    }
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
    test_style_defaults();
    test_inheritance();
    test_roundtrip();
    test_migration();
    test_mirror_limits();
    test_contact();
    test_glue_tweaks(db);
    test_discovery(db);
    test_editor_equals_gameplay(db);
    test_gizmo();
    test_undo();
    test_manifest();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
