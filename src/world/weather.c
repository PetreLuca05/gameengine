#include "weather.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_names[WP_COUNT] = { "cloud_cover", "cloud_density", "precipitation", "fog_density", "fog_height", "temperature",
                                         "wind_dir", "wind", "gust", "lightning_rate" };
const char *weather_param_name(int p) { return p >= 0 && p < WP_COUNT ? k_names[p] : "?"; }

float weather_precip_type(float t) {
    float x = (2.0f - t) / 3.0f;   // +2 C -> 0, -1 C -> 1
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    return x * x * (3.0f - 2.0f * x);
}

// ------------------------------------------------------------------ presets

static void preset(WeatherLib *l, const char *name, float cc, float cd, float pr, float fog, float fh, float temp, float wind, float gust, float lr, float tr,
                   float min_stay, float max_stay) {
    if (l->count >= WEATHER_MAX_PRESETS) return;
    WeatherPreset *p = &l->presets[l->count++];
    memset(p, 0, sizeof(*p));
    dg_strcpy(p->name, sizeof(p->name), name);
    float v[WP_COUNT] = { cc, cd, pr, fog, fh, temp, 60.0f, wind, gust, lr };
    memcpy(p->p.v, v, sizeof(v));
    p->set_mask = (1u << WP_COUNT) - 1u;
    p->set_mask &= ~(1u << WP_WIND_DIR);   // keep the current direction unless a preset names one
    p->transition = tr;
    p->min_stay = min_stay;
    p->max_stay = max_stay;
}

void weather_lib_defaults(WeatherLib *l) {
    memset(l, 0, sizeof(*l));
    //                       cover dens  prec  fog     fogh  temp  wind gust light trans  stay
    preset(l, "clear",         0.05f, 0.4f, 0.0f, 0.0015f, 30, 18, 2.0f, 0.1f, 0, 240, 240, 600);
    preset(l, "little_cloudy", 0.3f, 0.5f, 0.0f, 0.002f, 30, 16, 3.0f, 0.2f, 0, 240, 180, 480);
    preset(l, "cloudy",        0.6f, 0.65f, 0.0f, 0.003f, 40, 14, 4.5f, 0.3f, 0, 240, 180, 420);
    preset(l, "overcast",      0.95f, 0.85f, 0.0f, 0.005f, 50, 12, 5.0f, 0.3f, 0, 300, 180, 480);
    preset(l, "fog",           0.7f, 0.6f, 0.0f, 0.045f, 25, 9, 0.8f, 0.05f, 0, 90, 120, 300);
    preset(l, "light_rain",    0.85f, 0.8f, 0.3f, 0.006f, 50, 11, 5.0f, 0.3f, 0, 120, 150, 360);
    preset(l, "rain",          0.95f, 0.9f, 0.7f, 0.009f, 60, 10, 7.0f, 0.45f, 0, 120, 150, 360);
    preset(l, "storm",         1.0f, 1.0f, 1.0f, 0.012f, 70, 9, 13.0f, 0.8f, 4.0f, 90, 90, 240);
    preset(l, "light_snow",    0.85f, 0.75f, 0.3f, 0.008f, 50, -3, 3.0f, 0.2f, 0, 180, 180, 420);
    preset(l, "snow",          0.95f, 0.85f, 0.7f, 0.012f, 60, -6, 4.0f, 0.3f, 0, 180, 180, 420);
    preset(l, "blizzard",      1.0f, 0.95f, 1.0f, 0.04f, 80, -12, 16.0f, 0.9f, 0, 120, 90, 240);
}

int weather_find_preset(const WeatherLib *l, const char *name) {
    for (int i = 0; i < l->count; i++) if (!strcmp(l->presets[i].name, name)) return i;
    return -1;
}

bool weather_lib_load(WeatherLib *l, const char *dir) {
    weather_lib_defaults(l);
    for (int i = 0; i < l->count; i++) {
        char rel[256], full[512];
        snprintf(rel, sizeof(rel), "%s/%s.json", dir, l->presets[i].name);
        snprintf(full, sizeof(full), "%s%s", platform_base_dir(), rel);
        size_t size = 0;
        char *text = platform_read_file(full, &size);
        if (!text) continue;
        cJSON *root = cJSON_ParseWithLength(text, size);
        free(text);
        if (!root) { log_warn("weather: %s: JSON error", rel); continue; }
        WeatherPreset *p = &l->presets[i];
        p->set_mask = 0;
        for (int k = 0; k < WP_COUNT; k++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, k_names[k]);
            if (cJSON_IsNumber(v)) { p->p.v[k] = (float)v->valuedouble; p->set_mask |= 1u << k; }
        }
        const cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "transition"))) p->transition = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "min_stay"))) p->min_stay = (float)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(root, "max_stay"))) p->max_stay = (float)v->valuedouble;
        cJSON_Delete(root);
    }
    return true;
}

bool weather_compose(const WeatherLib *l, const char *spec, const WeatherParams *base, WeatherParams *out, float *transition) {
    *out = *base;
    float tr = 0;
    char buf[160];
    dg_strcpy(buf, sizeof(buf), spec);
    bool any = false;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        int i = weather_find_preset(l, tok);
        if (i < 0) return false;
        const WeatherPreset *p = &l->presets[i];
        for (int k = 0; k < WP_COUNT; k++) if (p->set_mask & (1u << k)) out->v[k] = p->p.v[k];
        tr = fmaxf(tr, p->transition);
        any = true;
    }
    if (transition) *transition = tr;
    return any;
}

// ------------------------------------------------------------------ transitions

void weather_state_init(WeatherState *s, const WeatherParams *p, u32 seed) {
    memset(s, 0, sizeof(*s));
    s->from = s->to = *p;
    s->seed = seed ? seed : 0x5EEDu;
    s->preset = 255;
    s->version = 1;
}

float weather_param_duration(int p, float T) {
    if (T <= 0.0f) return 0.0f;
    // scale, min, max (s): clouds over minutes, rain 20-60 s, fog about a minute, temperature slow.
    static const float k[WP_COUNT][3] = {
        { 1.0f, 30.0f, 900.0f },   // cloud cover
        { 1.0f, 30.0f, 900.0f },   // cloud density
        { 0.35f, 20.0f, 60.0f },   // precipitation
        { 0.5f, 45.0f, 90.0f },    // fog
        { 0.5f, 30.0f, 120.0f },   // fog height
        { 1.0f, 30.0f, 1200.0f },  // temperature
        { 0.6f, 10.0f, 300.0f },   // wind dir
        { 0.5f, 10.0f, 180.0f },   // wind
        { 0.5f, 10.0f, 180.0f },   // gust
        { 0.3f, 10.0f, 60.0f },    // lightning
    };
    float d = T * k[p][0];
    if (d < k[p][1]) d = k[p][1];
    if (d > k[p][2]) d = k[p][2];
    return fminf(d, fmaxf(T, k[p][1]));   // never longer than the transition (or the minimum)
}

static float smooth01(float x) {
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    return x * x * (3.0f - 2.0f * x);
}

static float lerp_deg(float a, float b, float t) {
    float d = fmodf(b - a + 540.0f, 360.0f) - 180.0f;
    float r = a + d * t;
    r = fmodf(r, 360.0f);
    return r < 0 ? r + 360.0f : r;
}

void weather_eval(const WeatherState *s, u32 tick, float tick_dt, WeatherParams *out) {
    float t = tick >= s->start_tick ? (float)(tick - s->start_tick) * tick_dt : 0.0f;
    for (int p = 0; p < WP_COUNT; p++) {
        float d = weather_param_duration(p, s->transition);
        float k = d <= 0.0f ? 1.0f : smooth01(t / d);
        out->v[p] = p == WP_WIND_DIR ? lerp_deg(s->from.v[p], s->to.v[p], k) : s->from.v[p] + (s->to.v[p] - s->from.v[p]) * k;
    }
}

void weather_set_target(WeatherState *s, const WeatherParams *target, u32 tick, float transition) {
    WeatherParams cur;
    weather_eval(s, tick, 1.0f / 60.0f, &cur);
    s->from = cur;
    s->to = *target;
    s->start_tick = tick;
    s->transition = fmaxf(0.0f, transition);
    s->version++;
}

// Surfaces: rain wets them in ~40 s, they dry over minutes (faster when warm and sunny); puddles
// fill slowly once surfaces are soaked and drain slower than they dry; snow settles slowly and melts
// above 0 C (rain melts it faster).
void weather_integrate(WeatherState *s, const WeatherParams *c, float dt) {
    float type = weather_precip_type(c->v[WP_TEMPERATURE]);
    float rain = c->v[WP_PRECIP] * (1.0f - type), snow = c->v[WP_PRECIP] * type;
    float temp = c->v[WP_TEMPERATURE];
    if (rain > 0.03f) s->wetness += dt * (0.35f + rain) / 40.0f;
    else s->wetness -= dt * (1.0f + fmaxf(temp, 0.0f) / 20.0f + (1.0f - c->v[WP_CLOUD_COVER])) / 360.0f;
    if (rain > 0.1f && s->wetness > 0.6f) s->puddles += dt * rain / 120.0f;
    else if (rain <= 0.03f) s->puddles -= dt * (1.0f + fmaxf(temp, 0.0f) / 25.0f) / 720.0f;
    if (snow > 0.03f) s->snow += dt * snow / 180.0f;
    if (temp > 0.0f) s->snow -= dt * (temp / 10.0f + rain * 2.0f) / 240.0f;
    s->wetness = fminf(fmaxf(s->wetness, 0.0f), 1.0f);
    s->puddles = fminf(fmaxf(s->puddles, 0.0f), 1.0f);
    s->snow = fminf(fmaxf(s->snow, 0.0f), 1.0f);
}

static u32 hash32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

// Strike slots of 0.5 s: a strike starts in a slot with probability rate / 120. Flash: a bright
// double flicker over ~0.3 s, distance 0.4 .. 4 km (thunder delay = distance / 343 m/s).
float weather_lightning(const WeatherState *s, const WeatherParams *c, u32 tick, float tick_dt, bool *strike_now, float *distance_m) {
    if (strike_now) *strike_now = false;
    float rate = c->v[WP_LIGHTNING];
    if (rate <= 0.0f) return 0.0f;
    const u32 slot_ticks = (u32)(0.5f / tick_dt + 0.5f);
    u32 slot = tick / slot_ticks, in_slot = tick % slot_ticks;
    float flash = 0.0f;
    for (u32 back = 0; back < 2; back++) {
        if (slot < back) break;
        u32 sl = slot - back;
        u32 h = hash32(sl * 2654435761u ^ s->seed);
        if ((float)(h & 0xFFFF) / 65536.0f >= rate / 120.0f) continue;
        u32 off = (h >> 16) % slot_ticks;
        i32 age = (i32)(back * slot_ticks + in_slot) - (i32)off;
        if (age < 0) continue;
        float a = (float)age * tick_dt;
        float f = a < 0.06f ? 1.0f : (a < 0.12f ? 0.25f : (a < 0.2f ? 0.8f : expf(-(a - 0.2f) * 12.0f)));
        float dist = 400.0f + (float)(hash32(h) % 3600u);
        f *= 1.0f / (1.0f + dist / 1500.0f);
        if (age == 0) {
            if (strike_now) *strike_now = true;
            if (distance_m) *distance_m = dist;
        }
        flash = fmaxf(flash, f);
    }
    return fminf(flash, 1.0f);
}

WeatherWind weather_wind(const WeatherParams *c, double time_s) {
    WeatherWind w;
    float a = c->v[WP_WIND_DIR] * DG_DEG2RAD;
    w.dir = v3(sinf(a), 0.0f, -cosf(a));
    float g = c->v[WP_GUST];
    float t = (float)fmod(time_s, 10000.0);
    float gust = 1.0f + g * (0.5f * sinf(t * 0.7f) + 0.3f * sinf(t * 1.9f + 1.3f) + 0.2f * sinf(t * 4.3f + 0.4f));
    w.strength = fmaxf(0.0f, c->v[WP_WIND] * gust);
    w.gust = g;
    return w;
}

// ------------------------------------------------------------------ scheduler

// Markov-style table: from each preset, weights for the next one (clearing / worsening cycles).
static int pick_next(const WeatherLib *l, int cur, u32 r) {
    static const struct { const char *from; const char *to[4]; float w[4]; } k[] = {
        { "clear", { "little_cloudy", "fog", "clear", NULL }, { 0.6f, 0.15f, 0.25f, 0 } },
        { "little_cloudy", { "cloudy", "clear", "little_cloudy", NULL }, { 0.5f, 0.3f, 0.2f, 0 } },
        { "cloudy", { "overcast", "little_cloudy", "light_rain", NULL }, { 0.45f, 0.35f, 0.2f, 0 } },
        { "overcast", { "rain", "light_rain", "cloudy", "light_snow" }, { 0.35f, 0.3f, 0.3f, 0.05f } },
        { "fog", { "little_cloudy", "cloudy", "clear", NULL }, { 0.4f, 0.3f, 0.3f, 0 } },
        { "light_rain", { "rain", "overcast", "cloudy", NULL }, { 0.4f, 0.35f, 0.25f, 0 } },
        { "rain", { "storm", "light_rain", "overcast", NULL }, { 0.25f, 0.45f, 0.3f, 0 } },
        { "storm", { "rain", "overcast", NULL, NULL }, { 0.6f, 0.4f, 0, 0 } },
        { "light_snow", { "snow", "overcast", "cloudy", NULL }, { 0.4f, 0.35f, 0.25f, 0 } },
        { "snow", { "blizzard", "light_snow", "overcast", NULL }, { 0.2f, 0.5f, 0.3f, 0 } },
        { "blizzard", { "snow", NULL, NULL, NULL }, { 1.0f, 0, 0, 0 } },
    };
    const char *name = cur >= 0 && cur < l->count ? l->presets[cur].name : "clear";
    for (int i = 0; i < (int)ARRAY_COUNT(k); i++) {
        if (strcmp(k[i].from, name)) continue;
        float sum = 0;
        for (int j = 0; j < 4; j++) sum += k[i].w[j];
        float x = (float)(r & 0xFFFF) / 65536.0f * sum;
        for (int j = 0; j < 4; j++) {
            if (!k[i].to[j]) break;
            x -= k[i].w[j];
            if (x <= 0) { int n = weather_find_preset(l, k[i].to[j]); return n >= 0 ? n : cur; }
        }
    }
    return weather_find_preset(l, "cloudy");
}

bool weather_schedule(WeatherState *s, const WeatherLib *l, u32 tick, float tick_dt) {
    if (!s->random || s->locked) return false;
    if (s->next_change_tick && tick < s->next_change_tick) return false;
    u32 r = hash32(tick ^ s->seed ^ 0xA341316Cu);
    int next = s->next_change_tick ? pick_next(l, s->preset == 255 ? -1 : s->preset, r) : (s->preset == 255 ? weather_find_preset(l, "clear") : s->preset);
    if (next < 0) next = 0;
    const WeatherPreset *p = &l->presets[next];
    WeatherParams cur, tgt;
    weather_eval(s, tick, tick_dt, &cur);
    tgt = cur;
    for (int k = 0; k < WP_COUNT; k++) if (p->set_mask & (1u << k)) tgt.v[k] = p->p.v[k];
    // The wind veers a little with every change.
    tgt.v[WP_WIND_DIR] = fmodf(cur.v[WP_WIND_DIR] + (float)((int)(hash32(r) % 61u) - 30) + 360.0f, 360.0f);
    weather_set_target(s, &tgt, tick, p->transition);
    s->preset = (u8)next;
    float stay = p->min_stay + (p->max_stay - p->min_stay) * (float)(hash32(r * 7u) & 0xFFFF) / 65536.0f;
    s->next_change_tick = tick + (u32)((p->transition + fmaxf(stay, 30.0f)) / tick_dt);
    return true;
}

// ------------------------------------------------------------------ replication

void weather_pack(const WeatherState *s, u8 out[WEATHER_PACK_SIZE]) {
    memset(out, 0, WEATHER_PACK_SIZE);
    u8 *p = out;
    memcpy(p, s->from.v, sizeof(float) * WP_COUNT); p += sizeof(float) * WP_COUNT;   // 40
    memcpy(p, s->to.v, sizeof(float) * WP_COUNT); p += sizeof(float) * WP_COUNT;     // 80
    memcpy(p, &s->start_tick, 4); p += 4;
    memcpy(p, &s->transition, 4); p += 4;
    memcpy(p, &s->wetness, 4); p += 4;
    memcpy(p, &s->puddles, 4); p += 4;
    memcpy(p, &s->snow, 4); p += 4;
    memcpy(p, &s->seed, 4); p += 4;
    memcpy(p, &s->version, 2); p += 2;
    *p++ = s->preset;
    *p++ = s->locked;
    *p++ = s->random;
    memcpy(p, &s->next_change_tick, 4); p += 4;
}

void weather_unpack(WeatherState *s, const u8 in[WEATHER_PACK_SIZE]) {
    const u8 *p = in;
    memcpy(s->from.v, p, sizeof(float) * WP_COUNT); p += sizeof(float) * WP_COUNT;
    memcpy(s->to.v, p, sizeof(float) * WP_COUNT); p += sizeof(float) * WP_COUNT;
    memcpy(&s->start_tick, p, 4); p += 4;
    memcpy(&s->transition, p, 4); p += 4;
    memcpy(&s->wetness, p, 4); p += 4;
    memcpy(&s->puddles, p, 4); p += 4;
    memcpy(&s->snow, p, 4); p += 4;
    memcpy(&s->seed, p, 4); p += 4;
    memcpy(&s->version, p, 2); p += 2;
    s->preset = *p++;
    s->locked = *p++;
    s->random = *p++;
    memcpy(&s->next_change_tick, p, 4); p += 4;
}

// ------------------------------------------------------------------ surfaces

void weather_wet_material(vec3 albedo, float rough, float metal, float porosity, float wet, float snow, vec3 *oa, float *orough) {
    // Wet: porous materials darken (water fills the pores: albedo^(1 + porosity)), every surface
    // gets glossier (a water film); metals barely change colour.
    float p = porosity * (1.0f - metal);
    float k = 1.0f - wet * (0.55f * p + 0.08f);
    vec3 a = v3(albedo.x * k, albedo.y * k, albedo.z * k);
    float r = rough + (fminf(rough, 0.18f) - rough) * wet * (0.6f + 0.35f * p);
    // Snow (on upward faces, the caller scales snow by the facing): white, matte.
    a = v3_lerp(a, v3(0.86f, 0.89f, 0.93f), snow);
    r = r + (0.62f - r) * snow;
    *oa = a;
    *orough = r;
}

// ------------------------------------------------------------------ lens droplets

static float lens_rand(u32 *s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

void weather_lens_init(WeatherLens *l, u32 seed) {
    memset(l, 0, sizeof(*l));
    l->rng = seed ? seed : 0xC0FFEEu;
}

void weather_lens_step(WeatherLens *l, const WeatherLensInput *in, float dt) {
    if (dt <= 0.0f) return;
    dt = fminf(dt, 0.1f);
    // Spawn: rain hitting the lens, more when looking up or moving forward, none under a roof.
    float rate = in->rain * in->exposed * (0.6f + 2.4f * fmaxf(in->look_up, 0.0f) + 0.15f * fmaxf(in->forward_speed, 0.0f)) * 9.0f;
    if (in->wiped) l->count = 0;
    l->spawn_acc += rate * dt;
    while (l->spawn_acc >= 1.0f) {
        l->spawn_acc -= 1.0f;
        if (l->count >= WEATHER_LENS_MAX) break;
        WeatherDrop *d = &l->d[l->count++];
        memset(d, 0, sizeof(*d));
        d->x = lens_rand(&l->rng);
        d->y = lens_rand(&l->rng);
        d->r = 0.006f + lens_rand(&l->rng) * 0.014f;
        d->alpha = 0.6f + lens_rand(&l->rng) * 0.4f;
        d->stretch = 1.0f;
    }
    // Grow (condensation while wet), run when heavy, evaporate (faster when dry / indoors / warm).
    float evap = (in->rain * in->exposed > 0.05f ? 0.03f : 0.22f) * (1.0f + fmaxf(in->temperature, 0.0f) / 30.0f);
    for (int i = 0; i < l->count; i++) {
        WeatherDrop *d = &l->d[i];
        d->age += dt;
        d->r += dt * 0.0006f * in->rain * in->exposed;
        if (d->r > 0.016f) {
            d->vy += dt * 0.12f * (d->r / 0.016f);
            d->vy = fminf(d->vy, 0.35f);
            d->r -= dt * d->vy * 0.012f;   // leaves a trail behind
            d->stretch = fminf(1.0f + d->vy * 12.0f, 4.0f);
        } else {
            d->vy *= expf(-dt * 4.0f);
            d->stretch = 1.0f + (d->stretch - 1.0f) * expf(-dt * 3.0f);
        }
        d->y += d->vy * dt;   // screen y grows downwards
        d->x += in->side_speed * dt * 0.01f;
        d->alpha -= evap * dt;
    }
    // Merge overlapping drops (the bigger one absorbs the smaller: area preserved).
    for (int i = 0; i < l->count; i++)
        for (int j = i + 1; j < l->count; j++) {
            WeatherDrop *a = &l->d[i], *b = &l->d[j];
            float dx = a->x - b->x, dy = a->y - b->y, rr = (a->r + b->r) * 0.6f;
            if (dx * dx + dy * dy >= rr * rr) continue;
            if (b->r > a->r) { WeatherDrop t = *a; *a = *b; *b = t; }
            a->r = sqrtf(a->r * a->r + b->r * b->r);
            a->alpha = fmaxf(a->alpha, b->alpha);
            b->alpha = 0.0f;
        }
    // Remove the dead ones (evaporated, merged, off screen, too small).
    int n = 0;
    for (int i = 0; i < l->count; i++) {
        WeatherDrop *d = &l->d[i];
        if (d->alpha <= 0.0f || d->y > 1.05f || d->x < -0.05f || d->x > 1.05f || d->r < 0.002f || !isfinite(d->x) || !isfinite(d->y)) continue;
        d->r = fminf(d->r, 0.04f);
        l->d[n++] = *d;
    }
    l->count = n;
}
