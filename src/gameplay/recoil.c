#include "recoil.h"
#include <math.h>

void recoil_params_default(RecoilParams *p) {
    memset(p, 0, sizeof(*p));
    p->v_first = 1.0f; p->v_per = 0.6f; p->v_min = 0.8f; p->v_max = 1.1f;
    p->h_per = 0.2f; p->h_min = -0.3f; p->h_max = 0.3f;
    p->pattern_randomness = 0.25f;
    p->ramp_shots = 4;
    p->recovery_delay = 0.08f; p->recovery_speed = 12.0f; p->recovery_fraction = 0.85f;
    p->recovery_stop_on_input = true;
    p->cam_pitch = 1.0f; p->cam_yaw = 0.25f; p->cam_roll = 0.5f; p->cam_back = 0.015f;
    p->cam_spring = (SpringParams){ 180, 18 };
    p->vm_back = 0.04f; p->vm_up = 0.012f; p->vm_pitch = 3.5f; p->vm_yaw = 1.2f; p->vm_roll = 1.8f; p->vm_random = 0.3f;
    p->vm_spring = (SpringParams){ 220, 20 };
    p->vm_ads_scale = 0.45f;
    p->base_hip = 1.8f; p->base_ads = 0.12f; p->per_shot_bloom = 0.25f; p->bloom_max = 3.0f; p->bloom_recovery = 8.0f;
    p->s_move = 1.6f; p->s_crouch = 0.7f; p->s_prone = 0.5f; p->s_air = 2.5f;
    p->ads_mult = 0.65f; p->crouch_mult = 0.8f; p->prone_mult = 0.6f; p->moving_mult = 1.25f; p->air_mult = 1.8f;
    p->fm_single = 1.0f; p->fm_burst = 1.1f; p->fm_auto = 1.0f;
    p->rpm = 600; p->burst_rpm = 900; p->burst_count = 3;
    p->fire_modes = FIRE_SINGLE;
    p->seed_per_mag = true;
    p->sprint_to_fire = 0.15f; p->after_sprint_kick = 1.3f;
    p->ads_time = 0.18f;
    p->idle_sway = 0.08f; p->scope_sway = 0.0f; p->hold_breath_time = 4.0f;
    // Neutral hip / ADS behaviour (the old model) until recoil_type_defaults / the JSON say otherwise.
    p->hip_aim_mult = 1.0f; p->hip_kick_mult = 1.0f; p->hip_spread_mult = 1.0f;
    p->hip_bloom_mult = 1.0f; p->hip_bloom_max_mult = 1.0f; p->hip_bloom_recovery_mult = 1.0f;
    p->ads_aim_mult = 1.0f; p->ads_forward_push = 1.0f; p->ads_roll = 1.0f; p->ads_max_misalign_deg = 1.5f;
    p->anchor_hip = RA_PIVOT; p->anchor_ads = RA_SIGHT;
    p->sway_anchor_hip = RA_PIVOT; p->sway_anchor_ads = RA_SIGHT;
    for (int i = 0; i < 4; i++) p->sway_hip[i] = 1.0f;
    p->sway_ads[0] = 0.35f; p->sway_ads[1] = 0.25f; p->sway_ads[2] = 0.15f; p->sway_ads[3] = 0.5f;
    p->hold_breath_mult = 0.1f;
}

void recoil_type_defaults(RecoilParams *p, RecoilType t) {
    // Hip-fire: the camera climbs much less than in ADS and the cone opens wide instead.
    //                       aim   spread bloom  max   recovery
    static const float k[6][5] = {
        { 0.35f, 1.6f, 2.0f, 2.0f, 0.5f },   // rifle
        { 0.50f, 1.3f, 1.6f, 1.6f, 0.6f },   // pistol
        { 0.40f, 1.4f, 1.8f, 1.8f, 0.6f },   // smg
        { 0.30f, 1.8f, 2.0f, 2.2f, 0.5f },   // lmg
        { 0.40f, 1.0f, 1.2f, 1.3f, 0.8f },   // shotgun (pellet pattern does most of it)
        { 0.30f, 2.5f, 2.0f, 2.0f, 0.5f },   // sniper / dmr
    };
    int i = DG_CLAMP((int)t, 0, 5);
    p->hip_aim_mult = k[i][0];
    p->hip_spread_mult = k[i][1];
    p->hip_bloom_mult = k[i][2];
    p->hip_bloom_max_mult = k[i][3];
    p->hip_bloom_recovery_mult = k[i][4];
    p->hip_kick_mult = 1.0f;   // the gun animation keeps its full strength at the hip
    p->ads_aim_mult = 1.0f;
}

float recoil_bloom_mult(const RecoilParams *p, float a) { return lerpf(p->hip_bloom_mult, 1.0f, saturate(a)); }
float recoil_bloom_max(const RecoilParams *p, float a) { return p->bloom_max * lerpf(p->hip_bloom_max_mult, 1.0f, saturate(a)); }
float recoil_kick_mult(const RecoilParams *p, float a) { return lerpf(p->hip_kick_mult, p->vm_ads_scale, saturate(a)); }

u32 recoil_mag_seed(u32 a, u32 b) { return dg_hash_u32(a * 0x9E3779B1u ^ dg_hash_u32(b + 0x7F4A7C15u)) | 1u; }

void recoil_reset(RecoilState *s, u32 seed) {
    // Input tracking survives so countering stays correct across the reset.
    RecoilState keep = *s;
    memset(s, 0, sizeof(*s));
    s->mag_seed = seed;
    s->prev_cmd_pitch = keep.prev_cmd_pitch;
    s->prev_cmd_yaw = keep.prev_cmd_yaw;
    s->have_prev = keep.have_prev;
    s->emitted_pitch = keep.emitted_pitch;
    s->emitted_yaw = keep.emitted_yaw;
    s->bloom = keep.bloom;
    s->since_shot = 10.0f;
}

float recoil_aim_mult(const RecoilParams *p, const RecoilMods *m) {
    float k = lerpf(p->hip_aim_mult, p->ads_mult * p->ads_aim_mult, saturate(m->ads_t));
    if (m->crouched) k *= p->crouch_mult;
    if (m->prone) k *= p->prone_mult;
    k *= lerpf(1.0f, p->moving_mult, saturate(m->move01));
    if (m->airborne) k *= p->air_mult;
    k *= m->fire_mode == FM_BURST ? p->fm_burst : (m->fire_mode == FM_AUTO ? p->fm_auto : p->fm_single);
    k *= m->extra > 0 ? m->extra : 1.0f;
    k *= m->scale > 0 ? m->scale : 1.0f;
    return k;
}

RecoilKick recoil_pattern_step(const RecoilParams *p, u32 seed, int i) {
    Rng rng = rng_make(((u64)seed << 16) ^ (u64)(u32)i * 0x9E3779B97F4A7C15ULL);
    // Ramp: first shot is the big punch, then blends to the steady per-shot value.
    float base_v;
    if (i == 0) base_v = p->v_first;
    else if (p->ramp_shots > 1 && i < p->ramp_shots) base_v = lerpf(p->v_first, p->v_per, (float)i / (float)p->ramp_shots);
    else base_v = p->v_per;
    vec2 pat = v2(0.0f, 1.0f);
    if (p->pattern_count > 0) pat = p->pattern[DG_MIN(i, p->pattern_count - 1)];
    float rand_v = rng_range(&rng, p->v_min, p->v_max);
    float rand_h = rng_range(&rng, p->h_min, p->h_max);
    float r = saturate(p->pattern_randomness);
    RecoilKick k;
    k.pitch = lerpf(base_v * pat.y, base_v * rand_v, r);
    // Horizontal: the pattern gives direction, randomness wanders within [h_min, h_max].
    k.yaw = lerpf(pat.x * p->h_per, rand_h, r) + p->h_bias;
    return k;
}

RecoilKick recoil_shot(const RecoilParams *p, RecoilState *s, const RecoilMods *m) {
    // A shot during recovery: fold what was already returned / countered into a fresh run
    // so accumulation is always "uncountered offset from the original aim".
    if (s->recovering) {
        // acc already shrank by what recovery returned; drop the countered part too.
        s->acc_pitch -= s->countered_pitch;
        s->acc_yaw -= s->countered_yaw * signf(s->acc_yaw);
        s->countered_pitch = s->countered_yaw = 0;
        s->recovering = 0;
        s->pool_pitch = s->pool_yaw = 0;
    }
    RecoilKick raw = recoil_pattern_step(p, s->mag_seed, s->shot_index);
    float k = recoil_aim_mult(p, m);
    RecoilKick out = { raw.pitch * k, raw.yaw * k };
    s->acc_pitch += out.pitch;
    s->acc_yaw += out.yaw;
    if (s->shot_index < 0xFFFF) s->shot_index++;
    s->since_shot = 0.0f;
    s->bloom = fminf(recoil_bloom_max(p, m->ads_t), s->bloom + p->per_shot_bloom * recoil_bloom_mult(p, m->ads_t) * (m->scale > 0 ? m->scale : 1.0f));
    return out;
}

void recoil_observe_input(const RecoilParams *p, RecoilState *s, float cmd_pitch, float cmd_yaw) {
    if (!s->have_prev) {
        s->prev_cmd_pitch = cmd_pitch;
        s->prev_cmd_yaw = cmd_yaw;
        s->have_prev = 1;
        s->emitted_pitch = s->emitted_yaw = 0;
        return;
    }
    // Player input = view change minus what the recoil system itself applied.
    float dyaw = cmd_yaw - s->prev_cmd_yaw;
    while (dyaw > 180.0f) dyaw -= 360.0f;
    while (dyaw < -180.0f) dyaw += 360.0f;
    float in_p = (cmd_pitch - s->prev_cmd_pitch) - s->emitted_pitch;
    float in_y = dyaw - s->emitted_yaw;
    s->prev_cmd_pitch = cmd_pitch;
    s->prev_cmd_yaw = cmd_yaw;
    s->emitted_pitch = s->emitted_yaw = 0;
    if (!p->recovery_stop_on_input) return;
    // Pulling against the accumulated direction counts as countered (never more than acc).
    if (s->acc_pitch > 0 && in_p < 0) {
        float c = fminf(-in_p, fmaxf(0.0f, s->acc_pitch - s->countered_pitch));
        s->countered_pitch += c;
        if (s->recovering) s->pool_pitch = fmaxf(0.0f, s->pool_pitch - c);
    }
    float ay = fabsf(s->acc_yaw);
    if (ay > 0 && in_y * signf(s->acc_yaw) < 0) {
        float c = fminf(fabsf(in_y), fmaxf(0.0f, ay - s->countered_yaw));
        s->countered_yaw += c;
        if (s->recovering) {
            float py = fabsf(s->pool_yaw);
            s->pool_yaw = signf(s->acc_yaw) * fmaxf(0.0f, py - c);
        }
    }
}

RecoilKick recoil_tick(const RecoilParams *p, RecoilState *s, float dt) {
    RecoilKick out = { 0, 0 };
    s->since_shot += dt;
    if (s->since_shot > 0.0f) s->bloom = fmaxf(0.0f, s->bloom - p->bloom_recovery * lerpf(p->hip_bloom_recovery_mult, 1.0f, saturate(s->ads_t)) * dt);
    bool has_acc = s->acc_pitch != 0.0f || s->acc_yaw != 0.0f;
    if (!s->recovering && has_acc && s->since_shot >= p->recovery_delay) {
        s->recovering = 1;
        s->pool_pitch = fmaxf(0.0f, s->acc_pitch - s->countered_pitch) * p->recovery_fraction;
        s->pool_yaw = fmaxf(0.0f, fabsf(s->acc_yaw) - s->countered_yaw) * p->recovery_fraction * signf(s->acc_yaw);
        s->pool_start = fmaxf(0.25f, fmaxf(s->pool_pitch, fabsf(s->pool_yaw)));
    }
    if (!s->recovering) return out;
    // Exponential return; the initial rate equals recovery_speed (deg/s).
    float k = 1.0f - expf(-dt * p->recovery_speed / s->pool_start);
    float step_p = s->pool_pitch * k, step_y = s->pool_yaw * k;
    if (fabsf(s->pool_pitch - step_p) < 0.005f && fabsf(s->pool_yaw - step_y) < 0.005f) {
        step_p = s->pool_pitch;
        step_y = s->pool_yaw;
    }
    s->pool_pitch -= step_p;
    s->pool_yaw -= step_y;
    s->acc_pitch -= step_p;   // acc stays "offset the system still owns"
    s->acc_yaw -= step_y;
    out.pitch = -step_p;
    out.yaw = -step_y;
    if (s->pool_pitch == 0.0f && s->pool_yaw == 0.0f) {
        // Done: the non-returned remainder stays in the aim; the pattern restarts.
        s->recovering = 0;
        s->acc_pitch = s->acc_yaw = 0;
        s->countered_pitch = s->countered_yaw = 0;
        s->shot_index = 0;
    }
    return out;
}

float recoil_spread(const RecoilParams *p, const RecoilState *s, const RecoilMods *m) {
    float c = lerpf(p->base_hip * p->hip_spread_mult, p->base_ads, saturate(m->ads_t));
    if (m->crouched) c *= p->s_crouch;
    if (m->prone) c *= p->s_prone;
    c *= lerpf(1.0f, p->s_move, saturate(m->move01));
    if (m->airborne) c *= p->s_air;
    c += s->bloom;
    return c * (m->scale > 0 ? m->scale : 1.0f);
}

vec3 recoil_cone_dir(vec3 dir, float cone_deg, Rng *rng) {
    if (cone_deg <= 0.0f) return dir;
    vec3 t = fabsf(dir.y) < 0.95f ? v3_norm(v3_cross(dir, v3(0, 1, 0))) : v3(1, 0, 0);
    vec3 b = v3_cross(t, dir);
    // Gaussian-ish radius (sum of uniforms), truncated to the cone edge.
    float g = (rng_float(rng) + rng_float(rng) + rng_float(rng)) / 3.0f;   // mean .5, sd ~.17
    float r = fabsf(g - 0.5f) * 2.0f * 1.35f;                              // ~half the shots in the inner 40%
    r = fminf(r, 1.0f);
    float a = rng_float(rng) * 2.0f * DG_PI;
    float rad = tanf(cone_deg * 0.5f * DG_DEG2RAD) * r;
    return v3_norm(v3_add(dir, v3_add(v3_scale(t, cosf(a) * rad), v3_scale(b, sinf(a) * rad))));
}

int recoil_preview(const RecoilParams *p, u32 seed, vec2 *out, int n) {
    vec2 acc = v2(0, 0);
    for (int i = 0; i < n; i++) {
        RecoilKick k = recoil_pattern_step(p, seed, i);
        acc.x += k.yaw;
        acc.y += k.pitch;
        out[i] = acc;
    }
    return n;
}

int recoil_fire_budget(float *cooldown, float interval, float dt, bool trigger, int max_shots) {
    *cooldown -= dt;
    int n = 0;
    if (!trigger) {
        // Released: no banking of idle time, but a shot is ready immediately.
        if (*cooldown < 0.0f) *cooldown = 0.0f;
        return 0;
    }
    // The remainder carries over: 900 rpm at 60 Hz fires 1,1,1,2... exactly 15 shots per second.
    while (*cooldown <= 1e-6f && n < max_shots) {
        *cooldown += interval;
        n++;
    }
    return n;
}

// ------------------------------------------------------------------ visual layers

void recoil_visual_kick(const RecoilParams *p, RecoilVisual *v, float scale, float ads_t, u32 seed) {
    Rng rng = rng_make(seed);
    // Impulse velocity such that the peak displacement is roughly the configured value.
    float wc = sqrtf(fmaxf(p->cam_spring.stiffness, 1.0f)) * 1.65f;
    float wv = sqrtf(fmaxf(p->vm_spring.stiffness, 1.0f)) * 1.65f;
    float cam_s = scale * lerpf(1.0f, 0.6f, ads_t);
    float vm_s = scale * recoil_kick_mult(p, ads_t);   // hip: hip.kick_mult, ADS: ads.kick_mult
    float side = rng_range(&rng, -1.0f, 1.0f);
    spring_vec3_impulse(&v->cam_rot, v3(p->cam_pitch * wc * cam_s, p->cam_yaw * side * wc * cam_s,
                                        p->cam_roll * rng_range(&rng, -1.0f, 1.0f) * wc * cam_s));
    spring_f32_impulse(&v->cam_back, p->cam_back * wc * cam_s);
    float jr = p->vm_random;
    float j1 = 1.0f + rng_range(&rng, -jr, jr), j2 = rng_range(&rng, -1.0f, 1.0f), j3 = rng_range(&rng, -1.0f, 1.0f);
    spring_vec3_impulse(&v->vm_pos, v3(p->vm_back * 0.15f * j2 * wv * vm_s, p->vm_up * j1 * wv * vm_s, p->vm_back * j1 * wv * vm_s));
    spring_vec3_impulse(&v->vm_rot, v3(p->vm_pitch * j1 * wv * vm_s, p->vm_yaw * j2 * wv * vm_s, p->vm_roll * j3 * wv * vm_s));
}

void recoil_visual_step(const RecoilParams *p, RecoilVisual *v, float dt) {
    spring_vec3_step(&v->cam_rot, p->cam_spring, dt);
    spring_f32_step(&v->cam_back, p->cam_spring, dt);
    spring_vec3_step(&v->vm_pos, p->vm_spring, dt);
    spring_vec3_step(&v->vm_rot, p->vm_spring, dt);
}
