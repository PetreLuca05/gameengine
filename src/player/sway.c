#define DG_DETMATH   // cross-platform bit-identical math (core/dgmath.h)
#include "sway.h"
#include "core/detmath.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sway_profile_defaults(SwayProfile *p) {
    memset(p, 0, sizeof(*p));
    p->sway_rot = 0.35f;
    p->sway_freq = 0.55f;
    p->breath_pos = 0.0025f;
    p->breath_freq = 0.25f;
    p->lag_rot = 1.6f;
    p->lag_pos = 0.004f;
    p->lag_max = 5.0f;
    p->lag_k = 140.0f;
    p->lag_d = 18.0f;
    p->bob_pos = 0.012f;
    p->bob_rot = 1.2f;
    p->bob_freq = 1.0f;
    p->sprint_bob = 1.4f;
    p->land_k = 110.0f;
    p->land_d = 13.0f;
    p->land_gain = 0.06f;
    p->land_max = 0.9f;
    p->land_pitch = 55.0f;
    for (int i = 0; i < SWAY_COMPONENTS; i++) p->hip[i] = 1.0f;
    p->ads[SWAY_BREATHING] = 0.35f;
    p->ads[SWAY_LOOK_LAG] = 0.25f;
    p->ads[SWAY_BOB] = 0.15f;
    p->ads[SWAY_LANDING] = 0.5f;
    p->hold_breath_mult = 0.1f;
    p->crouch = 0.75f;
    p->prone = 0.55f;
    p->lever = 0.35f;
    p->aim_sway = 0.05f;
}

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = jget(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

void sway_profile_parse(SwayProfile *p, const cJSON *j) {
    if (!j) return;
    const cJSON *b = jget(j, "breathing");
    p->sway_rot = jf(b, "rot", p->sway_rot);
    p->sway_freq = jf(b, "freq", p->sway_freq);
    p->breath_pos = jf(b, "pos", p->breath_pos);
    p->breath_freq = jf(b, "pos_freq", p->breath_freq);
    const cJSON *l = jget(j, "look_lag");
    p->lag_rot = jf(l, "rot", p->lag_rot);
    p->lag_pos = jf(l, "pos", p->lag_pos);
    p->lag_max = jf(l, "max", p->lag_max);
    p->lag_k = jf(l, "stiffness", p->lag_k);
    p->lag_d = jf(l, "damping", p->lag_d);
    const cJSON *o = jget(j, "bob");
    p->bob_pos = jf(o, "pos", p->bob_pos);
    p->bob_rot = jf(o, "rot", p->bob_rot);
    p->bob_freq = jf(o, "freq", p->bob_freq);
    p->sprint_bob = jf(o, "sprint", p->sprint_bob);
    const cJSON *n = jget(j, "landing");
    p->land_k = jf(n, "stiffness", p->land_k);
    p->land_d = jf(n, "damping", p->land_d);
    p->land_gain = jf(n, "gain", p->land_gain);
    p->land_max = jf(n, "max", p->land_max);
    p->land_pitch = jf(n, "pitch", p->land_pitch);
    static const char *keys[SWAY_COMPONENTS] = { "breathing", "look_lag", "bob", "landing" };
    const cJSON *h = jget(j, "hip"), *a = jget(j, "ads");
    for (int i = 0; i < SWAY_COMPONENTS; i++) {
        p->hip[i] = jf(h, keys[i], p->hip[i]);
        p->ads[i] = jf(a, keys[i], p->ads[i]);
    }
    p->hold_breath_mult = jf(j, "hold_breath_mult", p->hold_breath_mult);
    const cJSON *st = jget(j, "stance");
    p->crouch = jf(st, "crouch", p->crouch);
    p->prone = jf(st, "prone", p->prone);
    p->lever = jf(j, "ads_lever", p->lever);
    p->aim_sway = jf(j, "aim_sway", p->aim_sway);
}

bool sway_profile_load(SwayProfile *p, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!buf) return false;
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return false;
    sway_profile_parse(p, j);
    cJSON_Delete(j);
    return true;
}

static SwayProfile g_profile;
static bool g_profile_set;
const SwayProfile *sway_global(void) {
    if (!g_profile_set) { sway_profile_defaults(&g_profile); g_profile_set = true; }
    return &g_profile;
}
void sway_set_global(const SwayProfile *p) { g_profile = *p; g_profile_set = true; }

void sway_reset(SwayState *s) { memset(s, 0, sizeof(*s)); }

static float wrap_pi(float a) {
    while (a > DG_PI) a -= 2.0f * DG_PI;
    while (a < -DG_PI) a += 2.0f * DG_PI;
    return a;
}
static float spring1(float x, float *v, float target, float k, float d, float dt) {
    *v += (k * (target - x) - d * *v) * dt;
    return x + *v * dt;
}

void sway_eval(SwayState *st, const SwayProfile *p, const SwayInput *in, SwayOut *out) {
    float dt = in->dt;
    float S = in->static_pose ? 0.0f : in->sway_scale;
    float optic = in->optic > 0 ? in->optic : 1.0f;
    float breath_k = in->breath_k > 0 ? in->breath_k : 1.0f;
    float stance = in->stance > 0 ? in->stance : 1.0f;
    // Hold breath: steadies quickly; letting go (or running out) gives a short shaky overshoot.
    if (st->breath_s <= 0.0f) st->breath_s = 1.0f;
    if (st->breath_s < 0.5f && breath_k > 0.5f) st->shake = 1.0f;
    st->breath_s += (breath_k - st->breath_s) * (1.0f - dm_expf(-dt / 0.2f));
    st->shake = fmaxf(0.0f, st->shake - dt / 0.9f);
    float wy = dt > 1e-6f ? wrap_pi(in->aim_yaw - st->prev_yaw) / dt : 0.0f;
    float wp = dt > 1e-6f ? (in->aim_pitch - st->prev_pitch) / dt : 0.0f;
    st->prev_yaw = in->aim_yaw;
    st->prev_pitch = in->aim_pitch;
    // Look lag: the item trails the view rotation (muzzle behind the turn), grows with the mouse speed.
    vec2 lag_t = v2(clampf(-wp * p->lag_rot, -p->lag_max, p->lag_max) * S, clampf(-wy * p->lag_rot, -p->lag_max, p->lag_max) * S);
    st->lag.x = spring1(st->lag.x, &st->lag_v.x, lag_t.x, p->lag_k, p->lag_d, dt);
    st->lag.y = spring1(st->lag.y, &st->lag_v.y, lag_t.y, p->lag_k, p->lag_d, dt);
    st->t = in->phase_valid ? (float)fmod(in->clock, 3600.0) : st->t + dt;
    // Breathing: slow layered sines (figure-eight + breath), plus the extra wobble.
    float sw = p->sway_rot * S;
    vec3 breath_rot = v3(dm_sinf(st->t * p->sway_freq * 2.0f * DG_PI) * sw * 0.6f + dm_sinf(st->t * 0.37f * 2.0f * DG_PI + 1.7f) * sw * 0.25f,
                         dm_sinf(st->t * p->sway_freq * DG_PI) * sw + dm_sinf(st->t * 0.23f * 2.0f * DG_PI + 0.4f) * sw * 0.2f, 0);
    float wob = (in->wobble + st->shake * 0.8f) * S;
    breath_rot.x += (dm_sinf(st->t * 2.3f * 2.0f * DG_PI) * 0.5f + dm_sinf(st->t * 3.7f * 2.0f * DG_PI + 1.0f) * 0.35f) * wob;
    breath_rot.y += (dm_sinf(st->t * 2.9f * 2.0f * DG_PI + 0.5f) * 0.45f + dm_sinf(st->t * 4.3f * 2.0f * DG_PI + 2.1f) * 0.3f) * wob;
    vec3 breath_pos = v3(0, dm_sinf(st->t * p->breath_freq * 2.0f * DG_PI) * p->breath_pos * S, 0);
    // Movement bob, scaled with the speed (the camera's own bob comes from the head animation).
    float speed = sqrtf(in->vel_cam.x * in->vel_cam.x + in->vel_cam.z * in->vel_cam.z);
    float bob_target = in->grounded ? clampf(speed / 6.0f, 0, 1) : 0.0f;
    st->bob_amt += (bob_target - st->bob_amt) * (1.0f - dm_expf(-8.0f * dt));
    if (in->phase_valid) st->bob_phase = in->stride * p->bob_freq;
    else st->bob_phase += dt * (4.5f + speed * 1.1f) * p->bob_freq;
    float bob = st->bob_amt * S * (in->sprint ? p->sprint_bob : in->bob_scale);
    vec3 bob_pos = v3(dm_sinf(st->bob_phase) * p->bob_pos * bob, -fabsf(dm_cosf(st->bob_phase)) * p->bob_pos * 1.2f * bob, 0);
    vec3 bob_rot = v3(0, 0, dm_sinf(st->bob_phase) * p->bob_rot * bob);
    // Landing: a dip on touchdown, stronger for harder landings, settling on a spring.
    if (!in->grounded) st->air_vy = fminf(st->air_vy, in->vel_world_y);
    if (in->grounded && !st->was_grounded && st->air_vy < -1.5f) st->land_v += clampf(-st->air_vy * p->land_gain, 0.0f, p->land_max) * S;
    if (in->grounded) st->air_vy = 0.0f;
    st->was_grounded = in->grounded;
    st->land = spring1(st->land, &st->land_v, 0.0f, p->land_k, p->land_d, dt);
    if (in->static_pose) { st->lag = v2(0, 0); st->lag_v = v2(0, 0); st->land = st->land_v = 0; }
    vec3 land_pos = v3(0, -st->land, -st->land * 0.3f);
    vec3 land_rot = v3(-st->land * p->land_pitch, 0, 0);
    vec3 lag_rot = v3(st->lag.x, st->lag.y, 0);
    vec3 lag_pos = v3(st->lag.y * p->lag_pos * 10.0f, st->lag.x * p->lag_pos * 10.0f, 0);
    // Hip: everything about the hip anchor (look lag x the user's wpn_look_lag).
    const float *mh = p->hip;
    float am = in->ads_mult > 0 ? in->ads_mult : 1.0f;
    float ma[SWAY_COMPONENTS];
    for (int i = 0; i < SWAY_COMPONENTS; i++) ma[i] = p->ads[i] * am;
    float gl = in->look_lag_scale;
    out->hip_rot = v3_scale(v3_add(v3_add(v3_scale(breath_rot, mh[0]), v3_scale(lag_rot, mh[1] * gl)), v3_add(v3_scale(bob_rot, mh[2]), v3_scale(land_rot, mh[3]))), stance);
    out->hip_pos = v3_scale(v3_add(v3_add(v3_scale(breath_pos, mh[0]), v3_scale(lag_pos, mh[1] * gl)), v3_add(v3_scale(bob_pos, mh[2]), v3_scale(land_pos, mh[3]))), stance);
    // ADS: rotation only (about the sight), translations turned into the equivalent rotation of the
    // lever; x the optic's steadiness and the hold-breath factor.
    float lever = p->lever > 0.01f ? p->lever : 0.35f;
    vec3 as_breath = v3_add(breath_rot, v3(-breath_pos.y / lever * DG_RAD2DEG, 0, 0));
    vec3 as_bob = v3_add(bob_rot, v3(-bob_pos.y / lever * DG_RAD2DEG, bob_pos.x / lever * DG_RAD2DEG, 0));
    vec3 as_land = v3_add(land_rot, v3(-land_pos.y / lever * DG_RAD2DEG * 0.5f, 0, 0));
    float ads_k = optic * st->breath_s * stance * (1.0f + st->shake * 1.5f);
    out->ads_rot = v3_scale(v3_add(v3_add(v3_scale(as_breath, ma[0]), v3_scale(lag_rot, ma[1])), v3_add(v3_scale(as_bob, ma[2]), v3_scale(as_land, ma[3]))), ads_k);
}
