// Day / night unit tests (src/world/time_of_day.c): sun elevation / azimuth against reference
// values for given latitude / date / time, moon phase (illuminated fraction, lit side, the disc
// shading the sky shader uses), time state replication round trip + late join, anchoring
// (time_set / time_scale / pause / time_days), curves, events and light schedules.
#include "world/time_of_day.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Independent declination (Spencer 1971 Fourier series, error < 0.035 deg) for the cross check.
static double spencer_decl_deg(double doy) {
    double g = 2.0 * 3.14159265358979 / 365.0 * (doy - 1.0);
    double d = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g) - 0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
    return d * 180.0 / 3.14159265358979;
}

static void test_sun(void) {
    float el, az;
    // Solar noon: elevation = 90 - latitude + declination, the sun due south (north hemisphere).
    struct { float lat; double doy; float el_ref; float az_ref; const char *what; } noon[] = {
        { 40.0f, 172.0, 73.44f, 180.0f, "40N June solstice noon" },
        { 40.0f, 355.0, 26.56f, 180.0f, "40N December solstice noon" },
        { 51.48f, 172.0, 61.96f, 180.0f, "Greenwich (51.48N) June solstice noon" },
        { -33.9f, 172.0, 32.66f, 0.0f, "Cape Town (33.9S) June solstice noon (sun due north)" },
        { 0.0f, 80.0, 89.6f, -1.0f, "equator, March equinox noon (near zenith)" },
    };
    for (int i = 0; i < (int)(sizeof(noon) / sizeof(noon[0])); i++) {
        tod_sun_angles(noon[i].lat, 23.44f, noon[i].doy, 12.0f, &el, &az);
        CHECK(fabsf(el - noon[i].el_ref) < 0.5f, "%s: elevation %.2f vs %.2f", noon[i].what, el, noon[i].el_ref);
        if (noon[i].az_ref >= 0 && el < 89.0f) {
            float da = fabsf(fmodf(az - noon[i].az_ref + 540.0f, 360.0f) - 180.0f);
            CHECK(da < 0.5f, "%s: azimuth %.2f vs %.2f", noon[i].what, az, noon[i].az_ref);
        }
        printf("  %-52s elev %6.2f (ref %6.2f) az %6.2f\n", noon[i].what, el, noon[i].el_ref, az);
    }
    // Sunrise hour (centre on the horizon): cos H0 = -tan(lat) tan(dec).
    {
        float lat = 47.0f;
        double doy = 172.0;
        double dec = spencer_decl_deg(doy + 0.25) * 3.14159265358979 / 180.0;
        double H0 = acos(-tan(lat * 3.14159265358979 / 180.0) * tan(dec)) * 180.0 / 3.14159265358979;
        float sunrise = (float)(12.0 - H0 / 15.0), sunset = (float)(12.0 + H0 / 15.0);
        tod_sun_angles(lat, 23.44f, doy, sunrise, &el, &az);
        CHECK(fabsf(el) < 0.5f, "47N June sunrise %.2f h: elevation %.2f (expected 0)", sunrise, el);
        CHECK(az > 45.0f && az < 60.0f, "47N June sunrise azimuth %.1f (north-east, expected ~52)", az);
        tod_sun_angles(lat, 23.44f, doy, sunset, &el, &az);
        CHECK(fabsf(el) < 0.5f, "47N June sunset %.2f h: elevation %.2f", sunset, el);
        CHECK(az > 300.0f && az < 315.0f, "47N June sunset azimuth %.1f (north-west, expected ~308)", az);
        printf("  47N June: sunrise %.2f h, sunset %.2f h, day %.2f h\n", sunrise, sunset, sunset - sunrise);
    }
    // Equinox sunrise: due east at 06:00 everywhere.
    tod_sun_angles(60.0f, 23.44f, 80.0, 6.0f, &el, &az);
    CHECK(fabsf(el) < 0.7f && fabsf(az - 90.0f) < 1.0f, "60N equinox 06:00: elev %.2f az %.2f (expected 0 / 90)", el, az);
    // Declination over the year against Spencer.
    float worst = 0;
    for (int d = 1; d <= 365; d += 4) {
        // At noon on the meridian: elevation = 90 - lat + dec -> dec = elev - 90 + lat (lat 30 keeps it below the zenith).
        tod_sun_angles(30.0f, 23.44f, (double)d, 12.0f, &el, &az);
        double dec = el - 90.0 + 30.0;
        worst = fmaxf(worst, (float)fabs(dec - spencer_decl_deg(d + 0.5)));
    }
    CHECK(worst < 0.35f, "declination vs Spencer: worst %.3f deg", worst);
    printf("  declination vs Spencer over the year: worst %.3f deg\n", worst);
    // Polar day / night.
    tod_sun_angles(78.0f, 23.44f, 172.0, 0.0f, &el, &az);
    CHECK(el > 0.0f, "78N midsummer midnight: midnight sun (elev %.2f)", el);
    tod_sun_angles(78.0f, 23.44f, 355.0, 12.0f, &el, &az);
    CHECK(el < 0.0f, "78N midwinter noon: polar night (elev %.2f)", el);
}

static void test_moon(void) {
    TodConfig c;
    tod_config_default(&c);
    struct { float age; float illum; bool waxing; const char *what; } ph[] = {
        { 0.0f, 0.0f, true, "new moon" },
        { 7.38f, 0.5f, true, "first quarter" },
        { 14.765f, 1.0f, false, "full moon" },
        { 22.15f, 0.5f, false, "last quarter" },
    };
    for (int i = 0; i < 4; i++) {
        c.moon_age_days = ph[i].age;
        TodSky s;
        tod_sky(&c, 0.5, &s);   // noon of day 0 (the age advances 0.5 day)
        c.moon_age_days = ph[i].age - 0.5f;
        tod_sky(&c, 0.5, &s);
        CHECK(fabsf(s.moon_illum - ph[i].illum) < 0.01f, "%s: illuminated %.3f vs %.3f", ph[i].what, s.moon_illum, ph[i].illum);
        if (ph[i].illum > 0.1f && ph[i].illum < 0.9f) CHECK(s.waxing == ph[i].waxing, "%s: waxing %d", ph[i].what, s.waxing);
        // The sky shader lights the moon disc with the sun direction: lit area fraction of the
        // visible disc (Monte Carlo over the disc) must equal the illuminated fraction.
        vec3 m = s.moon_dir, sun = s.sun_dir;
        vec3 up = fabsf(m.y) > 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
        vec3 rx = v3_norm(v3_cross(up, m)), ry = v3_cross(m, rx);
        int lit = 0, total = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                float u = (x + 0.5f) / 32.0f - 1.0f, v = (y + 0.5f) / 32.0f - 1.0f;
                float r2 = u * u + v * v;
                if (r2 > 1.0f) continue;
                vec3 n = v3_add(v3_add(v3_scale(rx, u), v3_scale(ry, v)), v3_scale(m, -sqrtf(1.0f - r2)));   // faces the viewer
                total++;
                if (v3_dot(n, sun) > 0.0f) lit++;
            }
        float frac = (float)lit / (float)total;
        CHECK(fabsf(frac - s.moon_illum) < 0.04f, "%s: shaded disc lit %.3f vs illuminated %.3f", ph[i].what, frac, s.moon_illum);
        printf("  moon %-14s illum %.3f, disc lit %.3f, elongation from the sun %.1f deg\n", ph[i].what, s.moon_illum, frac,
               acosf(fmaxf(-1.0f, fminf(1.0f, v3_dot(s.moon_dir, s.sun_dir)))) * 57.2958f);
    }
    // Full moon rises at sunset (opposite the sun).
    c.moon_age_days = 14.765f - 0.75f;
    TodSky s;
    tod_sky(&c, 0.75, &s);   // 18:00
    CHECK(v3_dot(s.moon_dir, s.sun_dir) < -0.95f, "full moon opposite the sun (dot %.3f)", v3_dot(s.moon_dir, s.sun_dir));
}

static void test_state(void) {
    TodConfig c;
    tod_config_default(&c);
    c.start_hours = 18.5f;
    c.day_length_minutes = 24.0f;
    TimeState host;
    tod_state_init(&host, &c, 1000);
    CHECK(fabsf(tod_hours(&host, 1000) - 18.5f) < 1e-3f, "start time %.4f", tod_hours(&host, 1000));
    // 24 real minutes per day: 60 s = 1 in-game hour.
    CHECK(fabsf(tod_hours(&host, 1000 + 60 * 60) - 19.5f) < 1e-3f, "one real minute = one hour: %.4f", tod_hours(&host, 1000 + 3600));
    // Round trip on the wire, late join: the client evaluates the same time at any later tick.
    u8 wire[20];
    tod_pack(&host, wire);
    TimeState cl;
    tod_unpack(&cl, wire);
    CHECK(!memcmp(&cl, &host, sizeof(cl)), "pack / unpack round trip");
    for (u32 t = 1000; t < 1000 + 60 * 60 * 30; t += 7919) {
        float a = tod_hours(&host, t), b = tod_hours(&cl, t);
        CHECK(fabsf(a - b) < 1e-5f, "late join at tick %u: host %.5f client %.5f", t, a, b);
    }
    // Commands re-anchor without a jump.
    double t0 = 50000.0;
    float before = tod_hours(&host, t0);
    tod_set_day_length(&host, t0, 60.0f);                       // time_scale: 24 h in 60 s
    CHECK(fabsf(tod_hours(&host, t0) - before) < 1e-3f, "time_scale keeps the time: %.4f vs %.4f", tod_hours(&host, t0), before);
    CHECK(fabsf(tod_hours(&host, t0 + 60 * 60) - before) < 1e-3f || fabsf(fabsf(tod_hours(&host, t0 + 3600) - before) - 24.0f) < 1e-3f,
          "24 h in 60 s: %.4f after 60 s (was %.4f)", tod_hours(&host, t0 + 3600), before);
    tod_set_time(&host, t0, 6.25f);
    CHECK(fabsf(tod_hours(&host, t0) - 6.25f) < 1e-3f, "time_set 06:15 -> %.4f", tod_hours(&host, t0));
    int day = (int)tod_days(&host, t0);
    tod_add_days(&host, t0, 3);
    CHECK((int)tod_days(&host, t0) == day + 3, "time_days 3: day %d -> %d", day, (int)tod_days(&host, t0));
    tod_set_day_length(&host, t0, 0.0f);                        // pause
    CHECK(fabsf(tod_hours(&host, t0 + 100000) - 6.25f) < 1e-3f, "paused: %.4f", tod_hours(&host, t0 + 100000));
    CHECK(host.version > 1, "version bumps on change (%u)", host.version);
    float hh = tod_parse_hours("18:30");
    CHECK(fabsf(hh - 18.5f) < 1e-4f, "parse 18:30 -> %.3f", hh);
    char buf[16];
    tod_format_hours(18.5f, buf, sizeof(buf));
    CHECK(!strcmp(buf, "18:30"), "format 18.5 -> %s", buf);
}

static void test_curves_events_lamps(void) {
    TodConfig c;
    tod_config_default(&c);
    TodCurveKeys *k = &c.curves[TOD_FOG_DENSITY];
    k->count = 3;
    k->hour[0] = 6; k->value[0] = 1;
    k->hour[1] = 12; k->value[1] = 0;
    k->hour[2] = 18; k->value[2] = 0.5f;
    CHECK(fabsf(tod_curve(&c, TOD_FOG_DENSITY, 9) - 0.5f) < 1e-4f, "curve mid %.3f", tod_curve(&c, TOD_FOG_DENSITY, 9));
    // Wrap between 18:00 and 06:00 (+24): midnight is halfway.
    CHECK(fabsf(tod_curve(&c, TOD_FOG_DENSITY, 0) - 0.75f) < 1e-4f, "curve wrap at midnight %.3f", tod_curve(&c, TOD_FOG_DENSITY, 0));
    CHECK(fabsf(tod_curve(&c, TOD_FOG_DENSITY, 21) - 0.625f) < 1e-4f, "curve wrap evening %.3f", tod_curve(&c, TOD_FOG_DENSITY, 21));
    // Events over one day at 47N, June: one sunrise, one sunset, one noon, one midnight.
    int sr = 0, ss = 0, mn = 0, no = 0;
    double step = 1.0 / 1440.0;
    for (double d = 0.0; d < 1.0; d += step) {
        u32 e = tod_events_between(&c, d, d + step);
        if (e & TOD_EV_SUNRISE) sr++;
        if (e & TOD_EV_SUNSET) ss++;
        if (e & TOD_EV_MIDNIGHT) mn++;
        if (e & TOD_EV_NOON) no++;
    }
    CHECK(sr == 1 && ss == 1 && mn == 1 && no == 1, "events per day: sunrise %d sunset %d midnight %d noon %d", sr, ss, mn, no);
    // Lamps: on at night, off by day, per-light offsets within the jitter.
    TodLightSchedule ls = { 18.75f, 6.5f, 25.0f, 0.0f };
    int on_night = 0, on_day = 0;
    for (u32 id = 1; id <= 50; id++) { on_night += tod_light_on(&ls, 23.0f, id); on_day += tod_light_on(&ls, 12.0f, id); }
    CHECK(on_night == 50 && on_day == 0, "lamps: %d / 50 on at 23:00, %d on at noon", on_night, on_day);
    int differ = 0;
    for (u32 id = 1; id <= 50; id++) differ += tod_light_on(&ls, 18.75f, id) != tod_light_on(&ls, 18.75f, 1);
    CHECK(differ > 5, "per-light offsets: %d lamps switch at a different minute than lamp 1", differ);
}

int main(void) {
    printf("sun:\n");
    test_sun();
    printf("moon:\n");
    test_moon();
    test_state();
    test_curves_events_lamps();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
