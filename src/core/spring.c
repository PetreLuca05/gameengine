#include "spring.h"

// Sub-stepped so stiff springs stay stable at 60 Hz.
#define SPRING_SUBSTEPS 4

void spring_f32_step(SpringF32 *s, SpringParams p, float dt) {
    s->prev = s->x;
    float h = dt / SPRING_SUBSTEPS;
    for (int i = 0; i < SPRING_SUBSTEPS; i++) {
        float a = -p.stiffness * s->x - p.damping * s->v;
        s->v += a * h;
        s->x += s->v * h;
    }
}

void spring_vec3_step(SpringVec3 *s, SpringParams p, float dt) {
    s->prev = s->x;
    float h = dt / SPRING_SUBSTEPS;
    for (int i = 0; i < SPRING_SUBSTEPS; i++) {
        vec3 a = v3_sub(v3_scale(s->x, -p.stiffness), v3_scale(s->v, p.damping));
        s->v = v3_madd(s->v, a, h);
        s->x = v3_madd(s->x, s->v, h);
    }
}
