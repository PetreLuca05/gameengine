// Point-mass projectile integration (docs/BALLISTICS.md "Physics model").
#define DG_DETMATH   // cross-platform bit-identical math (core/dgmath.h)
#include "gameplay/ballistics/bal_sim.h"
#include "core/detmath.h"
#include "gameplay/ballistics/bal_drag.h"
#include <math.h>

float bal_sound_speed(float temp_c) { return 331.3f + 0.606f * temp_c; }

float bal_air_density(float temp_c, float altitude_m) {
    // Ideal gas at the standard-atmosphere pressure for the altitude (barometric formula,
    // 8.4 km scale height), dry air R = 287.05 J/(kg·K).
    float p = 101325.0f * dm_expf(-altitude_m / 8434.0f);
    return p / (287.05f * (temp_c + 273.15f));
}

BalEnv bal_env_make(float temp_c, float altitude_m, int drag_model) {
    BalEnv e;
    memset(&e, 0, sizeof(e));
    e.rho = bal_air_density(temp_c, altitude_m);
    e.sound_speed = bal_sound_speed(temp_c);
    e.drag_model = drag_model;
    e.spin_drift = true;
    return e;
}

float bal_rng_gauss(Rng *rng) {
    // Irwin-Hall with 4 uniforms, rescaled to unit variance: cheap and libm-free.
    float s = rng_float(rng);   // sequenced draws (operand order is unspecified in C)
    s += rng_float(rng);
    s += rng_float(rng);
    s += rng_float(rng);
    return (s - 2.0f) * 1.7320508f;
}

static vec3 any_perp(vec3 d) {
    vec3 a = fabsf(d.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0);
    return v3_norm(v3_cross(d, a));
}

vec3 bal_cone_dir(vec3 dir, float cone_rad, Rng *rng) {
    if (cone_rad <= 0.0f) return dir;
    vec3 u = any_perp(dir);
    vec3 v = v3_cross(dir, u);
    // Uniform over the disc of the cone's tangent plane (small angles).
    float r = sqrtf(rng_float(rng)) * dm_tanf(cone_rad);
    float a = rng_float(rng) * 6.2831853f;
    vec3 o = v3_add(v3_scale(u, r * dm_cosf(a)), v3_scale(v, r * dm_sinf(a)));
    return v3_norm(v3_add(dir, o));
}

void bal_launch(BalBullet *b, const BalCartridge *c, int cart_index, vec3 pos, vec3 dir, float speed,
                u32 seed, int pellet, bool tracer) {
    memset(b, 0, sizeof(*b));
    b->pos = pos;
    b->vel = v3_scale(dir, speed);
    b->mass = c->mass;
    b->bc = c->bc;
    if (tracer) {
        if (c->tracer.mass > 0.0f) b->mass = c->tracer.mass;
        if (c->tracer.bc > 0.0f) b->bc = c->tracer.bc;
        b->flags |= BF_TRACER;
    }
    b->rng = rng_make(((u64)seed << 8) ^ (u64)(pellet + 1) * 0x9E3779B97F4A7C15ULL);
    b->cart = (u8)cart_index;
    b->pellet = (u8)pellet;
    // Standard sea level for the launch flag; the step refines it with the real sound speed.
    if (speed < 340.0f) b->flags |= BF_SUBSONIC;
}

vec3 bal_drag_accel(const BalBullet *b, const BalCartridge *c, const BalEnv *e, vec3 vel) {
    vec3 air = v3_sub(vel, e->wind);
    float s = v3_len(air);
    if (s < 1e-3f) return v3(0, 0, 0);
    float a;
    if (e->drag_model == BAL_DRAG_EXP) {
        // a = k·v², k fitted for this cartridge at standard density; scale by density and BC ratio.
        a = c->exp_k * (e->rho / 1.2250f) * (c->bc / fmaxf(b->bc, 1e-4f)) * s * s;
    } else {
        a = bal_drag_decel(c->curve, b->bc, s, e->rho, e->sound_speed);
    }
    a *= 1.0f + 2.0f * b->yaw * b->yaw;   // a yawed bullet presents more area
    return v3_scale(air, -a / s);
}

float bal_bullet_speed(const BalBullet *b) { return v3_len(b->vel); }
float bal_bullet_energy(const BalBullet *b) { return 0.5f * b->mass * v3_len2(b->vel); }
float bal_bullet_mach(const BalBullet *b, const BalEnv *e) { return v3_len(v3_sub(b->vel, e->wind)) / e->sound_speed; }

static vec3 accel(const BalBullet *b, const BalCartridge *c, const BalEnv *e, vec3 vel) {
    vec3 a = bal_drag_accel(b, c, e, vel);
    a.y -= BAL_GRAVITY;
    return a;
}

bool bal_step(BalBullet *b, const BalCartridge *c, const BalEnv *e, float dt) {
    if (b->flags & BF_DEAD) return false;
    float speed = v3_len(b->vel);
    int n = (int)ceilf(speed * dt / BAL_SUBSTEP_LEN);
    n = DG_CLAMP(n, 1, BAL_MAX_SUBSTEPS);
    float h = dt / (float)n;
    for (int i = 0; i < n; i++) {
        // RK2 midpoint.
        vec3 a1 = accel(b, c, e, b->vel);
        vec3 vm = v3_madd(b->vel, a1, 0.5f * h);
        vec3 a2 = accel(b, c, e, vm);
        vec3 p0 = b->pos;
        b->pos = v3_madd(b->pos, vm, h);
        b->vel = v3_madd(b->vel, a2, h);
        b->dist += v3_dist(p0, b->pos);
        b->time += h;

        float s = v3_len(b->vel);
        float mach = bal_bullet_mach(b, e);
        if (mach > 1.0f) b->flags |= BF_SUPERSONIC_EVER;
        // Transonic instability: the centre of pressure moves as the shock waves detach, the bullet
        // picks up yaw while crossing Mach 1.2 -> 0.8 and random-walks sideways; dynamic stability
        // damps it again once subsonic.
        if (mach > 0.8f && mach < 1.2f && c->transonic_k > 0.0f) {
            b->yaw += c->transonic_k * h;
            vec3 dir = v3_scale(b->vel, 1.0f / fmaxf(s, 1e-3f));
            vec3 u = any_perp(dir);
            vec3 w = v3_cross(dir, u);
            float k = b->yaw * s * 0.1f * h;
            // one draw per statement: argument evaluation order differs between compilers (clang vs
            // gcc / MSVC), and a Mac host and a PC client must walk the same way
            float gw = bal_rng_gauss(&b->rng), gu = bal_rng_gauss(&b->rng);
            b->vel = v3_add(b->vel, v3_add(v3_scale(u, gu * k), v3_scale(w, gw * k)));
        } else if (b->yaw > 0.0f && !(b->flags & BF_RICOCHET)) {
            b->yaw -= b->yaw * fminf(2.0f * h, 1.0f);
        }
        // Spin drift (Litz): drift = 1.25·(Sg + 1.2)·t^1.83 inches for a right-hand twist. Applied as
        // its time derivative, to the right of the flight path, once the bullet has flown 0.3 s.
        if (e->spin_drift && c->spin_sg > 0.0f && b->time > 0.3f && !(b->flags & BF_RICOCHET)) {
            float rate = 1.25f * (c->spin_sg + 1.2f) * 1.83f * dm_powf(b->time, 0.83f) * 0.0254f;   // m/s
            float rate0 = 1.25f * (c->spin_sg + 1.2f) * 1.83f * dm_powf(b->time - h, 0.83f) * 0.0254f;
            vec3 dir = v3_scale(b->vel, 1.0f / fmaxf(s, 1e-3f));
            vec3 right = v3_norm(v3_cross(dir, v3(0, 1, 0)));
            b->vel = v3_madd(b->vel, right, rate - rate0);
        }
    }
    float s = v3_len(b->vel);
    if (s < BAL_MIN_SPEED || b->time > BAL_MAX_TIME || b->dist > c->max_range) {
        b->flags |= BF_DEAD;
        return false;
    }
    return true;
}

float bal_zero_angle(const BalCartridge *c, float speed, float zero_m, float sight_height, int drag_model) {
    if (zero_m <= 0.0f) return 0.0f;
    BalEnv e = bal_env_make(15.0f, 0.0f, drag_model);
    e.spin_drift = false;
    // The bore starts sight_height below the line of sight (y = 0, flying along -Z). Find the
    // elevation where the bullet's height at zero_m is 0 again.
    float lo = -0.02f, hi = 0.2f;
    for (int it = 0; it < 40; it++) {
        float ang = 0.5f * (lo + hi);
        BalBullet b;
        bal_launch(&b, c, 0, v3(0, -sight_height, 0), v3(0, dm_sinf(ang), -dm_cosf(ang)), speed, 1, 0, false);
        float y = -1e9f;
        while (bal_step(&b, c, &e, 1.0f / 240.0f)) {
            if (-b.pos.z >= zero_m) { y = b.pos.y; break; }
        }
        if (y > 0.0f) hi = ang; else lo = ang;
    }
    return 0.5f * (lo + hi);
}
