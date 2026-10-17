// Water unit tests (docs/WATER.md): the deterministic polynomials, the swell model (spectrum,
// steepness, phase continuity under wind changes, determinism: in process, golden hash, `--hash` for
// the cross-process ctest), body shapes and queries (priority, depth, shore fade, JSON), and
// buoyancy: the pure force model analytically, then real Jolt bodies (half-submersion at density
// 0.5, drag time constant, sinking, waves) and the 200-body budget.
#include "world/water/water.h"
#include "physics/buoyancy.h"
#include "physics/physics.h"
#include "core/engine.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
// physics.c debug draw (no renderer in tests)
void dbg_line(vec3 a, vec3 b, u32 color, float duration, bool depth) { (void)a; (void)b; (void)color; (void)duration; (void)depth; }
void dbg_capsule_axis(vec3 a, vec3 b, float r, u32 color, float duration) { (void)a; (void)b; (void)r; (void)color; (void)duration; }
void dbg_box(vec3 c, vec3 h, quat q, u32 color, float duration) { (void)c; (void)h; (void)q; (void)color; (void)duration; }
void dbg_sphere(vec3 c, float r, u32 color, float duration) { (void)c; (void)r; (void)color; (void)duration; }
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

static u32 fnv(u32 h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

// Heights over a 1 km grid at a few ticks for a fixed (seed, wind): the determinism fingerprint.
static u32 wave_hash(void) {
    static WaterWorld w;
    water_world_init(&w, 1234u);
    WaterBody sea;
    memset(&sea, 0, sizeof(sea));
    sea.kind = WATER_OCEAN;
    sea.shape = WSHAPE_INFINITE;
    sea.p = water_params_default(WATER_OCEAN);
    water_add_body(&w, &sea);
    water_set_wind(&w, 10.0f, 63.0f);
    u32 h = 2166136261u;
    const u32 ticks[4] = { 0, 1234, 60u * 3600u, 4000000000u };
    for (int k = 0; k < 4; k++) {
        float t = waves_time(ticks[k], 0.5f, SIM_HZ);
        for (int z = -500; z <= 500; z += 25)
            for (int x = -500; x <= 500; x += 25) {
                WaterSample s;
                water_sample(&w, (float)x + 0.37f, (float)z - 0.81f, t, &s);
                h = fnv(h, &s.height, sizeof(float));
                h = fnv(h, s.normal, sizeof(s.normal));
                h = fnv(h, s.vel, sizeof(s.vel));
            }
    }
    return h;
}

// ------------------------------------------------------------------ math

static void test_math(void) {
    float es = 0.0f, ec = 0.0f, ee = 0.0f, el = 0.0f;
    for (int i = -200000; i <= 200000; i++) {
        float x = (float)i * 0.025f;                 // +-5000 rad: the range wave phases reach
        float s, c;
        wsincos(x, &s, &c);
        es = fmaxf(es, fabsf(s - (float)sin((double)x)));
        ec = fmaxf(ec, fabsf(c - (float)cos((double)x)));
    }
    for (int i = -870; i <= 880; i++) {
        float x = (float)i * 0.1f;
        double ref = exp((double)x);
        ee = fmaxf(ee, (float)(fabs((double)wexp(x) - ref) / ref));
    }
    for (int i = 1; i < 100000; i++) {
        float x = (float)i * 0.0137f;
        el = fmaxf(el, fabsf(wlog(x) - (float)log((double)x)));
    }
    CHECK(es < 2e-6f && ec < 2e-6f, "wsincos within 2e-6 over +-5000 rad (sin %g cos %g)", es, ec);
    CHECK(ee < 2e-6f, "wexp relative error %g", ee);
    CHECK(el < 2e-6f, "wlog error %g", el);
    printf("  polynomials: sin %.2g cos %.2g exp %.2g log %.2g\n", es, ec, ee, el);
}

// ------------------------------------------------------------------ waves

static void test_waves(void) {
    static WaveModel m;
    waves_init(&m, 99u);
    // Frequencies quantized to the repeat period, wavelengths inside the swell band.
    bool quant = true, band = true;
    for (int i = 0; i < WAVE_MAX; i++) {
        double n = (double)m.comp[i].omega * WAVE_REPEAT / (2.0 * 3.14159265358979);
        if (fabs(n - floor(n + 0.5)) > 1e-3) quant = false;
        float len = 6.2831853f / m.comp[i].k;
        if (len < WAVE_MIN_LEN * 0.95f || len > WAVE_MAX_LEN * 1.05f) band = false;
    }
    CHECK(quant, "every omega is a multiple of 2 pi / WAVE_REPEAT");
    CHECK(band, "wavelengths in [%g, %g] m", WAVE_MIN_LEN, WAVE_MAX_LEN);
    // Time wraps exactly: the field at t and t + WAVE_REPEAT is the same.
    CHECK(waves_time(5u, 0.25f, 60) == waves_time(5u + WAVE_REPEAT * 60u, 0.25f, 60), "time wraps at WAVE_REPEAT");

    WaveParams p = wave_params_default();
    float hs_prev = 0.0f;
    bool grows = true, steep_ok = true;
    const float winds[5] = { 2, 5, 10, 15, 20 };
    for (int i = 0; i < 5; i++) {
        p.wind = winds[i];
        waves_set_params(&m, &p);
        if (m.hs < hs_prev) grows = false;
        if (m.steepness >= 0.9f) steep_ok = false;
        hs_prev = m.hs;
        printf("  wind %4.1f m/s: Hs %.2f m, peak %.1f m, %d active, steepness %.2f\n", winds[i], m.hs, m.peak_len, m.active_n, m.steepness);
        if (winds[i] == 10.0f) CHECK(m.hs > 0.4f && m.hs < 1.6f, "Hs at 10 m/s, 30 km fetch: %.2f m (expected ~0.5..1.5)", m.hs);
    }
    CHECK(grows, "Hs grows with the wind");
    CHECK(steep_ok, "steepness < 0.9 (no looping crests)");

    // Wind change re-weights amplitudes only: phases and wave vectors stay, so the field at a point
    // changes continuously (a small wind step makes a small height step).
    p.wind = 10.0f;
    waves_set_params(&m, &p);
    float h0 = waves_height(&m, 13.0f, -7.0f, 100.0f, 2);
    p.wind = 10.05f;
    waves_set_params(&m, &p);
    float h1 = waves_height(&m, 13.0f, -7.0f, 100.0f, 2);
    CHECK(fabsf(h1 - h0) < 0.03f, "0.05 m/s wind step moves the surface %.4f m", fabsf(h1 - h0));

    // Inversion: the displaced point of the solved x0 lands on the queried x.
    p.wind = 14.0f;
    waves_set_params(&m, &p);
    float worst = 0.0f;
    for (int i = 0; i < 400; i++) {
        float x = (float)(i % 20) * 7.3f - 70.0f, z = (float)(i / 20) * 6.1f - 60.0f, t = 37.0f;
        float px = x, pz = z, d[3];
        for (int it = 0; it < 2; it++) { waves_displace(&m, px, pz, t, d); px = x - d[0]; pz = z - d[2]; }
        waves_displace(&m, px, pz, t, d);
        worst = fmaxf(worst, hypotf(px + d[0] - x, pz + d[2] - z));
    }
    CHECK(worst < 0.15f, "2 fixed-point steps invert the horizontal displacement within 15 cm (%.3f)", worst);

    // Sample consistency: the normal matches the finite-difference slope of the height.
    WaveSample s;
    waves_sample(&m, 3.0f, 4.0f, 21.0f, &s);
    float e = 0.05f;
    float hx = (waves_height(&m, 3.0f + e, 4.0f, 21.0f, 4) - waves_height(&m, 3.0f - e, 4.0f, 21.0f, 4)) / (2 * e);
    float hz = (waves_height(&m, 3.0f, 4.0f + e, 21.0f, 4) - waves_height(&m, 3.0f, 4.0f - e, 21.0f, 4)) / (2 * e);
    CHECK(fabsf(-s.normal[0] / s.normal[1] - hx) < 0.05f && fabsf(-s.normal[2] / s.normal[1] - hz) < 0.05f,
          "normal slope (%.3f, %.3f) vs finite difference (%.3f, %.3f)", -s.normal[0] / s.normal[1], -s.normal[2] / s.normal[1], hx, hz);
    // Vertical orbital velocity matches the time derivative of the height (on a calm crest-free point).
    float dt = 1e-2f;
    float dh = (waves_height(&m, 3.0f, 4.0f, 21.0f + dt, 4) - waves_height(&m, 3.0f, 4.0f, 21.0f - dt, 4)) / (2 * dt);
    float vy_expect = dh + s.vel[0] * hx + s.vel[2] * hz;   // material derivative: dh/dt + u . grad h
    CHECK(fabsf(s.vel[1] - vy_expect) < 0.08f, "orbital vy %.3f vs dh/dt + u.grad h %.3f", s.vel[1], vy_expect);

    // Determinism: same model twice, same bits.
    u32 a = wave_hash(), b = wave_hash();
    CHECK(a == b, "wave hash repeatable in process (%08x %08x)", a, b);
    // Golden fingerprint: the same on every compiler / OS (MSVC and gcc / clang builds must agree, or
    // a Windows host and a Linux client disagree on the sea). A deliberate model change updates it.
    CHECK(a == 0xceb2f879u, "wave hash matches the golden fingerprint (%08x): FP contraction or libm in water_waves.c?", a);
    printf("  wave hash %08x\n", a);
}

// ------------------------------------------------------------------ bodies / queries

static float ground_slope(void *user, float x, float z) {
    (void)user; (void)z;
    return x * 0.05f - 4.0f;   // beach rising to +x: dry above x = 80
}

static void test_queries(void) {
    static WaterWorld w;
    water_world_init(&w, 7u);
    // Lake polygon at level 2 and a pond ellipse at level 3 overlapping its corner.
    WaterBody lake;
    memset(&lake, 0, sizeof(lake));
    lake.kind = WATER_LAKE;
    lake.shape = WSHAPE_POLYGON;
    lake.p = water_params_default(WATER_LAKE);
    lake.level = 2.0f;
    const float poly[5][2] = { { 0, 0 }, { 40, 0 }, { 40, 30 }, { 20, 45 }, { 0, 30 } };
    memcpy(lake.poly, poly, sizeof(poly));
    lake.poly_n = 5;
    int il = water_add_body(&w, &lake);
    WaterBody pond = lake;
    pond.shape = WSHAPE_ELLIPSE;
    pond.center[0] = 40; pond.center[1] = 0;
    pond.radius[0] = 6; pond.radius[1] = 3;
    pond.yaw = 90.0f;
    pond.level = 3.0f;
    int ip = water_add_body(&w, &pond);
    float lv, fl[2];
    CHECK(water_body_at(&w, 10, 10, &lv, fl) == il && lv == 2.0f, "inside the lake");
    CHECK(water_body_at(&w, 30, 42, &lv, fl) == -1, "outside the lake's notch");
    CHECK(water_body_at(&w, 39, 1, &lv, fl) == ip && lv == 3.0f, "overlap: the higher surface wins");
    CHECK(water_body_at(&w, 41.0f, 5.0f, &lv, fl) == ip, "rotated ellipse: long axis along z");
    CHECK(water_body_at(&w, 45.0f, 0.5f, &lv, fl) == -1, "rotated ellipse: short axis along x");
    CHECK(water_height_at(&w, 10, 10, 0) == 2.0f, "lake height is flat");
    CHECK(water_depth_at(&w, 10, 10, 0) >= 1e8f, "no ground function: depth unbounded");

    // Ocean with a beach: waves fade over the last metres, dry land has no water.
    water_world_clear(&w);
    WaterBody sea;
    memset(&sea, 0, sizeof(sea));
    sea.kind = WATER_OCEAN;
    sea.shape = WSHAPE_INFINITE;
    sea.p = water_params_default(WATER_OCEAN);
    water_add_body(&w, &sea);
    water_set_ground(&w, ground_slope, NULL);
    water_set_wind(&w, 12.0f, 90.0f);
    float amp_deep = 0.0f, amp_shore = 0.0f;
    for (int i = 0; i < 200; i++) {
        float t = (float)i * 0.31f;
        amp_deep = fmaxf(amp_deep, fabsf(water_height_at(&w, -100.0f, 0, t)));
        float hs = water_height_at(&w, 79.0f, 0, t);
        if (hs != WATER_NONE) amp_shore = fmaxf(amp_shore, fabsf(hs));
    }
    CHECK(amp_deep > 0.3f, "deep water has waves (%.2f m)", amp_deep);
    CHECK(amp_shore < 0.1f * amp_deep, "waves fade at the shore (%.3f vs %.2f)", amp_shore, amp_deep);
    CHECK(water_height_at(&w, 100.0f, 0, 0) == WATER_NONE && water_depth_at(&w, 100.0f, 0, 0) == 0.0f, "dry beach: no water");
    float d = water_depth_at(&w, -40.0f, 0, 0);
    CHECK(d > 4.0f && d < 8.5f, "depth = surface - ground (%.2f)", d);
    WaterSample s;
    CHECK(water_sample(&w, -40.0f, 0, 3.0f, &s) && s.kind == WATER_OCEAN && s.normal[1] > 0.5f, "sample: ocean, upward normal");

    // JSON: presets and bodies.
    WaterParams pp = water_params_default(WATER_LAKE);
    char err[128];
    CHECK(water_params_parse(&pp, "{\"preset\":\"murky\",\"absorb\":[1,0.5,0.4],\"turbidity\":0.8}", err, sizeof(err)), "preset parses");
    CHECK(pp.absorb[0] == 1.0f && pp.turbidity == 0.8f && pp.roughness == water_params_default(WATER_LAKE).roughness, "preset overrides only its keys");
    cJSON *o = cJSON_Parse("{\"type\":\"lake\",\"name\":\"pool\",\"level\":1.5,\"box\":{\"center\":[10,20],\"half\":[4,3]},\"roughness\":0.02}");
    WaterBody jb;
    CHECK(water_body_from_json(&jb, o, &pp, err, sizeof(err)) && jb.poly_n == 4 && jb.level == 1.5f && jb.p.roughness == 0.02f &&
          jb.p.turbidity == 0.8f, "body JSON: box, level, inline override over the preset");
    CHECK(jb.bmin[0] == 6.0f && jb.bmax[1] == 23.0f, "body bounds");
    cJSON_Delete(o);
    o = cJSON_Parse("{\"type\":\"lake\",\"polygon\":[[0,0],[1,0]]}");
    CHECK(!water_body_from_json(&jb, o, NULL, err, sizeof(err)), "a 2-point polygon is rejected (%s)", err);
    cJSON_Delete(o);
}

// ------------------------------------------------------------------ buoyancy

typedef struct FlatWater { float level; float vel[3]; float density; } FlatWater;
static bool flat_sample(void *u, float x, float z, float e, BuoyWaterPlane *o) {
    (void)x; (void)z; (void)e;
    const FlatWater *f = (const FlatWater *)u;
    o->height = f->level;
    o->gx = o->gz = 0.0f;
    memcpy(o->vel, f->vel, sizeof(o->vel));
    o->density = f->density;
    return true;
}
typedef struct WaveWater { const WaterWorld *w; float t; } WaveWater;
static bool wave_sample(void *u, float x, float z, float e, BuoyWaterPlane *o) {
    const WaveWater *ww = (const WaveWater *)u;
    WaterPatch p;
    if (!water_patch(ww->w, x, z, ww->t, e, &p)) return false;
    o->density = p.density;
    return water_patch_plane(&p, x, z, &o->height, &o->gx, &o->gz, o->vel);
}

static void test_buoy_pure(void) {
    ShapeDesc box = { 0 };
    box.kind = SHAPE_BOX;
    box.half = v3(0.5f, 0.5f, 0.5f);
    BuoyProbe pr[BUOY_MAX_PROBES];
    int n = buoy_probes_from_shape(&box, pr, BUOY_MAX_PROBES);
    float vol = 0;
    for (int i = 0; i < n; i++) vol += pr[i].volume;
    CHECK(n >= 4 && fabsf(vol - 1.0f) < 1e-4f, "box probes cover its volume (%d probes, %.4f m3)", n, vol);
    ShapeDesc sph = { 0 };
    sph.kind = SHAPE_SPHERE;
    sph.radius = 0.4f;
    n = buoy_probes_from_shape(&sph, pr, BUOY_MAX_PROBES);
    vol = 0;
    for (int i = 0; i < n; i++) vol += pr[i].volume;
    CHECK(fabsf(vol - 4.18879f * 0.064f) < 1e-3f, "sphere probes: analytic volume (%.4f)", vol);
    ShapeDesc cap = { 0 };
    cap.kind = SHAPE_CAPSULE;
    cap.radius = 0.3f;
    cap.half_height = 0.6f;
    CHECK(buoy_probes_from_shape(&cap, pr, BUOY_MAX_PROBES) > 2, "capsule probes");

    // Density 500 box at its equilibrium (centre at the surface): buoyancy = weight, no torque.
    n = buoy_probes_from_shape(&box, pr, BUOY_MAX_PROBES);
    BuoyDesc d = buoy_desc_default();
    d.density = 500.0f;
    Buoyancy b;
    buoy_setup(&b, 1, pr, n, 500.0f, &d);
    FlatWater fw = { 0.0f, { 0, 0, 0 }, 1000.0f };
    BuoyWater water = { flat_sample, &fw, 9.81f };
    BuoyBodyState st = { v3(0, 0, 0), quat_identity(), v3(0, 0, 0), v3(0, 0, 0), v3(0, 0, 0) };
    BuoyResult r;
    buoy_compute(&b, &st, &water, 1.0f / 60.0f, &r);
    float weight = 500.0f * 9.81f / 60.0f;
    CHECK(fabsf(r.impulse.y - weight) < 1e-3f * weight && fabsf(r.submerged - 0.5f) < 1e-4f, "equilibrium: lift %.3f = weight %.3f, submerged %.3f",
          r.impulse.y, weight, r.submerged);
    CHECK(v3_len(r.ang_impulse) < 1e-4f, "upright: no torque");
    // A flat plank (stable: its metacentre is far above its centre of mass, unlike a density 0.5
    // cube, which is genuinely unstable upright) tilted +z gets a righting moment -z.
    ShapeDesc plank = { 0 };
    plank.kind = SHAPE_BOX;
    plank.half = v3(1.0f, 0.1f, 0.5f);
    BuoyProbe pp[BUOY_MAX_PROBES];
    int pn = buoy_probes_from_shape(&plank, pp, BUOY_MAX_PROBES);
    Buoyancy bp;
    buoy_setup(&bp, 2, pp, pn, 0.2f * 1000.0f * 0.5f, &d);
    st.rot = quat_axis_angle(v3(0, 0, 1), 0.2f);
    buoy_compute(&bp, &st, &water, 1.0f / 60.0f, &r);
    CHECK(r.ang_impulse.z < 0.0f, "tilted plank +z: righting torque -z (%.3f)", r.ang_impulse.z);
    st.rot = quat_axis_angle(v3(0, 0, 1), -0.2f);
    buoy_compute(&bp, &st, &water, 1.0f / 60.0f, &r);
    CHECK(r.ang_impulse.z > 0.0f, "tilted plank -z: righting torque +z (%.3f)", r.ang_impulse.z);
    // Drag: moving with the current costs nothing, against it decelerates.
    st.rot = quat_identity();
    st.v = v3(2, 0, 0);
    fw.vel[0] = 2.0f;
    buoy_compute(&b, &st, &water, 1.0f / 60.0f, &r);
    CHECK(fabsf(r.impulse.x) < 1e-4f, "drifting with the flow: no drag");
    fw.vel[0] = 0.0f;
    buoy_compute(&b, &st, &water, 1.0f / 60.0f, &r);
    CHECK(r.impulse.x < 0.0f && -r.impulse.x < 500.0f * 2.0f * 0.5f, "drag opposes the motion and never reverses it");
    // Out of the water: nothing.
    st.v = v3(0, 0, 0);
    st.pos = st.com = v3(0, 3, 0);
    buoy_compute(&b, &st, &water, 1.0f / 60.0f, &r);
    CHECK(!r.in_water && r.impulse.y == 0.0f, "above the surface: no force");
}

static BodyId add_box(PhysicsWorld *pw, vec3 pos, vec3 half, float mass) {
    BodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.shape.kind = SHAPE_BOX;
    bd.shape.half = half;
    bd.pos = pos;
    bd.rot = quat_identity();
    bd.motion = PM_DYNAMIC;
    bd.layer = PL_MOVING;
    bd.mass = mass;
    bd.linear_damping = 1e-4f;      // no air drag: the water drag alone is measured
    bd.angular_damping = 1e-4f;
    return phys_add_body(pw, &bd);
}

static void sim(PhysicsWorld *pw, BuoySystem *bs, BuoyWater *water, int ticks) {
    for (int i = 0; i < ticks; i++) {
        buoy_step(bs, pw, water, SIM_DT);
        phys_step(pw, SIM_DT);
    }
}

static void test_buoy_jolt(void) {
    PhysicsWorld *pw = phys_world_create();
    static BuoySystem bs;
    buoy_system_init(&bs);
    FlatWater fw = { 0.0f, { 0, 0, 0 }, 1000.0f };
    BuoyWater water = { flat_sample, &fw, 9.81f };
    BuoyDesc d = buoy_desc_default();

    // 1 m cube of density 500 (500 kg) dropped from 2 m: settles half submerged.
    d.density = 500.0f;
    BodyId half = add_box(pw, v3(0, 2, 0), v3(0.5f, 0.5f, 0.5f), 500.0f);
    // Density 250 floats a quarter in, density 2000 sinks.
    d.density = 250.0f;
    BodyId light = add_box(pw, v3(5, 1, 0), v3(0.5f, 0.5f, 0.5f), 250.0f);
    d.density = 2000.0f;
    BodyId heavy = add_box(pw, v3(-5, 0, 0), v3(0.5f, 0.5f, 0.5f), 2000.0f);
    d.density = 500.0f;
    buoy_add(&bs, pw, half, &d);
    d.density = 250.0f;
    buoy_add(&bs, pw, light, &d);
    d.density = 2000.0f;
    buoy_add(&bs, pw, heavy, &d);
    CHECK(bs.count == 3 && fabsf(buoy_find(&bs, half)->mass - 500.0f) < 0.5f, "buoyant bodies registered with their mass");
    sim(pw, &bs, &water, 60 * 20);
    vec3 p;
    phys_get_transform(pw, half, &p, NULL);
    CHECK(fabsf(p.y) < 0.02f, "density 0.5 box settles half submerged: centre %.4f m (+-2 %% of 1 m)", p.y);
    phys_get_transform(pw, light, &p, NULL);
    CHECK(fabsf(p.y - 0.25f) < 0.02f, "density 0.25 box: centre %.4f (expected +0.25)", p.y);
    phys_get_transform(pw, heavy, &p, NULL);
    CHECK(p.y < -20.0f, "density 2 box sinks (%.1f m after 20 s)", p.y);
    vec3 vh = phys_get_velocity(pw, heavy);
    // Terminal velocity: (rho_b - rho_w) V g = drag; with the quadratic term it is a few m/s.
    CHECK(vh.y < -0.5f && vh.y > -6.0f, "sinking body reaches a terminal velocity (%.2f m/s)", vh.y);

    // Drag: push the floating box at 3 m/s; expected decay ~ exp(-drag * submerged * t) plus quadratic.
    phys_set_velocity(pw, half, v3(3, 0, 0), v3(0, 0, 0));
    sim(pw, &bs, &water, 60 * 2);
    float v2 = v3_len(phys_get_velocity(pw, half));
    sim(pw, &bs, &water, 60 * 8);
    float v10 = v3_len(phys_get_velocity(pw, half));
    float tau = 1.0f / (d.drag * 0.5f);
    CHECK(v2 < 3.0f * expf(-2.0f / tau) * 1.05f && v2 > 0.2f, "drag after 2 s: %.3f m/s (linear-only bound %.3f)", v2, 3.0f * expf(-2.0f / tau));
    CHECK(v10 < 0.05f, "at rest within 10 s (%.4f m/s)", v10);

    // Current: a floating plank (stable, unlike the density 0.5 cube, which rolls) drifts with the flow.
    d.density = 500.0f;
    BodyId plank = add_box(pw, v3(0, 0.5f, 10), v3(1.0f, 0.1f, 0.5f), 0.2f * 500.0f);
    buoy_add(&bs, pw, plank, &d);
    sim(pw, &bs, &water, 60 * 3);
    fw.vel[2] = 1.0f;
    sim(pw, &bs, &water, 60 * 10);
    vec3 vf = phys_get_velocity(pw, plank);
    CHECK(fabsf(vf.z - 1.0f) < 0.05f, "drifts with a 1 m/s current (%.3f)", vf.z);
    fw.vel[2] = 0.0f;
    phys_world_destroy(pw);
}

static void test_buoy_waves_and_budget(bool check_budget) {
    PhysicsWorld *pw = phys_world_create();
    static BuoySystem bs;
    buoy_system_init(&bs);
    static WaterWorld w;
    water_world_init(&w, 42u);
    WaterBody sea;
    memset(&sea, 0, sizeof(sea));
    sea.kind = WATER_OCEAN;
    sea.shape = WSHAPE_INFINITE;
    sea.p = water_params_default(WATER_OCEAN);
    water_add_body(&w, &sea);
    water_set_wind(&w, 10.0f, 45.0f);
    WaveWater ww = { &w, 0.0f };
    BuoyWater water = { wave_sample, &ww, 9.81f };
    // 200 bodies: crates, barrels, planks, of varying density.
    BuoyDesc d = buoy_desc_default();
    BodyId ids[200];
    for (int i = 0; i < 200; i++) {
        float x = (float)(i % 20) * 4.0f - 40.0f, z = (float)(i / 20) * 4.0f - 20.0f;
        vec3 half = (i % 3 == 0) ? v3(0.5f, 0.5f, 0.5f) : (i % 3 == 1) ? v3(1.2f, 0.1f, 0.25f) : v3(0.3f, 0.45f, 0.3f);
        d.density = 300.0f + (float)(i % 5) * 120.0f;   // all lighter than sea water
        float mass = d.density * 8.0f * half.x * half.y * half.z;
        ids[i] = add_box(pw, v3(x, 1.0f, z), half, mass);
        buoy_add(&bs, pw, ids[i], &d);
    }
    CHECK(bs.count == 200, "200 buoyant bodies");
    double total = 0.0, worst = 0.0, tr = 0.0, tc = 0.0, ta = 0.0;
    int above = 0, n = 0;
    for (int tick = 0; tick < 60 * 20; tick++) {
        ww.t = waves_time((u32)tick, 0.0f, SIM_HZ);
        buoy_step(&bs, pw, &water, SIM_DT);
        phys_step(pw, SIM_DT);
        if (tick >= 60 * 5) {
            total += bs.last_ms;
            tr += bs.read_ms; tc += bs.compute_ms; ta += bs.apply_ms;
            worst = fmax(worst, bs.last_ms);
            for (int i = 0; i < 200; i += 10) {
                vec3 p;
                phys_get_transform(pw, ids[i], &p, NULL);
                float h = water_height_at(&w, p.x, p.z, ww.t);
                above += p.y > h - 1.0f && p.y < h + 1.0f;
                n++;
            }
        }
    }
    double avg = total / (60.0 * 15.0);
    printf("  buoyancy: 200 bodies in waves, %d jobs workers: %.3f ms / tick avg (read %.3f, compute %.3f, apply %.3f), %.3f worst\n",
           g_engine.jobs ? jobs_worker_count() : 0, avg, tr / 900.0, tc / 900.0, ta / 900.0, worst);
    if (check_budget) CHECK(avg <= 0.5, "200 buoyant bodies within 0.5 ms / tick (%.3f)", avg);
    CHECK(above == n, "every floating body rides the waves (%d / %d within 1 m of the surface)", above, n);
    phys_world_destroy(pw);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--hash") == 0) {
        printf("%08x\n", wave_hash());
        return 0;
    }
    log_init(NULL);
    test_math();
    test_waves();
    test_queries();
    test_buoy_pure();
    phys_global_init();
    test_buoy_jolt();
    test_buoy_waves_and_budget(false);       // single thread: reported only
    jobs_init(0);
    test_buoy_waves_and_budget(true);
    jobs_shutdown();
    phys_global_shutdown();
    printf("test_water: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
