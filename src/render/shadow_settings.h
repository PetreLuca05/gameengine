// Unity-style shadow controls (docs/SHADOWS_UNITY_MODEL.md): the knobs artists, level data, the
// console and the Settings menu use, with Unity's (URP / Built-in) behaviour behind them.
//
//   per light      Shadow Type None / Hard / Soft, Resolution (or "Use Quality Settings"),
//                  Strength, Bias, Normal Bias, Near Plane, Culling Mask, distance override
//   per renderer   Cast Shadows Off / On / Two Sided / Shadows Only, Receive Shadows
//   quality        Shadows Disable / Hard Only / Hard and Soft, Shadow Resolution, Shadow
//                  Projection Close / Stable Fit, Shadow Distance, Shadowmask Mode (stub), Near
//                  Plane Offset, Cascades No / Two / Four + split percentages, Cascade Border,
//                  Soft Shadow Quality Low / Medium / High (URP tent 3x3 / 5x5 / 7x7) / Ultra (PCSS)
//
// Underneath, "Soft" is the existing sky-driven implementation (shadow_sky.c: the light size, the
// fade under overcast) and the cascades are shadow_csm.c's; the sky only modulates a Soft light's
// size, never its type. Pure: no renderer (tests/test_shadow_settings.c).
#ifndef DG_SHADOW_SETTINGS_H
#define DG_SHADOW_SETTINGS_H

#include "core/base.h"

typedef enum ShadowType { SHADOW_NONE, SHADOW_HARD, SHADOW_SOFT, SHADOW_TYPE_COUNT } ShadowType;
// Resolution tiers; SHADOW_RES_QUALITY = "Use Quality Settings".
typedef enum ShadowRes { SHADOW_RES_QUALITY = -1, SHADOW_RES_LOW, SHADOW_RES_MEDIUM, SHADOW_RES_HIGH, SHADOW_RES_VERY_HIGH, SHADOW_RES_COUNT } ShadowRes;
typedef enum ShadowsMode { SHADOWS_DISABLE, SHADOWS_HARD_ONLY, SHADOWS_HARD_AND_SOFT, SHADOWS_MODE_COUNT } ShadowsMode;
typedef enum SoftQuality { SOFT_LOW, SOFT_MEDIUM, SOFT_HIGH, SOFT_ULTRA, SOFT_QUALITY_COUNT } SoftQuality;
typedef enum ShadowProjection { SHADOW_CLOSE_FIT, SHADOW_STABLE_FIT, SHADOW_PROJECTION_COUNT } ShadowProjection;
typedef enum ShadowmaskMode { SHADOWMASK_SHADOWMASK, SHADOWMASK_DISTANCE, SHADOWMASK_MODE_COUNT } ShadowmaskMode;
typedef enum CastShadows { CAST_SHADOWS_OFF, CAST_SHADOWS_ON, CAST_SHADOWS_TWO_SIDED, CAST_SHADOWS_ONLY, CAST_SHADOWS_COUNT } CastShadows;
typedef enum ShadowLightKind { SHADOW_LIGHT_DIRECTIONAL, SHADOW_LIGHT_SPOT, SHADOW_LIGHT_POINT } ShadowLightKind;

#define SHADOW_LAYERS 8              // culling mask bits (layer 0 = Default)
#define SHADOW_LAYERS_ALL 0xFFu

// A light's shadow settings (Unity's Light component, Shadows section).
typedef struct LightShadowSettings {
    ShadowType type;
    int resolution;                  // ShadowRes (SHADOW_RES_QUALITY: the quality's)
    float strength;                  // 0..1: 0 = no darkening, 1 = full shadow (blended towards lit)
    float bias;                      // 0..2: constant depth bias, 1 = the engine default (0.75 texel)
    float normal_bias;               // 0..3: normal offset, 1 = the engine default (1.5 texels x sin(n, l))
    float near_plane;                // m, spot / point projections (0.1 .. 10)
    u32 culling_mask;                // layers whose renderers cast this light's shadows
    float distance;                  // m, spot / point: shadowed within this distance of the camera (0 = no limit)
} LightShadowSettings;

// Quality settings (Unity's Quality panel, Shadows section); Engine Fidelity sets defaults / bounds.
typedef struct QualityShadowSettings {
    ShadowsMode mode;
    ShadowRes resolution;            // for lights set to "Use Quality Settings"
    ShadowProjection projection;
    float distance;                  // m: where the last cascade ends (it fades over the border)
    ShadowmaskMode shadowmask;       // stub: no baked data yet
    float shadowmask_distance;       // m: the distance-shadowmask cut-over (stub)
    float near_plane_offset;         // m: how far the light box reaches towards the light beyond the receivers
    int cascades;                    // 1, 2 or 4
    float split2;                    // 0..1: the two-cascade split (fraction of the distance)
    float split4[3];                 // 0..1, increasing: the four-cascade splits
    float border;                    // 0..0.5: blend band at the end of each cascade (fraction of its range)
    SoftQuality soft_quality;
} QualityShadowSettings;

void shs_light_default(LightShadowSettings *s, ShadowLightKind kind);
void shs_quality_default(QualityShadowSettings *q);         // Unity's Quality defaults (= the Modern tier)
void shs_quality_for_tier(QualityShadowSettings *q, int fid_level);   // FID_OFF .. FID_MODERN (fidelity.h)
// Clamps every field to its range (splits sorted, cascades 1 / 2 / 4).
void shs_quality_sanitize(QualityShadowSettings *q);
void shs_light_sanitize(LightShadowSettings *s);

// ---- behaviour
// The type a light renders with under the quality cap (Hard Only turns Soft into Hard).
ShadowType shs_effective_type(ShadowType light, ShadowsMode mode);
// Shadow map texels: directional cascades / the viewmodel 512 .. 4096, spot and point faces
// (URP additional-light tiers) 256 .. 2048.
u32 shs_res_texels(int res, ShadowLightKind kind);
u32 shs_light_texels(const LightShadowSettings *s, const QualityShadowSettings *q, ShadowLightKind kind);

// Filter of one light (lighting.glsl sh_filter_mode): 0 none, 1 hard (one bilinear compare
// fetch), 2 tent of `tent` texels (URP 3 / 5 / 7), 3 PCSS (Ultra, contact hardening).
enum { SHF_NONE, SHF_HARD, SHF_TENT, SHF_PCSS };
typedef struct ShadowFilter { int mode; float tent; } ShadowFilter;
ShadowFilter shs_filter(ShadowType effective, SoftQuality q, ShadowLightKind kind);
float shs_tent_texels(SoftQuality q);                         // 3, 5, 7 (Ultra: 7 for local lights)
int shs_tent_fetches(float tent_texels);                      // bilinear fetches per axis: ceil((n + 1) / 2)
// One axis of the tent filter (lighting.glsl sh_tent_axis): tent of base `n` texels centred at
// `c` (texel space, texel i covers [i, i + 1]); bilinear fetch k at texel-space coordinate pos[k]
// (between two texel centres) with weight w[k]. Returns the fetch count (<= 4). Weights sum to 1.
int shs_tent_axis(float c, float n, float pos[4], float w[4]);

// Strength: the shadow term s (1 lit .. 0 shadowed) blended towards lit.
float shs_apply_strength(float s, float strength);
// Bias in metres for a texel of texel_m (low_sun 1 .. 3, shadow_csm.c csm_low_sun).
void shs_bias_m(float texel_m, float bias, float normal_bias, float low_sun, float *const_m, float *normal_m);

// Cascade selection distances (m) from the quality: count and splits[count] (the last = distance).
int shs_cascade_splits(const QualityShadowSettings *q, float splits[4]);

// Renderer flags (Unity's Cast Shadows / Receive Shadows).
bool shs_casts(CastShadows c);                                // into shadow maps
bool shs_visible(CastShadows c);                              // in the camera passes
bool shs_two_sided_caster(CastShadows c, bool material_two_sided);   // no back-face culling in the shadow pass

// ---- names (level JSON, console, settings round trip); -1 / false on an unknown name
const char *shs_type_name(ShadowType t);
int shs_type_from_name(const char *s);
const char *shs_res_name(int r);
int shs_res_from_name(const char *s, int *out);              // "use_quality" | "low" | "medium" | "high" | "very_high"
const char *shs_cast_name(CastShadows c);
int shs_cast_from_name(const char *s);
const char *shs_mode_name(ShadowsMode m);
const char *shs_soft_name(SoftQuality q);
const char *shs_projection_name(ShadowProjection p);

// Sets one light field by name ("type", "resolution", "strength", "bias", "normal_bias",
// "near_plane", "culling_mask", "distance"); false + err on an unknown key or value.
bool shs_light_set(LightShadowSettings *s, const char *key, const char *value, char *err, int err_size);
// "type soft resolution use_quality strength 1.00 ..." (the console / readout form; parses back with shs_light_set).
int shs_light_describe(const LightShadowSettings *s, char *buf, int size);

// ---- local light atlas (spot tiles, point cube faces): power-of-two squares in a square atlas,
// buddy (quadtree) allocation in priority order.
#define SHS_ATLAS_MAX 160
typedef struct ShsAtlasReq { u32 size; int light; } ShsAtlasReq;   // consecutive requests of one light = a group (6 cube faces)
typedef struct ShsAtlasRect { u32 x, y, size; } ShsAtlasRect;
// Places the requests in order (the caller sorts by priority). A group is all or nothing: if it
// does not fit it is retried one size smaller (every face) down to min_size, then dropped (rect
// size 0 for each of its requests). Returns the number of placed requests.
int shs_atlas_pack(u32 atlas, u32 min_size, const ShsAtlasReq *req, int n, ShsAtlasRect *out);

#endif
