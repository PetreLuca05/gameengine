// vgeo scene (docs/VGEO.md "Scene tables"): the global tables the classic renderer draws. Meshes
// (VgMesh + their BVH nodes, groups and pages) and instances (32 B VgInstance, binned into 8 m
// placement cells) are registered at load time and committed by vgeo_scene_commit; nothing here runs
// per frame. Without a context (the game: vgeo_scene_init(s, NULL)) the tables stay on the CPU; with
// one the commit also uploads them and a page pool (every page resident without a budget, slot =
// global page index; vgeo_scene_set_pool: root pages only, the page streamer of branch `nanite`).
// The CPU keeps every page's header + cluster records ("meta": the CPU reference cut, tests, debug)
// and a source for the payload: the page data in memory, or a byte range of the .vgeo file.
#ifndef DG_VGEO_SCENE_H
#define DG_VGEO_SCENE_H

#include "vgeo_ctx.h"
#include "vgeo_build.h"

// Where a mesh's page payloads come from: data (owned copy of VgBuilt.page_data) or path + base
// (the page data section of a .vgeo file; VgPageDesc.file_offset is relative to it).
typedef struct VgPageSource { u8 *data; char *path; u64 base; } VgPageSource;

// Placement chunks (Phase 7, docs/VGEO.md "Foliage support"): a region of the instance and cell
// buffers after the committed (static) ones, sub-allocated per chunk and patched in place, so loading
// or freeing a chunk costs no commit. GPU instance index of region instance i = instance_count + i,
// GPU cell of region cell j = cell_count + j. Free cells are empty (count 0, far away).
typedef struct VgRange { u32 first, count; } VgRange;
typedef struct VgPlacementChunk {
    bool used;
    u32 first, count;            // region instances
    u32 cell_count;
    u32 cells[16];               // region cells (an 8 m grid: 4 x 4 per 32 m chunk)
    u32 user;                    // caller's tag (chunk coordinates, ...)
} VgPlacementChunk;
typedef struct VgPlacements {
    u32 inst_cap, cell_cap;      // region capacity (vgeo_scene_reserve_placements; applied at the next commit)
    VgInstance *inst;            // [inst_cap]
    VgCell *cells;               // [cell_cap]: first relative to the region
    u32 *cell_chunk;             // [cell_cap]: owning chunk, ~0 = free
    VgRange *free_inst;          // free instance ranges, sorted, merged
    u32 free_inst_n, free_inst_cap;
    u32 *free_cells;             // stack of free cell indices
    u32 free_cells_n;
    VgPlacementChunk *chunks;
    u32 chunk_count, chunk_cap, live_chunks, live_instances;
    VgRange *dirty_inst;         // region ranges to upload at the next record
    u32 dirty_inst_n, dirty_inst_cap;
    u32 *dirty_cells;
    u32 dirty_cells_n, dirty_cells_cap;
    VgBuffer staging[3];         // per frame in flight (VG_MAX_FRAMES)
    u32 gpu_inst_cap, gpu_cell_cap;   // the region the buffers hold since the last commit
    u64 uploads, upload_bytes;   // stats
} VgPlacements;

typedef struct VgScene {
    const VgContext *ctx;
    VgMesh *meshes;
    u32 mesh_count, mesh_cap;
    VgNode *nodes;
    u32 node_count, node_cap;
    VgGroup *groups;
    u32 group_count, group_cap;
    u32 page_count, page_cap;
    u32 *page_table;             // global page -> slot (~0 = not resident): CPU mirror of b_page_table
    VgPageDesc *page_desc;       // per global page: offset / size in its mesh's source, groups
    u32 *page_mesh;              // per global page: mesh index
    u8 *page_root;               // per global page: 1 = root page (pinned, resident from the commit)
    u32 *meta_off;               // per global page: offset of its header + clusters in meta
    u8 *meta;
    size_t meta_size, meta_cap;
    VgPageSource *sources;       // per mesh
    u32 root_count;              // root pages over all meshes
    u32 pool_budget;             // slots requested (0 = every page resident)
    u32 pool_slots;              // slots of b_pool after the commit
    u32 *slot_page;              // slot -> global page (~0 = free)
    u32 stamp_frame;             // written by the streamer: the cull stamps used pages with it (b_stamps)
    u32 *mesh_depth;             // BVH depth per mesh
    VgInstance *instances;       // sorted by cell after commit
    u32 instance_count, instance_cap;
    VgCell *cells;
    u32 cell_count;
    u32 max_depth;               // deepest mesh BVH (traversal dispatches per pass)
    u64 cluster_count, tri_count;
    VgMaterial *materials;       // [0] = default grey
    u32 material_count, material_cap;
    u32 *mesh_mat_count;         // per mesh: material count (the commit derives the mesh's foliage fields from them)
    bool has_two_sided;          // a two-sided material is in use (the raster's second HW draw)
    bool has_masked, has_impostors;   // masked / dithered materials, impostor atlases (the raster's SW variants)
    float fol_fade_max, fol_shadow_max;   // largest foliage fade end / shadow distance (0: one is unlimited)
    VgPlacements pl;
    VgBuffer b_meshes, b_nodes, b_groups, b_page_table, b_pool, b_instances, b_cells, b_materials;
    VgBuffer b_stamps;           // u32 per pool slot: last frame a leaf emitted one of its groups (binding 20)
    u32 generation;              // bumps on every commit (cullers rewrite their descriptor sets)
    const struct VgPalettes *palettes;   // skinned instances (vgeo_skin.h, vg_palettes_attach); NULL: none
} VgScene;

void vgeo_scene_init(VgScene *s, const VgContext *ctx);
void vgeo_scene_free(VgScene *s);
// Registers a built mesh (copies its tables, page meta and page data). Returns the mesh index.
u32 vgeo_scene_add_mesh(VgScene *s, const VgBuilt *b);
// The same with the page payloads left in a .vgeo file: b must be the vgeo_deserialize of that file
// (its section table locates the page data); pages are read back with a file-range read.
u32 vgeo_scene_add_mesh_file(VgScene *s, const VgBuilt *b, const char *path);
// Pool budget in slots for the next commit (0 = every page resident; clamped to [root pages, pages]).
void vgeo_scene_set_pool(VgScene *s, u32 slots);
// Reads a page payload (desc size bytes) from its source into dst. Thread-safe.
bool vgeo_scene_read_page(const VgScene *s, u32 page, void *dst);
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
// Also derives every mesh's foliage fields (VgMesh max_wpo / fade / shadow_max_dist / fflags) and
// every material's declared wpo_max from the materials.
bool vgeo_scene_commit(VgScene *s);

// ------------------------------------------------------------------ placement chunks (Phase 7)

// Capacity of the placement region (instances, cells); takes effect at the next commit. Chunks
// loaded so far are kept when it grows.
void vgeo_scene_reserve_placements(VgScene *s, u32 instances, u32 cells);
// Adds a chunk: cells[ncells] (first / count relative to inst; bounds are recomputed from the
// instance mesh spheres + max WPO) over inst[n] (mesh = scene mesh index). Returns the chunk handle,
// ~0 when the region is full. Visible from the next vgeo_scene_placement_record.
u32 vgeo_scene_placement_add(VgScene *s, const VgCell *cells, u32 ncells, const VgInstance *inst, u32 n, u32 user);
void vgeo_scene_placement_remove(VgScene *s, u32 chunk);
// Uploads the chunk changes since the last record (frame < VG_MAX_FRAMES: its staging buffer is free
// again); before the frame's first cull. A barrier orders it after every earlier read.
void vgeo_scene_placement_record(VgScene *s, VkCommandBuffer cmd, u32 frame);
// Allocator audit: live chunks + free ranges cover the region exactly once, cells likewise. false +
// message on the first violation.
bool vgeo_scene_placement_audit(const VgScene *s, char *err, int err_size);
// GPU tables: instances / cells including the placement region (what the views cull).
DG_INLINE u32 vgeo_scene_gpu_instances(const VgScene *s) { return s->instance_count + s->pl.gpu_inst_cap; }
DG_INLINE u32 vgeo_scene_gpu_cells(const VgScene *s) { return s->cell_count + s->pl.gpu_cell_cap; }
// Instance i of the GPU table (static or placement region), NULL past the end.
const VgInstance *vgeo_scene_instance(const VgScene *s, u32 i);
const VgCell *vgeo_scene_gpu_cell(const VgScene *s, u32 i, VgCell *tmp);

// ------------------------------------------------------------------ CPU reference (tests)

// Decoded instance transform, exactly as the shaders decode it.
void vgeo_instance_decode(const VgInstance *in, float pos[3], float rot[4], float *scale);
// The cluster record of a pool slot (a zeroed record when the slot is free).
const VgCluster *vgeo_scene_cluster(const VgScene *s, u32 slot, u32 index);
// Header / cluster record of a global page (resident or not).
const VgPageHeader *vgeo_scene_page_header(const VgScene *s, u32 page);
const VgCluster *vgeo_scene_page_cluster(const VgScene *s, u32 page, u32 index);

typedef struct VgRefCluster {
    u32 instance, packed;        // packed = page slot << VG_CLUSTER_BITS | cluster (slot bits all set: not resident)
    u32 page;                    // global page
    float lod_margin;            // min |px - threshold| / threshold of the two LOD tests (borderline when tiny)
    i8 frustum;                  // 1 inside, 0 outside, -1 within the borderline band of a plane
} VgRefCluster;
// LOD selection over every instance (no node pruning): cluster c of group g is selected when
// px(g.parent_error, g.lod) > t and (px(c.error, c.lod) <= t or its child group's page is missing).
// With frustum, the culling sphere is also classified against the view planes. Skinned instances
// use the slot's current palettes as the shaders do (vgeo_skin.h: sphere transform, error scale,
// bone-box culling spheres; empty slots select nothing). Returns the count.
u32 vgeo_scene_cpu_select(const VgScene *s, const VgViewGpu *v, VgRefCluster *out, u32 cap);
// all_resident: as if every page were resident (the converged streaming cut).
u32 vgeo_scene_cpu_select_ex(const VgScene *s, const VgViewGpu *v, bool all_resident, VgRefCluster *out, u32 cap);

// View constants from a view-projection matrix (planes, viewport) and the error settings.
void vgeo_view_setup(VgViewGpu *v, const float view_proj[16], const float prev_view_proj[16], const float cam_pos[3], float proj_scale, float znear, u32 w,
                     u32 h, float error_px, float sw_px, u32 flags);

#endif
