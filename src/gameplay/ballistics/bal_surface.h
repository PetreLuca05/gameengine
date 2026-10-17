// What a bullet does when it meets a surface: ricochet, penetrate (exit with less energy and a
// wider cone) or stop. Driven by incidence angle, the material table and the cartridge's
// penetration factors; flesh adds fragmentation and yaw. Pure and deterministic (bullet RNG).
#ifndef DG_BAL_SURFACE_H
#define DG_BAL_SURFACE_H

#include "gameplay/ballistics/bal_sim.h"

#define BAL_MAX_EVENTS 4   // ricochets + penetrations per bullet

typedef enum BalOutcome { BAL_STOP, BAL_RICOCHET, BAL_PENETRATE } BalOutcome;

typedef struct BalHitResult {
    u8 outcome;            // BalOutcome
    bool fragment;         // flesh: the bullet broke up (bigger cavity, shorter path)
    bool yawed;            // flesh: turned sideways before exiting
    float incidence;       // rad from the surface
    float energy_in;       // J at the entry face
    float energy_out;      // J leaving (exit or ricochet), 0 when stopped
    float energy_deposit;  // J left in the target
    float capacity;        // m of this material the bullet could defeat
    float thickness;       // m used
    float cavity;          // flesh: wound / exit-spray scale (1 = 5.56 at 1 kJ deposit, point-on)
    vec3 out_pos;          // exit point (penetration) or the ricochet point
    vec3 out_dir;          // new unit direction
    float out_speed;
    float bc_mul;          // multiply the bullet BC (tumble / yaw)
} BalHitResult;

float bal_incidence(vec3 dir, vec3 normal);   // rad between the path and the surface plane
// Penetration capacity in metres of the material for a bullet with this energy (J).
float bal_capacity(const BalMaterial *m, const BalCartridge *c, float energy);
// Probability of a skip at this incidence (0 above ricochet_deg).
float bal_ricochet_chance(const BalMaterial *m, float incidence);

// World surfaces. thickness < 0: unknown, use the material's assumed thickness. Consumes
// random numbers from the bullet's stream only, so host and client agree.
BalHitResult bal_hit_surface(BalBullet *b, const BalCartridge *c, const BalMaterial *m, vec3 point,
                             vec3 dir, vec3 normal, float thickness);
// Flesh through one hitbox capsule: chord = path length inside it, depth_before = tissue
// already crossed by this bullet (several capsules in a row).
BalHitResult bal_hit_flesh(BalBullet *b, const BalCartridge *c, const BalMaterial *flesh, vec3 entry,
                           vec3 dir, float chord, float depth_before);
// Applies a result to the bullet (position, velocity, flags, BC). false: the bullet is gone.
bool bal_apply_hit(BalBullet *b, const BalHitResult *r);

#endif
