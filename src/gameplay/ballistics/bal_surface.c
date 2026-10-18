// Ricochet / penetration / flesh (docs/BALLISTICS.md "Surfaces").
#define DG_DETMATH   // cross-platform bit-identical math (core/dgmath.h)
#include "gameplay/ballistics/bal_surface.h"
#include "core/detmath.h"
#include <math.h>

#define DEG2RAD 0.017453293f

float bal_incidence(vec3 dir, vec3 normal) { return dm_asinf(clampf(fabsf(v3_dot(dir, normal)), 0.0f, 1.0f)); }

float bal_capacity(const BalMaterial *m, const BalCartridge *c, float energy) {
    // depth_ref is what 5.56 M855 defeats at its muzzle; the cartridge's hard / soft factor is what
    // it defeats at its own muzzle relative to that (hard: energy density and core; soft: sectional
    // density), and the capacity falls linearly with the energy the bullet has left.
    float pen = m->cls == BAL_SOFT ? c->pen_soft : c->pen_hard;
    return m->depth_ref * pen * (energy / fmaxf(c->e_ref, 1.0f));
}

float bal_ricochet_chance(const BalMaterial *m, float incidence) {
    float th = m->ricochet_deg * DEG2RAD;
    if (th <= 0.0f || incidence >= th) return 0.0f;
    float f = incidence / th;
    return 1.0f - f * f;
}

static float speed_for_energy(float mass, float e) { return e > 0.0f ? sqrtf(2.0f * e / mass) : 0.0f; }

BalHitResult bal_hit_surface(BalBullet *b, const BalCartridge *c, const BalMaterial *m, vec3 point,
                             vec3 dir, vec3 normal, float thickness) {
    BalHitResult r;
    memset(&r, 0, sizeof(r));
    float e = bal_bullet_energy(b);
    float speed = bal_bullet_speed(b);
    r.incidence = bal_incidence(dir, normal);
    r.energy_in = e;
    r.bc_mul = 1.0f;
    r.out_pos = point;
    r.out_dir = dir;
    if (v3_dot(dir, normal) > 0.0f) normal = v3_neg(normal);   // the face we came through
    r.capacity = bal_capacity(m, c, e);
    r.thickness = thickness >= 0.0f ? thickness : m->thickness;
    // The skip roll comes first (always drawn, so the stream stays aligned on every path).
    float roll = rng_float(&b->rng);
    if (b->events < BAL_MAX_EVENTS && roll < bal_ricochet_chance(m, r.incidence)) {
        float f = r.incidence / (m->ricochet_deg * DEG2RAD);
        float keep = lerpf(0.60f, 0.30f, f);   // speed retained: grazing hits lose least
        float out_ang = r.incidence * rng_range(&b->rng, 0.3f, 0.8f);
        vec3 t = v3_norm(v3_madd(dir, normal, -v3_dot(dir, normal)));
        vec3 nd = v3_add(v3_scale(t, dm_cosf(out_ang)), v3_scale(normal, dm_sinf(out_ang)));
        nd = bal_cone_dir(nd, rng_range(&b->rng, 2.0f, 8.0f) * DEG2RAD, &b->rng);   // tumble
        if (v3_dot(nd, normal) < 0.02f) nd = v3_norm(v3_madd(nd, normal, 0.03f - v3_dot(nd, normal)));
        r.outcome = BAL_RICOCHET;
        r.out_dir = nd;
        r.out_speed = speed * keep;
        r.energy_out = 0.5f * b->mass * r.out_speed * r.out_speed;
        r.energy_deposit = e - r.energy_out;
        r.out_pos = v3_madd(point, normal, 0.002f);
        r.bc_mul = 0.35f;   // a tumbling, deformed bullet
        return r;
    }
    if (b->events < BAL_MAX_EVENTS && r.capacity > r.thickness) {
        float left = 1.0f - r.thickness / r.capacity;
        r.outcome = BAL_PENETRATE;
        r.energy_out = e * left;
        r.energy_deposit = e - r.energy_out;
        r.out_speed = speed_for_energy(b->mass, r.energy_out);
        r.out_dir = bal_cone_dir(dir, m->exit_spread_deg * DEG2RAD * (1.0f - left), &b->rng);
        r.out_pos = v3_madd(point, dir, r.thickness + 0.002f);
        r.bc_mul = lerpf(0.6f, 1.0f, left);   // deformation from what it went through
        return r;
    }
    r.outcome = BAL_STOP;
    r.energy_deposit = e;
    return r;
}

BalHitResult bal_hit_flesh(BalBullet *b, const BalCartridge *c, const BalMaterial *flesh, vec3 entry,
                           vec3 dir, float chord, float depth_before) {
    BalHitResult r;
    memset(&r, 0, sizeof(r));
    float e = bal_bullet_energy(b);
    float speed = bal_bullet_speed(b);
    r.energy_in = e;
    r.incidence = 1.5707963f;
    r.bc_mul = 1.0f;
    r.out_pos = entry;
    r.out_dir = dir;
    r.thickness = chord;
    float cap = bal_capacity(flesh, c, e);
    // Fragmentation (M855-style yaw-and-break above ~760 m/s): the core still exits, but the jacket
    // fragments stay in the target, so the exit carries half the energy and the wound grows.
    if (c->fragment_v > 0.0f && speed > c->fragment_v) r.fragment = true;
    r.capacity = cap;
    float depth_after = depth_before + chord;
    if (c->yaw_depth > 0.0f && depth_after > c->yaw_depth) r.yawed = true;
    float cavity_mul = (r.fragment ? 2.5f : 1.0f) * (r.yawed ? 1.5f : 1.0f);
    if (cap > chord) {
        float left = 1.0f - chord / cap;
        r.outcome = BAL_PENETRATE;
        r.energy_out = e * left * (r.fragment ? 0.5f : 1.0f);
        r.out_speed = speed_for_energy(b->mass, r.energy_out);
        r.out_dir = bal_cone_dir(dir, flesh->exit_spread_deg * DEG2RAD * (1.0f - left), &b->rng);
        r.out_pos = v3_madd(entry, dir, chord + 0.002f);
        r.bc_mul = r.yawed ? 0.3f : 0.8f;
    } else {
        r.outcome = BAL_STOP;
    }
    r.energy_deposit = e - r.energy_out;
    // Temporary cavity scales with the energy deposited (volume ∝ E, so linear size ∝ E^(1/3)).
    r.cavity = dm_cbrtf(r.energy_deposit / 1000.0f) * cavity_mul;
    return r;
}

bool bal_apply_hit(BalBullet *b, const BalHitResult *r) {
    if (r->outcome == BAL_STOP || r->out_speed < BAL_MIN_SPEED) {
        b->flags |= BF_DEAD;
        b->vel = v3(0, 0, 0);
        return false;
    }
    b->pos = r->out_pos;
    b->vel = v3_scale(r->out_dir, r->out_speed);
    b->bc *= r->bc_mul;
    b->events++;
    if (r->outcome == BAL_RICOCHET) {
        b->flags |= BF_RICOCHET;
        b->yaw = fmaxf(b->yaw, 0.3f);
    } else {
        b->flags |= BF_PENETRATED;
        if (r->fragment) b->flags |= BF_FRAGMENTED;
    }
    return true;
}
