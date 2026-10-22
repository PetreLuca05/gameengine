// Viewmodel sway layers (see weapon_sway.h, docs/FIRE_ANIM.md "Viewmodel sway").
#include "weapon_sway.h"
#include "cJSON.h"
#include <math.h>
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
    c->ads_rot[WSW_LOOK] = 0.2f;
    c->ads_pos[WSW_LOOK] = 0.1f;
    c->ads_rot[WSW_MOVE] = 0.2f;
    c->ads_pos[WSW_MOVE] = 0.2f;
    c->ads_rot[WSW_BOB] = 0.12f;
    c->ads_pos[WSW_BOB] = 0.12f;
    c->scale = 1.0f;
}

static const cJSON *jget(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
static void jf(const cJSON *o, const char *k, float *v) {
    const cJSON *n = jget(o, k);
    if (cJSON_IsNumber(n)) *v = (float)n->valuedouble;
}
static void jv3(const cJSON *o, const char *k, vec3 *v) {
    const cJSON *a = jget(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 3) return;
    float f[3];
    for (int i = 0; i < 3; i++) {
        const cJSON *e = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(e)) return;
        f[i] = (float)e->valuedouble;
    }
    *v = v3(f[0], f[1], f[2]);
}

void weapon_sway_parse(WeaponSwayConfig *c, const cJSON *j) {
    if (!j) return;
    const cJSON *l = jget(j, "look");
    jf(l, "filter_hz", &c->look_filter_hz);
    jf(l, "yaw", &c->look_yaw);
    jf(l, "pitch", &c->look_pitch);
    jf(l, "roll", &c->look_roll);
    jf(l, "pos", &c->look_pos);
    jf(l, "max_rot", &c->look_max_rot);
    jf(l, "max_pos", &c->look_max_pos);
    jf(l, "stiffness", &c->look_k);
    jf(l, "damping_ratio", &c->look_zeta);
    const cJSON *m = jget(j, "move");
    jf(m, "roll", &c->move_roll);
    jf(m, "strafe_pos", &c->move_strafe_pos);
    jf(m, "fwd_pitch", &c->move_fwd_pitch);
    jf(m, "fwd_pos", &c->move_fwd_pos);
    jf(m, "max_speed", &c->move_max_speed);
    jf(m, "stiffness", &c->move_k);
    jf(m, "damping_ratio", &c->move_zeta);
    jf(m, "accel_threshold", &c->move_accel_threshold);
    jf(m, "accel_impulse", &c->move_accel_impulse);
    const cJSON *b = jget(j, "bob");
    jf(b, "walk_speed", &c->walk_speed);
    jf(b, "freq", &c->bob_freq);
    jf(b, "x", &c->bob_x);
    jf(b, "y", &c->bob_y);
    jf(b, "roll", &c->bob_roll);
    jf(b, "pitch", &c->bob_pitch);
    jf(b, "yaw", &c->bob_yaw);
    jf(b, "attack", &c->bob_attack);
    jf(b, "decay", &c->bob_decay);
    jf(b, "crouch", &c->bob_crouch);
    jf(b, "sprint_mult", &c->sprint_bob_mult);
    jf(b, "sprint_freq_mult", &c->sprint_freq_mult);
    jf(b, "sprint_roll", &c->sprint_roll);
    jf(b, "sprint_yaw", &c->sprint_yaw);
    const cJSON *s = jget(j, "sprint_pose");
    jv3(s, "rot", &c->sprint_rot);
    jv3(s, "pos", &c->sprint_pos);
    jv3(s, "tactical_rot", &c->tac_rot);
    jv3(s, "tactical_pos", &c->tac_pos);
    jf(s, "in", &c->sprint_in);
    jf(s, "out", &c->sprint_out);
    static const char *keys[WSW_LAYERS] = { "look", "move", "bob" };
    const cJSON *a = jget(j, "ads");
    for (int i = 0; i < WSW_LAYERS; i++) {
        const cJSON *e = jget(a, keys[i]);   // [rotation, position]
        if (cJSON_IsArray(e) && cJSON_GetArraySize(e) == 2) {
            c->ads_rot[i] = (float)cJSON_GetArrayItem(e, 0)->valuedouble;
            c->ads_pos[i] = (float)cJSON_GetArrayItem(e, 1)->valuedouble;
        }
    }
    jf(j, "scale", &c->scale);
}

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

static float ads_mix(float m, float ads) { return 1.0f + (m - 1.0f) * saturate(ads); }

// 1. Look sway: the item trails the view's rotation (rates in deg/s, filtered), overshooting slightly.
void weapon_sway_look(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, vec3 *rot, vec3 *pos) {
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
    *rot = v3_scale(s->look_rot.x, ads_mix(c->ads_rot[WSW_LOOK], in->ads_blend));
    *pos = v3_scale(s->look_pos.x, ads_mix(c->ads_pos[WSW_LOOK], in->ads_blend));
}

// 2. Movement sway: roll into the strafe and shift against it, push back / dip moving forward; sudden
// starts and stops kick the spring (the gun's inertia: it settles after you stop).
void weapon_sway_move(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, vec3 *rot, vec3 *pos) {
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
    *rot = v3_scale(s->move_rot.x, ads_mix(c->ads_rot[WSW_MOVE], in->ads_blend));
    *pos = v3_scale(s->move_pos.x, ads_mix(c->ads_pos[WSW_MOVE], in->ads_blend));
}

static float ease(float t) {
    t = saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

// 3. Walk / sprint bob + the sprint pose. The phase follows the distance walked on the ground (frozen
// in the air and standing still: no snap), the amplitude eases in / decays out.
void weapon_sway_bob(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, vec3 *rot, vec3 *pos, float *sprint_w) {
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
    if (moving) s->bob_phase = fmodf(s->bob_phase + 2.0f * DG_PI * freq * in->speed * dt, 2.0f * DG_PI);
    float A = s->bob_amt * (1.0f + (c->sprint_bob_mult - 1.0f) * w);
    float ph = s->bob_phase;
    float sn = sinf(ph), dip = (cosf(2.0f * ph) - 1.0f) * 0.5f;   // dip: 0 .. -1 twice per cycle (the footsteps)
    float roll = c->bob_roll + (c->sprint_roll - c->bob_roll) * w, yaw = c->bob_yaw + (c->sprint_yaw - c->bob_yaw) * w;
    vec3 br = v3(dip * c->bob_pitch * A, sn * yaw * A, sn * roll * A);
    vec3 bp = v3(sn * c->bob_x * A, dip * c->bob_y * A, 0.0f);
    br = v3_scale(br, ads_mix(c->ads_rot[WSW_BOB], in->ads_blend));
    bp = v3_scale(bp, ads_mix(c->ads_pos[WSW_BOB], in->ads_blend));
    // The pose itself (sprinting never aims; aiming drops it with the sprint). A weapon's own run clip
    // carrying the gun already lowers it (bob_scale 0): no second pose on top.
    float carry = saturate(in->bob_scale);
    vec3 pr = v3_scale(v3_add(v3_scale(c->sprint_rot, w * (1.0f - wt)), v3_scale(c->tac_rot, wt)), carry);
    vec3 pp = v3_scale(v3_add(v3_scale(c->sprint_pos, w * (1.0f - wt)), v3_scale(c->tac_pos, wt)), carry);
    *rot = v3_add(br, pr);
    *pos = v3_add(bp, pp);
    *sprint_w = w;
}

void weapon_sway_update(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, SwayOutput *out) {
    memset(out, 0, sizeof(*out));
    dt = clampf(dt, 0.0f, 0.1f);
    s->t += dt;
    weapon_sway_look(s, c, in, dt, &out->layer_rot[WSW_LOOK], &out->layer_pos[WSW_LOOK]);
    weapon_sway_move(s, c, in, dt, &out->layer_rot[WSW_MOVE], &out->layer_pos[WSW_MOVE]);
    weapon_sway_bob(s, c, in, dt, &out->layer_rot[WSW_BOB], &out->layer_pos[WSW_BOB], &out->sprint_w);
    s->init = true;
    vec3 r = v3(0, 0, 0), p = v3(0, 0, 0);
    for (int i = 0; i < WSW_LAYERS; i++) {
        r = v3_add(r, out->layer_rot[i]);
        p = v3_add(p, out->layer_pos[i]);
    }
    out->rot_deg = v3_scale(r, c->scale);
    out->pos_offset = v3_scale(p, c->scale);
    out->rot_offset = quat_euler(out->rot_deg.y * DG_DEG2RAD, out->rot_deg.x * DG_DEG2RAD, out->rot_deg.z * DG_DEG2RAD);
    out->fov_delta = 0.0f;
}
