// Virtualized geometry scene (docs/VGEO.md "GPU module"): the global GPU tables every view culls
// against. Meshes (VgMesh + their BVH nodes, groups and pages) and instances (32 B VgInstance,
// binned into 8 m placement cells) are registered at load time and uploaded by vgeo_scene_commit;
// nothing here runs per frame. Until streaming (Phase 4) every page is resident: page slot = global
// page index. The CPU tables are kept for the CPU reference cut (tests) and re-commits.
#ifndef DG_VGEO_SCENE_H
#define DG_VGEO_SCENE_H

#include "vgeo_ctx.h"
#include "vgeo_build.h"

typedef struct VgScene {
    const VgContext *ctx;
    VgMesh *meshes;
    u32 mesh_count, mesh_cap;
    VgNode *nodes;
    u32 node_count, node_cap;
    VgGroup *groups;
    u32 group_count, group_cap;
    u8 *pool;                    // CPU copy of the page pool (slot * VGEO_PAGE_SIZE)
    u32 page_count, page_cap;
    u32 *page_table;             // global page -> slot (~0 = not resident)
    u32 *mesh_depth;             // BVH depth per mesh
    VgInstance *instances;       // sorted by cell after commit
    u32 instance_count, instance_cap;
    VgCell *cells;
    u32 cell_count;
    u32 max_depth;               // deepest mesh BVH (traversal dispatches per pass)
    u64 cluster_count, tri_count;
    VgMaterial *materials;       // [0] = default grey
    u32 material_count, material_cap;
    VgBuffer b_meshes, b_nodes, b_groups, b_page_table, b_pool, b_instances, b_cells, b_materials;
    u32 generation;              // bumps on every commit (cullers rewrite their descriptor sets)
    const struct VgPalettes *palettes;   // skinned instances (vgeo_skin.h, vg_palettes_attach); NULL: none
} VgScene;

void vgeo_scene_init(VgScene *s, const VgContext *ctx);
void vgeo_scene_free(VgScene *s);
// Registers a built mesh (copies its tables and pages). Returns the mesh index.
u32 vgeo_scene_add_mesh(VgScene *s, const VgBuilt *b);
// Material table: returns the index; a mesh's clusters use materials material_base + cluster material.
u32 vgeo_scene_add_material(VgScene *s, const VgMaterial *m);
void vgeo_scene_set_mesh_materials(VgScene *s, u32 mesh, u32 material_base);
void vgeo_scene_add_instance(VgScene *s, u32 mesh, const float pos[3], const float rot[4], float scale);
// A skinned instance of a VGF_SKINNED mesh, animated by palette slot `slot` (vgeo_skin.h): its
// vertices are skinned into instance space, then transformed by pos / rot / scale. Skinned instances
// are binned into cells of their own (<= 32 each, unbounded: they move); their bounds come from the slot.
void vgeo_scene_add_skinned_instance(VgScene *s, u32 mesh, const float pos[3], const float rot[4], float scale, u32 slot);
void vgeo_scene_clear_instances(VgScene *s);
// Bins instances into cells and uploads everything (load time: waits for the device to be idle).
bool vgeo_scene_commit(VgScene *s);

// ------------------------------------------------------------------ CPU reference (tests)

// Decoded instance transform, exactly as the shaders decode it.
void vgeo_instance_decode(const VgInstance *in, float pos[3], float rot[4], float *scale);
// The cluster record of a resident page slot.
const VgCluster *vgeo_scene_cluster(const VgScene *s, u32 slot, u32 index);

typedef struct VgRefCluster {
    u32 instance, packed;        // packed = page slot << VG_CLUSTER_BITS | cluster
    float lod_margin;            // min |px - threshold| / threshold of the two LOD tests (borderline when tiny)
    i8 frustum;                  // 1 inside, 0 outside, -1 within the borderline band of a plane
} VgRefCluster;
// LOD selection over every instance (no node pruning): cluster c of group g is selected when
// px(g.parent_error, g.lod) > t and (px(c.error, c.lod) <= t or its child group's page is missing).
// With frustum, the culling sphere is also classified against the view planes. Skinned instances
// use the slot's current palettes as the shaders do (vgeo_skin.h: sphere transform, error scale,
// bone-box culling spheres; empty slots select nothing). Returns the count.
u32 vgeo_scene_cpu_select(const VgScene *s, const VgViewGpu *v, VgRefCluster *out, u32 cap);

// View constants from a view-projection matrix (planes, viewport) and the error settings.
void vgeo_view_setup(VgViewGpu *v, const float view_proj[16], const float prev_view_proj[16], const float cam_pos[3], float proj_scale, float znear, u32 w,
                     u32 h, float error_px, float sw_px, u32 flags);

#endif
