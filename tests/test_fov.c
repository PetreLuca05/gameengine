// The FOV model (src/player/fov.c, docs/CAMERA_HOLDPOINT.md "Field of view"):
//  1) Field of View 80 .. 120 is horizontal at 16:9, Hor+: the same vertical FOV on 16:9 / 21:9 / 4:3.
//  2) ADS Field of View: Independent = the sight's FOV at 80 for any Field of View; Affected = that
//     x Field of View / 80; scopes magnify from the ADS base.
//  3) The hip -> ADS blend: exact at both ends, monotonic, no step above the smoothstep curve's peak.
//  4) ADS sensitivity from the resulting FOVs (both modes).
//  5) Sprint and the fire punch are multipliers on the current FOV.
//  6) Weapon Field of View: Narrow < Default < Wide, the lateral scale keeps a point on its pixel.
//  7) The settings round-trip through a UserCmd (spectators).
#include "player/fov.h"
#include "core/dgmath.h"
#include <math.h>
#include <stdio.h>

static int g_checks, g_fails;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, e) (fabsf((a) - (b)) <= (e))

int main(void) {
    // 1) Hor+.
    {
        float v80 = fov_h16_to_v(80.0f), v100 = fov_h16_to_v(100.0f), v120 = fov_h16_to_v(120.0f);
        float v150 = fov_h16_to_v(FOV_MAX);
        CHECK(NEAR(v80, 50.534f, 0.01f) && NEAR(v100, 67.668f, 0.01f) && NEAR(v120, 88.511f, 0.01f), "vertical of 80 / 100 / 120: %.3f %.3f %.3f", v80, v100, v120);
        CHECK(FOV_MAX == 150.0f && NEAR(v150, 129.06f, 0.01f), "the slider's top: %.0f = %.3f vertical", FOV_MAX, v150);
        // Sprint widening on top of the widest setting stays a valid projection.
        CHECK(fov_sprint(v150, 0.12f) < 179.0f, "sprint at 150: %.2f", fov_sprint(v150, 0.12f));
        static const struct { const char *name; float aspect, h80; } k[] = { { "16:9", 16.0f / 9.0f, 80.0f }, { "21:9", 64.0f / 27.0f, 96.42f }, { "4:3", 4.0f / 3.0f, 64.37f } };
        for (int i = 0; i < 3; i++) {
            float h = fov_v_to_h(v80, k[i].aspect);
            CHECK(NEAR(h, k[i].h80, 0.05f), "80 at %s: horizontal %.2f (expected %.2f)", k[i].name, h, k[i].h80);
            // Hor+: the vertical FOV is the one the setting names, whatever the aspect.
            float back = 2.0f * atanf(tanf(h * 0.5f * DG_DEG2RAD) / k[i].aspect) * DG_RAD2DEG;
            CHECK(NEAR(back, v80, 1e-3f), "%s: vertical %.3f, expected %.3f", k[i].name, back, v80);
            printf("  Field of View 80 at %s: %.2f deg horizontal, %.2f vertical\n", k[i].name, h, back);
        }
        for (float f = FOV_MIN; f <= FOV_MAX; f += 5.0f) CHECK(NEAR(fov_v_to_h16(fov_h16_to_v(f)), f, 1e-3f), "round trip %.0f", f);
    }
    // 2) ADS Field of View (irons / red dot world zoom 1.5, scopes 4x / 8x).
    {
        static const float zooms[] = { 1.0f, 1.5f, 4.0f, 8.0f };
        float worst_ind = 0, worst_aff = 0;
        for (int z = 0; z < 4; z++) {
            float hs = fov_zoom(80.0f, zooms[z]);
            for (float f = FOV_MIN; f <= FOV_MAX; f += 1.0f) {
                worst_ind = fmaxf(worst_ind, fabsf(fov_ads(FOV_ADS_INDEPENDENT, f, zooms[z]) - hs));
                worst_aff = fmaxf(worst_aff, fabsf(fov_ads(FOV_ADS_AFFECTED, f, zooms[z]) - hs * f / 80.0f));
            }
        }
        CHECK(worst_ind < 1e-4f, "Independent: ADS FOV differs from the sight's FOV at 80 by %g", worst_ind);
        CHECK(worst_aff < 1e-4f, "Affected: ADS FOV differs from sight x FOV / 80 by %g", worst_aff);
        CHECK(NEAR(fov_ads_base(FOV_ADS_INDEPENDENT, 110.0f), 80.0f, 1e-6f) && NEAR(fov_ads_base(FOV_ADS_AFFECTED, 110.0f), 110.0f, 1e-6f), "ADS base");
        // A 4x scope magnifies 4x from the base: the lens FOV's tangent is the base's / 4.
        float lens = fov_zoom(fov_h16_to_v(fov_ads_base(FOV_ADS_INDEPENDENT, 120.0f)), 4.0f);
        CHECK(NEAR(tanf(lens * 0.5f * DG_DEG2RAD) * 4.0f, tanf(fov_h16_to_v(80.0f) * 0.5f * DG_DEG2RAD), 1e-5f), "4x scope from 80");
        printf("  ADS: Independent = the sight at 80 (max error %.2g), Affected = sight x FOV / 80 (%.2g); irons at 120: %.2f / %.2f deg (h16)\n", worst_ind, worst_aff,
               fov_ads(FOV_ADS_INDEPENDENT, 120.0f, 1.5f), fov_ads(FOV_ADS_AFFECTED, 120.0f, 1.5f));
    }
    // 3) The blend: hip -> ADS over a 0.2 s ADS at 60 fps (12 frames) and 240 fps.
    {
        static const int frames[] = { 12, 48 };
        for (int fi = 0; fi < 2; fi++)
            for (int m = 0; m < FOV_ADS_MODES; m++)
                for (float f = FOV_MIN; f <= FOV_MAX; f += 20.0f) {
                    float hip = fov_h16_to_v(f), ads = fov_h16_to_v(fov_ads(m, f, 1.5f));
                    int n = frames[fi];
                    float prev = fov_ads_blend(hip, ads, 0.0f), max_step = 0;
                    bool mono = true;
                    for (int i = 1; i <= n; i++) {
                        float cur = fov_ads_blend(hip, ads, (float)i / (float)n);
                        max_step = fmaxf(max_step, fabsf(cur - prev));
                        if (cur > prev + 1e-5f) mono = false;
                        prev = cur;
                    }
                    float mean = fabsf(hip - ads) / (float)n;
                    CHECK(NEAR(fov_ads_blend(hip, ads, 0.0f), hip, 1e-4f) && NEAR(fov_ads_blend(hip, ads, 1.0f), ads, 1e-4f), "blend ends (mode %d, fov %.0f)", m, f);
                    CHECK(mono, "blend not monotonic (mode %d, fov %.0f)", m, f);
                    // smoothstep's steepest slope is 1.5x the mean; the tangent-space blend front-loads a
                    // big zoom a little more (1.8x at 120 -> irons). A snap would be many times the mean.
                    CHECK(max_step <= mean * 2.5f + 1e-4f, "blend jump %.3f deg in one frame (mean %.3f; mode %d, fov %.0f, %d frames)", max_step, mean, m, f, n);
                }
        printf("  blend: exact ends, monotonic, steepest frame <= 2.5x the mean step (2 modes x 3 FOVs x 60 / 240 fps)\n");
    }
    // 4) Sensitivity from the resulting FOVs.
    {
        for (int m = 0; m < FOV_ADS_MODES; m++)
            for (float f = FOV_MIN; f <= FOV_MAX; f += 20.0f) {
                float hip = fov_h16_to_v(f), ads = fov_h16_to_v(fov_ads(m, f, 1.5f));
                float r = fov_sens_scale("fov_ratio", 1.0f, ads, hip), md = fov_sens_scale("monitor_distance", 1.0f, ads, hip);
                float md0 = fov_sens_scale("monitor_distance", 1e-4f, ads, hip);
                CHECK(NEAR(r, ads / hip, 1e-5f), "fov_ratio");
                CHECK(md > 0.0f && md < 1.0f && NEAR(md0, tanf(ads * 0.5f * DG_DEG2RAD) / tanf(hip * 0.5f * DG_DEG2RAD), 1e-3f), "monitor_distance %.4f (c->0 %.4f)", md, md0);
                CHECK(NEAR(fov_sens_scale("off", 1.0f, ads, hip), 1.0f, 0.0f) && NEAR(fov_sens_scale("monitor_distance", 1.0f, hip, hip), 1.0f, 1e-6f), "off / equal FOVs");
            }
        // Independent: the aimed FOV is the same at every Field of View, so is the aimed view's scale
        // against the default (what the reticle moves per count once aimed).
        float a80 = fov_h16_to_v(fov_ads(FOV_ADS_INDEPENDENT, 80.0f, 1.5f)), a120 = fov_h16_to_v(fov_ads(FOV_ADS_INDEPENDENT, 120.0f, 1.5f));
        CHECK(NEAR(a80, a120, 1e-5f), "Independent aimed FOV differs (%.4f / %.4f)", a80, a120);
        printf("  sensitivity: fov_ratio / monitor_distance from the resulting FOVs (both modes); Independent aimed FOV %.2f at 80 and 120\n", a80);
    }
    // 5) Multipliers.
    {
        CHECK(NEAR(fov_sprint(60.0f, 0.0f), 60.0f, 1e-4f) && fov_sprint(60.0f, 0.12f) > 60.0f, "sprint");
        CHECK(NEAR(tanf(fov_sprint(50.0f, 0.1f) * 0.5f * DG_DEG2RAD), tanf(25.0f * DG_DEG2RAD) * 1.1f, 1e-5f), "sprint is a tangent multiplier");
        CHECK(NEAR(fov_punch(75.0f, 2.5f), 72.5f, 1e-4f) && NEAR(fov_punch(50.0f, 2.5f), 50.0f * (1.0f - 2.5f / 75.0f), 1e-4f), "punch");
        printf("  sprint x (1 + w) on the tangent, punch x (1 - p / 75): 75 -> %.2f, 50.5 -> %.2f\n", fov_punch(75.0f, 2.5f), fov_punch(fov_h16_to_v(80.0f), 2.5f));
    }
    // 6) Weapon Field of View.
    {
        static const float presets[WEAPON_FOV_COUNT] = { 60.0f, 70.0f, 85.0f };
        float n = fov_h16_to_v(fov_weapon(presets, WEAPON_FOV_NARROW)), d = fov_h16_to_v(fov_weapon(presets, WEAPON_FOV_DEFAULT)), w = fov_h16_to_v(fov_weapon(presets, WEAPON_FOV_WIDE));
        CHECK(n < d && d < w, "presets order");
        CHECK(NEAR(fov_lateral_scale(d, d), 1.0f, 1e-6f) && fov_lateral_scale(n, d) < 1.0f && fov_lateral_scale(w, d) > 1.0f, "lateral scale");
        // A point at (x, y, -z) moved sideways by the scale lands on the same pixel with the new FOV.
        float x = 0.12f, y = -0.15f, z = 0.4f, k = fov_lateral_scale(w, d);
        float u0 = x / z / tanf(d * 0.5f * DG_DEG2RAD), u1 = x * k / z / tanf(w * 0.5f * DG_DEG2RAD);
        float v0 = y / z / tanf(d * 0.5f * DG_DEG2RAD), v1 = y * k / z / tanf(w * 0.5f * DG_DEG2RAD);
        CHECK(NEAR(u0, u1, 1e-5f) && NEAR(v0, v1, 1e-5f), "lateral scale keeps the pixel");
        CHECK(NEAR(fov_weapon(presets, 7), presets[WEAPON_FOV_WIDE], 0.0f), "preset clamp");
        printf("  Weapon FOV: Narrow %.1f < Default %.1f < Wide %.1f deg vertical, lateral x%.3f / x%.3f\n", n, d, w, fov_lateral_scale(n, d), fov_lateral_scale(w, d));
    }
    // 7) Forwarding.
    {
        bool ok = true;
        for (int m = 0; m < FOV_ADS_MODES; m++)
            for (int w = 0; w < WEAPON_FOV_COUNT; w++)
                for (float f = FOV_MIN; f <= FOV_MAX; f += 1.0f) {
                    FovPrefs p;
                    if (!fov_unpack(fov_pack_h16(f), fov_pack_flags(m, w), &p) || !NEAR(p.fov, f, 1e-4f) || p.ads_mode != m || p.weapon_fov != w) ok = false;
                }
        FovPrefs p;
        CHECK(ok, "pack / unpack round trip");
        CHECK(!fov_unpack(0, 0, &p), "an empty command has no settings");
        printf("  forwarding: every Field of View x ADS mode x Weapon FOV round-trips through a UserCmd\n");
    }
    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
