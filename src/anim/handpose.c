// Hand pose data: resolution, limits, presets, mirror, finger application, contact solver and
// the JSON format (docs/HANDPOSE.md).
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

static const char *k_state_names[HPS_COUNT] = { "default", "hip", "ads", "sprint", "crouch", "prone", "empty", "inspect", "vehicle_onehand",
                                                "phone_portrait", "phone_landscape", "use", "hold", "drive", "drive_onehand" };
static const char *k_cat_names[HPC_COUNT] = { "style", "weapon", "item", "attachment", "vehicle", "interactable" };
static const char *k_finger_names[HAND_FINGERS] = { "thumb", "index", "middle", "ring", "pinky" };
static const char *k_hand_names[2] = { "left", "right" };
// Share of a finger's curl per joint (proximal, middle, distal) and the thumb's scale: the split
// the old single-angle curl used, kept so generated defaults reproduce it.
static const float k_split[3] = { 0.30f, 0.38f, 0.32f };
#define THUMB_SCALE 0.6f

const char *handpose_state_name(int s) { return (s >= 0 && s < HPS_COUNT) ? k_state_names[s] : "?"; }
int handpose_state_from_name(const char *s) {
    for (int i = 0; s && i < HPS_COUNT; i++) if (!strcmp(s, k_state_names[i])) return i;
    return -1;
}
const char *handpose_category_name(int c) { return (c >= 0 && c < HPC_COUNT) ? k_cat_names[c] : "?"; }
const char *handpose_finger_name(int f) { return (f >= 0 && f < HAND_FINGERS) ? k_finger_names[f] : "?"; }
static int category_from_name(const char *s) {
    for (int i = 0; s && i < HPC_COUNT; i++) if (!strcmp(s, k_cat_names[i])) return i;
    return HPC_ITEM;
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
void handpose_db_free(HandPoseDB *db) { free(db->e); memset(db, 0, sizeof(*db)); }
void handpose_db_clear(HandPoseDB *db) { db->count = 0; db->hash = 0; db->revision++; }

void handpose_layer_clear(HandPoseLayer *l) {
    memset(l, 0, sizeof(*l));
    l->socket = xf_identity();
    l->wrist = xf_identity();
    l->ik_weight = 1.0f;
}
void handpose_entry_clear_layers(HandPoseEntry *e) {
    for (int h = 0; h < 2; h++)
        for (int s = 0; s < HPS_COUNT; s++) handpose_layer_clear(&e->layer[h][s]);
}

int handpose_find_hash(const HandPoseDB *db, u32 hash) {
    if (!hash) return -1;
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
}

bool handpose_entry_has_overrides(const HandPoseEntry *e) {
    for (int h = 0; h < 2; h++)
        for (int s = 0; s < HPS_COUNT; s++) if (e->layer[h][s].set) return true;
    return false;
}

static u32 hash_f(u32 h, float f) { return anim_hash(&f, sizeof(f), h); }
static u32 hash_xf(u32 h, Xform x) {
    float v[7] = { x.t.x, x.t.y, x.t.z, x.r.x, x.r.y, x.r.z, x.r.w };
    return anim_hash(v, sizeof(v), h);
}
static int cmp_u32(const void *a, const void *b) {
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

// Net identity over the entries that change a pose, in hash order (independent of file order;
// stubs and editor-only flags such as `reviewed` don't count).
void handpose_rehash(HandPoseDB *db) {
    u32 *order = (u32 *)malloc(sizeof(u32) * 2 * (size_t)(db->count + 1));
    int n = 0;
    for (int i = 0; i < db->count; i++)
        if (handpose_entry_has_overrides(&db->e[i])) { order[n * 2] = db->e[i].hash; order[n * 2 + 1] = (u32)i; n++; }
    qsort(order, (size_t)n, sizeof(u32) * 2, cmp_u32);
    u32 h = anim_hash_str("handposes", ANIM_HASH_SEED) ^ HANDPOSE_SCHEMA;
    for (int k = 0; k < n; k++) {
        const HandPoseEntry *e = &db->e[order[k * 2 + 1]];
        h = anim_hash_str(e->id, h);
        h = anim_hash_str(e->inherits, h);
        h = anim_hash(&e->relative, 1, h);
        for (int hd = 0; hd < 2; hd++)
            for (int s = 0; s < HPS_COUNT; s++) {
                const HandPoseLayer *l = &e->layer[hd][s];
                if (!l->set) continue;
                u32 tag = (u32)(hd * 64 + s) | ((u32)l->set << 8);
                h = anim_hash(&tag, sizeof(tag), h);
                if (l->set & HPL_SOCKET) h = hash_xf(h, l->socket);
                if (l->set & HPL_WRIST) h = hash_xf(h, l->wrist);
                if (l->set & HPL_POLE) { h = hash_f(h, l->pole.x); h = hash_f(h, l->pole.y); h = hash_f(h, l->pole.z); }
                if (l->set & HPL_IKW) h = hash_f(h, l->ik_weight);
                for (int f = 0; f < HAND_FINGERS; f++)
                    if (l->set & HPL_FINGER(f)) h = anim_hash(l->j[f], sizeof(l->j[f]), h);
            }
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

void handpose_resolve(const HandPoseDB *db, int entry, int hand, int state, HandPose *out) { handpose_resolve_over(db, -1, entry, hand, state, out); }

static int chain_of(const HandPoseDB *db, int entry, int *chain, int max) {
    int n = 0;
    for (int k = entry; k >= 0 && k < db->count && n < max; k = db->e[k].parent) chain[n++] = k;
    return n;
}

void handpose_resolve_over(const HandPoseDB *db, int base, int entry, int hand, int state, HandPose *out) {
    memset(out, 0, sizeof(*out));
    out->socket = xf_identity();
    out->wrist = xf_identity();
    out->ik_weight = 1.0f;
    if (!db || hand < 0 || hand > 1) return;
    if (state < 0 || state >= HPS_COUNT) state = HPS_DEFAULT;
    if (entry >= db->count) entry = -1;
    if (base >= db->count) base = -1;
    // chain[] leaf first: the entry's chain, then the base's (unless the entry already inherits it).
    int chain[16], n = chain_of(db, entry, chain, 8);
    bool has_base = base < 0;
    for (int i = 0; i < n && !has_base; i++) has_base = chain[i] == base;
    if (!has_base) n += chain_of(db, base, chain + n, 8);
    for (int i = n - 1; i >= 0; i--) {
        const HandPoseEntry *e = &db->e[chain[i]];
        apply_layer(out, &e->layer[hand][HPS_DEFAULT]);
        if (state != HPS_DEFAULT) apply_layer(out, &e->layer[hand][state]);
    }
    handpose_clamp_fingers(out->j);
    out->ik_weight = saturate(out->ik_weight);
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
    int to = from_hand ? 0 : 1;
    for (int s = 0; s < HPS_COUNT; s++) handpose_mirror_layer(&e->layer[from_hand][s], &e->layer[to][s]);
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

static int finger_joint_bone(const Skeleton *s, int side, int f, int joint) {
    int b = s->role[k_first[side][f]];
    for (int k = 0; k < joint && b >= 0; k++) b = first_child(s, b);
    return b;
}

vec3 handpose_fingertip(const Skeleton *s, const Xform *model, int side, int finger) {
    int d = finger_joint_bone(s, side, finger, 2);
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
        int b = finger_joint_bone(c->s, c->side, c->finger, k);
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

static void parse_layer(HandPoseLayer *l, const cJSON *o) {
    handpose_layer_clear(l);
    if (!cJSON_IsObject(o)) return;
    if (parse_xform(cJSON_GetObjectItemCaseSensitive(o, "socket"), &l->socket)) l->set |= HPL_SOCKET;
    if (parse_xform(cJSON_GetObjectItemCaseSensitive(o, "wrist"), &l->wrist)) l->set |= HPL_WRIST;
    float v[5];
    if (jvec(o, "pole", v, 3)) { l->pole = v3(v[0], v[1], v[2]); l->set |= HPL_POLE; }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(o, "ik_weight");
    if (cJSON_IsNumber(w)) { l->ik_weight = saturate((float)w->valuedouble); l->set |= HPL_IKW; }
    // Legacy shorthand: 5 curl angles (the old tables).
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
            int f = -1;
            const char *fn = jstring(c, "finger", "");
            for (int q = 0; q < HAND_FINGERS; q++) if (!strcmp(fn, k_finger_names[q])) f = q;
            if (f < 0 || !jvec(c, "point", v, 3)) continue;
            l->contacts[l->contact_count++] = (HandContact){ (u8)f, v3(v[0], v[1], v[2]) };
        }
    }
}

int handpose_migrate(cJSON *root) {
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema");
    int from = cJSON_IsNumber(sv) ? sv->valueint : 0;
    if (from < 1) {
        // Schema 0 (hand-written, like a controller.json hold style): top-level "curl_r" / "curl_l"
        // (5 angles) -> right / left "default" layers with the "curl" shorthand.
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
        cJSON_DeleteItemFromObjectCaseSensitive(root, "schema");
        cJSON_AddNumberToObject(root, "schema", 1);
    }
    return from;
}

bool handpose_entry_parse(HandPoseEntry *e, const cJSON *root_in, char *err, int err_size) {
    if (!cJSON_IsObject(root_in)) { snprintf(err, (size_t)err_size, "not a JSON object"); return false; }
    cJSON *root = cJSON_Duplicate(root_in, true);
    int from = handpose_migrate(root);
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (cJSON_IsNumber(sv) && sv->valueint > HANDPOSE_SCHEMA) {
        snprintf(err, (size_t)err_size, "schema %d is newer than this build (%d)", sv->valueint, HANDPOSE_SCHEMA);
        cJSON_Delete(root);
        return false;
    }
    if (from < HANDPOSE_SCHEMA) log_info("handpose: %s migrated from schema %d", jstring(root, "id", "?"), from);
    const char *id = jstring(root, "id", e->id);
    if (id && id != e->id) dg_strcpy(e->id, sizeof(e->id), id);
    e->hash = dg_hash_str(e->id);
    dg_strcpy(e->inherits, sizeof(e->inherits), jstring(root, "inherits", ""));
    dg_strcpy(e->source, sizeof(e->source), jstring(root, "source", e->source));
    dg_strcpy(e->label, sizeof(e->label), jstring(root, "label", e->label[0] ? e->label : e->id));
    e->category = (u8)category_from_name(jstring(root, "category", handpose_category_name(e->category)));
    e->relative = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "relative"));
    e->reviewed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reviewed"));
    e->auto_placed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "auto_placed"));
    const cJSON *a, *it;
    if ((a = cJSON_GetObjectItemCaseSensitive(root, "hands")) && cJSON_IsArray(a)) {
        e->hands = 0;
        cJSON_ArrayForEach(it, a) if (cJSON_IsString(it)) e->hands |= !strcmp(it->valuestring, "left") ? 1 : !strcmp(it->valuestring, "right") ? 2 : 0;
    }
    if ((a = cJSON_GetObjectItemCaseSensitive(root, "states")) && cJSON_IsArray(a)) {
        e->states = 0;
        cJSON_ArrayForEach(it, a) {
            int s = cJSON_IsString(it) ? handpose_state_from_name(it->valuestring) : -1;
            if (s >= 0) e->states |= 1u << s;
        }
    }
    handpose_entry_clear_layers(e);
    for (int h = 0; h < 2; h++) {
        const cJSON *hand = cJSON_GetObjectItemCaseSensitive(root, k_hand_names[h]), *st;
        cJSON_ArrayForEach(st, hand) {
            int s = handpose_state_from_name(st->string);
            if (s < 0) { log_warn("handpose: %s: unknown state '%s'", e->id, st->string ? st->string : "?"); continue; }
            parse_layer(&e->layer[h][s], st);
        }
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
    if (l->set & HPL_SOCKET) { FIELD("socket"); sb_xform(b, l->socket); }
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
    sb_printf(b, "%s}", first ? "" : "\n    ");
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
    sb_put(&b, ",\n  \"hands\": [");
    bool f = true;
    for (int h = 0; h < 2; h++) if (e->hands & (1 << h)) { sb_printf(&b, "%s\"%s\"", f ? "" : ", ", k_hand_names[h]); f = false; }
    sb_put(&b, "],\n  \"states\": [");
    f = true;
    for (int s = 0; s < HPS_COUNT; s++) if (e->states & (1u << s)) { sb_printf(&b, "%s\"%s\"", f ? "" : ", ", k_state_names[s]); f = false; }
    sb_printf(&b, "],\n  \"relative\": %s,\n  \"reviewed\": %s,\n  \"auto_placed\": %s", e->relative ? "true" : "false", e->reviewed ? "true" : "false",
              e->auto_placed ? "true" : "false");
    for (int h = 0; h < 2; h++) {
        sb_printf(&b, ",\n  \"%s\": {", k_hand_names[h]);
        bool fs = true;
        for (int s = 0; s < HPS_COUNT; s++) {
            if (!e->layer[h][s].set) continue;
            sb_printf(&b, "%s\n    \"%s\": ", fs ? "" : ",", k_state_names[s]);
            fs = false;
            write_layer(&b, &e->layer[h][s], "    ");
        }
        sb_put(&b, fs ? "}" : "\n  }");
    }
    sb_put(&b, "\n}\n");
    return b.p;
}

// ------------------------------------------------------------------ files

void handpose_entry_rel_path(const HandPoseEntry *e, const char *dir_rel, char *out, int out_size) {
    if (!strncmp(e->id, "style/", 6)) snprintf(out, (size_t)out_size, "%s/styles/%s.json", dir_rel, e->id + 6);
    else snprintf(out, (size_t)out_size, "%s/%s.json", dir_rel, e->id);
}

bool handpose_entry_save_file(const HandPoseEntry *e, const char *path_abs) {
    char *txt = handpose_entry_write(e);
    if (!txt) return false;
    bool ok = platform_write_file(path_abs, txt, strlen(txt));
    free(txt);
    return ok;
}

static bool load_text(HandPoseDB *db, const char *text, const char *path, bool style, char *err, int err_size) {
    cJSON *root = cJSON_Parse(text);
    if (!root) { snprintf(err, (size_t)err_size, "%s: JSON parse error", path); return false; }
    // The id defaults to the file name (styles: "style/<name>").
    char id[64];
    const char *base = strrchr(path, '/');
    const char *b2 = strrchr(path, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : path;
    snprintf(id, sizeof(id), "%s%s", style ? "style/" : "", base);
    char *dot = strrchr(id, '.');
    if (dot) *dot = 0;
    const char *jid = jstring(root, "id", NULL);
    if (jid && strcmp(jid, id)) log_warn("handpose: %s: id '%s' differs from the file name, using '%s'", path, jid, id);
    cJSON_DeleteItemFromObjectCaseSensitive(root, "id");
    int i = handpose_add(db, id, style ? HPC_STYLE : HPC_ITEM);
    bool ok = i >= 0 && handpose_entry_parse(&db->e[i], root, err, err_size);
    if (ok) {
        if (style) db->e[i].category = HPC_STYLE;
        db->e[i].on_disk = true;
    }
    cJSON_Delete(root);
    return ok;
}

bool handpose_entry_load_file(HandPoseDB *db, const char *path_abs, char *err, int err_size) {
    char *text = platform_read_file(path_abs, NULL);
    if (!text) { snprintf(err, (size_t)err_size, "%s: cannot read", path_abs); return false; }
    bool style = strstr(path_abs, "/styles/") || strstr(path_abs, "\\styles\\");
    bool ok = load_text(db, text, path_abs, style, err, err_size);
    free(text);
    return ok;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int load_glob(HandPoseDB *db, const char *dir_rel, bool style) {
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
        if (load_text(db, text, names[i], style, err, sizeof(err))) read++;
        else log_error("handpose: %s/%s: %s", dir_rel, names[i], err);
        free(text);
    }
    free(names);
    SDL_free(files);
    return read;
}

int handpose_db_load_dir(HandPoseDB *db, const char *dir_rel) {
    char styles[300];
    snprintf(styles, sizeof(styles), "%s/styles", dir_rel);
    int n = load_glob(db, styles, true);
    n += load_glob(db, dir_rel, false);
    handpose_link(db);
    handpose_rehash(db);
    return n;
}

// ------------------------------------------------------------------ style defaults

// First-person interaction grips (GripType order): the old curl table of the body's
// interaction pass (thumb, index, middle, ring, pinky degrees).
static const char *k_grip_names[] = { "relaxed", "fist", "point", "grab", "pistol", "rifle", "melee", "palm" };
static const float k_grip_curl[][5] = {
    { 10, 15, 20, 25, 30 }, { 50, 95, 100, 100, 100 }, { 50, 0, 95, 95, 95 }, { 35, 50, 55, 60, 65 },
    { 40, 70, 80, 85, 90 }, { 40, 70, 80, 85, 90 }, { 50, 85, 90, 90, 90 }, { 0, 5, 5, 5, 5 },
};
int handpose_grip_count(void) { return (int)ARRAY_COUNT(k_grip_names); }
const char *handpose_grip_name(int g) { return (g >= 0 && g < handpose_grip_count()) ? k_grip_names[g] : "relaxed"; }
u32 handpose_grip_style_hash(int g) {
    char n[48];
    snprintf(n, sizeof(n), "grip_%s", handpose_grip_name(g));
    return handpose_style_hash(n);
}

static HandPoseEntry *style_default(HandPoseDB *db, const char *name, const char *label) {
    char id[64];
    snprintf(id, sizeof(id), "style/%s", name);
    int i = handpose_find(db, id);
    if (i >= 0 && db->e[i].on_disk) return NULL;   // the file wins
    if (i < 0) i = handpose_add(db, id, HPC_STYLE);
    if (i < 0) return NULL;
    HandPoseEntry *e = &db->e[i];
    handpose_entry_clear_layers(e);
    e->category = HPC_STYLE;
    e->inherits[0] = 0;
    e->reviewed = true;   // generated from the shipped tables
    dg_strcpy(e->label, sizeof(e->label), label);
    dg_strcpy(e->source, sizeof(e->source), "generated");
    return e;
}

void handpose_add_style_defaults(HandPoseDB *db, const ProcParams *p) {
    for (int s = 0; p && s < p->style_count; s++) {
        const HoldStyle *hs = &p->styles[s];
        char label[48];
        snprintf(label, sizeof(label), "Hold style: %s", hs->name);
        HandPoseEntry *e = style_default(db, hs->name, label);
        if (!e) continue;
        e->hands = (u8)(2 | (hs->left_hand ? 1 : 0));
        e->states = 1u << HPS_DEFAULT;
        for (int st = 0; st < HP_STATE_COUNT; st++) if (hs->hp.state[st].set) e->states |= 1u << HPS_FROM_HOLD(st);
        handpose_from_curl(hs->curl_r, e->layer[1][HPS_DEFAULT].j);
        handpose_from_curl(hs->curl_l, e->layer[0][HPS_DEFAULT].j);
        e->layer[1][HPS_DEFAULT].set = e->layer[0][HPS_DEFAULT].set = HPL_FINGERS_ALL;
    }
    for (int g = 0; g < handpose_grip_count(); g++) {
        char name[48], label[48];
        snprintf(name, sizeof(name), "grip_%s", k_grip_names[g]);
        snprintf(label, sizeof(label), "Grip: %s", k_grip_names[g]);
        HandPoseEntry *e = style_default(db, name, label);
        if (!e) continue;
        e->hands = 3;
        e->states = (1u << HPS_DEFAULT) | (1u << HPS_USE) | (1u << HPS_HOLD) | (1u << HPS_DRIVE) | (1u << HPS_DRIVE_ONEHAND);
        for (int h = 0; h < 2; h++) {
            handpose_from_curl(k_grip_curl[g], e->layer[h][HPS_DEFAULT].j);
            e->layer[h][HPS_DEFAULT].set = HPL_FINGERS_ALL;
        }
    }
    handpose_link(db);
    handpose_rehash(db);
}
