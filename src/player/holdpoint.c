#include "holdpoint.h"
#include "sway.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_state_names[HP_STATE_COUNT] = { "hip", "ads", "sprint", "crouch", "prone", "empty", "inspect", "vehicle_onehand", "phone_portrait",
                                                      "phone_landscape" };

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
    bool phone = !strncmp(s->name, "phone", 5);
    // Hip: the anchor (right grip) in front of the right shoulder, below the eye line.
    if (pistol) {
        s->state[HP_HIP] = P(0.09f, -0.17f, 0.36f, 0, 2, 0);
        s->state[HP_SPRINT] = P(0.10f, -0.32f, 0.22f, -45, 10, 15);
        s->eye_relief = 0.34f;
    } else if (melee1) {
        // The blade rises forward into the lower right of the view (as the bat's does): pitched 55
        // deg it lay sideways out of the fist, out of the frame.
        s->state[HP_HIP] = P(0.18f, -0.24f, 0.34f, 20, -5, -18);
        s->state[HP_SPRINT] = P(0.20f, -0.32f, 0.24f, -5, -5, -18);
    } else if (melee2) {
        s->state[HP_HIP] = P(0.16f, -0.25f, 0.36f, 25, 0, -12);
        s->state[HP_SPRINT] = P(0.16f, -0.30f, 0.24f, 10, 8, -15);
    } else if (small) {
        s->state[HP_HIP] = P(0.17f, -0.22f, 0.33f, -15, 0, 0);
        s->state[HP_SPRINT] = P(0.18f, -0.30f, 0.26f, -30, 0, 0);
    } else if (phone) {
        // Lowered at the chest, screen tilted up towards the eyes (lens down / forward).
        s->state[HP_HIP] = P(0.07f, -0.30f, 0.30f, -55, 8, 0);
        s->state[HP_SPRINT] = P(0.10f, -0.40f, 0.20f, -75, 10, 0);
        s->eye_relief = 0.30f;
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
    // Driving one-handed (docs/VEHICLES.md): the ADS line (sights still on the aim), arm a little
    // more extended, the gun canted in towards the centre.
    s->state[HP_VEHICLE_ONEHAND] = s->state[HP_HIP];   // the hip hold (state_hold maps it to HP_HIP)
    // Phone camera mode: the screen (sight socket) held up in front of the face, a little low and
    // right of the eye; landscape rolled 90 deg (top of the screen to the left), two-handed.
    s->state[HP_PHONE_PORTRAIT] = P(0.03f, -0.05f, 0.30f, 0, 0, 0);
    s->state[HP_PHONE_LANDSCAPE] = P(0.0f, -0.05f, 0.32f, 0, 0, 90);
    if (!s->eye_relief) s->eye_relief = 0.3f;
    for (int i = 0; i < HP_STATE_COUNT; i++) s->blend_in[i] = 0.2f;
    s->blend_in[HP_ADS] = 0.16f;
    s->blend_in[HP_SPRINT] = 0.24f;
    s->blend_in[HP_EMPTY] = 0.25f;
    s->blend_in[HP_PHONE_PORTRAIT] = s->blend_in[HP_PHONE_LANDSCAPE] = 0.22f;
    s->ads_scale = 0.25f;
    s->kick_scale = 1.0f;
    s->col_back = 0.22f;
    s->col_up = 0.05f;
    s->col_pitch = 28.0f;
    s->col_curve = 1.4f;
    s->col_in_speed = 14.0f;
    s->col_out_speed = 6.0f;
    s->col_radius = 0.025f;
    s->clavicle_gain = melee1 || melee2 || small || phone ? 30.0f : 45.0f;
    // Bladed stance: the support shoulder comes forward so the off hand reaches the handguard.
    s->twist = pistol ? 10.0f : (melee1 || small || phone ? 0.0f : (melee2 ? 12.0f : 22.0f));
    s->clav_l = (melee1 || small || phone) ? v3(0, 0, 0) : v3(0, -22.0f, pistol ? 0.0f : -6.0f);
    s->clav_r = (melee1 || small || phone) ? v3(0, 0, 0) : v3(0, 6.0f, 0);
    s->reach_pull = (melee1 || small || phone) ? 0.12f : 0.25f;
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
    // "look_lag" / "sway" / "bob" are no longer per style: one global sway profile (src/player/sway.c).
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
    if (in->vehicle_onehand) return HP_VEHICLE_ONEHAND;
    if (in->inspect) return HP_INSPECT;
    if (in->sprint && !in->ads) return HP_SPRINT;
    if (in->phone_mode) return in->phone_mode == 2 ? HP_PHONE_LANDSCAPE : HP_PHONE_PORTRAIT;
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
    if (state == HP_VEHICLE_ONEHAND) state = HP_HIP;   // driving one-handed: exactly the on-foot hip hold
    HpPose p = pose_for(s, o, state);
    if (state == HP_PHONE_PORTRAIT || state == HP_PHONE_LANDSCAPE) {
        // The screen (sight socket) at pos, the phone turned by rot: item * sight = pos.
        quat r = hp_euler_deg(p.rot);
        Xform item = xf_make(v3_sub(hp_cam_pos(p.pos), quat_rotate(r, in->sight)), r);
        return xf_mul(item, in->anchor);
    }
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

// Hold aim weight of a state: the aiming holds aim the gun at P, the others (sprint, empty hands,
// inspect, the phone) are posed as authored.
static float aim_state_w(int state) {
    return (state == HP_HIP || state == HP_ADS || state == HP_CROUCH || state == HP_PRONE || state == HP_VEHICLE_ONEHAND) ? 1.0f : 0.0f;
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
        st->sway.prev_yaw = in->aim_yaw;
        st->sway.prev_pitch = in->aim_pitch;
        st->pitch_w = (state == HP_ADS || state >= HP_PHONE_PORTRAIT) ? 0.0f : 1.0f;
    }
    if (state != st->state || in->style_key != st->style_key) {
        // Offset (and its velocity) from the new target to where the hold point is now, decayed
        // to zero: position and velocity stay continuous through any switch, even mid-blend.
        float bd = s->blend_in[state];
        if (in->kick_params.set && state == HP_ADS && in->kick_params.ads_in > 0.0f) bd = in->kick_params.ads_in;
        else if (in->kick_params.set && st->state == HP_ADS && in->kick_params.ads_out > 0.0f) bd = in->kick_params.ads_out;
        st->blend_dur = fmaxf(0.01f, bd);
        vec3 op = v3_sub(st->base.t, target.t);
        vec3 orv = rotvec(quat_mul(st->base.r, quat_conj(target.r)));
        float x0[6] = { op.x, op.y, op.z, orv.x, orv.y, orv.z };
        float v0[6] = { st->base_vel.x, st->base_vel.y, st->base_vel.z, st->base_avel.x, st->base_avel.y, st->base_avel.z };
        for (int i = 0; i < 6; i++) inert_init(x0[i], v0[i], st->blend_dur, &st->ix0[i], &st->iv0[i], &st->iT[i]);
        st->blend_t = 0;
        st->ads_from = (u8)st->state;
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
    float pw_target = (state == HP_ADS || state == HP_PHONE_PORTRAIT || state == HP_PHONE_LANDSCAPE) ? 0.0f : 1.0f;
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
    HoldKickParams kp = in->kick_params;
    if (!kp.set) { kp.ads_push = 1.0f; kp.ads_roll = 1.0f; kp.ads_misalign_deg = 0.0f; kp.kick_hip = 0; kp.kick_ads = 2; kp.sway_hip = 0; kp.sway_ads = 2; }
    // Hip anchors: the pivot socket or the hold point (the anchor socket).
    vec3 hip_kick_item = kp.kick_hip == 1 ? in->anchor.t : pivot_item;
    vec3 hip_sway_item = kp.sway_hip == 1 ? in->anchor.t : pivot_item;
    // ADS anchor blend: how far the item is into ADS (eased with the state blend, both ways).
    float ads_a = state == HP_ADS ? 1.0f - k : (st->ads_from == HP_ADS ? k : 0.0f);

    // ---- weapon collision (gameplay pose): cast along the item from the hold point, pull back / up
    // on hit. Cast from the gameplay pose (not the drawn one): sway never changes the pull.
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
    out->gp_item = item;

    // ---- hold aim (docs/AIM.md "Hold aim"): the gameplay pose turned about its pivot (hip: the pivot
    // socket; ADS: the sight, which stays on the view axis) so the barrel line passes through the aim
    // point P, at most aim_max. Weighted by the state (1 hip / ADS / crouch / prone / one-handed, 0
    // sprint / empty / inspect / phone, eased with the state blend) and faded out by the wall pull:
    // pushed against a wall, the gun points where the pull turned it.
    out->aim_hold = quat_identity();
    out->aim_pivot = xf_point(item, pivot_item);   // without the hold aim: the raw turns nothing (weight 0)
    if (in->aim_valid && in->has_item) {
        float ws = lerpf(aim_state_w(st->ads_from), aim_state_w(state), 1.0f - k);
        vec3 sight_c = xf_point(item, in->sight), pivot_c = xf_point(item, pivot_item);
        vec3 bl = v3_len2(in->barrel) > 1e-12f ? in->barrel : v3(0, 0, -1);
        AimHoldRaw raw;
        raw.muzzle = xf_point(item, in->muzzle);
        raw.barrel = v3_norm(quat_rotate(item.r, bl));
        raw.pivot = ads_a >= 1.0f ? sight_c : (ads_a <= 0.0f ? pivot_c : v3_lerp(pivot_c, sight_c, ads_a));
        raw.w = ws * (1.0f - clampf(pull, 0.0f, 1.0f));
        raw.max_rad = lerpf(in->aim_max_hip, fminf(in->aim_max_hip, in->aim_max_ads), ads_a);
        raw.valid = true;
        AimHold h = aim_hold_solve(&raw, in->aim_p);
        item.r = quat_norm(quat_mul(h.rot, item.r));
        item.t = v3_add(raw.pivot, quat_rotate(h.rot, v3_sub(item.t, raw.pivot)));
        out->aim_raw = raw;
        out->aim_hold = h.rot;
        out->aim_pivot = raw.pivot;
        out->aim_w = raw.w;
        out->aim_max = raw.max_rad;
        out->aim_angle_deg = h.angle_deg;
        out->aim_miss = h.miss;
        out->aim_clamped = h.clamped;
    }
    out->aim_item = item;

    // ---- presentation (the drawn pose only): weapon sway (docs/RECOIL.md "Weapon sway") + kick
    // Components: breathing (idle figure-eight + breath + extra wobble), look lag (trails mouse
    // turns, settles on a spring), movement bob (walk / run cycle, sprint bounce), landing dip.
    // Each has a hip and an ADS multiplier, blended with the ADS progress; ADS sway turns the gun
    // about the reticle centre only (the sight stays on the view axis, see below).
    // Sway: ONE global profile for every held item (src/player/sway.c, assets/weapons/sway_global.json);
    // nothing weapon-specific reaches it except the optic's steadiness.
    SwayInput si;
    memset(&si, 0, sizeof(si));
    si.dt = dt;
    si.phase_valid = in->phase_valid;
    si.clock = in->clock;
    si.stride = in->stride;
    si.aim_yaw = in->aim_yaw;
    si.aim_pitch = in->aim_pitch;
    si.vel_cam = in->vel_cam;
    si.vel_world_y = quat_rotate(in->cam_world.r, in->vel_cam).y;
    si.grounded = in->grounded;
    si.sprint = state == HP_SPRINT;
    si.static_pose = in->static_pose;
    si.sway_scale = in->sway_scale;
    si.bob_scale = in->bob_scale;
    si.look_lag_scale = g_look_lag_scale;
    si.optic = kp.sway_set && kp.optic_sway > 0 ? kp.optic_sway : 1.0f;
    si.breath_k = kp.sway_set && kp.breath_k > 0 ? kp.breath_k : 1.0f;
    si.stance = kp.sway_set && kp.stance > 0 ? kp.stance : 1.0f;
    si.wobble = kp.sway_set ? kp.wobble : 0.0f;
    si.ads_mult = kp.sway_set && kp.ads_sway_mult > 0 ? kp.ads_sway_mult : 1.0f;
    SwayOut so;
    sway_eval(&st->sway, in->sway ? in->sway : sway_global(), &si, &so);
    out->sway_hip_rot = so.hip_rot;
    out->sway_hip_pos = so.hip_pos;
    out->sway_ads_rot = so.ads_rot;
    vec3 hip_rot = so.hip_rot, hip_pos = so.hip_pos, ads_sway = so.ads_rot;
    // Fire animation (src/anim/fire_anim.c): the gun kick + the screen kick along the view axis.
    float ks = s->kick_scale * (in->static_pose ? 0.0f : 1.0f);
    vec3 kick_pos = v3_scale(in->kick_pos, ks);
    vec3 kick_rot = v3_scale(in->kick_rot, ks);
    // Two contributions, blended by the ADS progress (no pop when aiming mid-burst / mid-stride):
    //  hip: sway about its hip anchor, kick about its hip anchor, full translation;
    //  ADS: everything about the reticle centre, translation only along the view axis (+ roll),
    //       so the sight point stays on the view axis and the gun body moves around it.
    vec3 kick_deg = v3(kick_rot.x, -kick_rot.y, kick_rot.z);
    // Hip: sway first (about its anchor), then the kick (about its anchor).
    {
        vec3 sp = xf_point(item, hip_sway_item);
        quat Rs = hp_euler_deg(v3_scale(hip_rot, 1.0f - ads_a));
        item.r = quat_norm(quat_mul(Rs, item.r));
        item.t = v3_add(sp, quat_rotate(Rs, v3_sub(item.t, sp)));
        vec3 kpv = xf_point(item, hip_kick_item);
        quat Rk = hp_euler_deg(v3_scale(kick_deg, 1.0f - ads_a));
        item.r = quat_norm(quat_mul(Rk, item.r));
        item.t = v3_add(kpv, quat_rotate(Rk, v3_sub(item.t, kpv)));
        item.t = v3_add(item.t, v3_scale(v3_add(hip_pos, kick_pos), 1.0f - ads_a));
    }
    // ADS: about the sight point (sway before the kick, both anchored there).
    if (ads_a > 0.0f) {
        vec3 ads_rot = v3_add(ads_sway, v3(kick_deg.x, kick_deg.y, kick_deg.z * kp.ads_roll));
        if (kp.ads_misalign_deg > 0.0f) {
            // Iron sights: keep the rear sight from covering the front post.
            float m = sqrtf(ads_rot.x * ads_rot.x + ads_rot.y * ads_rot.y);
            if (m > kp.ads_misalign_deg) { float f = kp.ads_misalign_deg / m; ads_rot.x *= f; ads_rot.y *= f; }
        }
        vec3 sc = xf_point(item, in->sight);
        quat Ra = hp_euler_deg(v3_scale(ads_rot, ads_a));
        item.r = quat_norm(quat_mul(Ra, item.r));
        item.t = v3_add(sc, quat_rotate(Ra, v3_sub(item.t, sc)));
        // Push back along the view axis only (camera +z); no lateral / vertical translation. The
        // sight never comes closer than 5 cm to the eye (a hard kick would push the gun into the view).
        float push = (kick_pos.z * kp.ads_push) * ads_a;
        float sight_z = xf_point(item, in->sight).z;
        push = fminf(push, fmaxf(0.0f, -0.05f - sight_z));
        item.t.z += push;
    }
    // ---- driven rotation (phone): about the screen, so it stays where the hold point put it
    if (in->rot_w > 1e-4f) {
        vec3 sc = xf_point(item, in->sight);
        quat q = quat_nlerp(item.r, quat_norm(in->rot_cam), saturate(in->rot_w));
        quat d = quat_mul(q, quat_conj(item.r));
        item.r = q;
        item.t = v3_add(sc, quat_rotate(d, v3_sub(item.t, sc)));
    }
    out->pull = st->pull;
    out->item = item;
    out->hold = xf_mul(item, in->anchor);
    out->pivot = xf_point(item, pivot_item);
    Xform hip = state_hold(s, o, HP_HIP, in);
    out->offset = v3_sub(out->hold.t, hip.t);
    out->ads_item = holdpoint_ads_item(s, o, in->sight);
}
