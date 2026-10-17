// Radial wheel geometry (src/ui/widgets/radial_math.h). Pure.
#include "radial_math.h"
#include <math.h>

#define TWO_PI (2.0f * DG_PI)

static float wrap2pi(float a) {
    a = fmodf(a, TWO_PI);
    return a < 0 ? a + TWO_PI : a;
}

float radial_slot_angle(int slot, int count) {
    if (count <= 0) return 0;
    return wrap2pi((float)slot * TWO_PI / (float)count);
}

float radial_angle_of(vec2 dir) {
    // Up (y < 0 on screen) is 0, clockwise positive: atan2(x, -y).
    return wrap2pi(atan2f(dir.x, -dir.y));
}

int radial_slot_for_angle(float angle, int count) {
    if (count <= 0) return -1;
    float step = TWO_PI / (float)count;
    int k = (int)floorf(wrap2pi(angle + step * 0.5f) / step);
    return k % count;
}

float radial_angle_dist(float a, float b) {
    float d = fabsf(wrap2pi(a) - wrap2pi(b));
    return d > DG_PI ? TWO_PI - d : d;
}

void radial_update(RadialSel *s, const RadialParams *p) {
    int n = DG_CLAMP(p->count, 1, RADIAL_MAX_SLOTS);
    float len = sqrtf(s->cursor.x * s->cursor.x + s->cursor.y * s->cursor.y);
    if (len < p->dead_zone || len <= 1e-6f) return;   // inside the dead zone: the selection keeps
    float a = radial_angle_of(s->cursor);
    int k = radial_slot_for_angle(a, n);
    if (s->slot < 0 || s->slot >= n || k == s->slot) { s->slot = k; return; }
    // Hysteresis: leave the current slot only once past its edge by the margin.
    float half = DG_PI / (float)n;
    float margin = fminf(p->hysteresis, half * 0.45f);
    if (radial_angle_dist(a, radial_slot_angle(s->slot, n)) > half + margin) s->slot = k;
}

void radial_feed_delta(RadialSel *s, const RadialParams *p, vec2 delta) {
    s->cursor.x += delta.x;
    s->cursor.y += delta.y;
    float len = sqrtf(s->cursor.x * s->cursor.x + s->cursor.y * s->cursor.y);
    if (p->radius > 0 && len > p->radius) { s->cursor.x *= p->radius / len; s->cursor.y *= p->radius / len; }
    radial_update(s, p);
}

void radial_feed_stick(RadialSel *s, const RadialParams *p, vec2 stick) {
    float r = p->radius > 0 ? p->radius : 1.0f;
    s->cursor = v2(stick.x * r, stick.y * r);
    radial_update(s, p);
}

void radial_select(RadialSel *s, const RadialParams *p, int slot) {
    if (slot < 0 || slot >= p->count) return;
    s->slot = slot;
    s->cursor = v2(0, 0);
}

void radial_step(RadialSel *s, const RadialParams *p, int dir) {
    if (p->count <= 0) return;
    int k = s->slot < 0 ? (dir > 0 ? 0 : p->count - 1) : ((s->slot + dir) % p->count + p->count) % p->count;
    radial_select(s, p, k);
}
