// Horde core tests (pure): spatial grid queries match brute force, steering is bit-identical
// whatever the chunking / thread count (600 ticks, 500 zombies), tier hysteresis, the shipped
// assets/ai/zombie.json. Run from the build output directory.
#include "gameplay/zombies/horde_grid.h"
#include "gameplay/zombies/horde_steer.h"
#include "core/engine.h"
#include "core/jobs.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
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

static u32 rng = 12345u;
static float frand(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 16777216.0f;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void test_grid(void) {
    enum { N = 2000 };
    static vec3 pos[N];
    static u8 act[N];
    for (int i = 0; i < N; i++) {
        pos[i] = v3((frand() - 0.5f) * 120.0f, frand(), (frand() - 0.5f) * 120.0f);
        act[i] = frand() > 0.1f;
    }
    // Dense clump (crowding at the player).
    for (int i = 0; i < 300; i++) pos[i] = v3((frand() - 0.5f) * 3.0f, 0, (frand() - 0.5f) * 3.0f);
    HordeGrid g;
    CHECK(hgrid_init(&g, N, 2.0f), "grid init");
    hgrid_build(&g, pos, act, N);
    int bad = 0;
    static int got[N], want[N];
    for (int q = 0; q < 1000; q++) {
        vec3 p = q < 200 ? v3((frand() - 0.5f) * 3.0f, 0, (frand() - 0.5f) * 3.0f) : v3((frand() - 0.5f) * 130.0f, 0, (frand() - 0.5f) * 130.0f);
        float r = 0.5f + frand() * 3.0f;
        int self = (int)(frand() * N);
        int n = hgrid_query(&g, pos, p, r, self, got, N);
        int m = 0;
        for (int i = 0; i < N; i++) {
            if (!act[i] || i == self) continue;
            float dx = pos[i].x - p.x, dz = pos[i].z - p.z;
            if (dx * dx + dz * dz <= r * r) want[m++] = i;
        }
        qsort(want, (size_t)m, sizeof(int), cmp_int);
        if (n != m || memcmp(got, want, sizeof(int) * (size_t)n)) bad++;
        // Ascending output (deterministic neighbour order).
        for (int k = 1; k < n; k++) if (got[k - 1] >= got[k]) { bad++; break; }
    }
    CHECK(bad == 0, "grid queries match brute force (%d mismatches of 1000)", bad);
    // Capped queries keep the lowest indices.
    int cap[8];
    int n = hgrid_query(&g, pos, v3(0, 0, 0), 2.0f, -1, cap, 8);
    int m = 0;
    for (int i = 0; i < N; i++) {
        if (!act[i]) continue;
        if (pos[i].x * pos[i].x + pos[i].z * pos[i].z <= 4.0f) want[m++] = i;
    }
    CHECK(n == 8 && m >= 8 && !memcmp(cap, want, sizeof(int) * 8), "capped query keeps the lowest indices");
    hgrid_free(&g);
}

// A tiny horde world: positions integrate the steered velocity, the target walks in a circle.
typedef struct Sim {
    int n;
    vec3 pos[600], vel[600], vel_out[600], target[600], avoid[600];
    float yaw[600], yaw_out[600], speed[600];
    u8 has_target[600], tier[600], active[600], busy[600];
    HordeGrid grid;
} Sim;

typedef struct Ctx { HordeSteerIn in; HordeSteerOut out; } Ctx;
static void steer_cb(void *u, int b, int e) { Ctx *c = (Ctx *)u; horde_steer_range(&c->in, &c->out, b, e); }

static void sim_init(Sim *s, const HordeConfig *cfg) {
    memset(s, 0, sizeof(*s));
    s->n = 500;
    u32 r = 99u;
    for (int i = 0; i < s->n; i++) {
        r = r * 1664525u + 1013904223u;
        float a = (float)(r >> 8) / 16777216.0f * 6.2831853f;
        float d = 25.0f + (float)(i % 7);
        s->pos[i] = v3(cosf(a) * d, 0, sinf(a) * d);
        s->speed[i] = 1.2f + (float)(i % 5) * 0.2f;
        s->yaw[i] = a;
        s->has_target[i] = 1;
        s->tier[i] = (u8)(i % 3);
        s->busy[i] = (i % 17) == 0;
    }
    hgrid_init(&s->grid, 600, 2.0f);
    (void)cfg;
}

// mode 0 serial, 1 chunks in reverse order (any interleaving must give the same result), 2 jobs.
static void sim_run(Sim *s, const HordeConfig *cfg, int mode, int ticks) {
    for (int t = 0; t < ticks; t++) {
        vec3 goal = v3(cosf((float)t * 0.01f) * 4.0f, 0, sinf((float)t * 0.01f) * 4.0f);
        for (int i = 0; i < s->n; i++) {
            s->target[i] = goal;
            s->active[i] = ((u32)(t + i) % (u32)cfg->tier_interval[s->tier[i]]) == 0;
            s->avoid[i] = (i % 11 == 0) ? v3(0.3f, 0, -0.2f) : v3(0, 0, 0);
        }
        hgrid_build(&s->grid, s->pos, NULL, s->n);
        Ctx c;
        memset(&c, 0, sizeof(c));
        c.in.count = s->n;
        c.in.pos = s->pos;
        c.in.vel = s->vel;
        c.in.yaw = s->yaw;
        c.in.speed_max = s->speed;
        c.in.target = s->target;
        c.in.has_target = s->has_target;
        c.in.tier = s->tier;
        c.in.active = s->active;
        c.in.busy = s->busy;
        c.in.avoid = s->avoid;
        c.in.grid = &s->grid;
        c.in.cfg = cfg;
        c.in.dt = 1.0f / 60.0f;
        c.out.vel = s->vel_out;
        c.out.yaw = s->yaw_out;
        if (mode == 0) steer_cb(&c, 0, s->n);
        else if (mode == 1) for (int b = ((s->n - 1) / 37) * 37; b >= 0; b -= 37) steer_cb(&c, b, DG_MIN(s->n, b + 37));
        else jobs_parallel_for(s->n, 64, steer_cb, &c);
        for (int i = 0; i < s->n; i++) {
            if (s->active[i]) { s->vel[i] = s->vel_out[i]; s->yaw[i] = s->yaw_out[i]; }
            s->pos[i] = v3_add(s->pos[i], v3_scale(s->vel[i], 1.0f / 60.0f));
        }
    }
}

static void test_determinism(const HordeConfig *cfg) {
    static Sim a, b;
    sim_init(&a, cfg);
    sim_run(&a, cfg, 0, 600);
    sim_init(&b, cfg);
    sim_run(&b, cfg, 1, 600);
    CHECK(!memcmp(a.pos, b.pos, sizeof(vec3) * (size_t)a.n) && !memcmp(a.yaw, b.yaw, sizeof(float) * (size_t)a.n), "chunk order does not change the result");
    const int workers[3] = { 1, 3, 7 };
    for (int k = 0; k < 3; k++) {
        jobs_init(workers[k]);
        sim_init(&b, cfg);
        double t0 = platform_time();
        sim_run(&b, cfg, 2, 600);
        double ms = (platform_time() - t0) * 1000.0 / 600.0;
        jobs_shutdown();
        CHECK(!memcmp(a.pos, b.pos, sizeof(vec3) * (size_t)a.n), "%d workers: bit-identical positions after 600 ticks", workers[k]);
        printf("  steering, %d workers: %.3f ms / tick (500 zombies, grid + steer)\n", workers[k], ms);
    }
    // They actually converged on the moving goal and stayed apart.
    int close = 0, overlaps = 0;
    for (int i = 0; i < a.n; i++) {
        if (sqrtf(a.pos[i].x * a.pos[i].x + a.pos[i].z * a.pos[i].z) < 20.0f) close++;
        for (int j = i + 1; j < a.n; j++) {
            float dx = a.pos[i].x - a.pos[j].x, dz = a.pos[i].z - a.pos[j].z;
            if (dx * dx + dz * dz < 0.15f * 0.15f) overlaps++;
        }
    }
    CHECK(close > a.n * 9 / 10, "the horde converges on the target (%d / %d within 20 m: a packed crowd of 500 is ~12 m across)", close, a.n);
    CHECK(overlaps < a.n / 20, "separation keeps them apart (%d pairs closer than 15 cm)", overlaps);
    printf("  steering: %d / %d converged, %d overlapping pairs\n", close, a.n, overlaps);
    hgrid_free(&a.grid);
    hgrid_free(&b.grid);
}

static void test_tiers(const HordeConfig *c) {
    CHECK(horde_tier_for(c, -1, 5) == HT_NEAR && horde_tier_for(c, -1, 30) == HT_MID && horde_tier_for(c, -1, 100) == HT_FAR && horde_tier_for(c, -1, 500) == HT_DORMANT, "plain bands");
    // Promotion / demotion only past the hysteresis band; dithering around an edge never flips.
    float edge = c->tier_dist[HT_NEAR];
    int t = HT_NEAR, flips = 0;
    for (int k = 0; k < 200; k++) {
        float d = edge + ((k & 1) ? 1.0f : -1.0f) * (c->tier_hyst * 0.6f);
        int nt = horde_tier_for(c, t, d);
        if (nt != t) flips++;
        t = nt;
    }
    CHECK(flips == 0, "no flapping inside the hysteresis band (%d flips)", flips);
    CHECK(horde_tier_for(c, HT_NEAR, edge + c->tier_hyst + 0.1f) == HT_MID, "demoted past the band");
    CHECK(horde_tier_for(c, HT_MID, edge - c->tier_hyst - 0.1f) == HT_NEAR, "promoted past the band");
    CHECK(horde_tier_for(c, HT_MID, edge - 0.5f) == HT_MID, "stays mid just inside the near bound");
}

int main(void) {
    printf("horde tests\n");
    HordeConfig cfg;
    size_t n = 0;
    char *text = platform_read_file("assets/ai/zombie.json", &n);
    char err[128] = { 0 };
    CHECK(text && horde_config_parse(&cfg, text, err, sizeof(err)), "assets/ai/zombie.json parses (%s)", err);
    free(text);
    CHECK(cfg.tier_dist[0] < cfg.tier_dist[1] && cfg.tier_dist[1] < cfg.tier_dist[2], "tier bands ascend");
    CHECK(cfg.tier_interval[HT_NEAR] == 1 && cfg.net_interval[HT_DORMANT] == 0, "near every tick, dormant not sent");
    CHECK(cfg.capacity <= 2048 && cfg.net_budget > 0, "capacity / budget");
    test_grid();
    test_tiers(&cfg);
    test_determinism(&cfg);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
