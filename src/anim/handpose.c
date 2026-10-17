// Hand pose data: universal grips, cases (timeline keys), resolution, limits, presets, mirror,
// finger application, contact solver, the JSON format and the schema 1 migration
// (docs/HANDPOSE.md).
#include "handpose.h"
#include "procedural_internal.h"
#include "cJSON.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static const char *k_cat_names[HPC_COUNT] = { "style", "weapon", "item", "attachment", "vehicle", "interactable" };
static const char *k_kind_names[HCK_COUNT] = { "interact", "vehicle", "item", "reload", "mantle", "stub" };
static const char *k_finger_names[HAND_FINGERS] = { "thumb", "index", "middle", "ring", "pinky" };
static const char *k_hand_names[2] = { "left", "right" };
// Schema 1 per-state layers (migration only): "default" + the hold point states, the
// interactable and vehicle-control states.
enum { V1_DEFAULT, V1_HIP, V1_USE = 11, V1_HOLD, V1_DRIVE, V1_DRIVE_ONEHAND, V1_COUNT };
static const char *k_v1_states[V1_COUNT] = { "default", "hip", "ads", "sprint", "crouch", "prone", "empty", "inspect", "vehicle_onehand",
                                             "phone_portrait", "phone_landscape", "use", "hold", "drive", "drive_onehand" };
// Share of a finger's curl per joint (proximal, middle, distal) and the thumb's scale: the split
// the old single-angle curl used, kept so the "curl" shorthand and the presets reproduce it.
static const float k_split[3] = { 0.30f, 0.38f, 0.32f };
#define THUMB_SCALE 0.6f
// Interaction timing the runtime used before cases (src/player/hands.c): reach 0.16 s, a press
// held 0.38 s, return 0.22 s.
#define CASE_REACH 0.16f
#define CASE_PRESS 0.38f
#define CASE_RETURN 0.22f

const char *handpose_category_name(int c) { return (c >= 0 && c < HPC_COUNT) ? k_cat_names[c] : "?"; }
const char *handpose_case_kind_name(int k) { return (k >= 0 && k < HCK_COUNT) ? k_kind_names[k] : "?"; }
int handpose_case_kind_from_name(const char *s) {
    for (int i = 0; s && i < HCK_COUNT; i++) if (!strcmp(s, k_kind_names[i])) return i;
    return -1;
}
const char *handpose_finger_name(int f) { return (f >= 0 && f < HAND_FINGERS) ? k_finger_names[f] : "?"; }
int handpose_finger_from_name(const char *s) {
    for (int i = 0; s && i < HAND_FINGERS; i++) if (!strcmp(s, k_finger_names[i])) return i;
    return -1;
}
static int category_from_name(const char *s) {
    for (int i = 0; s && i < HPC_COUNT; i++) if (!strcmp(s, k_cat_names[i])) return i;
    return HPC_ITEM;
}
static int v1_state_from_name(const char *s) {
    for (int i = 0; s && i < V1_COUNT; i++) if (!strcmp(s, k_v1_states[i])) return i;
    return -1;
}
void handpose_entry_id(char *out, int out_size, int category, const char *name) {
    static const char *prefix[HPC_COUNT] = { "style/", "", "", "att_", "veh_", "use_" };
    snprintf(out, (size_t)out_size, "%s%s", category >= 0 && category < HPC_COUNT ? prefix[category] : "", name ? name : "");
}
u32 handpose_entry_hash(int category, const char *name) {
    char id[96];
    handpose_entry_id(id, sizeof(id), category, name);
    return dg_hash_str(id);
}
u32 handpose_style_hash(const char *style_name) {
    char id[64];
    snprintf(id, sizeof(id), "style/%s", style_name ? style_name : "");
    return dg_hash_str(id);
}

// ------------------------------------------------------------------ DB

void handpose_db_init(HandPoseDB *db) { memset(db, 0, sizeof(*db)); }
void handpose_db_free(HandPoseDB *db) { free(db->e); free(db->c); memset(db, 0, sizeof(*db)); }
void handpose_db_clear(HandPoseDB *db) { db->count = 0; db->case_count = 0; db->hash = 0; db->revision++; }

void handpose_layer_clear(HandPoseLayer *l) {
    memset(l, 0, sizeof(*l));
    l->socket = xf_identity();
    l->wrist = xf_identity();
    l->ik_weight = 1.0f;
}
void handpose_entry_clear_layers(HandPoseEntry *e) {
    for (int h = 0; h < 2; h++) handpose_layer_clear(&e->grip[h]);
    e->has_root = false;
    e->root = xf_identity();
}

int handpose_find_hash(const HandPoseDB *db, u32 hash) {
    if (!hash || !db) return -1;
    for (int i = 0; i < db->count; i++) if (db->e[i].hash == hash) return i;
    return -1;
}
int handpose_find(const HandPoseDB *db, const char *id) { return id && id[0] ? handpose_find_hash(db, dg_hash_str(id)) : -1; }

int handpose_add(HandPoseDB *db, const char *id, int category) {
    int i = handpose_find(db, id);
    if (i >= 0) return i;
    if (db->count >= db->cap) {
        int cap = db->cap ? db->cap * 2 : 64;
        HandPoseEntry *n = (HandPoseEntry *)realloc(db->e, sizeof(HandPoseEntry) * (size_t)cap);
        if (!n) return -1;
        db->e = n;
        db->cap = cap;
    }
    HandPoseEntry *e = &db->e[db->count];
    memset(e, 0, sizeof(*e));
    dg_strcpy(e->id, sizeof(e->id), id);
    e->hash = dg_hash_str(e->id);
    e->category = (u8)category;
    e->parent = -1;
    handpose_entry_clear_layers(e);
    return db->count++;
}

// ------------------------------------------------------------------ cases

void handpose_case_defaults(HandPoseCase *c) {
    c->anchor = xf_identity();
    c->contact = CASE_REACH;
    c->use = CASE_REACH + CASE_PRESS * 0.4f;
    c->release = CASE_REACH + CASE_PRESS;
    c->duration = c->release + CASE_RETURN;
    c->distance = 0.75f;
    c->key_count = 0;
}

int handpose_case_find_hash(const HandPoseDB *db, u32 hash) {
    if (!hash || !db) return -1;
    for (int i = 0; i < db->case_count; i++) if (db->c[i].hash == hash) return i;
    return -1;
}
int handpose_case_find(const HandPoseDB *db, const char *id) { return id && id[0] ? handpose_case_find_hash(db, dg_hash_str(id)) : -1; }
int handpose_case_find_bind_hash(const HandPoseDB *db, u32 bind_hash) {
    if (!bind_hash || !db) return -1;
    // A reviewed case wins over a stub bound to the same thing (a person authored it).
    int stub = -1;
    for (int i = 0; i < db->case_count; i++)
        if (db->c[i].bind_hash == bind_hash) {
            if (handpose_case_has_overrides(&db->c[i])) return i;
            if (stub < 0) stub = i;
        }
    return stub;
}
int handpose_case_find_bind(const HandPoseDB *db, const char *bind) { return bind && bind[0] ? handpose_case_find_bind_hash(db, dg_hash_str(bind)) : -1; }

int handpose_case_add(HandPoseDB *db, const char *id, int kind) {
    int i = handpose_case_find(db, id);
    if (i >= 0) return i;
    if (db->case_count >= db->case_cap) {
        int cap = db->case_cap ? db->case_cap * 2 : 32;
        HandPoseCase *n = (HandPoseCase *)realloc(db->c, sizeof(HandPoseCase) * (size_t)cap);
        if (!n) return -1;
        db->c = n;
        db->case_cap = cap;
    }
    HandPoseCase *c = &db->c[db->case_count];
    memset(c, 0, sizeof(*c));
    dg_strcpy(c->id, sizeof(c->id), id);
    dg_strcpy(c->label, sizeof(c->label), id);
    c->hash = dg_hash_str(c->id);
    c->kind = (u8)(kind >= 0 && kind < HCK_COUNT ? kind : HCK_STUB);
    c->parent = -1;
    c->hands = 2;
    handpose_case_defaults(c);
    return db->case_count++;
}

static bool xf_is_identity(Xform x) { return x.t.x == 0 && x.t.y == 0 && x.t.z == 0 && x.r.x == 0 && x.r.y == 0 && x.r.z == 0 && x.r.w == 1; }

bool handpose_case_has_overrides(const HandPoseCase *c) { return c->key_count > 0 || !xf_is_identity(c->anchor) || c->reviewed; }

void handpose_case_sort_keys(HandPoseCase *c) {
    for (int i = 1; i < c->key_count; i++)
        for (int k = i; k > 0 && c->keys[k - 1].t > c->keys[k].t; k--) {
            HandCaseKey t = c->keys[k];
            c->keys[k] = c->keys[k - 1];
            c->keys[k - 1] = t;
        }
}

bool handpose_case_key_remove(HandPoseCase *c, int k) {
    if (k < 0 || k >= c->key_count) return false;
    memmove(&c->keys[k], &c->keys[k + 1], sizeof(HandCaseKey) * (size_t)(c->key_count - k - 1));
    c->key_count--;
    return true;
}

void handpose_link(HandPoseDB *db) {
    for (int i = 0; i < db->count; i++) {
        HandPoseEntry *e = &db->e[i];
        e->parent = e->inherits[0] ? (i16)handpose_find(db, e->inherits) : -1;
        if (e->parent == i) e->parent = -1;
    }
    // Cut cycles: walk each chain, a revisit drops the link that closes it.
    for (int i = 0; i < db->count; i++) {
        int seen[16], n = 0, k = i;
        while (k >= 0 && n < 16) {
            for (int q = 0; q < n; q++)
                if (seen[q] == k) {
                    log_warn("handpose: inheritance cycle at %s, link to %s dropped", db->e[seen[n - 1]].id, db->e[k].id);
                    db->e[seen[n - 1]].parent = -1;
                    k = -1;
                    break;
                }
            if (k < 0) break;
            seen[n++] = k;
            k = db->e[k].parent;
        }
    }
    for (int i = 0; i < db->case_count; i++) db->c[i].parent = db->c[i].inherits[0] ? (i16)handpose_find(db, db->c[i].inherits) : -1;
}

bool handpose_entry_has_overrides(const HandPoseEntry *e) { return e->grip[0].set || e->grip[1].set || e->has_root; }

static u32 hash_f(u32 h, float f) { return anim_hash(&f, sizeof(f), h); }
static u32 hash_xf(u32 h, Xform x) {
    float v[10] = { x.t.x, x.t.y, x.t.z, x.r.x, x.r.y, x.r.z, x.r.w, x.s.x, x.s.y, x.s.z };
    return anim_hash(v, sizeof(v), h);
}
static u32 hash_layer(u32 h, const HandPoseLayer *l, u32 tag) {
    if (!l->set) return h;
    tag |= (u32)l->set << 8;
    h = anim_hash(&tag, sizeof(tag), h);
    if (l->set & HPL_SOCKET) h = hash_xf(h, l->socket);
    if (l->set & HPL_WRIST) h = hash_xf(h, l->wrist);
    if (l->set & HPL_POLE) { h = hash_f(h, l->pole.x); h = hash_f(h, l->pole.y); h = hash_f(h, l->pole.z); }
    if (l->set & HPL_IKW) h = hash_f(h, l->ik_weight);
    for (int f = 0; f < HAND_FINGERS; f++)
        if (l->set & HPL_FINGER(f)) h = anim_hash(l->j[f], sizeof(l->j[f]), h);
    return h;
}
static int cmp_u32(const void *a, const void *b) {
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

// Net identity over the grips and cases that change a pose, in hash order (independent of file
// order; stubs and editor-only flags such as `reviewed` on grips don't count).
void handpose_rehash(HandPoseDB *db) {
    int total = db->count + db->case_count;
    u32 *order = (u32 *)malloc(sizeof(u32) * 2 * (size_t)(total + 1));
    int n = 0;
    for (int i = 0; i < db->count; i++)
        if (handpose_entry_has_overrides(&db->e[i])) { order[n * 2] = db->e[i].hash; order[n * 2 + 1] = (u32)i; n++; }
    for (int i = 0; i < db->case_count; i++)
        if (handpose_case_has_overrides(&db->c[i])) { order[n * 2] = db->c[i].hash; order[n * 2 + 1] = 0x80000000u | (u32)i; n++; }
    qsort(order, (size_t)n, sizeof(u32) * 2, cmp_u32);
    u32 h = anim_hash_str("handposes", ANIM_HASH_SEED) ^ HANDPOSE_SCHEMA;
    for (int k = 0; k < n; k++) {
        u32 idx = order[k * 2 + 1];
        if (idx & 0x80000000u) {
            const HandPoseCase *c = &db->c[idx & 0x7FFFFFFFu];
            h = anim_hash_str(c->id, h);
            h = anim_hash_str(c->inherits, h);
            h = anim_hash_str(c->bind, h);
            h = hash_xf(h, c->anchor);
            float m[4] = { c->duration, c->contact, c->use, c->release };
            h = anim_hash(m, sizeof(m), h);
            for (int q = 0; q < c->key_count; q++) {
                h = hash_f(h, c->keys[q].t);
                for (int hd = 0; hd < 2; hd++) h = hash_layer(h, &c->keys[q].hand[hd], (u32)(q * 2 + hd));
            }
            continue;
        }
        const HandPoseEntry *e = &db->e[idx];
        h = anim_hash_str(e->id, h);
        h = anim_hash_str(e->inherits, h);
        if (e->has_root) h = hash_xf(h, e->root);
        for (int hd = 0; hd < 2; hd++) h = hash_layer(h, &e->grip[hd], (u32)hd);
    }
    free(order);
    db->hash = h;
    db->revision++;
}

// ------------------------------------------------------------------ resolve

static void apply_layer(HandPose *o, const HandPoseLayer *l) {
    if (!l->set) return;
    if (l->set & HPL_SOCKET) { o->socket = l->socket; o->has_socket = true; }
    if (l->set & HPL_WRIST) o->wrist = l->wrist;
    if (l->set & HPL_POLE) { o->pole = l->pole; o->has_pole = true; }
    if (l->set & HPL_IKW) o->ik_weight = l->ik_weight;
    for (int f = 0; f < HAND_FINGERS; f++)
        if (l->set & HPL_FINGER(f)) memcpy(o->j[f], l->j[f], sizeof(o->j[f]));
    if (l->set & HPL_CONTACTS) {
        o->contact_count = l->contact_count;
        memcpy(o->contacts, l->contacts, sizeof(o->contacts));
    }
}

static void pose_base(HandPose *out) {
    memset(out, 0, sizeof(*out));
    out->socket = xf_identity();
    out->wrist = xf_identity();
    out->ik_weight = 1.0f;
}

void handpose_resolve(const HandPoseDB *db, int entry, int hand, HandPose *out) { handpose_resolve_over(db, -1, entry, hand, out); }

static int chain_of(const HandPoseDB *db, int entry, int *chain, int max) {
    int n = 0;
    for (int k = entry; k >= 0 && k < db->count && n < max; k = db->e[k].parent) chain[n++] = k;
    return n;
}

void handpose_resolve_over(const HandPoseDB *db, int base, int entry, int hand, HandPose *out) {
    pose_base(out);
    if (!db || hand < 0 || hand > 1) return;
    if (entry >= db->count) entry = -1;
    if (base >= db->count) base = -1;
    // chain[] leaf first: the entry's chain, then the base's (unless the entry already inherits it).
    int chain[16], n = chain_of(db, entry, chain, 8);
    bool has_base = base < 0;
    for (int i = 0; i < n && !has_base; i++) has_base = chain[i] == base;
    if (!has_base) n += chain_of(db, base, chain + n, 8);
    for (int i = n - 1; i >= 0; i--) apply_layer(out, &db->e[chain[i]].grip[hand]);
    handpose_clamp_fingers(out->j);
    out->ik_weight = saturate(out->ik_weight);
}

Xform handpose_root_clamp(Xform r) {
    // Unit quaternions stay bit-exact (lossless round trip); only drifted ones get normalized.
    float l2 = r.r.x * r.r.x + r.r.y * r.r.y + r.r.z * r.r.z + r.r.w * r.r.w;
    if (fabsf(l2 - 1.0f) > 1e-5f) r.r = quat_norm(r.r);
    float s = clampf(r.s.x, HANDPOSE_ROOT_SCALE_MIN, HANDPOSE_ROOT_SCALE_MAX);
    r.s = v3(s, s, s);
    return r;
}

Xform handpose_root(const HandPoseDB *db, int entry) {
    if (!db || entry < 0 || entry >= db->count || !db->e[entry].has_root) return xf_identity();
    return db->e[entry].root;
}

// ------------------------------------------------------------------ case evaluation

// A driver seat's anchor moves the steering frame itself (the gameplay target already has it).
bool handpose_case_anchor_in_target(const HandPoseCase *c) { return c && c->kind == HCK_VEHICLE && strstr(c->bind, ":driver") != NULL; }
Xform handpose_case_anchor_applied(const HandPoseCase *c) { return c && !handpose_case_anchor_in_target(c) ? c->anchor : xf_identity(); }

void handpose_case_key_pose(const HandPoseDB *db, int ci, int base, int key, int hand, HandPose *out) {
    const HandPoseCase *c = db && ci >= 0 && ci < db->case_count ? &db->c[ci] : NULL;
    handpose_resolve_over(db, base, c ? c->parent : -1, hand, out);
    Xform sock = xf_identity();
    if (c && key >= 0 && key < c->key_count) {
        const HandPoseLayer *l = &c->keys[key].hand[hand & 1];
        HandPose k = *out;
        k.has_socket = false;
        k.socket = xf_identity();
        apply_layer(&k, l);
        handpose_clamp_fingers(k.j);
        k.ik_weight = saturate(k.ik_weight);
        if (k.has_socket) sock = k.socket;
        *out = k;
    }
    // The case anchor is the root: the key's IK target is relative to it (the wrist folded in).
    out->socket = xf_mul(handpose_case_anchor_applied(c), xf_mul(sock, out->wrist));
    out->wrist = xf_identity();
    out->has_socket = true;
}

static void pose_lerp(HandPose *o, const HandPose *a, const HandPose *b, float u) {
    *o = *a;
    o->socket.t = v3_lerp(a->socket.t, b->socket.t, u);
    o->socket.r = quat_nlerp(a->socket.r, b->socket.r, u);
    o->has_pole = a->has_pole || b->has_pole;
    if (a->has_pole && b->has_pole) o->pole = v3_lerp(a->pole, b->pole, u);
    else o->pole = a->has_pole ? a->pole : b->pole;
    o->ik_weight = lerpf(a->ik_weight, b->ik_weight, u);
    for (int f = 0; f < HAND_FINGERS; f++)
        for (int k = 0; k < HAND_FINGER_JOINTS; k++) {
            o->j[f][k].curl = lerpf(a->j[f][k].curl, b->j[f][k].curl, u);
            o->j[f][k].spread = lerpf(a->j[f][k].spread, b->j[f][k].spread, u);
            o->j[f][k].twist = lerpf(a->j[f][k].twist, b->j[f][k].twist, u);
        }
    if (u >= 0.5f) { o->contact_count = b->contact_count; memcpy(o->contacts, b->contacts, sizeof(o->contacts)); }
}

void handpose_case_eval(const HandPoseDB *db, int ci, int base, int hand, float t, HandPose *out) {
    const HandPoseCase *c = db && ci >= 0 && ci < db->case_count ? &db->c[ci] : NULL;
    if (!c || c->key_count == 0) { handpose_case_key_pose(db, c ? ci : -1, base, -1, hand, out); return; }
    int i = 0;
    while (i < c->key_count && c->keys[i].t <= t) i++;
    if (i == 0) { handpose_case_key_pose(db, ci, base, 0, hand, out); return; }
    if (i == c->key_count) { handpose_case_key_pose(db, ci, base, c->key_count - 1, hand, out); return; }
    HandPose a, b;
    handpose_case_key_pose(db, ci, base, i - 1, hand, &a);
    handpose_case_key_pose(db, ci, base, i, hand, &b);
    float span = c->keys[i].t - c->keys[i - 1].t;
    float u = span > 1e-6f ? smoothstepf(0.0f, 1.0f, (t - c->keys[i - 1].t) / span) : 1.0f;
    pose_lerp(out, &a, &b, u);
}

int handpose_case_key_at(HandPoseDB *db, int ci, float t) {
    if (!db || ci < 0 || ci >= db->case_count) return -1;
    HandPoseCase *c = &db->c[ci];
    for (int k = 0; k < c->key_count; k++) if (fabsf(c->keys[k].t - t) < 1e-3f) return k;
    if (c->key_count >= HANDPOSE_CASE_KEYS) return -1;
    HandCaseKey nk;
    memset(&nk, 0, sizeof(nk));
    nk.t = t;
    const char *name = fabsf(t - c->contact) < 1e-3f ? "contact" : fabsf(t - c->release) < 1e-3f ? "release" : fabsf(t - c->use) < 1e-3f ? "use" : "key";
    dg_strcpy(nk.name, sizeof(nk.name), name);
    Xform inv_anchor = xf_inverse(handpose_case_anchor_applied(c));
    for (int h = 0; h < 2; h++) {
        HandPose p;
        handpose_case_eval(db, ci, -1, h, t, &p);
        HandPoseLayer *l = &nk.hand[h];
        handpose_layer_clear(l);
        l->set = HPL_SOCKET | HPL_IKW | HPL_FINGERS_ALL | (p.has_pole ? HPL_POLE : 0);
        l->socket = xf_mul(inv_anchor, p.socket);
        l->socket.r = quat_norm(l->socket.r);
        l->socket.s = v3(1, 1, 1);
        l->pole = p.pole;
        l->ik_weight = p.ik_weight;
        memcpy(l->j, p.j, sizeof(l->j));
    }
    c = &db->c[ci];
    c->keys[c->key_count++] = nk;
    handpose_case_sort_keys(c);
    for (int k = 0; k < c->key_count; k++) if (c->keys[k].t == t) return k;
    return -1;
}

float handpose_case_reach_time(const HandPoseCase *c) { return c ? fmaxf(0.02f, c->contact) : CASE_REACH; }
float handpose_case_return_time(const HandPoseCase *c) { return c ? fmaxf(0.02f, c->duration - c->release) : CASE_RETURN; }

float handpose_case_time(const HandPoseCase *c, int phase, float u) {
    u = saturate(u);
    float contact = c ? c->contact : CASE_REACH, release = c ? c->release : CASE_REACH + CASE_PRESS;
    float dur = c ? c->duration : CASE_REACH + CASE_PRESS + CASE_RETURN;
    // The segment ends exactly on the markers (keys placed on them are hit bit-exactly).
    if (phase == HCP_REACH) return u >= 1.0f ? contact : u * contact;
    if (phase == HCP_HOLD) return u >= 1.0f ? release : contact + u * (release - contact);
    return u >= 1.0f ? dur : release + u * (dur - release);
}

void handpose_case_phase(const HandPoseCase *c, float t, int *phase, float *u) {
    float contact = c ? c->contact : CASE_REACH, release = c ? c->release : CASE_REACH + CASE_PRESS;
    float dur = c ? c->duration : CASE_REACH + CASE_PRESS + CASE_RETURN;
    if (t < contact) { *phase = HCP_REACH; *u = contact > 1e-6f ? saturate(t / contact) : 1.0f; }
    else if (t < release) { *phase = HCP_HOLD; *u = saturate((t - contact) / fmaxf(1e-6f, release - contact)); }
    else { *phase = HCP_RETURN; *u = saturate((t - release) / fmaxf(1e-6f, dur - release)); }
}

// ------------------------------------------------------------------ limits / presets

void handpose_joint_limits(int finger, int joint, HandJoint *lo, HandJoint *hi) {
    // Degrees. Thumb: CMC (wide spread / twist), MCP, IP. Fingers: MCP (spread), PIP, DIP.
    static const HandJoint thumb_lo[3] = { { -30, -45, -45 }, { -15, -15, -20 }, { -20, -10, -15 } };
    static const HandJoint thumb_hi[3] = { { 70, 60, 45 }, { 70, 15, 20 }, { 90, 10, 15 } };
    static const HandJoint fing_lo[3] = { { -20, -25, -15 }, { -5, -5, -10 }, { -10, -5, -10 } };
    static const HandJoint fing_hi[3] = { { 95, 25, 15 }, { 110, 5, 10 }, { 90, 5, 10 } };
    joint = DG_CLAMP(joint, 0, 2);
    *lo = finger == HF_THUMB ? thumb_lo[joint] : fing_lo[joint];
    *hi = finger == HF_THUMB ? thumb_hi[joint] : fing_hi[joint];
}

HandJoint handpose_clamp_joint(int finger, int joint, HandJoint v) {
    HandJoint lo, hi;
    handpose_joint_limits(finger, joint, &lo, &hi);
    v.curl = clampf(v.curl, lo.curl, hi.curl);
    v.spread = clampf(v.spread, lo.spread, hi.spread);
    v.twist = clampf(v.twist, lo.twist, hi.twist);
    return v;
}

void handpose_clamp_fingers(HandFingers j) {
    for (int f = 0; f < HAND_FINGERS; f++)
        for (int k = 0; k < HAND_FINGER_JOINTS; k++) j[f][k] = handpose_clamp_joint(f, k, j[f][k]);
}

void handpose_from_curl(const float curl[5], HandFingers out) {
    memset(out, 0, sizeof(HandFingers));
    for (int f = 0; f < HAND_FINGERS; f++)
        for (int k = 0; k < HAND_FINGER_JOINTS; k++) out[f][k].curl = curl[f] * k_split[k] * (f == HF_THUMB ? THUMB_SCALE : 1.0f);
}

float handpose_finger_curl(const HandFingers j, int f) {
    if (f < 0 || f >= HAND_FINGERS) return 0.0f;
    return j[f][0].curl / (k_split[0] * (f == HF_THUMB ? THUMB_SCALE : 1.0f));
}

void handpose_set_finger_curl(HandFingers j, int f, float curl) {
    if (f < 0 || f >= HAND_FINGERS) return;
    for (int k = 0; k < HAND_FINGER_JOINTS; k++) {
        j[f][k].curl = curl * k_split[k] * (f == HF_THUMB ? THUMB_SCALE : 1.0f);
        j[f][k] = handpose_clamp_joint(f, k, j[f][k]);
    }
}

typedef struct PresetDef { const char *name; float curl[5]; float spread[5]; } PresetDef;
// Curl thumb..pinky (degrees, split over the joints like the old tables), spread at the base joint.
static const PresetDef k_presets[] = {
    { "open", { 0, 0, 0, 0, 0 }, { 0, 8, 0, -8, -14 } },
    { "relaxed", { 10, 15, 20, 25, 30 }, { 0 } },
    { "fist", { 50, 95, 100, 100, 100 }, { 0 } },
    { "trigger_finger", { 40, 15, 85, 90, 95 }, { 0 } },
    { "pistol_grip", { 40, 70, 80, 85, 90 }, { 0 } },
    { "rifle_grip", { 20, 25, 70, 75, 80 }, { 0 } },
    { "foregrip", { 40, 50, 55, 60, 65 }, { 0 } },
    { "handlebar", { 35, 60, 70, 75, 80 }, { 15, 0, 0, 0, 0 } },
    { "flat_palm", { 0, 5, 5, 5, 5 }, { 0 } },
    { "pinch", { 60, 55, 20, 15, 15 }, { 30, 0, 0, 0, 0 } },
    { "point", { 50, 0, 95, 95, 95 }, { 0 } },
};
int handpose_preset_count(void) { return (int)ARRAY_COUNT(k_presets); }
const char *handpose_preset_name(int i) { return (i >= 0 && i < handpose_preset_count()) ? k_presets[i].name : "?"; }
bool handpose_preset(const char *name, HandFingers out) {
    for (int i = 0; i < handpose_preset_count(); i++) {
        if (strcmp(name, k_presets[i].name)) continue;
        handpose_from_curl(k_presets[i].curl, out);
        for (int f = 0; f < HAND_FINGERS; f++) out[f][0].spread = k_presets[i].spread[f];
        handpose_clamp_fingers(out);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ mirror

Xform handpose_mirror_xform(Xform x) {
    // Reflect across x = 0 and keep the frame right-handed: R' = M R M  ->  q (x, -y, -z, w).
    Xform o = x;
    o.t.x = -x.t.x;
    o.r = quat_norm((quat){ x.r.x, -x.r.y, -x.r.z, x.r.w });
    return o;
}

void handpose_mirror_layer(const HandPoseLayer *in, HandPoseLayer *out) {
    HandPoseLayer m = *in;
    m.socket = handpose_mirror_xform(in->socket);
    m.wrist = handpose_mirror_xform(in->wrist);
    m.pole.x = -in->pole.x;
    for (int c = 0; c < in->contact_count && c < HANDPOSE_CONTACTS; c++) m.contacts[c].point.x = -in->contacts[c].point.x;
    *out = m;
}

void handpose_mirror_entry_hand(HandPoseEntry *e, int from_hand) {
    from_hand = from_hand ? 1 : 0;
    handpose_mirror_layer(&e->grip[from_hand], &e->grip[!from_hand]);
}

void handpose_mirror_case_hand(HandPoseCase *c, int from_hand) {
    from_hand = from_hand ? 1 : 0;
    for (int k = 0; k < c->key_count; k++) handpose_mirror_layer(&c->keys[k].hand[from_hand], &c->keys[k].hand[!from_hand]);
}

// ------------------------------------------------------------------ fingers

static const BoneRole k_first[2][5] = {
    { BR_L_THUMB1, BR_L_INDEX1, BR_L_MIDDLE1, BR_L_RING1, BR_L_PINKY1 },
    { BR_R_THUMB1, BR_R_INDEX1, BR_R_MIDDLE1, BR_R_RING1, BR_R_PINKY1 },
};

static int first_child(const Skeleton *s, int b) {
    for (int c = b + 1; c < s->count; c++) if (s->parent[c] == b) return c;
    return -1;
}

static vec3 bone_dir(const Skeleton *s, const Xform *model, int b) {
    int c = first_child(s, b);
    vec3 d = c >= 0 ? v3_sub(model[c].t, model[b].t) : (s->parent[b] >= 0 ? v3_sub(model[b].t, model[s->parent[b]].t) : v3(0, 0, -1));
    float l = v3_len(d);
    return l > 1e-6f ? v3_scale(d, 1.0f / l) : v3(0, 0, -1);
}

static void apply_finger(const Skeleton *s, Pose *pose, Xform *model, int side, int f, const HandJoint *jt, const HandFrames *hf, float weight) {
    int hand = s->role[side ? BR_R_HAND : BR_L_HAND];
    int b = s->role[k_first[side][f]];
    if (hand < 0) return;
    float sd = side ? 1.0f : -1.0f;
    for (int k = 0; k < HAND_FINGER_JOINTS && b >= 0; k++) {
        HandJoint v = jt[k];
        Xform palm = xf_mul(model[hand], hf->palm[side]);
        if (v.twist != 0.0f) proc_rotate_bone(s, pose, model, b, quat_axis_angle(bone_dir(s, model, b), v.twist * DG_DEG2RAD * weight * sd));
        if (v.spread != 0.0f) {
            vec3 up = quat_rotate(palm.r, v3(0, 1, 0));
            if (f == HF_THUMB) {
                vec3 ax = v3_cross(bone_dir(s, model, b), up);
                if (v3_len2(ax) > 1e-8f) proc_rotate_bone(s, pose, model, b, quat_axis_angle(v3_norm(ax), -v.spread * DG_DEG2RAD * weight));
            } else {
                proc_rotate_bone(s, pose, model, b, quat_axis_angle(up, -hf->thumb_side[side] * v.spread * DG_DEG2RAD * weight));
            }
        }
        if (v.curl != 0.0f) {
            vec3 axis = quat_rotate(palm.r, hf->curl_axis[side]);
            if (f == HF_THUMB) axis = quat_rotate(palm.r, v3(0, 0, side ? 1.0f : -1.0f));   // the thumb folds across the palm
            proc_rotate_bone(s, pose, model, b, quat_axis_angle(axis, v.curl * DG_DEG2RAD * weight));
        }
        b = first_child(s, b);
    }
}

void handpose_apply_fingers(const Skeleton *s, Pose *pose, Xform *model, int side, const HandFingers j, const HandFrames *hf, float weight) {
    if (weight <= 0 || s->role[side ? BR_R_HAND : BR_L_HAND] < 0) return;
    for (int f = 0; f < HAND_FINGERS; f++) apply_finger(s, pose, model, side, f, j[f], hf, weight);
}

int handpose_finger_bone(const Skeleton *s, int side, int f, int joint) {
    int b = s->role[k_first[side][f]];
    for (int k = 0; k < joint && b >= 0; k++) b = first_child(s, b);
    return b;
}

vec3 handpose_fingertip(const Skeleton *s, const Xform *model, int side, int finger) {
    int d = handpose_finger_bone(s, side, finger, 2);
    if (d < 0) return v3(0, 0, 0);
    int end = first_child(s, d);
    if (end >= 0) return model[end].t;
    int m = s->parent[d];
    return m >= 0 ? v3_madd(model[d].t, v3_sub(model[d].t, model[m].t), 0.8f) : model[d].t;
}

// ------------------------------------------------------------------ contact solver

float handpose_box_signed_dist(const HandBox *b, vec3 p) {
    vec3 l = quat_rotate(quat_conj(b->rot), v3_sub(p, b->center));
    vec3 q = v3(fabsf(l.x) - b->half.x, fabsf(l.y) - b->half.y, fabsf(l.z) - b->half.z);
    vec3 o = v3(fmaxf(q.x, 0), fmaxf(q.y, 0), fmaxf(q.z, 0));
    return v3_len(o) + fminf(fmaxf(q.x, fmaxf(q.y, q.z)), 0.0f);
}

typedef struct SolveCtx {
    const Skeleton *s;
    const Pose *pose;
    const Xform *model;
    const HandFrames *hf;
    const HandBox *boxes;
    int box_count, side, finger;
    HandJoint base[3];
    float radius;
} SolveCtx;

static Pose g_solve_pose;   // the solver runs on the main thread (editor / tests): scratch, not on the stack
static Xform g_solve_model[ANIM_MAX_BONES];

static void solve_joints(const SolveCtx *c, float t, HandJoint out[3]) {
    for (int k = 0; k < 3; k++) {
        out[k] = c->base[k];
        out[k].curl += t * k_split[k] * (c->finger == HF_THUMB ? THUMB_SCALE : 1.0f);
        out[k] = handpose_clamp_joint(c->finger, k, out[k]);
    }
}

// Finger surface distance to the nearest box (sphere-swept chain, sampled), and its tip.
static float solve_eval(const SolveCtx *c, float t, vec3 *tip) {
    pose_copy(&g_solve_pose, c->pose);
    memcpy(g_solve_model, c->model, sizeof(Xform) * (size_t)c->s->count);
    HandJoint jt[3];
    solve_joints(c, t, jt);
    apply_finger(c->s, &g_solve_pose, g_solve_model, c->side, c->finger, jt, c->hf, 1.0f);
    vec3 pts[4];
    for (int k = 0; k < 3; k++) {
        int b = handpose_finger_bone(c->s, c->side, c->finger, k);
        pts[k] = b >= 0 ? g_solve_model[b].t : v3(0, 0, 0);
    }
    pts[3] = handpose_fingertip(c->s, g_solve_model, c->side, c->finger);
    *tip = pts[3];
    float best = 1e9f;
    // Only the segments past the proximal joint: the knuckle itself sits on the grip by design.
    for (int seg = 1; seg < 3; seg++)
        for (int q = 0; q <= 6; q++) {
            vec3 p = v3_lerp(pts[seg], pts[seg + 1], (float)q / 6.0f);
            for (int i = 0; i < c->box_count; i++) best = fminf(best, handpose_box_signed_dist(&c->boxes[i], p) - c->radius);
        }
    return best;
}

HandContactResult handpose_contact_solve(const Skeleton *s, const Pose *pose, const Xform *model, int side, int finger, HandFingers j,
                                         const HandFrames *hf, const HandBox *boxes, int box_count, const vec3 *target, float radius, float tol) {
    HandContactResult r = { 0 };
    SolveCtx c = { s, pose, model, hf, boxes, box_count, side, finger, { { 0 } }, radius };
    memcpy(c.base, j[finger], sizeof(c.base));
    // Range: from where the first joint reaches its lower curl limit (opening) to where the first
    // reaches its upper one (closing); all three joints move together.
    float t_min = -1e9f, t_max = 1e9f;
    for (int k = 0; k < 3; k++) {
        HandJoint lo, hi;
        handpose_joint_limits(finger, k, &lo, &hi);
        float share = k_split[k] * (finger == HF_THUMB ? THUMB_SCALE : 1.0f);
        t_max = fminf(t_max, (hi.curl - c.base[k].curl) / share);
        t_min = fmaxf(t_min, (lo.curl - c.base[k].curl) / share);
    }
    t_max = fmaxf(t_max, 0.0f);
    t_min = fminf(t_min, 0.0f);
    const float step = 2.0f;
    vec3 tip;
    float d0 = solve_eval(&c, 0, &tip);
    float best_t = 0, best_target = target ? v3_dist(tip, *target) : 0;
    // Bracket [t_free, t_hit]: the finger is clear at t_free and touching at t_hit (adjacent steps).
    float t_free = 0, t_hit = 0;
    bool bracket = false;
    if (d0 < 0) {
        // Already inside: open step by step until clear.
        t_hit = 0;
        for (float t = -step; t >= t_min - step * 0.5f; t -= step) {
            float tt = fmaxf(t, t_min);
            float d = solve_eval(&c, tt, &tip);
            r.steps++;
            if (d >= 0) { t_free = tt; bracket = true; break; }
            t_hit = tt;
            if (tt <= t_min) break;
        }
        if (!bracket) {
            // Fully open and still inside: report it, leave the finger open.
            r.tip_dist = solve_eval(&c, t_min, &tip);
            solve_joints(&c, t_min, j[finger]);
            r.curl_add = t_min;
            return r;
        }
    } else {
        for (float t = step; t <= t_max + step * 0.5f; t += step) {
            float tt = fminf(t, t_max);
            float d = solve_eval(&c, tt, &tip);
            r.steps++;
            if (d < 0) { t_hit = tt; bracket = true; break; }
            t_free = tt;
            if (target) {
                float dt = v3_dist(tip, *target);
                if (dt < best_target) { best_target = dt; best_t = tt; }
            }
            if (tt >= t_max) break;
        }
    }
    float t_final;
    if (bracket) {
        // Bisect the clear / touching bracket until the surface gap is inside the tolerance.
        float lo = t_free, hi = t_hit;
        float d = solve_eval(&c, lo, &tip);
        for (int i = 0; i < 48 && d > tol; i++) {
            float m = 0.5f * (lo + hi);
            float dm = solve_eval(&c, m, &tip);
            r.steps++;
            if (dm >= 0) { lo = m; d = dm; } else hi = m;
        }
        t_final = lo;
        r.touched = true;
        r.tip_dist = d;
    } else {
        t_final = target ? best_t : t_max;
        r.tip_dist = solve_eval(&c, t_final, &tip);
    }
    solve_joints(&c, t_final, j[finger]);
    r.curl_add = t_final;
    return r;
}

// ------------------------------------------------------------------ euler

vec3 handpose_quat_to_euler_deg(quat q) {
    // quat_euler(yaw, pitch, roll) = Ry * Rx * Rz.
    float r12 = 2.0f * (q.y * q.z - q.w * q.x);
    float r10 = 2.0f * (q.x * q.y + q.w * q.z), r11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    float r02 = 2.0f * (q.x * q.z + q.w * q.y), r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float pitch = asinf(clampf(-r12, -1.0f, 1.0f));
    return v3(pitch * DG_RAD2DEG, atan2f(r02, r22) * DG_RAD2DEG, atan2f(r10, r11) * DG_RAD2DEG);
}
quat handpose_euler_deg_to_quat(vec3 e) { return quat_euler(e.y * DG_DEG2RAD, e.x * DG_DEG2RAD, e.z * DG_DEG2RAD); }

// ------------------------------------------------------------------ JSON read

static bool jvec(const cJSON *o, const char *k, float *out, int n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < n) return false;
    for (int i = 0; i < n; i++) out[i] = (float)cJSON_GetArrayItem(v, i)->valuedouble;
    return true;
}
static const char *jstring(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static float jfloat(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static bool parse_xform(const cJSON *o, Xform *out) {
    if (!cJSON_IsObject(o)) return false;
    Xform x = xf_identity();
    float v[4];
    if (jvec(o, "pos", v, 3)) x.t = v3(v[0], v[1], v[2]);
    if (jvec(o, "q", v, 4)) {
        // Saved quaternions are unit already: keep them bit-exact (lossless round trip), only
        // hand-typed ones get normalized.
        x.r = (quat){ v[0], v[1], v[2], v[3] };
        float l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
        if (fabsf(l2 - 1.0f) > 1e-5f) x.r = quat_norm(x.r);
    }
    else if (jvec(o, "rot", v, 3)) x.r = handpose_euler_deg_to_quat(v3(v[0], v[1], v[2]));
    *out = x;
    return true;
}

// The IK target with the legacy wrist offset folded in (the editor's IK control is one frame).
static void fold_wrist(HandPoseLayer *l) {
    if ((l->set & HPL_SOCKET) && (l->set & HPL_WRIST)) {
        l->socket = xf_mul(l->socket, l->wrist);
        l->socket.s = v3(1, 1, 1);
        l->wrist = xf_identity();
        l->set = (u16)(l->set & ~HPL_WRIST);
    }
}

static void parse_layer(HandPoseLayer *l, const cJSON *o) {
    handpose_layer_clear(l);
    if (!cJSON_IsObject(o)) return;
    if (parse_xform(cJSON_GetObjectItemCaseSensitive(o, "ik"), &l->socket) || parse_xform(cJSON_GetObjectItemCaseSensitive(o, "socket"), &l->socket)) l->set |= HPL_SOCKET;
    if (parse_xform(cJSON_GetObjectItemCaseSensitive(o, "wrist"), &l->wrist)) l->set |= HPL_WRIST;
    float v[5];
    if (jvec(o, "pole", v, 3)) { l->pole = v3(v[0], v[1], v[2]); l->set |= HPL_POLE; }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "ik_weight");
    if (cJSON_IsNumber(w)) { l->ik_weight = saturate((float)w->valuedouble); l->set |= HPL_IKW; }
    // Shorthand: 5 curl angles (thumb..pinky).
    if (jvec(o, "curl", v, 5)) {
        handpose_from_curl(v, l->j);
        l->set |= HPL_FINGERS_ALL;
    }
    const cJSON *fg = cJSON_GetObjectItemCaseSensitive(o, "fingers");
    for (int f = 0; f < HAND_FINGERS && fg; f++) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(fg, k_finger_names[f]);
        if (!cJSON_IsArray(a)) continue;
        for (int k = 0; k < HAND_FINGER_JOINTS && k < cJSON_GetArraySize(a); k++) {
            const cJSON *jj = cJSON_GetArrayItem(a, k);
            float c[3] = { 0, 0, 0 };
            for (int q = 0; q < 3 && q < cJSON_GetArraySize(jj); q++) c[q] = (float)cJSON_GetArrayItem(jj, q)->valuedouble;
            l->j[f][k] = handpose_clamp_joint(f, k, (HandJoint){ c[0], c[1], c[2] });
        }
        l->set |= (u16)HPL_FINGER(f);
    }
    const cJSON *ct = cJSON_GetObjectItemCaseSensitive(o, "contacts"), *c;
    if (cJSON_IsArray(ct)) {
        l->set |= HPL_CONTACTS;
        cJSON_ArrayForEach(c, ct) {
            if (l->contact_count >= HANDPOSE_CONTACTS) break;
            int f = handpose_finger_from_name(jstring(c, "finger", ""));
            if (f < 0 || !jvec(c, "point", v, 3)) continue;
            l->contacts[l->contact_count++] = (HandContact){ (u8)f, v3(v[0], v[1], v[2]) };
        }
    }
    fold_wrist(l);
}

// src's fields on top of dst (a sparse merge).
static void layer_over(HandPoseLayer *dst, const HandPoseLayer *src) {
    if (src->set & HPL_SOCKET) dst->socket = src->socket;
    if (src->set & HPL_WRIST) dst->wrist = src->wrist;
    if (src->set & HPL_POLE) dst->pole = src->pole;
    if (src->set & HPL_IKW) dst->ik_weight = src->ik_weight;
    for (int f = 0; f < HAND_FINGERS; f++)
        if (src->set & HPL_FINGER(f)) memcpy(dst->j[f], src->j[f], sizeof(dst->j[f]));
    if (src->set & HPL_CONTACTS) { dst->contact_count = src->contact_count; memcpy(dst->contacts, src->contacts, sizeof(dst->contacts)); }
    dst->set |= src->set;
}

static void note_add(HandPoseMigrateNote *n, const char *fmt, ...) {
    if (!n) return;
    size_t l = strlen(n->text);
    if (l + 4 >= sizeof(n->text)) return;
    if (l) { memcpy(n->text + l, "; ", 3); l += 2; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->text + l, sizeof(n->text) - l, fmt, ap);
    va_end(ap);
}

// Which fields of `s` (a state layer) differ from the grip they were merged against.
static void note_diff(HandPoseMigrateNote *n, const char *state, int hand, const HandPoseLayer *grip, const HandPoseLayer *s) {
    char what[160] = "";
    int wl = 0;
#define DIFF(bit, name, field) \
    if ((s->set & (bit)) && (!(grip->set & (bit)) || memcmp(&grip->field, &s->field, sizeof(grip->field)))) wl += snprintf(what + wl, sizeof(what) - (size_t)wl, "%s%s", wl ? "," : "", name)
    DIFF(HPL_SOCKET, "ik", socket);
    DIFF(HPL_WRIST, "wrist", wrist);
    DIFF(HPL_POLE, "pole", pole);
    DIFF(HPL_IKW, "ik_weight", ik_weight);
    for (int f = 0; f < HAND_FINGERS && wl < (int)sizeof(what) - 12; f++)
        if ((s->set & HPL_FINGER(f)) && (!(grip->set & HPL_FINGER(f)) || memcmp(grip->j[f], s->j[f], sizeof(grip->j[f])))) wl += snprintf(what + wl, sizeof(what) - (size_t)wl, "%s%s", wl ? "," : "", k_finger_names[f]);
    if ((s->set & HPL_CONTACTS) && (!(grip->set & HPL_CONTACTS) || grip->contact_count != s->contact_count || memcmp(grip->contacts, s->contacts, sizeof(grip->contacts))))
        wl += snprintf(what + wl, sizeof(what) - (size_t)wl, "%scontacts", wl ? "," : "");
#undef DIFF
    if (!wl) return;
    if (n) n->dropped++;
    note_add(n, "%s %s: %s differ%s (dropped)", state, k_hand_names[hand], what, strchr(what, ',') ? "" : "s");
}

// Schema 0 (hand-written, like a controller.json hold style): top-level "curl_r" / "curl_l"
// (5 angles) -> right / left "default" layers with the "curl" shorthand. Returns the schema read.
static int migrate_v0(cJSON *root) {
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema");
    int from = cJSON_IsNumber(sv) ? sv->valueint : 0;
    if (from >= 1) return from;
    static const char *keys[2] = { "curl_l", "curl_r" };
    for (int h = 0; h < 2; h++) {
        cJSON *c = cJSON_DetachItemFromObjectCaseSensitive(root, keys[h]);
        if (!c) continue;
        cJSON *hand = cJSON_GetObjectItemCaseSensitive(root, k_hand_names[h]);
        if (!hand) hand = cJSON_AddObjectToObject(root, k_hand_names[h]);
        cJSON *def = cJSON_GetObjectItemCaseSensitive(hand, "default");
        if (!def) def = cJSON_AddObjectToObject(hand, "default");
        if (!cJSON_GetObjectItemCaseSensitive(def, "curl") && !cJSON_GetObjectItemCaseSensitive(def, "fingers")) cJSON_AddItemToObject(def, "curl", c);
        else cJSON_Delete(c);
    }
    return from;
}

// A hand block of a schema 0 / 1 file: per state layers. The grip is "default" with "hip" on top
// (the hip version is kept); `merge_extra` (a state, -1 none) also goes on top (cases: use / drive).
// Every other state that differs from the result is noted and dropped.
static void parse_v1_hand(HandPoseLayer *out, const cJSON *hand, int h, int merge_extra, int merge_extra2, HandPoseMigrateNote *note, const char *id) {
    HandPoseLayer st[V1_COUNT];
    for (int s = 0; s < V1_COUNT; s++) handpose_layer_clear(&st[s]);
    const cJSON *it;
    cJSON_ArrayForEach(it, hand) {
        int s = v1_state_from_name(it->string);
        if (s < 0) { log_warn("handpose: %s: unknown state '%s'", id, it->string ? it->string : "?"); continue; }
        parse_layer(&st[s], it);
    }
    handpose_layer_clear(out);
    layer_over(out, &st[V1_DEFAULT]);
    layer_over(out, &st[V1_HIP]);
    if (merge_extra >= 0) layer_over(out, &st[merge_extra]);
    bool use_extra2 = merge_extra2 >= 0 && (merge_extra < 0 || !st[merge_extra].set);
    if (use_extra2) layer_over(out, &st[merge_extra2]);
    fold_wrist(out);
    for (int s = 0; s < V1_COUNT; s++) {
        if (s == V1_DEFAULT || s == V1_HIP || s == merge_extra || !st[s].set) continue;
        if (s == merge_extra2 && use_extra2) continue;
        note_diff(note, k_v1_states[s], h, out, &st[s]);
    }
}

static bool has_v1_layers(const cJSON *root) {
    for (int h = 0; h < 2; h++) {
        const cJSON *hand = cJSON_GetObjectItemCaseSensitive(root, k_hand_names[h]), *it;
        cJSON_ArrayForEach(it, hand) if (v1_state_from_name(it->string) >= 0 && cJSON_IsObject(it)) return true;
    }
    return false;
}

static u8 parse_hands(const cJSON *root, u8 def) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "hands"), *it;
    if (!cJSON_IsArray(a)) return def;
    u8 m = 0;
    cJSON_ArrayForEach(it, a) if (cJSON_IsString(it)) m |= !strcmp(it->valuestring, "left") ? 1 : !strcmp(it->valuestring, "right") ? 2 : 0;
    return m;
}

bool handpose_entry_parse(HandPoseEntry *e, const cJSON *root_in, char *err, int err_size, HandPoseMigrateNote *note) {
    if (!cJSON_IsObject(root_in)) { snprintf(err, (size_t)err_size, "not a JSON object"); return false; }
    cJSON *root = cJSON_Duplicate(root_in, true);
    int from = migrate_v0(root);
    if (note) { memset(note, 0, sizeof(*note)); note->from = from; }
    if (from > HANDPOSE_SCHEMA) {
        snprintf(err, (size_t)err_size, "schema %d is newer than this build (%d)", from, HANDPOSE_SCHEMA);
        cJSON_Delete(root);
        return false;
    }
    const char *id = jstring(root, "id", e->id);
    if (id && id != e->id) dg_strcpy(e->id, sizeof(e->id), id);
    e->hash = dg_hash_str(e->id);
    dg_strcpy(e->inherits, sizeof(e->inherits), jstring(root, "inherits", ""));
    dg_strcpy(e->source, sizeof(e->source), jstring(root, "source", e->source));
    dg_strcpy(e->label, sizeof(e->label), jstring(root, "label", e->label[0] ? e->label : e->id));
    e->category = (u8)category_from_name(jstring(root, "category", handpose_category_name(e->category)));
    e->reviewed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reviewed"));
    e->auto_placed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "auto_placed"));
    e->hands = parse_hands(root, e->hands);
    handpose_entry_clear_layers(e);
    bool v1 = from < 2 || has_v1_layers(root);
    for (int h = 0; h < 2; h++) {
        const cJSON *hand = cJSON_GetObjectItemCaseSensitive(root, k_hand_names[h]);
        if (v1) parse_v1_hand(&e->grip[h], hand, h, -1, -1, note, e->id);
        else parse_layer(&e->grip[h], hand);
    }
    const cJSON *ro = cJSON_GetObjectItemCaseSensitive(root, "root");
    Xform r;
    if (parse_xform(ro, &r)) {
        float s = jfloat(ro, "scale", 1.0f);
        r.s = v3(s, s, s);
        e->root = handpose_root_clamp(r);
        e->has_root = true;
    }
    if (v1 && note && note->text[0]) log_info("handpose: %s migrated from schema %d: %s", e->id, from, note->text);
    cJSON_Delete(root);
    return true;
}

static void parse_case_body(HandPoseCase *c, const cJSON *root) {
    dg_strcpy(c->label, sizeof(c->label), jstring(root, "label", c->label[0] ? c->label : c->id));
    dg_strcpy(c->source, sizeof(c->source), jstring(root, "source", c->source));
    dg_strcpy(c->inherits, sizeof(c->inherits), jstring(root, "inherits", ""));
    int k = handpose_case_kind_from_name(jstring(root, "kind", ""));
    c->kind = (u8)(k >= 0 ? k : HCK_STUB);
    dg_strcpy(c->bind, sizeof(c->bind), jstring(root, "bind", ""));
    c->bind_hash = c->bind[0] ? dg_hash_str(c->bind) : 0;
    c->hands = parse_hands(root, 2);
    c->reviewed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reviewed"));
    c->auto_placed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "auto_placed"));
    const cJSON *su = cJSON_GetObjectItemCaseSensitive(root, "setup");
    dg_strcpy(c->prop, sizeof(c->prop), jstring(su, "prop", ""));
    c->distance = jfloat(su, "distance", 0.75f);
    dg_strcpy(c->vehicle, sizeof(c->vehicle), jstring(su, "vehicle", ""));
    c->seat = (i8)jfloat(su, "seat", 0);
    dg_strcpy(c->item, sizeof(c->item), jstring(su, "item", ""));
    dg_strcpy(c->hold_state, sizeof(c->hold_state), jstring(su, "hold_state", ""));
    if (!parse_xform(cJSON_GetObjectItemCaseSensitive(root, "anchor"), &c->anchor)) c->anchor = xf_identity();
    const cJSON *mk = cJSON_GetObjectItemCaseSensitive(root, "markers");
    c->contact = jfloat(mk, "contact", c->contact);
    c->use = jfloat(mk, "use", c->use);
    c->release = jfloat(mk, "release", c->release);
    c->duration = jfloat(root, "duration", c->duration);
    // Markers ordered inside the timeline.
    c->duration = fmaxf(c->duration, 0.05f);
    c->contact = clampf(c->contact, 0.0f, c->duration);
    c->release = clampf(c->release, c->contact, c->duration);
    c->use = clampf(c->use, c->contact, c->release);
}

bool handpose_case_parse(HandPoseCase *c, const cJSON *root, char *err, int err_size) {
    if (!cJSON_IsObject(root)) { snprintf(err, (size_t)err_size, "not a JSON object"); return false; }
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema");
    int from = cJSON_IsNumber(sv) ? sv->valueint : 0;
    if (from > HANDPOSE_SCHEMA) { snprintf(err, (size_t)err_size, "schema %d is newer than this build (%d)", from, HANDPOSE_SCHEMA); return false; }
    handpose_case_defaults(c);
    parse_case_body(c, root);
    c->key_count = 0;
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "keys"), *it;
    cJSON_ArrayForEach(it, keys) {
        if (c->key_count >= HANDPOSE_CASE_KEYS) { log_warn("handpose: case %s: more than %d keys, the rest dropped", c->id, HANDPOSE_CASE_KEYS); break; }
        HandCaseKey *k = &c->keys[c->key_count++];
        memset(k, 0, sizeof(*k));
        k->t = clampf(jfloat(it, "t", 0.0f), 0.0f, c->duration);
        dg_strcpy(k->name, sizeof(k->name), jstring(it, "name", "key"));
        for (int h = 0; h < 2; h++) parse_layer(&k->hand[h], cJSON_GetObjectItemCaseSensitive(it, k_hand_names[h]));
    }
    handpose_case_sort_keys(c);
    return true;
}

bool handpose_case_from_v1(HandPoseCase *c, const cJSON *root_in, const char *id, HandPoseMigrateNote *note) {
    cJSON *root = cJSON_Duplicate(root_in, true);
    int from = migrate_v0(root);
    if (note) { memset(note, 0, sizeof(*note)); note->from = from; }
    bool veh = !strcmp(jstring(root, "category", ""), "vehicle") || !strncmp(id, "veh_", 4);
    const char *name = id + (veh ? 4 : (!strncmp(id, "use_", 4) ? 4 : 0));
    memset(c, 0, sizeof(*c));
    handpose_case_defaults(c);
    if (veh) {
        snprintf(c->id, sizeof(c->id), "vehicle_%s_driver", name);
        snprintf(c->bind, sizeof(c->bind), "vehicle:%s:driver", name);
        dg_strcpy(c->vehicle, sizeof(c->vehicle), name);
        c->kind = HCK_VEHICLE;
    } else {
        snprintf(c->id, sizeof(c->id), "use_%s", name);
        snprintf(c->bind, sizeof(c->bind), "interactable:%s", name);
        dg_strcpy(c->prop, sizeof(c->prop), name);
        c->kind = HCK_INTERACT;
    }
    c->hash = dg_hash_str(c->id);
    c->bind_hash = dg_hash_str(c->bind);
    c->parent = -1;
    dg_strcpy(c->label, sizeof(c->label), jstring(root, "label", c->id));
    dg_strcpy(c->source, sizeof(c->source), jstring(root, "source", ""));
    dg_strcpy(c->inherits, sizeof(c->inherits), jstring(root, "inherits", ""));
    c->hands = parse_hands(root, 3);
    c->reviewed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reviewed"));
    c->auto_placed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "auto_placed"));
    // One key at the contact marker: default + hip + the use / hold (drive) layer.
    HandPoseLayer hl[2];
    for (int h = 0; h < 2; h++)
        parse_v1_hand(&hl[h], cJSON_GetObjectItemCaseSensitive(root, k_hand_names[h]), h, veh ? V1_DRIVE : V1_USE, veh ? -1 : V1_HOLD, note, c->id);
    if (hl[0].set || hl[1].set) {
        HandCaseKey *k = &c->keys[c->key_count++];
        memset(k, 0, sizeof(*k));
        k->t = c->contact;
        dg_strcpy(k->name, sizeof(k->name), "contact");
        k->hand[0] = hl[0];
        k->hand[1] = hl[1];
    }
    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------------------ JSON write

typedef struct Sb { char *p; size_t n, cap; } Sb;
static void sb_put(Sb *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + l + 1) cap *= 2;
        char *np = (char *)realloc(b->p, cap);
        if (!np) return;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}
static void sb_printf(Sb *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_put(b, tmp);
}
// Shortest text that reads back to the same float (lossless round trip, readable values).
static void fmt_float(char *out, size_t n, float f) {
    if (f == 0.0f) { snprintf(out, n, "0"); return; }
    for (int prec = 6; prec <= 9; prec++) {
        snprintf(out, n, "%.*g", prec, (double)f);
        if (strtof(out, NULL) == f) return;
    }
}
static void sb_float(Sb *b, float f) {
    char t[32];
    fmt_float(t, sizeof(t), f);
    sb_put(b, t);
}
static void sb_vec(Sb *b, const float *v, int n) {
    sb_put(b, "[");
    for (int i = 0; i < n; i++) { if (i) sb_put(b, ", "); sb_float(b, v[i]); }
    sb_put(b, "]");
}
static void sb_xform(Sb *b, Xform x) {
    float t[3] = { x.t.x, x.t.y, x.t.z }, q[4] = { x.r.x, x.r.y, x.r.z, x.r.w };
    sb_put(b, "{ \"pos\": ");
    sb_vec(b, t, 3);
    sb_put(b, ", \"q\": ");
    sb_vec(b, q, 4);
    sb_put(b, " }");
}


static void write_layer(Sb *b, const HandPoseLayer *l, const char *ind) {
    bool first = true;
#define FIELD(name) do { sb_printf(b, "%s\n%s  \"%s\": ", first ? "" : ",", ind, name); first = false; } while (0)
    sb_put(b, "{");
    if (l->set & HPL_SOCKET) { FIELD("ik"); sb_xform(b, l->socket); }
    if (l->set & HPL_WRIST) { FIELD("wrist"); sb_xform(b, l->wrist); }
    if (l->set & HPL_POLE) { FIELD("pole"); sb_vec(b, &l->pole.x, 3); }
    if (l->set & HPL_IKW) { FIELD("ik_weight"); sb_float(b, l->ik_weight); }
    if (l->set & HPL_FINGERS_ALL) {
        FIELD("fingers");
        sb_put(b, "{");
        bool ff = true;
        for (int f = 0; f < HAND_FINGERS; f++) {
            if (!(l->set & HPL_FINGER(f))) continue;
            sb_printf(b, "%s\n%s    \"%s\": [", ff ? "" : ",", ind, k_finger_names[f]);
            ff = false;
            for (int k = 0; k < HAND_FINGER_JOINTS; k++) {
                if (k) sb_put(b, ", ");
                sb_vec(b, &l->j[f][k].curl, 3);
            }
            sb_put(b, "]");
        }
        sb_printf(b, "\n%s  }", ind);
    }
    if (l->set & HPL_CONTACTS) {
        FIELD("contacts");
        sb_put(b, "[");
        for (int c = 0; c < l->contact_count; c++) {
            sb_printf(b, "%s{ \"finger\": \"%s\", \"point\": ", c ? ", " : "", k_finger_names[l->contacts[c].finger % HAND_FINGERS]);
            sb_vec(b, &l->contacts[c].point.x, 3);
            sb_put(b, " }");
        }
        sb_put(b, "]");
    }
#undef FIELD
    sb_put(b, first ? "}" : "\n}");
}

static void sb_jstr(Sb *b, const char *s) {
    sb_put(b, "\"");
    for (; *s; s++) {
        char c[3] = { *s, 0, 0 };
        if (*s == '"' || *s == '\\') { c[0] = '\\'; c[1] = *s; }
        sb_put(b, c);
    }
    sb_put(b, "\"");
}

static void sb_hands(Sb *b, u8 hands) {
    sb_put(b, "[");
    bool f = true;
    for (int h = 0; h < 2; h++) if (hands & (1 << h)) { sb_printf(b, "%s\"%s\"", f ? "" : ", ", k_hand_names[h]); f = false; }
    sb_put(b, "]");
}

// A layer block with its closing brace at `ind` (write_layer closes at column 0 + ind).
static void sb_layer(Sb *b, const HandPoseLayer *l, const char *ind) {
    if (!l->set) { sb_put(b, "{}"); return; }
    write_layer(b, l, ind);
    // write_layer ends with "\n}": indent the brace.
    if (b->n >= 2 && b->p[b->n - 1] == '}' && b->p[b->n - 2] == '\n') {
        b->n--;
        b->p[b->n] = 0;
        sb_printf(b, "%s}", ind);
    }
}

char *handpose_entry_write(const HandPoseEntry *e) {
    Sb b = { 0 };
    sb_printf(&b, "{\n  \"schema\": %d,\n  \"id\": ", HANDPOSE_SCHEMA);
    sb_jstr(&b, e->id);
    sb_put(&b, ",\n  \"label\": ");
    sb_jstr(&b, e->label[0] ? e->label : e->id);
    sb_printf(&b, ",\n  \"category\": \"%s\",\n  \"source\": ", handpose_category_name(e->category));
    sb_jstr(&b, e->source);
    sb_put(&b, ",\n  \"inherits\": ");
    sb_jstr(&b, e->inherits);
    sb_put(&b, ",\n  \"hands\": ");
    sb_hands(&b, e->hands);
    sb_printf(&b, ",\n  \"reviewed\": %s,\n  \"auto_placed\": %s", e->reviewed ? "true" : "false", e->auto_placed ? "true" : "false");
    if (e->has_root) {
        sb_put(&b, ",\n  \"root\": { \"pos\": ");
        sb_vec(&b, &e->root.t.x, 3);
        float q[4] = { e->root.r.x, e->root.r.y, e->root.r.z, e->root.r.w };
        sb_put(&b, ", \"q\": ");
        sb_vec(&b, q, 4);
        sb_put(&b, ", \"scale\": ");
        sb_float(&b, e->root.s.x);
        sb_put(&b, " }");
    }
    for (int h = 0; h < 2; h++) {
        sb_printf(&b, ",\n  \"%s\": ", k_hand_names[h]);
        sb_layer(&b, &e->grip[h], "  ");
    }
    sb_put(&b, "\n}\n");
    return b.p;
}

char *handpose_case_write(const HandPoseCase *c) {
    Sb b = { 0 };
    sb_printf(&b, "{\n  \"schema\": %d,\n  \"id\": ", HANDPOSE_SCHEMA);
    sb_jstr(&b, c->id);
    sb_put(&b, ",\n  \"label\": ");
    sb_jstr(&b, c->label[0] ? c->label : c->id);
    sb_printf(&b, ",\n  \"kind\": \"%s\",\n  \"bind\": ", handpose_case_kind_name(c->kind));
    sb_jstr(&b, c->bind);
    sb_put(&b, ",\n  \"source\": ");
    sb_jstr(&b, c->source);
    sb_put(&b, ",\n  \"inherits\": ");
    sb_jstr(&b, c->inherits);
    sb_put(&b, ",\n  \"hands\": ");
    sb_hands(&b, c->hands);
    sb_printf(&b, ",\n  \"reviewed\": %s,\n  \"auto_placed\": %s", c->reviewed ? "true" : "false", c->auto_placed ? "true" : "false");
    // setup: only what is set
    sb_put(&b, ",\n  \"setup\": {");
    bool f = true;
#define SETUP_STR(k, v) if ((v)[0]) { sb_printf(&b, "%s \"%s\": ", f ? "" : ",", k); sb_jstr(&b, v); f = false; }
    SETUP_STR("prop", c->prop);
    SETUP_STR("vehicle", c->vehicle);
    SETUP_STR("item", c->item);
    SETUP_STR("hold_state", c->hold_state);
#undef SETUP_STR
    if (c->vehicle[0]) { sb_printf(&b, "%s \"seat\": %d", f ? "" : ",", c->seat); f = false; }
    sb_printf(&b, "%s \"distance\": ", f ? "" : ",");
    sb_float(&b, c->distance);
    sb_put(&b, " }");
    sb_put(&b, ",\n  \"anchor\": ");
    sb_xform(&b, c->anchor);
    sb_put(&b, ",\n  \"duration\": ");
    sb_float(&b, c->duration);
    sb_put(&b, ",\n  \"markers\": { \"contact\": ");
    sb_float(&b, c->contact);
    sb_put(&b, ", \"use\": ");
    sb_float(&b, c->use);
    sb_put(&b, ", \"release\": ");
    sb_float(&b, c->release);
    sb_put(&b, " },\n  \"keys\": [");
    for (int k = 0; k < c->key_count; k++) {
        const HandCaseKey *key = &c->keys[k];
        sb_printf(&b, "%s\n    { \"t\": ", k ? "," : "");
        sb_float(&b, key->t);
        sb_put(&b, ", \"name\": ");
        sb_jstr(&b, key->name);
        for (int h = 0; h < 2; h++) {
            if (!key->hand[h].set) continue;
            sb_printf(&b, ",\n      \"%s\": ", k_hand_names[h]);
            sb_layer(&b, &key->hand[h], "      ");
        }
        sb_put(&b, " }");
    }
    sb_put(&b, c->key_count ? "\n  ]\n}\n" : "]\n}\n");
    return b.p;
}

// ------------------------------------------------------------------ files

void handpose_entry_rel_path(const HandPoseEntry *e, const char *dir_rel, char *out, int out_size) {
    if (!strncmp(e->id, "style/", 6)) snprintf(out, (size_t)out_size, "%s/styles/%s.json", dir_rel, e->id + 6);
    else snprintf(out, (size_t)out_size, "%s/items/%s.json", dir_rel, e->id);
}
void handpose_case_rel_path(const HandPoseCase *c, const char *dir_rel, char *out, int out_size) {
    snprintf(out, (size_t)out_size, "%s/cases/%s.json", dir_rel, c->id);
}

static bool save_text(char *txt, const char *path_abs) {
    if (!txt) return false;
    bool ok = platform_write_file(path_abs, txt, strlen(txt));
    free(txt);
    return ok;
}
bool handpose_entry_save_file(const HandPoseEntry *e, const char *path_abs) { return save_text(handpose_entry_write(e), path_abs); }
bool handpose_case_save_file(const HandPoseCase *c, const char *path_abs) { return save_text(handpose_case_write(c), path_abs); }

typedef enum { LOAD_STYLE, LOAD_ITEM, LOAD_CASE, LOAD_LEGACY } LoadKind;

static void id_from_path(char *id, int n, const char *path, const char *prefix) {
    const char *base = strrchr(path, '/');
    const char *b2 = strrchr(path, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : path;
    snprintf(id, (size_t)n, "%s%s", prefix, base);
    char *dot = strrchr(id, '.');
    if (dot) *dot = 0;
}

static bool load_text(HandPoseDB *db, const char *text, const char *path, LoadKind kind, char *err, int err_size) {
    cJSON *root = cJSON_Parse(text);
    if (!root) { snprintf(err, (size_t)err_size, "%s: JSON parse error", path); return false; }
    bool ok = false;
    char id[64];
    id_from_path(id, sizeof(id), path, kind == LOAD_STYLE ? "style/" : "");
    const char *jid = jstring(root, "id", NULL);
    if (jid && strcmp(jid, id)) log_warn("handpose: %s: id '%s' differs from the file name, using '%s'", path, jid, id);
    const char *cat = jstring(root, "category", "");
    bool legacy_case = kind == LOAD_LEGACY && (!strcmp(cat, "vehicle") || !strcmp(cat, "interactable"));
    if (kind == LOAD_CASE || legacy_case) {
        HandPoseCase tmp;
        if (legacy_case) ok = handpose_case_from_v1(&tmp, root, id, NULL);
        else {
            memset(&tmp, 0, sizeof(tmp));
            dg_strcpy(tmp.id, sizeof(tmp.id), id);
            ok = handpose_case_parse(&tmp, root, err, err_size);
        }
        if (ok) {
            int i = handpose_case_add(db, tmp.id, tmp.kind);
            ok = i >= 0;
            if (ok) {
                tmp.hash = dg_hash_str(tmp.id);
                tmp.parent = -1;
                tmp.on_disk = !legacy_case;
                db->c[i] = tmp;
            }
        }
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "id");
        int i = handpose_add(db, id, kind == LOAD_STYLE ? HPC_STYLE : HPC_ITEM);
        ok = i >= 0 && handpose_entry_parse(&db->e[i], root, err, err_size, NULL);
        if (ok) {
            if (kind == LOAD_STYLE) db->e[i].category = HPC_STYLE;
            db->e[i].on_disk = kind != LOAD_LEGACY;
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool handpose_load_file(HandPoseDB *db, const char *path_abs, char *err, int err_size) {
    char *text = platform_read_file(path_abs, NULL);
    if (!text) { snprintf(err, (size_t)err_size, "%s: cannot read", path_abs); return false; }
    LoadKind k = LOAD_LEGACY;
    if (strstr(path_abs, "/styles/") || strstr(path_abs, "\\styles\\")) k = LOAD_STYLE;
    else if (strstr(path_abs, "/cases/") || strstr(path_abs, "\\cases\\")) k = LOAD_CASE;
    else if (strstr(path_abs, "/items/") || strstr(path_abs, "\\items\\")) k = LOAD_ITEM;
    bool ok = load_text(db, text, path_abs, k, err, err_size);
    free(text);
    return ok;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int load_glob(HandPoseDB *db, const char *dir_rel, LoadKind kind) {
    char dir[512];
    platform_data_path(dir_rel, dir, sizeof(dir));
    int count = 0, read = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    if (!files) return 0;
    char **names = (char **)malloc(sizeof(char *) * (size_t)(count + 1));
    for (int i = 0; i < count; i++) names[i] = files[i];
    qsort(names, (size_t)count, sizeof(char *), cmp_str);
    for (int i = 0; i < count; i++) {
        char path[768], err[256] = "";
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        char *text = platform_read_file(path, NULL);
        if (!text) continue;
        if (load_text(db, text, names[i], kind, err, sizeof(err))) read++;
        else log_error("handpose: %s/%s: %s", dir_rel, names[i], err);
        free(text);
    }
    free(names);
    SDL_free(files);
    return read;
}

int handpose_db_load_dir(HandPoseDB *db, const char *dir_rel) {
    char sub[300], abs[600];
    int n = 0;
    snprintf(sub, sizeof(sub), "%s/items", dir_rel);
    platform_data_path(sub, abs, sizeof(abs));
    SDL_PathInfo info;
    bool migrated = SDL_GetPathInfo(abs, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
    // Schema 1 files directly in the directory: only before the migration (tools/handpose_migrate);
    // after it a stale runtime copy of them is ignored.
    if (!migrated) {
        int legacy = load_glob(db, dir_rel, LOAD_LEGACY);
        if (legacy) log_warn("handpose: %d schema 1 files in %s migrated in memory (run tools/handpose_migrate)", legacy, dir_rel);
        n += legacy;
    }
    snprintf(sub, sizeof(sub), "%s/styles", dir_rel);
    n += load_glob(db, sub, LOAD_STYLE);
    snprintf(sub, sizeof(sub), "%s/items", dir_rel);
    n += load_glob(db, sub, LOAD_ITEM);
    snprintf(sub, sizeof(sub), "%s/cases", dir_rel);
    n += load_glob(db, sub, LOAD_CASE);
    handpose_link(db);
    handpose_rehash(db);
    return n;
}

// ------------------------------------------------------------------ grip styles

// First-person interaction grips (GripType order): their defaults live in styles/grip_<name>.json.
static const char *k_grip_names[] = { "relaxed", "fist", "point", "grab", "pistol", "rifle", "melee", "palm" };
int handpose_grip_count(void) { return (int)ARRAY_COUNT(k_grip_names); }
const char *handpose_grip_name(int g) { return (g >= 0 && g < handpose_grip_count()) ? k_grip_names[g] : "relaxed"; }
u32 handpose_grip_style_hash(int g) {
    char n[48];
    snprintf(n, sizeof(n), "grip_%s", handpose_grip_name(g));
    return handpose_style_hash(n);
}
