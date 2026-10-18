// Alpha masks of masked material textures (PF-0014, docs/CLASSIC_RENDERER.md "Alpha masks"). The
// shadow and prepass alpha tests of leaf / grass cards only read alpha, yet sampled the full RGBA8
// albedo: an R8 copy of its alpha (every resident level, texel for texel: alpha_mask.comp) feeds them
// the same values at a quarter of the bytes. Built on the GPU at the start of the frame after a draw
// asked for one (until then the albedo is bound, as before), rebuilt when the texture's image changes
// (GpuTexture.serial: r_texture_quality trims, reloads, rewrapped module slots) or its sampler does.
#include "vk_internal.h"
#include "core/profiler.h"
#include <string.h>

#define AM_BUILDS_PER_FRAME 16

typedef struct AlphaMask {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    VkDescriptorSet set;   // set 1 layout (R->tex_layout): the mask with the source's sampler
    VkSampler sampler;
    u32 serial;            // the source's GpuTexture.serial it was built from
    u8 state;              // 0 none, 1 queued, 2 built
} AlphaMask;

static struct {
    bool init, ok;
    VkDescriptorSetLayout layout;
    VkPipelineLayout pl;
    VkPipeline pipe;
    AlphaMask m[MAX_TEXTURES];
    TextureId queue[MAX_TEXTURES];
    u32 queued;
} A;

static void am_init(void) {
    A.init = true;
    if (!R->has_push_descriptor) return;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(R->gpu, VK_FORMAT_R8_UNORM, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) { log_warn("alpha masks: R8 storage images unsupported, albedo alpha tested"); return; }
    VkDescriptorSetLayoutBinding b[2];
    memset(b, 0, sizeof(b));
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1] = b[0];
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &A.layout));
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &A.layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &A.pl));
    VkShaderModule sm = vkp_load_shader("alpha_mask.comp");
    if (!sm) return;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = sm;
    ci.stage.pName = "main";
    ci.layout = A.pl;
    VkResult r = vkCreateComputePipelines(R->device, R->pipeline_cache, 1, &ci, NULL, &A.pipe);
    vkDestroyShaderModule(R->device, sm, NULL);
    if (r != VK_SUCCESS) { log_error("alpha masks: pipeline failed (%d)", (int)r); return; }
    A.ok = true;
}

// A set 1 for the mask (R->pool); NULL when the pool is full (the albedo stays bound).
static VkDescriptorSet am_alloc_set(VkImageView view, VkSampler sampler) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = R->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &R->tex_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(R->device, &ai, &set) != VK_SUCCESS) {
        static bool warned;
        if (!warned) { warned = true; log_warn("alpha masks: descriptor pool full, some masked materials test their albedo"); }
        return VK_NULL_HANDLE;
    }
    vkr_write_tex_set(set, view, sampler);
    return set;
}

static void am_release(AlphaMask *m) {
    if (m->set) vkr_defer_descriptor(m->set);
    if (m->image) vkr_defer_image(m->image, m->view, m->alloc);
    memset(m, 0, sizeof(*m));
}

VkDescriptorSet vkam_set(TextureId t, VkSampler sampler) {
    if (!prof_fix_enabled(PFX_ALPHA_MASK) || !t || t >= MAX_TEXTURES) return VK_NULL_HANDLE;
    if (!A.init) am_init();
    if (!A.ok) return VK_NULL_HANDLE;
    const GpuTexture *g = &R->textures[t];
    AlphaMask *m = &A.m[t];
    if (m->state == 2 && m->serial == g->serial) {
        if (m->sampler != sampler) {   // r_aniso / r_texture_quality recreated the sampler: a fresh set (the old one may be in flight)
            vkr_defer_descriptor(m->set);
            m->set = am_alloc_set(m->view, sampler);
            m->sampler = sampler;
            if (!m->set) am_release(m);
        }
        return m->set;
    }
    if (m->state != 1 && g->used && g->ready && g->mips) {
        m->state = 1;
        m->sampler = sampler;   // built with the sampler the draw binds (module slots: their own repeat / clamp)
        A.queue[A.queued++] = t;
    }
    return VK_NULL_HANDLE;
}

// Builds the masks asked for since the last frame (at most AM_BUILDS_PER_FRAME), after the uploads.
void vkam_record(VkCommandBuffer cmd) {
    if (!A.queued) return;
    u32 n = 0, keep = 0;
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    bool bound = false;
    for (u32 q = 0; q < A.queued; q++) {
        TextureId t = A.queue[q];
        AlphaMask *m = &A.m[t];
        const GpuTexture *g = &R->textures[t];
        if (m->state != 1) continue;
        if (!g->used || !g->ready || !g->mips || !g->view) { am_release(m); continue; }
        if (n == AM_BUILDS_PER_FRAME) { A.queue[keep++] = t; continue; }
        if (!bound) {
            vkCmdPipelineBarrier2(cmd, &dep);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, A.pipe);
            bound = true;
        }
        VkSampler sampler = m->sampler;
        VkDescriptorSet old_set = m->set;
        if (m->image) vkr_defer_image(m->image, m->view, m->alloc);
        m->image = VK_NULL_HANDLE;
        u32 mips = DG_MIN(g->mips, 16u);
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8_UNORM;
        ici.extent = (VkExtent3D){ g->w, g->h, 1 };
        ici.mipLevels = mips;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        VmaAllocationCreateInfo aci = { 0 };
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(R->vma, &ici, &aci, &m->image, &m->alloc, NULL) != VK_SUCCESS) {
            if (old_set) vkr_defer_descriptor(old_set);
            memset(m, 0, sizeof(*m));
            continue;
        }
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = m->image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = mips;
        vci.subresourceRange.layerCount = 1;
        VkImageViewCreateInfo sv = vci;   // sampled: the alpha tests read .a (cl_mask_alpha), R8 has it in .r
        sv.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R };
        VK_CHECK(vkCreateImageView(R->device, &sv, NULL, &m->view));
        vkr_image_barrier(cmd, m->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, mips);
        for (u32 l = 0; l < mips; l++) {
            VkImageViewCreateInfo lv = vci;
            lv.subresourceRange.baseMipLevel = l;
            lv.subresourceRange.levelCount = 1;
            VkImageView level_view;
            VK_CHECK(vkCreateImageView(R->device, &lv, NULL, &level_view));
            VkDescriptorImageInfo src = { R->sampler_linear, g->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dst = { VK_NULL_HANDLE, level_view, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[2];
            memset(w, 0, sizeof(w));
            for (int i = 0; i < 2; i++) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstBinding = (u32)i;
                w[i].descriptorCount = 1;
            }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].pImageInfo = &src;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].pImageInfo = &dst;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, A.pl, 0, 2, w);
            u32 lw = DG_MAX(1u, g->w >> l), lh = DG_MAX(1u, g->h >> l);
            i32 pc[4] = { (i32)l, (i32)lw, (i32)lh, 0 };
            vkCmdPushConstants(cmd, A.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(cmd, (lw + 7) / 8, (lh + 7) / 8, 1);
            vkr_defer_image(VK_NULL_HANDLE, level_view, VK_NULL_HANDLE);
        }
        vkr_image_barrier(cmd, m->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mips);
        if (old_set) vkr_defer_descriptor(old_set);
        m->set = am_alloc_set(m->view, sampler);
        m->sampler = sampler;
        m->serial = g->serial;
        m->state = 2;
        if (!m->set) am_release(m);
        n++;
    }
    A.queued = keep;
}

// The material / module samplers were recreated (the caller waited for the device): queued masks hold
// the old sampler (asked for again at their next draw), built ones get a set with the new one then.
void vkam_samplers_changed(void) {
    for (u32 i = 0; i < MAX_TEXTURES; i++) {
        AlphaMask *m = &A.m[i];
        if (m->state == 1) m->state = 0;
        m->sampler = VK_NULL_HANDLE;
    }
    A.queued = 0;
}

// Texture `t` was destroyed: its mask goes too (a later texture in the slot builds its own).
void vkam_texture_destroyed(TextureId t) {
    if (t && t < MAX_TEXTURES && A.m[t].state) am_release(&A.m[t]);
}

void vkam_shutdown(void) {
    for (u32 i = 0; i < MAX_TEXTURES; i++) {
        AlphaMask *m = &A.m[i];
        if (m->view) vkDestroyImageView(R->device, m->view, NULL);
        if (m->image) vmaDestroyImage(R->vma, m->image, m->alloc);
        if (m->set) vkFreeDescriptorSets(R->device, R->pool, 1, &m->set);
    }
    if (A.pipe) vkDestroyPipeline(R->device, A.pipe, NULL);
    if (A.pl) vkDestroyPipelineLayout(R->device, A.pl, NULL);
    if (A.layout) vkDestroyDescriptorSetLayout(R->device, A.layout, NULL);
    memset(&A, 0, sizeof(A));
}
