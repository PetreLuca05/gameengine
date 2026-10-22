// Temperature state layer (thermal.h, docs/NIGHT_OPTICS.md). Pure.
#include "world/thermal.h"
#include "cjson/cJSON.h"
#include <math.h>
#include <stdio.h>

static const char *k_class_names[THC_COUNT] = {
    "concrete", "brick", "soil", "grass", "foliage", "asphalt", "wood", "metal_painted", "metal_bare",
    "glass", "water", "cloth", "skin", "snow", "plastic", "hot",
};

static const char *k_role_names[THR_COUNT] = {
    "skin", "face", "hands", "torso", "legs", "feet", "headwear", "armor", "backpack", "hair",
};

static const char *k_part_names[THV_COUNT] = { "engine", "exhaust", "brake", "tire" };

const char *thermal_class_name(int cls) { return cls >= 0 && cls < THC_COUNT ? k_class_names[cls] : "?"; }

int thermal_class_by_name(const char *class_name) {
    for (int i = 0; i < THC_COUNT; i++)
        if (class_name && !strcmp(class_name, k_class_names[i])) return i;
    return -1;
}

static void set_class(ThermalData *d, int c, float e, float tau_h, float solar, float direct, float night, float sky,
                      float wet, float ir) {
    ThermalClassDef *k = &d->cls[c];
    dg_strcpy(k->name, sizeof(k->name), k_class_names[c]);
    k->emissivity = e; k->tau_h = tau_h; k->solar_gain = solar; k->direct_gain = direct;
    k->night_loss = night; k->sky_facing_loss = sky; k->wet_cool = wet; k->ir_reflectance = ir;
}

static void add_map(ThermalData *d, const char *name, int cls) {
    for (int i = 0; i < d->map_count; i++)
        if (!strcmp(d->map[i].name, name)) { d->map[i].cls = (u8)cls; return; }
    if (d->map_count >= ARRAY_COUNT(d->map)) return;
    dg_strcpy(d->map[d->map_count].name, sizeof(d->map[0].name), name);
    d->map[d->map_count++].cls = (u8)cls;
}

void thermal_defaults(ThermalData *d) {
    memset(d, 0, sizeof(*d));
    //                                 e     tau  solar direct night sky  wet  ir
    set_class(d, THC_CONCRETE,       0.92f, 5.0f,  9.0f, 6.0f, 2.0f, 2.0f, 3.0f, 0.40f);
    set_class(d, THC_BRICK,          0.93f, 4.0f,  9.0f, 6.0f, 2.0f, 2.0f, 3.0f, 0.35f);
    set_class(d, THC_SOIL,           0.93f, 2.5f,  8.0f, 5.0f, 4.0f, 2.0f, 4.0f, 0.30f);
    set_class(d, THC_GRASS,          0.98f, 0.5f,  3.0f, 2.0f, 5.0f, 1.5f, 2.0f, 0.50f);
    set_class(d, THC_FOLIAGE,        0.97f, 0.2f,  1.5f, 1.5f, 3.0f, 1.0f, 1.5f, 0.55f);
    set_class(d, THC_ASPHALT,        0.95f, 3.0f, 14.0f, 8.0f, 3.0f, 2.0f, 3.0f, 0.10f);
    set_class(d, THC_WOOD,           0.90f, 1.5f,  6.0f, 5.0f, 3.0f, 2.0f, 3.0f, 0.45f);
    set_class(d, THC_METAL_PAINTED,  0.90f, 0.4f, 12.0f,10.0f, 5.0f, 3.0f, 2.0f, -1.0f);
    set_class(d, THC_METAL_BARE,     0.12f, 0.3f, 10.0f, 8.0f, 5.0f, 3.0f, 1.0f, -1.0f);
    set_class(d, THC_GLASS,          0.88f, 1.0f,  4.0f, 3.0f, 4.0f, 2.0f, 2.0f, 0.08f);
    set_class(d, THC_WATER,          0.96f,24.0f,  1.0f, 0.5f, 1.0f, 0.5f, 0.0f, 0.02f);
    set_class(d, THC_CLOTH,          0.90f, 0.3f,  5.0f, 4.0f, 3.0f, 1.0f, 3.0f, 0.60f);
    set_class(d, THC_SKIN,           0.98f, 0.1f,  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.50f);
    set_class(d, THC_SNOW,           0.98f, 2.0f,  0.0f, 0.0f, 6.0f, 2.0f, 0.0f, 0.70f);
    set_class(d, THC_PLASTIC,        0.92f, 0.5f,  8.0f, 6.0f, 4.0f, 2.0f, 2.0f, 0.40f);
    set_class(d, THC_HOT,            0.95f, 0.01f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.60f);
    // SurfaceMaterial names (vfx_types.h) + the render / item material words
    static const struct { const char *n; int c; } maps[] = {
        {"default", THC_CONCRETE}, {"concrete", THC_CONCRETE}, {"brick", THC_BRICK}, {"plaster", THC_CONCRETE},
        {"metal_thin", THC_METAL_PAINTED}, {"metal_thick", THC_METAL_PAINTED}, {"metal", THC_METAL_PAINTED},
        {"metal_bare", THC_METAL_BARE}, {"steel", THC_METAL_BARE}, {"aluminium", THC_METAL_BARE}, {"chrome", THC_METAL_BARE},
        {"wood", THC_WOOD}, {"dirt", THC_SOIL}, {"mud", THC_SOIL}, {"sand", THC_SOIL}, {"gravel", THC_SOIL},
        {"grass", THC_GRASS}, {"glass", THC_GLASS}, {"water", THC_WATER}, {"flesh", THC_SKIN}, {"skin", THC_SKIN},
        {"cloth", THC_CLOTH}, {"fabric", THC_CLOTH}, {"tile", THC_CONCRETE}, {"asphalt", THC_ASPHALT},
        {"snow", THC_SNOW}, {"foliage", THC_FOLIAGE}, {"leaves", THC_FOLIAGE}, {"roof_metal", THC_METAL_PAINTED},
        {"plastic", THC_PLASTIC}, {"polymer", THC_PLASTIC}, {"rubber", THC_PLASTIC},
    };
    for (int i = 0; i < ARRAY_COUNT(maps); i++) add_map(d, maps[i].n, maps[i].c);

    d->human = (ThermalBody){ .core = 37.0f, .skin = 34.0f, .face = 1.5f, .hands = -1.0f, .cold_k = 0.12f, .wind_k = 0.15f };
    d->zombie = (ThermalBody){ .core = 24.0f, .skin = 22.0f, .face = 0.5f, .hands = -0.5f, .cold_k = 0.10f, .wind_k = 0.12f };
    static const float clo[THR_COUNT] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.8f, 1.5f, 2.0f, 3.0f, 4.0f, 0.6f };
    memcpy(d->clo, clo, sizeof(clo));
    d->clo_k = 0.9f;
    d->corpse_tau = 600.0f;
    d->blood_t0 = 34.0f; d->blood_tau = 90.0f;
    d->veh[THV_ENGINE]  = (ThermalPartDef){ .rise_idle = 55.0f, .rise_load = 30.0f, .heat_tau = 120.0f, .cool_tau = 900.0f };
    d->veh[THV_EXHAUST] = (ThermalPartDef){ .rise_idle = 120.0f, .rise_load = 200.0f, .heat_tau = 40.0f, .cool_tau = 300.0f };
    d->veh[THV_BRAKE]   = (ThermalPartDef){ .rise_idle = 0.0f, .rise_load = 0.0f, .heat_tau = 1.0f, .cool_tau = 60.0f };
    d->veh[THV_TIRE]    = (ThermalPartDef){ .rise_idle = 0.0f, .rise_load = 0.0f, .heat_tau = 300.0f, .cool_tau = 400.0f };
    d->brake_heat = 0.6f;
    d->tire_speed_k = 0.6f;
    d->barrel_k = 2.5f; d->barrel_max = 450.0f;
    d->bulb = 120.0f; d->led = 45.0f; d->fire = 700.0f; d->muzzle = 900.0f; d->explosion = 1200.0f;
    d->breath = 30.0f; d->exhaust_plume = 120.0f; d->footprint = 2.0f; d->footprint_tau = 60.0f;
    d->heat_decal_tau = 1200.0f;
    d->sky_clear_depression = 45.0f; d->sky_cloud_keep = 0.15f; d->sky_exponent = 0.35f;
}

static void num(const cJSON *o, const char *key, float *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) *dst = (float)v->valuedouble;
}

bool thermal_load_materials(ThermalData *d, const char *json_text) {
    if (!json_text) return true;
    cJSON *root = cJSON_Parse(json_text);
    if (!root) return false;
    const cJSON *classes = cJSON_GetObjectItemCaseSensitive(root, "classes");
    const cJSON *c;
    cJSON_ArrayForEach(c, classes) {
        int i = thermal_class_by_name(c->string);
        if (i < 0) continue;
        ThermalClassDef *k = &d->cls[i];
        num(c, "emissivity", &k->emissivity); num(c, "tau_h", &k->tau_h);
        num(c, "solar_gain", &k->solar_gain); num(c, "direct_gain", &k->direct_gain);
        num(c, "night_loss", &k->night_loss); num(c, "sky_facing_loss", &k->sky_facing_loss);
        num(c, "wet_cool", &k->wet_cool); num(c, "ir_reflectance", &k->ir_reflectance);
        k->emissivity = DG_CLAMP(k->emissivity, 0.01f, 1.0f);
        k->tau_h = DG_MAX(k->tau_h, 0.001f);
    }
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(root, "surfaces");
    cJSON_ArrayForEach(c, map) {
        int i = cJSON_IsString(c) ? thermal_class_by_name(c->valuestring) : -1;
        if (i >= 0) add_map(d, c->string, i);
    }
    cJSON_Delete(root);
    return true;
}

static void load_body(const cJSON *o, ThermalBody *b) {
    if (!o) return;
    num(o, "core", &b->core); num(o, "skin", &b->skin); num(o, "face", &b->face); num(o, "hands", &b->hands);
    num(o, "cold_k", &b->cold_k); num(o, "wind_k", &b->wind_k);
}

bool thermal_load_entities(ThermalData *d, const char *json_text) {
    if (!json_text) return true;
    cJSON *root = cJSON_Parse(json_text);
    if (!root) return false;
    load_body(cJSON_GetObjectItemCaseSensitive(root, "human"), &d->human);
    load_body(cJSON_GetObjectItemCaseSensitive(root, "zombie"), &d->zombie);
    const cJSON *clo = cJSON_GetObjectItemCaseSensitive(root, "clothing");
    if (clo) {
        for (int i = 0; i < THR_COUNT; i++) num(clo, k_role_names[i], &d->clo[i]);
        num(clo, "k", &d->clo_k);
    }
    const cJSON *corpse = cJSON_GetObjectItemCaseSensitive(root, "corpse");
    if (corpse) num(corpse, "tau", &d->corpse_tau);
    const cJSON *blood = cJSON_GetObjectItemCaseSensitive(root, "blood");
    if (blood) { num(blood, "t0", &d->blood_t0); num(blood, "tau", &d->blood_tau); }
    const cJSON *veh = cJSON_GetObjectItemCaseSensitive(root, "vehicle");
    if (veh) {
        for (int i = 0; i < THV_COUNT; i++) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(veh, k_part_names[i]);
            if (!p) continue;
            num(p, "rise_idle", &d->veh[i].rise_idle); num(p, "rise_load", &d->veh[i].rise_load);
            num(p, "heat_tau", &d->veh[i].heat_tau); num(p, "cool_tau", &d->veh[i].cool_tau);
        }
        num(veh, "brake_heat", &d->brake_heat); num(veh, "tire_speed_k", &d->tire_speed_k);
    }
    const cJSON *bar = cJSON_GetObjectItemCaseSensitive(root, "barrel");
    if (bar) { num(bar, "k", &d->barrel_k); num(bar, "max", &d->barrel_max); }
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "sources");
    if (src) {
        num(src, "bulb", &d->bulb); num(src, "led", &d->led); num(src, "fire", &d->fire);
        num(src, "muzzle", &d->muzzle); num(src, "explosion", &d->explosion); num(src, "breath", &d->breath);
        num(src, "exhaust_plume", &d->exhaust_plume); num(src, "footprint", &d->footprint);
        num(src, "footprint_tau", &d->footprint_tau); num(src, "heat_decal_tau", &d->heat_decal_tau);
    }
    const cJSON *sky = cJSON_GetObjectItemCaseSensitive(root, "sky");
    if (sky) {
        num(sky, "clear_depression", &d->sky_clear_depression); num(sky, "cloud_keep", &d->sky_cloud_keep);
        num(sky, "exponent", &d->sky_exponent);
    }
    cJSON_Delete(root);
    return true;
}

int thermal_class_of(const ThermalData *d, const char *name) {
    if (!name) return THC_CONCRETE;
    for (int i = 0; i < d->map_count; i++)
        if (!strcmp(d->map[i].name, name)) return d->map[i].cls;
    int c = thermal_class_by_name(name);
    return c >= 0 ? c : THC_CONCRETE;
}

// ---------------------------------------------------------------- surfaces

float thermal_sun_input(float sun_elev_deg) {
    float s = sinf(sun_elev_deg * 0.017453293f);
    return s > 0.0f ? powf(s, 0.8f) : 0.0f;
}

// The radiative night loss fades in as the sun input drops (full below ~4 deg of elevation).
static float night_weight(float sun) { return 1.0f - DG_MIN(sun * 4.0f, 1.0f); }

float thermal_class_eq(const ThermalData *d, int cls, const ThermalEnv *env) {
    const ThermalClassDef *k = &d->cls[cls];
    float clear = 1.0f - DG_CLAMP(env->cloud, 0.0f, 1.0f);
    float t = env->air + k->solar_gain * env->sun * (1.0f - 0.75f * env->cloud)
            - k->night_loss * (0.2f + 0.8f * clear) * night_weight(env->sun)
            - k->wet_cool * DG_CLAMP(env->wetness, 0.0f, 1.0f);
    if (cls == THC_SNOW) t = DG_MIN(t, 0.0f);
    return t;
}

float thermal_class_step(const ThermalData *d, int cls, float t, const ThermalEnv *env, float dt) {
    float eq = thermal_class_eq(d, cls, env);
    float tau = d->cls[cls].tau_h * 3600.0f;
    return eq + (t - eq) * expf(-dt / tau);
}

void thermal_surfaces_step(const ThermalData *d, ThermalSurfaces *s, const ThermalEnv *env, float dt) {
    if (!s->valid) {
        for (int c = 0; c < THC_COUNT; c++) { s->t[c] = thermal_class_eq(d, c, env); s->solar[c] = env->sun; }
        s->valid = true;
        return;
    }
    for (int c = 0; c < THC_COUNT; c++) {
        s->t[c] = thermal_class_step(d, c, s->t[c], env, dt);
        float a = expf(-dt / (d->cls[c].tau_h * 3600.0f));
        s->solar[c] = env->sun + (s->solar[c] - env->sun) * a;
    }
}

void thermal_surfaces_settle(const ThermalData *d, ThermalSurfaces *s, double hours, ThermalEnvFn fn, void *user) {
    const double step_h = 1.0 / 6.0;
    ThermalEnv env;
    s->valid = false;
    for (double h = hours - 48.0; h < hours + 1e-6; h += step_h) {
        fn(h, &env, user);
        thermal_surfaces_step(d, s, &env, (float)(step_h * 3600.0));
    }
}

float thermal_surface_temp(const ThermalData *d, const ThermalSurfaces *s, const ThermalEnv *env, int cls,
                           float n_dot_l, float sky_view) {
    const ThermalClassDef *k = &d->cls[cls];
    float clear = 1.0f - DG_CLAMP(env->cloud, 0.0f, 1.0f);
    float t = s->valid ? s->t[cls] : thermal_class_eq(d, cls, env);
    float solar = s->valid ? s->solar[cls] : env->sun;
    t += k->direct_gain * solar * (1.0f - 0.75f * env->cloud) * DG_CLAMP(n_dot_l, 0.0f, 1.0f);
    t -= k->sky_facing_loss * clear * night_weight(env->sun) * DG_CLAMP(sky_view, 0.0f, 1.0f);
    return t;
}

// ---------------------------------------------------------------- entities

float thermal_skin(const ThermalBody *b, float air, float wind) {
    float s = b->skin - b->cold_k * DG_MAX(0.0f, 20.0f - air) - b->wind_k * DG_MAX(0.0f, wind);
    return DG_MAX(s, DG_MIN(air, b->skin));   // never below the air when the air is colder than skin
}

float thermal_role_temp(const ThermalData *d, const ThermalBody *b, int role, float air, float wind) {
    float skin = thermal_skin(b, air, wind);
    if (role == THR_SKIN) return skin;
    if (role == THR_FACE) return skin + b->face;
    if (role == THR_HANDS) return skin + b->hands;
    float clo = d->clo[DG_CLAMP(role, 0, THR_COUNT - 1)];
    return air + (skin - air) / (1.0f + clo * d->clo_k);
}

float thermal_cooling(float t0, float air, float t, float tau) {
    if (t <= 0.0f) return t0;
    return air + (t0 - air) * expf(-t / DG_MAX(tau, 1e-3f));
}

void thermal_vehicle_init(ThermalVehicle *v, float air, float engine_c) {
    memset(v, 0, sizeof(*v));
    for (int i = 0; i < THV_COUNT; i++) v->part[i] = air;
    for (int i = 0; i < 4; i++) v->brake_w[i] = v->tire_w[i] = air;
    if (engine_c > air) {   // a parked warm car (level data): the exhaust a little warmer than the air
        v->part[THV_ENGINE] = engine_c;
        v->part[THV_EXHAUST] = air + (engine_c - air) * 0.8f;
    }
    v->valid = true;
}

static float approach(float t, float target, float heat_tau, float cool_tau, float dt) {
    float tau = target > t ? heat_tau : cool_tau;
    return target + (t - target) * expf(-dt / DG_MAX(tau, 1e-3f));
}

void thermal_vehicle_step(const ThermalData *d, ThermalVehicle *v, const ThermalVehInput *in, float air, float dt) {
    if (!v->valid) thermal_vehicle_init(v, air, air);
    float load = DG_CLAMP(in->load, 0.0f, 1.0f);
    for (int p = THV_ENGINE; p <= THV_EXHAUST; p++) {
        const ThermalPartDef *k = &d->veh[p];
        float target = in->engine ? air + k->rise_idle + k->rise_load * load : air;
        v->part[p] = approach(v->part[p], target, k->heat_tau, k->cool_tau, dt);
    }
    // brakes: heat input while braking (power ~ speed), Newtonian cooling to the air
    const ThermalPartDef *bk = &d->veh[THV_BRAKE];
    const ThermalPartDef *tk = &d->veh[THV_TIRE];
    float speed = fabsf(in->speed);
    float bmax = air, tmax = air;
    for (int w = 0; w < 4; w++) {
        if (in->braking) v->brake_w[w] += d->brake_heat * speed * dt;
        v->brake_w[w] = air + (v->brake_w[w] - air) * expf(-dt / DG_MAX(bk->cool_tau, 1e-3f));
        v->tire_w[w] = approach(v->tire_w[w], air + d->tire_speed_k * speed, tk->heat_tau, tk->cool_tau, dt);
        bmax = DG_MAX(bmax, v->brake_w[w]);
        tmax = DG_MAX(tmax, v->tire_w[w]);
    }
    v->part[THV_BRAKE] = bmax;
    v->part[THV_TIRE] = tmax;
}

float thermal_barrel_temp(const ThermalData *d, float heat_g, float air) {
    return DG_MIN(air + d->barrel_k * DG_MAX(heat_g, 0.0f), d->barrel_max);
}

float thermal_sky_temp(const ThermalData *d, float air, float cloud, float cos_zenith) {
    if (cos_zenith <= 0.0f) return air;
    float dep = d->sky_clear_depression * (1.0f - (1.0f - d->sky_cloud_keep) * DG_CLAMP(cloud, 0.0f, 1.0f));
    return air - dep * powf(cos_zenith, d->sky_exponent);
}

// ---------------------------------------------------------------- radiance

double thermal_band_radiance_exact(double t_c) {
    const double h = 6.62607015e-34, c = 2.99792458e8, k = 1.380649e-23;
    double T = t_c + 273.15;
    if (T <= 1.0) return 0.0;
    const int n = 600;
    double l0 = 8e-6, l1 = 14e-6, dl = (l1 - l0) / n, sum = 0.0;
    for (int i = 0; i <= n; i++) {
        double l = l0 + dl * i;
        double b = 2.0 * h * c * c / (l * l * l * l * l) / (exp(h * c / (l * k * T)) - 1.0);
        double w = (i == 0 || i == n) ? 1.0 : (i & 1) ? 4.0 : 2.0;   // Simpson
        sum += w * b;
    }
    return sum * dl / 3.0;
}

static float g_lut[THERMAL_LUT_N];
static bool g_lut_ok;

const float *thermal_lut(void) {
    if (!g_lut_ok) {
        for (int i = 0; i < THERMAL_LUT_N; i++)
            g_lut[i] = (float)thermal_band_radiance_exact(THERMAL_LUT_T0 + THERMAL_LUT_DT * i);
        g_lut_ok = true;
    }
    return g_lut;
}

float thermal_radiance(float t_c) {
    const float *l = thermal_lut();
    float x = (t_c - THERMAL_LUT_T0) / THERMAL_LUT_DT;
    if (x <= 0.0f) return l[0];
    if (x >= THERMAL_LUT_N - 1) return l[THERMAL_LUT_N - 1];
    int i = (int)x;
    float f = x - (float)i;
    return l[i] + (l[i + 1] - l[i]) * f;
}

float thermal_temp_of_radiance(float r) {
    const float *l = thermal_lut();
    if (r <= l[0]) return THERMAL_LUT_T0;
    if (r >= l[THERMAL_LUT_N - 1]) return THERMAL_LUT_T0 + THERMAL_LUT_DT * (THERMAL_LUT_N - 1);
    int lo = 0, hi = THERMAL_LUT_N - 1;
    while (hi - lo > 1) {
        int m = (lo + hi) / 2;
        if (l[m] <= r) lo = m; else hi = m;
    }
    float f = (r - l[lo]) / (l[hi] - l[lo]);
    return THERMAL_LUT_T0 + THERMAL_LUT_DT * ((float)lo + f);
}

float thermal_surface_radiance(float e, float t, float t_refl) {
    return e * thermal_radiance(t) + (1.0f - e) * thermal_radiance(t_refl);
}

float thermal_spot_temp(float r, float e, float t_refl) {
    e = DG_CLAMP(e, 0.01f, 1.0f);
    return thermal_temp_of_radiance((r - (1.0f - e) * thermal_radiance(t_refl)) / e);
}
