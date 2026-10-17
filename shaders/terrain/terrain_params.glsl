// Terrain shading parameters and resources (src/world/terrain/terrain_params.h TerrainGpu, std430 vec4s;
// src/render/terrain_tex.c bindings 16..24). Change a struct here and in terrain_params.h together.
#ifndef TERRAIN_PARAMS_GLSL
#define TERRAIN_PARAMS_GLSL
#define TER_SLOTS 16
#define TER_MAX_PAIRS 64
#define TPM_HEIGHT 1u
#define TPM_NOISE 2u
#define TPM_CRUMBS 4u
#define TPM_SET 8u
#define TPM_SCATTER 16u
#define TPM_DIRECTIONAL 32u
#define TPM_SLOPE 64u
#define TPM_WEAR 128u
#define TPM_MOISTURE 256u
#define TPM_RIM 512u
#define TMS_NOISE 0
#define TMS_MOISTURE 1
#define TMS_CURVATURE 2
#define TMS_SLOPE 3
#define TMS_PAINT 4
#define TMS_DRY 5

struct TerLayer {
    vec4 tex;      // albedo / nrh layer, macro layer, 1 / tile (m), 1 / macro scale (m)
    vec4 tiling;   // mode (0 plain, 1 cells, 2 hex), hex cell (tiles), scale jitter, rotation amount
    vec4 multi;    // far scale 1, far scale 2, multi-scale strength, uv-halving distance (m)
    vec4 hsv;      // hue jitter (rad), saturation jitter, value jitter, 1 / noise scale (m)
    vec4 blend;    // height_contrast, blend_sharpness, height_offset, porosity
    vec4 curv;     // cavity darkening, ridge lightening, moisture darkening, AO strength
    vec4 moist;    // moisture tint rgb, amount
    vec4 slope;    // slope tint rgb, amount
    vec4 ranges;   // slope tint n.y from x to y; altitude tint from z to w (m)
    vec4 alt;      // altitude tint rgb, amount
    vec4 aspect;   // north-facing tint rgb, amount
    vec4 var0;     // variant 0: layer (-1 none), mask source, lo, hi
    vec4 var1;
    vec4 varn;     // variant noise 1 / scale (m), noise weight, macro strength, 0
    vec4 detail;   // parallax depth (m), detail normal strength, micro roughness, normal strength
    vec4 snow;     // snow accumulation, wet darkening, exposure below n.y, exposure amount
    vec4 scatter;  // sprites per m2, cell (m), sprite mask bits, sprite size (m)
    vec4 mean;     // mean albedo (linear), base roughness
};
struct TerPair { vec4 a; vec4 b; vec4 c; vec4 d; vec4 e; };   // terrain_params.h TerPairGpu
struct TerGlobal { vec4 tier; vec4 dist; vec4 macro; vec4 snow; vec4 wind; vec4 grass; vec4 maps; vec4 scatter; };

layout(set = 0, binding = 20) uniform sampler2DArray terrain_alb;
layout(set = 0, binding = 21) uniform sampler2DArray terrain_nrh;
layout(set = 0, binding = 22) uniform sampler2DArray terrain_macro;
layout(set = 0, binding = 23) uniform sampler2D terrain_blend;
layout(set = 0, binding = 24) uniform sampler2D terrain_attr;
layout(set = 0, binding = 25) uniform sampler2D terrain_paint;
layout(set = 0, binding = 26) uniform sampler2D terrain_sc_alb;
layout(set = 0, binding = 27) uniform sampler2D terrain_sc_nrh;
layout(std430, set = 0, binding = 28) readonly buffer TerrainBlock {
    TerGlobal g;
    TerLayer layer[TER_SLOTS];
    TerPair pair[TER_MAX_PAIRS];
    uint pair_of[TER_SLOTS * TER_SLOTS];
} TB;
#endif
