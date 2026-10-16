#include "gameplay/attachments.h"
#include "hands.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "render/mesh_gen.h"
#include "gameplay/weapons.h"
#include "gameplay/characters.h"
#include "gameplay/entities.h"
#include "cJSON.h"
#include <stdlib.h>
#include <math.h>

#define MAX_CLIPS 32
#define MAX_KEYS 8

typedef struct HandKey {
    float t;
    vec3 pos[2];
    vec3 euler[2];     // pitch, yaw, roll in degrees (camera space)
    u8 grip[2];
} HandKey;

typedef struct HandClip {
    char name[32];
    float duration;
    bool loop;
    HandKey keys[MAX_KEYS];
    int key_count;
} HandClip;

typedef struct HandPose { vec3 pos[2]; quat rot[2]; u8 grip[2]; } HandPose;

typedef enum InteractPhase { IA_NONE, IA_REACH, IA_HOLD, IA_RETURN } InteractPhase;

typedef struct HandsSystem {
    WeaponDef weff;                 // the equipped weapon with its attachments (H->weapon points here)
    const ItemDB *db;
    HandClip clips[MAX_CLIPS];
    int clip_count;
    char poses_path[256];

    // clip playback with crossfade
    int clip, prev_clip;
    float clip_t, prev_clip_t, fade;


    // world overrides
    float ov_w[2];
    vec3 ov_pos[2];
    quat ov_rot[2];
    u8 ov_grip[2];

    // interaction animation
    InteractPhase ia_phase;
    EntityId ia_target;
    float ia_t;
    HandMode ia_mode;
    vec3 ia_last_pos[2];
    quat ia_last_rot[2];

    // smoothed final hand transforms (world) + solved chains
    vec3 hand_pos[2];
    quat hand_rot[2];
    bool initialized;
    ArmChain chain[2];
    vec3 weapon_pos;
    quat weapon_rot;
    const WeaponDef *weapon;
    const ItemDef *held_item;
    vec3 cam_pos;
    quat cam_rot;

    // first-person targets for the body model
    HandsFpTargets fp;
    vec3 kick_pos, kick_rot;
    bool fp_valid, body_arms;

    MeshId cube;
    Cvar *debug, *upper_len, *fore_len;
} HandsSystem;

#define H (g_engine.hands)

// ------------------------------------------------------------------ pose data

static vec3 jv3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}

static u8 grip_from(const char *s) {
    static const char *names[GRIP_COUNT] = { "relaxed", "fist", "point", "grab", "pistol", "rifle", "melee", "palm" };
    for (int i = 0; i < GRIP_COUNT; i++) if (s && !strcmp(s, names[i])) return (u8)i;
    return GRIP_RELAXED;
}

bool hand_poses_load(const char *path) {
    char buf[512];
    const char *full = platform_data_path(path, buf, sizeof(buf));
    char *text = platform_read_file(full, NULL);
    if (!text) { log_error("hands: cannot read %s", path); return false; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { log_error("hands: JSON error in %s", path); return false; }
    dg_strcpy(H->poses_path, sizeof(H->poses_path), path);
    H->clip_count = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "clips")) {
        if (H->clip_count >= MAX_CLIPS) break;
        HandClip *clip = &H->clips[H->clip_count++];
        memset(clip, 0, sizeof(*clip));
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(c, "name");
        dg_strcpy(clip->name, sizeof(clip->name), cJSON_IsString(n) ? n->valuestring : "clip");
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(c, "duration");
        clip->duration = cJSON_IsNumber(d) ? (float)d->valuedouble : 1.0f;
        clip->loop = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "loop"));
        const cJSON *k;
        cJSON_ArrayForEach(k, cJSON_GetObjectItemCaseSensitive(c, "keys")) {
            if (clip->key_count >= MAX_KEYS) break;
            HandKey *key = &clip->keys[clip->key_count++];
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(k, "t");
            key->t = cJSON_IsNumber(t) ? (float)t->valuedouble : 0;
            const char *sides[2] = { "left", "right" };
            for (int h = 0; h < 2; h++) {
                const cJSON *hd = cJSON_GetObjectItemCaseSensitive(k, sides[h]);
                key->pos[h] = jv3(hd, "pos", v3(h ? 0.2f : -0.2f, -0.25f, -0.38f));
                key->euler[h] = jv3(hd, "rot", v3(0, 0, 0));
                const cJSON *g = cJSON_GetObjectItemCaseSensitive(hd, "grip");
                key->grip[h] = grip_from(cJSON_IsString(g) ? g->valuestring : NULL);
            }
        }
    }
    cJSON_Delete(root);
    log_info("hands: %d pose clips loaded", H->clip_count);
    return H->clip_count > 0;
}

void hands_reload_poses(void) {
    if (H && H->poses_path[0]) hand_poses_load(H->poses_path);
}

static void cmd_reload_poses(int argc, const char **argv) { hands_reload_poses(); }

static int find_clip(const char *name) {
    for (int i = 0; i < H->clip_count; i++) if (!strcmp(H->clips[i].name, name)) return i;
    return -1;
}

static quat euler_deg(vec3 e) { return quat_euler(e.y * DG_DEG2RAD, e.x * DG_DEG2RAD, e.z * DG_DEG2RAD); }

// Samples a clip at normalized time u (0..1) into a camera-space pose.
static HandPose sample_clip(int ci, float u) {
    HandPose p;
    memset(&p, 0, sizeof(p));
    if (ci < 0 || ci >= H->clip_count || H->clips[ci].key_count == 0) {
        p.pos[0] = v3(-0.2f, -0.28f, -0.38f);
        p.pos[1] = v3(0.2f, -0.28f, -0.38f);
        p.rot[0] = p.rot[1] = quat_identity();
        return p;
    }
    const HandClip *c = &H->clips[ci];
    int k1 = 0;
    while (k1 < c->key_count && c->keys[k1].t < u) k1++;
    int k0 = k1 > 0 ? k1 - 1 : 0;
    if (k1 >= c->key_count) k1 = c->key_count - 1;
    const HandKey *a = &c->keys[k0], *b = &c->keys[k1];
    float span = b->t - a->t;
    float f = span > 1e-4f ? saturate((u - a->t) / span) : 0.0f;
    f = smoothstepf(0, 1, f);
    for (int h = 0; h < 2; h++) {
        p.pos[h] = v3_lerp(a->pos[h], b->pos[h], f);
        p.rot[h] = quat_nlerp(euler_deg(a->euler[h]), euler_deg(b->euler[h]), f);
        p.grip[h] = f < 0.5f ? a->grip[h] : b->grip[h];
    }
    return p;
}

static HandPose pose_lerp(const HandPose *a, const HandPose *b, float t) {
    HandPose p;
    for (int h = 0; h < 2; h++) {
        p.pos[h] = v3_lerp(a->pos[h], b->pos[h], t);
        p.rot[h] = quat_nlerp(a->rot[h], b->rot[h], t);
        p.grip[h] = t < 0.5f ? a->grip[h] : b->grip[h];
    }
    return p;
}

// ------------------------------------------------------------------ lifecycle

bool hands_init(void) {
    H = (HandsSystem *)calloc(1, sizeof(HandsSystem));
    H->debug = cvar_reg_bool("hands_debug", false, 0, "draw hand IK chains, targets and pole vectors");
    H->upper_len = cvar_reg_float("hands_upper_arm", 0.30f, 0.1f, 1.0f, 0, "upper arm length (m)");
    H->fore_len = cvar_reg_float("hands_forearm", 0.29f, 0.1f, 1.0f, 0, "forearm length (m)");
    cmd_register("hands_reload", cmd_reload_poses, "reload data/hand_poses.json");
    H->clip = H->prev_clip = -1;
    MeshBuilder mb;
    mb_init(&mb);
    mb_box(&mb, v3(0, 0, 0), v3s(0.5f), 0xFFFFFFFFu);
    H->cube = mb_upload(&mb);
    mb_free(&mb);
    return hand_poses_load("data/hand_poses.json");
}

void hands_shutdown(void) {
    if (!H) return;
    free(H);
    H = NULL;
}

const ArmChain *hands_chain(int hand) { return &H->chain[hand & 1]; }

// ------------------------------------------------------------------ IK

// Analytic two-bone IK: shared with the third-person characters (src/anim/anim.c).
static bool solve_two_bone(vec3 shoulder, vec3 target, vec3 pole, float a, float b, vec3 *elbow, vec3 *wrist) {
    return anim_ik_two_bone(shoulder, target, pole, a, b, elbow, wrist);
}

static void solve_arm(int h, vec3 cam_pos, quat cam_rot, vec3 hand_pos, quat hand_rot, u8 grip, ArmChain *out) {
    float side = h == HAND_LEFT ? -1.0f : 1.0f;
    vec3 shoulder = v3_add(cam_pos, quat_rotate(cam_rot, v3(0.19f * side, -0.24f, 0.10f)));
    // Wrist sits behind the palm center along the hand's -Z (fingers point -Z).
    vec3 wrist_target = v3_sub(hand_pos, quat_rotate(hand_rot, v3(0, 0, -0.055f)));
    // Elbows point down and out.
    vec3 pole = quat_rotate(cam_rot, v3(0.8f * side, -1.0f, 0.25f));
    vec3 elbow, wrist;
    bool reached = solve_two_bone(shoulder, wrist_target, pole, H->upper_len->f, H->fore_len->f, &elbow, &wrist);
    out->shoulder = shoulder;
    out->elbow = elbow;
    out->wrist = wrist;
    out->reach_limited = !reached;
    // If the target was out of reach, the hand stays attached to the wrist end.
    out->hand_pos = reached ? hand_pos : v3_add(wrist, quat_rotate(hand_rot, v3(0, 0, -0.055f)));
    out->hand_rot = hand_rot;
    out->grip = grip;
}

// ------------------------------------------------------------------ pose selection

static const char *base_clip_name(const World *w, int slot, bool ads, float *u_out, bool *timed) {
    const Player *p = &w->players[slot];
    const PlayerSim *sim = &p->sim;
    const WeaponState *ws = &sim->wpn;
    const InvSlot *s = weapon_equipped(w, slot);
    const ItemDef *item = s ? item_def(w->items, s->item) : NULL;
    const WeaponDef *wd = s ? item_weapon(w->items, s->item) : NULL;
    *timed = false;
    *u_out = 0;
    if (sim->move.mode == MOVE_HANG) return "hang";
    if (sim->move.mode == MOVE_MANTLE || sim->move.mode == MOVE_VAULT) {
        *timed = true;
        *u_out = saturate(sim->move.m_t);
        return sim->move.mode == MOVE_VAULT ? "vault" : "mantle";
    }
    if (wd) {
        if (wd->kind == WPN_MELEE) {
            if (ws->swing_t >= 0) {
                *timed = true;
                *u_out = saturate(ws->swing_t / fmaxf(0.01f, wd->swing_time));
                return "melee_swing";
            }
            return sim->move.sprinting ? "melee_sprint" : "melee";
        }
        const char *pose = wd->pose;
        if (ws->reloading) {
            *timed = true;
            *u_out = saturate(1.0f - ws->reload_t / fmaxf(0.01f, wd->reload_time));
            return !strcmp(pose, "rifle") ? "reload_rifle" : "reload_pistol";
        }
        if (sim->move.sprinting) return !strcmp(pose, "rifle") ? "rifle_sprint" : "pistol_sprint";
        if (ads) return !strcmp(pose, "rifle") ? "rifle_ads" : "pistol_ads";
        return !strcmp(pose, "rifle") ? "rifle" : "pistol";
    }
    if (item && item->kind == ITEM_CONSUMABLE) {
        if (ws->use_t >= 0) {
            *timed = true;
            *u_out = saturate(ws->use_t / fmaxf(0.01f, item->use_time));
            return "consumable_use";
        }
        return "consumable";
    }
    if (sim->move.sprinting) return "sprint";
    return "idle";
}

static vec3 cam_to_world(vec3 cam_pos, quat cam_rot, vec3 local) { return v3_add(cam_pos, quat_rotate(cam_rot, local)); }

// ------------------------------------------------------------------ update

void hands_update(const HandsInput *in) {
    World *w = in->world;
    const Player *p = &w->players[in->player];
    const PlayerSim *sim = &p->sim;
    const PlayerMove *m = &sim->move;
    float dt = fminf(in->dt, 0.05f);
    quat cam_rot = quat_euler(in->yaw, in->pitch, in->roll);
    H->cam_pos = in->eye;
    H->cam_rot = cam_rot;

    // ---- 1. base clip with crossfade
    float u = 0;
    bool timed = false;
    const char *name = base_clip_name(w, in->player, in->ads, &u, &timed);
    int ci = find_clip(name);
    if (ci < 0) ci = find_clip("idle");
    if (ci != H->clip) {
        H->prev_clip = H->clip;
        H->prev_clip_t = H->clip_t;
        H->clip = ci;
        H->clip_t = 0;
        H->fade = (H->prev_clip >= 0) ? 0.0f : 1.0f;
    }
    float dur = ci >= 0 ? fmaxf(0.01f, H->clips[ci].duration) : 1.0f;
    if (timed) H->clip_t = u * dur;
    else {
        H->clip_t += dt;
        if (ci >= 0 && H->clips[ci].loop) H->clip_t = fmodf(H->clip_t, dur);
        else H->clip_t = fminf(H->clip_t, dur);
    }
    H->fade = fminf(1.0f, H->fade + dt / 0.15f);
    HandPose pose = sample_clip(H->clip, H->clip_t / dur);
    if (H->fade < 1.0f && H->prev_clip >= 0) {
        float pd = fmaxf(0.01f, H->clips[H->prev_clip].duration);
        HandPose prev = sample_clip(H->prev_clip, fminf(1.0f, H->prev_clip_t / pd));
        pose = pose_lerp(&prev, &pose, smoothstepf(0, 1, H->fade));
    }

    // ---- 2. procedural layer: none here. Held items live on the hold point
    //         (src/player/holdpoint.c: sway, look lag, bob, kick, ADS from the sight socket); the
    //         body's arms are solved onto it. This rig only supplies interaction / ledge targets
    //         and the empty-hands fallback when the body is not drawn (cl_fp_body 0).
    const PlayerMove *mm = m;
    (void)mm;
    {
        const WeaponCvars *wc = weapons_cvars();
        float kk = wc->kick_scale ? wc->kick_scale->f : 0.0f;   // wpn_kick_scale: visual item kick (default 0)
        H->kick_pos = v3_scale(in->vm_kick_pos, kk);
        H->kick_rot = v3_scale(in->vm_kick_rot, kk);
    }

    // To world space.
    vec3 target_pos[2];
    quat target_rot[2];
    u8 grip[2];
    for (int h = 0; h < 2; h++) {
        target_pos[h] = cam_to_world(in->eye, cam_rot, pose.pos[h]);
        target_rot[h] = quat_mul(cam_rot, pose.rot[h]);
        grip[h] = pose.grip[h];
    }

    // Weapon attaches to the right hand's grip frame; the left hand supports it.
    const InvSlot *slot = weapon_equipped(w, in->player);
    if (g_vehicle_pose.holstered && g_vehicle_pose.holstered(w, in->player)) slot = NULL;   // driving: both hands on the wheel
    H->weapon = slot ? item_weapon(w->items, slot->item) : NULL;
    H->db = w->items;
    if (H->weapon) {
        attach_weapon_effective(w->items, H->weapon, slot->att, &H->weff);
        H->weapon = &H->weff;
    }
    H->held_item = slot ? item_def(w->items, slot->item) : NULL;
    bool weapon_two_handed = false;
    if (H->weapon && H->weapon->kind != WPN_MELEE && !sim->wpn.reloading) {
        vec3 support = v3_add(target_pos[HAND_RIGHT], quat_rotate(target_rot[HAND_RIGHT], H->weapon->support_offset));
        target_pos[HAND_LEFT] = support;
        target_rot[HAND_LEFT] = quat_mul(target_rot[HAND_RIGHT], quat_axis_angle(v3(0, 0, 1), -0.9f));
        grip[HAND_LEFT] = GRIP_GRAB;
        weapon_two_handed = true;
    }

    // ---- 3. world-space overrides
    float ov_target[2] = { 0, 0 };
    // Ledge anchors (hang / mantle / vault).
    if (m->hand_anchor_valid && (m->mode == MOVE_HANG || m->mode == MOVE_MANTLE || m->mode == MOVE_VAULT)) {
        for (int h = 0; h < 2; h++) {
            if (m->mode == MOVE_VAULT && h == HAND_RIGHT) continue;
            ov_target[h] = 1.0f;
            H->ov_pos[h] = m->hand_anchor[h];
            // Palm down on the ledge, fingers pointing over the edge.
            H->ov_rot[h] = quat_mul(quat_yaw(in->yaw), quat_axis_angle(v3(1, 0, 0), -0.25f));
            H->ov_grip[h] = GRIP_PALM;
        }
    }

    // Seated: hands on the steering wheel / handlebar (a free hand holds the one-handed gun).
    if (m->mode == MOVE_SEATED && g_vehicle_pose.seat_hands) {
        vec3 sp[2];
        quat sr[2];
        bool use[2];
        if (g_vehicle_pose.seat_hands(w, in->player, sp, sr, use))
            for (int h = 0; h < 2; h++) {
                if (!use[h]) continue;
                ov_target[h] = 1.0f;
                H->ov_pos[h] = sp[h];
                H->ov_rot[h] = sr[h];
                H->ov_grip[h] = GRIP_GRAB;
                if (h == HAND_LEFT) weapon_two_handed = false;
            }
    }

    // Interaction reach (local prediction: starts on the use press).
    if (in->use_pressed && ent_valid(w, in->focus) && H->ia_phase == IA_NONE) {
        int fi = (int)ENT_INDEX(in->focus);
        const InteractDef *def = w->inter[fi].def;
        if (def && def->hand_target) {
            H->ia_phase = IA_REACH;
            H->ia_target = in->focus;
            H->ia_t = 0;
            H->ia_mode = def->hands;
        }
    }
    // The target vanished inside this frame's tick (a pickup): reach for where it was.
    if (in->use_pressed && H->ia_phase == IA_NONE && in->use_target_valid) {
        H->ia_phase = IA_REACH;
        H->ia_target = ENT_NONE;
        H->ia_t = 0;
        H->ia_mode = (HandMode)in->use_target_hands;
        quat palm_down = quat_mul(quat_yaw(in->yaw), quat_axis_angle(v3(1, 0, 0), -1.2f));   // as pickup_hand
        for (int h = 0; h < 2; h++) { H->ia_last_pos[h] = in->use_target; H->ia_last_rot[h] = palm_down; }
    }
    if (H->ia_phase != IA_NONE) {
        H->ia_t += dt;
        bool valid = ent_valid(w, H->ia_target) && w->inter[ENT_INDEX(H->ia_target)].def;
        const InteractDef *def = valid ? w->inter[ENT_INDEX(H->ia_target)].def : NULL;
        float reach_time = 0.16f;
        float hold_time = def ? (def->hold_time > 0 ? def->hold_time + 0.25f : 0.38f) : (H->ia_target == ENT_NONE ? 0.12f : 0.0f);
        if (H->ia_phase == IA_REACH && H->ia_t >= reach_time) { H->ia_phase = IA_HOLD; H->ia_t = 0; }
        if (H->ia_phase == IA_HOLD) {
            bool hold_interaction = def && def->hold_time > 0;
            bool released = hold_interaction && !in->use_held && H->ia_t < def->hold_time;
            bool gone = !valid && H->ia_target != ENT_NONE;
            if (gone || released || H->ia_t >= hold_time) { H->ia_phase = IA_RETURN; H->ia_t = 0; }
        }
        if (H->ia_phase == IA_RETURN && H->ia_t >= 0.22f) H->ia_phase = IA_NONE;

        float wgt = 0;
        if (H->ia_phase == IA_REACH) wgt = smoothstepf(0, 1, H->ia_t / reach_time);
        else if (H->ia_phase == IA_HOLD) wgt = 1;
        else if (H->ia_phase == IA_RETURN) wgt = 1.0f - smoothstepf(0, 1, H->ia_t / 0.22f);
        // Holding something: the free (left) hand does the interaction / pickup, the item stays.
        bool holding = H->weapon || H->held_item;
        int free_hand = holding ? HAND_LEFT : HAND_RIGHT;
        for (int h = 0; h < 2; h++) {
            bool uses_hand = (H->ia_mode == HANDS_BOTH && !holding) || h == free_hand;
            if (!uses_hand) continue;
            vec3 tp;
            quat tr;
            if (valid && def->hand_target(w, H->ia_target, in->player, h, &tp, &tr)) {
                H->ia_last_pos[h] = tp;
                H->ia_last_rot[h] = tr;
            }
            // Entity may vanish (pickup taken): keep the last known point while returning.
            if (wgt > ov_target[h]) {
                ov_target[h] = wgt;
                H->ov_pos[h] = H->ia_last_pos[h];
                H->ov_rot[h] = H->ia_last_rot[h];
                H->ov_grip[h] = H->ia_mode == HANDS_POINT ? GRIP_POINT : GRIP_GRAB;
            }
            // A one-handed interaction releases the weapon support grip.
            if (h == free_hand && weapon_two_handed && wgt > 0) weapon_two_handed = false;
        }
    }
    for (int h = 0; h < 2; h++) {
        H->ov_w[h] = ov_target[h] > H->ov_w[h] ? approachf(H->ov_w[h], ov_target[h], dt * 12.0f) : approachf(H->ov_w[h], ov_target[h], dt * 6.0f);
        if (H->ov_w[h] > 0.001f) {
            float t = smoothstepf(0, 1, H->ov_w[h]);
            target_pos[h] = v3_lerp(target_pos[h], H->ov_pos[h], t);
            target_rot[h] = quat_nlerp(target_rot[h], H->ov_rot[h], t);
            if (t > 0.5f) grip[h] = H->ov_grip[h];
        }
    }

    // ---- 4. smoothing (fast critically damped follow) + IK
    for (int h = 0; h < 2; h++) {
        if (!H->initialized) { H->hand_pos[h] = target_pos[h]; H->hand_rot[h] = target_rot[h]; }
        // Follow tightly: the procedural/clip layers already provide motion; this only removes snaps.
        float k = exp_decay(H->ov_w[h] > 0.01f ? 30.0f : 40.0f, dt);
        // Stay rigid relative to the camera when not overridden so fast turns don't smear the hands.
        if (H->ov_w[h] < 0.01f) k = 1.0f;
        H->hand_pos[h] = v3_lerp(H->hand_pos[h], target_pos[h], k);
        H->hand_rot[h] = quat_nlerp(H->hand_rot[h], target_rot[h], k);
        solve_arm(h, in->eye, cam_rot, H->hand_pos[h], H->hand_rot[h], grip[h], &H->chain[h]);
    }
    H->initialized = true;
    H->weapon_pos = H->chain[HAND_RIGHT].hand_pos;
    H->weapon_rot = H->chain[HAND_RIGHT].hand_rot;

    // ---- 5. palm frames for the full-body model: weapon hands on the item sockets, everything
    //         else (idle, consumables, interactions, ledges) on the solved hand frames.
    {
        HandsFpTargets *t = &H->fp;
        memset(t, 0, sizeof(*t));
        t->weapon = H->weapon;
        t->item = H->held_item;
        t->cam_pos = in->eye;
        t->cam_rot = cam_rot;
        const ItemSockets *k = H->weapon ? &H->weapon->sockets : (H->held_item ? &H->held_item->sockets : NULL);
        Xform item = xf_make(H->weapon_pos, H->weapon_rot);
        bool melee = H->weapon && H->weapon->kind == WPN_MELEE;
        for (int h = 0; h < 2; h++) {
            Xform hand = xf_make(H->chain[h].hand_pos, H->chain[h].hand_rot);
            t->palm[h] = hand;
            t->ov_palm[h] = xf_make(H->ov_pos[h], H->ov_rot[h]);
            t->ov_w[h] = smoothstepf(0, 1, H->ov_w[h]);
            t->grip[h] = H->chain[h].grip;
            Xform sock;
            bool use = false;
            if (k && h == HAND_RIGHT && (H->weapon || H->held_item)) { sock = xf_mul(item, k->grip_r); use = true; }
            if (k && h == HAND_LEFT && k->has_left && H->weapon && !sim->wpn.reloading && (weapon_two_handed || melee)) { sock = xf_mul(item, k->grip_l); use = true; }
            if (use) {
                float ov = smoothstepf(0, 1, H->ov_w[h]);
                t->palm[h].t = v3_lerp(sock.t, hand.t, ov);
                t->palm[h].r = quat_nlerp(sock.r, hand.r, ov);
                t->on_weapon[h] = ov < 0.5f;
            }
        }
        H->fp_valid = true;
    }
    (void)weapon_two_handed;

    if (H->debug->i) {
        for (int h = 0; h < 2; h++) {
            const ArmChain *c = &H->chain[h];
            dbg_line(c->shoulder, c->elbow, DBG_GREEN, 0, false);
            dbg_line(c->elbow, c->wrist, DBG_CYAN, 0, false);
            dbg_cross(target_pos[h], 0.02f, c->reach_limited ? DBG_RED : DBG_YELLOW, 0);
            dbg_arrow(c->elbow, v3_add(c->elbow, v3_scale(v3_norm(quat_rotate(cam_rot, v3(h ? 0.8f : -0.8f, -1, 0.25f))), 0.08f)), DBG_MAGENTA, 0);
        }
    }
}

// ------------------------------------------------------------------ rendering

static u32 s_world_item_flags = MAT_NO_SHADOW;   // flags for third-person held items (hands_draw_held)

static void draw_box(vec3 center, quat rot, vec3 half, vec4 color, bool viewmodel) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.mesh = H->cube;
    d.model = mat4_trs(center, rot, v3_scale(half, 2.0f));
    d.mat = material_color(color);
    d.mat.roughness = 0.7f;
    d.mat.flags = viewmodel ? MAT_NO_SHADOW : s_world_item_flags;
    if (viewmodel) render_draw_viewmodel(&d);
    else render_draw(&d);
}

// Box spanning segment a->b with the given half thickness.
static void draw_segment(vec3 a, vec3 b, float th, vec4 color, vec3 up_hint, bool viewmodel) {
    vec3 d = v3_sub(b, a);
    float len = v3_len(d);
    if (len < 1e-4f) return;
    vec3 z = v3_scale(d, -1.0f / len);  // local -Z along the segment
    vec3 x = v3_norm(v3_cross(up_hint, z));
    if (v3_len2(x) < 1e-6f) x = v3(1, 0, 0);
    vec3 y = v3_cross(z, x);
    mat4 m = mat4_identity();
    m.m[0] = x.x * th * 2; m.m[1] = x.y * th * 2; m.m[2] = x.z * th * 2;
    m.m[4] = y.x * th * 2; m.m[5] = y.y * th * 2; m.m[6] = y.z * th * 2;
    m.m[8] = z.x * len; m.m[9] = z.y * len; m.m[10] = z.z * len;
    vec3 c = v3_scale(v3_add(a, b), 0.5f);
    m.m[12] = c.x; m.m[13] = c.y; m.m[14] = c.z;
    DrawItem di;
    memset(&di, 0, sizeof(di));
    di.mesh = H->cube;
    di.model = m;
    di.mat = material_color(color);
    di.mat.roughness = 0.85f;
    di.mat.flags = MAT_NO_SHADOW;
    if (viewmodel) render_draw_viewmodel(&di);
    else render_draw(&di);
}

// Hand cube shape per grip (the placeholder "hand"). Rotation is the hand frame (fingers -Z, back of hand +Y).
static void draw_hand(const ArmChain *c, int hand, bool viewmodel) {
    vec4 skin = v4(0.85f, 0.66f, 0.52f, 1);
    vec3 half;
    switch (c->grip) {
    case GRIP_FIST:
    case GRIP_PISTOL:
    case GRIP_RIFLE:
    case GRIP_MELEE: half = v3(0.042f, 0.035f, 0.045f); break;
    case GRIP_GRAB: half = v3(0.044f, 0.03f, 0.05f); break;
    case GRIP_PALM: half = v3(0.046f, 0.016f, 0.06f); break;
    case GRIP_POINT: half = v3(0.04f, 0.03f, 0.04f); break;
    default: half = v3(0.045f, 0.02f, 0.055f); break;
    }
    draw_box(c->hand_pos, c->hand_rot, half, skin, viewmodel);
    if (c->grip == GRIP_POINT) {
        // Extended index finger.
        vec3 tip = v3_add(c->hand_pos, quat_rotate(c->hand_rot, v3(hand == HAND_RIGHT ? -0.025f : 0.025f, 0.01f, -0.07f)));
        draw_box(tip, c->hand_rot, v3(0.009f, 0.009f, 0.035f), skin, viewmodel);
    }
}

static void draw_arm(const ArmChain *c, int hand, quat cam_rot, bool viewmodel) {
    vec4 sleeve = v4(0.22f, 0.26f, 0.2f, 1);
    vec4 cuff = v4(0.18f, 0.21f, 0.16f, 1);
    vec3 up = quat_rotate(cam_rot, v3(0, 1, 0));
    draw_segment(c->shoulder, c->elbow, 0.042f, sleeve, up, viewmodel);
    draw_segment(c->elbow, c->wrist, 0.035f, cuff, up, viewmodel);
    draw_hand(c, hand, viewmodel);
}

// wd: an effective weapon (attach_weapon_effective): its parts without the hidden ones, plus every
// applied attachment's parts placed on its slot.
static void draw_weapon(const WeaponDef *wd, vec3 pos, quat rot, bool viewmodel) {
    for (int i = 0; i < wd->part_count; i++) {
        const WeaponModelPart *p = &wd->parts[i];
        draw_box(v3_add(pos, quat_rotate(rot, p->offset)), rot, p->half, p->color, viewmodel);
    }
    if (!H->db) return;
    AttachDrawPart parts[ATT_SLOTS * ATT_MAX_PARTS];
    int n = attach_draw_parts(H->db, wd, wd->att_applied, parts, ARRAY_COUNT(parts));
    for (int i = 0; i < n; i++)
        draw_box(v3_add(pos, quat_rotate(rot, parts[i].center)), quat_mul(rot, parts[i].rot), parts[i].half, parts[i].color, viewmodel);
}

static void draw_held_item(const ItemDef *it, vec3 pos, quat rot, bool viewmodel) {
    // Pickup sizes are world-scale; held items are palm-sized (item_held_box, also the ground model).
    vec3 center, half;
    item_held_box(it, &center, &half);
    draw_box(v3_add(pos, quat_rotate(rot, center)), rot, half, it->color, viewmodel);
}

bool hands_muzzle(vec3 *pos, vec3 *fwd) {
    if (!H || !H->initialized || !H->weapon || H->weapon->kind == WPN_MELEE) return false;
    // The viewmodel may use its own FOV: return where the barrel tip appears in the world.
    if (pos) *pos = render_viewmodel_to_world(v3_add(H->weapon_pos, quat_rotate(H->weapon_rot, H->weapon->muzzle_offset)));
    if (fwd) *fwd = quat_rotate(H->weapon_rot, v3(0, 0, -1));
    return true;
}

bool hands_fp_item(vec3 *pos, quat *rot, const WeaponDef **wd) {
    if (!H || !H->initialized || !H->weapon) return false;
    if (pos) *pos = H->weapon_pos;
    if (rot) *rot = H->weapon_rot;
    if (wd) *wd = H->weapon;
    return true;
}

bool hands_fp_targets(HandsFpTargets *out) {
    if (!H || !H->fp_valid) return false;
    *out = H->fp;
    return true;
}

void hands_set_body_arms(bool on) { if (H) H->body_arms = on; }

void hands_vm_kick(vec3 *pos, vec3 *rot) {
    *pos = H ? H->kick_pos : v3(0, 0, 0);
    *rot = H ? H->kick_rot : v3(0, 0, 0);
}

void hands_fp_set_item(vec3 pos, quat rot) {
    if (!H) return;
    H->weapon_pos = pos;
    H->weapon_rot = rot;
}

void hands_fp_shift(vec3 delta) {
    if (!H) return;
    H->weapon_pos = v3_add(H->weapon_pos, delta);
    for (int h = 0; h < 2; h++)
        if (H->fp.on_weapon[h]) {
            H->fp.palm[h].t = v3_add(H->fp.palm[h].t, delta);
            H->chain[h].hand_pos = v3_add(H->chain[h].hand_pos, delta);
        }
}

void hands_render(void) {
    if (!H || !H->initialized) return;
    if (!H->body_arms)
        for (int h = 0; h < 2; h++) draw_arm(&H->chain[h], h, H->cam_rot, true);
    H->body_arms = false;
    if (H->weapon) draw_weapon(H->weapon, H->weapon_pos, H->weapon_rot, true);
    else if (H->held_item && H->held_item->kind != ITEM_WEAPON) draw_held_item(H->held_item, H->weapon_pos, H->weapon_rot, true);
}

void hands_draw_held(const WeaponDef *wd, const ItemDef *it, vec3 pos, quat rot, u32 flags) {
    if (!H) return;
    s_world_item_flags = flags;
    if (wd) draw_weapon(wd, pos, rot, false);
    else if (it) draw_held_item(it, pos, rot, false);
    s_world_item_flags = MAT_NO_SHADOW;
}

void hands_draw_held_att(const WeaponDef *wd, const ItemDef *it, const u8 *att, vec3 pos, quat rot, u32 flags) {
    if (!H) return;
    if (wd && H->db) {
        static WeaponDef eff;
        attach_weapon_effective(H->db, wd, att, &eff);
        hands_draw_held(&eff, NULL, pos, rot, flags);
        return;
    }
    hands_draw_held(wd, it, pos, rot, flags);
}

void hands_render_remote(World *w, int slot, vec3 eye, float yaw, float pitch) {
    if (!H) return;
    const Player *p = &w->players[slot];
    quat cam_rot = quat_euler(yaw, pitch * 0.6f, 0);
    int ent_i = (int)ENT_INDEX(p->ent);
    u16 item = w->item[ent_i];
    const WeaponDef *wd = item ? item_weapon(w->items, item) : NULL;
    static WeaponDef eff;
    if (wd) {
        attach_weapon_effective(w->items, wd, w->attach[ent_i], &eff);
        wd = &eff;
    }
    H->db = w->items;
    const char *name = "idle";
    if (wd) name = wd->kind == WPN_MELEE ? "melee" : (!strcmp(wd->pose, "rifle") ? "rifle" : "pistol");
    if (w->flags[ent_i] & EF_HANG) name = "hang";
    HandPose pose = sample_clip(find_clip(name), 0);
    ArmChain c[2];
    vec3 tp[2];
    quat tr[2];
    for (int h = 0; h < 2; h++) {
        tp[h] = cam_to_world(eye, cam_rot, pose.pos[h]);
        tr[h] = quat_mul(cam_rot, pose.rot[h]);
    }
    if (wd && wd->kind != WPN_MELEE) {
        tp[HAND_LEFT] = v3_add(tp[HAND_RIGHT], quat_rotate(tr[HAND_RIGHT], wd->support_offset));
        tr[HAND_LEFT] = tr[HAND_RIGHT];
    }
    for (int h = 0; h < 2; h++) {
        solve_arm(h, eye, cam_rot, tp[h], tr[h], pose.grip[h], &c[h]);
        draw_arm(&c[h], h, cam_rot, false);
    }
    if (wd) draw_weapon(wd, c[HAND_RIGHT].hand_pos, c[HAND_RIGHT].hand_rot, false);
}
