// CPU mirror of the terrain shader's blending and tiling (shaders/terrain/terrain_weights.glsl,
// terrain_tile.glsl; docs/TERRAIN_SHADING.md). The tests check the techniques on it: band width in metres,
// order independence, the taller material on top at the band centre, noise-broken borders, transition
// sets confined to the band, hex-bombing autocorrelation and histogram preservation. Keep each function
// identical to its GLSL twin (same names with tn_ / ter_ prefixes).
#ifndef DG_TERRAIN_BLEND_H
#define DG_TERRAIN_BLEND_H

#include "terrain_params.h"

// tn_hash / tn_noise / tn_fbm (terrain_noise.glsl): integer hash of the cell, smooth value noise, fBm.
float ter_hash(float x, float y);
float ter_noise(float x, float y);
float ter_fbm(float x, float y, int octaves);

#define TER_GATHER_MAX 8
typedef struct TerWeights {
    int n;                           // entries (merged ids)
    int id[TER_GATHER_MAX];
    float w[TER_GATHER_MAX];         // sum 1
} TerWeights;

// The blend map's 4 samples around world (x, z) (map: origin, 1 / spacing, n = TerGlobalGpu.maps),
// bilinear: each sample gives its fraction to its dominant and second material. Sorted by weight.
void ter_gather(const u8 *blend, const float maps[4], float x, float z, TerWeights *out);
// Signed distance (m) to the border between the top two entries (> 0: on a's side), from their weights:
// (wa - wb) / (wa + wb) x (blend radius + half a sample).
float ter_border_distance(const TerWeights *w, float radius_m, float spacing);
// The pair's border shift at (x, z) (noise-broken edge, crumbs, wear / moisture / slope / direction biases;
// mode bits gate each), in m. attr: curvature, moisture, wear (0..1); slope_dir: downhill (unit xz) x
// steepness (0 flat .. 1); border_dir: unit xz toward a; wind: unit xz; level: the fidelity level.
float ter_border_shift(const TerPairGpu *q, float x, float z, float crumb_m, const float attr[3], const float slope_dir[2], const float border_dir[2],
                       const float wind[2], int level);
// Band blend t_a in [0, 1] for distance d (m, shift applied): smoothstep over the pair's width.
float ter_band(const TerPairGpu *q, float d);
// Height blend of n layers: t (band weights, sum 1), H (layer height x contrast + offset), sharpness ->
// final weights (sum 1). Order independent; the taller layer wins where the bands are equal.
void ter_height_blend(int n, const float *t, const float *H, float sharpness, float *out);
// Transition-set weight inside the band (4 t (1 - t) x strength: 0 outside the band).
float ter_set_weight(const TerPairGpu *q, float ta);

// Hex tiling (terrain_tile.glsl ter_hex): the 3 hex cells of uv (in tiles, cell size c) and their weights
// (sum 1, sharpened), each cell's random offset / rotation / scale (rot_amount, scale_jitter).
typedef struct TerHex { float w[3]; float off[3][2]; float rot[3]; float scale[3]; float centre[3][2]; } TerHex;
void ter_hex(float u, float v, float cell, float rot_amount, float scale_jitter, TerHex *out);
// One hex-bombed, histogram-preserving sample of a single-channel tileable image (size x size, wrap,
// bilinear) at uv (tiles): mean + sum w_i (t_i - mean) / sqrt(sum w_i^2).
float ter_hex_sample(const float *img, int size, float mean, float u, float v, float cell, float rot_amount, float scale_jitter);
float ter_plain_sample(const float *img, int size, float u, float v);

#endif
