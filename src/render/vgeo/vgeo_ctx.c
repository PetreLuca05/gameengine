#include "vgeo_ctx.h"
#include "core/log.h"
#include <string.h>

VgBuffer vgc_buffer(const VgContext *c, VkDeviceSize size, VkBufferUsageFlags usage, VgMem mem) {
    VgBuffer b;
    memset(&b, 0, sizeof(b));
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size ? size : 16;
    bi.usage = usage;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (mem == VGM_UPLOAD) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else if (mem == VGM_READBACK) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &b.buffer, &b.alloc, &info) != VK_SUCCESS) {
        log_error("vgeo: cannot allocate a %.1f MB buffer", (double)bi.size / (1024.0 * 1024.0));
        memset(&b, 0, sizeof(b));
        return b;
    }
    b.mapped = info.pMappedData;
    b.size = bi.size;
    return b;
}

void vgc_buffer_free(const VgContext *c, VgBuffer *b) {
    if (b->buffer) vmaDestroyBuffer(c->vma, b->buffer, b->alloc);
    memset(b, 0, sizeof(*b));
}

VkCommandBuffer vgc_begin_once(const VgContext *c, VkCommandPool *pool) {
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = c->queue_family;
    vkCreateCommandPool(c->device, &pci, NULL, pool);
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = *pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(c->device, &ai, &cmd);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void vgc_submit_wait(const VgContext *c, VkCommandPool pool, VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    vkCreateFence(c->device, &fci, NULL, &fence);
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    vkQueueSubmit2(c->queue, 1, &si, fence);
    vkWaitForFences(c->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(c->device, fence, NULL);
    vkDestroyCommandPool(c->device, pool, NULL);
}

void vgc_upload(const VgContext *c, VgBuffer *dst, VkDeviceSize offset, const void *data, VkDeviceSize size) {
    const VkDeviceSize chunk = 64ull << 20;
    VgBuffer st = vgc_buffer(c, size < chunk ? size : chunk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_UPLOAD);
    for (VkDeviceSize done = 0; done < size;) {
        VkDeviceSize n = size - done < chunk ? size - done : chunk;
        memcpy(st.mapped, (const u8 *)data + done, (size_t)n);
        vmaFlushAllocation(c->vma, st.alloc, 0, n);
        VkCommandPool pool;
        VkCommandBuffer cmd = vgc_begin_once(c, &pool);
        VkBufferCopy r = { 0, offset + done, n };
        vkCmdCopyBuffer(cmd, st.buffer, dst->buffer, 1, &r);
        vgc_submit_wait(c, pool, cmd);
        done += n;
    }
    vgc_buffer_free(c, &st);
}

VkPipeline vgc_compute_pipeline(const VgContext *c, VkPipelineLayout layout, const char *shader) {
    VkShaderModule m = c->load_shader(shader);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) log_error("vgeo: pipeline %s failed", shader);
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

void vgc_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}
