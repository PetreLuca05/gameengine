// Virtualized geometry, offline: the cluster builder and its DAG (docs/VGEO.md "Builder").
// Pure C, no renderer: linked into the game (asset import), tools/build_vgeo and the tests.
//
//   source triangles -> weld (grid-snapped positions) -> leaf clusters (meshopt meshlets, <= 128 tris /
//   128 verts) -> DAG: repeat { cluster adjacency, greedy groups of ~8, per group: lock the group
//   boundary, simplify to 50 %, re-cluster } until one cluster is left -> per-level BVH over groups,
//   pages (128 KiB, coarse -> fine), quantised cluster payloads, discrete LOD cuts, stats.
#ifndef DG_VGEO_BUILD_H
#define DG_VGEO_BUILD_H

#include "vgeo_gpu.h"

#define VGEO_BUILD_VERSION 1     // bump when the output of the builder changes (cache key)
#define VGEO_MAX_LEVELS 32

typedef struct VgSource {
    u32 vertex_count;
    const float *pos;            // xyz per vertex
    const float *normal;         // xyz per vertex (NULL: flat normals from the faces)
    const float *uv;             // uv per vertex (NULL: zero)
    const u32 *color;            // RGBA8 per vertex (NULL: none, VGF_HAS_COLOR off)
    u32 index_count;
    const u32 *indices;
    const u16 *tri_material;     // per triangle (NULL: material 0)
    u32 material_count;
    const char (*material_names)[48];
} VgSource;

typedef struct VgBuildOptions {
    int threads;                 // <= 0: all cores
    int lod_count;               // discrete LOD cuts exported (<= VGEO_MAX_LODS; 0 = 4)
    float lod_error_rel[VGEO_MAX_LODS];   // their object-space errors as a fraction of the radius (0 = defaults)
    bool keep_build_data;        // keep the build-time clusters for tests (VgBuilt.dbg)
} VgBuildOptions;

typedef struct VgLevelStats { u32 clusters, tris, groups, unreducible; float err_min, err_max; } VgLevelStats;

typedef struct VgBuildStats {
    u32 input_tris, input_verts, welded_verts;
    u32 levels;
    VgLevelStats level[VGEO_MAX_LEVELS];
    u32 clusters, groups, nodes, pages, root_pages;
    u64 page_bytes;
    double ms_weld, ms_leaf, ms_dag, ms_pack, ms_lods, ms_total;
    int threads;
} VgBuildStats;

// Build-time view of one cluster (tests only): global welded-vertex triangles.
typedef struct VgDbgCluster {
    u32 first_index, tri_count;  // into VgDbg.indices
    u32 group;                   // group it belongs to
    u32 child_group;             // group it was simplified from (~0 leaf)
    u32 level;
    float error, lod_center[3], lod_radius;
    float center[3], radius;
} VgDbgCluster;

typedef struct VgDbg {
    u32 vertex_count;
    float *pos;                  // welded, grid-snapped positions
    u32 *pos_id;                 // position-only weld id per vertex (adjacency / crack tests)
    u32 cluster_count;
    VgDbgCluster *clusters;
    u32 *indices;
    u32 *group_of_cluster_order; // final file cluster order -> build cluster
} VgDbg;

// A built (or loaded) mesh: everything the .vgeo file holds.
typedef struct VgBuilt {
    VgFileHeader hdr;
    VgNode *nodes;
    VgGroup *groups;
    VgPageDesc *pages;
    VgMaterialRef *materials;
    VgDiscreteLod lods[VGEO_MAX_LODS];
    u32 *lod_indices;
    VgLodVertex *lod_vertices;
    u8 *page_data;
    u32 page_data_size;
    VgBuildStats stats;
    VgDbg dbg;                   // only with keep_build_data
} VgBuilt;

bool vgeo_build(const VgSource *src, const VgBuildOptions *opt, VgBuilt *out, char *err, int err_size);
void vgeo_free(VgBuilt *b);

// .vgeo file: serialize to / load from memory (the header carries the cache key).
bool vgeo_serialize(const VgBuilt *b, u32 key, u8 **data, size_t *size);
bool vgeo_deserialize(const u8 *data, size_t size, VgBuilt *out);

// Page access and decoding (CPU reference, tests, the CPU LOD export).
const VgPageHeader *vgeo_page(const VgBuilt *b, u32 page);
const VgCluster *vgeo_page_cluster(const VgBuilt *b, u32 page, u32 index);
typedef struct VgDecoded {
    u32 vert_count, tri_count, material;
    i32 grid[VGEO_MAX_CLUSTER_VERTS][3];   // exact grid coordinates (crack tests compare these)
    float pos[VGEO_MAX_CLUSTER_VERTS][3];
    float normal[VGEO_MAX_CLUSTER_VERTS][3];
    float uv[VGEO_MAX_CLUSTER_VERTS][2];
    u8 tri[VGEO_MAX_CLUSTER_TRIS][3];
} VgDecoded;
void vgeo_decode_cluster(const VgBuilt *b, u32 page, u32 index, VgDecoded *out);

// CPU reference cut at an object-space error: cluster c of group g is in the cut when
// g.parent_error > t and c.error <= t (every page resident). Returns the count; out may be NULL.
typedef struct VgCutRef { u32 page, index, group; } VgCutRef;
u32 vgeo_cut(const VgBuilt *b, float t, VgCutRef *out, u32 cap);

void vgeo_print_stats(const VgBuildStats *s, void (*print)(const char *line));

// Small helpers shared with the import path.
u32 vgeo_fnv1a(const void *data, size_t size, u32 h);
u16 vgeo_half(float f);
float vgeo_half_to_float(u16 h);

#endif
