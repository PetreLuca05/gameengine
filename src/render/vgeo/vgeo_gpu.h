// Virtualized geometry: the data layout shared by the builder, the .vgeo file, the renderer and the
// shaders (docs/VGEO.md "Data layout"). std430-compatible; mirrored in shaders/vgeo/vgeo_common.glsl.
// Every size is asserted: change a struct here and in the GLSL mirror together.
#ifndef DG_VGEO_GPU_H
#define DG_VGEO_GPU_H

#include "core/base.h"

#define VGEO_MAGIC 0x4F454756u           // 'VGEO'
#define VGEO_FILE_VERSION 1
#define VGEO_PAGE_SIZE (128u * 1024u)    // one streaming unit / one GPU page-pool slot
#define VGEO_MAX_CLUSTER_TRIS 128
#define VGEO_MAX_CLUSTER_VERTS 128
#define VGEO_MAX_LODS 4

enum VgFileFlags { VGF_SKINNED = 1u << 0, VGF_FOLIAGE = 1u << 1, VGF_HAS_COLOR = 1u << 2, VGF_HAS_IMPOSTOR = 1u << 3 };
enum VgNodeFlags { VGN_LEAF = 1u << 0 };
enum VgClusterFlags { VGC_TWO_SIDED = 1u << 0, VGC_NO_CONE = 1u << 1 };

// BVH over groups (4-wide), built per DAG level and merged under one root.
typedef struct VgNode {
    float center[3], radius;     // sphere enclosing the subtree's culling and LOD spheres (mesh space)
    float max_parent_error;      // max group parent error in the subtree: skip the node if it projects <= threshold
    u32 first;                   // first child node, or first group (leaf)
    u16 count, flags;            // children / groups; VGN_LEAF
    u32 pad;
} VgNode;

// A group of clusters simplified together (the DAG's unit of LOD).
typedef struct VgGroup {
    float lod_center[3], lod_radius;   // encloses every member cluster's LOD sphere (monotonic up the DAG)
    float parent_error;          // error of the simplification of this group (>= every member's error; FLT_MAX at the root)
    u32 page;                    // every cluster of a group lives in one page
    u16 first_cluster, cluster_count;  // within the page
    u32 level;                   // DAG level (0 = leaves)
} VgGroup;

// One loaded mesh in the global tables.
typedef struct VgMesh {
    float center[3], radius;
    u32 root_node, node_base, group_base, page_base;   // offsets into the global tables
    u32 page_count, flags, material_base, impostor;    // impostor: atlas id or ~0
    float max_wpo;               // m: max world-position offset over its materials (0 = static)
    float q_step;                // position grid step (m): pos = (cluster.base + u16 offset) * q_step
    float pad[2];
} VgMesh;

// Page payload: VgPageHeader, VgCluster[cluster_count], vertex data, triangle data (16-byte aligned blocks).
typedef struct VgPageHeader { u32 cluster_count, vertex_bytes, index_bytes, skin_bytes; } VgPageHeader;

// Positions are on one grid per mesh (VgMesh.q_step): a cluster stores its integer base and each vertex
// a u16x3 offset, so a vertex shared by two clusters decodes to the same bits in both (crack-free).
typedef struct VgCluster {
    float center[3], radius;     // culling sphere (mesh space; skinned: bind pose)
    float lod_center[3], lod_radius;   // sphere of the group this cluster was generated from
    float error;                 // that group's simplification error (0 at leaves)
    u32 cone;                    // oct axis 8 | 8, cutoff snorm8, flags 8 (VgClusterFlags)
    u32 counts;                  // verts 8 | tris 8 | material 16
    u32 offsets;                 // vertex data offset / 4 (15 bits) | triangle data offset / 4 (15 bits) << 15
    u32 child_group;             // group this cluster was simplified from (~0 = leaf)
    i32 base[3];                 // grid base of the vertex offsets
} VgCluster;

#define VGEO_VERTEX_BYTES 12     // u16x3 grid offset, oct normal 8+8, half2 uv
#define VGEO_VERTEX_COLOR_BYTES 4
#define VGEO_TRI_BYTES 4         // u8x3 local indices + 8 spare bits (SW raster edge flags)

typedef struct VgPageDesc {
    u32 file_offset, size;       // in the file's page data section
    u32 first_group, group_count;
} VgPageDesc;

typedef struct VgMaterialRef { char name[48]; u32 slot; } VgMaterialRef;

// Exported discrete LOD (2015 / 2010 eras): a cut of the DAG at a fixed object-space error.
typedef struct VgDiscreteLod {
    float error;                 // object-space error of the cut (m)
    u32 first_index, index_count;   // into the LOD index section (u32)
    u32 first_vertex, vertex_count; // into the LOD vertex section (VgLodVertex)
    u32 tris, pad[2];
} VgDiscreteLod;

typedef struct VgLodVertex { float pos[3]; float normal[3]; float uv[2]; } VgLodVertex;

enum VgSection { VGS_NODES, VGS_GROUPS, VGS_PAGES, VGS_MATERIALS, VGS_LODS, VGS_LOD_INDICES, VGS_LOD_VERTICES, VGS_IMPOSTOR, VGS_PAGE_DATA, VGS_STATS, VGS_COUNT };
typedef struct VgSectionRef { u32 offset, size; } VgSectionRef;

typedef struct VgFileHeader {
    u32 magic, version, flags, key;
    u32 cluster_count, group_count, node_count, page_count;
    u32 root_page_count, lod_count, material_count, bone_count;
    float sphere[4];
    float aabb_min[3], q_step;
    float aabb_max[3];
    u32 level_count;
    u32 root_node, pad[3];       // BVH root (index into the node section)
    VgSectionRef sections[VGS_COUNT];
} VgFileHeader;

// Instances (32 B): props, chunk placements and foliage share one format (Phase 2+).
typedef struct VgInstance {
    float pos[3];
    u32 rot;                     // quaternion, smallest-three 10:10:10:2
    u16 scale_h, mesh;           // half uniform scale; VgMesh index
    u32 flags_phase;             // wind phase u8 | era / density hash u8 | flags u16
    u32 extra;                   // XFORM: VgXform index; SKINNED: palette slot
    u32 cell;                    // owning placement cell
} VgInstance;

_Static_assert(sizeof(VgNode) == 32, "VgNode");
_Static_assert(sizeof(VgGroup) == 32, "VgGroup");
_Static_assert(sizeof(VgMesh) == 64, "VgMesh");
_Static_assert(sizeof(VgPageHeader) == 16, "VgPageHeader");
_Static_assert(sizeof(VgCluster) == 64, "VgCluster");
_Static_assert(sizeof(VgPageDesc) == 16, "VgPageDesc");
_Static_assert(sizeof(VgDiscreteLod) == 32, "VgDiscreteLod");
_Static_assert(sizeof(VgInstance) == 32, "VgInstance");
_Static_assert(sizeof(VgFileHeader) == 112 + VGS_COUNT * 8, "VgFileHeader");

#endif
