// LOD ranges and cross-fades (docs/LOD.md): the data-driven range table (assets/render/lod.json), the
// time-based cross-fade of objects with state (characters, their shadows) and the distance-band
// cross-fade of stateless instances (the classic renderer's LOD chains, impostors, foliage density,
// shadow distance ends). Pure C (cvars + cJSON): tests/test_lod.c.
//
// Every fade is a pair of draws with complementary dither masks (shaders/lod_fade.glsl): a signed
// fade value f, 0 = fully visible, (0, 1) fading in with coverage f, (-1, 0) fading out with coverage
// -f. The two draws of one fade use f_in = t and f_out = -(1 - t): coverage sums to 1.
//
// Applies to the classic renderer and the systems it shares (characters, terrain, VFX spawn LOD);
// with r_lod_legacy 1 those keep their behaviour from before docs/LOD.md (lod_active() false).
#ifndef DG_LOD_FADE_H
#define DG_LOD_FADE_H

#include "core/base.h"

#define LOD_CHAR_COUNT 4

// Range categories: a distance multiplier each (x the ranges before docs/LOD.md) x r_lod_bias (locked 1).
// The per-level "tiers" and per-era multipliers of lod.json must be 1: the ranges decide what a player
// can see, so they are the same at every Engine Fidelity level and era (docs/SETTINGS_AUDIT.md).
typedef enum LodCat {
    LODC_CHARACTER,   // skinned characters (the pixel table already holds the new targets: 1)
    LODC_STATIC,      // classic discrete LOD chains (props, level meshes, tree / bush LODs)
    LODC_IMPOSTOR,    // classic mesh <-> impostor switch
    LODC_FOLIAGE,     // foliage density fade ranges
    LODC_TERRAIN,     // terrain chunk LOD distances
    LODC_SHADOW,      // shadow caster distances (classic shadow_max_dist)
    LODC_VFX,         // particle spawn LOD distances
    LODC_COUNT
} LodCat;

typedef struct LodTable {
    float fade_time;                  // s: time-based cross-fades (r_lod_fade_time default)
    float band;                       // octaves: stateless cross-fade band (r_lod_fade_band default)
    float cat[LODC_COUNT];            // distance multiplier per category
    float tier[5];                    // per Engine Fidelity level (Off .. Modern)
    float era[4][LODC_COUNT];         // per-era multipliers (r_era 0..3)
    // characters
    float char_px[LOD_CHAR_COUNT - 1];   // projected height (px at bias 1): LOD0 above [0], LOD1 above [1], LOD2 above [2]
    float char_hysteresis;
    float char_cull_px, char_shadow_px;
    int char_update[LOD_CHAR_COUNT];     // pose update interval (ticks) per LOD
    float char_ramp_s;                   // s per octave of update interval change (no halving in one step)
    float char_ik_band;                  // fraction of the LOD0 threshold over which foot IK / lean fade
    // impostors
    float imp_band;                      // octaves of projected size over which mesh and impostor cross-fade
    float imp_gain;                      // impostor albedo gain matching the mesh's mean colour (classic)
    // foliage
    float fol_band;                      // x the fade band length (start .. end)
    float fol_instance;                  // per-instance fade width (fraction of the band; was 1/16)
    float fol_shrink;                    // a fading instance scales down to this fraction
    // terrain
    float ter_detail;                    // x the surface detail ranges (anti-tiling, triplanar)
    float ter_morph;                     // octaves of the geomorph band of each terrain LOD
    // shadows
    float shadow_band;                   // fraction of a shadow distance over which casters fade out
    // streaming
    float prefetch;                      // classic LODs are made resident this many octaves before use
} LodTable;

void lod_table_default(LodTable *t);   // the shipped values (as assets/render/lod.json)
bool lod_table_parse(LodTable *t, const char *json, char *err, int err_size);   // starts from the defaults

// Distance scale of a category (pure): tier multiplier x user bias x the category multiplier x the era's.
float lod_scale_of(const LodTable *t, LodCat c, int level, float user_bias, int era);

// Character LOD by projected height: thresholds = char_px / scale, hysteresis, one boundary per call.
int lod_char_select(const LodTable *t, float scale, int current, float pixels);

// ---------------------------------------------------------------- time-based cross-fade (stateful objects)
typedef struct LodFade {
    i8 cur, from;     // the LOD faded to / from (from == cur: settled)
    float t;          // 0..1 progress (1 = settled)
} LodFade;

void lod_fade_reset(LodFade *f, int lod);
// want: what the selector asks for this frame. A running fade always completes first (camera reversal
// mid-fade: the fade finishes, then a new one starts back): no ping-pong half-fades. fade_time <= 0: cuts.
void lod_fade_step(LodFade *f, int want, float dt, float fade_time);
bool lod_fade_active(const LodFade *f);
// The draws of the current state: 1 (lods[0], fade 0) or 2 (from fading out, cur fading in).
int lod_fade_draws(const LodFade *f, int lods[2], float fades[2]);
// Signed dither values of a cross-fade at progress t (0..1): in and out, never exactly 0 / +-1.
float lod_fade_in(float t);
float lod_fade_out(float t);

// ---------------------------------------------------------------- distance-band cross-fade (stateless)
// y: octaves past a switch threshold (>= 0: the coarser side). Weight of the coarser side over the band.
float lod_band_weight(float y, float band);
// Discrete LOD chain pick by projected error: err[i] (metres) x k (px per metre) <= px. Returns the
// finer LOD of the pair; *coarse_w the weight of lod + 1 (0: no fade).
int lod_pick_band(const float *err, int count, int first, float k, float px, float band, float *coarse_w);
// Coverage of one draw of a fade pair (for tests): in = t, out = 1 - t.
float lod_coverage(float f);
// The shader's test (shaders/lod_fade.glsl lod_faded_n): is a texel of threshold n in [0, 1) kept?
bool lod_kept(float f, float n);
// Classic instances: a kept interval [lo, hi) of the dither threshold (several draws of one instance
// split [0, 1): impostor / coarser / finer LOD), packed in one float (1/256 steps, classic_common.glsl);
// 0 = no interval (the whole coverage).
float lod_iv_pack(float lo, float hi);
bool lod_iv_kept(float packed, float n);
#define LOD_IV_FULL 131072.0f   // [0, 1): lod_iv_pack(0, 1) = 0 + 256 x 512

// Streaming: the LOD to draw when `want` may not be resident yet (bit i of ready: LOD i resident). The
// wanted LOD when ready; else the nearest coarser resident one (the lower LOD is held until the higher
// is in); else the nearest finer one; -1 when none is.
int lod_resident_pick(int want, u32 ready, int count);

// ---------------------------------------------------------------- terrain geomorph (chunks.c, lit.vert)
// Chunk LOD from the nearest point of the chunk (flat distance): floor(log2(near / lod_d)), 0..3.
int lod_terrain_level(float near_dist, float lod_d);
// Morph of a vertex at flat distance dist in a chunk of `level` towards level + 1 (0 = own grid, 1 =
// the coarser grid). Starts `band` octaves before the next level, never before the distance where a
// neighbour chunk diag metres away may still draw this level unmorphed: crack-free, pop-free.
float lod_terrain_morph(float dist, int level, float lod_d, float band, float diag);

// ---------------------------------------------------------------- runtime (cvars, the loaded table)
void lod_init(void);                       // cvars + assets/render/lod.json (after the render cvars)
bool lod_load(const char *path);           // reload (r_lod_reload)
const LodTable *lod_table(void);
bool lod_active(void);                     // the new ranges / fades apply (not r_lod_legacy)
float lod_scale(LodCat c);                 // tier x user bias x category x era, 1 when !lod_active()
float lod_user_bias(void);                 // r_lod_bias / the level's default (1 untouched)
float lod_fade_time(void);                 // r_lod_fade_time
float lod_band(void);                      // r_lod_fade_band
int lod_fade_debug(void);                  // r_lod_fade_debug
bool lod_scope(void);                      // r_lod_scope: LOD selection of the scope PiP view (both renderers)
int lod_debug(void);                       // r_lod_debug

#endif
