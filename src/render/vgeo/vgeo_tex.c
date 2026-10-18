#include "vgeo_tex.h"
#include "core/log.h"
#include <stdlib.h>
#include <string.h>

static void transition(VkCommandBuffer cmd, VkImage img, u32 mips, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src, VkAccessFlags2 src_a,
                       VkPipelineStageFlags2 dst, VkAccessFlags2 dst_a) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = src;
    b.srcAccessMask = src_a;
    b.dstStageMask = dst;
    b.dstAccessMask = dst_a;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

size_t vg_tex_mip_bytes(u32 w, u32 h, u32 *levels) {
    size_t n = 0;
    u32 l = 0;
    for (;;) {
        n += (size_t)w * h * 4;
        l++;
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (levels) *levels = l;
    return n;
}

u32 vg_tex_build_mips(const u8 *rgba, u32 w, u32 h, u8 *out) {
    u32 levels;
    vg_tex_mip_bytes(w, h, &levels);
    memcpy(out, rgba, (size_t)w * h * 4);
    const u8 *src = out;
    u8 *dst = out + (size_t)w * h * 4;
    for (u32 l = 1; l < levels; l++) {
        u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        for (u32 y = 0; y < nh; y++)
            for (u32 x = 0; x < nw; x++) {
                u32 c[3] = { 0, 0, 0 }, a = 0, cu[3] = { 0, 0, 0 };
                for (u32 k = 0; k < 4; k++) {
                    u32 sx = DG_MIN(x * 2 + (k & 1), w - 1), sy = DG_MIN(y * 2 + (k >> 1), h - 1);
                    const u8 *p = &src[((size_t)sy * w + sx) * 4];
                    for (int i = 0; i < 3; i++) { c[i] += (u32)p[i] * p[3]; cu[i] += p[i]; }
                    a += p[3];
                }
                u8 *o = &dst[((size_t)y * nw + x) * 4];
                for (int i = 0; i < 3; i++) o[i] = (u8)(a ? (c[i] + a / 2) / a : (cu[i] + 2) / 4);
                o[3] = (u8)((a + 2) / 4);
            }
        src = dst;
        dst += (size_t)nw * nh * 4;
        w = nw;
        h = nh;
    }
    return levels;
}

// `levels` levels of `fmt` packed in data (sizes[l] bytes each; NULL: RGBA8 sizes).
static bool create_image(VgTextures *t, VgOwnTex *o, u32 w, u32 h, u32 levels, const u8 *rgba, VkFormat fmt, const size_t *sizes) {
    const VgContext *c = t->ctx;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = { 0 };
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(c->vma, &ii, &ai, &o->image, &o->alloc, NULL) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = o->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(c->device, &vi, NULL, &o->view) != VK_SUCCESS) return false;
    size_t bytes = 0;
    for (u32 l = 0, lw = w, lh = h; l < levels; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) bytes += sizes ? sizes[l] : (size_t)lw * lh * 4;
    VgBuffer st = vgc_buffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VGM_UPLOAD);
    if (!st.buffer) return false;
    memcpy(st.mapped, rgba, bytes);
    vmaFlushAllocation(c->vma, st.alloc, 0, bytes);
    VkCommandPool pool;
    VkCommandBuffer cmd = vgc_begin_once(c, &pool);
    transition(cmd, o->image, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r[16];
    memset(r, 0, sizeof(r));
    size_t off = 0;
    u32 n = DG_MIN(levels, 16u);
    for (u32 l = 0, lw = w, lh = h; l < n; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) {
        r[l].bufferOffset = off;
        r[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r[l].imageSubresource.mipLevel = l;
        r[l].imageSubresource.layerCount = 1;
        r[l].imageExtent.width = lw;
        r[l].imageExtent.height = lh;
        r[l].imageExtent.depth = 1;
        off += sizes ? sizes[l] : (size_t)lw * lh * 4;
    }
    vkCmdCopyBufferToImage(cmd, st.buffer, o->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, r);
    transition(cmd, o->image, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vgc_submit_wait(c, pool, cmd);
    vgc_buffer_free(c, &st);
    o->w = w;
    o->h = h;
    o->mips = levels;
    o->format = fmt;
    return true;
}

static bool create_samplers(const VgContext *ctx, float anisotropy, float mip_bias, VkSampler *clamp, VkSampler *repeat) {
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 16.0f;
    si.mipLodBias = mip_bias;
    if (anisotropy > 1.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = anisotropy;
    }
    if (vkCreateSampler(ctx->device, &si, NULL, clamp) != VK_SUCCESS) return false;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(ctx->device, &si, NULL, repeat) != VK_SUCCESS) {
        vkDestroySampler(ctx->device, *clamp, NULL);
        return false;
    }
    return true;
}

void vg_tex_set_anisotropy(VgTextures *t, float anisotropy, float mip_bias) {
    if (!t->ctx) return;
    if (anisotropy <= 1.0f) anisotropy = 0.0f;
    if (anisotropy == t->anisotropy && mip_bias == t->mip_bias) return;
    VkSampler clamp, repeat;
    if (!create_samplers(t->ctx, anisotropy, mip_bias, &clamp, &repeat)) { log_warn("vgeo: texture samplers not recreated"); return; }
    for (u32 i = 0; i < VG_TEX_COUNT; i++) {
        VgTexEntry *e = &t->e[i];
        VkSampler s = e->sampler == t->clamp ? clamp : e->sampler == t->repeat ? repeat : VK_NULL_HANDLE;
        if (!s || !e->view) continue;
        e->sampler = s;
    }
    vkDestroySampler(t->ctx->device, t->clamp, NULL);
    vkDestroySampler(t->ctx->device, t->repeat, NULL);
    t->clamp = clamp;
    t->repeat = repeat;
    t->anisotropy = anisotropy;
    t->mip_bias = mip_bias;
    for (u32 i = 0; i < VG_TEX_COUNT; i++) t->ver[i]++;   // every entry: empty ones are written with clamp / repeat too
    t->gen++;
}

bool vg_tex_init(VgTextures *t, const VgContext *ctx) {
    memset(t, 0, sizeof(*t));
    t->ctx = ctx;
    if (!create_samplers(ctx, 0.0f, 0.0f, &t->clamp, &t->repeat)) return false;
    VgOwnTex w;
    memset(&w, 0, sizeof(w));
    const u8 white[4] = { 255, 255, 255, 255 };
    if (!create_image(t, &w, 1, 1, 1, white, VK_FORMAT_R8G8B8A8_UNORM, NULL)) { log_error("vgeo: texture table init failed"); return false; }
    t->white = w.image;
    t->white_alloc = w.alloc;
    t->white_view = w.view;
    return true;
}

void vg_tex_shutdown(VgTextures *t) {
    if (!t->ctx) return;
    VkDevice d = t->ctx->device;
    for (u32 i = 0; i < VG_TEX_OWN; i++) {
        VgOwnTex *o = &t->own[i];
        if (o->view) vkDestroyImageView(d, o->view, NULL);
        if (o->image) vmaDestroyImage(t->ctx->vma, o->image, o->alloc);
    }
    if (t->white_view) vkDestroyImageView(d, t->white_view, NULL);
    if (t->white) vmaDestroyImage(t->ctx->vma, t->white, t->white_alloc);
    if (t->clamp) vkDestroySampler(d, t->clamp, NULL);
    if (t->repeat) vkDestroySampler(d, t->repeat, NULL);
    memset(t, 0, sizeof(*t));
}

void vg_tex_set(VgTextures *t, u32 index, VkImageView view, VkSampler sampler, VkImageLayout layout) {
    if (index >= VG_TEX_COUNT) return;
    VgTexEntry *e = &t->e[index];
    if (!view) sampler = VK_NULL_HANDLE, layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (e->view == view && e->sampler == sampler && e->layout == layout) return;
    e->view = view;
    e->sampler = sampler;
    e->layout = layout;
    t->ver[index]++;
    t->gen++;
}

u32 vg_tex_alloc(VgTextures *t) {
    for (u32 i = 0; i < VG_TEX_OWN; i++)
        if (!t->own[i].used) {
            memset(&t->own[i], 0, sizeof(t->own[i]));
            t->own[i].used = true;
            return VG_TEX_GAME + i;
        }
    log_warn("vgeo: texture table full (%u module textures)", VG_TEX_OWN);
    return 0;
}

void vg_tex_release(VgTextures *t, u32 index) {
    if (index < VG_TEX_GAME || index >= VG_TEX_COUNT) return;
    VgOwnTex *o = &t->own[index - VG_TEX_GAME];
    vg_tex_set(t, index, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (o->image) {
        vkDeviceWaitIdle(t->ctx->device);   // load-time only: the image may be referenced by frames in flight
        vkDestroyImageView(t->ctx->device, o->view, NULL);
        vmaDestroyImage(t->ctx->vma, o->image, o->alloc);
    }
    memset(o, 0, sizeof(*o));
}

u32 vg_tex_create(VgTextures *t, u32 w, u32 h, u32 levels, const u8 *rgba, bool srgb, bool repeat) {
    u32 idx = vg_tex_alloc(t);
    if (!idx) return 0;
    VgOwnTex *o = &t->own[idx - VG_TEX_GAME];
    levels = DG_CLAMP(levels, 1u, 16u);
    u32 fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    size_t sizes[16];
    u8 *bc = t->compress ? t->compress(rgba, w, h, levels, srgb, &fmt, sizes) : NULL;
    bool ok = create_image(t, o, w, h, levels, bc ? bc : rgba, (VkFormat)fmt, bc ? sizes : NULL);
    free(bc);
    if (!ok) {
        log_error("vgeo: texture %ux%u upload failed", w, h);
        vg_tex_release(t, idx);
        return 0;
    }
    o->used = true;
    vg_tex_set(t, idx, o->view, repeat ? t->repeat : t->clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return idx;
}

void vg_tex_write_set(const VgTextures *t, VkDescriptorSet set, u32 *seen) {
    VkDescriptorImageInfo ii[64];
    VkWriteDescriptorSet w[64];
    u32 n = 0;
    for (u32 i = 0; i < VG_TEX_COUNT; i++) {
        if (seen[i] == t->ver[i]) continue;
        const VgTexEntry *e = &t->e[i];
        ii[n] = (VkDescriptorImageInfo){ e->view ? e->sampler : t->clamp, e->view ? e->view : t->white_view, e->view ? e->layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        if (!ii[n].sampler) ii[n].sampler = t->repeat;
        memset(&w[n], 0, sizeof(w[n]));
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set;
        w[n].dstBinding = VG_TEX_BINDING;
        w[n].dstArrayElement = i;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &ii[n];
        seen[i] = t->ver[i];
        if (++n == 64) { vkUpdateDescriptorSets(t->ctx->device, n, w, 0, NULL); n = 0; }
    }
    if (n) vkUpdateDescriptorSets(t->ctx->device, n, w, 0, NULL);
}
