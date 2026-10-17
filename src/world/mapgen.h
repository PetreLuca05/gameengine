// Open-world map generator (docs/ZOMBIE_MAP.md): the 2 km heightfield, its surface materials, water
// (sea, lakes, rivers), roads with their height profiles and bridges, and the keep-out areas that
// vegetation avoids, all from a JSON description of regions ("world" block of a level). Pure: no
// renderer, physics or engine state; the game glue (src/world/openworld.c) turns the result into
// streamed chunks, meshes and bodies, and mapgen_poi.c plans / builds the points of interest.
// Coordinates: the engine's (time_of_day.h): X east, Y up, Z south (north = -Z), metres; sea level 0.
// Deterministic for a description.
#ifndef DG_MAPGEN_H
#define DG_MAPGEN_H

#include "core/base.h"
#include "core/dgmath.h"
#include "FastNoiseLite.h"

#define MG_MAX_FEATURES 2048
#define MG_MAX_ROADS 256
#define MG_MAX_POLY 64           // points per polygon / control polyline
#define MG_MAX_RIVERS 4
#define MG_MAX_BRIDGES 32
#define MG_ROAD_STEP 2.0f        // m between road / river samples
#define MG_GRID_CELL 32.0f       // m: spatial hash of road / river / keep-out lookups

typedef enum MgMaterial {
    MGM_GRASS, MGM_MEADOW, MGM_FOREST, MGM_DIRT, MGM_ROCK, MGM_SAND, MGM_GRAVEL, MGM_ASPHALT, MGM_DIRT_ROAD,
    MGM_CONCRETE, MGM_FIELD, MGM_CROP, MGM_PLOWED, MGM_MUD, MGM_COUNT   // FIELD wheat, CROP green, PLOWED soil
} MgMaterial;

typedef enum MgFeatureKind {
    MGF_MOUNTAIN,   // pos, size = radii, height, param = ridge sharpness 0..1
    MGF_RIM,        // edge (MG_EDGE_*), height, blend = width (m) from the map edge inwards, range along the edge
    MGF_LAKE,       // pos, size = radii, level (water surface), depth, param = shoreline noise (m)
    MGF_PIT,        // pos, size = radii, depth, terraces (stepped quarry), yaw
    MGF_PAD,        // pos, size = half extents, yaw, level (or auto), blend (m), material (-1 keeps)
    MGF_FOREST,     // pos, size = radii, param = trees per 100 m2, height = pine fraction 0..1, depth = dead fraction 0..1
    MGF_PAINT,      // pos, size = half extents, yaw, material: surface only (fields, yards, lots)
    MGF_HILL,       // pos, size = radii, height (smooth bump, no ridges)
} MgFeatureKind;

enum { MG_EDGE_W = 1, MG_EDGE_E = 2, MG_EDGE_S = 4, MG_EDGE_N = 8 };

typedef struct MgFeature {
    int kind;
    vec2 pos, size;
    float yaw, height, depth, blend, param, level;
    vec2 range;                  // rim: [min, max] along the edge
    int material, terraces, edge, variant;
    bool auto_level;             // pad: level from the terrain under it (mean)
} MgFeature;

typedef enum MgRoadClass { MGR_MAIN, MGR_SECONDARY, MGR_STREET, MGR_DIRT, MGR_TRAIL, MGR_RUNWAY, MGR_CLASS_COUNT } MgRoadClass;

typedef struct MgRoadSample {
    vec2 p, dir;                 // centre, unit tangent
    float h;                     // profile height (deck height on bridges)
    u16 road;
    u8 bridge;                   // 1: this sample spans water (a bridge deck is built over it)
    u8 pad_;
} MgRoadSample;

typedef struct MgRoad {
    int cls;
    float width;                 // carriageway (m)
    float shoulder;              // blend to the terrain beyond the edge (m)
    int first, count;            // samples in MapGen.rs
    bool loop;
    bool keep_level;             // follow the terrain exactly (streets on pads: no smoothing beyond the pad)
    char name[32];
} MgRoad;

typedef struct MgRiver {
    vec2 ctrl[MG_MAX_POLY];
    int ctrl_count;
    float width, depth, bank;    // channel width, depth below the water surface, bank blend (m)
    float start_level, end_level;
    int first, count;            // samples in MapGen.vs
} MgRiver;

typedef struct MgRiverSample { vec2 p, dir; float water; u16 river; } MgRiverSample;

typedef struct MgBridge {
    int road;
    int s0, s1;                  // first / last sample (inclusive, in MapGen.rs) over the water
    vec2 a, b;                   // deck ends (abutments on the banks)
    float h;                     // deck surface height
    float width;
} MgBridge;

typedef struct MgKeepout { vec2 c, half; float yaw; u8 kind; } MgKeepout;   // vegetation-free footprints

typedef struct MgGrid {          // spatial hash over the map: per cell a list of item indices
    int n;                       // cells per side
    float origin;                // min corner (x and z)
    int *start, *items;          // start[n*n+1] prefix offsets, items[]
} MgGrid;

typedef struct MapGen {
    u32 seed;
    float half_size;             // playable half extent (m): the boundary walls
    float raster_half;           // generated area half extent (m): >= half_size
    float base_height, hill_amp, hill_freq;
    float sea_floor, beach_width, sea_noise;
    vec2 sea[MG_MAX_POLY];       // sea polygon (closed), empty: no sea
    int sea_count;
    MgFeature features[MG_MAX_FEATURES];
    int feature_count;
    MgRiver rivers[MG_MAX_RIVERS];
    int river_count;
    MgRoad roads[MG_MAX_ROADS];
    int road_count;
    vec2 *road_ctrl;             // control points of all roads (kept until finalize)
    int *road_ctrl_first, *road_ctrl_count;
    int road_ctrl_total, road_ctrl_cap;
    bool *road_smooth;
    MgRoadSample *rs;            // road samples (finalize)
    int rs_count;
    MgRiverSample *vs;           // river samples
    int vs_count;
    MgBridge bridges[MG_MAX_BRIDGES];
    int bridge_count;
    MgKeepout *keep;
    int keep_count, keep_cap;
    MgGrid road_grid, river_grid, keep_grid, feature_grid;
    fnl_state n_hills, n_ridge, n_detail, n_shore;
    bool finalized;
} MapGen;

// Surface raster at 1 m (what the chunks, the collision and the placement read).
typedef struct MgRaster {
    int n;                       // samples per side (2 * half / spacing + 1)
    float origin, spacing;       // min corner, m between samples
    float *h;
    u8 *mat;
} MgRaster;

// ---- description
struct cJSON;
void mapgen_init(MapGen *g, u32 seed);
void mapgen_free(MapGen *g);
// Reads the "world" block (terrain, sea, features, rivers, roads); false on a malformed block.
bool mapgen_parse(MapGen *g, const struct cJSON *world, char *err, size_t err_size);
int mapgen_add_feature(MapGen *g, const MgFeature *f);   // index or -1
// Control polyline (smooth: Catmull-Rom through the points). Returns the road index or -1.
int mapgen_add_road(MapGen *g, int cls, float width, const vec2 *pts, int n, bool smooth, bool loop, const char *name);
void mapgen_add_keepout(MapGen *g, vec2 c, vec2 half, float yaw, int kind);
int mapgen_road_class(const char *name);                 // "main", "secondary", ... (-1 unknown)
float mapgen_road_default_width(int cls);
// Pads' automatic levels, road / river sampling and height profiles, bridges, lookup grids. After
// every feature and road has been added (the POI planner adds streets and pads before this).
void mapgen_finalize(MapGen *g);

// ---- evaluation (thread-safe after finalize)
// Height before roads and rivers (natural terrain + sea + lakes + pits + pads).
float mapgen_height_base(const MapGen *g, float x, float z);
// Final height and surface material at (x, z).
float mapgen_height(const MapGen *g, float x, float z, u8 *mat);
// Water surface at (x, z) (sea 0, lake level, river profile), or -1e9 where there is no water body.
float mapgen_water_level(const MapGen *g, float x, float z);
// The parts of mapgen_water_level, for the water bodies (src/game/game_water.c): inside the sea
// (beach included), inside lake feature `feature`, on a river (its water level and flow direction).
bool mapgen_in_sea(const MapGen *g, float x, float z);
bool mapgen_in_lake(const MapGen *g, int feature, float x, float z);
bool mapgen_river_water(const MapGen *g, float x, float z, float *level, vec2 *dir);
// Nearest road sample within max_dist of (x, z) (-1 none); *dist = distance to its centreline.
int mapgen_nearest_road(const MapGen *g, float x, float z, float max_dist, float *dist);
bool mapgen_in_keepout(const MapGen *g, float x, float z, float margin);
// Tree density (trees per m2) at (x, z) from the forest features and the terrain (0 on roads, water,
// pads, steep rock and keep-outs; needs the raster for the slope).
float mapgen_tree_density(const MapGen *g, const MgRaster *r, float x, float z, float *pine_fraction, float *dead_fraction);
// Signed distance to the sea polygon's boundary (> 0 inside the sea), shoreline noise included.
float mapgen_sea_distance(const MapGen *g, float x, float z);

// ---- raster
bool mapgen_raster_alloc(MgRaster *r, float half, float spacing);
void mapgen_raster_free(MgRaster *r);
// Rows [row0, row1) (independent: run them in parallel); then mapgen_raster_slopes over all rows.
void mapgen_raster_rows(const MapGen *g, MgRaster *r, int row0, int row1);
void mapgen_raster_slopes(const MapGen *g, MgRaster *r, int row0, int row1);   // steep -> rock
float mapgen_raster_height(const MgRaster *r, float x, float z);              // bilinear (clamped)
u8 mapgen_raster_mat(const MgRaster *r, float x, float z);                    // nearest
float mapgen_raster_slope(const MgRaster *r, float x, float z);               // |gradient|

// Colour of a material (linear RGB) with a 0..1 variation (noise) and slope.
vec3 mapgen_material_color(int mat, float variation, float slope);
const char *mapgen_material_name(int mat);

#endif
