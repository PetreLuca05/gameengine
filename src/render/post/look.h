// Post-processing Looks (docs/LOOKS.md): five fixed colour-grading / finishing presets (Realism,
// PUBG-style, Cinematic, Vibrant, Gritty), independent of Engine Fidelity (what runs) and of the
// era (geometry). A Look only changes how the image is graded and finished.
//
// The grading stack, HDR scene-linear (exposed) -> display-linear, in this order:
//   1. exposure compensation (EV), white balance (temperature / tint)
//   2. tonal (log2 around mid grey): pivot contrast, lift / gamma / gain per channel on soft
//      shadow / midtone / highlight masks, highlight roll-off
//   3. colour: saturation, vibrance (skin protection), hue-vs-hue, hue-vs-sat, sat-vs-sat,
//      colour balance per range, split-toning
//   4. tonemapper (ACES fit, AgX, Khronos PBR Neutral, filmic with toe / shoulder)
//   5. the preset's creative LUT (procedural, compiled in: print film / bleach bypass) x strength
//   6. output levels (black point with its tint, white point)
// Stages 1-6 are baked once into a 32^3 LUT per look (log2 shaper, look_lut_coord); the tonemap
// pass replaces its tonemapper with one LUT fetch. The finish (bloom intensity / threshold / tint,
// vignette, grain, chromatic aberration, sharpening, halation, letterbox) and the gameplay
// feedback colours are PostSettings fields (look_apply_post), bounded per Engine Fidelity level.
// Modulators (time of day, weather) move the live grade uniforms (look_modulate / look_apply_mod).
//
// The five tables are static const C (looks.c). Nothing reads or writes a look from disk.
// Dev builds (DG_LOOK_TUNE) can override the constants live (look_tune) and print a table back as
// C (look_dump / look_format) to paste into looks.c. Pure C: tests/test_looks.c.
#ifndef DG_LOOK_H
#define DG_LOOK_H

#include "core/dgmath.h"
#include "render/post/post_settings.h"
#include <stddef.h>

#ifndef DG_LOOK_TUNE
#define DG_LOOK_TUNE 0
#endif

typedef enum LookId { LOOK_REALISM, LOOK_PUBG, LOOK_CINEMATIC, LOOK_VIBRANT, LOOK_GRITTY, LOOK_COUNT } LookId;
typedef enum LookTonemap { LOOK_TM_ACES, LOOK_TM_AGX, LOOK_TM_NEUTRAL, LOOK_TM_FILMIC } LookTonemap;
typedef enum LookLut { LOOK_LUT_NONE, LOOK_LUT_PRINT, LOOK_LUT_BLEACH } LookLut;

#define LOOK_HUE_KNOTS 8          // hue curves: a knot every 45 deg of opponent hue (0 red, 45 orange, 90 yellow-green,
                                  // 135 green, 180 cyan, 225 sky blue, 270 blue-violet, 315 magenta), periodic
#define LOOK_SAT_KNOTS 5          // sat-vs-sat: input saturation 0, 0.25, 0.5, 0.75, 1
#ifndef LOOK_LUT_N
#define LOOK_LUT_N 32             // baked LUT size (per axis); shaders/post/look.glsl LOOK_LUT_N
#endif
#define LOOK_EV_MIN (-11.0f)      // LUT shaper: log2(c / 0.18) from LOOK_EV_MIN .. LOOK_EV_MAX (shaders/post/look.glsl)
#define LOOK_EV_MAX 7.0f

// A modulator's effect at full strength (golden hour, overcast, storm, night). temp / tint / exposure
// add; sat / contrast / bloom / grain scale by (1 + value); vignette adds.
typedef struct LookModSet { float temp, tint, exposure, sat, contrast, bloom, grain, vignette; } LookModSet;

typedef struct LookParams {
    const char *id, *name, *desc;
    // 1. exposure / white balance
    float exposure_ev;
    float wb_temp, wb_tint;                  // -1..1, the shader's white_balance model (tint > 0 = magenta)
    // 2. tonal (EV = stops from mid grey 0.18)
    float contrast, pivot_ev;
    vec3 lift, gamma, gain;                  // EV offsets per channel on the shadow / midtone / highlight masks
    float shadow_end, highlight_start;       // EV where the shadow mask has faded out / the highlight mask starts
    float range_soft;                        // EV width of the mask transitions
    float rolloff, rolloff_start;            // highlight roll-off strength (0 none) above rolloff_start EV
    // 3. colour
    float saturation, vibrance, skin_protect;   // vibrance -1..1; skin_protect 0..1 spares orange hues
    float hue_hue[LOOK_HUE_KNOTS];           // degrees of hue rotation at each knot
    float hue_sat[LOOK_HUE_KNOTS];           // saturation multiplier at each knot
    float sat_sat[LOOK_SAT_KNOTS];           // saturation multiplier by input saturation
    vec3 bal_shadows, bal_mids, bal_highs;   // colour balance per range (luma-neutral chroma push)
    float split_shadow_hue, split_shadow_amt, split_high_hue, split_high_amt, split_balance;   // hue in degrees, balance in EV
    // 4. tonemapper
    int tonemapper;                          // LookTonemap
    float tm_toe, tm_shoulder;               // filmic only: toe / shoulder strength (Hable D / A)
    // 5. creative LUT
    int lut;                                 // LookLut
    float lut_strength;
    // 6. output levels (display-linear)
    float black_point, white_point;
    vec3 black_tint;                         // colour of the lifted black (x black_point)
    // finish (PostSettings, bounded by Engine Fidelity)
    float bloom, bloom_threshold;            // intensity (energy conserving mix), threshold (exposed scene units, 0 = none)
    vec3 bloom_tint;
    float vignette, grain, chromatic, sharpen, halation;
    int letterbox;                           // 2.39:1 bars
    // gameplay feedback colours (damage / low health keep their behaviour; the look sets the colours)
    vec3 damage_color, lowhealth_color;
    // modulators
    LookModSet golden, overcast, storm, night;
} LookParams;

// Conditions for the modulators (game_look.c from the sky and the weather).
typedef struct LookEnv {
    float sun_elev_deg;     // sun elevation (degrees)
    float daylight;         // 0 night .. 1 day
    float cloud;            // cloud cover 0..1
    float precip;           // precipitation 0..1
    float lightning;        // 0..1
    float wind;             // m/s
} LookEnv;

typedef struct LookMod { float temp, tint, exposure, sat_mul, contrast_mul, bloom_mul, grain_mul, vignette_add; } LookMod;

// The user's strength of each finish effect: its cvar / the cvar's default (0 = off, also when
// Engine Fidelity makes the feature unavailable).
typedef struct LookUser { float bloom, vignette, grain, chromatic, sharpen; } LookUser;

// Engine Fidelity caps of the finish (index = FidLevel: Off, Low, Medium, High, Modern).
typedef struct LookCaps { float bloom, grain, chromatic, halation, vignette, sharpen; int letterbox; } LookCaps;

typedef struct LookFinish {
    float bloom, bloom_threshold, vignette, grain, chromatic, sharpen, halation;
    vec3 bloom_tint;
    int letterbox;
} LookFinish;

// ---------------------------------------------------------------- registry (looks.c)
const LookParams *look_preset(int id);   // the compiled-in constants (NULL out of range)
const LookParams *look_get(int id);      // what renders: the constants, or a dev override (look_tune)
int look_find(const char *name);         // id or display name, case-insensitive; also 0..4; -1 = none
const char *look_id_name(int id);
const char *look_display_name(int id);
const char *look_desc(int id);

// ---------------------------------------------------------------- grading (look.c)
vec3 look_tonemap(int tonemapper, float toe, float shoulder, vec3 c);   // scene-linear -> display-linear 0..1
vec3 look_eval(const LookParams *p, vec3 c);   // the analytic stack 1-6 (exposed scene-linear -> display-linear)
void look_bake_lut(const LookParams *p, float *rgb);   // LOOK_LUT_N^3 x rgb, r fastest
vec3 look_lut_coord(vec3 c);                   // shaper: scene-linear -> 0..1 LUT coordinates
vec3 look_lut_sample(const float *rgb, vec3 c);   // CPU trilinear fetch (the GPU sampler's math)
float look_hue_deg(vec3 c);                    // opponent hue (0 red, 120 green, 240 blue)
float look_saturation(vec3 c);                 // (max - min) / max

// ---------------------------------------------------------------- finish / modulators
const LookCaps *look_caps(int fid_level);
void look_finish(const LookParams *p, const LookUser *u, int fid_level, LookFinish *out);
void look_modulate(const LookParams *p, const LookEnv *env, LookMod *out);
// Look fields of PostSettings: look_lut, the finish, the feedback colours. Nothing else is touched.
void look_apply_post(PostSettings *s, int id, const LookParams *p, const LookUser *u, int fid_level);
// The modulators on the live grade (wb, exposure, saturation, contrast, bloom, grain, vignette).
void look_apply_mod(PostSettings *s, const LookMod *m);

// ---------------------------------------------------------------- dev tuning (DG_LOOK_TUNE)
bool look_tune_compiled(void);   // false in release builds (no look_tune / look_dump code at all)
#if DG_LOOK_TUNE
typedef struct LookField { const char *name; size_t offset; int count; bool is_int; } LookField;
const LookField *look_fields(int *count);
LookParams *look_tune_edit(int id);       // a writable override of look id (created from the constants)
void look_tune_reset(int id);             // -1 = all
bool look_tune_dirty(int id);
int look_format(const LookParams *p, char *buf, size_t n);   // the table as a C initializer (for looks.c)
#endif

#endif
