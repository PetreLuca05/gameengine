// Night optics tests (docs/NIGHT_OPTICS.md):
//   data        assets/thermal/{materials,entities,palettes}.json and assets/nvg/nvg.json parse; every
//               class / variant present; surface names map to the documented classes
//   radiance    the Planck LUT matches the numeric 8..14 um integral; inverse round-trips; monotonic
//   surfaces    inertia: concrete stays warmer than metal after sunset, water lags the most; settle
//               lands on the periodic solution; snow never above 0 C; up-facing loses more at night
//   thermal     zombie vs human readouts match the config; glass shows its own temperature (no
//               transmission: the radiance is its own + reflection); bare metal reads the cold sky;
//               vehicle parts heat with use and cool per rates; barrel heat accumulates with fire rate;
//               corpses cool; AGC widens on a hot source and recovers; NUC cadence and freeze;
//               palettes map their ends correctly; digital zoom crops
//   NVG         ABC overshoots then settles; whiteout after a muzzle flash recovers within the
//               variant's time; scintillation variance monotonic in the flux; halo radius constant
//               in angle across resolutions; circle / figure-8 masks per variant; tube protection
//               shuts off in daylight; battery drain (+ illuminator)
//   rules       key 8 toggles only with an nvg holding charge; battery swap; ADS refused while down
#include "world/thermal.h"
#include "render/thermal_sensor.h"
#include "render/post/nvg_model.h"
#include "gameplay/nvg_rules.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

Engine g_engine;   // the core library logs through it
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

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)n + 1);
    size_t got = fread(t, 1, (size_t)n, f);
    t[got] = 0;
    fclose(f);
    return t;
}

static ThermalData g_td;
static NvgConfig g_nc;
static ThermalPalettes g_pal;

static void test_data(void) {
    thermal_defaults(&g_td);
    char *m = read_text("assets/thermal/materials.json"), *e = read_text("assets/thermal/entities.json");
    char *p = read_text("assets/thermal/palettes.json"), *n = read_text("assets/nvg/nvg.json");
    CHECK(m && e && p && n, "data files readable (run from the bin dir)");
    CHECK(thermal_load_materials(&g_td, m), "materials.json parses");
    CHECK(thermal_load_entities(&g_td, e), "entities.json parses");
    thermal_palettes_default(&g_pal);
    CHECK(thermal_palettes_load(&g_pal, p), "palettes.json parses");
    CHECK(g_pal.count == 5, "five palettes (%d)", g_pal.count);
    nvg_config_default(&g_nc);
    CHECK(nvg_config_load(&g_nc, n), "nvg.json parses");
    CHECK(g_nc.count == 3, "three NVG variants (%d)", g_nc.count);
    free(m); free(e); free(p); free(n);

    for (int c = 0; c < THC_COUNT; c++) {
        CHECK(thermal_class_by_name(thermal_class_name(c)) == c, "class %d name round trip", c);
        CHECK(g_td.cls[c].emissivity > 0.0f && g_td.cls[c].emissivity <= 1.0f, "class %s emissivity", thermal_class_name(c));
    }
    CHECK(thermal_class_of(&g_td, "glass") == THC_GLASS, "glass -> glass");
    CHECK(thermal_class_of(&g_td, "metal_thin") == THC_METAL_PAINTED, "metal_thin -> painted metal");
    CHECK(thermal_class_of(&g_td, "steel") == THC_METAL_BARE, "steel -> bare metal");
    CHECK(thermal_class_of(&g_td, "leaves") == THC_FOLIAGE, "leaves -> foliage");
    CHECK(thermal_class_of(&g_td, "unobtainium") == THC_CONCRETE, "unknown -> concrete");
    CHECK(g_td.cls[THC_METAL_BARE].emissivity < 0.3f, "bare metal is a LWIR mirror");
    CHECK(g_td.cls[THC_FOLIAGE].ir_reflectance > g_td.cls[THC_ASPHALT].ir_reflectance + 0.3f, "foliage bright in near-IR");
}

static void test_radiance(void) {
    float worst = 0.0f;
    for (float t = -40.0f; t <= 1200.0f; t += 7.3f) {
        double ex = thermal_band_radiance_exact(t);
        float lut = thermal_radiance(t);
        float err = (float)fabs(lut - ex) / (float)ex;
        if (err > worst) worst = err;
        float back = thermal_temp_of_radiance(lut);
        CHECK(fabsf(back - t) < 0.05f, "inverse at %.1f C -> %.3f", t, back);
    }
    CHECK(worst < 0.01f, "LUT vs Planck integral within 1 %% (%.4f)", worst);
    float prev = 0.0f;
    for (int i = 0; i < THERMAL_LUT_N; i++) {
        CHECK(thermal_lut()[i] > prev, "radiance monotonic at %d", i);
        prev = thermal_lut()[i];
    }
    // 8..14 um band radiance of a 300 K black body ~ 50 W/m2/sr (~160 W/m2 exitance / pi; sanity)
    float l300 = thermal_radiance(26.85f);
    CHECK(l300 > 45.0f && l300 < 60.0f, "band radiance at 300 K = %.2f", l300);
}

// A simple day: sun along a half sine 6..18 h, 70 deg at noon, constant air.
typedef struct DayEnv { float air, cloud; } DayEnv;
static void day_env(double hours, ThermalEnv *out, void *user) {
    const DayEnv *d = user;
    double h = fmod(hours, 24.0);
    if (h < 0) h += 24.0;
    float elev = (float)(sin((h - 6.0) / 12.0 * 3.14159265) * 70.0);
    *out = (ThermalEnv){ .air = d->air, .sun = thermal_sun_input(elev), .cloud = d->cloud, .wetness = 0, .wind = 0 };
}

static void test_surfaces(void) {
    DayEnv de = { .air = 15.0f, .cloud = 0.0f };
    ThermalSurfaces s;
    thermal_surfaces_settle(&g_td, &s, 21.0, day_env, &de);   // 9 pm, clear
    ThermalEnv env;
    day_env(21.0, &env, &de);
    CHECK(s.t[THC_CONCRETE] > s.t[THC_METAL_PAINTED] + 1.0f, "concrete warmer than metal at night (%.2f vs %.2f)",
          s.t[THC_CONCRETE], s.t[THC_METAL_PAINTED]);
    CHECK(s.t[THC_CONCRETE] > env.air, "concrete above air after a sunny day (%.2f)", s.t[THC_CONCRETE]);
    CHECK(s.t[THC_FOLIAGE] < env.air, "foliage below air at night (%.2f)", s.t[THC_FOLIAGE]);
    CHECK(fabsf(s.t[THC_WATER] - env.air) < 2.0f, "water near the air");
    ThermalSurfaces noon;
    thermal_surfaces_settle(&g_td, &noon, 13.0, day_env, &de);
    float swing_water = fabsf(noon.t[THC_WATER] - s.t[THC_WATER]);
    float swing_metal = fabsf(noon.t[THC_METAL_PAINTED] - s.t[THC_METAL_PAINTED]);
    CHECK(swing_water < swing_metal * 0.25f, "water lags (swing %.2f vs metal %.2f)", swing_water, swing_metal);
    CHECK(noon.t[THC_ASPHALT] > noon.t[THC_GRASS] + 3.0f, "asphalt hot at noon (%.2f vs grass %.2f)", noon.t[THC_ASPHALT], noon.t[THC_GRASS]);
    // settle is the periodic solution: settling a day later gives the same state
    ThermalSurfaces s2;
    thermal_surfaces_settle(&g_td, &s2, 45.0, day_env, &de);
    for (int c = 0; c < THC_COUNT; c++)
        CHECK(fabsf(s2.t[c] - s.t[c]) < 0.3f, "periodic class %s (%.2f vs %.2f)", thermal_class_name(c), s2.t[c], s.t[c]);
    // up-facing surfaces lose more at night; a sunlit face is warmer at noon
    float up = thermal_surface_temp(&g_td, &s, &env, THC_CONCRETE, 0.0f, 1.0f);
    float wall = thermal_surface_temp(&g_td, &s, &env, THC_CONCRETE, 0.0f, 0.0f);
    CHECK(up < wall, "roof colder than wall at night (%.2f vs %.2f)", up, wall);
    ThermalEnv nenv;
    day_env(13.0, &nenv, &de);
    float lit = thermal_surface_temp(&g_td, &noon, &nenv, THC_CONCRETE, 1.0f, 0.0f);
    float shade = thermal_surface_temp(&g_td, &noon, &nenv, THC_CONCRETE, 0.0f, 0.0f);
    CHECK(lit > shade + 2.0f, "sunlit face warmer (%.2f vs %.2f)", lit, shade);
    // snow
    ThermalEnv warm = { .air = 8.0f, .sun = 1.0f };
    CHECK(thermal_class_eq(&g_td, THC_SNOW, &warm) <= 0.0f, "snow never above 0 C");
    // rain cools
    ThermalEnv wet = env;
    wet.wetness = 1.0f;
    CHECK(thermal_class_eq(&g_td, THC_CONCRETE, &wet) < thermal_class_eq(&g_td, THC_CONCRETE, &env) - 2.0f, "wet surfaces cooler");
}

static void test_entities(void) {
    float air = 12.0f;
    float human = thermal_role_temp(&g_td, &g_td.human, THR_FACE, air, 0.0f);
    float zombie = thermal_role_temp(&g_td, &g_td.zombie, THR_FACE, air, 0.0f);
    CHECK(human > 32.0f && human < 37.0f, "human face %.2f C", human);
    CHECK(zombie > 18.0f && zombie < 28.0f, "zombie face %.2f C", zombie);
    CHECK(human - zombie > 6.0f, "zombies read colder (%.2f vs %.2f)", human, zombie);
    // the readout of a human face through the sensor (spot meter, e 0.95) is within 1 C of its temperature
    float lh = thermal_surface_radiance(g_td.cls[THC_SKIN].emissivity, human, air);
    float spot = thermal_spot_temp(lh, 0.95f, air);
    CHECK(fabsf(spot - human) < 1.0f, "spot readout of a face %.2f vs %.2f", spot, human);
    float torso = thermal_role_temp(&g_td, &g_td.human, THR_TORSO, air, 0.0f);
    float pack = thermal_role_temp(&g_td, &g_td.human, THR_BACKPACK, air, 0.0f);
    CHECK(torso < human && pack < torso && pack > air, "clothing insulates (torso %.2f, pack %.2f)", torso, pack);
    CHECK(thermal_role_temp(&g_td, &g_td.human, THR_HANDS, air, 0.0f) < human, "hands cooler than the face");
    CHECK(thermal_skin(&g_td.human, -10.0f, 8.0f) < thermal_skin(&g_td.human, 20.0f, 0.0f) - 3.0f, "cold wind lowers skin");
    // corpse cooling
    float c0 = thermal_cooling(37.0f, air, 0.0f, g_td.corpse_tau);
    float c1 = thermal_cooling(37.0f, air, g_td.corpse_tau, g_td.corpse_tau);
    CHECK(c0 == 37.0f && fabsf(c1 - (air + (37.0f - air) / 2.7182818f)) < 0.01f, "corpse Newtonian cooling");
    // vehicle: heats with use, cools per rates
    ThermalVehicle v;
    thermal_vehicle_init(&v, air, air);
    ThermalVehInput drive = { .engine = true, .load = 0.6f, .braking = false, .speed = 15.0f };
    for (int i = 0; i < 600 * 60; i++) thermal_vehicle_step(&g_td, &v, &drive, air, 1.0f / 60.0f);   // 10 min
    float eng_target = air + g_td.veh[THV_ENGINE].rise_idle + g_td.veh[THV_ENGINE].rise_load * 0.6f;
    CHECK(fabsf(v.part[THV_ENGINE] - eng_target) < 1.0f, "engine reaches %.1f (%.1f)", eng_target, v.part[THV_ENGINE]);
    CHECK(v.part[THV_EXHAUST] > v.part[THV_ENGINE], "exhaust hotter than the engine block");
    CHECK(v.part[THV_TIRE] > air + 3.0f, "tyres warm from driving (%.1f)", v.part[THV_TIRE]);
    ThermalVehInput brake = { .engine = true, .load = 0.2f, .braking = true, .speed = 20.0f };
    float b0 = v.part[THV_BRAKE];
    for (int i = 0; i < 3 * 60; i++) thermal_vehicle_step(&g_td, &v, &brake, air, 1.0f / 60.0f);
    CHECK(v.part[THV_BRAKE] > b0 + 20.0f, "brakes heat when braking (%.1f -> %.1f)", b0, v.part[THV_BRAKE]);
    float e_hot = v.part[THV_ENGINE];
    ThermalVehInput off = { 0 };
    float tau = g_td.veh[THV_ENGINE].cool_tau;
    for (int i = 0; i < (int)(tau * 10); i++) thermal_vehicle_step(&g_td, &v, &off, air, 0.1f);
    float expect = air + (e_hot - air) / 2.7182818f;
    CHECK(fabsf(v.part[THV_ENGINE] - expect) < 1.0f, "engine cools with tau %.0f s (%.1f vs %.1f)", tau, v.part[THV_ENGINE], expect);
    // barrel heat accumulates with fire rate (heat g with tau 8 s, as bal_muzzle) and is capped
    float slow = 0.0f, fast = 0.0f;
    for (int i = 0; i < 60 * 20; i++) {   // 20 s: 1 shot/s vs 10 shots/s of 1.7 g
        float dt = 1.0f / 60.0f;
        slow *= expf(-dt / 8.0f); fast *= expf(-dt / 8.0f);
        if (i % 60 == 0) slow += 1.7f;
        if (i % 6 == 0) fast += 1.7f;
    }
    float ts = thermal_barrel_temp(&g_td, slow, air), tf = thermal_barrel_temp(&g_td, fast, air);
    CHECK(tf > ts + 50.0f, "fast fire heats the barrel more (%.0f vs %.0f)", tf, ts);
    CHECK(thermal_barrel_temp(&g_td, 1e6f, air) <= g_td.barrel_max, "barrel cap");
}

static void test_thermal_optics(void) {
    float air = 10.0f;
    // glass: opaque in LWIR: its radiance is its own emission + reflection, whatever is behind it
    float glass_t = air + 1.0f;
    float e = g_td.cls[THC_GLASS].emissivity;
    float l_glass = thermal_surface_radiance(e, glass_t, air);
    float read = thermal_spot_temp(l_glass, e, air);
    CHECK(fabsf(read - glass_t) < 0.2f, "glass reads its own temperature (%.2f vs %.2f)", read, glass_t);
    float human = thermal_role_temp(&g_td, &g_td.human, THR_FACE, air, 0.0f);
    CHECK(fabsf(read - human) > 15.0f, "the person behind the glass is not seen");
    // bare metal (e 0.12) facing a clear sky reads near the sky temperature, painted metal near its own
    float sky = thermal_sky_temp(&g_td, air, 0.0f, 1.0f);
    CHECK(sky < air - 30.0f, "clear zenith sky cold (%.1f)", sky);
    CHECK(thermal_sky_temp(&g_td, air, 1.0f, 1.0f) > sky + 25.0f, "clouds warm the sky");
    CHECK(fabsf(thermal_sky_temp(&g_td, air, 0.0f, 0.0f) - air) < 0.01f, "horizon at the air temperature");
    float l_bare = thermal_surface_radiance(g_td.cls[THC_METAL_BARE].emissivity, air, sky);
    float l_paint = thermal_surface_radiance(g_td.cls[THC_METAL_PAINTED].emissivity, air, sky);
    float r_bare = thermal_spot_temp(l_bare, 0.95f, air), r_paint = thermal_spot_temp(l_paint, 0.95f, air);
    CHECK(r_bare < sky + 0.3f * (air - sky), "bare metal reflects the cold sky (%.1f, sky %.1f)", r_bare, sky);
    CHECK(fabsf(r_paint - air) < 6.0f, "painted metal reads near its temperature (%.1f)", r_paint);
    // AGC: a hot source widens the range fast, the scene compresses, then it recovers slowly
    ThermalAgc a = { 0 };
    float lo = thermal_radiance(5.0f), hi = thermal_radiance(36.0f), flash = thermal_radiance(900.0f);
    for (int i = 0; i < 120; i++) thermal_agc_step(&a, lo, hi, 1.0f / 60.0f);
    float person = thermal_agc_map(&a, thermal_radiance(34.0f));
    CHECK(person > 0.85f, "person near the top before the flash (%.2f)", person);
    for (int i = 0; i < 6; i++) thermal_agc_step(&a, lo, flash, 1.0f / 60.0f);
    float during = thermal_agc_map(&a, thermal_radiance(34.0f));
    CHECK(during < 0.2f, "a muzzle flash compresses the scene (%.2f)", during);
    for (int i = 0; i < 60 * 8; i++) thermal_agc_step(&a, lo, hi, 1.0f / 60.0f);
    CHECK(thermal_agc_map(&a, thermal_radiance(34.0f)) > 0.8f, "recovers after the flash");
    ThermalAgc flat = { 0 };
    for (int i = 0; i < 60; i++) thermal_agc_step(&flat, thermal_radiance(10.0f), thermal_radiance(10.1f), 1.0f / 60.0f);
    CHECK(thermal_temp_of_radiance(flat.hi) - thermal_temp_of_radiance(flat.lo) >= AGC_MIN_SPAN_C * 0.95f, "minimum span");
    // NUC cadence: first after 30 s, then growing to 180 s; each freezes for 0.3 s
    ThermalNuc n;
    thermal_nuc_reset(&n, air);
    float t = 0.0f, last = 0.0f, gaps[8];
    int k = 0, frozen_frames = 0;
    for (int i = 0; i < 60 * 900 && k < 8; i++) {
        t += 1.0f / 60.0f;
        if (thermal_nuc_step(&n, 1.0f / 60.0f, air)) { gaps[k++] = t - last; last = t; }
        if (k == 1 && n.freeze > 0.0f) frozen_frames++;
    }
    CHECK(k >= 6, "NUC events (%d)", k);
    CHECK(fabsf(gaps[0] - NUC_FIRST) < 0.1f, "first NUC at 30 s (%.2f)", gaps[0]);
    for (int i = 1; i < k; i++) CHECK(gaps[i] >= gaps[i - 1] - 0.05f && gaps[i] <= NUC_MAX + 0.1f, "NUC interval grows (%.1f)", gaps[i]);
    CHECK(abs(frozen_frames - (int)(NUC_FREEZE * 60)) <= 1, "freeze 0.3 s (%d frames)", frozen_frames);
    ThermalNuc n2;
    thermal_nuc_reset(&n2, air);
    n2.freeze = 0.0f;
    CHECK(thermal_nuc_step(&n2, 0.016f, air + 6.0f), "an ambient jump forces a NUC");
    // palettes: White Hot black -> white, Black Hot inverted, Red Hot red at the top
    const ThermalPalette *wh = &g_pal.pal[thermal_palette_find(&g_pal, "White Hot")];
    const ThermalPalette *bh = &g_pal.pal[thermal_palette_find(&g_pal, "Black Hot")];
    const ThermalPalette *rh = &g_pal.pal[thermal_palette_find(&g_pal, "Red Hot")];
    const ThermalPalette *ib = &g_pal.pal[thermal_palette_find(&g_pal, "Ironbow")];
    CHECK(wh->rgb[0][0] == 0 && wh->rgb[255][0] == 255 && wh->rgb[128][1] > 120 && wh->rgb[128][1] < 136, "White Hot ramp");
    CHECK(bh->rgb[0][0] == 255 && bh->rgb[255][0] == 0, "Black Hot inverted");
    CHECK(rh->rgb[250][0] > 200 && rh->rgb[250][2] < 80 && rh->rgb[200][0] == rh->rgb[200][2], "Red Hot: grey then red");
    CHECK(ib->rgb[0][2] > ib->rgb[0][0] && ib->rgb[255][0] > 240, "Ironbow dark blue -> white yellow");
    // digital zoom crops (pixelation): 4x shows a quarter of the core across
    ThermalCore core = thermal_core(1);
    CHECK(thermal_zoom_crop(&core, 1) == 512 && thermal_zoom_crop(&core, 4) == 128, "zoom crop");
    CHECK(thermal_core(0).w == 384 && thermal_core(0).h == 288, "low core 384 x 288");
}

static void test_nvg(void) {
    const NvgVariant *v = nvg_variant(&g_nc, "p43");
    // ABC: a step from moonlight to starlight raises the gain, overshoots, settles
    NvgState s;
    nvg_state_reset(&s, v, 5e-3f);
    float target = nvg_target_log_gain(v, 5e-4f), start = s.log_gain, peak = start;
    float settle_t = -1.0f;
    for (int i = 0; i < 60 * 4; i++) {
        nvg_step(&s, &g_nc, v, &(NvgInput){ .phi_mean = 5e-4f, .phi_current = 5e-4f, .dt = 1.0f / 60.0f });
        if (s.log_gain > peak) peak = s.log_gain;
        if (settle_t < 0.0f && i > 5 && fabsf(s.log_gain - target) < 0.05f * fabsf(target - start) && fabsf(s.vel) < 0.2f)
            settle_t = i / 60.0f;
    }
    CHECK(peak > target + 0.02f * (target - start), "ABC overshoots (%.3f > %.3f)", peak, target);
    CHECK(settle_t > 0.3f && settle_t < 2.0f, "ABC settles in %.2f s (slower than the eye, < 2 s)", settle_t);
    CHECK(fabsf(s.log_gain - target) < 0.02f, "ABC at target");
    // whiteout on a muzzle flash, recovered within whiteout_recover after it ends
    nvg_state_reset(&s, v, 1e-3f);
    for (int i = 0; i < 30; i++) nvg_step(&s, &g_nc, v, &(NvgInput){ .phi_mean = 1e-3f, .phi_current = 1e-3f, .dt = 1.0f / 60.0f });
    CHECK(s.whiteout < 0.01f, "no whiteout at night");
    for (int i = 0; i < 2; i++) nvg_step(&s, &g_nc, v, &(NvgInput){ .phi_mean = 2e-3f, .phi_current = 5.0f, .dt = 1.0f / 60.0f });
    CHECK(s.whiteout > 0.5f, "flash washes out (%.2f)", s.whiteout);
    float t = 0.0f;
    while (s.whiteout > 0.05f && t < 3.0f) {
        nvg_step(&s, &g_nc, v, &(NvgInput){ .phi_mean = 1e-3f, .phi_current = 1e-3f, .dt = 1.0f / 60.0f });
        t += 1.0f / 60.0f;
    }
    CHECK(t <= v->whiteout_recover + 0.02f && t > 0.2f, "recovers in %.2f s (limit %.2f)", t, v->whiteout_recover);
    float washed = nvg_screen(&g_nc, &(NvgState){ .whiteout = 1.0f }, 1000.0f, 1e-4f);
    float clean = nvg_screen(&g_nc, &(NvgState){ 0 }, 1000.0f, 1e-4f);
    CHECK(washed > clean + 0.5f, "whiteout raises the floor");
    // scintillation: relative noise and the measured variance grow as the flux falls
    float prev_rel = 0.0f, prev_var = 0.0f;
    float fluxes[] = { 1e-1f, 1e-2f, 1e-3f, 3e-4f, 1e-4f };
    for (int i = 0; i < 5; i++) {
        float rel = nvg_noise_rel(v, fluxes[i], 1.0f / 60.0f);
        Rng r = rng_make(1234 + i);
        double sum = 0, sum2 = 0;
        const int N = 20000;
        for (int k = 0; k < N; k++) { double x = nvg_noise_sample(v, fluxes[i], 1.0f / 60.0f, &r); sum += x; sum2 += x * x; }
        double mean = sum / N, var = sum2 / N - mean * mean;
        CHECK(fabs(mean - 1.0) < 0.1, "noise sample unbiased at %g (%.3f)", fluxes[i], mean);
        CHECK(rel > prev_rel, "relative noise grows as light falls (%g: %.4f)", fluxes[i], rel);
        CHECK(var > prev_var, "variance monotonic (%g: %.5f)", fluxes[i], var);
        prev_rel = rel; prev_var = (float)var;
    }
    // halo: constant angular size across resolutions
    float res[] = { 1280, 1920, 2560, 3840 };
    for (int i = 0; i < 4; i++) {
        float px = nvg_halo_px(v, 90.0f, res[i]);
        float ax, ay;
        nvg_pixel_angles(res[i] * 0.5f + px, res[i] * 0.28125f, res[i], res[i] * 0.5625f, 90.0f, &ax, &ay);
        CHECK(fabsf(ax - v->halo_deg) < 0.01f, "halo %.0f px at %.0f wide = %.3f deg", px, res[i], ax);
    }
    // masks: one 40 deg circle; the dual variant's figure 8 is wider than tall
    const NvgVariant *dual = nvg_variant(&g_nc, "dual");
    float W = 1920, H = 1080, F = 100.0f;
    float f = (W * 0.5f) / tanf(F * 0.5f * 0.017453293f);
    float edge = f * tanf(20.0f * 0.017453293f);
    CHECK(nvg_mask(v, W * 0.5f, H * 0.5f, W, H, F) > 0.99f, "centre inside");
    CHECK(nvg_mask(v, W * 0.5f + edge * 0.95f, H * 0.5f, W, H, F) > 0.99f, "inside near the 20 deg edge");
    CHECK(nvg_mask(v, W * 0.5f + edge * 1.06f, H * 0.5f, W, H, F) < 0.01f, "outside past 20 deg");
    CHECK(nvg_mask(v, W * 0.5f, H * 0.5f + edge * 1.06f, W, H, F) < 0.01f, "circular (vertical)");
    float ex = f * tanf(27.0f * 0.017453293f);
    CHECK(nvg_mask(dual, W * 0.5f + ex, H * 0.5f, W, H, F) > 0.99f, "dual: wider (27 deg inside)");
    CHECK(nvg_mask(dual, W * 0.5f, H * 0.5f + edge * 1.06f, W, H, F) < 0.01f, "dual: same height");
    CHECK(nvg_mask(v, W * 0.5f + ex, H * 0.5f, W, H, F) < 0.01f, "single: 27 deg outside");
    // the figure 8 pinches at the middle top: a point 19 deg up at the centre is outside both circles
    float up19 = f * tanf(19.5f * 0.017453293f);
    CHECK(nvg_mask(dual, W * 0.5f, H * 0.5f + up19, W, H, F) < 0.5f, "dual: figure 8 waist");
    // daylight: the tube protection shuts it off after protect_s
    nvg_state_reset(&s, v, 0.8f);
    float off_t = -1.0f;
    for (int i = 0; i < 60 * 6 && off_t < 0.0f; i++) {
        nvg_step(&s, &g_nc, v, &(NvgInput){ .phi_mean = 0.8f, .phi_current = 1.0f, .dt = 1.0f / 60.0f });
        if (s.shutoff) off_t = i / 60.0f;
    }
    CHECK(off_t > v->protect_s - 0.1f && off_t < v->protect_s + 0.5f, "daylight protection at %.2f s", off_t);
    CHECK(s.whiteout > 0.9f, "daylight whiteout before the shutoff");
    // battery
    float c = nvg_battery_drain(v, 1.0f, false, 3600.0f * v->runtime_h * 0.5f);
    CHECK(fabsf(c - 0.5f) < 1e-3f, "half the runtime drains half");
    float ci = nvg_battery_drain(v, 1.0f, true, 3600.0f * v->runtime_h * 0.5f);
    CHECK(ci < c, "the illuminator drains faster");
    CHECK(nvg_battery_drain(v, 0.001f, false, 100.0f) == 0.0f, "empty clamps at 0");
}

static void test_rules(void) {
    NvgItemView items[4] = {
        { .uid = 10, .is_nvg = false, .charge = 0 },
        { .uid = 11, .is_nvg = true, .charge = 0 },
        { .uid = 12, .is_nvg = true, .charge = 400 },
        { .uid = 13, .is_nvg = true, .charge = 900 },
    };
    CHECK(nvg_rules_pick(items, 1) < 0, "no nvg: nothing to toggle");
    CHECK(nvg_rules_pick(items, 2) < 0, "an nvg with an empty battery cannot turn on");
    CHECK(nvg_rules_pick(items, 4) == 2, "the first nvg with charge (grid order)");
    NvgRuleState st = { 0 };
    CHECK(!nvg_rules_toggle(&st, items, 2, 0.0), "toggle refused without charge");
    CHECK(nvg_rules_toggle(&st, items, 4, 0.0) && st.on && st.uid == 12, "toggle on");
    CHECK(nvg_rules_flipping(&st, 0.3, 0.6f), "flipping during the case");
    CHECK(!nvg_rules_ads_allowed(&st, 0.3, 0.6f), "no ADS while flipping down");
    CHECK(!nvg_rules_ads_allowed(&st, 5.0, 0.6f), "no ADS while down");
    CHECK(nvg_rules_toggle(&st, items, 4, 6.0) && !st.on, "toggle off");
    CHECK(!nvg_rules_ads_allowed(&st, 6.3, 0.6f), "no ADS while flipping up");
    CHECK(nvg_rules_ads_allowed(&st, 6.7, 0.6f), "ADS after the flip up");
    // battery swap: the empty one becomes full, one battery_aa consumed
    u16 charge = 0;
    int batteries = 2;
    CHECK(nvg_rules_swap_battery(&charge, &batteries) && charge == NVG_CHARGE_FULL && batteries == 1, "battery swap");
    batteries = 0;
    charge = 10;
    CHECK(!nvg_rules_swap_battery(&charge, &batteries) && charge == 10, "no battery, no swap");
    // the drain turns it off at 0
    st = (NvgRuleState){ .on = true, .uid = 12 };
    u16 ch = 3;
    bool died = false;
    for (int i = 0; i < 100 && !died; i++) died = nvg_rules_drain(&st, &ch, nvg_variant(&g_nc, "p43"), false, 10.0f);
    CHECK(died && !st.on && ch == 0, "battery empty switches off");
}

int main(void) {
    test_data();
    test_radiance();
    test_surfaces();
    test_entities();
    test_thermal_optics();
    test_nvg();
    test_rules();
    printf("night_optics: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
