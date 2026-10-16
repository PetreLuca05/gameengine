// Hand pose tests (docs/HANDPOSE.md), run from the build output dir (assets/ and data/ resolve):
// generated style defaults reproduce the old curl tables (bone for bone), inheritance and
// override resolution, lossless save / load, schema migration, mirror, joint limits, the
// contact solver and hand glue for every weapon with randomly tweaked poses.
#include "anim/anim_library.h"
#include "anim/character_rt.h"
#include "anim/handpose.h"
#include "gameplay/items.h"
#include "core/engine.h"
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
            // Inside from the start (25 deg past the contact): opens until clear, ends on the surface.
            HandFingers k;
            memcpy(k, j, sizeof(k));
            for (int q = 0; q < 3; q++) k[f][q].curl = fminf(k[f][q].curl + 25.0f * (q == 0 ? 0.30f : q == 1 ? 0.38f : 0.32f), 90.0f);
            HandFingers probe;
            memcpy(probe, k, sizeof(probe));
            HandContactResult in0 = handpose_contact_solve(s, &g_pa, g_ma, side, f, probe, &L->hands, &box, 1, NULL, 0.008f, 0.0005f);
            r = in0;
            CHECK(r.touched && r.curl_add < 0, "contact from inside %s %s: didn't open (%.1f deg)", side ? "R" : "L", handpose_finger_name(f), r.curl_add);
            CHECK(r.tip_dist >= 0 && r.tip_dist <= 0.001f, "contact from inside %s %s: %.2f mm", side ? "R" : "L", handpose_finger_name(f), r.tip_dist * 1000);
        }
    printf("  contact solver: worst surface gap %.3f mm (%s)\n", worst * 1000, handpose_finger_name(worst_f));
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
    for (int wi = 0; wi < idb->weapon_count; wi++) {
        const WeaponDef *wd = &idb->weapons[wi];
        int style = proc_hold_style_index(&L->proc, wd->hold_style);
        if (style < 0) continue;
        int e = handpose_add(db, wd->id, HPC_WEAPON);
        HandPoseEntry *en = &db->e[e];
        handpose_entry_clear_layers(en);
        snprintf(en->inherits, sizeof(en->inherits), "style/%s", wd->hold_style);
        for (int h = 0; h < 2; h++) {
            HandPoseLayer *l = &en->layer[h][HPS_DEFAULT];
            l->set = HPL_FINGERS_ALL | HPL_WRIST | (h == 0 ? HPL_SOCKET : 0);
            for (int f = 0; f < 5; f++)
                for (int k = 0; k < 3; k++) l->j[f][k] = handpose_clamp_joint(f, k, (HandJoint){ frand(&r, 0, 90), frand(&r, -10, 10), frand(&r, -8, 8) });
            l->wrist = xf_make(v3(frand(&r, -0.005f, 0.005f), frand(&r, -0.005f, 0.005f), frand(&r, -0.005f, 0.005f)), handpose_euler_deg_to_quat(v3(frand(&r, -8, 8), frand(&r, -8, 8), frand(&r, -8, 8))));
            Xform base = wd->sockets.grip_l;
            l->socket = xf_mul(base, xf_make(v3(frand(&r, -0.01f, 0.01f), frand(&r, -0.01f, 0.01f), frand(&r, -0.02f, 0.02f)), handpose_euler_deg_to_quat(v3(frand(&r, -10, 10), frand(&r, -10, 10), 0))));
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
                pi.item = &wd->sockets;
                pi.hand_ik_weight = 1.0f;
                charrt_post(rt, &pi);
            }
            // Right hand: the item is finalized from it (exact by construction); left: on its socket.
            Xform palm_r = xf_mul(rt->final_model[s->role[BR_R_HAND]], L->hands.palm[1]);
            float er = v3_dist(xf_mul(palm_r, xf_inverse(rt->proc.hand[1].sock)).t, rt->proc.item_model.t);
            CHECK(er < 1e-4f, "%s: item not finalized from the tweaked right socket (%.2f mm)", wd->id, er * 1000);
            if (wd->sockets.has_left && proc_hold_style(&L->proc, style)->left_hand) {
                Xform sock = rt->proc.hand[0].sock;
                sock.t.z += rt->proc.grip_l_slide;
                sock = xf_mul(rt->proc.item_model, sock);
                Xform palm_l = xf_mul(rt->final_model[s->role[BR_L_HAND]], L->hands.palm[0]);
                float pe = v3_dist(palm_l.t, sock.t), ae = quat_angle_between(palm_l.r, sock.r) * DG_RAD2DEG;
                CHECK(pe < 0.002f && ae < 1.0f, "%s %s: left hand %.2f mm / %.2f deg off its tweaked socket", wd->id, ads ? "ADS" : "hip", pe * 1000, ae);
                worst = fmaxf(worst, pe);
            }
            tested++;
        }
        handpose_entry_clear_layers(&db->e[e]);
    }
    anim_lib_reload_handposes(L);
    printf("  glue with random pose tweaks: %d weapon x state runs, worst left hand %.3f mm\n", tested, worst * 1000);
    free(rt);
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
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
