// Virtualized geometry, GPU side: the small device context the vgeo module runs on (docs/VGEO.md
// "GPU module"). The renderer fills it from its device (src/render/vk_vgeo.c); the window-less test
// harness and tools/bench_vgeo fill it from a device of their own. Nothing in the vgeo module touches
// the renderer's globals.
#ifndef DG_VGEO_CTX_H
#define DG_VGEO_CTX_H

#include "volk.h"
#include "vk_mem_alloc.h"
#include "vgeo_gpu.h"

typedef struct VgContext {
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    u32 queue_family;
    VmaAllocator vma;
    u32 frames;                  // frames in flight: per-frame descriptor sets / readbacks
    float timestamp_period;      // ns per tick (VkPhysicalDeviceLimits)
    // "vgeo/vg_traverse.comp" -> module (NULL on failure); the caller owns the returned module.
    VkShaderModule (*load_shader)(const char *name);
    // Capabilities (the creator enables them on the device when present).
    bool int64_atomics;          // shaderBufferInt64Atomics: the visibility buffer (Phase 3); vgeo off without it
    bool draw_indirect_count;
    bool mesh_shader;            // VK_EXT_mesh_shader (optional HW raster path)
    bool descriptor_indexing;
} VgContext;

typedef enum VgMem { VGM_DEVICE, VGM_UPLOAD, VGM_READBACK } VgMem;
typedef struct VgBuffer { VkBuffer buffer; VmaAllocation alloc; void *mapped; VkDeviceSize size; } VgBuffer;

VgBuffer vgc_buffer(const VgContext *c, VkDeviceSize size, VkBufferUsageFlags usage, VgMem mem);
void vgc_buffer_free(const VgContext *c, VgBuffer *b);
// Load-time copy into a device buffer through a staging buffer (blocks until done).
void vgc_upload(const VgContext *c, VgBuffer *dst, VkDeviceSize offset, const void *data, VkDeviceSize size);
VkPipeline vgc_compute_pipeline(const VgContext *c, VkPipelineLayout layout, const char *shader);
// One-shot command buffer on the context queue (load time / tests): begin, then submit + wait.
VkCommandBuffer vgc_begin_once(const VgContext *c, VkCommandPool *pool);
void vgc_submit_wait(const VgContext *c, VkCommandPool pool, VkCommandBuffer cmd);
// Compute / transfer / indirect -> compute / transfer / indirect memory dependency.
void vgc_barrier(VkCommandBuffer cmd);

#endif
