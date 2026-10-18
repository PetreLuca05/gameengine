// Frame graph: per-frame pass scheduler for the post / screen-space pipeline.
//
//   fg_begin()                                   new frame (render size, output size)
//   FgRes r = fg_texture(&desc)                  transient texture (pooled, aliased by lifetime)
//   FgRes h = fg_history(&desc)                  persistent texture (kept across frames)
//   FgRes i = fg_import(...)                     externally owned image (HDR, depth, swapchain)
//   int p = fg_pass("ssr", exec, user)           pass (skipped when disabled: r_post_disable)
//   fg_read(p, r, FG_SAMPLED) / fg_write(p, r, FG_STORAGE) ...
//   cmd = fg_exec_begin(cmd)                     allocate, schedule (the work recorded so far is the "pre" batch)
//   cmd = fg_exec_until(marker) ... fg_exec_rest()   barrier, time and run every pass in order (the caller
//                                                records a marker pass's work in between)
//   fg_submit(...)                               end and submit every batch of both queues
//
// Async compute (PF-0027, docs/RENDERING_POST.md "Async compute", r_async_compute): compute passes
// marked fg_async run on a second queue (a compute-only family or a second graphics queue). Every
// cross-queue dependency (a resource used on both queues, fg_after; an async pass also follows the
// graphics passes declared before it) splits the queues' work into batches ordered by timeline
// semaphores; layout transitions for the async queue are recorded at the end of the graphics batch
// it waits for. The declared order stays the serial order: with the fix off (perf_fix_disable
// PF-0027) or no second queue everything runs on the graphics queue as before.
//
// Transient textures with the same description share physical images when their lifetimes (first
// to last use pass) don't overlap. Every pass gets GPU timestamps (fg_pass_ms) and resources can
// be looked up by name for debug views (fg_find). Mip chains (Hi-Z, bloom) live in GENERAL layout
// and synchronise their own mips with fg_mip_barrier inside the pass.
#ifndef DG_FG_H
#define DG_FG_H

#include "render/vk_internal.h"

#define FG_MAX_PASSES 96
#define FG_MAX_RES 96
#define FG_MAX_MIPS 12

typedef int FgRes;          // -1 = none
#define FG_NONE (-1)

typedef enum FgSize { FG_SIZE_RENDER, FG_SIZE_HALF, FG_SIZE_QUARTER, FG_SIZE_OUTPUT, FG_SIZE_ABS } FgSize;

typedef struct FgTexDesc {
    const char *name;
    VkFormat format;
    FgSize size;
    u32 w, h, d;            // FG_SIZE_ABS (d > 1 = 3D)
    u32 mips;               // 0 = 1, FG_MAX_MIPS = full chain
    float scale;            // optional extra scale on the size class (0 = 1)
} FgTexDesc;

typedef enum FgAccess {
    FG_SAMPLED,             // texture() in compute / fragment
    FG_STORAGE,             // imageStore / imageLoad (GENERAL)
    FG_COLOR,               // color attachment
    FG_DEPTH,               // depth attachment write
    FG_DEPTH_READ,          // depth attachment read-only + sampled
    FG_TRANSFER_SRC,
    FG_TRANSFER_DST,
} FgAccess;

struct FgPass;
typedef void (*FgExecFn)(VkCommandBuffer cmd, void *user);

void fg_init(void);
void fg_shutdown(void);
void fg_targets_changed(void);   // resolution change: drop pooled images and histories
void fg_begin(u32 render_w, u32 render_h, u32 output_w, u32 output_h);
FgRes fg_texture(const FgTexDesc *d);
FgRes fg_history(const FgTexDesc *d);
FgRes fg_import(const char *name, VkImage image, VkImageView view, VkFormat format, u32 w, u32 h, VkImageAspectFlags aspect, VkImageLayout *layout_io);
int fg_pass(const char *name, FgExecFn exec, void *user);
void fg_read(int pass, FgRes r, FgAccess a);
void fg_write(int pass, FgRes r, FgAccess a);
// A binding that is not read by this pass (bound early, written / read later): keeps the image alive
// and in state like fg_read, ignored by the frame graph validation (fg_check).
void fg_reference(int pass, FgRes r, FgAccess a);
// Async compute (PF-0027): a compute-only pass (push descriptors, dispatches, compute barriers through
// fg_stages) that may run on the async queue.
void fg_async(int pass);
// `pass` needs `dep` (declared earlier) complete without sharing a frame graph resource (persistent
// images, the shadow maps): orders them across the queues.
void fg_after(int pass, int dep);
// A graphics pass whose work the caller records between fg_exec_until and fg_exec_rest (the shadow
// maps); never disabled.
int fg_marker(const char *name);
int fg_find_pass(const char *name);   // this frame's pass by name, -1 if none
bool fg_async_frame(void);            // this frame runs async passes on the second queue (fg_begin decides)
void fg_async_info(void);             // logs the last frame's queue batches (r_async_info)
// Stage masks for barriers recorded by a pass's exec: limited to what the queue running it supports.
VkPipelineStageFlags2 fg_stages(VkPipelineStageFlags2 stages);
VkCommandBuffer fg_exec_begin(VkCommandBuffer cmd);
VkCommandBuffer fg_exec_until(int marker);
VkCommandBuffer fg_exec_rest(void);
// Ends every command buffer of the frame and submits the batches (one submit per queue, in order).
// The last graphics batch also waits `wait` (binary, at wait_stage), signals `signal` (binary) and
// `fence`.
void fg_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal, VkFence fence);

// Inside exec callbacks (and after fg_execute for debug views).
VkImageView fg_view(FgRes r);
VkImageView fg_mip_view(FgRes r, u32 mip);
VkImage fg_image(FgRes r);
VkFormat fg_format(FgRes r);
void fg_extent(FgRes r, u32 *w, u32 *h);
u32 fg_mips(FgRes r);
bool fg_history_valid(FgRes r);   // false on the first frame / after a resize (reset temporal state)
void fg_mip_barrier(VkCommandBuffer cmd, FgRes r);   // compute -> compute on the whole image (GENERAL)
bool fg_valid(FgRes r);
// The store op of an attachment of the executing pass: DONT_CARE for a transient texture no later
// pass of this frame uses (on tile-based GPUs, Apple / MoltenVK: the tile is never written back;
// docs/PLATFORM_MACOS.md "TBDR"), STORE otherwise (imports, histories, anything used later).
VkAttachmentStoreOp fg_store_op(FgRes r);

// Diagnostics.
FgRes fg_find(const char *name);  // this frame's resource by name (debug views)
int fg_pass_count(void);
const char *fg_pass_name(int i);
float fg_pass_ms(int i);          // GPU time of the pass (last completed frame)
bool fg_pass_enabled(const char *name);
u64 fg_memory_bytes(void);        // pooled transient + history memory
int fg_pool_images(void);

// Validation of the last executed graph (fg_check.h, docs/ENGINE_FIDELITY.md "No dead passes").
#define FG_REPORT_NAMES 32
typedef struct FgReport {
    int passes, post_passes, dead_passes, unused_res, undefined_reads;
    int pool_images, stale_images;          // stale: pooled, not used by this graph (freed by fg_trim / eviction)
    u64 pool_bytes, stale_bytes;
    char names[FG_MAX_PASSES][32];
    char post[FG_REPORT_NAMES][32];
    char dead[12][32];
    char unused[12][48];
    char undefined[12][48];
} FgReport;
const FgReport *fg_last_report(void);
void fg_trim(void);

#endif
