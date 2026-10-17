// Terrain shading at map level (docs/TERRAIN_SHADING.md): on an open-world map load, the parameters
// (terrain_params.h), the layer textures, the baked maps (terrain_maps.h) and the ground colours go to the
// renderer; per frame the level / debug view / grass term follow the cvars; the JSON reloads in place.
#ifndef DG_TERRAIN_WORLD_H
#define DG_TERRAIN_WORLD_H

#include "core/base.h"
#include "terrain_params.h"

// heights / material ids: n x n raster samples, sample (i, j) at (origin_x + i spacing, origin_z + j spacing).
void terrain_world_load(const float *h, const u8 *mat, int n, float origin_x, float origin_z, float spacing);
void terrain_world_unload(void);
// Per frame: level 0..4, debug view, grass fade (start, end m) for the grass term. Uploads when changed.
void terrain_world_frame(int level, int debug, float grass_start, float grass_end);
// Re-reads the JSON (textures reload only when their list changed). false on a parse error (old kept).
bool terrain_world_reload(void);
// Newest mtime of the JSON files (0 without a loaded terrain).
i64 terrain_world_mtime(void);
bool terrain_world_loaded(void);
const TerrainParams *terrain_world_params(void);
// The map grid: origin x, z, 1 / spacing, samples (the paint brush).
void terrain_world_map_xf(float out[4]);

#endif
