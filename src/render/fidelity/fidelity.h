// Engine Fidelity (docs/ENGINE_FIDELITY.md): the base layer of the graphics settings. Five levels
// (Off, Low, Medium, High, Modern), each a complete table over every graphics feature, per era
// (r_era: modern, 2020, 2015, 2010). For each feature a level says
//   * unavailable / available (default off) / available (default on),
//   * the default value and the bounds the user may choose within (max tier, min / max value or
//     an explicit set of allowed values).
// Every other graphics control (post-processing & effects, resolution / upscaler, shadows,
// foliage, era look ...) is a cvar bound to one feature: whatever sets it (menu, console, config,
// presets, the era preset) goes through cvar.bound and is clamped to the current level's bounds.
//
// Data: assets/render/fidelity/{off,low,medium,high,modern}.json (+ eras.json, the era capability
// matrix). Pure C (cvar system + cJSON, no renderer): tests/test_fidelity.c.
#ifndef DG_FIDELITY_H
#define DG_FIDELITY_H

#include "core/base.h"

typedef enum FidLevel { FID_OFF, FID_LOW, FID_MEDIUM, FID_HIGH, FID_MODERN, FID_LEVELS } FidLevel;
typedef enum FidEra { FID_ERA_MODERN, FID_ERA_2020, FID_ERA_2015, FID_ERA_2010, FID_ERAS } FidEra;   // r_era 0..3 (4 custom = modern)
typedef enum FidAvail { FID_UNAVAILABLE, FID_DEFAULT_OFF, FID_DEFAULT_ON } FidAvail;
typedef enum FidKind { FID_BOOL, FID_INT, FID_FLOAT } FidKind;

// Features (one cvar each). Order = the tables in docs/ENGINE_FIDELITY.md.
typedef enum FidFeature {
    FF_AA, FF_UPSCALER, FF_UPSCALE_RATIO, FF_RENDER_SCALE, FF_SHARPEN,
    FF_AO, FF_CONTACT_SHADOWS, FF_SSR, FF_SSGI, FF_PUDDLE_REFLECTIONS, FF_VOLUMETRICS, FF_VOL_LOCAL_LIGHTS,
    FF_MOTION_BLUR, FF_DOF, FF_DOF_QUALITY, FF_ADS_DOF,
    FF_BLOOM, FF_BLOOM_QUALITY, FF_LENS_FLARE, FF_LENS_DIRT, FF_CHROMATIC, FF_LENS_DISTORTION, FF_LENS_DROPS,
    FF_VIGNETTE, FF_FILM_GRAIN, FF_AUTO_EXPOSURE, FF_TONEMAPPER, FF_HDR_OUTPUT,
    FF_SHADOW_QUALITY, FF_SHADOW_CASCADES, FF_SHADOW_DISTANCE, FF_SHADOW_FILTER, FF_SHADOW_MIN_RADIUS,
    FF_LIGHT_SHADOWS, FF_LIGHT_SHADOW_QUALITY, FF_CLOUD_SHADOWS, FF_MOON_SHADOWS,
    FF_TEXTURE_QUALITY, FF_ANISO, FF_VGEO, FF_VGEO_ERROR_PX, FF_VGEO_SHADOW_ERROR_PX, FF_VGEO_LOD_PX,
    FF_FOLIAGE_DENSITY, FF_FOLIAGE_WIND, FF_IMPOSTOR_PX, FF_LOD_BIAS, FF_CHAR_QUALITY, FF_SCOPE_PIP_SCALE,
    FF_VFX_QUALITY, FF_VFX_LIGHTING, FF_DECAL_MAX, FF_PRECIP_QUALITY, FF_WATER,
    FF_COUNT
} FidFeature;

typedef struct FidFeatureDef {
    const char *id;            // JSON key
    const char *cvar;
    FidKind kind;
    bool has_off;              // has an "off" value (unavailable forces it); otherwise unavailable only locks the control
    float off;
    float lo, hi;              // full range of the cvar
    bool cheaper_high;         // a higher value costs less (LOD error thresholds, impostor size, min shadow caster)
    bool era_owned;            // the era preset (r_era) sets the value; levels give bounds / a scale of it
    const char *label;
} FidFeatureDef;

#define FID_INT_BASE (-1)      // FidBounds.allowed bit (v - FID_INT_BASE): ints / bools -1 .. 30

typedef struct FidBounds {
    FidAvail avail;
    float def;                 // default value at this level (era-owned: from the era value)
    float lo, hi;              // allowed range
    u32 allowed;               // ints / bools: allowed values (bit v + 1)
    bool era_lacks;            // the era has no such feature (the level's entry is ignored)
} FidBounds;

// ---------------------------------------------------------------- registry / data
int fid_feature_count(void);
const FidFeatureDef *fid_feature(int f);
int fid_feature_find(const char *id);
int fid_feature_by_cvar(const char *cvar);
const char *fid_level_id(int level);          // "off" .. "modern"
const char *fid_level_name(int level);        // "Off" .. "Modern"
const char *fid_level_desc(int level);        // one line (JSON "description")
float fid_level_budget_ms(int level);         // 1080p mid-range GPU budget
bool fid_level_post(int level);               // false: no post-processing (direct output)
const char *fid_era_id(int era);              // "modern", "2020", "2015", "2010"
int fid_level_parse(const char *s);           // name / id / index -> level, -1
int fid_era_from_cvar(int r_era);             // r_era (4 = custom) -> FidEra

// Loading. Every level needs an entry for every feature (fid_tables_complete).
void fid_reset_tables(void);
bool fid_load_level_text(int level, const char *json, char *err, int err_size);
bool fid_load_eras_text(const char *json, char *err, int err_size);
bool fid_load_dir(const char *dir, char *err, int err_size);   // dir/{off,low,medium,high,modern,eras}.json
bool fid_tables_complete(char *err, int err_size);              // entries + defaults inside bounds + monotonic levels
bool fid_loaded(void);
// Era-owned features take their default from the era preset (vk_vgeo.c registers its table).
void fid_set_era_value(int era, int feature, float value);
float fid_era_value(int era, int feature);

// ---------------------------------------------------------------- bounds
FidBounds fid_bounds(int level, int era, int feature);
bool fid_allowed(const FidBounds *b, const FidFeatureDef *d, float v);
float fid_clamp(const FidBounds *b, const FidFeatureDef *d, float v);
bool fid_value_allowed(int level, int era, int feature, float v);
int fid_min_level_for(int era, int feature, float v);   // lowest level allowing v, -1 = none (the era lacks it)
// Cost order of two values of a feature: < 0 when a is cheaper than b.
int fid_cost_cmp(const FidFeatureDef *d, float a, float b);

// ---------------------------------------------------------------- runtime (cvars)
// The level / era whose bounds the cvar hooks enforce. The era is read from r_era when it exists.
void fid_attach_cvars(void);                  // install cvar.bound on every feature cvar (after registration)
void fid_detach_cvars(void);
void fid_set_level(int level);                // enforce this level's bounds (no defaults applied)
int fid_level(void);
int fid_era(void);
// Change level: every feature takes the new level's default, except the user's own choices that are
// below the new default (explicitly lower), which are kept (clamped to the new bounds). Unavailable
// features are forced off.
void fid_apply(int level);
void fid_enforce_all(void);                   // clamp every feature cvar to the current bounds
float fid_clamp_current(int feature, float v);   // what a set of v would store now
float fid_cvar_value(int feature);            // current value (NAN when the cvar is missing)
// User choices: what the user set away from a level default (menu, console, post presets) is
// remembered per feature and re-applied by fid_apply where it is below the new level's default.
// Persisted as r_fidelity_custom ("ssr=1;motion_blur=0"); tracking is off while the config loads.
void fid_set_tracking(bool on);
void fid_custom_string(char *out, int size);
void fid_custom_load(const char *s);
bool fid_user_value(int feature, float *v);
// Diagnostics: features whose cvar differs from the current level's default (user tuning).
int fid_custom_count(void);
int fid_check_table(int level, char *report, int size);   // features whose cvar != the table's default (0 = match)

// ---------------------------------------------------------------- frame graph passes
// Scene / gameplay passes (geometry, sky, sights, phone, UI, output) vs post-processing passes.
bool fid_pass_is_post(const char *name);

#endif
