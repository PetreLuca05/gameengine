// Hand pose editor logic (src/editor/handpose/hp_edit.c, docs/HANDPOSE.md "Editor"): field
// provenance (override / inherited), seeding an override keeps the resolved pose, reset to
// inherited, joint limits on edits, presets, mirror, contacts, the canonical file only holds
// overrides, case keys as targets, undo / redo (coalescing, bounds, redo cleared by a new edit, DB
// reorder, cases, a grip's item sockets), the weapon muzzle socket (hp_muzzle.c: the weapon file
// patch, number format, bore helpers, every shipped weapon file).
#include "editor/handpose/hp_edit.h"
#include "editor/handpose/hp_muzzle.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
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

static bool pose_eq(const HandPose *a, const HandPose *b) {
    if (a->has_socket != b->has_socket || a->has_pole != b->has_pole || a->contact_count != b->contact_count) return false;
    if (memcmp(a->j, b->j, sizeof(a->j)) || a->ik_weight != b->ik_weight) return false;
    if (a->has_socket && memcmp(&a->socket, &b->socket, sizeof(a->socket))) return false;
    if (memcmp(&a->wrist, &b->wrist, sizeof(a->wrist))) return false;
    return !a->has_pole || !memcmp(&a->pole, &b->pole, sizeof(a->pole));
}

// style/rifle: relaxed fingers on both hands, a socket on the right hand. rifle inherits it and
// overrides nothing; pistol inherits nothing. One case (door) inheriting style/rifle with a key.
static void build(HandPoseDB *db) {
    handpose_db_init(db);
    int s = handpose_add(db, "style/rifle", HPC_STYLE);
    int r = handpose_add(db, "rifle", HPC_WEAPON);
    handpose_add(db, "pistol", HPC_WEAPON);
    dg_strcpy(db->e[r].inherits, sizeof(db->e[r].inherits), "style/rifle");
    for (int h = 0; h < 2; h++) {
        HandPoseLayer *l = &db->e[s].grip[h];
        handpose_preset("relaxed", l->j);
        l->set |= HPL_FINGERS_ALL;
    }
    db->e[s].grip[1].socket = xf_make(v3(0.01f, 0.02f, -0.03f), quat_axis_angle(v3(0, 1, 0), 0.2f));
    db->e[s].grip[1].set |= HPL_SOCKET;
    int c = handpose_case_add(db, "door_knob", HCK_INTERACT);
    dg_strcpy(db->c[c].inherits, sizeof(db->c[c].inherits), "style/rifle");
    dg_strcpy(db->c[c].bind, sizeof(db->c[c].bind), "interactable:door");
    db->c[c].bind_hash = dg_hash_str(db->c[c].bind);
    db->c[c].anchor = xf_make(v3(0, 0.05f, 0), quat_identity());
    handpose_link(db);
    handpose_rehash(db);
}

static void test_sources(void) {
    HandPoseDB db;
    build(&db);
    int s = handpose_find(&db, "style/rifle"), r = handpose_find(&db, "rifle"), p = handpose_find(&db, "pistol");
    HpFieldSource src = hp_field_source(&db, r, 1, HPL_FINGER(HF_INDEX));
    CHECK(src.entry == s, "index on rifle should come from style/rifle (%d)", src.entry);
    src = hp_field_source(&db, r, 0, HPL_SOCKET);
    CHECK(src.entry < 0, "no left socket anywhere: base");
    src = hp_field_source(&db, p, 0, HPL_FINGER(HF_THUMB));
    CHECK(src.entry < 0, "pistol has no chain");
    char t[96];
    hp_field_source_text(&db, hp_field_source(&db, r, 1, HPL_SOCKET), t, sizeof(t));
    CHECK(!strcmp(t, "style/rifle"), "source text '%s'", t);
    CHECK(!hp_field_overridden(&db, hp_target_grip(r), 1, HPL_FINGER(HF_INDEX)), "rifle overrides nothing yet");
    CHECK(hp_field_source(&db, -1, 1, HPL_SOCKET).entry < 0 && hp_field_source(&db, 99, 1, HPL_SOCKET).entry < 0, "bad index");
    CHECK(!hp_target_has_layers(&db, hp_target_case(0, -1)) && hp_target_valid(&db, hp_target_case(0, -1)), "a case without a key: valid, no layers");
    CHECK(!hp_target_valid(&db, hp_target_case(0, 3)), "a missing key is not a target");
    handpose_db_free(&db);
}

static void test_touch_reset(void) {
    HandPoseDB db;
    build(&db);
    int r = handpose_find(&db, "rifle");
    HpTarget tg = hp_target_grip(r);
    // Seeding an override changes nothing visible, for every field.
    HpEditDefaults def;
    for (int h = 0; h < 2; h++) { def.socket[h] = xf_make(v3(0, -0.05f, 0.1f * (float)h), quat_identity()); def.pole[h] = v3(h ? 1.0f : -1.0f, -1, 0); }
    u16 bits[] = { HPL_SOCKET, HPL_WRIST, HPL_POLE, HPL_IKW, HPL_FINGER(HF_THUMB), HPL_FINGER(HF_PINKY), HPL_CONTACTS };
    for (int h = 0; h < 2; h++)
        for (int b = 0; b < (int)ARRAY_COUNT(bits); b++) {
            HandPose before, after;
            handpose_resolve(&db, r, h, &before);
            HandPoseLayer *l = hp_edit_touch(&db, tg, h, bits[b], &def);
            handpose_resolve(&db, r, h, &after);
            CHECK(l && (l->set & bits[b]), "touch didn't set the bit");
            if (bits[b] == HPL_SOCKET && !before.has_socket) {
                CHECK(after.has_socket && !memcmp(&after.socket, &def.socket[h], sizeof(Xform)), "unset socket seeds from the item's grip (hand %d)", h);
            } else if (bits[b] == HPL_POLE && !before.has_pole) {
                CHECK(after.has_pole && v3_dist(after.pole, def.pole[h]) < 1e-6f, "unset pole seeds from the style elbow");
            } else {
                CHECK(pose_eq(&before, &after), "touching bit %x (hand %d) changed the pose", bits[b], h);
            }
            CHECK(hp_field_overridden(&db, tg, h, bits[b]), "override flag");
            CHECK(hp_edit_reset(&db, tg, h, bits[b]), "reset");
            CHECK(!hp_edit_reset(&db, tg, h, bits[b]), "second reset is a no-op");
            handpose_resolve(&db, r, h, &after);
            CHECK(pose_eq(&before, &after), "reset to inherited didn't restore (bit %x hand %d)", bits[b], h);
        }
    CHECK(!handpose_entry_has_overrides(&db.e[r]), "rifle should be clean after resets");

    // A joint edit: clamped, the finger's other joints keep their inherited values.
    HandPose inh, now;
    handpose_resolve(&db, r, 1, &inh);
    hp_edit_set_joint(&db, tg, 1, HF_INDEX, 1, (HandJoint){ 500, 0, -500 });
    handpose_resolve(&db, r, 1, &now);
    HandJoint lo, hi;
    handpose_joint_limits(HF_INDEX, 1, &lo, &hi);
    CHECK(now.j[HF_INDEX][1].curl == hi.curl && now.j[HF_INDEX][1].twist == lo.twist, "edit not clamped (%.1f %.1f)", now.j[HF_INDEX][1].curl, now.j[HF_INDEX][1].twist);
    CHECK(!memcmp(&now.j[HF_INDEX][0], &inh.j[HF_INDEX][0], sizeof(HandJoint)) && !memcmp(&now.j[HF_INDEX][2], &inh.j[HF_INDEX][2], sizeof(HandJoint)),
          "other joints of the finger moved");
    CHECK(!memcmp(now.j[HF_MIDDLE], inh.j[HF_MIDDLE], sizeof(now.j[HF_MIDDLE])), "another finger moved");
    CHECK(db.e[r].grip[1].set == HPL_FINGER(HF_INDEX), "only the index bit is set (%x)", db.e[r].grip[1].set);
    CHECK(hp_field_source(&db, r, 1, HPL_FINGER(HF_INDEX)).entry == r, "override is now the source");
    // The curl shortcut drives the three joints together (clamped per joint).
    hp_edit_set_curl(&db, tg, 1, HF_RING, 80.0f);
    handpose_resolve(&db, r, 1, &now);
    CHECK(fabsf(handpose_finger_curl(now.j, HF_RING) - 80.0f) < 1e-3f && now.j[HF_RING][1].curl > now.j[HF_RING][0].curl, "curl shortcut (%.2f)", handpose_finger_curl(now.j, HF_RING));

    // The canonical file holds only the overrides.
    char *txt = handpose_entry_write(&db.e[r]);
    CHECK(txt && strstr(txt, "\"index\"") && !strstr(txt, "\"thumb\"") && !strstr(txt, "\"ik\"") && !strstr(txt, "\"root\""), "writer holds more than the overrides:\n%s", txt ? txt : "(null)");
    free(txt);

    // The root: set, clamped, written, reset.
    hp_target_set_root(&db, tg, xf_make(v3(0.01f, 0, 0), quat_identity()));
    Xform rs = hp_target_root(&db, tg);
    rs.s = v3s(9.0f);
    hp_target_set_root(&db, tg, rs);
    CHECK(fabsf(handpose_root(&db, r).s.x - HANDPOSE_ROOT_SCALE_MAX) < 1e-6f, "root scale clamped (%.2f)", handpose_root(&db, r).s.x);
    txt = handpose_entry_write(&db.e[r]);
    CHECK(txt && strstr(txt, "\"root\""), "root not written");
    free(txt);
    CHECK(hp_edit_reset_root(&db, tg) && !db.e[r].has_root, "root reset");

    // Presets: all five fingers; mirror copies to the other hand.
    CHECK(hp_edit_preset(&db, tg, 1, "fist"), "preset");
    CHECK(!hp_edit_preset(&db, tg, 1, "nope"), "unknown preset");
    HandFingers fist;
    handpose_preset("fist", fist);
    handpose_resolve(&db, r, 1, &now);
    CHECK(!memcmp(now.j, fist, sizeof(fist)), "preset not applied");
    CHECK((db.e[r].grip[1].set & HPL_FINGERS_ALL) == HPL_FINGERS_ALL, "preset sets every finger");
    hp_edit_touch(&db, tg, 1, HPL_SOCKET, NULL)->socket = xf_make(v3(0.1f, 0, 0), quat_identity());
    hp_edit_mirror(&db, tg, 1);
    CHECK(db.e[r].grip[0].set == db.e[r].grip[1].set && fabsf(db.e[r].grip[0].socket.t.x + 0.1f) < 1e-6f, "mirror R -> L");

    // Contacts.
    CHECK(hp_edit_add_contact(&db, tg, 1, HF_INDEX, v3(0, 0, -0.1f)), "add contact");
    CHECK(hp_edit_add_contact(&db, tg, 1, HF_MIDDLE, v3(0, 0, -0.2f)), "add contact 2");
    hp_edit_remove_contact(&db, tg, 1, 0);
    HandPoseLayer *l = &db.e[r].grip[1];
    CHECK(l->contact_count == 1 && l->contacts[0].finger == HF_MIDDLE, "remove contact");
    for (int i = 0; i < 8; i++) hp_edit_add_contact(&db, tg, 1, HF_RING, v3(0, 0, 0));
    CHECK(l->contact_count == HANDPOSE_CONTACTS, "contacts bounded");

    // Commit: net identity changes, revision bumps.
    u32 h0 = db.hash, rev = db.revision;
    hp_edit_commit(&db);
    CHECK(db.hash != h0 && db.revision != rev, "commit should rehash");
    handpose_db_free(&db);
}

// Case keys as targets: seeded relative to the anchor, the anchor is the root.
static void test_case_target(void) {
    HandPoseDB db;
    build(&db);
    int ci = handpose_case_find(&db, "door_knob");
    int k = handpose_case_key_at(&db, ci, 0.16f);
    CHECK(k == 0 && db.c[ci].key_count == 1, "key added (%d)", k);
    CHECK(handpose_case_key_at(&db, ci, 0.1605f) == 0, "a key within 1 ms is the same key");
    HpTarget t = hp_target_case(ci, k);
    HandPose kp;
    hp_target_resolve(&db, t, 1, &kp);
    CHECK(v3_len(kp.socket.t) < 1e-5f, "the seeded key sits on the anchor (relative %.4f)", v3_len(kp.socket.t));
    HandPose ev;
    handpose_case_eval(&db, ci, -1, 1, 0.16f, &ev);
    CHECK(v3_dist(ev.socket.t, db.c[ci].anchor.t) < 1e-5f, "evaluated socket = the anchor");
    HandPoseLayer *l = hp_edit_touch(&db, t, 1, HPL_SOCKET, NULL);
    l->socket.t = v3(0.02f, 0, 0);
    handpose_case_eval(&db, ci, -1, 1, 0.16f, &ev);
    CHECK(v3_dist(ev.socket.t, v3(0.02f, 0.05f, 0)) < 1e-5f, "key IK target is relative to the anchor");
    // Moving the anchor carries the hands.
    hp_target_set_root(&db, hp_target_case(ci, -1), xf_make(v3(0, 0.1f, 0), quat_identity()));
    handpose_case_eval(&db, ci, -1, 1, 0.16f, &ev);
    CHECK(v3_dist(ev.socket.t, v3(0.02f, 0.1f, 0)) < 1e-5f, "anchor moves the key's hand");
    char *txt = handpose_case_write(&db.c[ci]);
    CHECK(txt && strstr(txt, "\"keys\"") && strstr(txt, "\"ik\""), "case writer");
    free(txt);
    handpose_db_free(&db);
}

static float index_curl(const HandPoseDB *db, int e) {
    HandPose p;
    handpose_resolve(db, e, 1, &p);
    return p.j[HF_INDEX][0].curl;
}

static void set_curl(HandPoseDB *db, HpUndo *u, int e, float c, u32 key) {
    hp_undo_push(u, db, hp_target_grip(e), key);
    hp_edit_set_joint(db, hp_target_grip(e), 1, HF_INDEX, 0, (HandJoint){ c, 0, 0 });
}

static void test_undo(void) {
    HandPoseDB db;
    build(&db);
    HpUndo u;
    memset(&u, 0, sizeof(u));
    int r = handpose_find(&db, "rifle");
    float c0 = index_curl(&db, r);
    CHECK(hp_undo(&u, &db).entry < 0 && hp_redo(&u, &db).entry < 0, "empty stacks");
    // A drag (same key) is one step; a second control is another.
    for (int i = 1; i <= 10; i++) set_curl(&db, &u, r, (float)i, 7);
    CHECK(u.undo_count == 1, "drag should coalesce (%d)", u.undo_count);
    set_curl(&db, &u, r, 40, 8);
    CHECK(u.undo_count == 2, "new key = new step");
    hp_undo_break(&u);
    set_curl(&db, &u, r, 50, 8);
    CHECK(u.undo_count == 3, "break ends the coalescing");
    CHECK(hp_undo(&u, &db).entry == r && index_curl(&db, r) == 40, "undo 1 -> %.1f", index_curl(&db, r));
    CHECK(hp_undo(&u, &db).entry == r && index_curl(&db, r) == 10, "undo 2 -> %.1f", index_curl(&db, r));
    CHECK(hp_redo(&u, &db).entry == r && index_curl(&db, r) == 40, "redo -> %.1f", index_curl(&db, r));
    CHECK(hp_undo(&u, &db).entry == r && hp_undo(&u, &db).entry == r && index_curl(&db, r) == c0, "back to the start");
    CHECK(!handpose_entry_has_overrides(&db.e[r]), "undo removes the override bit too");
    CHECK(hp_undo(&u, &db).entry < 0, "nothing left");
    CHECK(u.redo_count == 3, "redo holds 3 (%d)", u.redo_count);
    set_curl(&db, &u, r, 20, 9);
    CHECK(u.redo_count == 0, "a new edit clears redo");
    // reviewed and the root are part of the snapshot.
    hp_undo_push(&u, &db, hp_target_grip(r), 0);
    db.e[r].reviewed = true;
    hp_target_set_root(&db, hp_target_grip(r), xf_make(v3(0, 0.3f, 0), quat_identity()));
    hp_undo(&u, &db);
    CHECK(!db.e[r].reviewed && !db.e[r].has_root, "reviewed / root restored");
    // Cases: the whole case (anchor, keys) is one snapshot.
    int ci = handpose_case_find(&db, "door_knob");
    Xform a0 = db.c[ci].anchor;
    hp_undo_push(&u, &db, hp_target_case(ci, -1), 0);
    handpose_case_key_at(&db, ci, 0.3f);
    db.c[ci].anchor.t.x = 1.0f;
    HpTarget back = hp_undo(&u, &db);
    CHECK(back.ci == ci && db.c[ci].key_count == 0 && !memcmp(&db.c[ci].anchor, &a0, sizeof(a0)), "case undo (keys %d)", db.c[ci].key_count);
    CHECK(hp_redo(&u, &db).ci == ci && db.c[ci].key_count == 1 && db.c[ci].anchor.t.x == 1.0f, "case redo");
    // Bounded: the oldest steps drop off.
    hp_undo_clear(&u);
    for (int i = 0; i < HP_UNDO_MAX + 10; i++) set_curl(&db, &u, r, (float)(i % 90), (u32)(100 + i));
    CHECK(u.undo_count == HP_UNDO_MAX, "bounded (%d)", u.undo_count);
    int n = 0;
    while (hp_undo(&u, &db).entry >= 0) n++;
    CHECK(n == HP_UNDO_MAX, "undid %d", n);
    // Entries found by hash after a reload reorders the DB; a removed entry's steps are skipped.
    hp_undo_clear(&u);
    int p = handpose_find(&db, "pistol");
    set_curl(&db, &u, p, 33, 1);
    set_curl(&db, &u, r, 66, 2);
    HandPoseEntry tmp = db.e[0];
    db.e[0] = db.e[2];
    db.e[2] = tmp;   // style/rifle <-> pistol
    handpose_link(&db);
    r = handpose_find(&db, "rifle");
    p = handpose_find(&db, "pistol");
    CHECK(hp_undo(&u, &db).entry == r && index_curl(&db, r) != 66, "undo after reorder (rifle)");
    db.e[p] = db.e[db.count - 1];   // pistol removed
    db.count--;
    handpose_link(&db);
    CHECK(handpose_find(&db, "pistol") < 0 && hp_undo(&u, &db).entry < 0, "a removed entry's step is skipped");
    hp_undo_free(&u);
    handpose_db_free(&db);
}

// A grip step carries its item sockets through the accessors (the editor's weapon muzzle): undo /
// redo restore them in order with the grip edits; grips without sockets and stacks without
// accessors behave as before.
static vec3 g_mz;
static u32 g_mz_grip;
static bool io_get(void *ctx, u32 grip, HpItemSockets *out) {
    (void)ctx;
    if (grip != g_mz_grip) return false;
    out->muzzle = g_mz;
    return true;
}
static void io_set(void *ctx, u32 grip, const HpItemSockets *in) {
    (void)ctx;
    if (grip == g_mz_grip) g_mz = in->muzzle;
}

static void test_undo_sockets(void) {
    HandPoseDB db;
    build(&db);
    HpUndo u;
    memset(&u, 0, sizeof(u));
    u.io = (HpSocketIO){ io_get, io_set, NULL };
    int r = handpose_find(&db, "rifle"), p = handpose_find(&db, "pistol");
    g_mz_grip = db.e[r].hash;
    vec3 a = v3(0, 0.06f, -0.62f), b = v3(0, 0.05f, -0.53f);
    g_mz = a;
    float c0 = index_curl(&db, r);
    hp_undo_push(&u, &db, hp_target_grip(r), 0);   // a muzzle step
    g_mz = b;
    set_curl(&db, &u, r, 40, 0);                   // then a hand edit
    CHECK(hp_undo(&u, &db).entry == r && index_curl(&db, r) == c0 && v3_dist(g_mz, b) == 0, "undo the hand edit: the muzzle stays");
    CHECK(hp_undo(&u, &db).entry == r && v3_dist(g_mz, a) == 0 && index_curl(&db, r) == c0, "undo the muzzle step");
    CHECK(hp_redo(&u, &db).entry == r && v3_dist(g_mz, b) == 0 && index_curl(&db, r) == c0, "redo the muzzle step");
    CHECK(hp_redo(&u, &db).entry == r && index_curl(&db, r) == 40 && v3_dist(g_mz, b) == 0, "redo the hand edit");
    static HpUndoSnap snap;
    hp_undo_snap(&u, &db, hp_target_grip(r), &snap);
    CHECK(!snap.is_case && snap.hash == db.e[r].hash && snap.sock.valid && v3_dist(snap.sock.muzzle, b) == 0, "a snapshot holds the muzzle");
    hp_undo_snap(&u, &db, hp_target_grip(p), &snap);
    CHECK(!snap.sock.valid, "a grip without sockets");
    // No accessors: the grip alone (the muzzle is left as it is).
    hp_undo_free(&u);
    CHECK(!u.io.get && !u.io.set, "free forgets the accessors");
    hp_undo_push(&u, &db, hp_target_grip(r), 0);
    g_mz = a;
    hp_edit_set_joint(&db, hp_target_grip(r), 1, HF_INDEX, 0, (HandJoint){ 60, 0, 0 });
    CHECK(hp_undo(&u, &db).entry == r && index_curl(&db, r) == 40 && v3_dist(g_mz, a) == 0, "no accessors: grip only");
    hp_undo_free(&u);
    handpose_db_free(&db);
}

// Lines of a and b that differ (both as text), the last differing line of b in *line.
static int diff_lines(const char *a, const char *b, char *line, int size) {
    int n = 0;
    while (*a || *b) {
        const char *ea = strchr(a, '\n'), *eb = strchr(b, '\n');
        size_t la = ea ? (size_t)(ea - a) : strlen(a), lb = eb ? (size_t)(eb - b) : strlen(b);
        if (la != lb || memcmp(a, b, la)) {
            n++;
            snprintf(line, (size_t)size, "%.*s", (int)lb, b);
        }
        a += la + (ea ? 1 : 0);
        b += lb + (eb ? 1 : 0);
    }
    return n;
}

static void check_patch(const char *src, vec3 v, const char *want, const char *what) {
    char *p = hp_muzzle_json_patch(src, v);
    CHECK(p && !strcmp(p, want), "%s:\n%s\nwanted:\n%s", what, p ? p : "(null)", want);
    vec3 back;
    CHECK(p && hp_muzzle_json_read(p, &back) && v3_dist(back, hp_muzzle_round(v)) < 1e-7f, "%s: reads back", what);
    free(p);
}

static void test_muzzle(void) {
    char b[96];
    hp_muzzle_format(v3(0, 0.06f, -0.62f), b, sizeof(b));
    CHECK(!strcmp(b, "[0, 0.06, -0.62]"), "format %s", b);
    hp_muzzle_format(v3(-0.00001f, 0.0612345f, -0.5f), b, sizeof(b));
    CHECK(!strcmp(b, "[0, 0.0612, -0.5]"), "format: 0.1 mm, no -0 (%s)", b);
    hp_muzzle_format(v3(0.01f, -1.25f, 0.00015f), b, sizeof(b));
    CHECK(!strcmp(b, "[0.01, -1.25, 0.0002]"), "format %s", b);
    vec3 r = hp_muzzle_round(v3(0.06004f, -0.00004f, -0.61996f));
    CHECK(r.x == 0.06f && r.y == 0.0f && !signbit(r.y) && r.z == -0.62f, "round to what the file keeps");

    // In place: only the value of hold.sockets.muzzle (not the top-level "muzzle", not a string
    // holding braces / quotes) changes.
    const char *src = "{\n"
                      "  \"id\": \"rifle\",\n"
                      "  \"muzzle\": [0, 0.06, -0.62],\n"
                      "  \"hold\": {\n"
                      "    \"style\": \"rifle\",\n"
                      "    \"sockets\": {\n"
                      "      \"grip_r\": { \"pos\": [0, 0, 0], \"name\": \"a } \\\" ]\" },\n"
                      "      \"muzzle\": [0, 0.06, -0.62]\n"
                      "    }\n"
                      "  },\n"
                      "  \"model\": [{ \"offset\": [0, 0.05, -0.2] }]\n"
                      "}\n";
    vec3 m;
    CHECK(hp_muzzle_json_read(src, &m) && m.x == 0 && m.y == 0.06f && m.z == -0.62f, "read");
    const char *want = "{\n"
                       "  \"id\": \"rifle\",\n"
                       "  \"muzzle\": [0, 0.06, -0.62],\n"
                       "  \"hold\": {\n"
                       "    \"style\": \"rifle\",\n"
                       "    \"sockets\": {\n"
                       "      \"grip_r\": { \"pos\": [0, 0, 0], \"name\": \"a } \\\" ]\" },\n"
                       "      \"muzzle\": [0, 0.05, -0.5305]\n"
                       "    }\n"
                       "  },\n"
                       "  \"model\": [{ \"offset\": [0, 0.05, -0.2] }]\n"
                       "}\n";
    check_patch(src, v3(0, 0.05f, -0.5305f), want, "replace in place");
    // Added: the last member of "sockets" (indented like it), a "sockets" in "hold", one-line and
    // empty objects, CRLF files.
    check_patch("{\n  \"hold\": {\n    \"sockets\": {\n      \"grip_r\": { \"pos\": [0, 0, 0] }\n    }\n  }\n}\n", v3(0, 0.05f, -0.5f),
                "{\n  \"hold\": {\n    \"sockets\": {\n      \"grip_r\": { \"pos\": [0, 0, 0] },\n      \"muzzle\": [0, 0.05, -0.5]\n    }\n  }\n}\n", "added to sockets");
    check_patch("{\n  \"hold\": {\n    \"style\": \"pistol\"\n  }\n}\n", v3(0, 0.05f, -0.5f),
                "{\n  \"hold\": {\n    \"style\": \"pistol\",\n    \"sockets\": { \"muzzle\": [0, 0.05, -0.5] }\n  }\n}\n", "sockets added to hold");
    check_patch("{ \"hold\": { \"style\": \"x\" } }", v3(0, 0, -0.1f), "{ \"hold\": { \"style\": \"x\", \"sockets\": { \"muzzle\": [0, 0, -0.1] } } }", "one-line object");
    check_patch("{\"hold\": {\"sockets\": {}}}", v3(0, 0, -0.1f), "{\"hold\": {\"sockets\": { \"muzzle\": [0, 0, -0.1] }}}", "empty sockets");
    check_patch("{\r\n  \"hold\": {\r\n    \"style\": \"x\"\r\n  }\r\n}\r\n", v3(0, 0, -0.1f),
                "{\r\n  \"hold\": {\r\n    \"style\": \"x\",\r\n    \"sockets\": { \"muzzle\": [0, 0, -0.1] }\r\n  }\r\n}\r\n", "CRLF kept");
    // Refused: no "hold" object, malformed text.
    CHECK(!hp_muzzle_json_patch("{ \"id\": \"bat\" }", v3(0, 0, 0)), "no hold");
    CHECK(!hp_muzzle_json_patch("{ \"hold\": 3 }", v3(0, 0, 0)), "hold is not an object");
    CHECK(!hp_muzzle_json_patch("{ \"hold\": { \"sockets\": { \"muzzle\": [0, 0 ", v3(0, 0, 0)), "malformed");
    CHECK(!hp_muzzle_json_read("{ \"hold\": { \"sockets\": { \"muzzle\": [0, 1] } } }", &m), "a muzzle needs 3 numbers");

    // Bore helpers: the forward-most model part (iron sights left out): its centre line, its front face.
    static WeaponDef wd;
    memset(&wd, 0, sizeof(wd));
    wd.parts[0] = (WeaponModelPart){ v3(0, 0.05f, -0.2f), v3(0.022f, 0.035f, 0.32f), v4(1, 1, 1, 1), 0 };
    wd.parts[1] = (WeaponModelPart){ v3(0, -0.02f, 0), v3(0.016f, 0.05f, 0.025f), v4(1, 1, 1, 1), 0 };
    wd.parts[2] = (WeaponModelPart){ v3(0, 0.11f, -0.9f), v3(0.001f, 0.005f, 0.003f), v4(1, 1, 1, 1), 0 };   // a front sight post
    wd.part_count = 3;
    wd.iron_parts = 1u << 2;
    CHECK(hp_muzzle_barrel_part(&wd) == 0, "barrel part %d", hp_muzzle_barrel_part(&wd));
    vec3 bo = hp_muzzle_snap(&wd, v3(0.01f, 0.06f, -0.62f), HPM_BORE), tp = hp_muzzle_snap(&wd, v3(0.01f, 0.06f, -0.62f), HPM_TIP);
    CHECK(bo.x == 0 && bo.y == 0.05f && bo.z == -0.62f, "bore: on the centre line, z kept");
    CHECK(tp.x == 0 && tp.y == 0.05f && fabsf(tp.z + 0.52f) < 1e-6f, "tip: the front face (%.4f)", tp.z);
    wd.part_count = 0;
    CHECK(hp_muzzle_barrel_part(&wd) < 0 && hp_muzzle_snap(&wd, v3(1, 2, 3), HPM_TIP).z == 3, "no parts: unchanged");

    // Every shipped weapon file with a muzzle: a patch changes that one line only and reads back;
    // the old value goes back the same way.
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/data/weapons", DG_SOURCE_DIR);
    int count = 0, with = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    for (int i = 0; files && i < count; i++) {
        char path[800], line[256] = "";
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        char *text = platform_read_file(path, NULL);
        vec3 old, back;
        if (!text || !hp_muzzle_json_read(text, &old)) { free(text); continue; }
        with++;
        char *p = hp_muzzle_json_patch(text, v3_add(old, v3(0.001f, -0.002f, -0.0105f)));
        CHECK(p && diff_lines(text, p, line, sizeof(line)) == 1 && strstr(line, "\"muzzle\""), "%s: one line changed (%s)", files[i], line);
        CHECK(p && hp_muzzle_json_read(p, &back) && v3_dist(back, hp_muzzle_round(v3_add(old, v3(0.001f, -0.002f, -0.0105f)))) < 1e-6f, "%s: reads back", files[i]);
        char *q = p ? hp_muzzle_json_patch(p, old) : NULL;
        CHECK(q && hp_muzzle_json_read(q, &back) && v3_dist(back, hp_muzzle_round(old)) < 1e-6f && diff_lines(text, q, line, sizeof(line)) <= 1, "%s: the old value back", files[i]);
        free(q);
        free(p);
        free(text);
    }
    SDL_free(files);
    CHECK(with >= 8, "weapon files with hold.sockets.muzzle: %d", with);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    test_sources();
    test_touch_reset();
    test_case_target();
    test_undo();
    test_undo_sockets();
    test_muzzle();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
