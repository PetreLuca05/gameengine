// Sky-driven shadows (docs/SHADOWS.md): what the sky and the weather make of the sun / moon
// shadows each frame. Nobody sets a shadow strength or softness: both follow from
//
//   contrast   C = E_direct / E_sky (direct irradiance at normal incidence vs the sky's horizontal
//              irradiance): the lit / shaded luminance ratio is 1 + C cos(theta); the shading
//              (direct x shadow + sky SH x AO, ambient scale 1) already gives it, the driver only
//              decides when the cast shadow is worth drawing (cutoff with hysteresis + fade)
//   softness   the effective angular radius of the source: the disc (sun 0.265 deg) widened by the
//              aureole of haze, thin cloud, fog and rain (shadows_sky.json curves). Penumbra half
//              width = d x tan(radius), d the blocker distance: measured (PCSS, weight pcss_weight)
//              or assumed (pcf_ref_m, the PCF tiers); one continuous filter, no discrete modes; past
//              wide_deg cascade 0 fades out (wide blur from cascade 1), under the cutoff the passes
//              are skipped after the fade
//   clouds     a uniform sheet (veil) attenuates the direct light: exp(-tau / mu); the cloud
//              shadow map adds the patches (cumulus tau) and drifts with the weather wind aloft
//   moon       the sun owns the cascades while any of its disc is up, the moon after that; while
//              the sun owns them the moon has its own tile (atlas tile 3) on the tiers with moon
//              shadows: both lights stay shadowed and the handover changes nothing on screen
//
// Pure: no renderer, no world (tests/test_shadow_sky.c). game_daynight.c fills the input from the
// sky / weather, the renderer gets the output through RenderEnvironment.
#ifndef DG_SHADOW_SKY_H
#define DG_SHADOW_SKY_H

#include "core/base.h"
#include "core/dgmath.h"

#define SSKY_CURVE_KEYS 12
#define SSKY_PATH "assets/render/shadows_sky.json"

// Piecewise linear, clamped at both ends.
typedef struct SskyCurve {
    int n;
    float x[SSKY_CURVE_KEYS], y[SSKY_CURVE_KEYS];
} SskyCurve;

typedef struct ShadowSkyConfig {
    float sun_radius_deg, moon_radius_deg;   // physical disc angular radii
    // Clouds: veil optical depth by cover (x density factor), cumulus patch depth (cloud shadow map).
    SskyCurve veil_tau;                      // cloud cover -> tau of the uniform sheet
    float veil_density_k;                    // tau x (1 - k + k x density)
    float cumulus_tau;                       // optical depth of a full cloud texel (cloud shadow map)
    float min_mu;                            // airmass clamp (low sun)
    // Aureole (deg, combined in quadrature with the disc).
    SskyCurve aureole_haze;                  // aerosol excess (mie_scale - 1) -> deg
    SskyCurve aureole_veil;                  // veil tau -> deg
    SskyCurve aureole_fog;                   // fog density (1/m) -> deg
    SskyCurve aureole_precip;                // precipitation 0..1 -> deg
    float max_radius_deg;
    // Cutoff (contrast C) and hysteresis.
    float cutoff_off, cutoff_on;             // fade 0 at off, 1 at on
    float cutoff_reenable;                   // passes come back above off x (1 + this)
    float moon_min_direct;                   // absolute floor for the moon (new moon)
    // Wide mode: cascade 0 fades out past wide_deg, back under wide_deg x (1 - wide_hyst).
    float wide_deg, wide_hyst;
    float smooth_s;                          // time constant of radius / fade / weights (s)
    // One filter (docs/SHADOWS.md "The filter"): half width = d x tan(r) in metres, d measured by the
    // blocker search (PCSS tier) or pcf_ref_m (PCF tiers), clamped to [min_texels, max] texels.
    float pcf_ref_m;                         // PCF tiers: assumed blocker distance (m)
    float min_texels;                        // smallest filter half width (texels): no aliased edges
    float era_pcf;                           // fixed-hardness era (2010): this half width (cascade 0 texels, kept in metres)
    float mode_s;                            // time constant of a tier change (PCF <-> PCSS, max width), s
    // PCSS: blocker search distance (m), max filter half width (texels) per softness tier.
    float pcss_search_m;
    float pcss_max_texels[3];
    float contact_px;                        // contact shadow ray: until the penumbra spans this many px
    // Exposure compensation (EV) by condition, summed.
    SskyCurve exposure_cover;                // effective cloud cover -> EV
    SskyCurve exposure_snow;                 // snow cover -> EV
    // Ground below the sky probe (SH lower hemisphere).
    float ground_albedo[3], snow_albedo[3];
    float wet_darken;                        // diffuse albedo x (1 - this x wetness)
    // Cloud drift: the weather wind at the cloud layer.
    float cloud_wind_aloft, cloud_wind_min;  // x surface speed, m/s floor
} ShadowSkyConfig;

typedef struct ShadowSkyInput {
    float dt;                                // s (0: snap, no smoothing)
    float sun_elev, moon_elev;               // rad
    vec3 sun_light, moon_light;              // ground irradiance through the atmosphere (rgb, engine units; no clouds)
    vec3 sky_up;                             // sky horizontal irradiance (SH, ambient scale applied)
    float cloud_cover, cloud_density;        // 0..1
    float fog, precip, snow, wetness;        // 1/m, 0..1, 0..1, 0..1
    float mie_scale;                         // aerosols (1 = clear)
    float wind_dir_deg, wind_speed;          // weather wind (towards, 0 = -Z, 90 = +X), m/s
    int softness;                            // fidelity: 0 PCF (12 taps), 1 PCF (16 taps), 2 PCSS + wide mode
    bool moon_shadows;                       // fidelity: the moon tile while the sun owns the cascades
    bool fixed_hardness;                     // era 2010: one PCF radius, contrast still physical
} ShadowSkyInput;

typedef struct SskyLight {
    vec3 color;                              // normalised (max channel 1)
    float intensity;                         // direct irradiance (max channel), veil applied
    float radius_deg;                        // effective source radius (smoothed)
    float contrast;                          // E_direct / E_sky
    float fade;                              // cast shadow weight 0..1 (smoothed)
    bool cast;                               // its shadow passes run
} SskyLight;

typedef struct ShadowSkyState {
    bool init;
    bool cast[2], wide;
    float fade[2], radius[2], near_w;
    float pcss_w, max_tex;                   // tier crossfade (filter_model / pcss_max_texels targets)
    vec2 cloud_offset;
} ShadowSkyState;

typedef struct ShadowSkyOut {
    SskyLight key;                           // owns the cascades
    SskyLight second;                        // the moon while the sun owns the cascades (intensity 0: none)
    bool key_is_moon, second_shadowed;
    float sky_e;                             // sky horizontal irradiance (max channel)
    float veil_tau, veil_t;                  // sheet optical depth, its transmittance at the key
    float cumulus_tau;                       // cloud shadow map depth (0 = no cloud shadows)
    float near_weight;                       // cascade 0 weight (0 = wide mode: not drawn)
    int filter_model;                        // tap budget: 0 PCF 12 taps, 1 PCF 16 taps, 2 PCSS 16 + 16
    float pcss_weight;                       // 0 .. 1: measured vs assumed blocker distance (smoothed over mode_s)
    float pcf_ref_m, min_texels;             // assumed blocker distance (m), smallest half width (texels)
    float fixed_texels;                      // era 2010: fixed half width (cascade 0 texels), 0 = from the sky
    float pcss_max_texels, pcss_search_m;    // max half width (texels, smoothed over mode_s), search (m)
    float contact_px;                        // contact shadow ray: until the penumbra spans this many px
    vec2 cloud_offset, cloud_vel;            // m, m/s
    float exposure_comp;                     // EV
    vec3 ground_albedo;
} ShadowSkyOut;

void ssky_config_default(ShadowSkyConfig *c);
// JSON text / file (missing keys keep their value). False + err on a parse error or a bad curve.
bool ssky_config_parse(ShadowSkyConfig *c, const char *json, char *err, int err_size);
bool ssky_config_load(ShadowSkyConfig *c, const char *path, char *err, int err_size);

float ssky_curve(const SskyCurve *k, float x);
float ssky_veil_tau(const ShadowSkyConfig *c, float cover, float density);
// Effective source radius (deg) for a disc of disc_deg under the given air / cloud state.
float ssky_source_radius(const ShadowSkyConfig *c, float disc_deg, float mie_scale, float veil_tau, float fog, float precip);
// Penumbra half width (m) at blocker distance d (m).
float ssky_penumbra(float d, float radius_deg);
// Expected lit / shaded luminance ratio for a light at incidence cos_theta.
float ssky_lit_shade_ratio(const SskyLight *l, float sky_e, float cos_theta);

void ssky_state_reset(ShadowSkyState *s);
void ssky_update(ShadowSkyState *s, const ShadowSkyConfig *c, const ShadowSkyInput *in, ShadowSkyOut *out);

#endif
