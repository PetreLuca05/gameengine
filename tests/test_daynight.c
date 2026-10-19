// Day / night unit tests (src/world/time_of_day.c): sun elevation / azimuth against reference
// values for given latitude / date / time, moon phase (illuminated fraction, lit side, the disc
// shading the sky shader uses), time state replication round trip + late join, anchoring
// (time_set / time_scale / pause / time_days), curves, events and light schedules; continuity
// (docs/DAYNIGHT.md "Continuity"): re-anchoring at fractional ticks, the segment before the anchor,
// time_set blends, the sun's per-frame steps over whole days at 30 / 60 / 144 fps, precision after
// 100 and 2100 days, a client behind the host through a speed change, the slewed client clock.
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
    u8 wire[TOD_WIRE_BYTES];
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

// ------------------------------------------------------------------ continuity

#define TWO_PI_D 6.283185307179586

static double dir_angle(vec3 a, vec3 b) {
    double cx = (double)a.y * b.z - (double)a.z * b.y, cy = (double)a.z * b.x - (double)a.x * b.z, cz = (double)a.x * b.y - (double)a.y * b.x;
    return atan2(sqrt(cx * cx + cy * cy + cz * cz), (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z);
}

// One day sampled per render frame: the clock and the sun's per-frame steps against the exact rate.
// Returns the worst |step / expected - 1| of the sun; counts azimuth / elevation reversals.
static double sweep_day(const TodConfig *c, double start_day, float day_len_s, double fps, int *az_back, int *el_back, double *clock_err) {
    TimeState s;
    TodConfig cc = *c;
    cc.start_hours = 0.0f;
    cc.day_length_minutes = day_len_s / 60.0f;
    tod_state_init(&s, &cc, 0.0);
    if (start_day > 0) tod_add_days(&s, 0.0, (int)start_day);
    double dt = 1.0 / fps, rate = TOD_SEC_PER_DAY / day_len_s, worst = 0, cerr = 0;
    int frames = (int)(day_len_s * fps);
    TodSky prev, cur;
    double prev_sec = tod_seconds(&s, 0.0);
    tod_sky(&cc, prev_sec / TOD_SEC_PER_DAY, &prev);
    *az_back = *el_back = 0;
    for (int f = 1; f <= frames; f++) {
        double tick = f * dt * 60.0;
        double sec = tod_seconds(&s, tick);
        cerr = fmax(cerr, fabs((sec - prev_sec) / (rate * dt) - 1.0));
        tod_sky(&cc, sec / TOD_SEC_PER_DAY, &cur);
        double dH = (sec - prev_sec) / TOD_SEC_PER_DAY * TWO_PI_D;
        double expect = 2.0 * asin(cos((double)cur.declination) * sin(dH * 0.5));
        double r = dir_angle(prev.sun_dir, cur.sun_dir) / expect;
        worst = fmax(worst, fabs(r - 1.0));
        double da = (double)cur.sun_azim - (double)prev.sun_azim;
        if (da > 3.14159) da -= TWO_PI_D;
        if (da < -3.14159) da += TWO_PI_D;
        if (da < 0.0) (*az_back)++;
        double de = (double)cur.sun_elev - (double)prev.sun_elev;
        if (fabs(sin(cur.hour_angle)) > 0.02 && de * -sin(cur.hour_angle) < 0.0) (*el_back)++;
        prev = cur;
        prev_sec = sec;
    }
    *clock_err = cerr;
    return worst;
}

static void test_continuity(void) {
    printf("continuity:\n");
    TodConfig c;
    tod_config_default(&c);
    c.start_hours = 12.0f;
    c.day_length_minutes = 24.0f;
    // Re-anchoring at a fractional tick: no jump, the new rate from there on.
    TimeState s, old;
    tod_state_init(&s, &c, 1000.0);
    double t = 1234.37;
    double before = tod_seconds(&s, t);
    old = s;
    tod_set_day_length(&s, t, 24.0f);   // time_scale 60
    CHECK(fabs(tod_seconds(&s, t) - before) < 1e-9, "time_scale at tick %.2f: %.9f -> %.9f game s", t, before, tod_seconds(&s, t));
    CHECK(fabs((tod_seconds(&s, t + 1.0) - tod_seconds(&s, t)) - 60.0) < 1e-6, "the new rate: %.6f game s per tick (60)", tod_seconds(&s, t + 1.0) - tod_seconds(&s, t));
    // A client rendering behind the anchor sees the clock the host showed before the change.
    double worst = 0;
    for (double u = t - 12.0; u < t; u += 0.37) worst = fmax(worst, fabs(tod_seconds(&s, u) - tod_seconds(&old, u)));
    CHECK(worst < 1e-6, "before the anchor: the old clock (max diff %.2e game s)", worst);
    CHECK(tod_rate(&s, t - 1.0) == 60.0 && tod_rate(&s, t + 1.0) == 3600.0, "rate before / after the anchor: %.1f / %.1f game s per s", tod_rate(&s, t - 1.0),
          tod_rate(&s, t + 1.0));
    // Pause and resume at fractional ticks: no jump either way.
    double t2 = 1500.81, v2 = tod_seconds(&s, t2);
    tod_set_day_length(&s, t2, 0.0f);
    CHECK(fabs(tod_seconds(&s, t2) - v2) < 1e-9 && fabs(tod_seconds(&s, t2 + 500.0) - v2) < 1e-9, "pause holds %.6f", tod_seconds(&s, t2 + 500.0));
    tod_set_day_length(&s, t2 + 500.0, 1440.0f);
    CHECK(fabs(tod_seconds(&s, t2 + 500.0) - v2) < 1e-9, "resume continues %.6f (%.6f)", tod_seconds(&s, t2 + 500.0), v2);

    // time_set blend: 12:00 -> 18:00 over 1.5 s at the default day; continuous, no step larger than
    // the ease allows, monotonic (forward), at the target (plus the rate) once it ends.
    TimeState b;
    tod_state_init(&b, &c, 0.0);
    double tb = 600.25, v0 = tod_seconds(&b, tb);
    tod_blend_time(&b, tb, 18.0f, 1.5f);
    double off = 18.0 * 3600.0 - (v0 - floor(v0 / 86400.0) * 86400.0);
    CHECK(fabs(tod_seconds(&b, tb) - v0) < 1e-9, "blend starts at the old time (%.6f vs %.6f)", tod_seconds(&b, tb), v0);
    double prev = v0, max_step = 0, bound = (60.0 + 1.5 * fabs(off) / 1.5) / 144.0 * 1.0001;
    bool mono = true;
    for (int f = 1; f <= 300; f++) {
        double u = tb + f * 60.0 / 144.0, v = tod_seconds(&b, u);
        max_step = fmax(max_step, fabs(v - prev));
        if (v < prev) mono = false;
        prev = v;
    }
    CHECK(max_step <= bound, "blend: max step %.3f game s per frame (bound %.3f)", max_step, bound);
    CHECK(mono, "blend forward: monotonic");
    double end = tod_seconds(&b, tb + 90.0 + 60.0), want = floor(v0 / 86400.0) * 86400.0 + 18.0 * 3600.0 + 150.0;
    CHECK(fabs(end - want) < 1e-6, "blend ends at 18:00 + the rate (%.6f vs %.6f)", end, want);
    CHECK(tod_blending(&b, tb + 10.0) && !tod_blending(&b, tb + 91.0), "blending flag");
    // A speed change during the blend keeps easing to the time that was set.
    TimeState b2;
    tod_state_init(&b2, &c, 0.0);
    tod_blend_time(&b2, 100.0, 6.0f, 2.0f);
    double tc = 160.5, vc = tod_seconds(&b2, tc);
    tod_set_day_length(&b2, tc, 24.0f);
    CHECK(fabs(tod_seconds(&b2, tc) - vc) < 1e-9, "time_scale inside a blend: no jump (%.6f vs %.6f)", tod_seconds(&b2, tc), vc);
    double tgt = 6.0 * 3600.0 + (tc - 100.0) * 1.0 + (230.0 - tc) * 60.0;   // 06:00 + 1 game s per tick until tc, 60 after
    CHECK(fabs(tod_seconds(&b2, 230.0) - tgt) < 1e-6, "... and lands on 06:00 + the clock (%.6f vs %.6f)", tod_seconds(&b2, 230.0), tgt);
    // An instant set: a step at the anchor, the old clock before it.
    TimeState st;
    tod_state_init(&st, &c, 0.0);
    double vi = tod_seconds(&st, 99.0);
    tod_set_time(&st, 100.0, 3.0f);
    CHECK(fabs(tod_seconds(&st, 99.0) - vi) < 1e-9 && fabs(tod_hours(&st, 100.0) - 3.0f) < 1e-5f, "instant: old before (%.3f), 03:00 at the anchor (%.5f)", tod_seconds(&st, 99.0),
          tod_hours(&st, 100.0));

    // The sun per render frame over whole days (spec: monotonic, per-frame deltas within +-5 % of the
    // rate, no zero frames, no spikes): a 10-minute day at 30 / 60 / 144 fps, the default day, the
    // editor's longest (240 min), at day 0, 100 and 2100.
    struct { float len; double fps, day; const char *what; } runs[] = {
        { 600.0f, 30.0, 0, "10-minute day, 30 fps" }, { 600.0f, 60.0, 0, "10-minute day, 60 fps" }, { 600.0f, 144.0, 0, "10-minute day, 144 fps" },
        { 24.0f, 144.0, 0, "time_scale 60 (24 s), 144 fps" }, { 1440.0f, 144.0, 0, "default day (24 min), 144 fps" },
        { 14400.0f, 144.0, 0, "240-minute day, 144 fps" }, { 600.0f, 144.0, 100, "10-minute day after 100 days, 144 fps" },
        { 1440.0f, 144.0, 2100, "default day after 2100 days, 144 fps" },
    };
    for (int i = 0; i < (int)(sizeof(runs) / sizeof(runs[0])); i++) {
        int azb, elb;
        double cerr;
        double wr = sweep_day(&c, runs[i].day, runs[i].len, runs[i].fps, &azb, &elb, &cerr);
        printf("  continuity %-40s sun step / rate within %.4f %%, reversals az %d el %d, clock %.1e\n", runs[i].what, wr * 100.0, azb, elb, cerr);
        CHECK(wr < 0.05 && azb == 0 && elb == 0 && cerr < 1e-6, "%s: sun step within %.3f %% of the rate (max), azimuth reversals %d, elevation reversals %d, clock %.1e",
              runs[i].what, wr * 100.0, azb, elb, cerr);
    }
    // Float precision after 100 game days: the clock's per-frame step at 144 fps is exact.
    TimeState d100;
    tod_state_init(&d100, &c, 0.0);
    tod_add_days(&d100, 0.0, 100);
    double a = tod_seconds(&d100, 5e6), a1 = tod_seconds(&d100, 5e6 + 60.0 / 144.0);
    CHECK(fabs((a1 - a) - 60.0 / 144.0) < 1e-6, "day 100+: one 144 fps frame = %.9f game s (%.9f)", a1 - a, 60.0 / 144.0);

    // Version: never 0 once set (0 = no state yet).
    TimeState vw;
    tod_state_init(&vw, &c, 0.0);
    vw.version = 65535;
    tod_set_day_length(&vw, 1.0, 60.0f);
    CHECK(vw.version == 1, "version wraps to 1 (%u)", vw.version);
    u8 bad[TOD_WIRE_BYTES];
    tod_pack(&vw, bad);
    double nan = NAN;
    memcpy(bad + 8, &nan, 8);
    TimeState bu;
    tod_unpack(&bu, bad);
    CHECK(bu.version == 0, "a non-finite clock on the wire is dropped");
}

// A client renders ~6.3 ticks behind the host and receives each state 2 frames after it was made;
// the host goes 1 -> 60 -> 0 -> 1 (pause, resume). Through the timeline the client shows exactly the
// host's clock at the same tick: no jump at any change.
static void test_client_timeline(void) {
    TodConfig c;
    tod_config_default(&c);
    c.start_hours = 9.0f;
    TimeState host;
    tod_state_init(&host, &c, 0.0);
    TodTimeline tl;
    tod_timeline_reset(&tl);
    tod_timeline_push(&tl, &host);
    TimeState pending[8];
    int pend_at[8], np = 0;
    u16 sent = host.version;
    double dt = 1.0 / 144.0, worst = 0, prev = -1, max_rate_err = 0;
    int bad_steps = 0;
    for (int f = 1; f < 144 * 20; f++) {
        double ht = f * dt * 60.0 + 0.123;   // the host renders tick + alpha
        if (f == 144 * 3) tod_set_day_length(&host, ht, 24.0f);
        if (f == 144 * 7) tod_set_day_length(&host, ht, 0.0f);
        if (f == 144 * 9) tod_set_day_length(&host, ht, 24.0f);
        if (f == 144 * 12) tod_set_day_length(&host, ht, 1440.0f);
        if (f == 144 * 15) tod_blend_time(&host, ht, 20.0f, 1.5f);
        if (host.version != sent && np < 8) { pending[np] = host; pend_at[np] = f + 2; np++; sent = host.version; }
        for (int i = 0; i < np; i++)
            if (pend_at[i] == f) tod_timeline_push(&tl, &pending[i]);
        double ct = ht - 6.3;
        const TimeState *cs = tod_timeline_at(&tl, ct);
        double v = tod_seconds(cs, ct), hv = tod_seconds(&host, ct);
        worst = fmax(worst, fabs(v - hv));
        if (prev >= 0) {
            double step = v - prev, lo = 0.0, hi = 3600.0 * dt * 1.0001 + 1e-9;
            if (tod_blending(cs, ct)) hi += 1.5 * fabs(cs->blend_sec) / 1.5 * dt;
            if (step < lo - 1e-9 || step > hi) bad_steps++;
            max_rate_err = fmax(max_rate_err, step - hi);
        }
        prev = v;
    }
    CHECK(worst < 1e-6, "client behind the host: the host's clock at the same tick (max diff %.2e game s)", worst);
    CHECK(bad_steps == 0, "client: every frame steps between 0 and the fastest rate (+ the blend's ease): %d outside", bad_steps);
}

// The client clock: a target (the interpolated server tick) that jitters +-5 ms, jumps forward 8 ms
// on early packets and drifts back; the clock's correction never exceeds the slew rate, it follows
// within a tick and snaps only on a real stall (0.5 s).
static void test_client_clock(void) {
    TodClock k;
    memset(&k, 0, sizeof(k));
    unsigned rng = 12345u;
    double offset = 0.0, max_corr = 0, max_err = 0;
    for (int f = 0; f < 144 * 60; f++) {
        double now = f / 144.0;
        rng = rng * 1664525u + 1013904223u;
        double jitter = ((rng >> 8) / 16777216.0 - 0.5) * 0.010;
        if (f % 97 == 0) offset += 0.008;            // an early packet: the estimate jumps forward
        offset *= 0.9995;                              // and drifts back
        double target = (now + offset + jitter) * 60.0 + 1000.0;
        tod_clock_step(&k, target, 1.0 / 144.0, 60.0, 0.02, 0.5, 15.0);
        if (f > 0) max_corr = fmax(max_corr, fabs(k.corr));
        if (f > 144 * 5) max_err = fmax(max_err, fabs(k.tick - target));
    }
    CHECK(max_corr <= 0.02 + 1e-12, "client clock: correction max %.4f of the advance (slew 0.02)", max_corr);
    CHECK(max_err < 1.5 && k.snaps == 0, "client clock: within %.2f ticks of the target, %d snaps", max_err, k.snaps);
    // A stall (the target 0.5 s ahead): one snap.
    tod_clock_step(&k, k.tick + 30.0 + 60.0 / 144.0, 1.0 / 144.0, 60.0, 0.02, 0.5, 15.0);
    CHECK(k.snaps == 1, "client clock: a 0.5 s stall snaps once (%d)", k.snaps);
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
    test_continuity();
    test_client_timeline();
    test_client_clock();
    test_curves_events_lamps();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
