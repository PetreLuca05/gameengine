// Unit tests for the pure recoil / spread / spring math (gameplay/recoil.c, core/spring.c).
// Build: part of the normal CMake build (target test_recoil); run: ctest or build/bin/test_recoil.
#include "gameplay/recoil.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

static RecoilMods mods_neutral(void) {
    RecoilMods m;
    memset(&m, 0, sizeof(m));
    m.fire_mode = FM_AUTO;
    m.extra = 1;
    m.scale = 1;
    return m;
}

// Drives the state like the game does: the view (what the command carries) gets every emitted
// delta plus the player's own input.
typedef struct Sim {
    RecoilParams p;
    RecoilState s;
    float view_pitch, view_yaw;
} Sim;

static void sim_init(Sim *sim, u32 seed) {
    memset(sim, 0, sizeof(*sim));
    recoil_params_default(&sim->p);
    recoil_reset(&sim->s, seed);
}

static void sim_tick(Sim *sim, int shots, float input_pitch, float input_yaw) {
    RecoilMods m = mods_neutral();
    sim->view_pitch += input_pitch;
    sim->view_yaw += input_yaw;
    recoil_observe_input(&sim->p, &sim->s, sim->view_pitch, sim->view_yaw);
    RecoilKick total = { 0, 0 };
    for (int i = 0; i < shots; i++) {
        RecoilKick k = recoil_shot(&sim->p, &sim->s, &m);
        total.pitch += k.pitch;
        total.yaw += k.yaw;
    }
    RecoilKick r = recoil_tick(&sim->p, &sim->s, 1.0f / 60.0f);
    total.pitch += r.pitch;
    total.yaw += r.yaw;
    recoil_set_emitted(&sim->s, total);
    sim->view_pitch += total.pitch;
    sim->view_yaw += total.yaw;
}

static void test_determinism(void) {
    RecoilParams p;
    recoil_params_default(&p);
    p.pattern_randomness = 0.5f;
    bool differs = false;
    for (int i = 0; i < 40; i++) {
        RecoilKick a = recoil_pattern_step(&p, 1234, i), b = recoil_pattern_step(&p, 1234, i);
        CHECK(a.pitch == b.pitch && a.yaw == b.yaw, "pattern step %d not deterministic", i);
        RecoilKick c = recoil_pattern_step(&p, 999, i);
        if (c.pitch != a.pitch || c.yaw != a.yaw) differs = true;
    }
    CHECK(differs, "different seeds should give different patterns");
    CHECK(recoil_mag_seed(10, 2) == recoil_mag_seed(10, 2), "mag seed not deterministic");
    CHECK(recoil_mag_seed(10, 2) != recoil_mag_seed(11, 2), "mag seed ignores input");
}

// Cross-platform golden (docs/PLATFORM_MACOS.md "Determinism"): pattern steps, shots with every
// modifier and the recovery ticks of a 60-round string hashed bit for bit; golden from Linux x86_64,
// checked in CI on Windows and macOS arm64 / x86_64.
static void test_cross_platform_golden(void) {
    u64 h = 0xCBF29CE484222325ull;
    RecoilParams p;
    recoil_params_default(&p);
    p.pattern_randomness = 0.6f;
    RecoilState s;
    memset(&s, 0, sizeof(s));   // recoil_reset keeps the input tracking of the previous state
    recoil_reset(&s, recoil_mag_seed(77, 3));
    RecoilMods m;
    memset(&m, 0, sizeof(m));
    m.ads_t = 0.3f;
    m.move01 = 0.2f;
    m.extra = 1.0f;
    for (int i = 0; i < 60; i++) {
        RecoilKick a = recoil_pattern_step(&p, 4242, i), k = recoil_shot(&p, &s, &m), t = recoil_tick(&p, &s, 1.0f / 60.0f);
        float sp = recoil_spread(&p, &s, &m);
        float v[7] = { a.pitch, a.yaw, k.pitch, k.yaw, t.pitch, t.yaw, sp };
        for (size_t b = 0; b < sizeof(v); b++) { h ^= ((const u8 *)v)[b]; h *= 0x100000001B3ull; }
    }
    const u64 golden = 0x6BD1B0A74189FDB1ull;
    printf("cross-platform recoil hash %016llx (golden %016llx)\n", (unsigned long long)h, (unsigned long long)golden);
    CHECK(h == golden, "recoil hash %016llx differs from the golden %016llx", (unsigned long long)h, (unsigned long long)golden);
}

static void test_ramp_and_pattern(void) {
    RecoilParams p;
    recoil_params_default(&p);
    p.pattern_randomness = 0;
    p.v_first = 2.0f;
    p.v_per = 0.5f;
    p.ramp_shots = 4;
    p.h_per = 0.3f;
    p.h_bias = 0;
    p.pattern_count = 2;
    p.pattern[0] = v2(1, 1);
    p.pattern[1] = v2(-1, 0.5f);
    RecoilKick k0 = recoil_pattern_step(&p, 1, 0);
    CHECK(NEAR(k0.pitch, 2.0f, 1e-5f), "first shot pitch %f", k0.pitch);
    CHECK(NEAR(k0.yaw, 0.3f, 1e-5f), "first shot yaw %f", k0.yaw);
    RecoilKick k1 = recoil_pattern_step(&p, 1, 1);
    CHECK(NEAR(k1.pitch, (2.0f + (0.5f - 2.0f) * 0.25f) * 0.5f, 1e-5f), "ramp shot 1 pitch %f", k1.pitch);
    CHECK(NEAR(k1.yaw, -0.3f, 1e-5f), "pattern shot 1 yaw %f", k1.yaw);
    RecoilKick k9 = recoil_pattern_step(&p, 1, 9);
    CHECK(NEAR(k9.pitch, 0.5f * 0.5f, 1e-5f), "steady shot pitch %f (last pattern entry repeats)", k9.pitch);
    // Full randomness: inside [min, max] * base.
    p.pattern_randomness = 1;
    p.v_min = 0.8f;
    p.v_max = 1.2f;
    p.h_min = -0.4f;
    p.h_max = 0.4f;
    for (int i = 4; i < 200; i++) {
        RecoilKick k = recoil_pattern_step(&p, 77, i);
        CHECK(k.pitch >= 0.5f * 0.8f - 1e-5f && k.pitch <= 0.5f * 1.2f + 1e-5f, "random pitch %f out of range", k.pitch);
        CHECK(k.yaw >= -0.4f - 1e-5f && k.yaw <= 0.4f + 1e-5f, "random yaw %f out of range", k.yaw);
    }
}

static void test_modifiers(void) {
    RecoilParams p;
    recoil_params_default(&p);
    RecoilMods m = mods_neutral();
    float base = recoil_aim_mult(&p, &m);
    CHECK(NEAR(base, 1.0f, 1e-6f), "neutral mult %f", base);
    m.ads_t = 1;
    CHECK(NEAR(recoil_aim_mult(&p, &m), p.ads_mult, 1e-6f), "ads mult");
    m.crouched = true;
    CHECK(NEAR(recoil_aim_mult(&p, &m), p.ads_mult * p.crouch_mult, 1e-6f), "ads+crouch mult");
    m = mods_neutral();
    m.scale = 0.5f;
    CHECK(NEAR(recoil_aim_mult(&p, &m), 0.5f, 1e-6f), "global scale");
    // Spread: ADS tighter than hip, moving / air wider.
    RecoilState s;
    memset(&s, 0, sizeof(s));
    m = mods_neutral();
    float hip = recoil_spread(&p, &s, &m);
    m.ads_t = 1;
    float ads = recoil_spread(&p, &s, &m);
    CHECK(ads < hip, "ads spread %f !< hip %f", ads, hip);
    m = mods_neutral();
    m.move01 = 1;
    CHECK(recoil_spread(&p, &s, &m) > hip, "moving spread");
    m = mods_neutral();
    m.scale = 0;
    m.extra = 1;
    // scale <= 0 means "unset" -> 1
    CHECK(NEAR(recoil_spread(&p, &s, &m), hip, 1e-6f), "unset scale treated as 1");
}

static void test_recovery_uncountered(void) {
    Sim sim;
    sim_init(&sim, 42);
    sim.p.recovery_fraction = 0.8f;
    sim.p.pattern_randomness = 0.5f;
    sim_tick(&sim, 0, 0, 0);   // establish the input baseline
    float start_p = sim.view_pitch, start_y = sim.view_yaw;
    for (int i = 0; i < 10; i++) sim_tick(&sim, 1, 0, 0);
    float acc_p = sim.s.acc_pitch, acc_y = sim.s.acc_yaw;
    CHECK(acc_p > 1.0f, "accumulated pitch %f", acc_p);
    CHECK(NEAR(sim.view_pitch - start_p, acc_p, 1e-3f), "view moved by acc %f vs %f", sim.view_pitch - start_p, acc_p);
    for (int i = 0; i < 600 && (sim.s.recovering || sim.s.acc_pitch != 0); i++) sim_tick(&sim, 0, 0, 0);
    CHECK(!sim.s.recovering && sim.s.acc_pitch == 0 && sim.s.shot_index == 0, "recovery did not finish");
    float net_p = sim.view_pitch - start_p, net_y = sim.view_yaw - start_y;
    CHECK(NEAR(net_p, acc_p * 0.2f, 2e-3f), "net pitch %f expected %f", net_p, acc_p * 0.2f);
    CHECK(NEAR(net_y, acc_y * 0.2f, 2e-3f), "net yaw %f expected %f", net_y, acc_y * 0.2f);
}

static void test_recovery_countered(void) {
    // Player pulls down 60% of the climb while firing: recovery returns fraction of the rest.
    Sim sim;
    sim_init(&sim, 7);
    sim.p.recovery_fraction = 1.0f;
    sim.p.h_min = sim.p.h_max = sim.p.h_per = sim.p.h_bias = 0;
    sim_tick(&sim, 0, 0, 0);
    float start = sim.view_pitch;
    float pulled = 0;
    for (int i = 0; i < 12; i++) {
        float before = sim.s.acc_pitch;
        sim_tick(&sim, 1, 0, 0);
        float added = sim.s.acc_pitch - before;
        // Next tick the player counters 60% of what was just added (with the next shot).
        float pull = -added * 0.6f;
        pulled += -pull;
        sim.view_pitch += pull;
    }
    sim_tick(&sim, 0, 0, 0);   // last pull observed
    float acc = sim.s.acc_pitch;
    CHECK(NEAR(sim.s.countered_pitch, pulled, 2e-3f), "countered %f expected %f", sim.s.countered_pitch, pulled);
    for (int i = 0; i < 600 && sim.s.acc_pitch != 0; i++) sim_tick(&sim, 0, 0, 0);
    // fraction 1: the uncountered part fully returns, so the view ends at the start aim.
    CHECK(NEAR(sim.view_pitch - start, 0.0f, 3e-3f), "net pitch %f (acc %f, pulled %f)", sim.view_pitch - start, acc, pulled);
}

static void test_overcorrect_and_cancel(void) {
    // Over-pulling never counts more than the accumulation, and a new shot cancels recovery.
    Sim sim;
    sim_init(&sim, 9);
    sim_tick(&sim, 0, 0, 0);
    for (int i = 0; i < 5; i++) sim_tick(&sim, 1, 0, 0);
    sim_tick(&sim, 0, -50.0f, 0);
    CHECK(sim.s.countered_pitch <= sim.s.acc_pitch + 1e-5f, "countered %f > acc %f", sim.s.countered_pitch, sim.s.acc_pitch);
    Sim b;
    sim_init(&b, 9);
    sim_tick(&b, 0, 0, 0);
    for (int i = 0; i < 5; i++) sim_tick(&b, 1, 0, 0);
    for (int i = 0; i < 12; i++) sim_tick(&b, 0, 0, 0);   // past the delay: recovering
    CHECK(b.s.recovering, "should be recovering");
    u16 idx = b.s.shot_index;
    sim_tick(&b, 1, 0, 0);
    CHECK(!b.s.recovering, "shot should cancel recovery");
    CHECK(b.s.shot_index == idx + 1, "pattern continues %u -> %u", idx, b.s.shot_index);
}

static void test_cone(void) {
    Rng rng = rng_make(12345);
    vec3 dirs[3] = { v3(0, 0, -1), v3_norm(v3(0.3f, 0.9f, -0.2f)), v3(0, 1, 0) };
    float cones[4] = { 0.1f, 1.0f, 4.0f, 12.0f };
    for (int d = 0; d < 3; d++) {
        for (int c = 0; c < 4; c++) {
            float half = cones[c] * 0.5f;
            float max_off = 0, sum = 0;
            int n = 20000;
            for (int i = 0; i < n; i++) {
                vec3 o = recoil_cone_dir(dirs[d], cones[c], &rng);
                float off = atan2f(v3_len(v3_cross(o, dirs[d])), v3_dot(o, dirs[d])) * DG_RAD2DEG;
                max_off = fmaxf(max_off, off);
                sum += off / half;
            }
            CHECK(max_off <= half * 1.0005f + 1e-4f, "cone %.2f: bullet at %.4f deg > half %.4f", cones[c], max_off, half);
            CHECK(max_off > half * 0.9f, "cone %.2f never reaches the edge (%.4f)", cones[c], max_off);
            float mean = sum / (float)n;
            CHECK(mean > 0.2f && mean < 0.6f, "cone %.2f mean radius %.3f (center weighted)", cones[c], mean);
        }
    }
    vec3 same = recoil_cone_dir(v3(0, 0, -1), 0, &rng);
    CHECK(same.z == -1.0f, "zero cone must not deviate");
}

static void test_fire_budget(void) {
    float rpms[5] = { 48, 400, 600, 750, 900 };
    for (int r = 0; r < 5; r++) {
        float cd = 0;
        int shots = 0;
        for (int t = 0; t < 600; t++) shots += recoil_fire_budget(&cd, 60.0f / rpms[r], 1.0f / 60.0f, true, 8);
        int expect = (int)(rpms[r] / 6.0f);   // 10 s
        CHECK(abs(shots - expect) <= 1, "%.0f rpm: %d shots in 10 s, expected %d", rpms[r], shots, expect);
    }
    // 4800 rpm at 60 Hz needs 2 shots in some ticks.
    float cd = 0;
    int two = 0, total = 0;
    for (int t = 0; t < 60; t++) {
        int n = recoil_fire_budget(&cd, 60.0f / 4800.0f, 1.0f / 60.0f, true, 8);
        total += n;
        if (n >= 2) two++;
    }
    CHECK(abs(total - 80) <= 1 && two > 0, "4800 rpm: %d shots / s, multi-shot ticks %d", total, two);
    // Released trigger: no banked shots.
    cd = 0.1f;
    for (int t = 0; t < 120; t++) recoil_fire_budget(&cd, 0.1f, 1.0f / 60.0f, false, 8);
    int n = recoil_fire_budget(&cd, 0.1f, 1.0f / 60.0f, true, 8);
    CHECK(n == 1, "after a pause the first tick fires exactly one shot (got %d)", n);
}

static void test_springs(void) {
    RecoilParams p;
    recoil_params_default(&p);
    RecoilVisual v;
    memset(&v, 0, sizeof(v));
    recoil_visual_kick(&p, &v, 1.0f, 0.0f, 5);
    float peak = 0;
    for (int i = 0; i < 180; i++) {
        recoil_visual_step(&p, &v, 1.0f / 60.0f);
        peak = fmaxf(peak, v.cam_rot.x.x);
    }
    CHECK(peak > p.cam_pitch * 0.4f && peak < p.cam_pitch * 2.0f, "camera kick peak %f vs configured %f", peak, p.cam_pitch);
    CHECK(fabsf(v.cam_rot.x.x) < 1e-3f && fabsf(v.vm_pos.x.z) < 1e-4f, "springs did not settle (%f, %f)", v.cam_rot.x.x, v.vm_pos.x.z);
    RecoilVisual z;
    memset(&z, 0, sizeof(z));
    recoil_visual_kick(&p, &z, 0.0f, 0.0f, 5);
    recoil_visual_step(&p, &z, 1.0f / 60.0f);
    CHECK(z.cam_rot.x.x == 0 && z.vm_rot.x.x == 0, "visual scale 0 must not kick");
    SpringF32 s = { 1.0f, 0, 1.0f };
    for (int i = 0; i < 240; i++) spring_f32_step(&s, (SpringParams){ 400, 40 }, 1.0f / 60.0f);
    CHECK(fabsf(s.x) < 1e-4f, "stiff spring unstable or slow: %f", s.x);
}

int main(void) {
    test_determinism();
    test_cross_platform_golden();
    test_ramp_and_pattern();
    test_modifiers();
    test_recovery_uncountered();
    test_recovery_countered();
    test_overcorrect_and_cancel();
    test_cone();
    test_fire_budget();
    test_springs();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
