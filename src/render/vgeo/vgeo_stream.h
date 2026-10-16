// Virtualized geometry page streaming (docs/VGEO.md "Streaming"): the page pool holds a budget of
// 128 KiB slots; root pages are pinned from the commit, every other page is loaded on demand.
//
//   cull (frame N)     leaves stamp the slots of the groups they emit (b_stamps = N) and append a
//                      request (page, projected error) for each group whose page is missing
//   update (N + 1)     copies the stamps, request counts and lists of every view to a per-frame host
//                      buffer (read when that frame slot comes round again, after its fence)
//   update (N + 1 + F) reads them: dedupe, sort by priority, load up to pages_per_frame pages
//                      (parents first: a page is resident only with the pages of its parent clusters),
//                      copy staging -> pool slot, patch the page table with a compute scatter;
//                      LRU eviction of slots unused for F + 1 frames (never root pages, never a page
//                      a resident page depends on); an evicted slot is reused F frames later at the
//                      earliest, so no frame in flight can see its data change
//
// Memory sources load synchronously; file sources are read on a worker thread and copied the next
// frame. The update is recorded before the first cull of the frame (every view culls against it).
#ifndef DG_VGEO_STREAM_H
#define DG_VGEO_STREAM_H

#include "vgeo_cull.h"
#include <SDL3/SDL.h>

typedef struct VgStreamConfig {
    u32 pool_mb;                 // page pool budget (whole 128 KiB slots); applies at the next commit
    u32 pages_per_frame;         // loads issued per frame (capped by the value given to vg_stream_init)
} VgStreamConfig;

typedef struct VgStreamStats {
    u32 pool_slots, resident, free;   // now
    u32 requested;               // distinct missing pages in the request lists read this frame
    u32 loaded, evicted;         // this frame: pages patched in / out of the page table
    u32 waiting;                 // requests left for later (per-frame budget or pool full)
    u32 pending;                 // file reads in flight
    u64 loaded_total, evicted_total;
    float cpu_ms;                // vg_stream_update
    float gpu_ms;                // readback copies + page copies + page-table scatter (of frame slot read back)
} VgStreamStats;

typedef struct VgStreamLoad { const VgScene *scene; u32 page, slot, size, batch; void *dst; bool ok, async; } VgStreamLoad;
typedef struct VgStreamReq { float prio; u32 page; } VgStreamReq;

enum { VG_STREAM_RING = VG_MAX_FRAMES + 1 };

typedef struct VgStream {
    VgScene *scene;
    VgCuller *cull;
    const VgContext *ctx;
    VgStreamConfig cfg;
    u32 ppf_cap;                 // pages per frame the staging ring is sized for
    u32 frame;                   // internal frame counter (the GPU stamp value)
    u32 generation;              // scene generation the state belongs to
    u32 slots, pages;            // pool slots / pages of that generation
    // per slot
    u32 *last_used;              // CPU mirror of the GPU stamps (raised to the load frame)
    u32 *release;                // free slot: reusable from this frame on
    u32 *slot_page;              // page loading into / resident in the slot (~0 free)
    u8 *slot_state;              // VGSS_*
    // per page
    u8 *loading;
    u32 *dependents;             // resident / loading pages that depend on it
    u32 *parent_first, *parents; // CSR: pages holding the parent clusters of a page's clusters
    u32 *req_mark, *req_at;      // request dedupe: frame stamp, index into reqs
    VgStreamReq *reqs;
    u32 req_cap;
    // staging ring: VG_STREAM_RING regions of ppf_cap pages + patch entries (uvec2 page, slot)
    VgBuffer staging;
    VkDeviceSize region_size, patch_offset;
    u32 patch_cap;
    VgStreamLoad *loads;         // VG_STREAM_RING * ppf_cap
    u32 batch_count[VG_STREAM_RING];   // loads of the batch issued into a region
    SDL_AtomicInt batch_left[VG_STREAM_RING];   // file reads of the batch not done yet (copied by the next update)
    // readback per frame slot: stamps[slots], then per view { count, pad, requests[cap] }
    VgBuffer readback[VG_MAX_FRAMES];
    bool readback_valid[VG_MAX_FRAMES];
    u32 rb_views[VG_MAX_FRAMES]; // views copied (bit mask)
    VkDeviceSize rb_view_offset, rb_view_size;
    // page-table scatter
    VkDescriptorSetLayout layout;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkDescriptorSet set;
    VkPipeline p_patch;
    VkQueryPool qp;
    bool qp_written[VG_MAX_FRAMES];
    // file reader
    SDL_Thread *worker;
    SDL_Semaphore *work_sem;
    SDL_Mutex *work_mutex;
    u32 *work_queue, work_head, work_tail, work_cap;   // indices into loads
    SDL_AtomicInt quit;
    VgStreamStats stats;
    bool logged_fail, ok;
} VgStream;

// Sets the scene's pool budget (re-commits it when already committed) and creates the streamer.
// The culler provides the per-view request lists.
bool vg_stream_init(VgStream *st, VgScene *scene, VgCuller *cull, const VgStreamConfig *cfg);
void vg_stream_shutdown(VgStream *st);
// pages_per_frame applies now; pool_mb at the next vgeo_scene_commit.
void vg_stream_set_config(VgStream *st, const VgStreamConfig *cfg);
// Once per frame, before the first vg_cull_record(_pass1) of the frame, once frame slot `frame`'s
// fence has signalled (as for vg_cull_collect). Records the readback copies, the page uploads and
// the page-table patch into cmd, and sets the stamp the culls of this frame write.
void vg_stream_update(VgStream *st, VkCommandBuffer cmd, u32 frame);
const VgStreamStats *vg_stream_stats(const VgStream *st);
// Slots for a pool of pool_mb (whole 128 KiB slots, at least 1).
u32 vg_stream_pool_slots(u32 pool_mb);

#endif
