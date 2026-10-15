// Frame graph: per-frame pass scheduler for the post / screen-space pipeline.
//
//   fg_begin()                                   new frame (render size, output size)
//   FgRes r = fg_texture(&desc)                  transient texture (pooled, aliased by lifetime)
//   FgRes h = fg_history(&desc)                  persistent texture (kept across frames)
//   FgRes i = fg_import(...)                     externally owned image (HDR, depth, swapchain)
//   int p = fg_pass("ssr", exec, user)           pass (skipped when disabled: r_post_disable)
//   fg_read(p, r, FG_SAMPLED) / fg_write(p, r, FG_STORAGE) ...
//   fg_execute(cmd)                              allocate, barrier, time and run every pass in order
//
// Transient textures with the same description share physical images when their lifetimes (first
// to last use pass) don't overlap. Every pass gets GPU timestamps (fg_pass_ms) and resources can
// be looked up by name for debug views (fg_find). Mip chains (Hi-Z, bloom) live in GENERAL layout
// and synchronise their own mips with fg_mip_barrier inside the pass.
#ifndef DG_FG_H
#define DG_FG_H

#include "render/vk_internal.h"

#define FG_MAX_PASSES 64
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
void fg_execute(VkCommandBuffer cmd);

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

// Diagnostics.
FgRes fg_find(const char *name);  // this frame's resource by name (debug views)
int fg_pass_count(void);
const char *fg_pass_name(int i);
float fg_pass_ms(int i);          // GPU time of the pass (last completed frame)
bool fg_pass_enabled(const char *name);
u64 fg_memory_bytes(void);        // pooled transient + history memory
int fg_pool_images(void);

#endif
