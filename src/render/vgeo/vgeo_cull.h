// Virtualized geometry GPU culling and LOD selection (docs/VGEO.md "Culling"), per view:
//
//   cells (frustum + last frame's Hi-Z) -> instances -> BVH traversal, one indirect dispatch per
//   depth (level-synchronous, ping-pong node queues) -> cluster selection (DAG cut) + frustum / cone /
//   Hi-Z -> HW / SW visible lists; occluded cells / instances / nodes / clusters are re-tested in pass 2
//   against this frame's Hi-Z (VGV_TWO_PASS).
//
// The CPU records a fixed number of dispatches (it depends on the deepest BVH, not on the instance
// count) and never reads anything back in the frame: counters are copied to a per-frame host buffer
// and read when that frame slot comes round again.
#ifndef DG_VGEO_CULL_H
#define DG_VGEO_CULL_H

#include "vgeo_scene.h"

#define VG_MAX_FRAMES 3

typedef struct VgCullCaps { u32 nodes, groups, visible, occluded, requests; } VgCullCaps;

// A Hi-Z pyramid to test against (NULL view: none, occlusion off for that pass). w, h: the depth
// buffer it was built from (the pyramid's mip 0 is half of it, rounded up); mips: its levels.
typedef struct VgHizRef { VkImageView view; VkImageLayout layout; u32 w, h, mips; } VgHizRef;

enum VgTimestamp { VGT_BEGIN, VGT_INSTANCES, VGT_TRAVERSE, VGT_CLUSTERS, VGT_PASS2, VGT_COUNT };

typedef struct VgViewRes {
    bool allocated;
    u32 cells_cap, inst_cap;
    VgBuffer counters, cell_list, inst_list, node_queue, group_items, visible, occluded, requests;
    VgBuffer readback[VG_MAX_FRAMES];
    bool readback_valid[VG_MAX_FRAMES];
    VkDescriptorSet set[VG_MAX_FRAMES];
    u32 set_gen[VG_MAX_FRAMES];
    VkImageView set_prev[VG_MAX_FRAMES], set_cur[VG_MAX_FRAMES];
    VkBuffer set_vis[VG_MAX_FRAMES];
    VgCounters last;             // the most recent completed frame's counters
    bool last_valid;
} VgViewRes;

typedef struct VgCuller {
    const VgContext *ctx;
    VgScene *scene;
    VgCullCaps caps;
    VkDescriptorSetLayout layout, hiz_layout;
    VkPipelineLayout pl, hiz_pl;
    VkDescriptorPool pool;
    VkPipeline p_args, p_cells, p_inst, p_trav, p_clus, p_hiz;
    VkSampler sampler;
    VgBuffer views;              // VgViewGpu[VG_MAX_VIEWS] (vkCmdUpdateBuffer per record)
    VgViewRes v[VG_MAX_VIEWS];
    VkImage dummy;
    VmaAllocation dummy_alloc;
    VkImageView dummy_view;
    VkDescriptorSet hiz_sets[VG_MAX_FRAMES][16];
    VkQueryPool qp;              // VGT_COUNT timestamps per (frame, view)
    bool qp_written[VG_MAX_FRAMES][VG_MAX_VIEWS];
    u32 dispatches;              // dispatches recorded by the last vg_cull_record
    VkBuffer vis_buffer;         // binding 18 (raster's visibility buffer; a dummy until set)
    VgBuffer dummy_buf;
    bool ok;
} VgCuller;

bool vg_cull_init(VgCuller *c, const VgContext *ctx, VgScene *scene, const VgCullCaps *caps);
void vg_cull_shutdown(VgCuller *c);

// Records the culling of one view. `view` comes from vgeo_view_setup; counts, caps and Hi-Z info are
// filled here. frame < ctx->frames selects the per-frame descriptor set / readback.
void vg_cull_record(VgCuller *c, VkCommandBuffer cmd, u32 frame, u32 slot, const VgViewGpu *view, const VgHizRef *prev, const VgHizRef *cur);

// Builds a MAX Hi-Z (mip views of a half-resolution R32F image in GENERAL layout) from a w x h depth view.
void vg_hiz_record(VgCuller *c, VkCommandBuffer cmd, u32 frame, VkImageView depth, VkImageLayout depth_layout, const VkImageView *mips, u32 mip_count,
                   u32 w, u32 h);

// The visibility buffer bound at binding 18 of every set (Phase 3 raster / resolve share the cull
// layout and sets: vg_cull_set after vg_cull_record of the same frame / slot).
void vg_cull_set_vis(VgCuller *c, VkBuffer vis);
VkDescriptorSet vg_cull_set(const VgCuller *c, u32 frame, u32 slot);

// Counters of the last completed frame for a view (false before the first one).
bool vg_cull_counters(const VgCuller *c, u32 slot, VgCounters *out);
// Collects frame `frame`'s readback (call once that frame's fence has signalled; the renderer does it
// at the start of the next record of the same slot).
void vg_cull_collect(VgCuller *c, u32 frame);
// GPU ms per stage (instances, traverse, clusters, pass 2) of frame `frame` / view `slot`; false
// when not available yet. wait: block for the results (bench / tests).
bool vg_cull_timings(const VgCuller *c, u32 frame, u32 slot, bool wait, float ms[4]);

#endif
