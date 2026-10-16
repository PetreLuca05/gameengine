// Weather unit tests (src/world/weather.c, docs/WEATHER.md): deterministic blending (same ticks,
// same values; per-parameter durations; the wind direction takes the short way round), preset
// composition, pack / unpack round trip and late join (a client that only has the pack evaluates
// the same weather), surface rates (wetness / puddles / snow cover), wet material ranges, lightning
// determinism and frequency, the scheduler (deterministic, honours the lock), the lens droplet
// simulation (300 frames headless).
#include "world/weather.h"
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

static const float DT = 1.0f / 60.0f;

static const WeatherParams *P(const WeatherLib *l, const char *n) {
    int i = weather_find_preset(l, n);
    if (i < 0) { printf("missing preset %s\n", n); exit(1); }
    return &l->presets[i].p;
}

static void test_presets(void) {
    WeatherLib l;
    weather_lib_defaults(&l);
    const char *names[] = { "clear", "little_cloudy", "cloudy", "overcast", "fog", "light_rain", "rain", "storm", "light_snow", "snow", "blizzard" };
    for (int i = 0; i < 11; i++) CHECK(weather_find_preset(&l, names[i]) >= 0, "preset %s", names[i]);
    CHECK(P(&l, "rain")->v[WP_PRECIP] > P(&l, "light_rain")->v[WP_PRECIP], "rain > light rain");
    CHECK(weather_precip_type(P(&l, "snow")->v[WP_TEMPERATURE]) > 0.99f, "snow preset is snow");
    CHECK(weather_precip_type(P(&l, "rain")->v[WP_TEMPERATURE]) < 0.01f, "rain preset is rain");
    CHECK(fabsf(weather_precip_type(0.5f) - 0.5f) < 0.01f, "half way at 0.5 C: %.3f", weather_precip_type(0.5f));
    // Composition: overcast+rain = overcast with rain's set parameters on top.
    WeatherParams out;
    float tr = 0;
    CHECK(weather_compose(&l, "overcast+rain", P(&l, "clear"), &out, &tr), "compose");
    CHECK(out.v[WP_PRECIP] == P(&l, "rain")->v[WP_PRECIP], "composed precip");
    CHECK(tr >= 120.0f, "composed transition %.0f", tr);
    CHECK(!weather_compose(&l, "overcast+nonsense", P(&l, "clear"), &out, NULL), "unknown preset rejected");
    // Loading from the asset dir (when run from the build output) keeps every preset.
    WeatherLib l2;
    weather_lib_load(&l2, "assets/world/weather");
    CHECK(l2.count == l.count, "loaded presets %d", l2.count);
    CHECK(fabsf(P(&l2, "storm")->v[WP_LIGHTNING] - P(&l, "storm")->v[WP_LIGHTNING]) < 1e-4f, "json matches the built-in storm");
}

static void test_blend(void) {
    WeatherLib l;
    weather_lib_defaults(&l);
    WeatherState s;
    weather_state_init(&s, P(&l, "clear"), 42);
    weather_set_target(&s, P(&l, "storm"), 1000, 120.0f);
    WeatherParams a, b, prev;
    // Determinism: evaluating the same tick twice (or on another machine) gives identical values.
    weather_eval(&s, 4000, DT, &a);
    weather_eval(&s, 4000, DT, &b);
    CHECK(!memcmp(&a, &b, sizeof(a)), "eval deterministic");
    // Endpoints.
    weather_eval(&s, 1000, DT, &a);
    CHECK(fabsf(a.v[WP_CLOUD_COVER] - P(&l, "clear")->v[WP_CLOUD_COVER]) < 1e-5f, "start = from");
    weather_eval(&s, 1000 + 121 * 60, DT, &a);
    for (int k = 0; k < WP_COUNT; k++)
        if (k != WP_WIND_DIR) CHECK(fabsf(a.v[k] - P(&l, "storm")->v[k]) < 1e-4f, "end = to (%s %.4f)", weather_param_name(k), a.v[k]);
    // Monotonic, no jumps (60 Hz steps).
    weather_eval(&s, 1000, DT, &prev);
    float max_step = 0;
    bool mono = true;
    for (u32 t = 1001; t < 1000 + 130 * 60; t++) {
        weather_eval(&s, t, DT, &a);
        if (a.v[WP_PRECIP] < prev.v[WP_PRECIP] - 1e-6f || a.v[WP_CLOUD_COVER] < prev.v[WP_CLOUD_COVER] - 1e-6f) mono = false;
        max_step = fmaxf(max_step, fabsf(a.v[WP_PRECIP] - prev.v[WP_PRECIP]));
        prev = a;
    }
    CHECK(mono, "monotonic transition");
    CHECK(max_step < 0.002f, "smooth (max precip step per tick %.5f)", max_step);
    // Per-parameter durations: rain arrives within a minute, clouds take the whole transition.
    weather_eval(&s, 1000 + 61 * 60, DT, &a);
    CHECK(fabsf(a.v[WP_PRECIP] - 1.0f) < 1e-4f, "rain done after 60 s: %.3f", a.v[WP_PRECIP]);
    CHECK(a.v[WP_CLOUD_COVER] < 0.9f, "clouds still moving at 60 s: %.3f", a.v[WP_CLOUD_COVER]);
    // Instant change.
    weather_set_target(&s, P(&l, "fog"), 20000, 0.0f);
    weather_eval(&s, 20000, DT, &a);
    CHECK(fabsf(a.v[WP_FOG] - P(&l, "fog")->v[WP_FOG]) < 1e-6f, "instant");
    // Wind direction: 350 -> 10 goes through 0, never through 180.
    WeatherParams w0 = *P(&l, "clear"), w1 = w0;
    w0.v[WP_WIND_DIR] = 350.0f;
    w1.v[WP_WIND_DIR] = 10.0f;
    weather_state_init(&s, &w0, 1);
    weather_set_target(&s, &w1, 0, 100.0f);
    bool short_way = true;
    for (u32 t = 0; t < 120 * 60; t += 30) {
        weather_eval(&s, t, DT, &a);
        float d = a.v[WP_WIND_DIR];
        if (d > 15.0f && d < 345.0f) short_way = false;
    }
    CHECK(short_way, "wind direction short arc");
    // A retarget mid-way continues from the current blend (no pop).
    weather_state_init(&s, P(&l, "clear"), 7);
    weather_set_target(&s, P(&l, "rain"), 0, 120.0f);
    weather_eval(&s, 1800, DT, &a);
    weather_set_target(&s, P(&l, "snow"), 1800, 120.0f);
    weather_eval(&s, 1800, DT, &b);
    CHECK(fabsf(a.v[WP_PRECIP] - b.v[WP_PRECIP]) < 1e-5f && fabsf(a.v[WP_CLOUD_COVER] - b.v[WP_CLOUD_COVER]) < 1e-5f, "retarget continuous");
}

static void test_pack(void) {
    WeatherLib l;
    weather_lib_defaults(&l);
    WeatherState host, client;
    weather_state_init(&host, P(&l, "cloudy"), 99);
    weather_set_target(&host, P(&l, "rain"), 5000, 90.0f);
    host.wetness = 0.4f;
    host.puddles = 0.2f;
    host.snow = 0.1f;
    host.locked = 1;
    host.preset = 6;
    host.next_change_tick = 123456;
    u8 buf[WEATHER_PACK_SIZE];
    weather_pack(&host, buf);
    memset(&client, 0xAB, sizeof(client));
    weather_unpack(&client, buf);
    CHECK(client.version == host.version && client.seed == host.seed && client.start_tick == host.start_tick, "pack header");
    CHECK(client.wetness == host.wetness && client.puddles == host.puddles && client.snow == host.snow, "pack surfaces");
    CHECK(client.locked == 1 && client.preset == 6 && client.next_change_tick == 123456, "pack flags");
    // Late join: the client evaluates the same parameters and the same lightning at any tick.
    bool same = true;
    for (u32 t = 4000; t < 20000; t += 37) {
        WeatherParams a, b;
        weather_eval(&host, t, DT, &a);
        weather_eval(&client, t, DT, &b);
        if (memcmp(&a, &b, sizeof(a))) same = false;
    }
    CHECK(same, "late joiner evaluates identically");
    WeatherParams st = *P(&l, "storm");
    weather_state_init(&host, &st, 5);
    weather_pack(&host, buf);
    weather_unpack(&client, buf);
    int diff = 0;
    for (u32 t = 0; t < 60 * 600; t++) {
        bool s1, s2;
        float f1 = weather_lightning(&host, &st, t, DT, &s1, NULL), f2 = weather_lightning(&client, &st, t, DT, &s2, NULL);
        if (f1 != f2 || s1 != s2) diff++;
    }
    CHECK(diff == 0, "lightning identical on host / client (%d diffs)", diff);
}

static void test_rates(void) {
    WeatherLib l;
    weather_lib_defaults(&l);
    WeatherState s;
    weather_state_init(&s, P(&l, "rain"), 1);
    float t_wet = -1, t_pud = -1;
    for (int i = 0; i < 600 * 60; i++) {
        weather_integrate(&s, P(&l, "rain"), DT);
        float t = (float)i * DT;
        if (t_wet < 0 && s.wetness >= 0.99f) t_wet = t;
        if (t_pud < 0 && s.puddles >= 0.99f) t_pud = t;
    }
    CHECK(t_wet > 15 && t_wet < 60, "soaked in %.0f s", t_wet);
    CHECK(t_pud > t_wet && t_pud < 400, "puddles full in %.0f s", t_pud);
    // Light rain fills puddles slower than rain.
    WeatherState s2;
    weather_state_init(&s2, P(&l, "light_rain"), 1);
    for (int i = 0; i < 120 * 60; i++) weather_integrate(&s2, P(&l, "light_rain"), DT);
    WeatherState s3;
    weather_state_init(&s3, P(&l, "rain"), 1);
    for (int i = 0; i < 120 * 60; i++) weather_integrate(&s3, P(&l, "rain"), DT);
    CHECK(s2.puddles < s3.puddles, "light rain fills slower (%.2f < %.2f)", s2.puddles, s3.puddles);
    // Drying: surfaces dry before the puddles drain.
    float t_dry = -1, t_drain = -1;
    for (int i = 0; i < 3600 * 60; i++) {
        weather_integrate(&s, P(&l, "clear"), DT);
        float t = (float)i * DT;
        if (t_dry < 0 && s.wetness <= 0.01f) t_dry = t;
        if (t_drain < 0 && s.puddles <= 0.01f) t_drain = t;
    }
    CHECK(t_dry > 60 && t_dry < t_drain, "dry in %.0f s, drained in %.0f s", t_dry, t_drain);
    // Snow settles below 0 C and melts above.
    weather_state_init(&s, P(&l, "snow"), 1);
    for (int i = 0; i < 300 * 60; i++) weather_integrate(&s, P(&l, "snow"), DT);
    CHECK(s.snow > 0.8f && s.wetness < 0.05f, "snow cover %.2f (wet %.2f)", s.snow, s.wetness);
    float before = s.snow;
    for (int i = 0; i < 300 * 60; i++) weather_integrate(&s, P(&l, "clear"), DT);
    CHECK(s.snow < before * 0.5f, "melts above 0 C: %.2f", s.snow);
    // Ranges always clamped.
    for (int i = 0; i < 60; i++) weather_integrate(&s, P(&l, "storm"), 100.0f);
    CHECK(s.wetness >= 0 && s.wetness <= 1 && s.puddles >= 0 && s.puddles <= 1 && s.snow >= 0 && s.snow <= 1, "clamped");
}

static void test_material(void) {
    vec3 out;
    float r;
    // Dry is unchanged.
    weather_wet_material(v3(0.5f, 0.4f, 0.3f), 0.8f, 0.0f, 0.8f, 0.0f, 0.0f, &out, &r);
    CHECK(fabsf(out.x - 0.5f) < 1e-5f && fabsf(r - 0.8f) < 1e-5f, "dry unchanged");
    // Fully wet porous: darker (30-60 %), glossier.
    weather_wet_material(v3(0.5f, 0.4f, 0.3f), 0.8f, 0.0f, 0.8f, 1.0f, 0.0f, &out, &r);
    CHECK(out.x / 0.5f > 0.4f && out.x / 0.5f < 0.7f, "porous darkening %.2f", out.x / 0.5f);
    CHECK(r < 0.4f && r > 0.1f, "wet roughness %.2f", r);
    // Metal: almost no colour change.
    weather_wet_material(v3(0.9f, 0.9f, 0.9f), 0.4f, 1.0f, 0.8f, 1.0f, 0.0f, &out, &r);
    CHECK(out.x > 0.8f, "metal keeps colour %.2f", out.x);
    // Snow: bright and matte.
    weather_wet_material(v3(0.1f, 0.1f, 0.1f), 0.2f, 0.0f, 0.5f, 0.0f, 1.0f, &out, &r);
    CHECK(out.x > 0.8f && r > 0.55f, "snow %.2f %.2f", out.x, r);
}

static void test_lightning_scheduler(void) {
    WeatherLib l;
    weather_lib_defaults(&l);
    WeatherState s;
    weather_state_init(&s, P(&l, "storm"), 1234);
    int strikes = 0;
    float max_flash = 0;
    for (u32 t = 0; t < 600 * 60; t++) {
        bool st;
        float d = 0;
        float f = weather_lightning(&s, P(&l, "storm"), t, DT, &st, &d);
        if (st) { strikes++; CHECK(d >= 400 && d <= 4000, "strike distance %.0f", d); }
        max_flash = fmaxf(max_flash, f);
    }
    float per_min = (float)strikes / 10.0f;
    CHECK(per_min > 2.0f && per_min < 6.5f, "storm strikes %.1f / min (rate 4)", per_min);
    CHECK(max_flash > 0.3f && max_flash <= 1.0f, "flash %.2f", max_flash);
    int none = 0;
    for (u32 t = 0; t < 600 * 60; t += 7) { bool st; weather_lightning(&s, P(&l, "rain"), t, DT, &st, NULL); none += st; }
    CHECK(none == 0, "no lightning without a rate");
    // Scheduler: deterministic for a seed, honours the lock, picks valid presets, stays a while.
    WeatherState a, b;
    weather_state_init(&a, P(&l, "clear"), 77);
    a.random = 1;
    b = a;
    int changes = 0, min_gap = 1 << 30;
    u32 last = 0;
    for (u32 t = 1; t < 3 * 3600 * 60; t += 60) {
        bool ca = weather_schedule(&a, &l, t, DT), cb = weather_schedule(&b, &l, t, DT);
        CHECK(ca == cb, "scheduler deterministic");
        if (ca) {
            CHECK(a.preset < l.count, "valid preset");
            if (changes) min_gap = DG_MIN(min_gap, (int)(t - last));
            last = t;
            changes++;
        }
    }
    CHECK(!memcmp(&a, &b, sizeof(a)), "same state");
    CHECK(changes >= 8 && changes < 200, "%d changes in 3 h", changes);
    CHECK(min_gap >= 30 * 60, "min stay %d ticks", min_gap);
    a.locked = 1;
    int locked_changes = 0;
    for (u32 t = 3 * 3600 * 60; t < 4 * 3600 * 60; t += 60) locked_changes += weather_schedule(&a, &l, t, DT);
    CHECK(locked_changes == 0, "lock honoured");
}

static void test_lens(void) {
    WeatherLens l;
    weather_lens_init(&l, 3);
    WeatherLensInput in;
    memset(&in, 0, sizeof(in));
    in.rain = 1.0f;
    in.exposed = 1.0f;
    in.look_up = 0.5f;
    in.temperature = 10.0f;
    int max_n = 0;
    bool finite = true, running = false;
    for (int f = 0; f < 300; f++) {
        weather_lens_step(&l, &in, DT);
        max_n = DG_MAX(max_n, l.count);
        for (int i = 0; i < l.count; i++) {
            const WeatherDrop *d = &l.d[i];
            if (!isfinite(d->x) || !isfinite(d->y) || !isfinite(d->r) || d->r <= 0 || d->alpha <= 0) finite = false;
            if (d->vy > 0.02f) running = true;
        }
    }
    CHECK(finite, "drops finite / positive");
    CHECK(max_n > 20 && max_n <= WEATHER_LENS_MAX, "drop count %d", max_n);
    CHECK(running, "heavy drops run down");
    // Under a roof: no new drops, the old ones evaporate.
    in.exposed = 0.0f;
    for (int f = 0; f < 300; f++) weather_lens_step(&l, &in, DT);
    CHECK(l.count < max_n / 3, "evaporated under a roof: %d", l.count);
    // Wipe.
    in.wiped = true;
    weather_lens_step(&l, &in, DT);
    CHECK(l.count == 0, "wiped");
    // Determinism.
    WeatherLens a, b;
    weather_lens_init(&a, 9);
    weather_lens_init(&b, 9);
    in.wiped = false;
    in.exposed = 1.0f;
    for (int f = 0; f < 300; f++) { weather_lens_step(&a, &in, DT); weather_lens_step(&b, &in, DT); }
    CHECK(a.count == b.count && !memcmp(a.d, b.d, sizeof(WeatherDrop) * (size_t)a.count), "lens sim deterministic");
}

int main(void) {
    test_presets();
    test_blend();
    test_pack();
    test_rates();
    test_material();
    test_lightning_scheduler();
    test_lens();
    printf("test_weather: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
