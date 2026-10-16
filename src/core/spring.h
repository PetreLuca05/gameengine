// Damped springs for procedural motion (camera kick, viewmodel recoil, sway).
// Step them at the fixed sim rate; keep `prev` for render interpolation.
#ifndef DG_SPRING_H
#define DG_SPRING_H

#include "base.h"
#include "dgmath.h"

typedef struct SpringF32 { float x, v, prev; } SpringF32;
typedef struct SpringVec3 { vec3 x, v, prev; } SpringVec3;

typedef struct SpringParams {
    float stiffness;   // k (1/s^2): higher = snappier return
    float damping;     // c (1/s): ~2*sqrt(k) is critical damping
} SpringParams;

// Semi-implicit Euler towards target 0 (offsets decay back to rest).
void spring_f32_step(SpringF32 *s, SpringParams p, float dt);
void spring_vec3_step(SpringVec3 *s, SpringParams p, float dt);
// Impulses add velocity, so rapid fire stacks up and settles smoothly.
DG_INLINE void spring_f32_impulse(SpringF32 *s, float dv) { s->v += dv; }
DG_INLINE void spring_vec3_impulse(SpringVec3 *s, vec3 dv) { s->v = v3_add(s->v, dv); }
DG_INLINE float spring_f32_sample(const SpringF32 *s, float alpha) { return lerpf(s->prev, s->x, alpha); }
DG_INLINE vec3 spring_vec3_sample(const SpringVec3 *s, float alpha) { return v3_lerp(s->prev, s->x, alpha); }
DG_INLINE void spring_vec3_reset(SpringVec3 *s) { memset(s, 0, sizeof(*s)); }

#endif
