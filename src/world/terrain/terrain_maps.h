// Terrain maps baked from the material raster at map load (docs/TERRAIN_SHADING.md "Maps"), one texel per
// raster sample (1 m on the open-world maps), uploaded beside the material map:
//
//   blend  RGBA8: R the dominant material, G the second, B / A their fractions inside a box of
//          radius R (blend_radius_m). For a straight border the fraction ramps linearly across +-R, so
//          the shader turns (fa - fb) / (fa + fb) x (R + 0.5 texel) into a signed distance to the border in
//          metres (d > 0 on the dominant side): band widths are set in metres and do not depend on the
//          terrain LOD or the camera distance.
//   attr   RGBA8: R curvature (0.5 flat, > 0.5 concave / cavity, < 0.5 convex / ridge; Laplacian of the
//          height blurred over 2 m, x curvature_scale), G moisture (flow accumulation, log-scaled, plus
//          low ground; wet materials 1), B wear (paths: 1 on dirt roads, falling off over wear_m beside
//          them; painted wear adds), A distance to the nearest other material (0..8 m -> 0..255).
//
// Pure: no renderer (tests/test_terrain.c checks every channel against a direct CPU reference).
#ifndef DG_TERRAIN_MAPS_H
#define DG_TERRAIN_MAPS_H

#include "core/base.h"

typedef struct TerrainMapParams {
    float spacing;                   // m per sample
    int blend_radius;                // box radius in samples (blend map)
    float curvature_scale;           // curvature (1 / m) -> R: 0.5 + c x scale
    float flow_ref;                  // flow accumulation (cells) that reads as fully wet
    float low_radius_m, low_depth_m; // low ground: below the mean height of this radius by this much -> moist
    float wear_m;                    // path wear fall-off (m)
    u32 wet_mask;                    // materials that are fully moist (bits of material ids: mud)
    u32 path_mask;                   // materials that are worn paths (bits: dirt_road)
} TerrainMapParams;

void terrain_map_params_default(TerrainMapParams *p);

// h: n x n heights (row-major, z rows), mat: n x n material ids. blend / attr: n x n x 4 out.
void terrain_maps_build(const float *h, const u8 *mat, int n, const TerrainMapParams *p, u8 *blend, u8 *attr);

// CPU references (tests): the box fractions of the two dominant materials at (x, y) (ids out), the
// curvature (1 / m), the flow accumulation (cells, D8 steepest descent) of the whole raster, the distance
// (m) to the nearest sample with another material (brute force within 8 m).
void terrain_ref_fractions(const u8 *mat, int n, int x, int y, int r, int *a, int *b, float *fa, float *fb);
float terrain_ref_curvature(const float *h, int n, int x, int y, float spacing);
void terrain_ref_flow(const float *h, int n, float *acc);
float terrain_ref_border_distance(const u8 *mat, int n, int x, int y, float spacing);

#endif
