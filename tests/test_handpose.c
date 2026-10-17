// Hand pose tests (docs/HANDPOSE.md), run from the build output dir (assets/ and data/ resolve):
// the style files reproduce the removed curl tables (bone for bone), grip inheritance, lossless
// save / load of grips and cases, the schema 1 migration (the hip grip kept, differences reported,
// interactables / vehicles -> cases), mirror, joint limits, the contact solver, the universal grip
// (bit-identical in every hold state and on every client), hand glue for every weapon with randomly
// tweaked grips and roots, and discovery (grips + case stubs).
#include "anim/anim_library.h"
#include "anim/character_rt.h"
#include "anim/handpose.h"
#include "editor/handpose/hp_discover.h"
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

// The finger curl tables the code had before the style files (procedural.c default_style +
// controller.json, the grip table of the interaction pass): the style files must reproduce them.
typedef struct LegacyCurl { const char *name; float r[5], l[5]; } LegacyCurl;
static const LegacyCurl k_legacy[] = {
    { "none", { 0, 0, 0, 0, 0 }, { 40, 50, 55, 60, 65 } },
    { "pistol", { 20, 25, 70, 75, 80 }, { 40, 50, 55, 60, 65 } },
    { "rifle", { 20, 25, 70, 75, 80 }, { 40, 50, 55, 60, 65 } },
    { "shotgun", { 20, 25, 70, 75, 80 }, { 40, 50, 55, 60, 65 } },
    { "smg", { 20, 25, 70, 75, 80 }, { 40, 50, 55, 60, 65 } },
    { "lmg", { 20, 25, 70, 75, 80 }, { 40, 50, 55, 60, 65 } },
    { "melee_1h", { 60, 85, 90, 90, 90 }, { 40, 50, 55, 60, 65 } },
    { "melee_2h", { 60, 85, 90, 90, 90 }, { 60, 85, 90, 90, 90 } },
    { "item_small", { 30, 45, 50, 55, 60 }, { 40, 50, 55, 60, 65 } },
    { "item_large", { 20, 25, 70, 75, 80 }, { 40, 50, 55, 60, 65 } },
    { "phone", { 25, 8, 8, 10, 12 }, { 40, 50, 55, 60, 65 } },
    { "phone_landscape", { 15, 35, 40, 45, 50 }, { 15, 35, 40, 45, 50 } },
    { "grip_relaxed", { 10, 15, 20, 25, 30 }, { 10, 15, 20, 25, 30 } },
    { "grip_fist", { 50, 95, 100, 100, 100 }, { 50, 95, 100, 100, 100 } },
    { "grip_point", { 50, 0, 95, 95, 95 }, { 50, 0, 95, 95, 95 } },
    { "grip_grab", { 35, 50, 55, 60, 65 }, { 35, 50, 55, 60, 65 } },
    { "grip_pistol", { 40, 70, 80, 85, 90 }, { 40, 70, 80, 85, 90 } },
    { "grip_rifle", { 40, 70, 80, 85, 90 }, { 40, 70, 80, 85, 90 } },
    { "grip_melee", { 50, 85, 90, 90, 90 }, { 50, 85, 90, 90, 90 } },
    { "grip_palm", { 0, 5, 5, 5, 5 }, { 0, 5, 5, 5, 5 } },
};

// Every style file resolves to its old curl, and applying it moves the bones as the old code did
// (within float rounding: the angle products are associated differently).
static void test_style_defaults(void) {
    const Skeleton *s = &L->skel;
    const HandPoseDB *db = &L->handposes;
    for (int i = 0; i < (int)ARRAY_COUNT(k_legacy); i++) {
        const LegacyCurl *lc = &k_legacy[i];
        int e = handpose_find_hash(db, handpose_style_hash(lc->name));
        CHECK(e >= 0 && db->e[e].on_disk, "style %s: no style file", lc->name);
        if (e < 0) continue;
        for (int h = 0; h < 2; h++) {
            HandPose p;
            handpose_resolve(db, e, h, &p);
            HandFingers want;
            handpose_from_curl(h ? lc->r : lc->l, want);
            float d = 0;
            for (int f = 0; f < 5; f++)
                for (int k = 0; k < 3; k++) d = fmaxf(d, fabsf(p.j[f][k].curl - want[f][k].curl) + fabsf(p.j[f][k].spread) + fabsf(p.j[f][k].twist));
            CHECK(d < 1e-4f, "style %s hand %d: resolved fingers differ from the curl table by %.5f deg", lc->name, h, d);
            CHECK(!p.has_socket && !p.has_pole, "style %s: the style file overrides a socket / pole", lc->name);
            pose_bind(s, &g_pa);
            pose_to_model(s, &g_pa, g_ma);
            pose_copy(&g_pb, &g_pa);
            memcpy(g_mb, g_ma, sizeof(Xform) * (size_t)s->count);
            legacy_curl(s, &g_pa, g_ma, h, h ? lc->r : lc->l, &L->hands, 1.0f);
            handpose_apply_fingers(s, &g_pb, g_mb, h, p.j, &L->hands, 1.0f);
            float bd = max_bone_diff(s, g_ma, g_mb);
            CHECK(bd < 1e-5f, "style %s hand %d: finger bones %.4f mm off the legacy curl", lc->name, h, bd * 1000);
        }
    }
    for (int i = 0; i < L->proc.style_count; i++) CHECK(handpose_find_hash(db, handpose_style_hash(L->proc.styles[i].name)) >= 0, "hold style %s has no style file", L->proc.styles[i].name);
    for (int g = 0; g < handpose_grip_count(); g++) CHECK(handpose_find_hash(db, handpose_grip_style_hash(g)) >= 0, "grip style %s missing", handpose_grip_name(g));
    printf("  style files: %d hold styles + %d grips reproduce the removed curl tables\n", L->proc.style_count, handpose_grip_count());
}

// ------------------------------------------------------------------ inheritance

static void test_inheritance(void) {
    HandPoseDB db;
    handpose_db_init(&db);
    int a = handpose_add(&db, "style/t_a", HPC_STYLE);
    db.e[a].grip[1].set = HPL_FINGER(HF_INDEX) | HPL_FINGER(HF_MIDDLE) | HPL_POLE;
    db.e[a].grip[1].j[HF_INDEX][0].curl = 10;
    db.e[a].grip[1].j[HF_MIDDLE][0].curl = 11;
    db.e[a].grip[1].pole = v3(1, 2, 3);
    int b = handpose_add(&db, "t_b", HPC_WEAPON);
    dg_strcpy(db.e[b].inherits, sizeof(db.e[b].inherits), "style/t_a");
    db.e[b].grip[1].set = HPL_FINGER(HF_INDEX) | HPL_SOCKET;
    db.e[b].grip[1].j[HF_INDEX][0].curl = 30;
    db.e[b].grip[1].socket = xf_make(v3(0.01f, 0, 0), quat_identity());
    int c = handpose_add(&db, "t_c", HPC_ATTACHMENT);   // no inherits: the runtime base supplies the style
    db.e[c].grip[0].set = HPL_FINGER(HF_RING);
    db.e[c].grip[0].j[HF_RING][1].curl = 50;
    db.e[a].grip[0].set = HPL_FINGER(HF_PINKY);
    db.e[a].grip[0].j[HF_PINKY][2].curl = 5;
    handpose_link(&db);
    HandPose p;
    handpose_resolve(&db, b, 1, &p);
    CHECK(p.j[HF_INDEX][0].curl == 30 && p.j[HF_MIDDLE][0].curl == 11, "item grip over its style (%.0f, %.0f)", p.j[HF_INDEX][0].curl, p.j[HF_MIDDLE][0].curl);
    CHECK(p.has_socket && p.socket.t.x == 0.01f && p.has_pole && p.pole.y == 2, "item IK target, style pole");
    handpose_resolve_over(&db, a, c, 0, &p);
    CHECK(p.j[HF_RING][1].curl == 50 && p.j[HF_PINKY][2].curl == 5, "resolve_over: base style under an entry without inherits");
    handpose_resolve_over(&db, a, b, 1, &p);
    CHECK(p.j[HF_MIDDLE][0].curl == 11 && p.j[HF_INDEX][0].curl == 30, "resolve_over: base already in the chain applies once");
    handpose_resolve(&db, -1, 1, &p);
    CHECK(!p.has_socket && p.j[HF_INDEX][0].curl == 0 && p.ik_weight == 1 && p.wrist.r.w == 1, "no entry: open hand, identity");
    CHECK(handpose_root(&db, b).s.x == 1.0f && handpose_root(&db, b).r.w == 1.0f, "no root: identity, scale 1");
    // Cycle: a -> b -> a is cut, resolution terminates.
    dg_strcpy(db.e[a].inherits, sizeof(db.e[a].inherits), "t_b");
    handpose_link(&db);
    handpose_resolve(&db, b, 1, &p);
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
    db.e[stub].grip[1].set = HPL_IKW;
    db.e[stub].grip[1].ik_weight = 0.5f;
    handpose_rehash(&db);
    CHECK(db.hash != h0, "an override doesn't change the net hash");
    u32 h1 = db.hash;
    db.e[stub].has_root = true;
    db.e[stub].root = xf_make(v3(0, 0.01f, 0), quat_identity());
    handpose_rehash(&db);
    CHECK(db.hash != h1, "a root doesn't change the net hash");
    // A stub case doesn't count, a keyed one does.
    h1 = db.hash;
    int ci = handpose_case_add(&db, "t_case", HCK_INTERACT);
    handpose_rehash(&db);
    CHECK(db.hash == h1, "a stub case changes the net hash");
    handpose_case_key_at(&db, ci, 0.2f);
    handpose_rehash(&db);
    CHECK(db.hash != h1, "a case key doesn't change the net hash");
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

// The wrist offset is folded into the IK target on load (one control in the editor): compare
// with it folded.
static void fold(HandPoseLayer *l) {
    if ((l->set & HPL_SOCKET) && (l->set & HPL_WRIST)) {
        l->socket = xf_mul(l->socket, l->wrist);
        l->socket.s = v3(1, 1, 1);
        l->wrist = xf_identity();
        l->set = (u16)(l->set & ~HPL_WRIST);
    }
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
        e->reviewed = trial & 1;
        e->auto_placed = trial & 2;
        e->has_root = trial & 4;
        float sc = frand(&r, 0.6f, 1.8f);
        e->root = e->has_root ? xf_make(v3(frand(&r, -0.05f, 0.05f), frand(&r, -0.05f, 0.05f), frand(&r, -0.05f, 0.05f)), handpose_euler_deg_to_quat(v3(frand(&r, -9, 9), frand(&r, -9, 9), frand(&r, -9, 9)))) : xf_identity();
        if (e->has_root) e->root.s = v3s(sc);
        for (int h = 0; h < 2; h++) {
            random_layer(&r, &e->grip[h]);
            if (rng_next(&r) % 4 == 0) e->grip[h].set = 0;
            fold(&e->grip[h]);
        }
        char *txt = handpose_entry_write(e);
        cJSON *root = cJSON_Parse(txt);
        HandPoseEntry back;
        memset(&back, 0, sizeof(back));
        dg_strcpy(back.id, sizeof(back.id), "rt_entry");
        char err[128];
        bool ok = root && handpose_entry_parse(&back, root, err, sizeof(err), NULL);
        char *txt2 = ok ? handpose_entry_write(&back) : NULL;
        bool same = ok && txt2 && !strcmp(txt, txt2) && back.reviewed == e->reviewed && back.has_root == e->has_root && !strcmp(back.label, e->label) &&
                    !strcmp(back.inherits, e->inherits) && (!e->has_root || !memcmp(&back.root, &e->root, sizeof(Xform)));
        for (int h = 0; h < 2 && same; h++) same = layer_equal(&e->grip[h], &back.grip[h]);
        if (!same && !bad++) printf("  round trip mismatch:\n%s\n---\n%s\n", txt, txt2 ? txt2 : "(parse failed)");
        cJSON_Delete(root);
        free(txt);
        free(txt2);
    }
    CHECK(bad == 0, "grip save / load round trip lossy in %d of 50 random grips", bad);
    // Cases: anchor, markers, keys.
    bad = 0;
    for (int trial = 0; trial < 30; trial++) {
        int i = handpose_case_add(&db, "rt_case", HCK_VEHICLE);
        HandPoseCase *c = &db.c[i];
        dg_strcpy(c->bind, sizeof(c->bind), "vehicle:car:driver");
        dg_strcpy(c->vehicle, sizeof(c->vehicle), "car");
        dg_strcpy(c->inherits, sizeof(c->inherits), "style/grip_grab");
        c->anchor = xf_make(v3(frand(&r, -0.1f, 0.1f), frand(&r, -0.1f, 0.1f), 0), handpose_euler_deg_to_quat(v3(frand(&r, -20, 20), 0, 0)));
        c->duration = 1.5f;
        c->contact = frand(&r, 0.1f, 0.3f);
        c->release = frand(&r, 0.6f, 1.2f);
        c->use = 0.5f;
        c->key_count = (int)(rng_next(&r) % 5);
        for (int k = 0; k < c->key_count; k++) {
            c->keys[k].t = 0.1f + 0.3f * (float)k;
            snprintf(c->keys[k].name, sizeof(c->keys[k].name), "k%d", k);
            for (int h = 0; h < 2; h++) { random_layer(&r, &c->keys[k].hand[h]); fold(&c->keys[k].hand[h]); }
        }
        char *txt = handpose_case_write(c);
        cJSON *root = cJSON_Parse(txt);
        HandPoseCase back;
        memset(&back, 0, sizeof(back));
        dg_strcpy(back.id, sizeof(back.id), "rt_case");
        char err[128];
        bool ok = root && handpose_case_parse(&back, root, err, sizeof(err));
        char *txt2 = ok ? handpose_case_write(&back) : NULL;
        bool same = ok && txt2 && !strcmp(txt, txt2) && back.key_count == c->key_count && back.contact == c->contact && !memcmp(&back.anchor, &c->anchor, sizeof(Xform));
        for (int k = 0; k < c->key_count && same; k++)
            for (int h = 0; h < 2 && same; h++) same = layer_equal(&c->keys[k].hand[h], &back.keys[k].hand[h]) && back.keys[k].t == c->keys[k].t;
        if (!same && !bad++) printf("  case round trip mismatch:\n%s\n---\n%s\n", txt, txt2 ? txt2 : "(parse failed)");
        cJSON_Delete(root);
        free(txt);
        free(txt2);
    }
    CHECK(bad == 0, "case save / load round trip lossy in %d of 30 random cases", bad);
    handpose_db_free(&db);
}

static void test_migration(void) {
    // Schema 0: top-level curls -> the grip.
    const char *v0 = "{ \"inherits\": \"style/rifle\", \"curl_r\": [10, 20, 30, 40, 50], \"curl_l\": [1, 2, 3, 4, 5], \"left\": { \"ads\": { \"ik_weight\": 0.5 } } }";
    cJSON *root = cJSON_Parse(v0);
    HandPoseEntry e;
    memset(&e, 0, sizeof(e));
    dg_strcpy(e.id, sizeof(e.id), "legacy");
    char err[128];
    HandPoseMigrateNote note;
    CHECK(handpose_entry_parse(&e, root, err, sizeof(err), &note), "schema 0 parse: %s", err);
    float cr[5] = { 10, 20, 30, 40, 50 };
    HandFingers want;
    handpose_from_curl(cr, want);
    CHECK((e.grip[1].set & HPL_FINGERS_ALL) == HPL_FINGERS_ALL && !memcmp(e.grip[1].j, want, sizeof(want)), "schema 0 curl_r -> right grip fingers");
    CHECK(!(e.grip[0].set & HPL_IKW) && note.dropped == 1 && strstr(note.text, "ads left"), "schema 0: the ads layer is reported and dropped (%s)", note.text);
    char *txt = handpose_entry_write(&e);
    CHECK(strstr(txt, "\"schema\": 2") != NULL, "migrated entry written at the current schema");
    free(txt);
    cJSON_Delete(root);
    // Schema 1: the grip is default with hip on top; other states differing from it are reported.
    const char *v1 = "{ \"schema\": 1, \"inherits\": \"style/rifle\", \"right\": {"
                     " \"default\": { \"fingers\": { \"index\": [[10,0,0],[0,0,0],[0,0,0]], \"ring\": [[20,0,0],[0,0,0],[0,0,0]] }, \"pole\": [1, -1, 0] },"
                     " \"hip\": { \"fingers\": { \"index\": [[15,0,0],[0,0,0],[0,0,0]] }, \"socket\": { \"pos\": [0.01, 0, 0], \"q\": [0, 0, 0, 1] }, \"wrist\": { \"pos\": [0, 0.01, 0], \"q\": [0, 0, 0, 1] } },"
                     " \"ads\": { \"fingers\": { \"index\": [[15,0,0],[0,0,0],[0,0,0]], \"middle\": [[30,0,0],[0,0,0],[0,0,0]] } },"
                     " \"sprint\": { \"pole\": [1, -1, 0] } } }";
    root = cJSON_Parse(v1);
    memset(&e, 0, sizeof(e));
    dg_strcpy(e.id, sizeof(e.id), "legacy1");
    CHECK(handpose_entry_parse(&e, root, err, sizeof(err), &note), "schema 1 parse");
    const HandPoseLayer *g = &e.grip[1];
    CHECK(g->j[HF_INDEX][0].curl == 15 && g->j[HF_RING][0].curl == 20 && g->pole.x == 1, "hip grip kept over default (index %.0f ring %.0f)", g->j[HF_INDEX][0].curl, g->j[HF_RING][0].curl);
    CHECK((g->set & HPL_SOCKET) && !(g->set & HPL_WRIST) && fabsf(g->socket.t.x - 0.01f) < 1e-6f && fabsf(g->socket.t.y - 0.01f) < 1e-6f, "wrist folded into the IK target");
    CHECK(!(g->set & HPL_FINGER(HF_MIDDLE)), "an ads-only finger is not in the grip");
    CHECK(note.dropped == 1 && strstr(note.text, "ads right: middle") && !strstr(note.text, "sprint"), "differences: only ads/middle (same-valued layers are not differences): %s", note.text);
    cJSON_Delete(root);
    // Schema 1 interactable -> a case with one key (default + use).
    const char *vi = "{ \"schema\": 1, \"category\": \"interactable\", \"inherits\": \"style/grip_grab\", \"hands\": [\"right\"], \"right\": {"
                     " \"default\": { \"socket\": { \"pos\": [0, 0.02, 0], \"q\": [0, 0, 0, 1] } }, \"use\": { \"ik_weight\": 0.8 }, \"hold\": { \"ik_weight\": 0.3 } } }";
    root = cJSON_Parse(vi);
    HandPoseCase c;
    CHECK(handpose_case_from_v1(&c, root, "use_crank", &note), "v1 interactable -> case");
    CHECK(!strcmp(c.id, "use_crank") && !strcmp(c.bind, "interactable:crank") && c.kind == HCK_INTERACT && c.key_count == 1 && c.keys[0].t == c.contact,
          "case id / bind / one key at contact (%s %s %d)", c.id, c.bind, c.key_count);
    CHECK(c.keys[0].hand[1].ik_weight == 0.8f && fabsf(c.keys[0].hand[1].socket.t.y - 0.02f) < 1e-6f && strstr(note.text, "hold right"), "key = default + use, hold reported (%s)", note.text);
    cJSON_Delete(root);
    root = cJSON_Parse("{ \"schema\": 1, \"category\": \"vehicle\", \"left\": {}, \"right\": {} }");
    handpose_case_from_v1(&c, root, "veh_car", NULL);
    CHECK(!strcmp(c.id, "vehicle_car_driver") && !strcmp(c.bind, "vehicle:car:driver") && c.key_count == 0, "v1 vehicle -> vehicle_car_driver, no key (%s)", c.id);
    cJSON_Delete(root);
    cJSON *future = cJSON_Parse("{ \"schema\": 99 }");
    CHECK(!handpose_entry_parse(&e, future, err, sizeof(err), NULL), "a newer schema is refused");
    cJSON_Delete(future);
}

// ------------------------------------------------------------------ cases

// Timeline: phase <-> time round trip, deterministic scrubbing (forwards, backwards, random order),
// blending between keys, held outside them.
static void test_case_timeline(void) {
    HandPoseDB *db = &L->handposes;
    int ci = handpose_case_add(db, "t_timeline", HCK_INTERACT);
    HandPoseCase *c = &db->c[ci];
    dg_strcpy(c->inherits, sizeof(c->inherits), "style/grip_grab");
    handpose_link(db);
    int k0 = handpose_case_key_at(db, ci, db->c[ci].contact), k1;
    db->c[ci].keys[k0].hand[1].socket.t = v3(0, 0, 0);
    handpose_set_finger_curl(db->c[ci].keys[k0].hand[1].j, HF_INDEX, 10);
    k1 = handpose_case_key_at(db, ci, db->c[ci].release);
    c = &db->c[ci];
    c->keys[k1].hand[1].socket.t = v3(0.1f, 0, 0);
    handpose_set_finger_curl(c->keys[k1].hand[1].j, HF_INDEX, 70);
    for (int ph = 0; ph < 3; ph++)
        for (int q = 0; q <= 10; q++) {
            float u = (float)q / 10.0f, t = handpose_case_time(c, ph, u), u2;
            int ph2;
            handpose_case_phase(c, t, &ph2, &u2);
            bool boundary = (q == 10 && ph < 2) || (q == 0 && ph > 0);
            CHECK(boundary || (ph2 == ph && fabsf(u2 - u) < 1e-5f), "phase %d u %.1f -> t %.3f -> phase %d u %.3f", ph, u, t, ph2, u2);
        }
    HandPose a, b, m;
    handpose_case_eval(db, ci, -1, 1, 0.0f, &a);
    handpose_case_eval(db, ci, -1, 1, 0.16f, &b);
    CHECK(!memcmp(&a, &b, sizeof(a)), "before the first key: held");
    handpose_case_eval(db, ci, -1, 1, 0.35f, &m);
    CHECK(m.socket.t.x > 0.0f && m.socket.t.x < 0.1f && m.j[HF_INDEX][0].curl > b.j[HF_INDEX][0].curl, "between keys: blended (%.3f)", m.socket.t.x);
    handpose_case_eval(db, ci, -1, 1, 0.76f, &a);
    CHECK(fabsf(a.socket.t.x - 0.1f) < 1e-6f, "after the last key: held");
    // Deterministic scrubbing: the same time gives the same bits whatever the order of evaluation.
    static HandPose fwd[77];
    for (int i = 0; i <= 76; i++) handpose_case_eval(db, ci, -1, 1, (float)i * 0.01f, &fwd[i]);
    int diff = 0;
    for (int i = 76; i >= 0; i--) { handpose_case_eval(db, ci, -1, 1, (float)i * 0.01f, &a); diff += memcmp(&a, &fwd[i], sizeof(a)) != 0; }
    Rng r = { 5, 9 };
    for (int n = 0; n < 200; n++) {
        int i = (int)(rng_next(&r) % 77);
        handpose_case_eval(db, ci, -1, 1, (float)i * 0.01f, &a);
        diff += memcmp(&a, &fwd[i], sizeof(a)) != 0;
    }
    CHECK(diff == 0, "case scrubbing not deterministic (%d mismatches)", diff);
    // The runtime's phase mapping (hands.c: reach / hold / return progress -> time) lands on the
    // keys exactly: contact = end of the reach, release = end of the hold.
    CHECK(handpose_case_time(c, HCP_REACH, 1.0f) == c->keys[k0].t && handpose_case_time(c, HCP_HOLD, 1.0f) == c->keys[k1].t, "phase mapping vs keys");
    handpose_case_key_remove(&db->c[ci], 0);
    handpose_case_key_remove(&db->c[ci], 0);
    db->case_count--;   // drop the test case
    printf("  case timeline: phase mapping, blending, deterministic scrubbing (%d evaluations)\n", 77 * 2 + 200);
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
    cJSON *o = cJSON_Parse("{ \"schema\": 2, \"right\": { \"fingers\": { \"thumb\": [[999, -999, 999], [0, 0, 0], [0, 0, 0]] } } }");
    HandPoseEntry e;
    memset(&e, 0, sizeof(e));
    char err[64];
    handpose_entry_parse(&e, o, err, sizeof(err), NULL);
    handpose_joint_limits(HF_THUMB, 0, &lo, &hi);
    CHECK(e.grip[1].j[HF_THUMB][0].curl == hi.curl && e.grip[1].j[HF_THUMB][0].spread == lo.spread, "limits not enforced on load");
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
        // A calibration root (offset, rotation, scale): the hands stay glued to the scaled item.
        en->has_root = true;
        en->root = xf_make(v3(frand(&r, -0.01f, 0.01f), frand(&r, -0.01f, 0.01f), frand(&r, -0.02f, 0.02f)), handpose_euler_deg_to_quat(v3(frand(&r, -4, 4), frand(&r, -4, 4), frand(&r, -4, 4))));
        en->root.s = v3s(frand(&r, 0.9f, 1.1f));
        for (int h = 0; h < 2; h++) {
            HandPoseLayer *l = &en->grip[h];
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
    printf("  glue with random grip + root tweaks: %d weapon x state runs, worst left hand %.3f mm\n", tested, worst * 1000);
    free(rt);
}

// ------------------------------------------------------------------ glue: items + case keys

static void run_rt(CharRuntime *rt, const CharacterDef *def, int style, const ItemSockets *item, int ticks, ProcInput *last, void (*setup)(ProcInput *, void *), void *ud) {
    charrt_init(rt, L, def);
    CharInputs in;
    memset(&in, 0, sizeof(in));
    in.grounded = true;
    in.weapon_type = style;
    for (int t = 0; t < ticks; t++) {
        charrt_set_inputs(rt, &in);
        charrt_tick(rt, 1.0f / 60.0f);
        charrt_eval_base(rt);
        ProcInput pi;
        charrt_proc_input(rt, &pi, xf_identity(), 1.0f / 60.0f);
        pi.item = item;
        pi.hand_ik_weight = 1.0f;
        if (setup) setup(&pi, ud);
        charrt_post(rt, &pi);
        if (last) *last = pi;
    }
}

// Every held item (not only the weapons): the hands on their grip sockets.
static void test_glue_items(const ItemDB *idb) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    const Skeleton *s = &L->skel;
    int tested = 0;
    float worst = 0;
    for (int i = 1; i < idb->item_count; i++) {
        const ItemDef *it = &idb->items[i];
        if (!it->has_hold || it->weapon >= 0) continue;
        int style = proc_hold_style_index(&L->proc, it->hold_style);
        if (style < 0) continue;
        run_rt(rt, def, style, &it->sockets, 30, NULL, NULL, NULL);
        Xform palm_r = xf_mul(rt->final_model[s->role[BR_R_HAND]], L->hands.palm[1]);
        float er = v3_dist(xf_mul(palm_r, xf_inverse(rt->proc.hand[1].sock)).t, rt->proc.item_model.t);
        CHECK(er < 1e-4f, "%s: item not on the right hand (%.2f mm)", it->id, er * 1000);
        if (it->sockets.has_left && proc_hold_style(&L->proc, style)->left_hand) {
            Xform sock = xf_mul(rt->proc.item_model, rt->proc.hand[0].sock);
            Xform palm_l = xf_mul(rt->final_model[s->role[BR_L_HAND]], L->hands.palm[0]);
            float pe = v3_dist(palm_l.t, sock.t);
            CHECK(pe < 0.002f, "%s: left hand %.2f mm off its socket", it->id, pe * 1000);
            worst = fmaxf(worst, pe);
        }
        tested++;
    }
    printf("  glue: %d held items, worst left hand %.3f mm\n", tested, worst * 1000);
    free(rt);
}

typedef struct KeyGlue { u32 case_hash; float t; vec3 target[2]; u8 hands; } KeyGlue;
static void key_setup(ProcInput *pi, void *ud) {
    const KeyGlue *k = (const KeyGlue *)ud;
    for (int h = 0; h < 2; h++) {
        pi->hand_anchor[h] = k->target[h];
        pi->hand_anchor_rot[h] = quat_identity();
        pi->anchor_case[h] = k->case_hash;
        pi->anchor_case_t[h] = k->t;
        pi->anchor_pose_base[h] = handpose_grip_style_hash(3);   // grab
    }
    pi->hand_anchor_rot_valid = true;
    pi->hand_anchor_w = 1.0f;
    pi->hand_anchor_skip = (u8)(~k->hands & 3);
}

// Every case keyframe (each case gets one where it has none): the case's IK targets on a
// reachable anchor in front of the chest, solved by the anchor pass; the palms land on them.
static void test_glue_case_keys(void) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    HandPoseDB *db = &L->handposes;
    CharRuntime *rt = (CharRuntime *)malloc(sizeof(CharRuntime));
    const Skeleton *s = &L->skel;
    int keys = 0;
    float worst = 0;
    for (int ci = 0; ci < db->case_count; ci++) {
        HandPoseCase *c = &db->c[ci];
        if (c->kind == HCK_ITEM || c->kind == HCK_RELOAD) continue;   // their hands are the item's grip / the clip
        if (!c->key_count) handpose_case_key_at(db, ci, db->c[ci].contact);
        c = &db->c[ci];
        for (int k = 0; k < c->key_count; k++) {
            KeyGlue g = { c->hash, c->keys[k].t, { v3(-0.14f, 1.15f, -0.38f), v3(0.14f, 1.15f, -0.38f) }, c->hands };
            run_rt(rt, def, 0, NULL, 20, NULL, key_setup, &g);
            for (int h = 0; h < 2; h++) {
                if (!(c->hands & (1 << h))) continue;
                HandPose hp;
                handpose_case_eval(db, ci, handpose_find_hash(db, handpose_grip_style_hash(3)), h, c->keys[k].t, &hp);
                // The anchor pass works in model space (root = identity here): target * socket.
                Xform want = xf_mul(xf_make(g.target[h], quat_identity()), hp.socket);
                Xform palm = xf_mul(rt->final_model[s->role[h ? BR_R_HAND : BR_L_HAND]], L->hands.palm[h]);
                float e = v3_dist(palm.t, want.t);
                CHECK(e < 0.002f, "case %s key %d hand %d: palm %.2f mm off its IK target", c->id, k, h, e * 1000);
                worst = fmaxf(worst, e);
            }
            keys++;
        }
    }
    printf("  glue: %d case keyframes, worst palm %.3f mm\n", keys, worst * 1000);
    anim_lib_reload_handposes(L);   // drop the keys added here
    free(rt);
}

// ------------------------------------------------------------------ universal grip

// One grip per item: the hand targets on the item (item space socket, pole, IK weight, fingers)
// are bit-identical in every hold point state and on two independent runtimes (two clients),
// and equal what proc_hand_grip computes from the data alone (inventory preview, editor).
static void test_universal_grip(const ItemDB *idb) {
    const CharacterDef *def = anim_lib_char_by_id(L, "player");
    if (!def) return;
    CharRuntime *rt[2] = { (CharRuntime *)malloc(sizeof(CharRuntime)), (CharRuntime *)malloc(sizeof(CharRuntime)) };
    HandPoseDB *db = &L->handposes;
    // Give the rifle a real grip (fingers + IK target + root) so the check isn't on a stub.
    int e = handpose_find(db, "rifle");
    if (e < 0) { CHECK(false, "no rifle grip"); return; }
    HandPoseEntry keep = db->e[e];
    handpose_set_finger_curl(db->e[e].grip[1].j, HF_INDEX, 35);
    db->e[e].grip[1].set |= HPL_FINGER(HF_INDEX);
    db->e[e].grip[0].set |= HPL_SOCKET;
    const WeaponDef *wd = NULL;
    for (int i = 0; i < idb->weapon_count; i++) if (!strcmp(idb->weapons[i].id, "rifle")) wd = &idb->weapons[i];
    if (!wd) { CHECK(false, "no rifle"); return; }
    db->e[e].grip[0].socket = xf_mul(wd->sockets.grip_l, xf_make(v3(0, 0.004f, -0.01f), quat_axis_angle(v3(0, 0, 1), 0.05f)));
    db->e[e].has_root = true;
    db->e[e].root = xf_make(v3(0.003f, -0.002f, 0.01f), quat_axis_angle(v3(0, 1, 0), 0.03f));
    db->e[e].root.s = v3s(1.02f);
    handpose_rehash(db);
    int style = proc_hold_style_index(&L->proc, wd->hold_style);
    HandHold ref[2];
    for (int h = 0; h < 2; h++) proc_hand_grip(db, proc_hold_style(&L->proc, style), &wd->sockets, h, 1.02f, &ref[h]);
    static const char *names[] = { "hip", "ads", "sprint", "crouch", "prone", "inspect", "vehicle_onehand", "empty" };
    int mismatches = 0;
    for (int st = 0; st < (int)ARRAY_COUNT(names); st++)
        for (int client = 0; client < 2; client++) {
            charrt_init(rt[client], L, def);
            CharInputs in;
            memset(&in, 0, sizeof(in));
            in.grounded = true;
            in.weapon_type = style;
            in.ads = st == 1;
            in.sprint = st == 2;
            in.crouch = st == 3;
            in.prone = st == 4;
            for (int t = 0; t < 40; t++) {
                in.aim_pitch = 0.1f * (float)client;   // the view differs per client: the grip doesn't
                charrt_set_inputs(rt[client], &in);
                charrt_tick(rt[client], 1.0f / 60.0f);
                charrt_eval_base(rt[client]);
                ProcInput pi;
                charrt_proc_input(rt[client], &pi, xf_identity(), 1.0f / 60.0f);
                pi.item = &wd->sockets;
                pi.hand_ik_weight = 1.0f;
                pi.inspect = st == 5;
                pi.vehicle_onehand = st == 6;
                pi.empty_hands = st == 7;
                charrt_post(rt[client], &pi);
            }
            for (int h = 0; h < 2; h++) {
                const HandHold *g = &rt[client]->proc.hand[h];
                bool same = !memcmp(&g->sock, &ref[h].sock, sizeof(Xform)) && !memcmp(&g->pole, &ref[h].pole, sizeof(vec3)) && g->ikw == ref[h].ikw &&
                            !memcmp(g->j, ref[h].j, sizeof(HandFingers));
                if (!same) {
                    mismatches++;
                    printf("  grip differs: %s client %d hand %d (state %s)\n", names[st], client, h, holdpoint_state_name(rt[client]->proc.hp_out.state));
                }
            }
        }
    CHECK(mismatches == 0, "universal grip: %d state / client / hand combinations differ", mismatches);
    printf("  universal grip: rifle grip bit-identical in %d states x 2 clients\n", (int)ARRAY_COUNT(names));
    db->e[e] = keep;
    handpose_rehash(db);
    free(rt[0]);
    free(rt[1]);
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
        { "data/weapons", "\"hold\"", "" }, { "assets/attachments", "\"grip_l\"", "att_" }, { "assets/vehicles", "\"driver\": true", "vehicle_" },
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
                if (k && sscanf(k + strlen(key), " \"%47[^\"]\"", v) == 1) snprintf(ids[n++], 64, "%s%s%s", dirs[d].prefix, v, d == 2 ? "_driver" : "");
            }
            free(t);
        }
        SDL_free(files);
    }
    return n;
}

// Grip files (dir/items) and other sub directories.
static int count_files_sub(const char *dir, const char *sub) {
    char d[400];
    snprintf(d, sizeof(d), "%s/%s", dir, sub);
    int count = 0;
    char **files = SDL_GlobDirectory(d, "*.json", 0, &count);
    SDL_free(files);
    return files ? count : 0;
}
static int count_files(const char *dir) { return count_files_sub(dir, "items"); }

// Removes every *.json in dir (and its sub directories), then the directories.
static void clear_dir(const char *dir) {
    const char *subs[4] = { "/styles", "/items", "/cases", "" };
    for (int k = 0; k < 4; k++) {
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
    // 1) Discovery == the text scan of the assets (grips: items, weapons, attachments; vehicles:
    // one driver case each).
    HandPoseSourceList src = { 0 };
    hp_sources_from_items(&src, idb, &L->proc);
    hp_sources_from_vehicle_dir(&src, "assets/vehicles");
    static char grep_ids[128][64];
    int gn = grep_sources(grep_ids, 128);
    int matched = 0, grips = 0, cases = 0;
    for (int k = 0; k < src.count; k++) { if (src.v[k].is_case) cases++; else grips++; }
    for (int g = 0; g < gn; g++) {
        bool found = false;
        for (int k = 0; k < src.count && !found; k++) {
            char id[96];
            if (src.v[k].is_case) snprintf(id, sizeof(id), "%s", src.v[k].name);
            else handpose_entry_id(id, sizeof(id), src.v[k].category, src.v[k].name);
            found = !strcmp(id, grep_ids[g]);
        }
        CHECK(found, "discovery missed %s (found by the text scan)", grep_ids[g]);
        matched += found;
    }
    int veh = 0;
    for (int k = 0; k < src.count; k++) veh += src.v[k].is_case && src.v[k].kind == HCK_VEHICLE && strstr(src.v[k].bind, ":driver");
    CHECK(gn == grips + veh, "discovery found %d grips + %d driver seats, the text scan %d", grips, veh, gn);
    printf("  discovery: %d grip sources + %d case sources, text scan %d, matched %d\n", grips, cases, gn, matched);
    // Hands / binds of a few known sources.
    int reloads = 0, guns = 0;
    for (int i = 0; i < idb->weapon_count; i++) guns += idb->weapons[i].has_hold && idb->weapons[i].kind != WPN_MELEE;
    bool phone_case = false, passenger = false;
    for (int k = 0; k < src.count; k++) {
        const HandPoseSource *s = &src.v[k];
        if (!s->is_case && s->category == HPC_WEAPON && !strcmp(s->name, "rifle")) CHECK(s->hands == 3 && !strcmp(s->inherits, "style/rifle"), "rifle source: hands %d %s", s->hands, s->inherits);
        if (!s->is_case && s->category == HPC_WEAPON && !strcmp(s->name, "knife")) CHECK(s->hands == 2, "knife: one hand (%d)", s->hands);
        if (!s->is_case && s->category == HPC_ITEM && !strcmp(s->name, "phone")) CHECK(s->hands & 1, "phone: both hands (landscape) (%d)", s->hands);
        if (!s->is_case && s->category == HPC_ATTACHMENT) CHECK(s->hands == 1 && s->inherits[0], "attachment grip %s: left hand, inherits a host style", s->name);
        if (s->is_case && s->kind == HCK_VEHICLE && strstr(s->bind, ":driver")) CHECK(s->hands == 3 && s->vehicle[0], "vehicle case %s: both hands, setup", s->name);
        if (s->is_case && s->kind == HCK_RELOAD) reloads++;
        phone_case |= s->is_case && !strcmp(s->name, "phone_portrait");
        passenger |= s->is_case && !strcmp(s->bind, "vehicle:passenger");
    }
    CHECK(reloads == guns, "one reload case per gun (%d of %d)", reloads, guns);
    CHECK(phone_case && passenger, "phone_portrait / passenger case sources");
    // Every shipped case binds something the game has (or is a stub) and the shipped files cover
    // the interactables / seats / phone / mantle named in the spec.
    static const char *must[] = { "door_knob", "door_push", "lever", "container_lid", "pickup_ground", "ladder_rung", "vehicle_car_driver", "vehicle_moto_driver",
                                  "vehicle_passenger", "phone_portrait", "mantle_ledge" };
    for (int i = 0; i < (int)ARRAY_COUNT(must); i++) CHECK(handpose_case_find(&L->handposes, must[i]) >= 0 && L->handposes.c[handpose_case_find(&L->handposes, must[i])].on_disk, "case %s not shipped", must[i]);

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
    HandPoseScanOpts o = { dir, NULL, false };
    HandPoseScanReport r;
    hp_scan(&db, &src, &o, &r);   // the shipped assets first
    int base_files = count_files(dir) + count_files_sub(dir, "cases");
    CHECK(r.created == src.count && base_files == src.count, "first scan: %d created, %d files for %d sources", r.created, base_files, src.count);
    hp_scan(&db, &src2, &o, &r);
    CHECK(r.created == 2 && r.written == 2 && count_files(dir) + count_files_sub(dir, "cases") == base_files + 2, "new item + interactable: %d created, %d written (want 2)", r.created, r.written);
    int ci = handpose_find(&db, "test_crowbar"), ii = handpose_case_find(&db, "use_test_crank");
    CHECK(ci >= 0 && ii >= 0, "stub grip / case missing");
    if (ci >= 0) {
        const HandPoseEntry *e = &db.e[ci];
        CHECK(!strcmp(e->inherits, "style/item_large") && e->hands == (2 | (proc_hold_style(&L->proc, proc_hold_style_index(&L->proc, "item_large"))->left_hand && db2->items[1].sockets.has_left ? 1 : 0)) &&
              !e->reviewed && !handpose_entry_has_overrides(e) && e->category == HPC_ITEM,
              "crowbar stub: inherits %s hands %d reviewed %d", e->inherits, e->hands, e->reviewed);
    }
    if (ii >= 0) {
        const HandPoseCase *c = &db.c[ii];
        CHECK(c->hands == 3 && c->kind == HCK_INTERACT && !strcmp(c->bind, "interactable:test_crank") && c->auto_placed && !c->reviewed && !strcmp(c->inherits, "style/grip_grab"),
              "crank stub: hands %d bind %s auto %d", c->hands, c->bind, c->auto_placed);
        CHECK(fabsf(c->release - (c->contact + 1.5f + 0.25f)) < 1e-5f, "held interaction: the hold segment spans the hold time (%.2f)", c->release);
        CHECK(c->key_count == 1 && (c->keys[0].hand[0].set & HPL_SOCKET) && (c->keys[0].hand[1].set & HPL_SOCKET) && c->keys[0].hand[0].socket.t.x < 0,
              "auto-placed key on both hands, apart");
    }
    // The stub files read back as the same grips / cases.
    HandPoseDB back;
    handpose_db_init(&back);
    handpose_db_load_dir(&back, dir);
    int bi = handpose_case_find(&back, "use_test_crank");
    CHECK(bi >= 0 && back.c[bi].auto_placed && back.c[bi].hands == 3 && back.c[bi].key_count == 1 && handpose_find(&back, "test_crowbar") >= 0, "stub file round trip");
    handpose_db_free(&back);
    // Idempotent: a rescan creates nothing.
    hp_scan(&db, &src2, &o, &r);
    CHECK(r.created == 0 && r.written == 0, "rescan created %d", r.created);
    // Never over a file: an existing file for a source the DB doesn't know stays untouched.
    char path[400];
    snprintf(path, sizeof(path), "%s/items/test_kept.json", dir);
    const char *mine = "{ \"schema\": 3, \"reviewed\": true, \"note\": \"hand-made\" }\n";
    platform_write_file(path, mine, strlen(mine));
    hp_sources_add(&src2, HPC_ITEM, "test_kept");
    hp_scan(&db, &src2, &o, &r);
    char *now = platform_read_file(path, NULL);
    CHECK(now && !strcmp(now, mine), "an existing file was overwritten by a stub");
    free(now);
    src2.count--;
    remove(path);
    // 3) Remove both sources: their stubs are orphaned, reported, removed only on request.
    src.case_kinds |= 1u << HCK_INTERACT;   // the game's list always covers the registered interactables
    hp_scan(&db, &src, &o, &r);
    ci = handpose_find(&db, "test_crowbar");
    ii = handpose_case_find(&db, "use_test_crank");
    CHECK(ci >= 0 && db.e[ci].orphan && ii >= 0 && db.c[ii].orphan, "removed sources not orphaned");
    // + the never-overwrite case's grip (in memory only, its source removed too).
    CHECK(r.orphans == 3, "orphan count %d", r.orphans);
    char *rep = (char *)malloc(65536);
    hp_report_text(&db, rep, 65536);
    CHECK(strstr(rep, "orphaned") && strstr(rep, "test_crowbar") && strstr(rep, "use_test_crank"), "report doesn't list the orphans");
    free(rep);
    CHECK(count_files(dir) + count_files_sub(dir, "cases") == base_files + 2, "orphans deleted without confirmation");
    CHECK(!hp_remove_orphan(&db, handpose_find(&db, "rifle"), dir, NULL), "a live grip was removable");
    hp_remove_orphan(&db, handpose_find(&db, "test_crowbar"), dir, NULL);
    hp_remove_orphan_case(&db, handpose_case_find(&db, "use_test_crank"), dir, NULL);
    CHECK(count_files(dir) + count_files_sub(dir, "cases") == base_files && handpose_find(&db, "test_crowbar") < 0 && handpose_case_find(&db, "use_test_crank") < 0, "orphan removal");
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
    test_case_timeline();
    test_universal_grip(db);
    test_glue_tweaks(db);
    test_glue_items(db);
    test_glue_case_keys();
    test_discovery(db);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
