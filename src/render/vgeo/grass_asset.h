// Grass and ground-cover assets (docs/GROUND_REFERENCE.md 7.1, docs/FOLIAGE_GRASS.md): one JSON per
// asset in assets/foliage/grass/ describes the clump (crossed, bent cards that each show one cell of the
// asset's atlas), where it grows (clumps per m2 per terrain material, clumping noise), its shading
// (root darkening, colour variation, ground-colour matching, translucency, specular, clump AO), its
// fades / shadows per fidelity level and its wind. Pure: no renderer (tests/test_grass.c).
//
// Atlas: assets/foliage/tex/<atlas>_albedo.png (sRGB colour, coverage alpha) and <atlas>_nta.png (normal
// xy, thickness, AO), cols x rows cells, blades rising from each cell's bottom edge (tools/vfxgen/groundgen.py).
//
// Mesh vertex colour (canonical wind layout, foliage_wind.h): R = height along the card (0 root .. 1 tip:
// the flutter weight and the shading's root -> tip parameter), G = wind phase of the card, B = 0 (no
// branches), A = the card's hash (per-card colour variation).
#ifndef DG_GRASS_ASSET_H
#define DG_GRASS_ASSET_H

#include "core/base.h"
#include "vgeo_procgen_foliage.h"

#define GRASS_MAX_ASSETS 8           // the cover id is 3 bits of the material flags (MAT_COVER_ID)
#define GRASS_MAX_CELLS 8
#define GRASS_MAX_MATERIALS 16       // terrain materials (MgMaterial ids) with a density
#define GRASS_TIERS 5                // fidelity levels Off .. Modern
#define GRASS_DIR "assets/foliage/grass"

typedef struct GrassTier {
    float full_m;                    // full density to here
    float end_m;                     // dithered thinning to nothing here (material fade_start / fade_end)
    float shadow_m;                  // sun shadow casting distance (0: none)
    float density;                   // x the density table (the fidelity level's own cut on top of r_foliage_density)
} GrassTier;

typedef struct GrassAsset {
    char name[48];
    char atlas[48];
    int cols, rows;                  // atlas cells
    int variants;                    // meshes (different seeds) per asset
    // clump
    int cards;                       // crossed cards per clump
    int segments;                    // vertical segments per card (bend resolution)
    int cells[GRASS_MAX_CELLS], cell_count;   // atlas cells the cards pick from
    float height_m[2], width_m[2];   // card size range
    float fan_deg;                   // outward lean of the cards (deg)
    float lean;                      // random lean (tan)
    float bend_m;                    // random tip bend (m)
    float base_jitter_m;             // card base offsets
    float normal_dome;               // vertex normals blended toward the clump dome (soft clump lighting)
    float mirror;                    // chance a card shows its cell mirrored
    // placement
    float density[GRASS_MAX_MATERIALS];   // clumps per m2 at full density (0 = none)
    float clump_scale_m, clump_amount, bare;   // clumping noise: patch size, how much it modulates, bare fraction
    float scale[2];                  // instance scale range
    float slope_max;                 // tan of the steepest ground it grows on
    // shading (MAT_COVER, frame cover table)
    float roughness, specular;       // specular: x F0 (0.04)
    float sss, sss_wrap;             // translucency strength, diffuse wrap
    float alpha_cutoff;
    float root_dark, root_extent;    // albedo x root_dark at the root, fading out over root_extent (0..1 of the height)
    float ground_match, ground_extent;   // lower blade mixed toward the ground colour
    float var_hue, var_sat, var_val; // per clump / card variation (deg, fraction, fraction)
    float macro;                     // strength of the shared macro variation (terrain macro noise)
    float ao_root;                   // clump AO at the root (1 at 40 % height)
    float ao_strength;               // weight of the screen-space AO (render/material_ao.h; 0.3, 0 in JSON = none)
    int ao_combine;                  // MaterialAoCombine of the clump AO against it ("min" default, "multiply")
    // terrain grass term beyond the fade (shaders/terrain): the grass tops' colour and how much of the
    // ground they cover at full density
    float top_color[3];              // linear
    float coverage;
    GrassTier tier[GRASS_TIERS];
    int shadow_class;                // "shadow_class": the sun shadow caster class (render/shadow_casters.h; default "ground")
    // wind (foliage_wind.h grass preset, scaled)
    float wind_main, wind_detail, wind_flutter;
} GrassAsset;

// Defaults, then the JSON over them. false (log) on a parse error or a missing atlas name.
bool grass_asset_parse(GrassAsset *a, const char *json, const char *name);
bool grass_asset_load(GrassAsset *a, const char *path, const char *name);
// Every assets/foliage/grass/*.json (sorted by name, at most max). Returns the count.
int grass_assets_load_dir(GrassAsset *out, int max);

// Card fitting (PF-0016, docs/FOLIAGE_GRASS.md "Fitted cards"): per atlas cell and card band (one per
// segment, root first) the part of the cell's width (0..1) holding every texel whose alpha could pass the
// test down to mip level GRASS_FIT_LEVELS - 1: the texels with any alpha, grown by 2 texels of that level.
#define GRASS_FIT_BANDS 16
#define GRASS_FIT_LEVELS 3
typedef struct GrassFit {
    bool valid;
    int bands;
    float l[GRASS_MAX_CELLS * GRASS_MAX_CELLS][GRASS_FIT_BANDS], r[GRASS_MAX_CELLS * GRASS_MAX_CELLS][GRASS_FIT_BANDS];
} GrassFit;
// From the atlas as uploaded (RGBA8, w x h, the asset's cols x rows cells).
void grass_fit_compute(const GrassAsset *a, const u8 *rgba, u32 w, u32 h, GrassFit *f);

// The clump mesh of variant v (deterministic). Card uvs address the atlas cells. fit (NULL: full cards)
// narrows every card to its cell's opaque part per band: the same triangles, plane and uv mapping, so
// the pixels that stay are unchanged and the ones cut away were transparent.
void grass_gen_mesh(VgFoliageMesh *m, const GrassAsset *a, u32 variant, const GrassFit *fit);
// Two unreferenced vertices at full's bounds appended to fitted: the vgeo build quantizes positions on a
// grid of the mesh's extent, so the fitted card's vertices land on the full card's grid points.
void grass_mesh_match_bounds(VgFoliageMesh *fitted, const VgFoliageMesh *full);
// Mesh bounds the generator can reach (m): max height, max horizontal radius.
void grass_mesh_extent(const GrassAsset *a, float *height, float *radius);
// The asset's material for fidelity level tier (0 Off .. 4 Modern): flags, cutoff, roughness, WPO, fades,
// shadow distance; cover_id goes into the flags (MAT_COVER | MAT_COVER_ID). Texture indices are the caller's.
void grass_material(const GrassAsset *a, int tier, u32 cover_id, VgMaterial *out);
// The frame cover table entry (3 x vec4, shaders/lit_body.glsl cover_*):
//   [0] root_dark, root_extent, ground_match, ground_extent
//   [1] var_hue (rad), var_sat, var_val, macro
//   [2] sss, sss_wrap, specular, ao_root
void grass_cover_params(const GrassAsset *a, float out[12]);
// Expected clumps per m2 on material mat for a tier (density table x tier density x the mean of the
// clumping noise's keep rate).
float grass_expected_density(const GrassAsset *a, int mat, int tier);
// The clumping keep factor at a world position (0..1, the placement and the tests).
float grass_clump_keep(const GrassAsset *a, float x, float z, u32 seed);

#endif
