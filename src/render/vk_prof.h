// GPU side of the profiler (docs/PERF_AUDIT.md "GPU"): while a capture records, every frame graph
// pass and the passes recorded outside it (shadows, uploads, particles) are GPU scopes with
// timestamps, pipeline statistics (vertex / fragment / compute invocations: overdraw and occupancy
// proxies) and the commands recorded in them (draws, dispatches, pipeline / descriptor binds, push
// constants, barriers, clears, copies). Results arrive two frames late and land in the frame they
// were recorded in. Nothing is recorded or changed while no capture runs.
#ifndef DG_VK_PROF_H
#define DG_VK_PROF_H

#include "volk.h"
#include "core/base.h"

typedef enum VkpCount {
    VKP_DRAW, VKP_DRAW_INDIRECT, VKP_DISPATCH, VKP_PIPELINE, VKP_DESCSET, VKP_PUSH, VKP_BARRIER, VKP_IMAGE_BARRIER,
    VKP_RENDERING, VKP_CLEAR, VKP_CLEAR_PX, VKP_COPY, VKP_COPY_BYTES,
    VKP_COUNT
} VkpCount;

void vkp_init(bool pipeline_stats);
void vkp_register(void);   // profile section "gpu" + capture hook
void vkp_shutdown(void);
void vkp_frame_begin(VkCommandBuffer cmd, u32 slot);   // after vkBeginCommandBuffer: read back the slot, reset its queries
void vkp_scope_begin(VkCommandBuffer cmd, const char *name);   // flat (a begin closes an open scope)
void vkp_scope_end(VkCommandBuffer cmd);

#ifdef DG_PROFILE
extern volatile int dg_prof_recording;
void vkp_note_slow(VkpCount c, u64 n);
DG_INLINE void vkp_note(VkpCount c, u64 n) { if (dg_prof_recording) vkp_note_slow(c, n); }

// Command counting: the render files call the volk entry points through these (vk_internal.h,
// vgeo_ctx.h include this header after volk.h). The originals are called as (vkCmdX)(...).
void vkp_begin_rendering(VkCommandBuffer cmd, const VkRenderingInfo *ri);
void vkp_pipeline_barrier2(VkCommandBuffer cmd, const VkDependencyInfo *dep);
void vkp_copy_buffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r);
#ifndef VKP_NO_COUNT
#define vkCmdDraw(...) (vkp_note(VKP_DRAW, 1), vkCmdDraw(__VA_ARGS__))
#define vkCmdDrawIndexed(...) (vkp_note(VKP_DRAW, 1), vkCmdDrawIndexed(__VA_ARGS__))
#define vkCmdDrawIndirect(...) (vkp_note(VKP_DRAW_INDIRECT, 1), vkCmdDrawIndirect(__VA_ARGS__))
#define vkCmdDrawIndexedIndirect(...) (vkp_note(VKP_DRAW_INDIRECT, 1), vkCmdDrawIndexedIndirect(__VA_ARGS__))
#define vkCmdDrawIndirectCount(...) (vkp_note(VKP_DRAW_INDIRECT, 1), vkCmdDrawIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(...) (vkp_note(VKP_DRAW_INDIRECT, 1), vkCmdDrawIndexedIndirectCount(__VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(...) (vkp_note(VKP_DRAW, 1), vkCmdDrawMeshTasksEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(...) (vkp_note(VKP_DRAW_INDIRECT, 1), vkCmdDrawMeshTasksIndirectEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(...) (vkp_note(VKP_DRAW_INDIRECT, 1), vkCmdDrawMeshTasksIndirectCountEXT(__VA_ARGS__))
#define vkCmdDispatch(...) (vkp_note(VKP_DISPATCH, 1), vkCmdDispatch(__VA_ARGS__))
#define vkCmdDispatchIndirect(...) (vkp_note(VKP_DISPATCH, 1), vkCmdDispatchIndirect(__VA_ARGS__))
#define vkCmdBindPipeline(...) (vkp_note(VKP_PIPELINE, 1), vkCmdBindPipeline(__VA_ARGS__))
#define vkCmdBindDescriptorSets(...) (vkp_note(VKP_DESCSET, 1), vkCmdBindDescriptorSets(__VA_ARGS__))
#define vkCmdPushDescriptorSetKHR(...) (vkp_note(VKP_DESCSET, 1), vkCmdPushDescriptorSetKHR(__VA_ARGS__))
#define vkCmdPushConstants(...) (vkp_note(VKP_PUSH, 1), vkCmdPushConstants(__VA_ARGS__))
#define vkCmdClearColorImage(...) (vkp_note(VKP_CLEAR, 1), vkCmdClearColorImage(__VA_ARGS__))
#define vkCmdClearDepthStencilImage(...) (vkp_note(VKP_CLEAR, 1), vkCmdClearDepthStencilImage(__VA_ARGS__))
#define vkCmdClearAttachments(...) (vkp_note(VKP_CLEAR, 1), vkCmdClearAttachments(__VA_ARGS__))
#define vkCmdFillBuffer(...) (vkp_note(VKP_CLEAR, 1), vkCmdFillBuffer(__VA_ARGS__))
#define vkCmdCopyImage(...) (vkp_note(VKP_COPY, 1), vkCmdCopyImage(__VA_ARGS__))
#define vkCmdCopyBufferToImage(...) (vkp_note(VKP_COPY, 1), vkCmdCopyBufferToImage(__VA_ARGS__))
#define vkCmdCopyImageToBuffer(...) (vkp_note(VKP_COPY, 1), vkCmdCopyImageToBuffer(__VA_ARGS__))
#define vkCmdBlitImage(...) (vkp_note(VKP_COPY, 1), vkCmdBlitImage(__VA_ARGS__))
#define vkCmdUpdateBuffer(...) (vkp_note(VKP_COPY, 1), vkCmdUpdateBuffer(__VA_ARGS__))
#define vkCmdBeginRendering(cmd, ri) vkp_begin_rendering((cmd), (ri))
#define vkCmdPipelineBarrier2(cmd, dep) vkp_pipeline_barrier2((cmd), (dep))
#define vkCmdCopyBuffer(cmd, s, d, n, r) vkp_copy_buffer((cmd), (s), (d), (n), (r))
#endif
#else
#define vkp_note(c, n) ((void)0)
#endif

#endif
