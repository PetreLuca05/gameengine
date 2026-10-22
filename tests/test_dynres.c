// Dynamic resolution controller tests (src/render/dynres.c), a simulated GPU whose frame time goes with
// the rendered pixels (ratio squared) plus a fixed part:
//   start       the player's ratio (max) until the GPU says otherwise
//   over        a heavy scene: the ratio drops within a second, settles under the budget, holds
//   recovery    the load goes away: the ratio climbs back to the maximum in steps, no overshoot
//   noise       a single slow frame (a hitch) and +-10 % jitter around 90 % of the budget: no change
//   bounds      never below min / above max; a lower maximum is followed at once
//   rate        changes are rare (cooldown): at most one per 1.5 s
#include "render/dynres.h"
#include <math.h>
#include <stdio.h>

static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static unsigned g_rng = 12345u;
static float noise(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 16777216.0f * 2.0f - 1.0f;
}

// GPU ms at a ratio: fixed + per-pixel part (ms at ratio 1).
static float gpu_at(float ratio, float fixed, float pixels) { return fixed + pixels * ratio * ratio; }

// Runs `seconds` at fps frames per second, returns the number of ratio changes.
static int run(DynresState *s, const DynresParams *p, float seconds, float fixed, float pixels, float jitter) {
    int changes = s->changes;
    float dt = 1.0f / 300.0f;
    for (float t = 0; t < seconds; t += dt) {
        float g = gpu_at(s->ratio > 0 ? s->ratio : p->max_ratio, fixed, pixels) * (1.0f + jitter * noise());
        dynres_update(s, p, g, dt);
    }
    return s->changes - changes;
}

int main(void) {
    DynresParams p;
    dynres_defaults(&p, 300.0f, 0.5f, 0.75f);   // 3.33 ms
    CHECK(fabsf(p.target_ms - 3.3333f) < 1e-3f, "budget %g", p.target_ms);
    DynresState s = { 0 };
    // start: light scene (2 ms at 0.75): stays at the maximum
    run(&s, &p, 3.0f, 0.5f, 2.0f, 0.0f);
    CHECK(fabsf(s.ratio - 0.75f) < 1e-4f && s.changes == 0, "start: ratio %g, changes %d", s.ratio, s.changes);
    // over: 5 ms at 0.75 (1.0 + 7.7 * r^2): must drop and settle under the budget
    float t_first = -1;
    {
        float dt = 1.0f / 300.0f;
        for (float t = 0; t < 8.0f; t += dt) {
            dynres_update(&s, &p, gpu_at(s.ratio, 1.0f, 7.7f), dt);
            if (t_first < 0 && s.ratio < 0.749f) t_first = t;
        }
    }
    float g = gpu_at(s.ratio, 1.0f, 7.7f);
    CHECK(t_first >= 0 && t_first < 1.0f, "over: first drop after %g s", t_first);
    CHECK(g <= p.target_ms * 0.95f && g >= p.target_ms * 0.6f, "over: settled at ratio %g (%g ms)", s.ratio, g);
    int settled = s.changes;
    run(&s, &p, 5.0f, 1.0f, 7.7f, 0.0f);
    CHECK(s.changes == settled, "over: holds (%d more changes)", s.changes - settled);
    // recovery: the load goes away (1.0 + 2.0 r^2): back to 0.75, never above
    run(&s, &p, 20.0f, 1.0f, 2.0f, 0.0f);
    CHECK(fabsf(s.ratio - 0.75f) < 1e-4f, "recovery: ratio %g", s.ratio);
    // noise: +-10 % around 90 % of the budget (3.0 ms at 0.75), and one 30 ms hitch: no change
    int before = s.changes;
    run(&s, &p, 10.0f, 1.0f, (3.0f - 1.0f) / (0.75f * 0.75f), 0.10f);
    dynres_update(&s, &p, 30.0f, 1.0f / 300.0f);
    run(&s, &p, 2.0f, 1.0f, (3.0f - 1.0f) / (0.75f * 0.75f), 0.0f);
    CHECK(s.changes == before, "noise: %d changes", s.changes - before);
    // bounds: an impossible scene (20 ms fixed): stops at the minimum
    run(&s, &p, 20.0f, 20.0f, 4.0f, 0.0f);
    CHECK(fabsf(s.ratio - 0.5f) < 1e-4f, "bounds: min %g", s.ratio);
    p.max_ratio = 0.45f;   // a maximum below the minimum: the maximum wins
    dynres_update(&s, &p, 2.0f, 1.0f / 300.0f);
    CHECK(fabsf(s.ratio - 0.45f) < 1e-4f, "bounds: lower maximum followed at once (%g)", s.ratio);
    // rate: a load oscillating every 0.5 s between heavy and light: at most one change per cooldown
    dynres_defaults(&p, 300.0f, 0.5f, 1.0f);
    DynresState r = { 0 };
    int n = 0;
    for (int k = 0; k < 20; k++) n += run(&r, &p, 0.5f, (k & 1) ? 0.5f : 2.5f, (k & 1) ? 1.0f : 4.0f, 0.0f);
    CHECK(n <= (int)(10.0f / p.cooldown) + 1, "rate: %d changes in 10 s", n);
    printf("dynres: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
