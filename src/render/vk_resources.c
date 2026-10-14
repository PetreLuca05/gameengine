// GPU resources: buffers, meshes, textures, samplers, deferred deletion and staged uploads.
#include "vk_internal.h"
#include <stdlib.h>
#include <math.h>

GpuBuffer vkr_buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    GpuBuffer b = { 0 };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(R->vma, &bci, &aci, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    b.size = size;
    return b;
}

void vkr_buffer_destroy_now(GpuBuffer *b) {
    if (b->buffer) vmaDestroyBuffer(R->vma, b->buffer, b->alloc);
    memset(b, 0, sizeof(*b));
}

static Deferred *push_deferred(void) {
    FrameResources *f = &R->frames[R->frame_slot];
    if (f->deferred_count >= MAX_DEFERRED) {
        // Should never happen; fall back to a full idle so the queue can drain.
        vkDeviceWaitIdle(R->device);
        vkr_flush_deferred(f);
    }
    return &f->deferred[f->deferred_count++];
}

void vkr_defer_buffer(GpuBuffer *b) {
    if (!b->buffer) return;
    Deferred *d = push_deferred();
    memset(d, 0, sizeof(*d));
    d->kind = DEFER_BUFFER;
    d->buffer = b->buffer;
    d->alloc = b->alloc;
    memset(b, 0, sizeof(*b));
}

void vkr_defer_image(VkImage image, VkImageView view, VmaAllocation alloc) {
    Deferred *d = push_deferred();
    memset(d, 0, sizeof(*d));
    d->kind = DEFER_IMAGE;
    d->image = image;
    d->view = view;
    d->alloc = alloc;
}

static void defer_descriptor(VkDescriptorSet set) {
    Deferred *d = push_deferred();
    memset(d, 0, sizeof(*d));
    d->kind = DEFER_DESCRIPTOR;
    d->set = set;
}

void vkr_flush_deferred(FrameResources *f) {
    for (int i = 0; i < f->deferred_count; i++) {
        Deferred *d = &f->deferred[i];
        switch (d->kind) {
        case DEFER_BUFFER: vmaDestroyBuffer(R->vma, d->buffer, d->alloc); break;
        case DEFER_IMAGE:
            if (d->view) vkDestroyImageView(R->device, d->view, NULL);
            vmaDestroyImage(R->vma, d->image, d->alloc);
            break;
        case DEFER_DESCRIPTOR: vkFreeDescriptorSets(R->device, R->pool, 1, &d->set); break;
        }
    }
    f->deferred_count = 0;
}

void vkr_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                       VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, u32 mips) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = mips ? mips : VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

RenderTarget vkr_target_create(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    RenderTarget t = { 0 };
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VK_CHECK(vmaCreateImage(R->vma, &ici, &aci, &t.image, &t.alloc, NULL));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &t.view));
    t.format = fmt;
    t.w = w;
    t.h = h;
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return t;
}

void vkr_target_destroy(RenderTarget *t) {
    if (t->view) vkDestroyImageView(R->device, t->view, NULL);
    if (t->image) vmaDestroyImage(R->vma, t->image, t->alloc);
    memset(t, 0, sizeof(*t));
}

void vkr_create_samplers(void) {
    VkSamplerCreateInfo s = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    s.magFilter = VK_FILTER_LINEAR;
    s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float aniso = (float)R->r_aniso->i;
    if (aniso > 1.0f && R->max_anisotropy > 1.0f) {
        s.anisotropyEnable = VK_TRUE;
        s.maxAnisotropy = fminf(aniso, R->max_anisotropy);
    }
    // Texture quality skips top mip levels (0 = high, 1 = medium, 2 = low).
    s.minLod = (float)DG_CLAMP(2 - R->r_texture_quality->i, 0, 2);
    s.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &R->sampler_linear));

    VkSamplerCreateInfo c = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    c.magFilter = VK_FILTER_LINEAR;
    c.minFilter = VK_FILTER_LINEAR;
    c.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    c.addressModeU = c.addressModeV = c.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    c.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(R->device, &c, NULL, &R->sampler_clamp));

    VkSamplerCreateInfo sh = c;
    sh.compareEnable = VK_TRUE;
    sh.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sh.addressModeU = sh.addressModeV = sh.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sh.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VK_CHECK(vkCreateSampler(R->device, &sh, NULL, &R->sampler_shadow));
}

void vkr_destroy_samplers(void) {
    vkDestroySampler(R->device, R->sampler_linear, NULL);
    vkDestroySampler(R->device, R->sampler_clamp, NULL);
    vkDestroySampler(R->device, R->sampler_shadow, NULL);
}

void vkr_write_tex_set(VkDescriptorSet set, VkImageView view, VkSampler sampler) {
    VkDescriptorImageInfo ii = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
}

VkDescriptorSet vkr_alloc_tex_set(VkImageView view, VkSampler sampler) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = R->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &R->tex_layout;
    VkDescriptorSet set;
    VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &set));
    vkr_write_tex_set(set, view, sampler);
    return set;
}

void vkr_rewrite_all_texture_sets(void) {
    for (u32 i = 1; i < MAX_TEXTURES; i++) {
        GpuTexture *t = &R->textures[i];
        if (t->used) vkr_write_tex_set(t->set, t->view, R->sampler_linear);
    }
}

// ------------------------------------------------------------------ meshes

static Upload *push_upload(void) {
    if (R->upload_count >= MAX_UPLOADS) {
        log_error("render: upload queue full");
        return NULL;
    }
    Upload *u = &R->uploads[R->upload_count++];
    memset(u, 0, sizeof(*u));
    return u;
}

static void stage_buffer_copy(VkBuffer dst, const void *data, VkDeviceSize size) {
    Upload *u = push_upload();
    if (!u) return;
    u->kind = UPLOAD_BUFFER;
    u->staging = vkr_buffer_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(u->staging.mapped, data, (size_t)size);
    u->dst = dst;
    u->size = size;
}

MeshId render_mesh_create(const Vertex *verts, u32 vcount, const u32 *indices, u32 icount) {
    if (!vcount || !icount) return 0;
    u32 slot = 0;
    for (u32 n = 0; n < MAX_MESHES - 1; n++) {
        u32 i = 1 + (R->mesh_free_hint + n) % (MAX_MESHES - 1);
        if (!R->meshes[i].used) { slot = i; break; }
    }
    if (!slot) { log_error("render: out of mesh slots"); return 0; }
    R->mesh_free_hint = slot;
    GpuMesh *m = &R->meshes[slot];
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->vertex_count = vcount;
    m->index_count = icount;
    VkDeviceSize vsize = sizeof(Vertex) * vcount, isize = sizeof(u32) * icount;
    m->vb = vkr_buffer_create(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    m->ib = vkr_buffer_create(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    stage_buffer_copy(m->vb.buffer, verts, vsize);
    stage_buffer_copy(m->ib.buffer, indices, isize);
    AABB b = { verts[0].pos, verts[0].pos };
    for (u32 i = 1; i < vcount; i++) { b.min = v3_min(b.min, verts[i].pos); b.max = v3_max(b.max, verts[i].pos); }
    m->bounds = b;
    R->stats.meshes_live++;
    return slot;
}

void render_mesh_destroy(MeshId id) {
    if (!id || id >= MAX_MESHES || !R->meshes[id].used) return;
    GpuMesh *m = &R->meshes[id];
    vkr_defer_buffer(&m->vb);
    vkr_defer_buffer(&m->ib);
    m->used = false;
    R->stats.meshes_live--;
}

AABB render_mesh_bounds(MeshId id) {
    AABB empty = { { 0, 0, 0 }, { 0, 0, 0 } };
    if (!id || id >= MAX_MESHES || !R->meshes[id].used) return empty;
    return R->meshes[id].bounds;
}

// ------------------------------------------------------------------ textures

static u32 alloc_texture_slot(void) {
    for (u32 i = 1; i < MAX_TEXTURES; i++) if (!R->textures[i].used) return i;
    log_error("render: out of texture slots");
    return 0;
}

static u32 mip_count(int w, int h) {
    u32 m = 1;
    int s = DG_MAX(w, h);
    while (s > 1) { s >>= 1; m++; }
    return m;
}

static TextureId texture_alloc(int w, int h, u32 mips, VkFormat fmt) {
    u32 slot = alloc_texture_slot();
    if (!slot) return 0;
    GpuTexture *t = &R->textures[slot];
    memset(t, 0, sizeof(*t));
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = (u32)w;
    ici.extent.height = (u32)h;
    ici.extent.depth = 1;
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(R->vma, &ici, &aci, &t->image, &t->alloc, NULL));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = t->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = mips;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &t->view));
    t->set = vkr_alloc_tex_set(t->view, R->sampler_linear);
    t->w = (u32)w;
    t->h = (u32)h;
    t->mips = mips;
    t->format = fmt;
    t->used = true;
    R->stats.textures_live++;
    return slot;
}

TextureId render_texture_create(int w, int h, const void *rgba8, bool srgb, bool mips) {
    u32 levels = mips ? mip_count(w, h) : 1;
    TextureId id = texture_alloc(w, h, levels, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
    if (!id) return 0;
    Upload *u = push_upload();
    if (!u) return id;
    VkDeviceSize size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    u->kind = UPLOAD_TEXTURE;
    u->staging = vkr_buffer_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(u->staging.mapped, rgba8, (size_t)size);
    u->texture = id;
    u->levels_in_data = 1;
    u->gen_mips = levels > 1;
    return id;
}

TextureId render_texture_create_raw(int w, int h, int levels, u32 vk_format, const void *data, const size_t *level_sizes) {
    levels = DG_CLAMP(levels, 1, 16);
    TextureId id = texture_alloc(w, h, (u32)levels, (VkFormat)vk_format);
    if (!id) return 0;
    size_t total = 0;
    for (int i = 0; i < levels; i++) total += level_sizes[i];
    Upload *u = push_upload();
    if (!u) return id;
    u->kind = UPLOAD_TEXTURE;
    u->staging = vkr_buffer_create(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(u->staging.mapped, data, total);
    size_t off = 0;
    for (int i = 0; i < levels; i++) { u->level_offsets[i] = off; off += level_sizes[i]; }
    u->texture = id;
    u->levels_in_data = levels;
    u->gen_mips = false;
    return id;
}

void render_texture_destroy(TextureId id) {
    if (!id || id >= MAX_TEXTURES || !R->textures[id].used) return;
    GpuTexture *t = &R->textures[id];
    defer_descriptor(t->set);
    vkr_defer_image(t->image, t->view, t->alloc);
    t->used = false;
    R->stats.textures_live--;
}

static void record_texture_upload(VkCommandBuffer cmd, Upload *u) {
    GpuTexture *t = &R->textures[u->texture];
    if (!t->used) return;
    vkr_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, t->mips);
    for (int lvl = 0; lvl < u->levels_in_data; lvl++) {
        VkBufferImageCopy region = { 0 };
        region.bufferOffset = u->level_offsets[lvl];
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = (u32)lvl;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = DG_MAX(1u, t->w >> lvl);
        region.imageExtent.height = DG_MAX(1u, t->h >> lvl);
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, u->staging.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    if (u->gen_mips) {
        int mw = (int)t->w, mh = (int)t->h;
        for (u32 i = 1; i < t->mips; i++) {
            VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t->image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = i - 1;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
            VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
            VkImageBlit blit = { 0 };
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1].x = mw; blit.srcOffsets[1].y = mh; blit.srcOffsets[1].z = 1;
            int nw = DG_MAX(1, mw / 2), nh = DG_MAX(1, mh / 2);
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1].x = nw; blit.dstOffsets[1].y = nh; blit.dstOffsets[1].z = 1;
            vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);
            mw = nw; mh = nh;
        }
        // All levels but the last are TRANSFER_SRC now; unify to TRANSFER_DST before the final transition.
        VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = t->mips - 1;
        b.subresourceRange.layerCount = 1;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    vkr_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, t->mips);
    t->ready = true;
}

void vkr_record_uploads(VkCommandBuffer cmd) {
    if (!R->upload_count) return;
    for (int i = 0; i < R->upload_count; i++) {
        Upload *u = &R->uploads[i];
        if (u->kind == UPLOAD_BUFFER) {
            VkBufferCopy c = { 0, 0, u->size };
            vkCmdCopyBuffer(cmd, u->staging.buffer, u->dst, 1, &c);
        } else {
            record_texture_upload(cmd, u);
        }
        vkr_defer_buffer(&u->staging);
    }
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    mb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    R->upload_count = 0;
}

void vkr_destroy_all_resources(void) {
    for (int i = 0; i < R->upload_count; i++) vkr_buffer_destroy_now(&R->uploads[i].staging);
    R->upload_count = 0;
    for (u32 i = 1; i < MAX_MESHES; i++) {
        GpuMesh *m = &R->meshes[i];
        if (!m->used) continue;
        vkr_buffer_destroy_now(&m->vb);
        vkr_buffer_destroy_now(&m->ib);
        m->used = false;
    }
    for (u32 i = 1; i < MAX_TEXTURES; i++) {
        GpuTexture *t = &R->textures[i];
        if (!t->used) continue;
        vkDestroyImageView(R->device, t->view, NULL);
        vmaDestroyImage(R->vma, t->image, t->alloc);
        t->used = false;
    }
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkr_flush_deferred(&R->frames[i]);
}
