// Virtualized geometry: the data layout shared by the builder, the .vgeo file, the renderer and the
// shaders (docs/VGEO.md "Data layout"). std430-compatible; mirrored in shaders/vgeo/vgeo_common.glsl.
// Every size is asserted: change a struct here and in the GLSL mirror together.
#ifndef DG_VGEO_GPU_H
#define DG_VGEO_GPU_H

#include "core/base.h"
#include <math.h>

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

// Placement cell (8 m): the first level of instance culling. Instances of a cell are contiguous.
typedef struct VgCell { float min[3]; u32 first; float max[3]; u32 count; } VgCell;
#define VGEO_CELL_SIZE 8.0f

// ------------------------------------------------------------------ culling (Phase 2)

#define VG_MAX_VIEWS 4               // main camera + 3 shadow cascades
#define VG_PAGE_WORDS (VGEO_PAGE_SIZE / 4u)
#define VG_CLUSTER_BITS 12           // visible entry: page slot << 12 | cluster within the page
enum VgViewFlags {
    VGV_NO_FRUSTUM = 1u << 0,        // tests: LOD selection only
    VGV_NO_CONE = 1u << 1,
    VGV_NO_OCCLUSION = 1u << 2,
    VGV_ORTHO = 1u << 3,             // shadow cascades: error projects without distance
    VGV_TWO_PASS = 1u << 4,          // pass 2 re-tests pass 1 occluders against the current Hi-Z
};

// Per-view constants (std430, binding 0). Planes are normalised (inside: dot(n, p) + d >= 0).
typedef struct VgViewGpu {
    float view_proj[16];
    float prev_view_proj[16];        // the view the previous Hi-Z was built with (pass 1)
    float planes[6][4];
    float cam_pos[3], proj_scale;    // proj_scale = render_h / (2 tan(fov / 2)), ortho: px per metre
    float error_px, sw_px, znear;
    u32 flags;                       // VgViewFlags
    float viewport[4];               // w, h, 1 / w, 1 / h
    float hiz_prev[4];               // mip 0 w, h, mip count, valid
    float hiz_cur[4];
    u32 cell_count, instance_count, max_depth, pad0;
    u32 cap_nodes, cap_groups, cap_visible, cap_occluded;
    u32 cap_cells, cap_instances, cap_requests, pad1;
} VgViewGpu;

// Counters (binding 8): indirect dispatch args first (x, y, z, unused), then counts.
enum VgArgs { VGA_INST, VGA_NODE, VGA_GROUP, VGA_INSTLIST, VGA_OCCL, VGA_COUNT = 8 };
enum VgCount {
    VGN_CELLS_VIS, VGN_CELLS_OCC, VGN_INST_VIS, VGN_INST_OCC,
    VGN_NODEQ0, VGN_NODEQ1, VGN_NODEQ2,          // node queue regions (0 / 1 ping-pong, 2 = pass 2 seeds)
    VGN_NODES_VISITED, VGN_NODES_OCC,
    VGN_GROUPS0, VGN_GROUPS1,                    // group items, pass 1 / pass 2
    VGN_CLUSTERS_SELECTED, VGN_HW, VGN_SW, VGN_HW_P1, VGN_SW_P1,
    VGN_CL_OCC, VGN_PAGE_REQ, VGN_OVERFLOW, VGN_TRIS, VGN_INST_P2, VGN_CL_P2, VGN_CELLS_P2,
    VGN_COUNT = 32
};
typedef struct VgCounters { u32 args[VGA_COUNT][4]; u32 n[VGN_COUNT]; } VgCounters;

// Push constants of every cull kernel (48 B).
typedef struct VgPush { u32 pass, in_region, out_region, view; u32 a[8]; } VgPush;

// Instance rotation: smallest-three, 10 bits per component, index of the dropped one in the top 2.
DG_INLINE u32 vg_quat_encode(const float q[4]) {
    int big = 0;
    for (int i = 1; i < 4; i++) if (fabsf(q[i]) > fabsf(q[big])) big = i;
    float s = q[big] < 0 ? -1.0f : 1.0f;
    u32 out = (u32)big << 30, k = 0;
    for (int i = 0; i < 4; i++) {
        if (i == big) continue;
        float v = q[i] * s * 1.41421356f * 0.5f + 0.5f;
        int iv = (int)lrintf(v * 1023.0f);
        out |= (u32)(iv < 0 ? 0 : (iv > 1023 ? 1023 : iv)) << (k * 10);
        k++;
    }
    return out;
}
DG_INLINE void vg_quat_decode(u32 e, float q[4]) {
    u32 big = e >> 30;
    float v[3];
    for (int k = 0; k < 3; k++) v[k] = ((float)((e >> (k * 10)) & 1023u) / 1023.0f * 2.0f - 1.0f) * 0.70710678f;
    float w = sqrtf(fmaxf(0.0f, 1.0f - v[0] * v[0] - v[1] * v[1] - v[2] * v[2]));
    u32 k = 0;
    for (u32 i = 0; i < 4; i++) q[i] = i == big ? w : v[k++];
}

_Static_assert(sizeof(VgNode) == 32, "VgNode");
_Static_assert(sizeof(VgGroup) == 32, "VgGroup");
_Static_assert(sizeof(VgMesh) == 64, "VgMesh");
_Static_assert(sizeof(VgPageHeader) == 16, "VgPageHeader");
_Static_assert(sizeof(VgCluster) == 64, "VgCluster");
_Static_assert(sizeof(VgPageDesc) == 16, "VgPageDesc");
_Static_assert(sizeof(VgDiscreteLod) == 32, "VgDiscreteLod");
_Static_assert(sizeof(VgInstance) == 32, "VgInstance");
_Static_assert(sizeof(VgCell) == 32, "VgCell");
_Static_assert(sizeof(VgViewGpu) == 352, "VgViewGpu");
_Static_assert(sizeof(VgCounters) == 256, "VgCounters");
_Static_assert(sizeof(VgPush) == 48, "VgPush");
_Static_assert(sizeof(VgFileHeader) == 112 + VGS_COUNT * 8, "VgFileHeader");

#endif
