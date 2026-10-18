// GPU resources: buffers, meshes, textures, samplers, deferred deletion and staged uploads.
#include "vk_internal.h"
#include "core/profiler.h"
#include "vgeo/vgeo_tex.h"
#include "tex_compress.h"
#include <stdlib.h>
#include <math.h>

GpuBuffer vkr_buffer_create(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    GpuBuffer b = { 0 };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = usage;
    vkd_share_buffer(&bci);   // the async compute passes read the frame's buffers (PF-0027)
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(R->vma, &bci, &aci, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    b.size = size;
    return b;
}

// Host-cached memory for GPU -> CPU readback (write-combined memory is very slow to read).
GpuBuffer vkr_buffer_create_readback(VkDeviceSize size) {
    GpuBuffer b = { 0 };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
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
    // Inside a frame: this slot (freed after this frame's fence). Between frames (game ticks: chunk
    // streaming, map changes) frame_slot already names the next slot, whose fence is the frame
    // FRAMES_IN_FLIGHT ago: the most recent frame may still use the resource, so it goes to the slot
    // of that most recent frame.
    int slot = R->frame_active ? R->frame_slot : (R->frame_slot + FRAMES_IN_FLIGHT - 1) % FRAMES_IN_FLIGHT;
    FrameResources *f = &R->frames[slot];
    if (f->deferred_count >= MAX_DEFERRED) {
        // Should never happen; fall back to a full idle so the queue can drain.
        vkDeviceWaitIdle(R->device);
        vkr_flush_deferred(f);
    }
    return &f->deferred[f->deferred_count++];
}

void vkr_defer_buffer(GpuBuffer *b) {
    if (!b->buffer) return;
    // Drop staged copies into a buffer destroyed before its upload was recorded (e.g. a map
    // switch on the first frame); the copy would target a deleted VkBuffer.
    for (int i = 0; i < R->upload_count; i++) {
        Upload *u = &R->uploads[i];
        if (u->kind != UPLOAD_BUFFER || u->dst != b->buffer) continue;
        vkr_buffer_destroy_now(&u->staging);
        R->uploads[i--] = R->uploads[--R->upload_count];
    }
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

void vkr_defer_descriptor(VkDescriptorSet set) {
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

static RenderTarget target_create(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool shared) {
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
    if (shared) vkd_share_image(&ici);
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

RenderTarget vkr_target_create(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    return target_create(w, h, fmt, usage, aspect, false);
}
RenderTarget vkr_target_create_shared(u32 w, u32 h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    return target_create(w, h, fmt, usage, aspect, true);
}

void vkr_target_destroy(RenderTarget *t) {
    if (t->view) vkDestroyImageView(R->device, t->view, NULL);
    if (t->image) vmaDestroyImage(R->vma, t->image, t->alloc);
    memset(t, 0, sizeof(*t));
}

float vkr_material_anisotropy(void) {
    float aniso = (float)R->r_aniso->i;
    return aniso > 1.0f && R->max_anisotropy > 1.0f ? fminf(aniso, R->max_anisotropy) : 0.0f;
}

float vkr_material_mip_bias(void) {
    if (!prof_fix_enabled(PFX_UPSCALE_MIP_BIAS) || !R->hdr.w || !R->swap_extent.width || !R->port_mip_bias) return 0.0f;
    static Cvar *aa, *up;
    if (!aa) aa = cvar_find("r_aa");
    if (!up) up = cvar_find("r_upscaler");
    // temporal resolves only: TAA, TAA-U (r_upscaler 3 / 4 are slots that run FSR 1's spatial EASU for now)
    bool temporal = (aa && aa->i == 3) || (up && up->i == 1);
    float ratio = (float)R->hdr.w / (float)R->swap_extent.width;
    if (!temporal || ratio >= 0.999f) return 0.0f;
    return fmaxf(log2f(ratio), -1.0f);
}

// The material samplers: trilinear + anisotropic. sampler_linear skips the top mip levels for
// r_texture_quality (0 = high, 1 = medium, 2 = low) as a min LOD (texture arrays, impostors, UI
// atlases: their levels stay resident); sampler_world does not: world textures dropped those levels
// from memory (texture_drop), their level 0 is already the quality's top.
static void create_linear_sampler(void) {
    VkSamplerCreateInfo s = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    s.magFilter = VK_FILTER_LINEAR;
    s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float aniso = vkr_material_anisotropy();
    if (aniso > 0.0f) {
        s.anisotropyEnable = VK_TRUE;
        s.maxAnisotropy = aniso;
    }
    s.minLod = (float)DG_CLAMP(2 - R->r_texture_quality->i, 0, 2);
    s.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &R->sampler_linear));
    // the scene's materials below display resolution under a temporal resolve: the display's texel
    // density (PF-0029); the UI and the other sampler_linear users stay unbiased
    s.mipLodBias = R->mip_bias;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &R->sampler_material));
    s.minLod = 0.0f;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &R->sampler_world));
}

// r_aniso / r_texture_quality only shape the material samplers: recreate those (the clamp /
// shadow samplers stay, with every frame set, VFX and vgeo binding that holds them) and rewrite their
// users: the material texture sets and the vgeo texture table. The caller waits for the device.
void vkr_recreate_linear_sampler(void) {
    vkDestroySampler(R->device, R->sampler_linear, NULL);
    vkDestroySampler(R->device, R->sampler_material, NULL);
    vkDestroySampler(R->device, R->sampler_world, NULL);
    create_linear_sampler();
    vkr_rewrite_all_texture_sets();
    vkg_samplers_changed();
    vks_samplers_changed();
    vkt_samplers_changed();
}

void vkr_create_samplers(void) {
    create_linear_sampler();

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

    // Raw shadow depth (PCSS blocker search): nearest, no compare (D32 linear filtering is optional).
    VkSamplerCreateInfo dp = c;
    dp.magFilter = dp.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(R->device, &dp, NULL, &R->sampler_depth));
}

void vkr_destroy_samplers(void) {
    vkDestroySampler(R->device, R->sampler_linear, NULL);
    vkDestroySampler(R->device, R->sampler_material, NULL);
    vkDestroySampler(R->device, R->sampler_world, NULL);
    vkDestroySampler(R->device, R->sampler_clamp, NULL);
    vkDestroySampler(R->device, R->sampler_shadow, NULL);
    vkDestroySampler(R->device, R->sampler_depth, NULL);
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

static void write_mat_set(const struct MatSet *ms) {
    VkDescriptorImageInfo ii[4];
    for (int k = 0; k < 4; k++) ii[k] = (VkDescriptorImageInfo){ vkr_texture_sampler(ms->tex[k]), R->textures[ms->tex[k]].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = ms->set;
    w.dstBinding = 0;
    w.descriptorCount = 4;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
}

VkSampler vkr_texture_sampler(u32 id) {
    const GpuTexture *t = &R->textures[id < MAX_TEXTURES ? id : 0];
    return t->wrapped ? R->sampler_clamp : t->world ? R->sampler_world : R->sampler_linear;
}

void vkr_rewrite_all_texture_sets(void) {
    for (u32 i = 1; i < MAX_TEXTURES; i++) {
        GpuTexture *t = &R->textures[i];
        if (t->used) vkr_write_tex_set(t->set, t->view, vkr_texture_sampler(i));
    }
    for (u32 i = 0; i < R->mat_set_count; i++) write_mat_set(&R->mat_sets[i]);
}

// glTF PBR maps (docs/PBR.md): the set of the four map textures a material samples, created on first
// use. A map that is missing or still uploading is white (the normal map flat); once it is ready the
// material gets its own set. Entries go when one of their textures is destroyed.
static TextureId mat_tex(const Material *m, u32 flag, TextureId id, TextureId fallback) {
    return (m->flags & flag) && id && id < MAX_TEXTURES && R->textures[id].used && R->textures[id].ready ? id : fallback;
}

u32 vkr_material_set(const Material *m) {
    if (!R->mat_set_count) {   // [0]: no maps
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = R->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &R->mat_layout;
        struct MatSet *ms = &R->mat_sets[0];
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &ms->set));
        for (int k = 0; k < 4; k++) ms->tex[k] = R->white_tex;
        write_mat_set(ms);
        R->mat_set_count = 1;
    }
    if (!(m->flags & MAT_PBR_MAPS)) return 0;
    TextureId t[4] = { mat_tex(m, MAT_MR_TEX, m->mr_tex, R->white_tex), mat_tex(m, MAT_NORMAL_TEX, m->normal_tex, R->flat_normal_tex),
                       mat_tex(m, MAT_OCCLUSION_TEX, m->occlusion_tex, R->white_tex), mat_tex(m, MAT_EMISSIVE_TEX, m->emissive_tex, R->white_tex) };
    for (u32 i = 1; i < R->mat_set_count; i++)
        if (!memcmp(R->mat_sets[i].tex, t, sizeof(t))) return i;
    if (R->mat_set_count >= MAX_MAT_SETS) {
        static bool warned;
        if (!warned) { warned = true; log_warn("render: more than %d glTF map combinations, the rest draw without maps", MAX_MAT_SETS); }
        return 0;
    }
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = R->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &R->mat_layout;
    struct MatSet *ms = &R->mat_sets[R->mat_set_count];
    if (vkAllocateDescriptorSets(R->device, &ai, &ms->set) != VK_SUCCESS) return 0;
    memcpy(ms->tex, t, sizeof(t));
    write_mat_set(ms);
    return R->mat_set_count++;
}

// A texture goes away: drop the map sets that sample it (freed once the frames using them are done).
static void mat_sets_forget(TextureId id) {
    for (u32 i = 1; i < R->mat_set_count;) {
        struct MatSet *ms = &R->mat_sets[i];
        if (ms->tex[0] == id || ms->tex[1] == id || ms->tex[2] == id || ms->tex[3] == id) {
            vkr_defer_descriptor(ms->set);
            *ms = R->mat_sets[--R->mat_set_count];
        } else {
            i++;
        }
    }
}

static u32 alloc_texture_slot(void);

TextureId vkr_texture_wrap(VkImage image, VkImageView view, u32 w, u32 h, VkFormat fmt) {
    u32 slot = alloc_texture_slot();
    if (!slot) return 0;
    GpuTexture *t = &R->textures[slot];
    memset(t, 0, sizeof(*t));
    t->image = image;
    t->view = view;
    t->serial = ++R->tex_serial;
    t->set = vkr_alloc_tex_set(view, R->sampler_clamp);
    t->w = w;
    t->h = h;
    t->mips = 1;
    t->format = fmt;
    t->used = true;
    t->ready = true;
    t->wrapped = true;
    return slot;
}

void vkr_texture_rewrap(TextureId id, VkImage image, VkImageView view, u32 w, u32 h) {
    if (!id || id >= MAX_TEXTURES || !R->textures[id].used || !R->textures[id].wrapped) return;
    GpuTexture *t = &R->textures[id];
    t->image = image;
    t->view = view;
    t->serial = ++R->tex_serial;
    t->w = w;
    t->h = h;
    vkr_write_tex_set(t->set, view, R->sampler_clamp);
}

void vkr_texture_unwrap(TextureId id) {
    if (!id || id >= MAX_TEXTURES || !R->textures[id].used || !R->textures[id].wrapped) return;
    vkam_texture_destroyed(id);
    vkr_defer_descriptor(R->textures[id].set);
    memset(&R->textures[id], 0, sizeof(GpuTexture));
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

MeshId render_skinned_mesh_create(const SkinVertex *verts, u32 vcount, const u32 *indices, u32 icount) {
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
    m->skinned = true;
    m->vertex_count = vcount;
    m->index_count = icount;
    VkDeviceSize vsize = sizeof(SkinVertex) * vcount, isize = sizeof(u32) * icount;
    m->vb = vkr_buffer_create(vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    m->ib = vkr_buffer_create(isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    stage_buffer_copy(m->vb.buffer, verts, vsize);
    stage_buffer_copy(m->ib.buffer, indices, isize);
    m->cpu_verts = (SkinVertex *)malloc((size_t)vsize);
    memcpy(m->cpu_verts, verts, (size_t)vsize);
    m->cpu_indices = (u32 *)malloc((size_t)isize);
    memcpy(m->cpu_indices, indices, (size_t)isize);
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
    free(m->cpu_verts);
    free(m->cpu_indices);
    m->cpu_verts = NULL;
    m->cpu_indices = NULL;
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

// The texture's image and view (all levels); w / h / mips / format follow.
static void texture_image(GpuTexture *t, int w, int h, u32 mips, VkFormat fmt) {
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
    t->w = (u32)w;
    t->h = (u32)h;
    t->mips = mips;
    t->format = fmt;
    t->serial = ++R->tex_serial;
}

static TextureId texture_alloc(int w, int h, u32 mips, VkFormat fmt, bool world) {
    u32 slot = alloc_texture_slot();
    if (!slot) return 0;
    GpuTexture *t = &R->textures[slot];
    memset(t, 0, sizeof(*t));
    texture_image(t, w, h, mips, fmt);
    t->world = world;
    t->full_w = (u32)w;
    t->full_h = (u32)h;
    t->full_mips = mips;
    t->set = vkr_alloc_tex_set(t->view, vkr_texture_sampler(slot));
    t->used = true;
    R->stats.textures_live++;
    return slot;
}

// r_texture_quality (docs/TEXTURE_MIPS.md): the top levels of a world texture's chain of `mips` that
// stay out of memory (high 0, medium 1, low 2; the last level always stays).
static u32 texture_drop(u32 mips) {
    u32 d = (u32)DG_CLAMP(2 - R->r_texture_quality->i, 0, 2);
    return mips > 1 ? DG_MIN(d, mips - 1) : 0;
}

// One 2x2 box level of an RGBA8 image (floor sizes, as the mip chain). sRGB colour is averaged in
// linear light, as the GPU's mip blits filter it; alpha and UNORM data as they are. The block
// compressor builds its chains with the same filter (tex_compress.c).
static u8 *halve_rgba(const u8 *src, int w, int h, bool srgb, int *ow, int *oh) { return texc_halve(src, w, h, srgb, ow, oh); }

// Mip generation by blits needs blit support and linear filtering for the format (RGBA8: everywhere
// in practice); without it the chain is built on the CPU (vg_tex_build_mips) and uploaded as is.
static bool blit_mips_ok(VkFormat fmt) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(R->gpu, fmt, &p);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (p.optimalTilingFeatures & need) == need;
}

// Queues level 0 of texture `id` (its resident size) and the rest of its chain.
static void texture_queue_rgba(TextureId id, const u8 *rgba8) {
    GpuTexture *t = &R->textures[id];
    Upload *u = push_upload();
    if (!u) return;
    u->kind = UPLOAD_TEXTURE;
    u->texture = id;
    if (t->mips > 1 && !blit_mips_ok(t->format)) {
        static bool warned;
        if (!warned) { warned = true; log_warn("render: format %d has no linear blits: mip chains built on the CPU", (int)t->format); }
        u32 levels;
        size_t bytes = vg_tex_mip_bytes(t->w, t->h, &levels);
        u->staging = vkr_buffer_create(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        vg_tex_build_mips(rgba8, t->w, t->h, (u8 *)u->staging.mapped);
        size_t off = 0;
        u->levels_in_data = (int)DG_MIN(DG_MIN(levels, t->mips), 16u);
        for (int l = 0; l < u->levels_in_data; l++) {
            u->level_offsets[l] = off;
            off += (size_t)DG_MAX(1u, t->w >> l) * DG_MAX(1u, t->h >> l) * 4;
        }
        u->gen_mips = false;
        return;
    }
    VkDeviceSize size = (VkDeviceSize)t->w * (VkDeviceSize)t->h * 4;
    u->staging = vkr_buffer_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(u->staging.mapped, rgba8, (size_t)size);
    u->levels_in_data = 1;
    u->gen_mips = t->mips > 1;
}

// The image of world texture `id` at the current quality from its full-size pixels (w x h): the
// dropped levels are box-filtered away on the CPU, the rest is uploaded (and mipmapped) as usual.
static void world_fill_rgba(TextureId id, int w, int h, const u8 *rgba8, VkFormat fmt) {
    GpuTexture *t = &R->textures[id];
    u32 full = mip_count(w, h), drop = texture_drop(full);
    bool srgb = fmt == VK_FORMAT_R8G8B8A8_SRGB;
    const u8 *px = rgba8;
    u8 *tmp = NULL;
    int rw = w, rh = h;
    for (u32 i = 0; i < drop; i++) {
        int nw, nh;
        u8 *n = halve_rgba(px, rw, rh, srgb, &nw, &nh);
        free(tmp);
        px = tmp = n;
        rw = nw;
        rh = nh;
    }
    texture_image(t, rw, rh, full - drop, fmt);
    t->full_w = (u32)w;
    t->full_h = (u32)h;
    t->full_mips = full;
    t->dropped = drop;
    t->ready = false;
    texture_queue_rgba(id, px);
    free(tmp);
}

// Queues `levels` tightly packed levels (level_sizes) as the whole chain of texture `id`.
static void texture_queue_raw(TextureId id, int levels, const u8 *data, const size_t *level_sizes) {
    size_t total = 0;
    for (int i = 0; i < levels; i++) total += level_sizes[i];
    Upload *u = push_upload();
    if (!u) return;
    u->kind = UPLOAD_TEXTURE;
    u->staging = vkr_buffer_create(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(u->staging.mapped, data, total);
    size_t off = 0;
    for (int i = 0; i < levels; i++) { u->level_offsets[i] = off; off += level_sizes[i]; }
    u->texture = id;
    u->levels_in_data = levels;
    u->gen_mips = false;
}

// World texture `id` from a pre-built chain (KTX2): the dropped levels are skipped.
static void world_fill_raw(TextureId id, int w, int h, int levels, VkFormat fmt, const u8 *data, const size_t *level_sizes) {
    GpuTexture *t = &R->textures[id];
    u32 drop = texture_drop((u32)levels);
    size_t skip = 0;
    for (u32 i = 0; i < drop; i++) skip += level_sizes[i];
    texture_image(t, DG_MAX(1, w >> drop), DG_MAX(1, h >> drop), (u32)levels - drop, fmt);
    t->full_w = (u32)w;
    t->full_h = (u32)h;
    t->full_mips = (u32)levels;
    t->dropped = drop;
    t->ready = false;
    texture_queue_raw(id, levels - (int)drop, data + skip, level_sizes + drop);
}

TextureId render_texture_create(int w, int h, const void *rgba8, bool srgb, bool mips) {
    u32 levels = mips ? mip_count(w, h) : 1;
    TextureId id = texture_alloc(w, h, levels, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, false);
    if (id) texture_queue_rgba(id, (const u8 *)rgba8);
    return id;
}

// Pre-encoded data (KTX2) in a format the device cannot sample: BC on a GPU without
// textureCompressionBC, ASTC on Intel / AMD Macs (docs/PLATFORM_MACOS.md "Textures / formats").
static bool raw_format_ok(u32 vk_format, int w, int h) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(R->gpu, (VkFormat)vk_format, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((fp.optimalTilingFeatures & need) == need) return true;
    log_error("texture: format %u is not sampleable on this GPU (%dx%d skipped)", vk_format, w, h);
    return false;
}

TextureId render_texture_create_raw(int w, int h, int levels, u32 vk_format, const void *data, const size_t *level_sizes) {
    levels = DG_CLAMP(levels, 1, 16);
    if (!raw_format_ok(vk_format, w, h)) return 0;
    TextureId id = texture_alloc(w, h, (u32)levels, (VkFormat)vk_format, false);
    if (id) texture_queue_raw(id, levels, (const u8 *)data, level_sizes);
    return id;
}

TextureId render_texture_create_gray(int w, int h, int levels, const void *data, const size_t *level_sizes) {
    levels = DG_CLAMP(levels, 1, 16);
    if (!raw_format_ok(VK_FORMAT_R8_UNORM, w, h)) return 0;
    TextureId id = texture_alloc(w, h, (u32)levels, VK_FORMAT_R8_UNORM, false);
    if (!id) return 0;
    // The view replicates the channel: (r, r, r, 1) like an opaque grey RGBA texture.
    GpuTexture *t = &R->textures[id];
    vkDestroyImageView(R->device, t->view, NULL);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = t->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = t->mips;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &t->view));
    vkr_write_tex_set(t->set, t->view, vkr_texture_sampler(id));
    texture_queue_raw(id, levels, (const u8 *)data, level_sizes);
    return id;
}

// World textures start as a 1x1 slot (texture_alloc's set needs a view), replaced by their image at
// the current quality before anything can sample them.
static TextureId world_alloc(VkFormat fmt) {
    TextureId id = texture_alloc(1, 1, 1, fmt, true);
    if (!id) return 0;
    GpuTexture *t = &R->textures[id];
    vkDestroyImageView(R->device, t->view, NULL);
    vmaDestroyImage(R->vma, t->image, t->alloc);
    return id;
}

TextureId render_texture_create_world(int w, int h, const void *rgba8, bool srgb) {
    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    TextureId id = world_alloc(fmt);
    if (!id) return 0;
    world_fill_rgba(id, w, h, (const u8 *)rgba8, fmt);
    vkr_write_tex_set(R->textures[id].set, R->textures[id].view, R->sampler_world);
    return id;
}

TextureId render_texture_create_raw_world(int w, int h, int levels, u32 vk_format, const void *data, const size_t *level_sizes) {
    levels = DG_CLAMP(levels, 1, 16);
    if (!raw_format_ok(vk_format, w, h)) return 0;
    TextureId id = world_alloc((VkFormat)vk_format);
    if (!id) return 0;
    world_fill_raw(id, w, h, levels, (VkFormat)vk_format, (const u8 *)data, level_sizes);
    vkr_write_tex_set(R->textures[id].set, R->textures[id].view, R->sampler_world);
    return id;
}

// ------------------------------------------------------------------ texture quality

// Queued copies into texture `id`'s image are for the image it is about to lose: dropped (the record
// skips entries of the unused slot 0).
static void cancel_texture_uploads(TextureId id) {
    for (int i = 0; i < R->upload_count; i++)
        if (R->uploads[i].kind == UPLOAD_TEXTURE && R->uploads[i].texture == id) R->uploads[i].texture = 0;
}

static VkCommandBuffer once_begin(VkCommandPool *pool) {
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = R->queue_family;
    VK_CHECK(vkCreateCommandPool(R->device, &pci, NULL, pool));
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = *pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(R->device, &ai, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

static void once_end(VkCommandPool pool, VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(R->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(R->queue));
    vkDestroyCommandPool(R->device, pool, NULL);
}

// Lower quality: each world texture's resident chain loses its top levels, copied on the GPU into a
// smaller image (one submit for all, waited; the old images go right after). Higher quality: the
// levels are gone, the owner decodes the source again (render_textures_reload_wanted). A texture
// whose upload is still queued is trimmed next frame (texq_retry).
void vkr_textures_quality_changed(void) {
    R->texq_retry = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    typedef struct OldImage { VkImage image; VkImageView view; VmaAllocation alloc; } OldImage;
    OldImage *old = NULL;
    u32 old_n = 0, trimmed = 0;
    VkDeviceSize saved = 0;
    for (u32 i = 1; i < MAX_TEXTURES; i++) {
        GpuTexture *t = &R->textures[i];
        if (!t->used || !t->world) continue;
        u32 want = texture_drop(t->full_mips);
        if (want < t->dropped) { R->tex_reload = true; continue; }
        if (want == t->dropped) continue;
        if (!t->ready) { R->texq_retry = true; continue; }
        if (!cmd) {
            cmd = once_begin(&pool);
            old = (OldImage *)malloc(sizeof(OldImage) * MAX_TEXTURES);
        }
        u32 k = want - t->dropped;
        old[old_n++] = (OldImage){ t->image, t->view, t->alloc };
        VkImage src = t->image;
        u32 ow = t->w, oh = t->h;
        texture_image(t, (int)DG_MAX(1u, ow >> k), (int)DG_MAX(1u, oh >> k), t->mips - k, t->format);
        saved += (VkDeviceSize)ow * oh - (VkDeviceSize)t->w * t->h;
        vkr_image_barrier(cmd, src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0);
        vkr_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, t->mips);
        VkImageCopy regions[16];
        u32 n = DG_MIN(t->mips, 16u);
        for (u32 l = 0; l < n; l++) {
            VkImageCopy *r = &regions[l];
            memset(r, 0, sizeof(*r));
            r->srcSubresource.aspectMask = r->dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r->srcSubresource.mipLevel = l + k;
            r->dstSubresource.mipLevel = l;
            r->srcSubresource.layerCount = r->dstSubresource.layerCount = 1;
            r->extent.width = DG_MAX(1u, t->w >> l);
            r->extent.height = DG_MAX(1u, t->h >> l);
            r->extent.depth = 1;
        }
        vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regions);
        vkr_image_barrier(cmd, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, t->mips);
        t->dropped = want;
        vkg_texture_replaced(i);
        trimmed++;
    }
    if (!cmd) return;
    once_end(pool, cmd);
    for (u32 i = 0; i < old_n; i++) {
        vkDestroyImageView(R->device, old[i].view, NULL);
        vmaDestroyImage(R->vma, old[i].image, old[i].alloc);
    }
    free(old);
    vkr_rewrite_all_texture_sets();
    vkg_samplers_changed();
    log_info("texture quality %d: %u world textures trimmed (~%.1f Mtexels of level 0 freed)", R->r_texture_quality->i, trimmed, (double)saved / 1e6);
}

bool render_textures_reload_wanted(void) { return R && R->tex_reload; }

bool render_texture_reload_needed(TextureId id) {
    if (!id || id >= MAX_TEXTURES || !R->textures[id].used || !R->textures[id].world) return false;
    const GpuTexture *t = &R->textures[id];
    return texture_drop(t->full_mips) < t->dropped;
}

// The images replaced below may be in use by frames in flight.
void render_textures_reload_begin(void) { vkDeviceWaitIdle(R->device); }

// World texture `id` loses its image (the device is idle: render_textures_reload_begin).
static GpuTexture *reload_slot(TextureId id, int w, int h) {
    if (!render_texture_reload_needed(id)) return NULL;
    GpuTexture *t = &R->textures[id];
    if ((u32)w != t->full_w || (u32)h != t->full_h) {
        log_warn("render: texture %u reload is %dx%d, was %ux%u (source changed): kept", id, w, h, t->full_w, t->full_h);
        return NULL;
    }
    cancel_texture_uploads(id);
    vkDestroyImageView(R->device, t->view, NULL);
    vmaDestroyImage(R->vma, t->image, t->alloc);
    return t;
}

void render_texture_reload(TextureId id, int w, int h, const void *rgba8) {
    GpuTexture *t = reload_slot(id, w, h);
    if (!t) return;
    world_fill_rgba(id, w, h, (const u8 *)rgba8, t->format);
    vkg_texture_replaced(id);
}

void render_texture_reload_raw(TextureId id, int w, int h, int levels, u32 vk_format, const void *data, const size_t *level_sizes) {
    levels = DG_CLAMP(levels, 1, 16);
    if (render_texture_reload_needed(id) && ((VkFormat)vk_format != R->textures[id].format || (u32)levels != R->textures[id].full_mips)) {
        log_warn("render: texture %u reload changed format / levels (source changed): kept", id);
        return;
    }
    GpuTexture *t = reload_slot(id, w, h);
    if (!t) return;
    world_fill_raw(id, w, h, levels, t->format, (const u8 *)data, level_sizes);
    vkg_texture_replaced(id);
}

// A world texture's image of another format (r_texture_compress switched: RGBA8 <-> BC7). The
// device is idle (render_textures_reload_begin); the quality's dropped levels apply as at creation.
static GpuTexture *replace_slot(TextureId id) {
    if (!id || id >= MAX_TEXTURES || !R->textures[id].used || !R->textures[id].world) return NULL;
    GpuTexture *t = &R->textures[id];
    cancel_texture_uploads(id);
    vkDestroyImageView(R->device, t->view, NULL);
    vmaDestroyImage(R->vma, t->image, t->alloc);
    return t;
}

void render_texture_replace(TextureId id, int w, int h, const void *rgba8, bool srgb) {
    if (!replace_slot(id)) return;
    world_fill_rgba(id, w, h, (const u8 *)rgba8, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
    vkg_texture_replaced(id);
}

void render_texture_replace_raw(TextureId id, int w, int h, int levels, u32 vk_format, const void *data, const size_t *level_sizes) {
    levels = DG_CLAMP(levels, 1, 16);
    if (!raw_format_ok(vk_format, w, h) || !replace_slot(id)) return;
    world_fill_raw(id, w, h, levels, (VkFormat)vk_format, (const u8 *)data, level_sizes);
    vkg_texture_replaced(id);
}

// The replaced images' sets (texture, glTF map sets) and the vgeo texture table follow.
void render_textures_reload_end(void) {
    R->tex_reload = false;
    vkr_rewrite_all_texture_sets();
    vkg_samplers_changed();
}

void render_texture_destroy(TextureId id) {
    if (!id || id >= MAX_TEXTURES || !R->textures[id].used) return;
    GpuTexture *t = &R->textures[id];
    mat_sets_forget(id);
    vkam_texture_destroyed(id);
    vkr_defer_descriptor(t->set);
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

void vkr_upload_image(VkImage image, u32 w, u32 h, u32 mips, u32 layers, const void *data, const size_t *mip_offsets, size_t size) {
    Upload *u = push_upload();
    if (!u) return;
    u->kind = UPLOAD_IMAGE;
    u->staging = vkr_buffer_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(u->staging.mapped, data, size);
    u->image = image;
    u->w = w;
    u->h = h;
    u->layers = layers;
    u->levels_in_data = (int)DG_MIN(mips, 16u);
    for (int i = 0; i < u->levels_in_data; i++) u->level_offsets[i] = mip_offsets[i];
}

// An image destroyed before its queued upload was recorded (replaced within one frame).
void vkr_cancel_image_upload(VkImage image) {
    for (int i = 0; i < R->upload_count; i++)
        if (R->uploads[i].kind == UPLOAD_IMAGE && R->uploads[i].image == image) R->uploads[i].image = VK_NULL_HANDLE;
}

// UPLOAD_IMAGE: every mip / layer from the staging buffer, then shader-readable (all layers).
static void record_image_upload(VkCommandBuffer cmd, Upload *u) {
    if (!u->image) return;   // cancelled
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = u->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = (u32)u->levels_in_data;
    b.subresourceRange.layerCount = u->layers;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    for (int lvl = 0; lvl < u->levels_in_data; lvl++) {
        VkBufferImageCopy region = { 0 };
        region.bufferOffset = u->level_offsets[lvl];
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = (u32)lvl;
        region.imageSubresource.layerCount = u->layers;
        region.imageExtent.width = DG_MAX(1u, u->w >> lvl);
        region.imageExtent.height = DG_MAX(1u, u->h >> lvl);
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cmd, u->staging.buffer, u->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void vkr_record_uploads(VkCommandBuffer cmd) {
    if (!R->upload_count) return;
    for (int i = 0; i < R->upload_count; i++) {
        Upload *u = &R->uploads[i];
        if (u->kind == UPLOAD_BUFFER) {
            VkBufferCopy c = { 0, 0, u->size };
            vkCmdCopyBuffer(cmd, u->staging.buffer, u->dst, 1, &c);
        } else if (u->kind == UPLOAD_IMAGE) {
            record_image_upload(cmd, u);
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
        if (t->wrapped) { t->used = false; continue; }
        vkDestroyImageView(R->device, t->view, NULL);
        vmaDestroyImage(R->vma, t->image, t->alloc);
        t->used = false;
    }
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkr_flush_deferred(&R->frames[i]);
}
