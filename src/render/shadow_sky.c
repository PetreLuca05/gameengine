// Sky-driven shadows (shadow_sky.h, docs/SHADOWS.md): config, curves, the per-frame driver.
#include "render/shadow_sky.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void curve_set(SskyCurve *k, int n, const float *xy) {
    k->n = n;
    for (int i = 0; i < n; i++) { k->x[i] = xy[i * 2]; k->y[i] = xy[i * 2 + 1]; }
}

void ssky_config_default(ShadowSkyConfig *c) {
    memset(c, 0, sizeof(*c));
    c->sun_radius_deg = 0.265f;
    c->moon_radius_deg = 0.26f;
    // Cloud sheet: broken cumulus barely veils the sun between the patches; altostratus /
    // stratus decks above ~0.8 cover reach tau 10+ (direct light gone).
    static const float veil[] = { 0.0f, 0.0f, 0.4f, 0.05f, 0.65f, 0.4f, 0.8f, 1.5f, 0.9f, 5.0f, 1.0f, 14.0f };
    curve_set(&c->veil_tau, 6, veil);
    c->veil_density_k = 0.6f;
    c->cumulus_tau = 3.0f;
    c->min_mu = 0.08f;
    static const float haze[] = { 0.0f, 0.0f, 0.5f, 0.6f, 1.5f, 1.4f, 4.0f, 3.0f };
    curve_set(&c->aureole_haze, 4, haze);
    static const float av[] = { 0.0f, 0.0f, 0.3f, 1.5f, 1.0f, 3.5f, 3.0f, 8.0f };
    curve_set(&c->aureole_veil, 4, av);
    static const float af[] = { 0.0f, 0.0f, 0.005f, 0.4f, 0.02f, 2.0f, 0.08f, 6.0f };
    curve_set(&c->aureole_fog, 4, af);
    static const float ap[] = { 0.0f, 0.0f, 0.3f, 1.5f, 1.0f, 5.0f };
    curve_set(&c->aureole_precip, 3, ap);
    c->max_radius_deg = 12.0f;
    c->cutoff_off = 0.08f;
    c->cutoff_on = 0.15f;
    c->cutoff_reenable = 0.25f;
    c->moon_min_direct = 0.0005f;
    c->wide_deg = 2.0f;
    c->wide_hyst = 0.15f;
    c->smooth_s = 1.0f;
    c->pcf_ref_m = 2.0f;
    c->min_texels = 1.0f;
    c->era_pcf = 1.5f;
    c->mode_s = 0.12f;
    c->pcss_search_m = 30.0f;
    c->pcss_max_texels[0] = 4.0f;
    c->pcss_max_texels[1] = 8.0f;
    c->pcss_max_texels[2] = 32.0f;
    c->contact_px = 1.5f;
    static const float ec[] = { 0.0f, 0.0f, 0.5f, 0.1f, 0.9f, 0.3f, 1.0f, 0.3f };
    curve_set(&c->exposure_cover, 4, ec);
    static const float es[] = { 0.0f, 0.0f, 1.0f, 0.6f };
    curve_set(&c->exposure_snow, 2, es);
    c->ground_albedo[0] = 0.25f; c->ground_albedo[1] = 0.24f; c->ground_albedo[2] = 0.22f;
    c->snow_albedo[0] = 0.86f; c->snow_albedo[1] = 0.89f; c->snow_albedo[2] = 0.93f;
    c->wet_darken = 0.35f;
    c->cloud_wind_aloft = 2.5f;
    c->cloud_wind_min = 2.0f;
}

// ------------------------------------------------------------------ JSON

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void jv3(const cJSON *o, const char *k, float *out) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != 3) return;
    for (int i = 0; i < 3; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (cJSON_IsNumber(v)) out[i] = (float)v->valuedouble;
    }
}

// [[x, y], ...], x increasing.
static bool jcurve(const cJSON *o, const char *k, SskyCurve *out, char *err, int err_size) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!a) return true;
    int n = cJSON_IsArray(a) ? cJSON_GetArraySize(a) : 0;
    if (n < 1 || n > SSKY_CURVE_KEYS) { snprintf(err, (size_t)err_size, "curve '%s': 1..%d keys", k, SSKY_CURVE_KEYS); return false; }
    SskyCurve c = { 0 };
    for (int i = 0; i < n; i++) {
        const cJSON *p = cJSON_GetArrayItem(a, i);
        const cJSON *x = cJSON_GetArrayItem(p, 0), *y = cJSON_GetArrayItem(p, 1);
        if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) { snprintf(err, (size_t)err_size, "curve '%s': key %d is not [x, y]", k, i); return false; }
        c.x[i] = (float)x->valuedouble;
        c.y[i] = (float)y->valuedouble;
        if (i > 0 && c.x[i] <= c.x[i - 1]) { snprintf(err, (size_t)err_size, "curve '%s': x must increase (key %d)", k, i); return false; }
    }
    c.n = n;
    *out = c;
    return true;
}

bool ssky_config_parse(ShadowSkyConfig *c, const char *json, char *err, int err_size) {
    cJSON *r = cJSON_Parse(json);
    if (!r) { snprintf(err, (size_t)err_size, "JSON parse error near '%.24s'", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : ""); return false; }
    ShadowSkyConfig n = *c;
    bool ok = true;
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(r, "source");
    n.sun_radius_deg = jf(src, "sun_radius_deg", n.sun_radius_deg);
    n.moon_radius_deg = jf(src, "moon_radius_deg", n.moon_radius_deg);
    n.max_radius_deg = jf(src, "max_radius_deg", n.max_radius_deg);
    ok = ok && jcurve(src, "aureole_haze", &n.aureole_haze, err, err_size);
    ok = ok && jcurve(src, "aureole_veil", &n.aureole_veil, err, err_size);
    ok = ok && jcurve(src, "aureole_fog", &n.aureole_fog, err, err_size);
    ok = ok && jcurve(src, "aureole_precip", &n.aureole_precip, err, err_size);
    const cJSON *cl = cJSON_GetObjectItemCaseSensitive(r, "clouds");
    ok = ok && jcurve(cl, "veil_tau", &n.veil_tau, err, err_size);
    n.veil_density_k = jf(cl, "veil_density_k", n.veil_density_k);
    n.cumulus_tau = jf(cl, "cumulus_tau", n.cumulus_tau);
    n.min_mu = jf(cl, "min_mu", n.min_mu);
    n.cloud_wind_aloft = jf(cl, "wind_aloft", n.cloud_wind_aloft);
    n.cloud_wind_min = jf(cl, "wind_min", n.cloud_wind_min);
    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(r, "cutoff");
    n.cutoff_off = jf(cu, "off", n.cutoff_off);
    n.cutoff_on = jf(cu, "on", n.cutoff_on);
    n.cutoff_reenable = jf(cu, "reenable", n.cutoff_reenable);
    n.moon_min_direct = jf(cu, "moon_min_direct", n.moon_min_direct);
    n.smooth_s = jf(cu, "smooth_s", n.smooth_s);
    const cJSON *fi = cJSON_GetObjectItemCaseSensitive(r, "filter");
    n.wide_deg = jf(fi, "wide_deg", n.wide_deg);
    n.wide_hyst = jf(fi, "wide_hyst", n.wide_hyst);
    n.pcf_ref_m = jf(fi, "pcf_ref_m", n.pcf_ref_m);
    n.min_texels = jf(fi, "min_texels", n.min_texels);
    n.era_pcf = jf(fi, "era_pcf", n.era_pcf);
    n.mode_s = jf(fi, "mode_s", n.mode_s);
    n.pcss_search_m = jf(fi, "pcss_search_m", n.pcss_search_m);
    const cJSON *pm = cJSON_GetObjectItemCaseSensitive(fi, "pcss_max_texels");
    if (cJSON_IsArray(pm) && cJSON_GetArraySize(pm) == 3)
        for (int i = 0; i < 3; i++) n.pcss_max_texels[i] = (float)cJSON_GetArrayItem(pm, i)->valuedouble;
    n.contact_px = jf(fi, "contact_px", n.contact_px);
    const cJSON *ex = cJSON_GetObjectItemCaseSensitive(r, "exposure");
    ok = ok && jcurve(ex, "cover", &n.exposure_cover, err, err_size);
    ok = ok && jcurve(ex, "snow", &n.exposure_snow, err, err_size);
    const cJSON *gr = cJSON_GetObjectItemCaseSensitive(r, "ground");
    jv3(gr, "albedo", n.ground_albedo);
    jv3(gr, "snow_albedo", n.snow_albedo);
    n.wet_darken = jf(gr, "wet_darken", n.wet_darken);
    cJSON_Delete(r);
    if (ok && !(n.cutoff_on > n.cutoff_off && n.cutoff_off >= 0.0f)) { snprintf(err, (size_t)err_size, "cutoff: need 0 <= off < on"); ok = false; }
    if (ok && !(n.pcf_ref_m > 0.0f && n.min_texels >= 0.5f && n.pcss_max_texels[0] >= n.min_texels)) {
        snprintf(err, (size_t)err_size, "filter: need pcf_ref_m > 0, min_texels >= 0.5, pcss_max_texels >= min_texels");
        ok = false;
    }
    if (ok && !(n.wide_deg > 0.0f && n.sun_radius_deg > 0.0f && n.moon_radius_deg > 0.0f)) { snprintf(err, (size_t)err_size, "radii must be > 0"); ok = false; }
    if (ok) *c = n;
    return ok;
}

bool ssky_config_load(ShadowSkyConfig *c, const char *path, char *err, int err_size) {
    char *txt = platform_read_file(path, NULL);
    if (!txt) { snprintf(err, (size_t)err_size, "cannot read %s", path); return false; }
    bool ok = ssky_config_parse(c, txt, err, err_size);
    free(txt);
    return ok;
}

// ------------------------------------------------------------------ model

float ssky_curve(const SskyCurve *k, float x) {
    if (k->n <= 0) return 0.0f;
    if (x <= k->x[0]) return k->y[0];
    for (int i = 1; i < k->n; i++)
        if (x < k->x[i]) {
            float t = (x - k->x[i - 1]) / (k->x[i] - k->x[i - 1]);
            return k->y[i - 1] + (k->y[i] - k->y[i - 1]) * t;
        }
    return k->y[k->n - 1];
}

float ssky_veil_tau(const ShadowSkyConfig *c, float cover, float density) {
    float d = clampf(density, 0.0f, 1.0f);
    return ssky_curve(&c->veil_tau, clampf(cover, 0.0f, 1.0f)) * (1.0f - c->veil_density_k + c->veil_density_k * d);
}

float ssky_source_radius(const ShadowSkyConfig *c, float disc_deg, float mie_scale, float veil_tau, float fog, float precip) {
    float h = ssky_curve(&c->aureole_haze, fmaxf(mie_scale - 1.0f, 0.0f));
    float v = ssky_curve(&c->aureole_veil, fmaxf(veil_tau, 0.0f));
    float f = ssky_curve(&c->aureole_fog, fmaxf(fog, 0.0f));
    float p = ssky_curve(&c->aureole_precip, clampf(precip, 0.0f, 1.0f));
    float r = sqrtf(disc_deg * disc_deg + h * h + v * v + f * f + p * p);
    return fminf(r, c->max_radius_deg);
}

float ssky_penumbra(float d, float radius_deg) { return d * tanf(radius_deg * DG_DEG2RAD); }

float ssky_lit_shade_ratio(const SskyLight *l, float sky_e, float cos_theta) {
    if (sky_e <= 1e-9f) return l->intensity > 0 ? 1e9f : 1.0f;
    return 1.0f + l->intensity * fmaxf(cos_theta, 0.0f) / sky_e;
}

void ssky_state_reset(ShadowSkyState *s) { memset(s, 0, sizeof(*s)); }

static float vmax3(vec3 v) { return fmaxf(v.x, fmaxf(v.y, v.z)); }

// Exponential approach (dt 0 = snap).
static float approach(float cur, float target, float dt, float tc) {
    if (dt <= 0.0f || tc <= 0.0f) return target;
    return target + (cur - target) * expf(-dt / tc);
}

// One light: contrast, cutoff with hysteresis (state cast), smoothed fade and radius.
static void light_eval(SskyLight *l, vec3 rgb, float sky_e, float radius_target, float min_direct, bool *cast, float *fade, float *radius,
                       const ShadowSkyConfig *c, float dt, bool snap) {
    float e = vmax3(rgb);
    l->color = e > 1e-9f ? v3_scale(rgb, 1.0f / e) : v3(1, 1, 1);
    l->intensity = e;
    l->contrast = sky_e > 1e-9f ? e / sky_e : (e > 0 ? 1e6f : 0.0f);
    float C = e >= min_direct ? l->contrast : 0.0f;
    if (*cast && C < c->cutoff_off) *cast = false;
    else if (!*cast && C > c->cutoff_off * (1.0f + c->cutoff_reenable)) *cast = true;
    float target = *cast ? smoothstepf(c->cutoff_off, c->cutoff_on, C) : 0.0f;
    *fade = snap ? target : approach(*fade, target, dt, c->smooth_s);
    if (!*cast && *fade < 0.002f) *fade = 0.0f;
    // The passes run while the fade is still visible on the way out.
    l->cast = *cast || *fade > 0.0f;
    *radius = snap ? radius_target : approach(*radius, radius_target, dt, c->smooth_s);
    l->radius_deg = *radius;
    l->fade = *fade;
}

void ssky_update(ShadowSkyState *s, const ShadowSkyConfig *c, const ShadowSkyInput *in, ShadowSkyOut *out) {
    memset(out, 0, sizeof(*out));
    bool snap = !s->init || in->dt <= 0.0f;
    // The fades and the source radius follow the sky, which follows the clock: at time_scale 60 they
    // keep up with the sun instead of lagging an hour of game time (the tier crossfade below stays in
    // real seconds: a settings change is not the sky's).
    float dt = in->dt * fmaxf(in->clock_speed, 1.0f);
    out->sky_e = vmax3(in->sky_up);
    // Cloud sheet over the key's path.
    out->veil_tau = ssky_veil_tau(c, in->cloud_cover, in->cloud_density);
    float sun_mu = fmaxf(sinf(in->sun_elev), c->min_mu), moon_mu = fmaxf(sinf(in->moon_elev), c->min_mu);
    float t_sun = expf(-out->veil_tau / sun_mu), t_moon = expf(-out->veil_tau / moon_mu);
    vec3 sun = v3_scale(in->sun_light, t_sun), moon = v3_scale(in->moon_light, t_moon);
    // The sun owns the cascades while any of its disc is up.
    bool sun_up = vmax3(in->sun_light) > 0.0f;
    out->key_is_moon = !sun_up;
    out->veil_t = sun_up ? t_sun : t_moon;
    float r_sun = ssky_source_radius(c, c->sun_radius_deg, in->mie_scale, out->veil_tau, in->fog, in->precip);
    float r_moon = ssky_source_radius(c, c->moon_radius_deg, in->mie_scale, out->veil_tau, in->fog, in->precip);
    // State slot 0 = the sun, 1 = the moon (on the cascades or in its tile: one history, the
    // handover is continuous).
    SskyLight *key = &out->key, *sec = &out->second;
    if (!sun_up) {
        // Moon on the cascades: it continues slot 1's history.
        light_eval(key, moon, out->sky_e, r_moon, c->moon_min_direct, &s->cast[1], &s->fade[1], &s->radius[1], c, dt, snap);
        s->cast[0] = false; s->fade[0] = 0.0f; s->radius[0] = r_sun;
    } else {
        light_eval(key, sun, out->sky_e, r_sun, 0.0f, &s->cast[0], &s->fade[0], &s->radius[0], c, dt, snap);
        light_eval(sec, moon, out->sky_e, r_moon, c->moon_min_direct, &s->cast[1], &s->fade[1], &s->radius[1], c, dt, snap);
        out->second_shadowed = in->moon_shadows && sec->cast;
        if (!in->moon_shadows) { sec->cast = false; sec->fade = 0.0f; }
    }
    // Wide mode: cascade 0 fades out (the filter would need more taps than it can afford).
    float r = key->radius_deg;
    bool can_wide = in->softness >= 2 && !in->fixed_hardness;
    if (!can_wide) s->wide = false;
    else if (!s->wide && r > c->wide_deg) s->wide = true;
    else if (s->wide && r < c->wide_deg * (1.0f - c->wide_hyst)) s->wide = false;
    float near_target = s->wide ? 0.0f : 1.0f;
    s->near_w = snap ? near_target : approach(s->near_w, near_target, dt, c->smooth_s);
    if (s->wide && s->near_w < 0.01f) s->near_w = 0.0f;
    if (!s->wide && s->near_w > 0.99f) s->near_w = 1.0f;
    out->near_weight = key->cast ? s->near_w : 1.0f;
    // One filter for every tier: the half width is d x tan(r) in metres with d measured (PCSS) or
    // assumed (pcf_ref_m). A tier change (Engine Fidelity) only moves the PCSS weight and the
    // max width, both over mode_s: no discrete switch of the filter between frames.
    int soft = DG_CLAMP(in->softness, 0, 2);
    float pcss_target = soft >= 2 && !in->fixed_hardness ? 1.0f : 0.0f;
    float max_target = c->pcss_max_texels[soft];
    if (snap || s->max_tex <= 0.0f) { s->pcss_w = pcss_target; s->max_tex = max_target; }
    else {
        s->pcss_w = approach(s->pcss_w, pcss_target, in->dt, c->mode_s);
        s->max_tex = approach(s->max_tex, max_target, in->dt, c->mode_s);
        if (fabsf(s->pcss_w - pcss_target) < 1e-3f) s->pcss_w = pcss_target;
    }
    out->filter_model = in->fixed_hardness ? 0 : soft;
    if (s->pcss_w > 0.0f && out->filter_model < 2) out->filter_model = 2;   // the blocker search runs while PCSS fades
    out->pcss_weight = s->pcss_w;
    out->pcf_ref_m = c->pcf_ref_m;
    out->min_texels = c->min_texels;
    out->fixed_texels = in->fixed_hardness ? c->era_pcf : 0.0f;
    out->pcss_max_texels = s->max_tex;
    out->pcss_search_m = c->pcss_search_m;
    out->contact_px = c->contact_px;
    // Cloud patches (the cloud shadow map) and their drift: the weather wind at the layer.
    out->cumulus_tau = c->cumulus_tau;
    float ws = fmaxf(in->wind_speed * c->cloud_wind_aloft, c->cloud_wind_min);
    float wd = in->wind_dir_deg * DG_DEG2RAD;
    out->cloud_vel = v2(sinf(wd) * ws, -cosf(wd) * ws);
    // The drift advances with the world clock (drift_dt: paused with it, a time-lapse at time_scale),
    // in double without a wrap: after 100 game days (~1e6 m) a frame's step is still exact.
    if (!snap) {
        s->cloud_offset[0] += (double)out->cloud_vel.x * in->drift_dt;
        s->cloud_offset[1] += (double)out->cloud_vel.y * in->drift_dt;
    }
    out->cloud_offset_d[0] = s->cloud_offset[0];
    out->cloud_offset_d[1] = s->cloud_offset[1];
    out->cloud_offset = v2((float)s->cloud_offset[0], (float)s->cloud_offset[1]);
    // Exposure: overcast / snow scenes metered to mid grey read darker than they are.
    float eff_cover = clampf(in->cloud_cover * (0.6f + 0.4f * clampf(in->cloud_density, 0.0f, 1.0f)) + in->precip * 0.3f, 0.0f, 1.0f);
    out->exposure_comp = ssky_curve(&c->exposure_cover, eff_cover) + ssky_curve(&c->exposure_snow, clampf(in->snow, 0.0f, 1.0f));
    // The ground under the sky probe: snow brightens the bounce, wet ground darkens it.
    float sn = clampf(in->snow, 0.0f, 1.0f), wet = clampf(in->wetness, 0.0f, 1.0f) * (1.0f - sn);
    vec3 g = v3(c->ground_albedo[0], c->ground_albedo[1], c->ground_albedo[2]);
    g = v3_lerp(g, v3(c->snow_albedo[0], c->snow_albedo[1], c->snow_albedo[2]), sn);
    out->ground_albedo = v3_scale(g, 1.0f - c->wet_darken * wet);
    s->init = true;
}
