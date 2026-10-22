// Night vision goggles: image-intensifier model (nvg_model.h, docs/NIGHT_OPTICS.md). Pure.
#include "render/post/nvg_model.h"
#include "cjson/cJSON.h"
#include <math.h>

static void variant_base(NvgVariant *v, const char *id, const char *label) {
    memset(v, 0, sizeof(*v));
    dg_strcpy(v->id, sizeof(v->id), id);
    dg_strcpy(v->label, sizeof(v->label), label);
    v->phosphor[0] = 0.40f; v->phosphor[1] = 1.00f; v->phosphor[2] = 0.32f;
    v->tubes = 1;
    v->circle_deg = 40.0f;
    v->halo_deg = 0.9f;
    v->whiteout_recover = 0.7f;
    v->persistence = 0.06f;
    v->gain_min = 20.0f; v->gain_max = 4000.0f;
    v->target = 0.32f;
    v->settle_down = 0.35f; v->settle_up = 0.9f;
    v->damping = 0.55f;
    v->tube_px = 1024.0f;
    v->photons = 12000.0f;
    v->noise_factor = 1.6f;
    v->ebi = 2.0e-5f;
    v->protect_s = 3.0f;
    v->runtime_h = 1.0f;
    v->illum_drain = 1.4f;
    v->illum_range = 40.0f; v->illum_angle = 30.0f; v->illum_intensity = 60.0f;
}

void nvg_config_default(NvgConfig *c) {
    memset(c, 0, sizeof(*c));
    variant_base(&c->v[0], "p43", "Green phosphor (P43)");
    variant_base(&c->v[1], "p45", "White phosphor (P45)");
    c->v[1].phosphor[0] = 0.86f; c->v[1].phosphor[1] = 0.90f; c->v[1].phosphor[2] = 1.00f;
    c->v[1].persistence = 0.04f;
    variant_base(&c->v[2], "dual", "Dual tube, white phosphor");
    c->v[2].phosphor[0] = 0.86f; c->v[2].phosphor[1] = 0.90f; c->v[2].phosphor[2] = 1.00f;
    c->v[2].persistence = 0.04f;
    c->v[2].tubes = 2;
    c->v[2].tube_sep_deg = 8.0f;
    c->v[2].runtime_h = 0.75f;
    c->count = 3;
    c->ir_moon = 1.0f;
    c->ir_sky = 3.0f;
    c->k_vis = 1.0f; c->k_nir = 1.0f;
    c->w_gaas[0] = 0.55f; c->w_gaas[1] = 0.35f; c->w_gaas[2] = 0.10f;
    c->y_sat = 1.0f;
    c->overload = 4.0f;
    c->flip_time = 0.6f;
}

static void num(const cJSON *o, const char *key, float *dst) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) *dst = (float)v->valuedouble;
}

static void vec3n(const cJSON *o, const char *key, float *dst) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_GetArraySize(a) != 3) return;
    for (int i = 0; i < 3; i++) dst[i] = (float)cJSON_GetArrayItem(a, i)->valuedouble;
}

bool nvg_config_load(NvgConfig *c, const char *json_text) {
    if (!json_text) return true;
    cJSON *root = cJSON_Parse(json_text);
    if (!root) return false;
    num(root, "ir_moon", &c->ir_moon); num(root, "ir_sky", &c->ir_sky);
    num(root, "k_vis", &c->k_vis); num(root, "k_nir", &c->k_nir);
    vec3n(root, "photocathode", c->w_gaas);
    num(root, "y_sat", &c->y_sat); num(root, "overload", &c->overload); num(root, "flip_time", &c->flip_time);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "variants")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
        if (!cJSON_IsString(id)) continue;
        NvgVariant *v = NULL;
        for (int i = 0; i < c->count; i++)
            if (!strcmp(c->v[i].id, id->valuestring)) v = &c->v[i];
        if (!v) {
            if (c->count >= NVG_VARIANTS) continue;
            v = &c->v[c->count++];
            variant_base(v, id->valuestring, id->valuestring);
        }
        const cJSON *lb = cJSON_GetObjectItemCaseSensitive(e, "label");
        if (cJSON_IsString(lb)) dg_strcpy(v->label, sizeof(v->label), lb->valuestring);
        vec3n(e, "phosphor", v->phosphor);
        float tubes = (float)v->tubes;
        num(e, "tubes", &tubes);
        v->tubes = DG_CLAMP((int)tubes, 1, 2);
        num(e, "circle_deg", &v->circle_deg); num(e, "tube_sep_deg", &v->tube_sep_deg);
        num(e, "halo_deg", &v->halo_deg); num(e, "whiteout_recover", &v->whiteout_recover);
        num(e, "persistence", &v->persistence); num(e, "gain_min", &v->gain_min); num(e, "gain_max", &v->gain_max);
        num(e, "target", &v->target); num(e, "settle_down", &v->settle_down); num(e, "settle_up", &v->settle_up);
        num(e, "damping", &v->damping); num(e, "tube_px", &v->tube_px); num(e, "photons", &v->photons);
        num(e, "noise_factor", &v->noise_factor); num(e, "ebi", &v->ebi); num(e, "protect_s", &v->protect_s);
        num(e, "runtime_h", &v->runtime_h); num(e, "illum_drain", &v->illum_drain);
        num(e, "illum_range", &v->illum_range); num(e, "illum_angle", &v->illum_angle);
        num(e, "illum_intensity", &v->illum_intensity);
    }
    cJSON_Delete(root);
    return true;
}

const NvgVariant *nvg_variant(const NvgConfig *c, const char *id) {
    for (int i = 0; i < c->count; i++)
        if (id && !strcmp(c->v[i].id, id)) return &c->v[i];
    return &c->v[0];
}

// ---------------------------------------------------------------- ABC / whiteout

float nvg_target_log_gain(const NvgVariant *v, float phi_mean) {
    float g = v->target / DG_MAX(phi_mean + v->ebi, 1e-9f);
    g = DG_CLAMP(g, v->gain_min, v->gain_max);
    return log2f(g);
}

float nvg_gain(const NvgState *s) { return exp2f(s->log_gain); }

void nvg_state_reset(NvgState *s, const NvgVariant *v, float phi_mean) {
    memset(s, 0, sizeof(*s));
    s->log_gain = nvg_target_log_gain(v, phi_mean);
    s->valid = true;
}

void nvg_step(NvgState *s, const NvgConfig *c, const NvgVariant *v, const NvgInput *in) {
    if (!s->valid) nvg_state_reset(s, v, in->phi_mean);
    float dt = DG_MAX(in->dt, 0.0f);
    s->frame++;
    s->shutoff = false;
    // ABC: damped spring on log2 gain; settle time ~ 4 / (zeta omega)
    float target = nvg_target_log_gain(v, in->phi_mean);
    float zeta = DG_CLAMP(v->damping, 0.1f, 2.0f);
    float settle = target < s->log_gain ? v->settle_down : v->settle_up;
    float omega = 4.0f / (zeta * DG_MAX(settle, 0.01f));
    float prev = s->log_gain;
    int n = (int)ceilf(dt / 0.004f);
    float h = n > 0 ? dt / (float)n : 0.0f;
    for (int i = 0; i < n; i++) {   // semi-implicit Euler, small substeps
        float acc = omega * omega * (target - s->log_gain) - 2.0f * zeta * omega * s->vel;
        s->vel += acc * h;
        s->log_gain += s->vel * h;
    }
    float lo = log2f(v->gain_min), hi = log2f(v->gain_max);
    if (s->log_gain < lo) { s->log_gain = lo; s->vel = DG_MAX(s->vel, 0.0f); }
    if (s->log_gain > hi) { s->log_gain = hi; s->vel = DG_MIN(s->vel, 0.0f); }

    // whiteout: the log mean over saturation or the photocurrent (linear mean: a few small lamps
    // barely move it, a light filling the tube does) over c->overload x saturation, log scale (2 stops
    // over = full whiteout)
    float g = nvg_gain(s);
    float load = g * DG_MAX(in->phi_current / c->overload, in->phi_mean) / c->y_sat;
    float over = log2f(DG_MAX(load, 1e-6f));
    float w_target = DG_CLAMP(over * 0.5f, 0.0f, 1.0f);
    if (w_target > s->whiteout)
        s->whiteout = w_target + (s->whiteout - w_target) * expf(-dt / 0.03f);
    else   // ln(20) = 3: under 5 % of an overload within whiteout_recover
        s->whiteout = w_target + (s->whiteout - w_target) * expf(-dt * 3.0f / DG_MAX(v->whiteout_recover, 0.05f));

    // autogating: the supply gates the photocathode while the gain is pulled down fast
    float rate = dt > 0.0f ? (prev - s->log_gain) / dt : 0.0f;   // stops / s, > 0 = gain falling
    float p = DG_CLAMP((rate - 2.0f) * 0.06f, 0.0f, 0.5f) + (w_target > 0.05f && s->whiteout < 0.95f ? 0.1f : 0.0f);
    u32 hsh = dg_hash_u32(s->frame * 747796405u + 2891336453u);
    s->flicker = ((float)(hsh & 0xffff) / 65535.0f) < p ? 0.6f : 0.0f;

    // tube protection: the gain is at its minimum and the mean is still far over the range
    bool extreme = s->log_gain <= lo + 0.01f && v->gain_min * in->phi_mean > c->y_sat * c->overload;
    s->overbright = extreme ? s->overbright + dt : DG_MAX(s->overbright - 2.0f * dt, 0.0f);
    if (s->overbright >= v->protect_s) { s->shutoff = true; s->overbright = 0.0f; }
}

float nvg_screen(const NvgConfig *c, const NvgState *s, float gain, float phi) {
    float y = c->y_sat * (1.0f - expf(-gain * DG_MAX(phi, 0.0f) / c->y_sat));
    float w = s ? s->whiteout : 0.0f;
    y = y + (0.92f - y) * w * 0.85f;   // washed out: floor raised, contrast compressed
    if (s) y *= 1.0f - s->flicker;
    return DG_CLAMP(y, 0.0f, 1.0f);
}

// ---------------------------------------------------------------- noise

float nvg_photoelectrons(const NvgVariant *v, float phi, float dt) {
    return (DG_MAX(phi, 0.0f) + v->ebi) * v->photons * dt * 60.0f;
}

float nvg_noise_rel(const NvgVariant *v, float phi, float dt) {
    return sqrtf(v->noise_factor / DG_MAX(nvg_photoelectrons(v, phi, dt), 1e-6f));
}

static float gauss(Rng *r) {
    float u1 = DG_MAX(rng_float(r), 1e-7f), u2 = rng_float(r);
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

float nvg_noise_sample(const NvgVariant *v, float phi, float dt, Rng *rng) {
    float n = nvg_photoelectrons(v, phi, dt);
    float k;
    if (n > 20.0f) {
        k = n + sqrtf(n) * gauss(rng);
    } else {   // Knuth Poisson
        float l = expf(-n), p = 1.0f;
        k = -1.0f;
        do { k += 1.0f; p *= rng_float(rng); } while (p > l && k < 200.0f);
    }
    // MCP gain per electron: exponential-ish spread with excess noise factor F (gamma, shape 1 / (F - 1))
    float shape = 1.0f / DG_MAX(v->noise_factor - 1.0f, 0.05f);
    float g = 0.0f;
    if (k > 0.0f) {   // sum of k gains ~ gamma(k shape) / shape: normal approximation
        float mean = k, sd = sqrtf(k / shape);
        g = DG_MAX(mean + sd * gauss(rng), 0.0f);
    }
    return g / DG_MAX(n, 1e-6f);
}

// ---------------------------------------------------------------- geometry

float nvg_halo_px(const NvgVariant *v, float hfov_deg, float width_px) {
    float t = tanf(v->halo_deg * 0.017453293f) / tanf(hfov_deg * 0.5f * 0.017453293f);
    return t * width_px * 0.5f;
}

void nvg_pixel_angles(float px, float py, float w, float h, float hfov_deg, float *ax, float *ay) {
    float f = (w * 0.5f) / tanf(hfov_deg * 0.5f * 0.017453293f);   // focal length in pixels
    *ax = atanf((px - w * 0.5f) / f) * 57.29578f;
    *ay = atanf((py - h * 0.5f) / f) * 57.29578f;
}

// Angle between the pixel's ray and a tube axis turned cx deg about the vertical: a true cone.
static float circle(float x, float y, float f, float cx, float r) {
    float c = cx * 0.017453293f;
    float len = sqrtf(x * x + y * y + f * f);
    float d = acosf(DG_CLAMP((x * sinf(c) + f * cosf(c)) / len, -1.0f, 1.0f)) * 57.29578f;
    float t = DG_CLAMP((r - d) / 0.4f + 0.5f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float nvg_mask(const NvgVariant *v, float px, float py, float w, float h, float hfov_deg) {
    float f = (w * 0.5f) / tanf(hfov_deg * 0.5f * 0.017453293f);
    float x = px - w * 0.5f, y = py - h * 0.5f;
    float r = v->circle_deg * 0.5f;
    if (v->tubes < 2) return circle(x, y, f, 0.0f, r);
    return DG_MAX(circle(x, y, f, -v->tube_sep_deg, r), circle(x, y, f, v->tube_sep_deg, r));
}

float nvg_battery_drain(const NvgVariant *v, float charge, bool illuminator, float dt) {
    if (v->runtime_h <= 0.0f) return charge;
    float rate = (illuminator ? v->illum_drain : 1.0f) / (v->runtime_h * 3600.0f);
    return DG_MAX(charge - rate * dt, 0.0f);
}
