// Terrain shading parameters (docs/TERRAIN_SHADING.md): assets/terrain/terrain.json (global: slots,
// macro, scatter, distances, snow line), assets/terrain/layers/<name>.json (one per terrain material
// slot: textures, tiling, variation, blending, variants, detail, weather response) and
// assets/terrain/transitions/*.json (per material pair, family defaults + overrides), parsed into the
// GPU block the terrain shader reads (shaders/terrain/terrain_params.glsl, std430, vec4 only) plus the
// list of textures the renderer loads into its arrays. Pure: no renderer (tests/test_terrain.c).
#ifndef DG_TERRAIN_PARAMS_H
#define DG_TERRAIN_PARAMS_H

#include "core/base.h"

#define TER_SLOTS 16                 // terrain material slots (MgMaterial ids, the material raster)
#define TER_MAX_TEX 48               // textures in the layer arrays (layers, variants, transition sets)
#define TER_MAX_PAIRS 64
#define TER_DIR "assets/terrain"
#define TER_TIERS 5                  // fidelity levels Off .. Modern (r_terrain_quality)

// Pair mode bits (TerPairGpu.b.w): which transition techniques a pair uses.
enum TerPairMode {
    TPM_HEIGHT = 1,                  // height-based (the taller material wins inside the band)
    TPM_NOISE = 2,                   // noise-broken border (multi-octave world noise)
    TPM_CRUMBS = 4,                  // crumbs of one material inside the other
    TPM_SET = 8,                     // a transition texture set inside the band
    TPM_SCATTER = 16,                // border scatter sprites
    TPM_DIRECTIONAL = 32,            // drifts downwind (wind direction) / downhill
    TPM_SLOPE = 64,                  // the first material spills downhill; steep slopes expose it
    TPM_WEAR = 128,                  // wear mask (paths): packed centre -> worn edge
    TPM_MOISTURE = 256,              // moisture moves the border (grass -> mud)
    TPM_RIM = 512,                   // a rim texture on the second material's side (crater lips)
};

// Variant mask sources (TerLayerGpu.var*.y).
enum TerMaskSource { TMS_NOISE, TMS_MOISTURE, TMS_CURVATURE, TMS_SLOPE, TMS_PAINT, TMS_DRY };

// Tiling modes (TerLayerGpu.tiling.x; the level caps them: Off / Low cells, Medium+ hex).
enum TerTiling { TT_PLAIN, TT_CELLS, TT_HEX };

typedef struct TerLayerGpu {
    float tex[4];      // albedo / nrh array layer, macro array layer, 1 / tile (m), 1 / macro scale (m)
    float tiling[4];   // mode (TerTiling), hex cell size (tiles), scale jitter, rotation amount (0..1 of a turn)
    float multi[4];    // far scale 1, far scale 2, multi-scale strength, distance where the uv scale halves (m)
    float hsv[4];      // hue jitter (rad), saturation jitter, value jitter, 1 / jitter noise scale (m)
    float blend[4];    // height_contrast, blend_sharpness, height_offset, porosity
    float curv[4];     // cavity darkening, ridge lightening, moisture darkening, baked AO strength
    float moist[4];    // moisture tint (rgb multiplier), amount
    float slope[4];    // slope tint (rgb multiplier), amount
    float ranges[4];   // slope tint from n.y = x to y; altitude tint from z to w (m)
    float alt[4];      // altitude tint (rgb multiplier), amount
    float aspect[4];   // north-facing tint (rgb multiplier), amount
    float var0[4];     // variant 0: array layer (-1 none), mask source (TerMaskSource), lo, hi
    float var1[4];     // variant 1
    float varn[4];     // variant mask noise 1 / scale (m), noise weight, macro strength, 0
    float detail[4];   // parallax depth (m), detail normal strength, micro roughness, normal strength
    float snow[4];     // snow accumulation x, wet darkening x, rock exposure slope (n.y below), exposure amount
    float scatter[4];  // sprites per m2, cell size (m), sprite mask (bits of the scatter atlas cells), sprite scale (m)
    float mean[4];     // mean albedo (linear rgb), base roughness
} TerLayerGpu;

typedef struct TerPairGpu {
    float a[4];        // band width (m), edge noise scale (m), edge noise strength (m), crumbs (0..1)
    float b[4];        // set array layer (-1 none), set strength, 1 / set tile (m), mode bits (TerPairMode)
    float c[4];        // direction: 0 none 1 wind 2 downhill, its shift (m), slope bias (m), wear bias (m)
    float d[4];        // moisture bias (m), rim array layer (-1), rim width (m), tint_by_weight
    float e[4];        // scatter mask bits, scatter density (per m2), puddle bias, snow bias
} TerPairGpu;

typedef struct TerGlobalGpu {
    float tier[4];     // level (0..4), debug view, time (s), blend map radius (m)
    float dist[4];     // hex / multi-scale far end, parallax end, scatter end, detail normal end (m)
    float macro[4];    // macro texture strength, crumb size (m), slope-tint strength, curvature strength
    float snow[4];     // snow line altitude (m), band width (m), band noise (m), temperature shift (m / degree)
    float wind[4];     // wind direction x, z (unit), 0, 0
    float grass[4];    // grass term start, end (m), strength, 0
    float maps[4];     // terrain maps: origin x, z, 1 / spacing, samples per side (0: none)
    float scatter[4];  // scatter atlas cells x, y, 0, 0
} TerGlobalGpu;

typedef struct TerrainGpu {
    TerGlobalGpu g;
    TerLayerGpu layer[TER_SLOTS];
    TerPairGpu pair[TER_MAX_PAIRS];
    u32 pair_of[TER_SLOTS * TER_SLOTS];   // pair index of (a, b) (symmetric, 0 = the default pair)
} TerrainGpu;

typedef struct TerrainParams {
    TerrainGpu gpu;
    // Textures of the arrays, in array order: name in assets/terrain/tex (<name>_albedo.jpg / _nrh.png /
    // _ao.png / _macro.png) or "surf:<layer>" (assets/textures/surfaces, the old set: asphalt, concrete ...).
    char tex[TER_MAX_TEX][48];
    int tex_count;
    char slot_layer[TER_SLOTS][48];  // layer JSON per slot ("" = untextured slot)
    char pair_name[TER_MAX_PAIRS][64];
    int pair_count;
    float mean_albedo[TER_SLOTS][3]; // the slots' mean albedo (linear, from the main texture's meta.json)
    char scatter_atlas[48];
    float ground_macro[6];           // render_ground_set_macro: 1 / scale 1, 1 / scale 2 (m), hue (rad), value, saturation, strength
    float level_quality[TER_TIERS];  // r_terrain_quality per fidelity level (informative)
    char err[256];
} TerrainParams;

// Loads terrain.json, the layers and transitions under dir (TER_DIR). false + p->err on failure.
bool terrain_params_load(TerrainParams *p, const char *dir);
// The same from strings (tests): global JSON, a layer lookup (name -> JSON text, NULL = missing) and the
// transition JSON texts. Mean albedo comes from mean_of(name) (NULL: grey).
typedef const char *(*TerTextFn)(const char *name, void *user);
bool terrain_params_parse(TerrainParams *p, const char *global_json, TerTextFn layer_json, TerTextFn meta_json, const char *const *pair_json, int pair_count, void *user);
// The texture array index of a name (adding it), -1 when full.
int terrain_params_tex(TerrainParams *p, const char *name);
// Newest mtime of every file the parameters came from (hot reload).
i64 terrain_params_mtime(const char *dir);

#endif
