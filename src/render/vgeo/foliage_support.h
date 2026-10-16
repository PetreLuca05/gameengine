// Virtualized geometry foliage support (Phase 7, docs/VGEO.md "Foliage support"): what a foliage
// system needs from the vgeo core, without content of its own:
//   - WPO snippets: CPU mirror of shaders/vgeo/wpo.glsl and the declared bound of each (vgeo_wpo_max,
//     applied by vgeo_scene_commit to every material and mesh bound)
//   - instance fade / density (r_foliage_density, a fixed hash per instance) and the impostor switch:
//     CPU mirrors of vg_foliage_alpha / vg_use_impostor
//   - placement chunks: the .vgp file (header, mesh names, cells, 32 B instances) and its loading into
//     the scene's placement region (vgeo_scene_placement_*), foliage_query over the loaded cells
//   - impostor atlases on the GPU (vgeo_impostor.h bakes them)
//   - the displacement buffer: 128 x 128 RG16F, 0.5 m texels around the camera, toroidal, decaying;
//     vg_displace_push is the writer API (players, vehicles, explosions flatten grass), WPO_DISPLACE reads it
#ifndef DG_FOLIAGE_SUPPORT_H
#define DG_FOLIAGE_SUPPORT_H

#include "vgeo_cull.h"

// ------------------------------------------------------------------ WPO (wpo.glsl)

// Declared max displacement (m) of a material's snippet (wpo = VgWpo) for its parameters, at any wind.
float vgeo_wpo_max(u32 wpo, const float prm[4]);
// The snippet's offset at world position p (CPU mirror): instance position ip, vertex colour col (0..1),
// phase (0..1), wind (dir x, z, strength, gust), time t; disp: the displacement buffer's push at p
// (WPO_DISPLACE; NULL = none).
void vgeo_wpo_eval(u32 wpo, const float prm[4], const float p[3], const float ip[3], const float col[4], float phase, const float wind[4], float t,
                   const float disp[2], float out[3]);

// ------------------------------------------------------------------ instances

// vg_foliage_alpha: fade alpha of an instance (flags_phase fp) at distance dist (0 culled).
float vgeo_foliage_alpha(const VgMesh *m, u32 fp, float dist, float density);
// vg_use_impostor for the view v and the instance's world sphere (WPO-inflated, as the culling uses it;
// the switch takes the margin off again).
bool vgeo_use_impostor(const VgViewGpu *v, const VgMesh *m, const float sphere[4]);
// vg_project_size (pixels across).
float vgeo_project_size(const VgViewGpu *v, const float sphere[4]);
// A foliage instance: rotation about y, wind phase and density hash fixed by its position (stable
// whatever order chunks load in).
void vgeo_foliage_instance(VgInstance *out, u32 mesh, const float pos[3], float yaw, float scale);

// ------------------------------------------------------------------ placement chunks (.vgp)

#define VGP_MAGIC 0x31504756u        // 'VGP1'
#define VGP_VERSION 1
#define VGP_CHUNK_SIZE 32.0f         // m: 4 x 4 cells of VGEO_CELL_SIZE
typedef struct VgpHeader {
    u32 magic, version, mesh_count, instance_count;
    i32 chunk_x, chunk_z;            // chunk coordinates (origin = chunk * VGP_CHUNK_SIZE)
    u32 cell_count, flags;           // cells <= 16
    float origin[3], size;           // chunk min corner (m), side (m)
} VgpHeader;
// File: VgpHeader, char names[mesh_count][48], VgCell cells[cell_count] (first / count into the
// chunk's instances, bounds informative: recomputed at load), VgInstance instances[instance_count]
// (mesh = index into names, grouped by cell).
typedef struct VgpView {
    const VgpHeader *hdr;
    const char (*names)[48];
    const VgCell *cells;
    const VgInstance *instances;
} VgpView;
bool vgp_parse(const u8 *data, size_t size, VgpView *out);
// Writes a chunk: instances (mesh = index into names) are binned into the chunk's 8 m cells (stable).
// Returns the malloc'd file, size in *size; NULL when an instance lies outside the chunk.
u8 *vgp_write(i32 cx, i32 cz, const char (*names)[48], u32 mesh_count, const VgInstance *inst, u32 n, size_t *size);

// Test scatter (foliage_test_scatter, the streamed terrain test, tests): a jittered grid over an area,
// deterministic per chunk (a chunk generates the same instances whenever it loads).
typedef struct VgScatter {
    float x0, z0, x1, z1;        // area (m)
    float density;               // instances per m2
    u32 kinds;                   // <= 8
    float weight[8];             // per kind: relative frequency
    float scale[8][2];           // per kind: scale range
    float (*height)(void *user, float x, float z);   // ground height (NULL: 0)
    void *user;
    u32 seed;
} VgScatter;
// The instances of 32 m chunk (cx, cz) inside the area (mesh = kind). Returns the count (<= cap).
u32 vgeo_foliage_scatter_chunk(const VgScatter *sc, i32 cx, i32 cz, VgInstance *out, u32 cap);

// Loads a parsed chunk into the scene's placement region: names resolve to scene meshes (resolve
// returns ~0 for unknown: those instances are dropped). Returns the chunk handle (~0: region full).
u32 vgeo_foliage_chunk_load(VgScene *s, const VgpView *v, u32 (*resolve)(const char *name, void *user), void *user, u32 tag);

typedef struct VgFoliageHit {
    u32 instance;                // GPU instance index
    u32 mesh;
    float pos[3], scale, dist;   // dist: horizontal distance to the query centre
} VgFoliageHit;
// Placement instances whose position lies within radius (horizontal) of pos, nearest first (footsteps,
// crushing, burning). Returns the count (<= max).
u32 vgeo_foliage_query(const VgScene *s, const float pos[3], float radius, VgFoliageHit *out, u32 max);

// ------------------------------------------------------------------ impostors on the GPU

// Uploads a mesh's impostor atlas (vgeo_impostor.h; with mips) into two module textures and points the
// mesh at them (VgMesh impostor / impostor_grid / impostor_radius; before the next commit).
bool vgeo_foliage_impostor_upload(VgScene *s, VgTextures *t, u32 mesh, const VgBuilt *b);
// The mesh switches to its impostor below scale x the view's switch size (r_vgeo_impostor_px): large,
// card-heavy meshes (trees) whose DAG cannot coarsen their cards switch sooner. 1/16 steps.
void vgeo_impostor_scale(VgScene *s, u32 mesh, float scale);

// ------------------------------------------------------------------ displacement buffer

#define VG_DISP_SIZE 128
#define VG_DISP_TEXEL 0.5f           // m
#define VG_DISP_PUSHES 64
typedef struct VgDisplace {
    const VgContext *ctx;
    VgTextures *tex;
    u32 tex_index;
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    VkDescriptorSetLayout layout;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorPool pool;
    VkDescriptorSet set[VG_MAX_FRAMES];
    VgBuffer pushes[VG_MAX_FRAMES];  // host: float4 (x, z, radius, strength) per push
    float queue[VG_DISP_PUSHES][4];
    u32 queued;
    i32 ox, oz;                      // window origin (texels), this frame
    bool valid;                      // the image holds a window (else cleared on the next record)
    bool ok;
} VgDisplace;
bool vg_displace_init(VgDisplace *d, const VgContext *ctx, VgTextures *t);
void vg_displace_shutdown(VgDisplace *d);
// Queues a push: pos (world), radius (m), strength (m of displacement per second, signed: < 0 pulls in).
void vg_displace_push(VgDisplace *d, const float pos[3], float radius, float strength);
// Re-centres the window on cam (toroidal: texels that change world cell are cleared), decays every
// texel, applies the queued pushes (dt seconds) and clears the queue; before the frame's culls.
void vg_displace_record(VgDisplace *d, VkCommandBuffer cmd, u32 frame, const float cam[3], float dt);
// The view fields WPO_DISPLACE reads (VgViewGpu disp / disp_tex).
void vg_displace_view(const VgDisplace *d, VgViewGpu *v);

#endif
