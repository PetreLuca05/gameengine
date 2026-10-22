// Viewmodel sway layers (see weapon_sway.h, docs/FIRE_ANIM.md "Viewmodel sway").
#include "weapon_sway.h"
#include "cJSON.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

void weapon_sway_defaults(WeaponSwayConfig *c) {
    memset(c, 0, sizeof(*c));
    c->look_filter_hz = 20.0f;
    c->look_yaw = 0.018f;
    c->look_pitch = 0.014f;
    c->look_roll = 0.012f;
    c->look_pos = 0.00007f;
    c->look_max_rot = 5.0f;
    c->look_max_pos = 0.025f;
    c->look_k = 160.0f;
    c->look_zeta = 0.68f;
    c->move_roll = 2.5f;
    c->move_strafe_pos = 0.012f;
    c->move_fwd_pitch = 0.8f;
    c->move_fwd_pos = 0.007f;
    c->move_max_speed = 1.7f;
    c->move_k = 110.0f;
    c->move_zeta = 0.72f;
    c->move_accel_threshold = 6.0f;
    c->move_accel_impulse = 0.03f;
    c->walk_speed = 4.2f;
    c->bob_freq = 0.3f;
    c->bob_x = 0.006f;
    c->bob_y = 0.008f;
    c->bob_roll = 1.2f;
    c->bob_pitch = 0.8f;
    c->bob_yaw = 0.6f;
    c->bob_attack = 0.12f;
    c->bob_decay = 0.25f;
    c->bob_crouch = 0.5f;
    c->sprint_bob_mult = 1.8f;
    c->sprint_freq_mult = 0.8f;
    c->sprint_roll = 2.0f;
    c->sprint_yaw = 1.4f;
    c->sprint_rot = v3(-12.0f, 24.0f, 8.0f);
    c->sprint_pos = v3(0.02f, -0.035f, 0.0f);
    c->tac_rot = v3(-18.0f, 34.0f, 16.0f);
    c->tac_pos = v3(0.03f, -0.05f, 0.02f);
    c->sprint_in = 0.18f;
    c->sprint_out = 0.08f;
    c->jump_impulse = 0.35f;
    c->jump_pitch = 50.0f;
    c->air_pos = 0.0025f;
    c->air_pitch = 0.4f;
    c->air_max_pos = 0.025f;
    c->air_max_rot = 3.0f;
    c->land_min_speed = 2.5f;
    c->land_scale = 0.16f;
    c->land_max = 1.2f;
    c->land_pitch = 140.0f;
    c->land_crouch = 0.5f;
    c->air_k = 150.0f;
    c->air_zeta = 0.5f;
    c->stance_down = 0.3f;
    c->stance_up = 0.15f;
    c->cam_land_pitch = 25.0f;
    c->cam_k = 120.0f;
    c->cam_zeta = 0.6f;
    c->idle_rot = 0.4f;
    c->idle_pos = 0.0008f;
    c->idle_f1 = 0.35f;
    c->idle_f2 = 0.9f;
    c->idle_noise = 0.15f;
    c->idle_noise_hz = 0.25f;
    c->idle_fade_in = 0.5f;
    c->idle_fade_out = 0.2f;
    c->idle_move_speed = 0.3f;
    c->idle_crouch = 0.7f;
    c->hold_breath = 0.1f;
    c->ads_settle_pitch = 20.0f;
    c->ads_settle_pos = 0.04f;
    c->ads_fov_delta = 0.0f;
    c->ads_ease = true;
    c->ads_rot[WSW_LOOK] = 0.2f;
    c->ads_pos[WSW_LOOK] = 0.1f;
    c->ads_rot[WSW_MOVE] = 0.2f;
    c->ads_pos[WSW_MOVE] = 0.2f;
    c->ads_rot[WSW_BOB] = 0.12f;
    c->ads_pos[WSW_BOB] = 0.12f;
    c->ads_rot[WSW_AIR] = 0.35f;
    c->ads_pos[WSW_AIR] = 0.3f;
    c->ads_rot[WSW_IDLE] = 0.25f;
    c->ads_pos[WSW_IDLE] = 0.25f;
    for (int i = 0; i < WSW_LAYERS; i++) c->layer_on[i] = true;
    c->weight = 0.5f;
    c->pivot_offset = v3(0, 0, 0);
    c->max_rot = v3(40.0f, 40.0f, 30.0f);
    c->max_pos = v3(0.08f, 0.08f, 0.08f);
    c->scale = 1.0f;
}

// ------------------------------------------------------------------ fields (parse / save / tuner)

#define F(sec, key, field, lo, hi, step) { sec, key, WSF_FLOAT, offsetof(WeaponSwayConfig, field), lo, hi, step }
#define V(sec, key, field, lo, hi, step) { sec, key, WSF_VEC3, offsetof(WeaponSwayConfig, field), lo, hi, step }
static const WeaponSwayField k_fields[] = {
    F("look", "filter_hz", look_filter_hz, 1, 60, 0.5f),
    F("look", "yaw", look_yaw, 0, 0.1f, 0.001f),
    F("look", "pitch", look_pitch, 0, 0.1f, 0.001f),
    F("look", "roll", look_roll, 0, 0.1f, 0.001f),
    F("look", "pos", look_pos, 0, 0.0005f, 0.00001f),
    F("look", "max_rot", look_max_rot, 0, 15, 0.1f),
    F("look", "max_pos", look_max_pos, 0, 0.06f, 0.001f),
    F("look", "stiffness", look_k, 10, 600, 5),
    F("look", "damping_ratio", look_zeta, 0.1f, 1.5f, 0.01f),
    F("move", "roll", move_roll, 0, 10, 0.1f),
    F("move", "strafe_pos", move_strafe_pos, 0, 0.04f, 0.001f),
    F("move", "fwd_pitch", move_fwd_pitch, 0, 5, 0.1f),
    F("move", "fwd_pos", move_fwd_pos, 0, 0.03f, 0.001f),
    F("move", "max_speed", move_max_speed, 0.5f, 4, 0.1f),
    F("move", "stiffness", move_k, 10, 600, 5),
    F("move", "damping_ratio", move_zeta, 0.1f, 1.5f, 0.01f),
    F("move", "accel_threshold", move_accel_threshold, 0, 50, 0.5f),
    F("move", "accel_impulse", move_accel_impulse, 0, 0.2f, 0.005f),
    F("bob", "walk_speed", walk_speed, 1, 10, 0.1f),
    F("bob", "freq", bob_freq, 0.05f, 1, 0.01f),
    F("bob", "x", bob_x, 0, 0.03f, 0.0005f),
    F("bob", "y", bob_y, 0, 0.03f, 0.0005f),
    F("bob", "roll", bob_roll, 0, 6, 0.1f),
    F("bob", "pitch", bob_pitch, 0, 6, 0.1f),
    F("bob", "yaw", bob_yaw, 0, 6, 0.1f),
    F("bob", "attack", bob_attack, 0.01f, 1, 0.01f),
    F("bob", "decay", bob_decay, 0.01f, 1, 0.01f),
    F("bob", "crouch", bob_crouch, 0, 2, 0.05f),
    F("bob", "sprint_mult", sprint_bob_mult, 0, 4, 0.05f),
    F("bob", "sprint_freq_mult", sprint_freq_mult, 0.2f, 2, 0.05f),
    F("bob", "sprint_roll", sprint_roll, 0, 10, 0.1f),
    F("bob", "sprint_yaw", sprint_yaw, 0, 10, 0.1f),
    V("sprint_pose", "rot", sprint_rot, -45, 45, 0.5f),
    V("sprint_pose", "pos", sprint_pos, -0.1f, 0.1f, 0.001f),
    V("sprint_pose", "tactical_rot", tac_rot, -60, 60, 0.5f),
    V("sprint_pose", "tactical_pos", tac_pos, -0.12f, 0.12f, 0.001f),
    F("sprint_pose", "in", sprint_in, 0.01f, 1, 0.01f),
    F("sprint_pose", "out", sprint_out, 0.01f, 1, 0.01f),
    F("air", "jump_impulse", jump_impulse, 0, 2, 0.01f),
    F("air", "jump_pitch", jump_pitch, 0, 300, 1),
    F("air", "pos", air_pos, 0, 0.01f, 0.0001f),
    F("air", "pitch", air_pitch, 0, 3, 0.05f),
    F("air", "max_pos", air_max_pos, 0, 0.06f, 0.001f),
    F("air", "max_rot", air_max_rot, 0, 15, 0.1f),
    F("air", "land_min_speed", land_min_speed, 0, 10, 0.1f),
    F("air", "land_scale", land_scale, 0, 1, 0.01f),
    F("air", "land_max", land_max, 0, 4, 0.05f),
    F("air", "land_pitch", land_pitch, 0, 500, 5),
    F("air", "land_crouch", land_crouch, 0, 1, 0.05f),
    F("air", "stiffness", air_k, 10, 600, 5),
    F("air", "damping_ratio", air_zeta, 0.1f, 1.5f, 0.01f),
    F("air", "stance_down", stance_down, 0, 2, 0.01f),
    F("air", "stance_up", stance_up, 0, 2, 0.01f),
    F("camera", "land_pitch", cam_land_pitch, 0, 200, 1),
    F("camera", "stiffness", cam_k, 10, 600, 5),
    F("camera", "damping_ratio", cam_zeta, 0.1f, 1.5f, 0.01f),
    F("idle", "rot", idle_rot, 0, 3, 0.02f),
    F("idle", "pos", idle_pos, 0, 0.01f, 0.0001f),
    F("idle", "f1", idle_f1, 0.05f, 3, 0.01f),
    F("idle", "f2", idle_f2, 0.05f, 3, 0.01f),
    F("idle", "noise", idle_noise, 0, 2, 0.01f),
    F("idle", "noise_hz", idle_noise_hz, 0.01f, 3, 0.01f),
    F("idle", "fade_in", idle_fade_in, 0.01f, 3, 0.01f),
    F("idle", "fade_out", idle_fade_out, 0.01f, 3, 0.01f),
    F("idle", "move_speed", idle_move_speed, 0, 3, 0.05f),
    F("idle", "crouch", idle_crouch, 0, 2, 0.05f),
    F("idle", "hold_breath", hold_breath, 0, 1, 0.01f),
    F("ads_transition", "settle_pitch", ads_settle_pitch, -100, 100, 1),
    F("ads_transition", "settle_pos", ads_settle_pos, -0.3f, 0.3f, 0.005f),
    F("ads_transition", "fov_delta", ads_fov_delta, -20, 20, 0.5f),
    { "ads_transition", "ease", WSF_BOOL, offsetof(WeaponSwayConfig, ads_ease), 0, 1, 1 },
    { "ads", "look", WSF_ADS, WSW_LOOK, 0, 1, 0.01f },
    { "ads", "move", WSF_ADS, WSW_MOVE, 0, 1, 0.01f },
    { "ads", "bob", WSF_ADS, WSW_BOB, 0, 1, 0.01f },
    { "ads", "air", WSF_ADS, WSW_AIR, 0, 1, 0.01f },
    { "ads", "idle", WSF_ADS, WSW_IDLE, 0, 1, 0.01f },
    { "layers", "look", WSF_LAYER, WSW_LOOK, 0, 1, 1 },
    { "layers", "move", WSF_LAYER, WSW_MOVE, 0, 1, 1 },
    { "layers", "bob", WSF_LAYER, WSW_BOB, 0, 1, 1 },
    { "layers", "air", WSF_LAYER, WSW_AIR, 0, 1, 1 },
    { "layers", "idle", WSF_LAYER, WSW_IDLE, 0, 1, 1 },
    F(NULL, "weight", weight, 0, 1, 0.01f),
    V(NULL, "pivot_offset", pivot_offset, -0.3f, 0.3f, 0.005f),
    V(NULL, "max_rot", max_rot, 0, 60, 0.5f),
    V(NULL, "max_pos", max_pos, 0, 0.2f, 0.002f),
    F(NULL, "scale", scale, 0, 2, 0.01f),
};
#undef F
#undef V

const WeaponSwayField *weapon_sway_fields(int *count) {
    *count = (int)(sizeof(k_fields) / sizeof(k_fields[0]));
    return k_fields;
}

const char *weapon_sway_layer_name(int layer) {
    static const char *names[WSW_LAYERS] = { "look", "move", "bob", "air", "idle" };
    return layer >= 0 && layer < WSW_LAYERS ? names[layer] : "?";
}

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }

static bool jvec(const cJSON *a, float *f, int n) {
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != n) return false;
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(e)) return false;
        f[i] = (float)e->valuedouble;
    }
    return true;
}

void weapon_sway_parse(WeaponSwayConfig *c, const cJSON *j) {
    if (!j) return;
    char *base = (char *)c;
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
        const WeaponSwayField *f = &k_fields[i];
        const cJSON *n = jget(f->section ? jget(j, f->section) : j, f->key);
        if (!n) continue;
        float v[3];
        switch (f->type) {
        case WSF_FLOAT: if (cJSON_IsNumber(n)) *(float *)(base + f->off) = (float)n->valuedouble; break;
        case WSF_VEC3: if (jvec(n, v, 3)) *(vec3 *)(base + f->off) = v3(v[0], v[1], v[2]); break;
        case WSF_ADS:   // [rotation, position]
            if (jvec(n, v, 2)) { c->ads_rot[f->off] = v[0]; c->ads_pos[f->off] = v[1]; }
            break;
        case WSF_LAYER: if (cJSON_IsBool(n)) c->layer_on[f->off] = cJSON_IsTrue(n); break;
        case WSF_BOOL: if (cJSON_IsBool(n)) *(bool *)(base + f->off) = cJSON_IsTrue(n); break;
        default: break;
        }
    }
}

static bool fdiff(float a, float b) { return fabsf(a - b) > 1e-7f * fmaxf(1.0f, fabsf(a)); }

// Floats as the file writes them: 6 significant digits (no 0.800000011920929).
static double round6(float v) {
    if (v == 0.0f) return 0.0;
    double m = pow(10.0, 5 - (int)floor(log10(fabs((double)v))));
    return round((double)v * m) / m;
}
static cJSON *jarr(const float *v, int n) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(round6(v[i])));
    return a;
}

cJSON *weapon_sway_diff_json(const WeaponSwayConfig *c, const WeaponSwayConfig *b) {
    cJSON *root = cJSON_CreateObject();
    const char *cb = (const char *)c, *bb = (const char *)b;
    for (size_t i = 0; i < sizeof(k_fields) / sizeof(k_fields[0]); i++) {
        const WeaponSwayField *f = &k_fields[i];
        cJSON *item = NULL;
        if (f->type == WSF_FLOAT) {
            float x = *(const float *)(cb + f->off);
            if (fdiff(x, *(const float *)(bb + f->off))) item = cJSON_CreateNumber(round6(x));
        } else if (f->type == WSF_VEC3) {
            vec3 x = *(const vec3 *)(cb + f->off), y = *(const vec3 *)(bb + f->off);
            if (fdiff(x.x, y.x) || fdiff(x.y, y.y) || fdiff(x.z, y.z)) {
                float a[3] = { x.x, x.y, x.z };
                item = jarr(a, 3);
            }
        } else if (f->type == WSF_ADS) {
            if (fdiff(c->ads_rot[f->off], b->ads_rot[f->off]) || fdiff(c->ads_pos[f->off], b->ads_pos[f->off])) {
                float a[2] = { c->ads_rot[f->off], c->ads_pos[f->off] };
                item = jarr(a, 2);
            }
        } else if (f->type == WSF_LAYER) {
            if (c->layer_on[f->off] != b->layer_on[f->off]) item = cJSON_CreateBool(c->layer_on[f->off]);
        } else if (f->type == WSF_BOOL) {
            bool x = *(const bool *)(cb + f->off);
            if (x != *(const bool *)(bb + f->off)) item = cJSON_CreateBool(x);
        }
        if (!item) continue;
        cJSON *dst = root;
        if (f->section) {
            dst = cJSON_GetObjectItemCaseSensitive(root, f->section);
            if (!dst) dst = cJSON_AddObjectToObject(root, f->section);
        }
        cJSON_AddItemToObject(dst, f->key, item);
    }
    return root;
}

// ------------------------------------------------------------------ state, helpers

void weapon_sway_reset(WeaponSwayState *s) { memset(s, 0, sizeof(*s)); }

// Semi-implicit Euler on a damped spring (stiffness k, damping ratio zeta), sub-stepped at <= 1/240 s
// so the motion is the same at any frame rate.
static void spring3(WsSpring3 *s, vec3 target, float k, float zeta, float dt) {
    if (dt <= 0.0f) return;
    float c = 2.0f * zeta * sqrtf(fmaxf(k, 0.0f));
    int n = (int)ceilf(dt * 240.0f);
    n = n < 1 ? 1 : n;
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        s->v = v3_add(s->v, v3_scale(v3_sub(v3_scale(v3_sub(target, s->x), k), v3_scale(s->v, c)), h));
        s->x = v3_add(s->x, v3_scale(s->v, h));
    }
}

static void spring1(float *x, float *v, float target, float k, float zeta, float dt) {
    WsSpring3 s = { v3(*x, 0, 0), v3(*v, 0, 0) };
    spring3(&s, v3(target, 0, 0), k, zeta, dt);
    *x = s.x.x;
    *v = s.v.x;
}

static float ads_mix(float m, float ads) { return 1.0f + (m - 1.0f) * saturate(ads); }

static float ease(float t) {
    t = saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

// Smooth 1D value noise in -1..1 (cubic-eased between hashed lattice values).
static float hash1(int i, u32 seed) {
    u32 h = (u32)i * 0x27d4eb2du ^ (seed * 0x165667b1u);
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return (float)(h & 0xFFFFu) / 32767.5f - 1.0f;
}
static float noise1(float x, u32 seed) {
    float fl = floorf(x);
    int i = (int)fl;
    float f = ease(x - fl);
    return hash1(i, seed) + (hash1(i + 1, seed) - hash1(i, seed)) * f;
}

// ------------------------------------------------------------------ layers

// 1. Look sway: the item trails the view's rotation (rates in deg/s, filtered), overshooting slightly.
void weapon_sway_look(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos) {
    vec2 rate = dt > 1e-5f ? v2(in->mouse_dx / dt, in->mouse_dy / dt) : s->rate_f;
    float a = 1.0f - expf(-2.0f * DG_PI * c->look_filter_hz * dt);
    s->rate_f = v2(s->rate_f.x + (rate.x - s->rate_f.x) * a, s->rate_f.y + (rate.y - s->rate_f.y) * a);
    float dx = s->rate_f.x, dy = s->rate_f.y, mr = c->look_max_rot, mp = c->look_max_pos;
    // Turning right: the muzzle stays behind (yaw left), the gun tilts into the turn (top right) and
    // drags left; looking up: the muzzle stays low and the gun drags down.
    vec3 tr = v3(clampf(-dy * c->look_pitch, -mr, mr), clampf(dx * c->look_yaw, -mr, mr), clampf(-dx * c->look_roll, -mr, mr));
    vec3 tp = v3(clampf(-dx * c->look_pos, -mp, mp), clampf(-dy * c->look_pos, -mp, mp), 0.0f);
    spring3(&s->look_rot, tr, c->look_k, c->look_zeta, dt);
    spring3(&s->look_pos, tp, c->look_k, c->look_zeta, dt);
    *rot = v3_scale(s->look_rot.x, ads_mix(c->ads_rot[WSW_LOOK], ads));
    *pos = v3_scale(s->look_pos.x, ads_mix(c->ads_pos[WSW_LOOK], ads));
}

// 2. Movement sway: roll into the strafe and shift against it, push back / dip moving forward; sudden
// starts and stops kick the spring (the gun's inertia: it settles after you stop).
void weapon_sway_move(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos) {
    vec3 vl = in->velocity_local;
    float ws = c->walk_speed > 0.1f ? c->walk_speed : 4.2f, ms = c->move_max_speed;
    float nx = clampf(vl.x / ws, -ms, ms), nz = clampf(vl.z / ws, -ms, ms);
    vec3 tr = v3(-nz * c->move_fwd_pitch, 0.0f, -nx * c->move_roll);
    vec3 tp = v3(-nx * c->move_strafe_pos, 0.0f, nz * c->move_fwd_pos);
    if (s->init && dt > 1e-5f) {
        vec3 dv = v3_sub(vl, s->prev_vel);
        float acc = sqrtf(dv.x * dv.x + dv.z * dv.z) / dt;
        if (acc > c->move_accel_threshold) {
            // The velocity change itself (not / dt): the same kick at any frame rate.
            s->move_pos.v = v3_add(s->move_pos.v, v3_scale(v3(-dv.x, 0.0f, dv.z), c->move_accel_impulse));
            s->move_rot.v = v3_add(s->move_rot.v, v3_scale(v3(0.0f, 0.0f, dv.x), c->move_accel_impulse * 100.0f));
        }
    }
    s->prev_vel = vl;
    spring3(&s->move_rot, tr, c->move_k, c->move_zeta, dt);
    spring3(&s->move_pos, tp, c->move_k, c->move_zeta, dt);
    *rot = v3_scale(s->move_rot.x, ads_mix(c->ads_rot[WSW_MOVE], ads));
    *pos = v3_scale(s->move_pos.x, ads_mix(c->ads_pos[WSW_MOVE], ads));
}

// 3. Walk / sprint bob + the sprint pose. The phase follows the distance walked on the ground (frozen
// in the air and standing still: no snap), the amplitude eases in / decays out. Aimed only a small
// residual stays (the frequency is the same: it reads as the steps under a steady aim).
void weapon_sway_bob(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos, float *sprint_w) {
    // Sprint pose: linear progress, eased; out faster than in.
    bool sprint = in->sprinting && !in->is_mantle;
    float tin = fmaxf(c->sprint_in, 1e-3f), tout = fmaxf(c->sprint_out, 1e-3f);
    s->sprint_t = saturate(s->sprint_t + (sprint ? dt / tin : -dt / tout));
    s->tac_t = saturate(s->tac_t + (sprint && in->tactical_sprint ? dt / tin : -dt / tout));
    float w = ease(s->sprint_t), wt = ease(s->tac_t);
    float ws = c->walk_speed > 0.1f ? c->walk_speed : 4.2f;
    bool moving = in->grounded && in->speed > 0.05f && !in->is_mantle;
    float target = moving ? clampf(in->speed / ws, 0.0f, 2.0f) * (in->crouching ? c->bob_crouch : 1.0f) * saturate(in->bob_scale) : 0.0f;
    float tau = target > s->bob_amt ? c->bob_attack : c->bob_decay;
    s->bob_amt += (target - s->bob_amt) * (1.0f - expf(-dt / fmaxf(tau, 1e-3f)));
    s->bob_sprint = w;
    float freq = c->bob_freq * (1.0f + (c->sprint_freq_mult - 1.0f) * w);
    s->step = 0;
    if (moving) {
        // The footsteps are the dips (pi / 2, 3 pi / 2): report the one crossed.
        float p0 = s->bob_phase, p1 = p0 + 2.0f * DG_PI * freq * in->speed * dt;
        for (int k = 0; k < 3 && !s->step; k++) {
            float m = 0.5f * DG_PI + (float)k * DG_PI;
            if (p0 < m && p1 >= m) s->step = (k & 1) ? 2 : 1;
        }
        s->bob_phase = fmodf(p1, 2.0f * DG_PI);
    }
    float A = s->bob_amt * (1.0f + (c->sprint_bob_mult - 1.0f) * w);
    float ph = s->bob_phase;
    float sn = sinf(ph), dip = (cosf(2.0f * ph) - 1.0f) * 0.5f;   // dip: 0 .. -1 twice per cycle (the footsteps)
    float roll = c->bob_roll + (c->sprint_roll - c->bob_roll) * w, yaw = c->bob_yaw + (c->sprint_yaw - c->bob_yaw) * w;
    vec3 br = v3(dip * c->bob_pitch * A, sn * yaw * A, sn * roll * A);
    vec3 bp = v3(sn * c->bob_x * A, dip * c->bob_y * A, 0.0f);
    br = v3_scale(br, ads_mix(c->ads_rot[WSW_BOB], ads));
    bp = v3_scale(bp, ads_mix(c->ads_pos[WSW_BOB], ads));
    // The pose itself (sprinting never aims; aiming drops it with the sprint). A weapon's own run clip
    // carrying the gun already lowers it (bob_scale 0): no second pose on top.
    float carry = saturate(in->bob_scale);
    vec3 pr = v3_scale(v3_add(v3_scale(c->sprint_rot, w * (1.0f - wt)), v3_scale(c->tac_rot, wt)), carry);
    vec3 pp = v3_scale(v3_add(v3_scale(c->sprint_pos, w * (1.0f - wt)), v3_scale(c->tac_pos, wt)), carry);
    *rot = v3_add(br, pr);
    *pos = v3_add(bp, pp);
    *sprint_w = w;
}

void weapon_sway_stance_change(WeaponSwayState *s, const WeaponSwayConfig *c, bool down) {
    s->air_pos.v.y += down ? -c->stance_down : c->stance_up;
}

// 4. Jump / fall / land (+ the stance dips, weapon_sway_stance_change): one heavy spring for the
// position (y up) and the pitch. Jumping dips the gun with the nose up; in the air it floats with
// the vertical speed; a landing hits the spring by the impact (soft ones barely, crouched less) and
// it bounces once. The camera's own dip is a separate spring the game may read.
void weapon_sway_air(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos) {
    if (in->just_jumped) {
        s->air_pos.v.y -= c->jump_impulse;
        s->air_rot.v.x += c->jump_pitch;
    }
    if (in->just_landed) {
        float imp = clampf((fabsf(in->land_impact_speed) - c->land_min_speed) * c->land_scale, 0.0f, c->land_max);
        if (in->crouching) imp *= c->land_crouch;
        s->air_pos.v.y -= imp;
        s->air_rot.v.x -= imp * c->land_pitch;
        s->cam_v -= imp * c->cam_land_pitch;
    }
    vec3 tp = v3(0, 0, 0), tr = v3(0, 0, 0);
    if (!in->grounded && !in->is_mantle) {
        // Falling (vy < 0): the gun floats up, nose up; rising: a little down.
        float vy = in->velocity.y;
        tp.y = clampf(-vy * c->air_pos, -c->air_max_pos, c->air_max_pos);
        tr.x = clampf(-vy * c->air_pitch, -c->air_max_rot, c->air_max_rot);
    }
    spring3(&s->air_pos, tp, c->air_k, c->air_zeta, dt);
    spring3(&s->air_rot, tr, c->air_k, c->air_zeta, dt);
    spring1(&s->cam, &s->cam_v, 0.0f, c->cam_k, c->cam_zeta, dt);
    *rot = v3_scale(v3(s->air_rot.x.x, 0, 0), ads_mix(c->ads_rot[WSW_AIR], ads));
    *pos = v3_scale(v3(0, s->air_pos.x.y, 0), ads_mix(c->ads_pos[WSW_AIR], ads));
}

// 5. Idle / breathing: two sines at a non-integer ratio (pitch at f1 + f2, yaw at half f1: a slow
// figure-eight) and slow value noise, so it never looks periodic; fades out moving / in the air,
// back in when still; crouched its own amplitude; holding the breath steadies it.
void weapon_sway_idle(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos) {
    float bk = in->holding_breath ? c->hold_breath : 1.0f;
    s->breath += (bk - s->breath) * (1.0f - expf(-dt / 0.2f));
    bool moving = !in->grounded || in->speed > c->idle_move_speed || in->is_mantle;
    s->idle_w = saturate(s->idle_w + (moving ? -dt / fmaxf(c->idle_fade_out, 1e-3f) : dt / fmaxf(c->idle_fade_in, 1e-3f)));
    float w = ease(s->idle_w) * s->breath * (in->crouching ? c->idle_crouch : 1.0f);
    float t = s->t, f1 = 2.0f * DG_PI * c->idle_f1, f2 = 2.0f * DG_PI * c->idle_f2, nt = t * c->idle_noise_hz;
    vec3 r = v3((sinf(f1 * t) * 0.65f + sinf(f2 * t + 1.3f) * 0.25f) * c->idle_rot + noise1(nt, 1u) * c->idle_noise,
                (sinf(f1 * 0.5f * t + 0.7f) * 0.8f + sinf(f2 * 0.83f * t + 2.2f) * 0.2f) * c->idle_rot + noise1(nt, 2u) * c->idle_noise,
                noise1(nt * 0.7f, 3u) * c->idle_noise * 0.5f);
    vec3 p = v3(0, sinf(f1 * t) * c->idle_pos, 0);
    *rot = v3_scale(r, w * ads_mix(c->ads_rot[WSW_IDLE], ads));
    *pos = v3_scale(p, w * ads_mix(c->ads_pos[WSW_IDLE], ads));
}

// ------------------------------------------------------------------ composition

void weapon_sway_weighted(const WeaponSwayConfig *c, WeaponSwayConfig *e) {
    *e = *c;
    float w = clampf(c->weight, 0.0f, 1.0f);
    float kf = 1.0f + (0.5f - w) * 0.5f;   // light: stiffer (x1.25), heavy: softer (x0.75)
    float af = 1.0f + (w - 0.5f) * 0.4f;   // light: smaller (x0.8), heavy: larger (x1.2)
    e->look_k *= kf;
    e->move_k *= kf;
    e->air_k *= kf;
    e->cam_k *= kf;
    float *amp[] = { &e->look_yaw, &e->look_pitch, &e->look_roll, &e->look_pos, &e->move_roll, &e->move_strafe_pos, &e->move_fwd_pitch, &e->move_fwd_pos,
                     &e->bob_x, &e->bob_y, &e->bob_roll, &e->bob_pitch, &e->bob_yaw, &e->sprint_roll, &e->sprint_yaw, &e->jump_impulse, &e->jump_pitch,
                     &e->air_pos, &e->air_pitch, &e->land_scale, &e->idle_rot, &e->idle_pos, &e->idle_noise };
    for (size_t i = 0; i < sizeof(amp) / sizeof(amp[0]); i++) *amp[i] *= af;
}

void weapon_sway_update(WeaponSwayState *s, const WeaponSwayConfig *cfg, const WeaponSwayInput *in, float dt, SwayOutput *out) {
    memset(out, 0, sizeof(*out));
    dt = clampf(dt, 0.0f, 0.1f);
    WeaponSwayConfig e;
    weapon_sway_weighted(cfg, &e);
    const WeaponSwayConfig *c = &e;
    if (!s->init) {
        s->idle_w = 1.0f;   // a fresh state (new weapon, view) breathes at once
        s->breath = 1.0f;
    }
    s->t += dt;
    // 7. ADS: the multipliers follow the eased weight; the transition kicks the look spring (the gun
    // settles into the sight / out of it instead of sliding).
    float raw = saturate(in->ads_blend), ads = c->ads_ease ? ease(raw) : raw;
    if (s->have_ads && c->layer_on[WSW_LOOK]) {
        float d = raw - s->ads_prev;
        s->look_rot.v.x -= d * c->ads_settle_pitch;
        s->look_pos.v.z += d * c->ads_settle_pos;
    }
    s->ads_prev = raw;
    s->have_ads = true;
    // 8. Stance: crouch / stand edges dip / lift the gun (the air spring).
    if (s->have_crouch && in->crouching != s->crouch_prev && c->layer_on[WSW_AIR]) weapon_sway_stance_change(s, c, in->crouching);
    s->crouch_prev = in->crouching;
    s->have_crouch = true;
    if (c->layer_on[WSW_LOOK]) weapon_sway_look(s, c, in, ads, dt, &out->layer_rot[WSW_LOOK], &out->layer_pos[WSW_LOOK]);
    else { s->look_rot = s->look_pos = (WsSpring3){ { 0 } }; s->rate_f = v2(0, 0); }
    if (c->layer_on[WSW_MOVE]) weapon_sway_move(s, c, in, ads, dt, &out->layer_rot[WSW_MOVE], &out->layer_pos[WSW_MOVE]);
    else { s->move_rot = s->move_pos = (WsSpring3){ { 0 } }; s->prev_vel = in->velocity_local; }
    if (c->layer_on[WSW_BOB]) weapon_sway_bob(s, c, in, ads, dt, &out->layer_rot[WSW_BOB], &out->layer_pos[WSW_BOB], &out->sprint_w);
    else { s->bob_amt = s->sprint_t = s->tac_t = 0.0f; s->step = 0; }
    if (c->layer_on[WSW_AIR]) weapon_sway_air(s, c, in, ads, dt, &out->layer_rot[WSW_AIR], &out->layer_pos[WSW_AIR]);
    else { s->air_pos = s->air_rot = (WsSpring3){ { 0 } }; s->cam = s->cam_v = 0.0f; }
    if (c->layer_on[WSW_IDLE]) weapon_sway_idle(s, c, in, ads, dt, &out->layer_rot[WSW_IDLE], &out->layer_pos[WSW_IDLE]);
    else s->idle_w = 0.0f;
    s->init = true;
    vec3 r = v3(0, 0, 0), p = v3(0, 0, 0);
    for (int i = 0; i < WSW_LAYERS; i++) {
        r = v3_add(r, out->layer_rot[i]);
        p = v3_add(p, out->layer_pos[i]);
    }
    r = v3_scale(r, c->scale);
    p = v3_scale(p, c->scale);
    // The final clamp: whatever the layers add up to, the gun stays in the view.
    vec3 rc = v3(clampf(r.x, -c->max_rot.x, c->max_rot.x), clampf(r.y, -c->max_rot.y, c->max_rot.y), clampf(r.z, -c->max_rot.z, c->max_rot.z));
    vec3 pc = v3(clampf(p.x, -c->max_pos.x, c->max_pos.x), clampf(p.y, -c->max_pos.y, c->max_pos.y), clampf(p.z, -c->max_pos.z, c->max_pos.z));
    out->clamped = rc.x != r.x || rc.y != r.y || rc.z != r.z || pc.x != p.x || pc.y != p.y || pc.z != p.z;
    out->rot_deg = rc;
    out->pos_offset = pc;
    // Small-angle Euler -> quaternion from this frame's sum (pitch, yaw, roll): nothing accumulates.
    out->rot_offset = quat_euler(rc.y * DG_DEG2RAD, rc.x * DG_DEG2RAD, rc.z * DG_DEG2RAD);
    out->pivot_offset = v3_scale(c->pivot_offset, 1.0f - ads);
    out->fov_delta = c->ads_fov_delta * ads;
    out->idle_w = ease(s->idle_w);
    out->ads = ads;
}

vec3 weapon_sway_camera_offset(const WeaponSwayState *s) { return v3(s->cam, 0.0f, 0.0f); }
float weapon_sway_bob_phase(const WeaponSwayState *s) { return s->bob_phase; }
int weapon_sway_footstep(const WeaponSwayState *s) { return s->step; }
