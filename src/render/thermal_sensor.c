// Thermal scope sensor (thermal_sensor.h, docs/NIGHT_OPTICS.md). Pure.
#include "render/thermal_sensor.h"
#include "world/thermal.h"
#include "cjson/cJSON.h"
#include <math.h>

typedef struct Stop { float p, r, g, b; } Stop;

static void build(ThermalPalette *pal, const char *name, const Stop *s, int n) {
    dg_strcpy(pal->name, sizeof(pal->name), name);
    for (int i = 0; i < THERMAL_PALETTE_N; i++) {
        float x = (float)i / (THERMAL_PALETTE_N - 1);
        int k = 0;
        while (k < n - 2 && x > s[k + 1].p) k++;
        float span = s[k + 1].p - s[k].p;
        float f = span > 0.0f ? DG_CLAMP((x - s[k].p) / span, 0.0f, 1.0f) : 0.0f;
        if (x <= s[0].p) f = 0.0f, k = 0;
        if (x >= s[n - 1].p) k = n - 2, f = 1.0f;
        pal->rgb[i][0] = (u8)lroundf(s[k].r + (s[k + 1].r - s[k].r) * f);
        pal->rgb[i][1] = (u8)lroundf(s[k].g + (s[k + 1].g - s[k].g) * f);
        pal->rgb[i][2] = (u8)lroundf(s[k].b + (s[k + 1].b - s[k].b) * f);
    }
}

void thermal_palettes_default(ThermalPalettes *p) {
    memset(p, 0, sizeof(*p));
    static const Stop white[] = { {0, 0, 0, 0}, {1, 255, 255, 255} };
    static const Stop black[] = { {0, 255, 255, 255}, {1, 0, 0, 0} };
    // Red Hot: White Hot with the hottest 8 % of the range in red / yellow
    static const Stop red[] = { {0, 0, 0, 0}, {0.92f, 225, 225, 225}, {0.921f, 200, 20, 10}, {0.97f, 255, 70, 0}, {1, 255, 230, 40} };
    static const Stop iron[] = { {0, 0, 0, 10}, {0.15f, 32, 0, 90}, {0.35f, 140, 0, 150}, {0.55f, 220, 40, 60},
                                 {0.75f, 250, 140, 0}, {0.9f, 255, 215, 40}, {1, 255, 255, 230} };
    static const Stop rainbow[] = { {0, 0, 0, 0}, {0.12f, 30, 0, 120}, {0.28f, 0, 60, 255}, {0.45f, 0, 220, 220},
                                    {0.6f, 40, 220, 0}, {0.75f, 255, 240, 0}, {0.88f, 255, 80, 0}, {1, 255, 255, 255} };
    build(&p->pal[TPAL_WHITE_HOT], "White Hot", white, ARRAY_COUNT(white));
    build(&p->pal[TPAL_BLACK_HOT], "Black Hot", black, ARRAY_COUNT(black));
    build(&p->pal[TPAL_RED_HOT], "Red Hot", red, ARRAY_COUNT(red));
    build(&p->pal[TPAL_IRONBOW], "Ironbow", iron, ARRAY_COUNT(iron));
    build(&p->pal[TPAL_RAINBOW], "Rainbow", rainbow, ARRAY_COUNT(rainbow));
    p->count = 5;
}

int thermal_palette_find(const ThermalPalettes *p, const char *name) {
    for (int i = 0; i < p->count; i++)
        if (name && !strcmp(p->pal[i].name, name)) return i;
    return -1;
}

bool thermal_palettes_load(ThermalPalettes *p, const char *json_text) {
    if (!json_text) return true;
    cJSON *root = cJSON_Parse(json_text);
    if (!root) return false;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "palettes")) {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
        const cJSON *stops = cJSON_GetObjectItemCaseSensitive(e, "stops");
        if (!cJSON_IsString(nm) || !cJSON_IsArray(stops)) continue;
        Stop s[16];
        int n = 0;
        const cJSON *st;
        cJSON_ArrayForEach(st, stops) {
            if (n >= 16 || cJSON_GetArraySize(st) < 4) continue;
            s[n].p = (float)cJSON_GetArrayItem(st, 0)->valuedouble;
            s[n].r = (float)cJSON_GetArrayItem(st, 1)->valuedouble;
            s[n].g = (float)cJSON_GetArrayItem(st, 2)->valuedouble;
            s[n].b = (float)cJSON_GetArrayItem(st, 3)->valuedouble;
            n++;
        }
        if (n < 2) continue;
        int i = thermal_palette_find(p, nm->valuestring);
        if (i < 0) {
            if (p->count >= THERMAL_PALETTES) continue;
            i = p->count++;
        }
        build(&p->pal[i], nm->valuestring, s, n);
    }
    cJSON_Delete(root);
    return true;
}

ThermalCore thermal_core(int quality) {
    return quality >= 1 ? (ThermalCore){ 640, 512, 12.0f } : (ThermalCore){ 384, 288, 17.0f };
}

int thermal_core_square(const ThermalCore *c) { return DG_MIN(c->w, c->h); }

int thermal_zoom_crop(const ThermalCore *c, int zoom) {
    zoom = DG_CLAMP(zoom, 1, THERMAL_ZOOM_MAX);
    return DG_MAX(thermal_core_square(c) / zoom, 8);
}

// ---------------------------------------------------------------- AGC

static float follow(float v, float target, float dt) {
    float tau = AGC_TAU_DOWN;
    return target + (v - target) * expf(-dt / tau);
}

void thermal_agc_step(ThermalAgc *a, float frame_lo, float frame_hi, float dt) {
    if (a->fixed) return;
    if (frame_hi < frame_lo) { float t = frame_hi; frame_hi = frame_lo; frame_lo = t; }
    if (!a->valid) {
        a->lo = frame_lo; a->hi = frame_hi; a->valid = true;
    } else {
        // the range widens fast (attack) and narrows slowly (release)
        a->hi = frame_hi > a->hi ? frame_hi + (a->hi - frame_hi) * expf(-dt / AGC_TAU_UP) : follow(a->hi, frame_hi, dt);
        a->lo = frame_lo < a->lo ? frame_lo + (a->lo - frame_lo) * expf(-dt / AGC_TAU_UP) : follow(a->lo, frame_lo, dt);
    }
    // the minimum span in radiance around the middle (the noise floor: no stretching of nothing)
    float mid = 0.5f * (a->lo + a->hi);
    float tmid = thermal_temp_of_radiance(mid);
    float span = thermal_radiance(tmid + AGC_MIN_SPAN_C * 0.5f) - thermal_radiance(tmid - AGC_MIN_SPAN_C * 0.5f);
    if (a->hi - a->lo < span) { a->lo = mid - span * 0.5f; a->hi = mid + span * 0.5f; }
}

float thermal_agc_map(const ThermalAgc *a, float l) {
    float span = a->hi - a->lo;
    if (span <= 0.0f) return 0.5f;
    return DG_CLAMP((l - a->lo) / span, 0.0f, 1.0f);
}

// ---------------------------------------------------------------- NUC

void thermal_nuc_reset(ThermalNuc *n, float ambient) {
    memset(n, 0, sizeof(*n));
    n->interval = NUC_FIRST;
    n->ambient_ref = ambient;
    n->freeze = NUC_FREEZE;   // power-on calibration
    n->count = 1;
}

bool thermal_nuc_step(ThermalNuc *n, float dt, float ambient) {
    n->on_time += dt;
    n->since += dt;
    if (n->freeze > 0.0f) n->freeze = DG_MAX(n->freeze - dt, 0.0f);
    bool due = n->since >= n->interval || fabsf(ambient - n->ambient_ref) > 4.0f;
    if (!due || n->freeze > 0.0f) return false;
    n->since = 0.0f;
    n->freeze = NUC_FREEZE;
    n->ambient_ref = ambient;
    n->count++;
    // the core warms up and drifts less: 30, 45, 67, 101, 151, 180 s ...
    n->interval = DG_MIN(n->interval * 1.5f, NUC_MAX);
    return true;
}

float thermal_nuc_drift(const ThermalNuc *n) {
    return DG_CLAMP(n->since / NUC_MAX, 0.0f, 1.0f);
}

float thermal_battery_drain(float charge, float runtime_h, float dt) {
    if (runtime_h <= 0.0f) return charge;
    return DG_MAX(charge - dt / (runtime_h * 3600.0f), 0.0f);
}
