// Unit tests for the pure ballistics modules (src/gameplay/ballistics: drag, sim, surfaces,
// acoustics, tracers, muzzle flash, data). Reference trajectories: tests/data/ballistics_ref.json
// (tools/ballistics_ref.py, py-ballisticcalc). Build: target test_ballistics; run: ctest.
#include "core/detmath.h"
#include "gameplay/ballistics/bal_acoustics.h"
#include "gameplay/ballistics/bal_data.h"
#include "gameplay/ballistics/bal_drag.h"
#include "gameplay/ballistics/bal_muzzle.h"
#include "gameplay/ballistics/bal_sim.h"
#include "gameplay/ballistics/bal_surface.h"
#include "gameplay/ballistics/bal_tracer.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

#define DEG2RAD 0.017453293f

static char *read_text(const char *rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    s[n] = 0;
    fclose(f);
    return s;
}

static BalData g_data;

static void load_data(void) {
    bal_data_init(&g_data);
    char *c = read_text("assets/ballistics/cartridges.json");
    char *m = read_text("assets/ballistics/materials.json");
    CHECK(c && bal_data_parse_cartridges(&g_data, c), "cartridges.json");
    CHECK(m && bal_data_parse_materials(&g_data, m), "materials.json");
    const char *att[] = { "suppressor", "compensator", "flash_hider", "muzzle_brake" };
    for (int i = 0; i < 4; i++) {
        char rel[128];
        snprintf(rel, sizeof(rel), "assets/attachments/%s.json", att[i]);
        char *a = read_text(rel);
        CHECK(a && bal_data_parse_attachment(&g_data, a), "%s", rel);
        free(a);
    }
    free(c);
    free(m);
    CHECK(g_data.cart_count >= 7, "cartridge count %d", g_data.cart_count);
    CHECK(g_data.dev_count == 5, "device count %d (bare + 4)", g_data.dev_count);
}

static const BalCartridge *cart(const char *id) {
    int i = bal_cartridge_find(&g_data, id);
    CHECK(i >= 0, "cartridge %s", id);
    return &g_data.cart[i >= 0 ? i : 0];
}

// ------------------------------------------------------------------ trajectories

typedef struct Sample { float v, drop, t, drift; bool ok; } Sample;

// Flies along +X at the game tick (60 Hz) with an optional +Z crosswind; samples by interpolating
// the tick positions at each range.
static void fly(const BalCartridge *c0, float mv, int model, float wind, const float *ranges, int n, Sample *out) {
    BalCartridge c = *c0;
    c.transonic_k = 0.0f;   // the references are plain point mass
    BalEnv e = bal_env_make(15.0f, 0.0f, model);
    e.spin_drift = false;
    e.wind = v3(0, 0, wind);
    BalBullet b;
    bal_launch(&b, &c, 0, v3(0, 0, 0), v3(1, 0, 0), mv, 7, 0, false);
    memset(out, 0, sizeof(Sample) * (size_t)n);
    int k = 0;
    BalBullet prev = b;
    while (k < n && bal_step(&b, &c, &e, 1.0f / 60.0f)) {
        while (k < n && b.pos.x >= ranges[k]) {
            float f = (ranges[k] - prev.pos.x) / (b.pos.x - prev.pos.x);
            out[k].v = lerpf(v3_len(prev.vel), v3_len(b.vel), f);
            out[k].drop = -lerpf(prev.pos.y, b.pos.y, f);
            out[k].t = lerpf(prev.time, b.time, f);
            out[k].drift = lerpf(prev.pos.z, b.pos.z, f);
            out[k].ok = true;
            k++;
        }
        prev = b;
    }
}

static bool within(float a, float ref, float rel) { return fabsf(a - ref) <= fabsf(ref) * rel; }

static void test_trajectories(void) {
    char *txt = read_text("tests/data/ballistics_ref.json");
    CHECK(txt != NULL, "tests/data/ballistics_ref.json");
    if (!txt) return;
    cJSON *root = cJSON_Parse(txt);
    const cJSON *cs;
    const float ranges[3] = { 100, 300, 500 };
    cJSON_ArrayForEach(cs, cJSON_GetObjectItemCaseSensitive(root, "cases")) {
        const char *id = cJSON_GetObjectItemCaseSensitive(cs, "cartridge")->valuestring;
        float mv = (float)cJSON_GetObjectItemCaseSensitive(cs, "mv")->valuedouble;
        const cJSON *pts = cJSON_GetObjectItemCaseSensitive(cs, "points");
        const BalCartridge *c = cart(id);
        bool rifle = !strcmp(id, "556_m855") || !strcmp(id, "762x51_m80");
        Sample s[3], sw[3], sx[3];
        fly(c, mv, BAL_DRAG_TABLE, 0.0f, ranges, 3, s);
        fly(c, mv, BAL_DRAG_TABLE, 5.0f, ranges, 3, sw);
        fly(c, mv, BAL_DRAG_EXP, 0.0f, ranges, 3, sx);
        for (int i = 0; i < 3; i++) {
            char key[8];
            snprintf(key, sizeof(key), "%d", (int)ranges[i]);
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(pts, key);
            float rv = (float)cJSON_GetObjectItemCaseSensitive(p, "v")->valuedouble;
            float rd = (float)cJSON_GetObjectItemCaseSensitive(p, "drop")->valuedouble;
            float rt = (float)cJSON_GetObjectItemCaseSensitive(p, "t")->valuedouble;
            float rw = (float)cJSON_GetObjectItemCaseSensitive(p, "drift")->valuedouble;
            CHECK(s[i].ok, "%s reaches %d m", id, (int)ranges[i]);
            // The brief's acceptance: 5.56 and 7.62x51 within 5 % (velocity and drop); the other
            // cartridges are held to the same bar as a regression check.
            CHECK(within(s[i].v, rv, 0.05f), "%s v@%dm %.1f ref %.1f", id, (int)ranges[i], s[i].v, rv);
            CHECK(within(s[i].drop, rd, 0.05f), "%s drop@%dm %.3f ref %.3f", id, (int)ranges[i], s[i].drop, rd);
            CHECK(within(s[i].t, rt, 0.05f), "%s t@%dm %.3f ref %.3f", id, (int)ranges[i], s[i].t, rt);
            CHECK(within(sw[i].drift, rw, 0.05f), "%s drift@%dm %.3f ref %.3f", id, (int)ranges[i], sw[i].drift, rw);
            // Didion's analytic drift: D = W·(t - x/v0).
            float didion = 5.0f * (sw[i].t - ranges[i] / mv);
            CHECK(within(sw[i].drift, didion, 0.05f), "%s drift@%dm %.3f Didion %.3f", id, (int)ranges[i], sw[i].drift, didion);
            if (rifle && ranges[i] <= 300) {
                CHECK(within(sx[i].v, rv, 0.10f), "%s exp v@%dm %.1f ref %.1f", id, (int)ranges[i], sx[i].v, rv);
                CHECK(within(sx[i].drop, rd, 0.10f), "%s exp drop@%dm %.3f ref %.3f", id, (int)ranges[i], sx[i].drop, rd);
            }
            if (rifle) printf("  %-11s %3dm  v %6.1f (ref %6.1f)  drop %6.3f (ref %6.3f)  drift %5.3f (ref %5.3f, Didion %5.3f)  t %.3f\n",
                              id, (int)ranges[i], s[i].v, rv, s[i].drop, rd, sw[i].drift, rw, didion, s[i].t);
        }
    }
    cJSON_Delete(root);
    free(txt);
    // Pistol rounds arc: a 9 mm drops > 40 cm by 100 m and is subsonic from launch at 330 m/s.
    Sample p[1];
    const float r100 = 100;
    fly(cart("9x19_fmj"), 360.0f, BAL_DRAG_TABLE, 0, &r100, 1, p);
    CHECK(p[0].drop > 0.40f, "9mm drop@100 %.2f", p[0].drop);
    BalBullet b;
    bal_launch(&b, cart("45acp_fmj"), 1, v3(0, 0, 0), v3(1, 0, 0), 260.0f, 1, 0, false);
    CHECK(b.flags & BF_SUBSONIC, ".45 ACP is flagged subsonic");
}

static void test_zero_and_mv(void) {
    const BalCartridge *c = cart("556_m855");
    CHECK(fabsf(bal_cartridge_mv(c, 0.368f) - 900.0f) < 0.5f, "M855 mv at 368 mm %.1f", bal_cartridge_mv(c, 0.368f));
    CHECK(bal_cartridge_mv(c, 0.254f) < bal_cartridge_mv(c, 0.508f), "longer barrel is faster");
    CHECK(fabsf(cart("9x19_fmj")->mv_ref - 360.0f) < 0.5f && fabsf(cart("762x51_m80")->mv_ref - 850.0f) < 0.5f, "mv_ref");
    // Zero: the bullet launched at the solved elevation crosses the line of sight at 100 m.
    float ang = bal_zero_angle(c, 900.0f, 100.0f, 0.0f, BAL_DRAG_TABLE);
    BalEnv e = bal_env_make(15.0f, 0.0f, BAL_DRAG_TABLE);
    e.spin_drift = false;
    BalBullet b;
    bal_launch(&b, c, 0, v3(0, 0, 0), v3(cosf(ang), sinf(ang), 0), 900.0f, 1, 0, false);
    BalBullet prev = b;
    float y100 = 99, y50 = 0;
    while (bal_step(&b, c, &e, 1.0f / 60.0f)) {
        if (prev.pos.x < 50 && b.pos.x >= 50) y50 = lerpf(prev.pos.y, b.pos.y, (50 - prev.pos.x) / (b.pos.x - prev.pos.x));
        if (prev.pos.x < 100 && b.pos.x >= 100) { y100 = lerpf(prev.pos.y, b.pos.y, (100 - prev.pos.x) / (b.pos.x - prev.pos.x)); break; }
        prev = b;
    }
    CHECK(fabsf(y100) < 0.01f, "zero at 100 m: %.4f m", y100);
    CHECK(y50 > 0.0f, "above the line of sight before the zero (%.4f)", y50);
}

static void test_determinism(void) {
    const BalCartridge *c = cart("9x19_fmj");   // crosses the transonic band: random walk active
    BalEnv e = bal_env_make(20.0f, 0.0f, BAL_DRAG_TABLE);
    e.wind = v3(1.5f, 0, -2.0f);
    BalBullet a, b, d;
    bal_launch(&a, c, 0, v3(1, 2, 3), v3_norm(v3(1, 0.05f, 0.2f)), 380.0f, 0xC0FFEE, 0, false);
    bal_launch(&b, c, 0, v3(1, 2, 3), v3_norm(v3(1, 0.05f, 0.2f)), 380.0f, 0xC0FFEE, 0, false);
    bal_launch(&d, c, 0, v3(1, 2, 3), v3_norm(v3(1, 0.05f, 0.2f)), 380.0f, 0xC0FFEE, 1, false);
    for (int i = 0; i < 300; i++) {
        bal_step(&a, c, &e, 1.0f / 60.0f);
        bal_step(&b, c, &e, 1.0f / 60.0f);
        bal_step(&d, c, &e, 1.0f / 60.0f);
    }
    CHECK(!memcmp(&a.pos, &b.pos, sizeof(vec3)) && !memcmp(&a.vel, &b.vel, sizeof(vec3)), "same seed: bit-identical");
    CHECK(a.yaw > 0.0f, "transonic yaw built up (%.4f)", a.yaw);
    CHECK(!v3_eq(a.pos, d.pos), "another pellet takes another random walk");
}

// Cross-platform golden (docs/PLATFORM_MACOS.md "Determinism"): every cartridge, both drag models,
// wind, the transonic walk, the zero solve: the bits of the whole flight hashed. The golden value is
// from Linux x86_64; CI checks it on Windows (MSVC) and macOS arm64 / x86_64 (a Mac host and a PC
// client predict the same trajectories).
static u64 hash_bits(u64 h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= ((const u8 *)p)[i]; h *= 0x100000001B3ull; }
    return h;
}

static void test_cross_platform_golden(void) {
    u64 h = 0xCBF29CE484222325ull;
    for (int ci = 0; ci < g_data.cart_count; ci++) {
        const BalCartridge *c = &g_data.cart[ci];
        for (int model = 0; model < 2; model++) {
            BalEnv e = bal_env_make(12.0f + 3.0f * (float)ci, 150.0f * (float)model, model ? BAL_DRAG_TABLE : 0);
            e.wind = v3(2.5f, 0.0f, -1.25f);
            float speed = c->mv_ref > 0.0f ? c->mv_ref : 400.0f;
            float zero = bal_zero_angle(c, speed, 100.0f, 0.06f, e.drag_model);
            h = hash_bits(h, &zero, sizeof(zero));
            BalBullet b;
            bal_launch(&b, c, ci, v3(0, 1.5f, 0), v3_norm(v3(1.0f, dm_tanf(zero) + 0.01f * (float)model, 0.03f)), speed, 0xBADC0DEu + (u32)ci, model, false);
            for (int i = 0; i < 360 && bal_step(&b, c, &e, 1.0f / 60.0f); i++) {
                h = hash_bits(h, &b.pos, sizeof(vec3));
                h = hash_bits(h, &b.vel, sizeof(vec3));
                h = hash_bits(h, &b.yaw, sizeof(float));
            }
            float mach = bal_bullet_mach(&b, &e), energy = bal_bullet_energy(&b);
            h = hash_bits(h, &mach, sizeof(float));
            h = hash_bits(h, &energy, sizeof(float));
        }
    }
    const u64 golden = 0x1AE223078FDA6D4Bull;
    printf("  cross-platform trajectory hash %016llx (golden %016llx)\n", (unsigned long long)h, (unsigned long long)golden);
    CHECK(h == golden, "trajectory hash %016llx differs from the golden %016llx (FMA contraction / libm in a deterministic module?)",
          (unsigned long long)h, (unsigned long long)golden);
}

// ------------------------------------------------------------------ surfaces

// Bullets never ricochet: on every material, at every incidence down to a 0.5 deg graze, a hit
// stops in the surface or goes through it (the exit beyond the entry face), never back off it.
static void test_never_ricochets(void) {
    const char *carts[] = { "556_m855", "762x51_m80", "9x19_fmj" };
    const float angles[] = { 0.5f, 2.0f, 5.0f, 10.0f, 20.0f, 45.0f, 89.0f };
    const float thick[] = { 0.002f, 0.02f, 10.0f };
    int n = 0, stops = 0, through = 0;
    for (int ci = 0; ci < 3; ci++) {
        const BalCartridge *c = cart(carts[ci]);
        for (int m = 0; m < SURF_COUNT; m++) {
            for (int ai = 0; ai < 7; ai++) {
                float a = angles[ai] * DEG2RAD;
                vec3 dir = v3(cosf(a), -sinf(a), 0);
                for (int ti = 0; ti < 3; ti++) {
                    for (u32 s = 1; s <= 40; s++) {
                        BalBullet b;
                        bal_launch(&b, c, 0, v3(0, 0, 0), dir, 900.0f, s * 7919u + (u32)m, 0, false);
                        BalHitResult r = bal_hit_surface(&b, c, &g_data.mat[m], v3(0, 0, 0), dir, v3(0, 1, 0), thick[ti]);
                        n++;
                        CHECK(r.outcome == BAL_STOP || r.outcome == BAL_PENETRATE, "%s %s %.1f deg: outcome %d", carts[ci], bal_material_name(m), angles[ai], r.outcome);
                        if (r.outcome == BAL_STOP) {
                            stops++;
                            CHECK(r.energy_out == 0.0f && r.energy_deposit == r.energy_in, "a stop keeps all the energy in the target");
                            CHECK(!bal_apply_hit(&b, &r) && (b.flags & BF_DEAD), "a stopped bullet is gone");
                        } else {
                            through++;
                            CHECK(r.out_pos.y < 0.0f, "%s %s %.1f deg: the exit is beyond the entry face", carts[ci], bal_material_name(m), angles[ai]);
                            CHECK(r.energy_out < r.energy_in, "an exit carries less energy");
                            CHECK(bal_apply_hit(&b, &r) && (b.flags & BF_PENETRATED), "a penetration continues, flagged");
                        }
                    }
                }
            }
        }
    }
    CHECK(stops > 0 && through > 0, "both outcomes occur (%d stops, %d through of %d)", stops, through, n);
    // The old ricochet case (5.56 at 5 deg on thick steel, 2 cm): it stops in the plate, every seed.
    for (u32 s = 1; s <= 500; s++) {
        BalBullet b;
        float a = 5.0f * DEG2RAD;
        vec3 dir = v3(cosf(a), -sinf(a), 0);
        bal_launch(&b, cart("556_m855"), 0, v3(0, 0, 0), dir, 900.0f, s, 0, false);
        BalHitResult r = bal_hit_surface(&b, cart("556_m855"), &g_data.mat[SURF_METAL_THICK], v3(0, 0, 0), dir, v3(0, 1, 0), 0.02f);
        CHECK(r.outcome == BAL_STOP, "grazing steel: seed %u does not stop", s);
    }
}

static void test_penetration(void) {
    const BalCartridge *m855 = cart("556_m855"), *m80 = cart("762x51_m80"), *p9 = cart("9x19_fmj");
    const BalMaterial *steel = &g_data.mat[SURF_METAL_THICK], *wood = &g_data.mat[SURF_WOOD], *sand = &g_data.mat[SURF_SAND];
    const BalMaterial *flesh = &g_data.mat[SURF_FLESH];
    // The brief's table at the muzzle: 5.56 ~6 mm mild steel, ~12 cm pine, ~20 cm sandbag;
    // 7.62x51 about twice on steel; pistols much less.
    CHECK(fabsf(bal_capacity(steel, m855, m855->e_ref) - 0.006f) < 1e-4f, "M855 steel %.4f", bal_capacity(steel, m855, m855->e_ref));
    CHECK(fabsf(bal_capacity(wood, m855, m855->e_ref) - 0.12f) < 1e-3f, "M855 pine");
    CHECK(fabsf(bal_capacity(sand, m855, m855->e_ref) - 0.20f) < 1e-3f, "M855 sandbag");
    CHECK(fabsf(bal_capacity(steel, m80, m80->e_ref) / bal_capacity(steel, m855, m855->e_ref) - 2.0f) < 0.01f, "M80 = 2x M855 on steel");
    CHECK(bal_capacity(steel, p9, p9->e_ref) < 0.3f * bal_capacity(steel, m855, m855->e_ref), "9 mm much less on steel");
    // Linear in the energy left.
    CHECK(fabsf(bal_capacity(wood, m855, 0.5f * m855->e_ref) - 0.06f) < 1e-3f, "half energy, half depth");
    // Through 3 mm steel: exits with half the energy (6 mm capacity), wider cone, far side point.
    BalBullet b;
    float mv = m855->mv_ref;
    bal_launch(&b, m855, 0, v3(0, 0, 0), v3(1, 0, 0), mv, 3, 0, false);
    BalHitResult r = bal_hit_surface(&b, m855, steel, v3(10, 0, 0), v3(1, 0, 0), v3(-1, 0, 0), 0.003f);
    CHECK(r.outcome == BAL_PENETRATE, "M855 defeats 3 mm steel");
    CHECK(fabsf(r.energy_out - 0.5f * r.energy_in) < 1.0f, "exit energy %.0f of %.0f", r.energy_out, r.energy_in);
    CHECK(fabsf(r.out_pos.x - 10.005f) < 1e-3f, "exit point on the far face");
    CHECK(bal_apply_hit(&b, &r) && (b.flags & BF_PENETRATED), "continues penetrated");
    bal_launch(&b, m855, 0, v3(0, 0, 0), v3(1, 0, 0), mv, 3, 0, false);
    r = bal_hit_surface(&b, m855, steel, v3(10, 0, 0), v3(1, 0, 0), v3(-1, 0, 0), 0.012f);
    CHECK(r.outcome == BAL_STOP, "M855 stopped by 12 mm steel");
    bal_launch(&b, m80, 0, v3(0, 0, 0), v3(1, 0, 0), m80->mv_ref, 3, 0, false);
    r = bal_hit_surface(&b, m80, steel, v3(10, 0, 0), v3(1, 0, 0), v3(-1, 0, 0), 0.010f);
    CHECK(r.outcome == BAL_PENETRATE, "M80 defeats 10 mm steel");
    // Flesh: a 9 mm stops in a 30 cm torso but goes through a 10 cm limb; 5.56 above its
    // fragmentation speed exits a torso with fragments; 7.62x51 exits with energy to spare.
    bal_launch(&b, p9, 0, v3(0, 0, 0), v3(1, 0, 0), 360.0f, 5, 0, false);
    r = bal_hit_flesh(&b, p9, flesh, v3(5, 0, 0), v3(1, 0, 0), 0.30f, 0);
    CHECK(r.outcome == BAL_STOP && !r.fragment, "9 mm stops in a torso (cap %.3f)", r.capacity);
    r = bal_hit_flesh(&b, p9, flesh, v3(5, 0, 0), v3(1, 0, 0), 0.10f, 0);
    CHECK(r.outcome == BAL_PENETRATE, "9 mm through a limb");
    bal_launch(&b, m855, 0, v3(0, 0, 0), v3(1, 0, 0), 880.0f, 5, 0, false);
    BalHitResult rf = bal_hit_flesh(&b, m855, flesh, v3(5, 0, 0), v3(1, 0, 0), 0.30f, 0);
    CHECK(rf.outcome == BAL_PENETRATE && rf.fragment && rf.yawed, "M855 fragments, yaws and exits");
    bal_launch(&b, m855, 0, v3(0, 0, 0), v3(1, 0, 0), 600.0f, 5, 0, false);
    BalHitResult rn = bal_hit_flesh(&b, m855, flesh, v3(5, 0, 0), v3(1, 0, 0), 0.30f, 0);
    CHECK(!rn.fragment, "M855 below 760 m/s does not fragment");
    CHECK(rf.cavity > 2.0f * rn.cavity, "fragmenting cavity %.2f >> intact %.2f", rf.cavity, rn.cavity);
    bal_launch(&b, p9, 0, v3(0, 0, 0), v3(1, 0, 0), 360.0f, 5, 0, false);
    BalHitResult rp = bal_hit_flesh(&b, p9, flesh, v3(5, 0, 0), v3(1, 0, 0), 0.30f, 0);
    CHECK(rf.cavity > 2.0f * rp.cavity, "rifle cavity %.2f >> pistol %.2f", rf.cavity, rp.cavity);
    bal_launch(&b, m80, 0, v3(0, 0, 0), v3(1, 0, 0), 800.0f, 5, 0, false);
    r = bal_hit_flesh(&b, m80, flesh, v3(5, 0, 0), v3(1, 0, 0), 0.30f, 0);
    CHECK(r.outcome == BAL_PENETRATE && r.energy_out > 0.25f * r.energy_in, "M80 exits with %.0f J", r.energy_out);
}

// ------------------------------------------------------------------ acoustics

static void test_acoustics(void) {
    // Straight bullet at 900 m/s along +X, listener 300 m downrange, 5 m off the line.
    float c = 343.0f, v = 900.0f, x = 300.0f, r = 5.0f;
    float arr = 0, miss = 0;
    bool ok = bal_crack_segment(v3(0, 0, 0), v3(450, 0, 0), 0.0f, v, c, v3(x, 0, r), &arr, &miss);
    float m = c / v, s = x - r * m / sqrtf(1 - m * m);
    float expect = s / v + sqrtf((x - s) * (x - s) + r * r) / c;
    CHECK(ok && fabsf(arr - expect) < 1e-4f && fabsf(miss - r) < 1e-3f, "crack arrival %.4f expect %.4f", arr, expect);
    // The same answer when the path is split into tick segments (only the right one fires).
    int found = 0;
    float arr2 = 0;
    for (int i = 0; i < 30; i++) {
        float a2, m2;
        vec3 p0 = v3(15.0f * (float)i, 0, 0), p1 = v3(15.0f * (float)(i + 1), 0, 0);
        if (bal_crack_segment(p0, p1, (float)i * 15.0f / v, v, c, v3(x, 0, r), &a2, &m2)) { found++; arr2 = a2; }
    }
    CHECK(found == 1 && fabsf(arr2 - expect) < 1e-4f, "segmented crack found %d arrival %.4f", found, arr2);
    CHECK(!bal_crack_segment(v3(0, 0, 0), v3(450, 0, 0), 0, 300.0f, c, v3(x, 0, r), &arr, &miss), "no crack when subsonic");
    CHECK(bal_whizz_segment(v3(0, 0, 0), v3(450, 0, 0), 0, 300.0f, c, v3(x, 0, r), &arr, &miss) && fabsf(arr - (x / 300.0f + r / c)) < 1e-4f,
          "whizz at closest approach");
    // Real flight: the listener stands 300 m from the shooter, 5 m beside the line of fire; the
    // round hits a backstop at 320 m. Order heard: crack, impact, report.
    const BalCartridge *cc = cart("556_m855");
    BalEnv e = bal_env_make(20.0f, 0.0f, BAL_DRAG_TABLE);
    BalBullet b;
    bal_launch(&b, cc, 0, v3(0, 1.5f, 0), v3(1, 0, 0), 900.0f, 9, 0, false);
    vec3 L = v3(300, 1.5f, 5);
    float crack = -1, impact_t = -1;
    vec3 impact = v3(0, 0, 0);
    BalBullet prev = b;
    while (bal_step(&b, cc, &e, 1.0f / 60.0f)) {
        float a2, m2;
        if (crack < 0 && bal_crack_segment(prev.pos, b.pos, prev.time, 0.5f * (v3_len(prev.vel) + v3_len(b.vel)), e.sound_speed, L, &a2, &m2))
            crack = a2;
        if (b.pos.x >= 320.0f) {
            float f = (320.0f - prev.pos.x) / (b.pos.x - prev.pos.x);
            impact_t = lerpf(prev.time, b.time, f);
            impact = v3_lerp(prev.pos, b.pos, f);
            break;
        }
        prev = b;
    }
    float impact_heard = impact_t + v3_dist(impact, L) / e.sound_speed;
    float report = bal_report_delay(v3_dist(v3(0, 1.5f, 0), L), e.sound_speed);
    printf("  300 m, 5 m off the line: crack %.3f s, impact %.3f s, report %.3f s\n", crack, impact_heard, report);
    CHECK(crack > 0 && crack < impact_heard && impact_heard < report, "crack -> impact -> report");
    // Crack precedes the report by the computed interval (within one tick of integration).
    float tof = 0;
    {
        BalBullet q;
        bal_launch(&q, cc, 0, v3(0, 1.5f, 0), v3(1, 0, 0), 900.0f, 9, 0, false);
        BalBullet pq = q;
        while (bal_step(&q, cc, &e, 1.0f / 60.0f)) {
            if (q.pos.x >= 300.0f) { tof = lerpf(pq.time, q.time, (300.0f - pq.pos.x) / (q.pos.x - pq.pos.x)); break; }
            pq = q;
        }
    }
    CHECK(fabsf((report - crack) - (report - tof)) < 0.03f, "crack-report interval %.3f vs time-of-flight estimate %.3f",
          report - crack, report - tof);
    CHECK(bal_lowpass_hz(10) > 19000 && bal_lowpass_hz(900) < 2100 && bal_lowpass_hz(5000) == 1500, "air absorption low-pass");
    CHECK(bal_crack_gain(2) > bal_crack_gain(20) && bal_crack_variant(1) == 0 && bal_crack_variant(150) == 2, "N-wave level / length");
    CHECK(fabsf(bal_sound_speed(20) - 343.42f) < 0.01f, "speed of sound at 20 C");
}

// ------------------------------------------------------------------ tracers

static void test_tracers(void) {
    const BalTracerDef *t = &cart("556_m855")->tracer;   // every 5, last 3
    for (u32 seed = 1; seed < 64; seed++) {
        int count = 0, first = -1, prev = -100;
        bool spaced = true;
        for (int i = 0; i < 30; i++) {
            bool a = bal_tracer_round(t, seed * 2654435761u, i, 30, false, false);
            bool b = bal_tracer_round(t, seed * 2654435761u, i, 30, false, false);
            CHECK(a == b, "deterministic");
            if (i >= 27) { CHECK(a, "last 3 rounds are tracers"); continue; }
            if (a) {
                count++;
                if (first < 0) first = i;
                if (prev >= 0 && i - prev != 5) spaced = false;
                prev = i;
            }
        }
        CHECK(spaced && first < 5 && (count == 5 || count == 6), "every 5th round (seed %u: %d, first %d)", seed, count, first);
    }
    int phases[5] = { 0 };
    for (u32 seed = 1; seed < 200; seed++)
        for (int i = 0; i < 5; i++)
            if (bal_tracer_round(t, seed, i, 30, false, false)) phases[i]++;
    for (int i = 0; i < 5; i++) CHECK(phases[i] > 15, "mag seed varies the phase (%d at %d)", phases[i], i);
    CHECK(bal_tracer_round(t, 1, 1, 30, true, false) && bal_tracer_round(t, 1, 2, 30, false, true), "all-tracer mag / forced");
    BalTracerDef none = { 0 };
    CHECK(!bal_tracer_round(&none, 1, 0, 30, false, false), "no tracer loading");
    // Look: igniter dim near the muzzle, full burn, burnout, dark.
    BalTracerLook l = bal_tracer_look(t, 30.0f, 0.04f, false);
    CHECK(l.state == BAL_TRS_IGNITER && l.intensity < 0.3f, "igniter at 30 m");
    CHECK(bal_tracer_look(t, 30.0f, 0.04f, true).state == BAL_TRS_BURNING, "arcade: no ignition delay");
    CHECK(bal_tracer_look(t, 300.0f, 0.4f, false).state == BAL_TRS_BURNING, "burning at 300 m");
    CHECK(bal_tracer_look(t, 900.0f, 2.0f, false).state == BAL_TRS_DARK, "dark after burn out");
    CHECK(fabsf(bal_tracer_streak(900.0f, 1.0f / 60.0f) - 15.0f) < 0.01f, "streak = v / 60");
}

// ------------------------------------------------------------------ muzzle flash

static BalMuzzleOut compose_dev(const char *cart_id, const BalDevice *dev, float barrel, bool pop) {
    BalMuzzleIn in;
    memset(&in, 0, sizeof(in));
    in.cart = cart(cart_id);
    in.dev = dev;
    in.barrel = barrel;
    in.speed = bal_cartridge_mv(in.cart, barrel);
    in.exposure_gain = 1.0f;
    in.pop = pop;
    BalMuzzleOut o;
    bal_muzzle_compose(&in, &o);
    return o;
}

static BalMuzzleOut compose(const char *cart_id, const char *dev_id, float barrel, bool pop) {
    return compose_dev(cart_id, &g_data.dev[bal_device_find(&g_data, dev_id)], barrel, pop);
}

static void test_muzzle(void) {
    BalMuzzleOut bare = compose("556_m855", "bare", 0.368f, false);
    BalDevice unit = g_data.dev[0];   // every component at 1: the physics before any device
    for (int i = 0; i < BFC_COUNT; i++) unit.mul[i] = 1.0f;
    BalMuzzleOut base = compose_dev("556_m855", &unit, 0.368f, false);
    struct { const char *id; float sec, vents, prongs, vapor; } want[] = {
        { "flash_hider", 0.2f, 0, 0.35f, 0 },
        { "compensator", 0.7f, 0.8f, 0, 0 },
        { "muzzle_brake", 0.7f, 0.8f, 0, 0 },
        { "suppressor", 0.0f, 0, 0, 1 },
    };
    for (int i = 0; i < 4; i++) {
        BalMuzzleOut o = compose("556_m855", want[i].id, 0.368f, false);
        CHECK(fabsf(o.c[BFC_SECONDARY].intensity - want[i].sec * base.c[BFC_SECONDARY].intensity) < 1e-4f, "%s secondary x%.2f", want[i].id, want[i].sec);
        CHECK(fabsf(o.c[BFC_VENTS].intensity - want[i].vents * base.c[BFC_VENTS].intensity) < 1e-4f, "%s vents", want[i].id);
        CHECK(fabsf(o.c[BFC_PRONGS].intensity - want[i].prongs * base.c[BFC_PRONGS].intensity) < 1e-4f, "%s prongs", want[i].id);
        CHECK(fabsf(o.c[BFC_VAPOR].intensity - want[i].vapor * base.c[BFC_VAPOR].intensity) < 1e-4f, "%s vapor", want[i].id);
    }
    CHECK(bare.c[BFC_VENTS].intensity == 0 && bare.c[BFC_PRONGS].intensity == 0 && bare.c[BFC_VAPOR].intensity == 0, "bare has no device components");
    CHECK(compose("556_m855", "compensator", 0.368f, false).c[BFC_BLAST].intensity > bare.c[BFC_BLAST].intensity, "brake: bigger blast");
    // Suppressor pop: the cold can gives a brief flash.
    CHECK(compose("556_m855", "suppressor", 0.368f, true).c[BFC_SECONDARY].intensity > 0.5f, "first-round pop flash");
    // Geometry from the physics: the shock bottle sits 2-6 cm out, the secondary 10-50 cm; a short
    // barrel or a big charge makes a bigger secondary; pistols spit more sparks.
    CHECK(bare.c[BFC_MACH].dist >= 0.02f && bare.c[BFC_MACH].dist <= 0.06f, "mach disk %.3f m", bare.c[BFC_MACH].dist);
    CHECK(bare.c[BFC_SECONDARY].dist >= 0.10f && bare.c[BFC_SECONDARY].dist <= 0.50f, "secondary at %.3f m", bare.c[BFC_SECONDARY].dist);
    BalMuzzleOut sbr = compose("556_m855", "bare", 0.254f, false), lng = compose("556_m855", "bare", 0.508f, false);
    CHECK(sbr.c[BFC_SECONDARY].size > lng.c[BFC_SECONDARY].size, "short barrel: bigger secondary");
    CHECK(compose("762x51_m80", "bare", 0.508f, false).c[BFC_SECONDARY].size > lng.c[BFC_SECONDARY].size, "bigger charge: bigger secondary");
    BalMuzzleOut pis = compose("9x19_fmj", "bare", 0.102f, false);
    CHECK(pis.c[BFC_SPARKS].count > lng.c[BFC_SPARKS].count, "pistol sparks %.2f > rifle %.2f", pis.c[BFC_SPARKS].count, lng.c[BFC_SPARKS].count);
    CHECK(bare.c[BFC_MACH].rgb.z > compose("9x19_fmj", "bare", 0.102f, false).c[BFC_MACH].rgb.z, "high-velocity blue tinge");
    // Exposure: the perceived daylight flash ≤ 0.2x night.
    // Night (EV at the night floor, no daylight) vs full sun at a few day exposures.
    float night = bal_flash_perceived(-1.5f, -1.5f, 0.0f);
    for (float ev = 0.5f; ev <= 7.0f; ev += 1.5f) {
        float day = bal_flash_perceived(ev, -1.5f, 1.0f);
        CHECK(day <= 0.2f * night, "day (EV %.1f) %.3f vs night %.3f", ev, day, night);
    }
    CHECK(bal_flash_perceived(0.0f, -1.5f, 0.5f) > bal_flash_perceived(0.0f, -1.5f, 1.0f), "dusk in between");
    vec3 bb = bal_blackbody_rgb(2500);
    CHECK(bb.x == 1.0f && bb.z < bb.y && bb.y < bb.x, "2500 K is yellow-orange (%.2f %.2f %.2f)", bb.x, bb.y, bb.z);
    // First-round pop: once per cold suppressor, again after it has cooled.
    const BalDevice *sup = &g_data.dev[bal_device_find(&g_data, "suppressor")];
    const BalCartridge *c = cart("556_m855");
    BalMuzzleHeat h;
    memset(&h, 0, sizeof(h));
    int pops = 0;
    for (int i = 0; i < 30; i++) pops += bal_muzzle_heat_shot(&h, c, sup, 0.08 * i);
    CHECK(pops == 1, "one pop per cold can (%d)", pops);
    CHECK(bal_muzzle_can_heat_at(&h, sup, 2.4) >= sup->can_heat_rounds, "can heat haze after a burst");
    CHECK(bal_muzzle_heat_shot(&h, c, sup, 2.4 + sup->pop_cold_s + 0.1), "pops again after cooling");
    CHECK(!bal_muzzle_heat_shot(&h, c, sup, 2.4 + sup->pop_cold_s + 0.2), "and not twice");
    BalMuzzleHeat hb;
    memset(&hb, 0, sizeof(hb));
    CHECK(!bal_muzzle_heat_shot(&hb, c, &g_data.dev[0], 0), "bare never pops");
}

static void test_budget(void) {
    // The pure step (integration, drag, transonic, spin drift) for 500 bullets; the world ray is
    // measured in game (ballistics_bench). Generous bound: only catches pathological regressions.
    enum { N = 500, STEPS = 120 };
    static BalBullet b[N];
    const BalCartridge *c = cart("556_m855");
    BalEnv e = bal_env_make(15.0f, 0.0f, BAL_DRAG_TABLE);
    for (int i = 0; i < N; i++) bal_launch(&b[i], c, 0, v3(0, 2, 0), v3_norm(v3(1, 0.01f * (float)(i % 7), 0.003f * (float)i)), 900.0f, (u32)i, 0, false);
    clock_t t0 = clock();
    int steps = 0;
    for (int s = 0; s < STEPS; s++)
        for (int i = 0; i < N; i++)
            if (bal_step(&b[i], c, &e, 1.0f / 60.0f)) steps++;
    double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6;
    double per = us / (double)(steps > 0 ? steps : 1);
    printf("  pure step: %.3f us per projectile step (%d steps)\n", per, steps);
    CHECK(per < 1.0, "pure step %.3f us", per);
}

int main(void) {
    load_data();
    test_trajectories();
    test_zero_and_mv();
    test_determinism();
    test_cross_platform_golden();
    test_never_ricochets();
    test_penetration();
    test_acoustics();
    test_tracers();
    test_muzzle();
    test_budget();
    printf("test_ballistics: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
