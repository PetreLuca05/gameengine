#include "holdpoint.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_state_names[HP_STATE_COUNT] = { "hip", "ads", "sprint", "crouch", "prone", "empty", "inspect" };

const char *holdpoint_state_name(int s) { return (s >= 0 && s < HP_STATE_COUNT) ? k_state_names[s] : "hip"; }
int holdpoint_state_from_name(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < HP_STATE_COUNT; i++) if (!strcmp(s, k_state_names[i])) return i;
    return -1;
}

// ------------------------------------------------------------------ defaults

static HpPose P(float r, float u, float f, float pitch, float yaw, float roll) {
    HpPose p;
    memset(&p, 0, sizeof(p));   // padding bytes too: hold styles are hashed for the net asset manifest
    p.pos = v3(r, u, f);
    p.rot = v3(pitch, yaw, roll);
    p.set = true;
    return p;
}

void holdpoint_style_defaults(HoldPointStyle *s, const char *name) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name ? name : "rifle");
    bool pistol = !strcmp(s->name, "pistol");
    bool melee1 = !strcmp(s->name, "melee_1h"), melee2 = !strcmp(s->name, "melee_2h");
    bool small = !strcmp(s->name, "item_small") || !strcmp(s->name, "none");
    bool large = !strcmp(s->name, "item_large");
    bool lmg = !strcmp(s->name, "lmg");
    // Hip: the anchor (right grip) in front of the right shoulder, below the eye line.
    if (pistol) {
        s->state[HP_HIP] = P(0.09f, -0.17f, 0.36f, 0, 2, 0);
        s->state[HP_SPRINT] = P(0.10f, -0.32f, 0.22f, -45, 10, 15);
        s->eye_relief = 0.34f;
    } else if (melee1) {
        s->state[HP_HIP] = P(0.20f, -0.26f, 0.32f, 55, -10, 0);
        s->state[HP_SPRINT] = P(0.20f, -0.32f, 0.24f, 30, -10, 0);
    } else if (melee2) {
        s->state[HP_HIP] = P(0.16f, -0.25f, 0.36f, 25, 0, -12);
        s->state[HP_SPRINT] = P(0.16f, -0.30f, 0.24f, 10, 8, -15);
    } else if (small) {
        s->state[HP_HIP] = P(0.17f, -0.22f, 0.33f, -15, 0, 0);
        s->state[HP_SPRINT] = P(0.18f, -0.30f, 0.26f, -30, 0, 0);
    } else if (large) {
        s->state[HP_HIP] = P(0.06f, -0.24f, 0.36f, 0, 0, 0);
        s->state[HP_SPRINT] = P(0.06f, -0.30f, 0.30f, -10, 0, 0);
    } else {
        // Long guns: grip a little right of / below the eye, stock in the shoulder.
        s->state[HP_HIP] = P(0.11f, lmg ? -0.22f : -0.19f, lmg ? 0.16f : 0.18f, 0, 1.5f, 0);
        s->state[HP_SPRINT] = P(0.10f, -0.25f, 0.17f, -22, 20, 15);
        s->eye_relief = lmg ? 0.26f : 0.24f;
    }
    s->state[HP_CROUCH] = s->state[HP_HIP];
    s->state[HP_CROUCH].pos.y += 0.01f;
    s->state[HP_CROUCH].pos.z -= 0.015f;
    s->state[HP_PRONE] = s->state[HP_HIP];
    s->state[HP_PRONE].pos.y += 0.03f;
    s->state[HP_EMPTY] = s->state[HP_HIP];
    s->state[HP_EMPTY].pos.y -= 0.32f;          // lowered out of view (mantle, ledge, empty hands)
    s->state[HP_EMPTY].pos.z -= 0.08f;
    s->state[HP_EMPTY].rot.x -= 40.0f;
    s->state[HP_INSPECT] = P(0.04f, -0.12f, 0.34f, 10, 55, 35);
    if (!s->eye_relief) s->eye_relief = 0.3f;
    for (int i = 0; i < HP_STATE_COUNT; i++) s->blend_in[i] = 0.2f;
    s->blend_in[HP_ADS] = 0.16f;
    s->blend_in[HP_SPRINT] = 0.24f;
    s->blend_in[HP_EMPTY] = 0.25f;
    s->ads_scale = 0.25f;
    s->lag_rot = 1.6f;                          // deg per rad/s of view rotation
    s->lag_pos = 0.004f;
    s->lag_max = 5.0f;
    s->lag_k = 140.0f;
    s->lag_d = 18.0f;
    s->sway_rot = 0.35f;
    s->sway_freq = 0.55f;
    s->breath_pos = 0.0025f;
    s->breath_freq = 0.25f;
    s->bob_pos = 0.012f;
    s->bob_rot = 1.2f;
    s->bob_freq = 1.0f;
    s->kick_scale = 1.0f;
    s->col_back = 0.22f;
    s->col_up = 0.05f;
    s->col_pitch = 28.0f;
    s->col_curve = 1.4f;
    s->col_in_speed = 14.0f;
    s->col_out_speed = 6.0f;
    s->col_radius = 0.025f;
    s->clavicle_gain = melee1 || melee2 || small ? 30.0f : 45.0f;
    // Bladed stance: the support shoulder comes forward so the off hand reaches the handguard.
    s->twist = pistol ? 10.0f : (melee1 || small ? 0.0f : (melee2 ? 12.0f : 22.0f));
    s->clav_l = (melee1 || small) ? v3(0, 0, 0) : v3(0, -22.0f, pistol ? 0.0f : -6.0f);
    s->clav_r = (melee1 || small) ? v3(0, 0, 0) : v3(0, 6.0f, 0);
    s->reach_pull = (melee1 || small) ? 0.12f : 0.25f;
    s->pitch_soft = 45.0f;
    s->pitch_follow = 0.35f;
    s->ads_pitch_soft = 65.0f;
    s->ads_pitch_follow = 0.5f;
}

// ------------------------------------------------------------------ json

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static bool jv(const cJSON *o, const char *k, vec3 *out) {
    const cJSON *v = jget(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return false;
    *out = v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
    return true;
}

static void parse_pose(HpPose *p, const cJSON *o) {
    if (!o) return;
    if (jv(o, "pos", &p->pos)) p->set = true;
    if (jv(o, "rot", &p->rot)) p->set = true;
}

void holdpoint_style_parse(HoldPointStyle *s, const cJSON *o) {
    if (!o) return;
    const cJSON *st = jget(o, "states");
    for (int i = 0; i < HP_STATE_COUNT; i++) {
        const cJSON *e = jget(st, k_state_names[i]);
        if (!e) continue;
        parse_pose(&s->state[i], e);
        s->blend_in[i] = jf(e, "blend", s->blend_in[i]);
    }
    s->eye_relief = jf(o, "eye_relief", s->eye_relief);
    jv(o, "ads_rot", &s->ads_rot);
    const cJSON *pr = jget(o, "procedural");
    s->ads_scale = jf(pr, "ads_scale", s->ads_scale);
    const cJSON *lg = jget(pr, "look_lag");
    s->lag_rot = jf(lg, "rot", s->lag_rot);
    s->lag_pos = jf(lg, "pos", s->lag_pos);
    s->lag_max = jf(lg, "max", s->lag_max);
    s->lag_k = jf(lg, "stiffness", s->lag_k);
    s->lag_d = jf(lg, "damping", s->lag_d);
    const cJSON *sw = jget(pr, "sway");
    s->sway_rot = jf(sw, "rot", s->sway_rot);
    s->sway_freq = jf(sw, "freq", s->sway_freq);
    s->breath_pos = jf(sw, "breath", s->breath_pos);
    s->breath_freq = jf(sw, "breath_freq", s->breath_freq);
    const cJSON *bb = jget(pr, "bob");
    s->bob_pos = jf(bb, "pos", s->bob_pos);
    s->bob_rot = jf(bb, "rot", s->bob_rot);
    s->bob_freq = jf(bb, "freq", s->bob_freq);
    s->kick_scale = jf(pr, "kick_scale", s->kick_scale);
    const cJSON *co = jget(o, "collision");
    s->col_back = jf(co, "back", s->col_back);
    s->col_up = jf(co, "up", s->col_up);
    s->col_pitch = jf(co, "pitch", s->col_pitch);
    s->col_curve = jf(co, "curve", s->col_curve);
    s->col_in_speed = jf(co, "in_speed", s->col_in_speed);
    s->col_out_speed = jf(co, "out_speed", s->col_out_speed);
    s->col_radius = jf(co, "radius", s->col_radius);
    s->clavicle_gain = jf(o, "clavicle_gain", s->clavicle_gain);
    s->reach_pull = jf(o, "reach_pull", s->reach_pull);
    s->pitch_soft = jf(o, "pitch_soft", s->pitch_soft);
    s->pitch_follow = jf(o, "pitch_follow", s->pitch_follow);
    s->ads_pitch_soft = jf(o, "ads_pitch_soft", s->ads_pitch_soft);
    s->ads_pitch_follow = jf(o, "ads_pitch_follow", s->ads_pitch_follow);
    const cJSON *body = jget(o, "body");
    s->twist = jf(body, "twist", s->twist);
    jv(body, "clavicle_l", &s->clav_l);
    jv(body, "clavicle_r", &s->clav_r);
}

void holdpoint_override_parse(HoldPointOverride *o, const cJSON *j) {
    memset(o, 0, sizeof(*o));
    if (!j) return;
    o->any = true;
    const cJSON *st = jget(j, "states");
    for (int i = 0; i < HP_STATE_COUNT; i++) {
        const cJSON *e = jget(st, k_state_names[i]);
        if (!e) continue;
        o->state[i].pos = v3(0, 0, 0);
        o->state[i].rot = v3(0, 0, 0);
        parse_pose(&o->state[i], e);
    }
    o->eye_relief = jf(j, "eye_relief", 0);
    o->has_pivot = jv(j, "pivot", &o->pivot);
}

// Rounded in double precision (floats widened to double print as 0.17000000178813934).
static double r4(float x) { return round((double)x * 10000.0) / 10000.0; }
static void put_num(cJSON *o, const char *k, float v) { cJSON_AddNumberToObject(o, k, r4(v)); }
static void put_v3(cJSON *o, const char *k, vec3 v) {
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.x)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.y)));
    cJSON_AddItemToArray(a, cJSON_CreateNumber(r4(v.z)));
    cJSON_AddItemToObject(o, k, a);
}

char *holdpoint_style_to_json(const HoldPointStyle *s) {
    cJSON *o = cJSON_CreateObject();
    cJSON *st = cJSON_AddObjectToObject(o, "states");
    for (int i = 0; i < HP_STATE_COUNT; i++) {
        if (i == HP_ADS) continue;   // computed from the sight socket
        cJSON *e = cJSON_AddObjectToObject(st, k_state_names[i]);
        put_v3(e, "pos", s->state[i].pos);
        put_v3(e, "rot", s->state[i].rot);
        put_num(e, "blend", s->blend_in[i]);
    }
    cJSON *ads = cJSON_AddObjectToObject(st, "ads");
    put_num(ads, "blend", s->blend_in[HP_ADS]);
    put_num(o, "eye_relief", s->eye_relief);
    put_v3(o, "ads_rot", s->ads_rot);
    cJSON *pr = cJSON_AddObjectToObject(o, "procedural");
    put_num(pr, "ads_scale", s->ads_scale);
    cJSON *lg = cJSON_AddObjectToObject(pr, "look_lag");
    put_num(lg, "rot", s->lag_rot);
    put_num(lg, "pos", s->lag_pos);
    put_num(lg, "max", s->lag_max);
    put_num(lg, "stiffness", s->lag_k);
    put_num(lg, "damping", s->lag_d);
    cJSON *sw = cJSON_AddObjectToObject(pr, "sway");
    put_num(sw, "rot", s->sway_rot);
    put_num(sw, "freq", s->sway_freq);
    put_num(sw, "breath", s->breath_pos);
    put_num(sw, "breath_freq", s->breath_freq);
    cJSON *bb = cJSON_AddObjectToObject(pr, "bob");
    put_num(bb, "pos", s->bob_pos);
    put_num(bb, "rot", s->bob_rot);
    put_num(bb, "freq", s->bob_freq);
    put_num(pr, "kick_scale", s->kick_scale);
    cJSON *co = cJSON_AddObjectToObject(o, "collision");
    put_num(co, "back", s->col_back);
    put_num(co, "up", s->col_up);
    put_num(co, "pitch", s->col_pitch);
    put_num(co, "curve", s->col_curve);
    put_num(co, "in_speed", s->col_in_speed);
    put_num(co, "out_speed", s->col_out_speed);
    put_num(co, "radius", s->col_radius);
    put_num(o, "clavicle_gain", s->clavicle_gain);
    put_num(o, "reach_pull", s->reach_pull);
    put_num(o, "pitch_soft", s->pitch_soft);
    put_num(o, "pitch_follow", s->pitch_follow);
    put_num(o, "ads_pitch_soft", s->ads_pitch_soft);
    put_num(o, "ads_pitch_follow", s->ads_pitch_follow);
    cJSON *body = cJSON_AddObjectToObject(o, "body");
    put_num(body, "twist", s->twist);
    put_v3(body, "clavicle_l", s->clav_l);
    put_v3(body, "clavicle_r", s->clav_r);
    char *txt = cJSON_Print(o);
    cJSON_Delete(o);
    return txt;
}

// ------------------------------------------------------------------ evaluation

void holdpoint_reset(HoldPointState *st) {
    memset(st, 0, sizeof(*st));
    st->state = -1;
    st->style_key = -1;
    st->off_rot = quat_identity();
    st->base = xf_identity();
}

int holdpoint_pick_state(const HoldPointInput *in) {
    if (in->empty || !in->has_item) return HP_EMPTY;
    if (in->inspect) return HP_INSPECT;
    if (in->sprint && !in->ads) return HP_SPRINT;
    if (in->ads) return HP_ADS;
    if (in->prone) return HP_PRONE;
    if (in->crouch) return HP_CROUCH;
    return HP_HIP;
}

static HpPose pose_for(const HoldPointStyle *s, const HoldPointOverride *o, int state) {
    HpPose p = s->state[state];
    if (o && o->any && o->state[state].set) p = o->state[state];
    return p;
}

static float g_ads_relief_scale = 1.0f;
void holdpoint_set_ads_relief_scale(float k) { g_ads_relief_scale = clampf(k, 0.2f, 1.5f); }
static float g_look_lag_scale = 1.0f;
void holdpoint_set_look_lag_scale(float k) { g_look_lag_scale = clampf(k, 0.0f, 2.0f); }
float holdpoint_ads_relief(float relief) { return fmaxf(relief > 0.12f ? 0.10f : 0.055f, relief * g_ads_relief_scale); }

Xform holdpoint_ads_item(const HoldPointStyle *s, const HoldPointOverride *o, vec3 sight) {
    float relief = (o && o->eye_relief > 0) ? o->eye_relief : s->eye_relief;
    if (o) relief += o->eye_relief_add;
    // Iron sights / red dots sit at the stock weld (24-34 cm), scopes at their eye relief (8-9 cm):
    // ads_eye_relief_scale brings either towards the eye so the sight picture fills more of the
    // view (floors keep the ocular / rear sight in front of the near plane).
    relief = holdpoint_ads_relief(relief);
    quat r = hp_euler_deg(s->ads_rot);
    // item * sight = (0, 0, -relief) with the item's barrel along the view axis.
    vec3 t = v3_sub(v3(0, 0, -relief), quat_rotate(r, sight));
    return xf_make(t, r);
}

static Xform state_hold(const HoldPointStyle *s, const HoldPointOverride *o, int state, const HoldPointInput *in) {
    if (state == HP_ADS) return xf_mul(holdpoint_ads_item(s, o, in->sight), in->anchor);
    HpPose p = pose_for(s, o, state);
    return xf_make(hp_cam_pos(p.pos), hp_euler_deg(p.rot));
}

// Rotation vector (axis * angle) <-> quaternion.
static vec3 rotvec(quat q) {
    if (q.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    float s = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-7f) return v3(q.x * 2.0f, q.y * 2.0f, q.z * 2.0f);
    float a = 2.0f * atan2f(s, q.w);
    return v3(q.x / s * a, q.y / s * a, q.z / s * a);
}
static quat from_rotvec(vec3 v) {
    float a = v3_len(v);
    if (a < 1e-7f) return quat_norm((quat){ v.x * 0.5f, v.y * 0.5f, v.z * 0.5f, 1.0f });
    return quat_axis_angle(v3_scale(v, 1.0f / a), a);
}

// Inertialization (D. Bollo, GDC 2018): an offset x0 moving at v0 decays to zero over T with zero
// velocity and acceleration at the end - continuous position AND velocity at every switch.
static void inert_init(float x0, float v0, float T, float *ox0, float *ov0, float *oT) {
    float sign = x0 < 0 ? -1.0f : 1.0f;
    x0 *= sign;
    v0 *= sign;
    // Velocity is kept as is (also when moving away from the target): a small overshoot is
    // preferred over a velocity kink. Only very fast approaches shorten the blend.
    if (v0 < 0 && x0 > 1e-9f) T = fminf(T, fmaxf(-5.0f * x0 / v0, T * 0.5f));
    *ox0 = x0 * sign;
    *ov0 = v0 * sign;
    *oT = fmaxf(T, 1e-4f);
}
static float inert_eval(float x0, float v0, float T, float t) {
    if (t >= T) return 0.0f;
    float sign = x0 < 0 ? -1.0f : 1.0f;
    x0 *= sign;
    v0 *= sign;
    float T2 = T * T, T3 = T2 * T, T4 = T3 * T, T5 = T4 * T;
    float a0 = (-8.0f * v0 * T - 20.0f * x0) / T2;
    float A = -(a0 * T2 + 6.0f * v0 * T + 12.0f * x0) / (2.0f * T5);
    float B = (3.0f * a0 * T2 + 16.0f * v0 * T + 30.0f * x0) / (2.0f * T4);
    float C = -(3.0f * a0 * T2 + 12.0f * v0 * T + 20.0f * x0) / (2.0f * T3);
    float t2 = t * t, t3 = t2 * t;
    return sign * (A * t3 * t2 + B * t2 * t2 + C * t3 + a0 * 0.5f * t2 + v0 * t + x0);
}

// Smootherstep easing: zero velocity and acceleration at both ends (C2 continuous blends).
static float ease5(float t) {
    t = clampf(t, 0, 1);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float spring1(float x, float *v, float target, float k, float d, float dt) {
    // Semi-implicit Euler, sub-stepped for stability at large dt.
    int n = dt > 1.0f / 120.0f ? (int)ceilf(dt * 120.0f) : 1;
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        *v += ((target - x) * k - *v * d) * h;
        x += *v * h;
    }
    return x;
}

void holdpoint_eval(HoldPointState *st, const HoldPointInput *in, HoldPointOutput *out) {
    const HoldPointStyle *s = in->style;
    const HoldPointOverride *o = in->ovr;
    float dt = fmaxf(in->dt, 0.0f);
    memset(out, 0, sizeof(*out));
    int state = holdpoint_pick_state(in);
    Xform target = state_hold(s, o, state, in);
    // ---- inertialized state / item blend
    if (!st->init) {
        st->init = true;
        st->state = state;
        st->style_key = in->style_key;
        st->base = target;
        st->blend_t = st->blend_dur = 1;
        st->off_pos = v3(0, 0, 0);
        st->off_rot = quat_identity();
        st->prev_yaw = in->aim_yaw;
        st->prev_pitch = in->aim_pitch;
        st->pitch_w = state == HP_ADS ? 0.0f : 1.0f;
    }
    if (state != st->state || in->style_key != st->style_key) {
        // Offset (and its velocity) from the new target to where the hold point is now, decayed
        // to zero: position and velocity stay continuous through any switch, even mid-blend.
        st->blend_dur = fmaxf(0.01f, s->blend_in[state]);
        vec3 op = v3_sub(st->base.t, target.t);
        vec3 orv = rotvec(quat_mul(st->base.r, quat_conj(target.r)));
        float x0[6] = { op.x, op.y, op.z, orv.x, orv.y, orv.z };
        float v0[6] = { st->base_vel.x, st->base_vel.y, st->base_vel.z, st->base_avel.x, st->base_avel.y, st->base_avel.z };
        for (int i = 0; i < 6; i++) inert_init(x0[i], v0[i], st->blend_dur, &st->ix0[i], &st->iv0[i], &st->iT[i]);
        st->blend_t = 0;
        st->state = state;
        st->style_key = in->style_key;
    }
    st->blend_t = fminf(st->blend_dur, st->blend_t + dt);
    float k = 1.0f - ease5(st->blend_t / st->blend_dur);
    float off[6];
    for (int i = 0; i < 6; i++) off[i] = st->blend_t >= st->blend_dur ? 0.0f : inert_eval(st->ix0[i], st->iv0[i], st->iT[i], st->blend_t);
    Xform base;
    base.t = v3_add(target.t, v3(off[0], off[1], off[2]));
    base.r = quat_norm(quat_mul(from_rotvec(v3(off[3], off[4], off[5])), target.r));
    base.s = v3(1, 1, 1);
    if (dt > 1e-6f) {
        st->base_vel = v3_scale(v3_sub(base.t, st->base.t), 1.0f / dt);
        st->base_avel = v3_scale(rotvec(quat_mul(base.r, quat_conj(st->base.r))), 1.0f / dt);
    }
    st->base = base;
    out->state = state;
    out->blend = 1.0f - k;

    // Item (camera space) before the procedural layer; pivot in camera space.
    Xform inv_anchor = xf_inverse(in->anchor);
    Xform item = xf_mul(base, inv_anchor);
    // Soft pitch limit: looking steeply up / down the item stops following the view fully (arms
    // stay in reach); none in ADS (the sight must stay on the view axis), eased with the blend.
    float pw_target = state == HP_ADS ? 0.0f : 1.0f;
    st->pitch_w += (pw_target - st->pitch_w) * (1.0f - expf(-dt / fmaxf(0.02f, s->blend_in[state] * 0.35f)));
    {
        float pdeg = in->aim_pitch * DG_RAD2DEG, a = fabsf(pdeg);
        float soft = lerpf(s->ads_pitch_soft, s->pitch_soft, st->pitch_w), fol = lerpf(s->ads_pitch_follow, s->pitch_follow, st->pitch_w);
        if (a > soft) {
            float limited = soft + (a - soft) * fol;
            float delta = pdeg > 0 ? limited - a : a - limited;   // rotation back towards level (deg)
            quat rp = quat_axis_angle(v3(1, 0, 0), delta * DG_DEG2RAD);
            item = xf_mul(xf_make(v3(0, 0, 0), rp), item);
        }
    }
    vec3 pivot_item = (o && o->any && o->has_pivot) ? o->pivot : in->pivot;
    vec3 pivot = xf_point(item, pivot_item);

    // ---- procedural layer (around the pivot)
    // In ADS the sight stays on the view axis: item lag / sway / bob fade out with the blend
    // (ads_scale, 0 by default); scope sway comes from the aim sway, which moves the view itself.
    float ads_w = state == HP_ADS ? s->ads_scale + (1.0f - s->ads_scale) * k : 1.0f;   // eases with the blend
    if (in->static_pose) ads_w = 0.0f;
    float ads_k = ads_w * in->sway_scale;
    float lag_k = ads_w * in->sway_scale * g_look_lag_scale;
    float wy = dt > 1e-6f ? wrap_angle(in->aim_yaw - st->prev_yaw) / dt : 0.0f;
    float wp = dt > 1e-6f ? (in->aim_pitch - st->prev_pitch) / dt : 0.0f;
    st->prev_yaw = in->aim_yaw;
    st->prev_pitch = in->aim_pitch;
    // Look lag: the item trails the view rotation (muzzle behind the turn).
    vec2 lag_t = v2(clampf(-wp * s->lag_rot, -s->lag_max, s->lag_max) * lag_k, clampf(-wy * s->lag_rot, -s->lag_max, s->lag_max) * lag_k);
    st->lag.x = spring1(st->lag.x, &st->lag_v.x, lag_t.x, s->lag_k, s->lag_d, dt);
    st->lag.y = spring1(st->lag.y, &st->lag_v.y, lag_t.y, s->lag_k, s->lag_d, dt);
    st->t += dt;
    // Idle sway (figure-eight) + breathing.
    float sw = s->sway_rot * ads_k * g_look_lag_scale * 2.0f;   // idle sway follows wpn_look_lag (0.12 -> a quarter)
    vec3 sway_rot = v3(sinf(st->t * s->sway_freq * 2.0f * DG_PI) * sw * 0.6f, sinf(st->t * s->sway_freq * DG_PI) * sw, 0);
    vec3 breath = v3(0, sinf(st->t * s->breath_freq * 2.0f * DG_PI) * s->breath_pos * ads_k, 0);
    // Movement bob (the camera's own bob comes from the head animation).
    float speed = sqrtf(in->vel_cam.x * in->vel_cam.x + in->vel_cam.z * in->vel_cam.z);
    float bob_target = in->grounded ? clampf(speed / 6.0f, 0, 1) : 0.0f;
    st->bob_amt += (bob_target - st->bob_amt) * (1.0f - expf(-8.0f * dt));
    st->bob_phase += dt * (4.5f + speed * 1.1f) * s->bob_freq;
    // Walking leaves the item still (bob_scale, default 0); sprinting keeps its bob.
    float bob = st->bob_amt * ads_w * (state == HP_SPRINT ? in->sway_scale : in->bob_scale);
    vec3 bob_pos = v3(sinf(st->bob_phase) * s->bob_pos * bob, -fabsf(cosf(st->bob_phase)) * s->bob_pos * 1.2f * bob, 0);
    vec3 bob_rot = v3(0, 0, sinf(st->bob_phase) * s->bob_rot * bob);
    // Recoil item spring (from the recoil system).
    vec3 kick_pos = v3_scale(in->kick_pos, s->kick_scale * (in->static_pose ? 0.0f : 1.0f));
    vec3 kick_rot = v3_scale(in->kick_rot, s->kick_scale * (in->static_pose ? 0.0f : 1.0f));
    if (in->static_pose) { st->lag = v2(0, 0); st->lag_v = v2(0, 0); }
    vec3 rot_deg = v3(st->lag.x + sway_rot.x + bob_rot.x + kick_rot.x, st->lag.y + sway_rot.y + bob_rot.y - kick_rot.y, sway_rot.z + bob_rot.z + kick_rot.z);
    vec3 pos = v3_add(v3_add(breath, bob_pos), kick_pos);
    pos = v3_add(pos, v3(st->lag.y * s->lag_pos * 10.0f, st->lag.x * s->lag_pos * 10.0f, 0));
    quat R = hp_euler_deg(rot_deg);
    item.r = quat_norm(quat_mul(R, item.r));
    item.t = v3_add(v3_add(pivot, quat_rotate(R, v3_sub(item.t, pivot))), pos);

    // ---- weapon collision: cast along the item from the hold point, pull back / up on hit
    float want = 0;
    if (in->raycast && in->has_item && in->length > 0.05f) {
        Xform hold = xf_mul(item, in->anchor);
        vec3 fwd_cam = quat_rotate(item.r, v3(0, 0, -1));
        vec3 a = xf_point(in->cam_world, hold.t);
        vec3 d = v3_scale(quat_rotate(in->cam_world.r, fwd_cam), in->length + s->col_radius);
        float frac;
        vec3 hp, hn;
        out->cast_from = a;
        out->cast_to = v3_add(a, d);
        if (in->raycast(in->ray_ctx, a, d, &frac, &hp, &hn) && frac < 1.0f) {
            want = 1.0f - frac;
            out->cast_hit = true;
        }
    }
    float rate = want > st->pull ? s->col_in_speed : s->col_out_speed;
    st->pull += (want - st->pull) * (1.0f - expf(-rate * dt));
    float pull = powf(clampf(st->pull, 0, 1), s->col_curve);
    if (pull > 1e-5f) {
        quat Rc = quat_axis_angle(v3(1, 0, 0), s->col_pitch * pull * DG_DEG2RAD);
        vec3 pv = xf_point(item, pivot_item);
        item.r = quat_norm(quat_mul(Rc, item.r));
        item.t = v3_add(pv, quat_rotate(Rc, v3_sub(item.t, pv)));
        item.t = v3_add(item.t, v3(0, s->col_up * pull, s->col_back * pull));
    }
    out->pull = st->pull;
    out->item = item;
    out->hold = xf_mul(item, in->anchor);
    out->pivot = xf_point(item, pivot_item);
    Xform hip = state_hold(s, o, HP_HIP, in);
    out->offset = v3_sub(out->hold.t, hip.t);
    out->ads_item = holdpoint_ads_item(s, o, in->sight);
}
