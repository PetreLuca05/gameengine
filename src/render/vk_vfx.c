// GPU VFX: persistent SoA particle pool simulated in compute (emit -> sub-emit -> update ->
// draw args -> bitonic sort), vertex-pulled particle draws (sorted alpha, additive, distortion,
// CPU sprites), box-projected decals and a texture atlas (2D array) built from the VFX library
// (the HDR bloom lives in the post stack, src/render/post/post_bloom.c). No CPU readback in the
// hot path: counters are copied to a host buffer and read two frames later for stats and sort
// sizing only.
#include "vk_internal.h"
#include "graph/fg.h"
#include "post/post.h"
#include "core/platform.h"
#include "vfx/vfx_sim.h"
#include "vfx/vfx_atlas.h"
#include "vfx/vfx_curve.h"
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define VFX_MAX_SORT 131072u
#define VFX_MAX_DISTORT 16384u
#define VFX_MAX_SUBS 4096u
#define VFX_MAX_SPRITES 8192u
#define VFX_MAX_GPU_DECALS 4096u
#define VFX_PAGE 2048
#define VFX_BINDINGS 24

// std430 / std140 mirrors of shaders/vfx_common.glsl
typedef struct GpuEmitter {
    vec4 spawn0, spawn1, speed_life, size_rot, spin_misc, color_a, color_b, phys0, phys1, render0, render1;
    vec4 tex_uv, mv_uv, lm0_uv, lm1_uv, nrm_uv;
    u32 modes[4];
    u32 sub[4];
} GpuEmitter;

typedef struct GpuCounters {
    u32 dead, alive[2], add, alpha, distort, sub, sub_pending;
    u32 update_args[4], draw_add[4], draw_alpha[4], draw_distort[4], emitted[4];
} GpuCounters;

typedef struct VfxUbo {
    mat4 view_proj, prev_view_proj, inv_view_proj, view, proj;
    vec4 cam_pos, cam_right, cam_up, cam_fwd, wind, screen, depth_params;
    u32 counts[4];
    u32 caps[4];
    vec4 misc;
} VfxUbo;

typedef struct VfxFrameRes {
    GpuBuffer ubo, spawns, prefix, sprites, decals, instances, readback;
    VkDescriptorSet set;
    u32 spawn_particles, spawn_records;
    int sprite_add, sprite_alpha, decal_count, decal_dynamic;
    bool readback_valid;
    bool fed;
} VfxFrameRes;

static struct {
    bool ok;
    u32 max_particles;
    VkDescriptorSetLayout layout;
    VkDescriptorSetLayout fog_layout;   // set 2 of the draws (push descriptor): the froxel fog volume
    VkDescriptorPool pool;
    VkPipelineLayout comp_layout, gfx_layout;
    VkPipeline c_reset, c_begin, c_emit, c_emit_sub, c_args, c_update, c_draw_args, c_sort;
    VkPipeline g_add, g_alpha, g_distort, g_decal;
    GpuBuffer pos, vel, misc, info, dead, alive_a, alive_b, counters, emitters, luts, list_add, sort_keys, list_distort, subs;
    VfxFrameRes fr[FRAMES_IN_FLIGHT];
    VkImage atlas;
    VmaAllocation atlas_alloc;
    VkImageView atlas_view;
    int pages, atlas_textures;
    VkSampler atlas_sampler, noise_sampler, depth_sampler;
    VkImage noise;
    VmaAllocation noise_alloc;
    VkImageView noise_view;
    RenderTarget scene_copy;
    VkQueryPool qp;
    float ts_period;
    bool need_reset;
    bool depth_valid;
    mat4 prev_vp;
    u32 frame_index;
    u32 sort_n;
    u32 last_alpha, last_distort;
    int frames_since_spawn;
    RenderVfxFrame in;      // last submitted frame input (counts / settings)
    RenderVfxStats stats;
    int emitter_count;
    Cvar *max_particles_cvar;
} X;

// ------------------------------------------------------------------ helpers

static void mem_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = ss;
    mb.srcAccessMask = sa;
    mb.dstStageMask = ds;
    mb.dstAccessMask = da;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}
#define CS VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
#define RW (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
static void cs_barrier(VkCommandBuffer cmd) { mem_barrier(cmd, CS, RW, CS, RW); }

static void image_barrier_mips(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, u32 base_mip, u32 mips, u32 layers, VkImageLayout o,
                               VkImageLayout n, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = ss;
    b.srcAccessMask = sa;
    b.dstStageMask = ds;
    b.dstAccessMask = da;
    b.oldLayout = o;
    b.newLayout = n;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = base_mip;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.layerCount = layers;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// One-time command buffer (loading only).
static VkCommandBuffer begin_once(VkCommandPool *pool) {
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
static void end_once(VkCommandPool pool, VkCommandBuffer cmd) {
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

static GpuBuffer dev_buffer(VkDeviceSize size, VkBufferUsageFlags extra) {
    return vkr_buffer_create(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra, false);
}

// ------------------------------------------------------------------ descriptors

static void write_buffer(VkDescriptorSet set, u32 binding, VkDescriptorType type, VkBuffer b, VkDeviceSize range) {
    VkDescriptorBufferInfo bi = { b, 0, range };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
}
static void write_image(VkDescriptorSet set, u32 binding, VkImageView view, VkSampler s, VkImageLayout layout) {
    VkDescriptorImageInfo ii = { s, view, layout };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);
}

static void write_static_descriptors(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VfxFrameRes *f = &X.fr[i];
        VkDescriptorSet s = f->set;
        const VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write_buffer(s, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, f->ubo.buffer, sizeof(VfxUbo));
        GpuBuffer *bufs[] = { &X.pos, &X.vel, &X.misc, &X.info, &X.dead, &X.alive_a, &X.alive_b, &X.counters, &X.emitters, &X.luts,
                              &f->spawns, &f->prefix, &X.list_add, &X.sort_keys, &X.list_distort, &X.subs };
        for (int b = 0; b < 16; b++) write_buffer(s, (u32)(1 + b), SB, bufs[b]->buffer, VK_WHOLE_SIZE);
        write_buffer(s, 21, SB, f->sprites.buffer, VK_WHOLE_SIZE);
        write_buffer(s, 22, SB, f->decals.buffer, VK_WHOLE_SIZE);
        write_buffer(s, 23, SB, f->instances.buffer, VK_WHOLE_SIZE);
        if (X.noise_view) write_image(s, 18, X.noise_view, X.noise_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (X.atlas_view) write_image(s, 17, X.atlas_view, X.atlas_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// ------------------------------------------------------------------ pipelines

static VkPipeline compute_pipe(const char *name) {
    VkShaderModule m = vkp_load_shader(name);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = X.comp_layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(R->device, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    vkDestroyShaderModule(R->device, m, NULL);
    if (r != VK_SUCCESS) { log_error("vfx: compute pipeline %s failed (%d)", name, (int)r); return VK_NULL_HANDLE; }
    return p;
}

typedef enum Blend { BL_NONE, BL_ADD, BL_PREMUL, BL_ALPHA } Blend;

static VkPipeline graphics_pipe(const char *vs_name, const char *fs_name, VkPipelineLayout layout, Blend blend, bool depth_test,
                                VkCullModeFlags cull, VkFormat color, VkFormat depth) {
    VkShaderModule vs = vkp_load_shader(vs_name), fs = vkp_load_shader(fs_name);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(R->device, vs, NULL);
        if (fs) vkDestroyShaderModule(R->device, fs, NULL);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = depth_test;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 0xF;
    if (blend != BL_NONE) {
        cba.blendEnable = VK_TRUE;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
        if (blend == BL_ADD) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        } else if (blend == BL_PREMUL) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        } else {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &color;
    rci.depthAttachmentFormat = depth;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(R->device, VK_NULL_HANDLE, 1, &gp, NULL, &p);
    vkDestroyShaderModule(R->device, vs, NULL);
    vkDestroyShaderModule(R->device, fs, NULL);
    if (r != VK_SUCCESS) { log_error("vfx: pipeline %s/%s failed (%d)", vs_name, fs_name, (int)r); return VK_NULL_HANDLE; }
    return p;
}

static void destroy_pipelines(void) {
    VkPipeline *all[] = { &X.c_reset, &X.c_begin, &X.c_emit, &X.c_emit_sub, &X.c_args, &X.c_update, &X.c_draw_args, &X.c_sort,
                          &X.g_add, &X.g_alpha, &X.g_distort, &X.g_decal };
    for (int i = 0; i < (int)ARRAY_COUNT(all); i++) {
        if (*all[i]) vkDestroyPipeline(R->device, *all[i], NULL);
        *all[i] = VK_NULL_HANDLE;
    }
}

bool vkx_create_pipelines(void) {
    if (!X.layout) return true;
    destroy_pipelines();
    VkFormat hdr = VK_FORMAT_R16G16B16A16_SFLOAT, depth = VK_FORMAT_D32_SFLOAT;
    X.c_reset = compute_pipe("vfx_reset.comp");
    X.c_begin = compute_pipe("vfx_begin.comp");
    X.c_emit = compute_pipe("vfx_emit.comp");
    X.c_emit_sub = compute_pipe("vfx_emit_sub.comp");
    X.c_args = compute_pipe("vfx_args.comp");
    X.c_update = compute_pipe("vfx_update.comp");
    X.c_draw_args = compute_pipe("vfx_draw_args.comp");
    X.c_sort = compute_pipe("vfx_sort.comp");
    X.g_add = graphics_pipe("vfx_particle.vert", "vfx_particle.frag", X.gfx_layout, BL_ADD, true, VK_CULL_MODE_NONE, hdr, depth);
    X.g_alpha = graphics_pipe("vfx_particle.vert", "vfx_particle.frag", X.gfx_layout, BL_PREMUL, true, VK_CULL_MODE_NONE, hdr, depth);
    X.g_distort = graphics_pipe("vfx_particle.vert", "vfx_particle.frag", X.gfx_layout, BL_ALPHA, true, VK_CULL_MODE_NONE, hdr, depth);
    // Decals: back faces, no depth test (camera inside the box works), blended over the scene.
    X.g_decal = graphics_pipe("vfx_decal.vert", "vfx_decal.frag", X.gfx_layout, BL_PREMUL, false, VK_CULL_MODE_FRONT_BIT, hdr, depth);
    bool ok = X.c_reset && X.c_begin && X.c_emit && X.c_emit_sub && X.c_args && X.c_update && X.c_draw_args && X.c_sort && X.g_add && X.g_alpha &&
              X.g_distort && X.g_decal;
    if (!ok) log_error("vfx: some pipelines failed to build (VFX disabled until shaders are fixed)");
    X.ok = ok;
    return ok;
}

// ------------------------------------------------------------------ noise / atlas images

static VkImage create_image(VkImageType type, VkFormat fmt, u32 w, u32 h, u32 d, u32 mips, u32 layers, VkImageUsageFlags usage, VmaAllocation *alloc) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = type;
    ici.format = fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = d;
    ici.mipLevels = mips;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage img;
    VK_CHECK(vmaCreateImage(R->vma, &ici, &aci, &img, alloc, NULL));
    return img;
}

static float hash3(int x, int y, int z, int c) {
    u32 h = vfx_hash((u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u ^ (u32)c * 2654435761u);
    return (float)(h & 0xFFFFFF) / 16777215.0f * 2.0f - 1.0f;
}
// Smooth value noise (periodic over 8 cells) of channel c.
static float vnoise(float x, float y, float z, int c) {
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float fx = x - (float)xi, fy = y - (float)yi, fz = z - (float)zi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    fz = fz * fz * (3 - 2 * fz);
    float v = 0;
    for (int k = 0; k < 8; k++) {
        int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
        float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
        v += w * hash3((xi + dx) & 7, (yi + dy) & 7, (zi + dz) & 7, c);
    }
    return v;
}

static void create_noise(void) {
    // 32^3 curl-noise vectors (curl of a 3-channel periodic value-noise potential), RGBA8 SNORM.
    const int N = 32;
    i8 *px = (i8 *)malloc((size_t)N * N * N * 4);
    float s = 8.0f / (float)N, e = 0.5f;
    float maxl = 1e-4f;
    float *tmp = (float *)malloc(sizeof(float) * 3 * (size_t)N * N * N);
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float fx = (float)x * s, fy = (float)y * s, fz = (float)z * s;
                // curl = (dPz/dy - dPy/dz, dPx/dz - dPz/dx, dPy/dx - dPx/dy)
                float dPz_dy = vnoise(fx, fy + e, fz, 2) - vnoise(fx, fy - e, fz, 2);
                float dPy_dz = vnoise(fx, fy, fz + e, 1) - vnoise(fx, fy, fz - e, 1);
                float dPx_dz = vnoise(fx, fy, fz + e, 0) - vnoise(fx, fy, fz - e, 0);
                float dPz_dx = vnoise(fx + e, fy, fz, 2) - vnoise(fx - e, fy, fz, 2);
                float dPy_dx = vnoise(fx + e, fy, fz, 1) - vnoise(fx - e, fy, fz, 1);
                float dPx_dy = vnoise(fx, fy + e, fz, 0) - vnoise(fx, fy - e, fz, 0);
                float *t = tmp + 3 * ((size_t)(z * N + y) * N + x);
                t[0] = dPz_dy - dPy_dz;
                t[1] = dPx_dz - dPz_dx;
                t[2] = dPy_dx - dPx_dy;
                float l = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                if (l > maxl) maxl = l;
            }
    for (size_t i = 0; i < (size_t)N * N * N; i++) {
        for (int c = 0; c < 3; c++) px[i * 4 + c] = (i8)lrintf(clampf(tmp[i * 3 + c] / maxl, -1, 1) * 127.0f);
        px[i * 4 + 3] = 127;
    }
    free(tmp);
    X.noise = create_image(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_SNORM, N, N, N, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &X.noise_alloc);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = X.noise;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = VK_FORMAT_R8G8B8A8_SNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &X.noise_view));
    GpuBuffer st = vkr_buffer_create((VkDeviceSize)N * N * N * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(st.mapped, px, (size_t)N * N * N * 4);
    free(px);
    VkCommandPool pool;
    VkCommandBuffer cmd = begin_once(&pool);
    image_barrier_mips(cmd, X.noise, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = r.imageExtent.height = r.imageExtent.depth = (u32)N;
    vkCmdCopyBufferToImage(cmd, st.buffer, X.noise, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier_mips(cmd, X.noise, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    end_once(pool, cmd);
    vkr_buffer_destroy_now(&st);
}

typedef struct Img { u8 *px; int w, h; } Img;

static Img load_png(const char *dir, const char *file) {
    Img im = { 0 };
    char rel[600], buf[700];
    snprintf(rel, sizeof(rel), "%s/tex/%s", dir, file);
    int comp;
    im.px = stbi_load(platform_data_path(rel, buf, sizeof(buf)), &im.w, &im.h, &comp, 4);
    if (!im.px) { im.w = im.h = 0; return im; }
    // Fit into a page: halve (box filter) until it fits.
    while (im.w > VFX_PAGE - 8 || im.h > VFX_PAGE - 8) {
        int nw = im.w / 2, nh = im.h / 2;
        u8 *np = (u8 *)malloc((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    int s = im.px[((2 * y) * im.w + 2 * x) * 4 + c] + im.px[((2 * y) * im.w + 2 * x + 1) * 4 + c] +
                            im.px[((2 * y + 1) * im.w + 2 * x) * 4 + c] + im.px[((2 * y + 1) * im.w + 2 * x + 1) * 4 + c];
                    np[(y * nw + x) * 4 + c] = (u8)((s + 2) / 4);
                }
        stbi_image_free(im.px);
        im.px = np;
        im.w = nw;
        im.h = nh;
    }
    return im;
}

static void free_img(Img *im) { if (im->px) free(im->px); im->px = NULL; }   // stb uses malloc/free

static void destroy_atlas(void) {
    if (X.atlas_view) vkDestroyImageView(R->device, X.atlas_view, NULL);
    if (X.atlas) vmaDestroyImage(R->vma, X.atlas, X.atlas_alloc);
    X.atlas_view = VK_NULL_HANDLE;
    X.atlas = VK_NULL_HANDLE;
}

static vec4 rect_uv(const VfxAtlasRect *r) {
    return v4((float)r->x / VFX_PAGE, (float)r->y / VFX_PAGE, (float)(r->x + r->w) / VFX_PAGE, (float)(r->y + r->h) / VFX_PAGE);
}

static void upload_atlas(u8 **pages, int page_count) {
    destroy_atlas();
    u32 mips = 1;
    while ((VFX_PAGE >> mips) >= 16) mips++;
    X.atlas = create_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VFX_PAGE, VFX_PAGE, 1, mips, (u32)page_count,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &X.atlas_alloc);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = X.atlas;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = mips;
    vci.subresourceRange.layerCount = (u32)page_count;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &X.atlas_view));
    // CPU mips (premultiplied box filter), all pages and levels in one staging buffer.
    size_t total = 0;
    for (u32 m = 0; m < mips; m++) total += (size_t)(VFX_PAGE >> m) * (VFX_PAGE >> m) * 4 * (size_t)page_count;
    GpuBuffer st = vkr_buffer_create(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    u8 *dst = (u8 *)st.mapped;
    VkBufferImageCopy *regions = (VkBufferImageCopy *)calloc((size_t)mips * (size_t)page_count, sizeof(VkBufferImageCopy));
    size_t off = 0;
    int ri = 0;
    for (int p = 0; p < page_count; p++) {
        u8 *level = pages[p];
        int w = VFX_PAGE;
        u8 *prev = NULL;
        for (u32 m = 0; m < mips; m++) {
            if (m > 0) {
                int pw = w * 2;
                u8 *nl = (u8 *)malloc((size_t)w * w * 4);
                for (int y = 0; y < w; y++)
                    for (int x = 0; x < w; x++)
                        for (int c = 0; c < 4; c++) {
                            int s = prev[((2 * y) * pw + 2 * x) * 4 + c] + prev[((2 * y) * pw + 2 * x + 1) * 4 + c] + prev[((2 * y + 1) * pw + 2 * x) * 4 + c] +
                                    prev[((2 * y + 1) * pw + 2 * x + 1) * 4 + c];
                            nl[(y * w + x) * 4 + c] = (u8)((s + 2) / 4);
                        }
                if (prev != pages[p]) free(prev);
                level = nl;
            }
            size_t sz = (size_t)w * w * 4;
            memcpy(dst + off, level, sz);
            VkBufferImageCopy *r = &regions[ri++];
            r->bufferOffset = off;
            r->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r->imageSubresource.mipLevel = m;
            r->imageSubresource.baseArrayLayer = (u32)p;
            r->imageSubresource.layerCount = 1;
            r->imageExtent.width = r->imageExtent.height = (u32)w;
            r->imageExtent.depth = 1;
            off += sz;
            prev = level;
            w /= 2;
        }
        if (prev != pages[p]) free(prev);
    }
    VkCommandPool pool;
    VkCommandBuffer cmd = begin_once(&pool);
    image_barrier_mips(cmd, X.atlas, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, (u32)page_count, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, st.buffer, X.atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (u32)ri, regions);
    image_barrier_mips(cmd, X.atlas, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, (u32)page_count, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    end_once(pool, cmd);
    vkr_buffer_destroy_now(&st);
    free(regions);
    X.pages = page_count;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) write_image(X.fr[i].set, 17, X.atlas_view, X.atlas_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Emitter table + curve LUTs from the library (after the atlas placed the textures).
static void upload_emitters(const VfxLibrary *lib) {
    GpuEmitter *ge = (GpuEmitter *)X.emitters.mapped;
    vec4 *lut = (vec4 *)X.luts.mapped;
    memset(ge, 0, sizeof(GpuEmitter) * VFX_MAX_EMITTERS);
    for (int i = 0; i < lib->emitter_count && i < VFX_MAX_EMITTERS; i++) {
        const VfxEmitterDef *e = &lib->emitters[i];
        const VfxSpawnDef *s = &e->spawn;
        const VfxUpdateDef *u = &e->update;
        const VfxRenderDef *r = &e->render;
        GpuEmitter *g = &ge[i];
        g->spawn0 = v4((float)s->shape, s->radius, s->angle, s->offset);
        g->spawn1 = v4(s->box.x, s->box.y, s->box.z, s->dir_bias);
        g->speed_life = v4(s->speed.min, s->speed.max, s->life.min, s->life.max);
        g->size_rot = v4(s->size.min, s->size.max, s->rotation.min * DG_DEG2RAD, s->rotation.max * DG_DEG2RAD);
        g->spin_misc = v4(s->spin.min * DG_DEG2RAD, s->spin.max * DG_DEG2RAD, s->inherit, (float)s->dir);
        g->color_a = s->color_a;
        g->color_b = s->color_b;
        g->phys0 = v4(u->gravity, u->drag, u->wind, u->turbulence);
        g->phys1 = v4(u->turb_freq, u->turb_speed, u->bounce, u->friction);
        g->render0 = v4(r->stretch, r->stretch_max, r->soft, r->emissive);
        g->render1 = v4(r->near_fade, r->mode == VFX_RENDER_DISTORT ? r->distortion : r->scatter, r->aspect, u->fps);
        int grid_cols = 1, grid_rows = 1, frames = 1, page = 0, mvp = 0, lmp = 0, np = 0;
        g->tex_uv = g->mv_uv = g->lm0_uv = g->lm1_uv = g->nrm_uv = v4(0, 0, 0, 0);
        if (r->texture_id >= 0) {
            const VfxTextureMeta *m = &lib->textures[r->texture_id];
            if (m->uv.z > m->uv.x) {
                g->tex_uv = m->uv;
                page = m->page;
                grid_cols = m->cols;
                grid_rows = m->rows;
                frames = m->variants > 1 ? m->variants : m->frames;
                if (m->motion_vectors && m->mv_uv.z > m->mv_uv.x) { g->mv_uv = m->mv_uv; mvp = m->mv_page; }
                if (m->lighting == VFX_LIGHT_SIXWAY && m->lm0_uv.z > m->lm0_uv.x) { g->lm0_uv = m->lm0_uv; g->lm1_uv = m->lm1_uv; lmp = m->lm_page; }
            }
        }
        if (r->normal_tex_id >= 0 && lib->textures[r->normal_tex_id].uv.z > lib->textures[r->normal_tex_id].uv.x) {
            g->nrm_uv = lib->textures[r->normal_tex_id].uv;
            np = lib->textures[r->normal_tex_id].page;
        }
        // Distortion sprites sample their normal map as the main texture.
        if (r->mode == VFX_RENDER_DISTORT && g->nrm_uv.z > g->nrm_uv.x) { g->tex_uv = g->nrm_uv; page = np; }
        u32 flags = 0;
        if (u->frame_over_life) flags |= 1u;
        if (u->motion_vectors && g->mv_uv.z > g->mv_uv.x) flags |= 2u;
        if (u->kill_on_hit) flags |= 4u;
        if (r->exposure_aware) flags |= 8u;
        if (s->local) flags |= 16u;
        if (s->frame_random) flags |= 32u;
        u32 lighting = r->lighting;
        if (lighting == VFX_LIGHT_SIXWAY && !(g->lm0_uv.z > g->lm0_uv.x)) lighting = VFX_LIGHT_SIMPLE;
        u32 rm = r->mode <= VFX_RENDER_DISTORT ? r->mode : VFX_RENDER_ALPHA;
        g->modes[0] = rm | ((u32)r->orient << 4) | (lighting << 8) | ((u32)(u->collide ? 1u : 0u) << 12) | (flags << 16);
        g->modes[1] = (u32)page | ((u32)mvp << 8) | ((u32)lmp << 16) | ((u32)np << 24);
        g->modes[2] = (u32)DG_CLAMP(grid_cols, 1, 255) | ((u32)DG_CLAMP(grid_rows, 1, 255) << 8) | ((u32)DG_CLAMP(frames, 1, 65535) << 16);
        g->modes[3] = (u32)i;
        g->sub[0] = (u32)u->sub_on | ((u32)DG_CLAMP(u->sub_count, 0, 255) << 8);
        g->sub[1] = g->sub[2] = g->sub[3] = 0;
        if (u->sub_effect_id >= 0) {
            const VfxEffectDef *sf = &lib->effects[u->sub_effect_id];
            int n = 0;
            for (int k = 0; k < sf->emitter_count && n < 3; k++) {
                const VfxEmitterDef *se = &lib->emitters[sf->emitters[k]];
                if (se->tier != VFX_TIER_GPU) continue;
                g->sub[1 + n++] = (u32)sf->emitters[k] + 1u;
            }
        }
        vec4 *row = lut + (size_t)i * 4 * VFX_LUT_SAMPLES;
        vfx_curve_bake(&u->size_curve, row);
        vfx_curve_bake(&u->color_curve, row + VFX_LUT_SAMPLES);
        vfx_curve_bake(&u->velocity_curve, row + 2 * VFX_LUT_SAMPLES);
        vfx_curve_bake(&u->spin_curve, row + 3 * VFX_LUT_SAMPLES);
    }
    X.emitter_count = lib->emitter_count;
}

void render_vfx_update_emitters(VfxLibrary *lib) {
    if (!R || !X.layout) return;
    vkDeviceWaitIdle(R->device);   // editor only: the tables are host mapped and read by in-flight frames
    upload_emitters(lib);
}

bool render_vfx_set_library(VfxLibrary *lib) {
    if (!R || !X.layout) return false;
    vkDeviceWaitIdle(R->device);
    // Collect rects: main texture, + motion vectors, + 6-way maps (2) per texture.
    int n = lib->texture_count;
    Img *imgs = (Img *)calloc((size_t)n * 4, sizeof(Img));
    VfxAtlasRect *rects = (VfxAtlasRect *)calloc((size_t)n * 4, sizeof(VfxAtlasRect));
    int *map = (int *)malloc(sizeof(int) * (size_t)n * 4);
    int rc = 0, loaded = 0;
    for (int i = 0; i < n; i++) {
        VfxTextureMeta *m = &lib->textures[i];
        m->uv = m->mv_uv = m->lm0_uv = m->lm1_uv = v4(0, 0, 0, 0);
        char stem[VFX_NAME + 8];
        dg_strcpy(stem, sizeof(stem), m->file);
        char *dot = strrchr(stem, '.');
        if (dot) *dot = 0;
        const char *suffix[4] = { NULL, "_mv", "_lm0", "_lm1" };
        for (int k = 0; k < 4; k++) {
            if (k == 1 && !m->motion_vectors) continue;
            if (k >= 2 && m->lighting != VFX_LIGHT_SIXWAY) continue;
            char file[160];
            if (k == 0) dg_strcpy(file, sizeof(file), m->file);
            else snprintf(file, sizeof(file), "%s%s.png", stem, suffix[k]);
            Img im = load_png(lib->dir, file);
            if (!im.px) { if (k == 0) log_warn("vfx: texture %s missing (%s)", m->name, file); continue; }
            imgs[rc] = im;
            rects[rc].w = im.w;
            rects[rc].h = im.h;
            map[rc] = i * 4 + k;
            rc++;
            if (k == 0) loaded++;
        }
    }
    int pages = vfx_atlas_pack(rects, rc, VFX_PAGE, 2);
    if (pages <= 0) pages = 1;
    u8 **page_px = (u8 **)calloc((size_t)pages, sizeof(u8 *));
    for (int p = 0; p < pages; p++) page_px[p] = (u8 *)calloc((size_t)VFX_PAGE * VFX_PAGE, 4);
    for (int r = 0; r < rc; r++) {
        if (!rects[r].placed) { free_img(&imgs[r]); continue; }
        u8 *dst = page_px[rects[r].page];
        for (int y = 0; y < rects[r].h; y++)
            memcpy(dst + ((size_t)(rects[r].y + y) * VFX_PAGE + (size_t)rects[r].x) * 4, imgs[r].px + (size_t)y * (size_t)imgs[r].w * 4, (size_t)imgs[r].w * 4);
        VfxTextureMeta *m = &lib->textures[map[r] / 4];
        int k = map[r] % 4;
        vec4 uv = rect_uv(&rects[r]);
        // Half-texel inset so bilinear never reads the neighbor.
        uv.x += 0.5f / VFX_PAGE; uv.y += 0.5f / VFX_PAGE; uv.z -= 0.5f / VFX_PAGE; uv.w -= 0.5f / VFX_PAGE;
        if (k == 0) { m->uv = uv; m->page = rects[r].page; m->w = rects[r].w; m->h = rects[r].h; }
        else if (k == 1) { m->mv_uv = uv; m->mv_page = rects[r].page; }
        else if (k == 2) { m->lm0_uv = uv; m->lm_page = rects[r].page; }
        else { m->lm1_uv = uv; }
        free_img(&imgs[r]);
    }
    // 6-way maps must share a page (the shader takes one lm page): drop them otherwise.
    upload_atlas(page_px, pages);
    for (int p = 0; p < pages; p++) free(page_px[p]);
    free(page_px);
    free(imgs);
    free(rects);
    free(map);
    upload_emitters(lib);
    X.atlas_textures = loaded;
    X.need_reset = true;
    log_info("vfx: atlas %d page(s) of %d^2, %d/%d textures, %d emitters", pages, VFX_PAGE, loaded, n, lib->emitter_count);
    return true;
}

// ------------------------------------------------------------------ targets (scene copy)

void vkx_targets_changed(void) {
    if (!X.layout) return;
    vkDeviceWaitIdle(R->device);
    vkr_target_destroy(&X.scene_copy);
    X.scene_copy = vkr_target_create(R->hdr.w, R->hdr.h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
    // Initial layouts: depth / scene copy readable (the descriptors expect them).
    VkCommandPool pool;
    VkCommandBuffer cmd = begin_once(&pool);
    image_barrier_mips(cmd, R->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    image_barrier_mips(cmd, X.scene_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    end_once(pool, cmd);
    R->depth.layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    X.scene_copy.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        write_image(X.fr[i].set, 19, R->depth.view, X.depth_sampler, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        write_image(X.fr[i].set, 20, X.scene_copy.view, R->sampler_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    X.depth_valid = false;
}


// ------------------------------------------------------------------ init

bool vkx_init(void) {
    memset(&X, 0, sizeof(X));
    X.max_particles_cvar = cvar_reg_int("vfx_max_particles", 1 << 20, 1 << 14, 1 << 22, CVAR_ARCHIVE, "GPU particle pool size (restart)");
    X.max_particles = (u32)X.max_particles_cvar->i;
    VkDescriptorSetLayoutBinding b[VFX_BINDINGS];
    memset(b, 0, sizeof(b));
    for (u32 i = 0; i < VFX_BINDINGS; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[i].descriptorType = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                       : ((i >= 17 && i <= 20) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = VFX_BINDINGS;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &X.layout));
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT },
                                   { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * FRAMES_IN_FLIGHT },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 19 * FRAMES_IN_FLIGHT } };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = FRAMES_IN_FLIGHT;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(R->device, &pci, NULL, &X.pool));
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &X.layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &X.comp_layout));
    VkDescriptorSetLayoutBinding fb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo fli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    fli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    fli.bindingCount = 1;
    fli.pBindings = &fb;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &fli, NULL, &X.fog_layout));
    VkDescriptorSetLayout gl[3] = { R->frame_layout, X.layout, X.fog_layout };
    pli.setLayoutCount = 3;
    pli.pSetLayouts = gl;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &X.gfx_layout));

    u32 M = X.max_particles;
    X.pos = dev_buffer((VkDeviceSize)M * 16, 0);
    X.vel = dev_buffer((VkDeviceSize)M * 16, 0);
    X.misc = dev_buffer((VkDeviceSize)M * 16, 0);
    X.info = dev_buffer((VkDeviceSize)M * 16, 0);
    X.dead = dev_buffer((VkDeviceSize)M * 4, 0);
    X.alive_a = dev_buffer((VkDeviceSize)M * 4, 0);
    X.alive_b = dev_buffer((VkDeviceSize)M * 4, 0);
    X.list_add = dev_buffer((VkDeviceSize)M * 4, 0);
    X.sort_keys = dev_buffer((VkDeviceSize)VFX_MAX_SORT * 8, 0);
    X.list_distort = dev_buffer((VkDeviceSize)VFX_MAX_DISTORT * 4, 0);
    X.subs = dev_buffer((VkDeviceSize)VFX_MAX_SUBS * 48, 0);
    X.counters = dev_buffer(sizeof(GpuCounters), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    X.emitters = vkr_buffer_create(sizeof(GpuEmitter) * VFX_MAX_EMITTERS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    X.luts = vkr_buffer_create(sizeof(vec4) * VFX_MAX_EMITTERS * 4 * VFX_LUT_SAMPLES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    memset(X.emitters.mapped, 0, sizeof(GpuEmitter) * VFX_MAX_EMITTERS);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VfxFrameRes *f = &X.fr[i];
        f->ubo = vkr_buffer_create(sizeof(VfxUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f->spawns = vkr_buffer_create(sizeof(VfxGpuSpawn) * VFX_MAX_GPU_SPAWNS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f->prefix = vkr_buffer_create(sizeof(u32) * VFX_MAX_GPU_SPAWNS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f->sprites = vkr_buffer_create(sizeof(RenderVfxSprite) * VFX_MAX_SPRITES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f->decals = vkr_buffer_create(sizeof(RenderVfxDecal) * VFX_MAX_GPU_DECALS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f->instances = vkr_buffer_create(sizeof(vec4) * 2 * VFX_MAX_INSTANCES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f->readback = vkr_buffer_create_readback(sizeof(GpuCounters));
        memset(f->instances.mapped, 0, sizeof(vec4) * 2 * VFX_MAX_INSTANCES);
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = X.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &X.layout;
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &f->set));
    }
    VkSamplerCreateInfo s = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &X.atlas_sampler));
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.maxLod = 0;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &X.noise_sampler));
    s.magFilter = s.minFilter = VK_FILTER_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(R->device, &s, NULL, &X.depth_sampler));
    create_noise();
    // Placeholder 1-page atlas until the library loads.
    u8 *blank = (u8 *)calloc((size_t)VFX_PAGE * VFX_PAGE, 4);
    write_static_descriptors();
    upload_atlas(&blank, 1);
    free(blank);
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 8 * FRAMES_IN_FLIGHT;
    VK_CHECK(vkCreateQueryPool(R->device, &qi, NULL, &X.qp));
    X.ts_period = R->gpu_props.limits.timestampPeriod;
    X.need_reset = true;
    X.sort_n = 1024;
    vkx_targets_changed();
    bool ok = vkx_create_pipelines();
    log_info("vfx: GPU pool %u particles (%.0f MB), sort cap %u", M, (double)M * 84.0 / 1048576.0, VFX_MAX_SORT);
    return ok;
}

void vkx_shutdown(void) {
    if (!X.layout) return;
    vkDeviceWaitIdle(R->device);
    destroy_pipelines();
    GpuBuffer *bufs[] = { &X.pos, &X.vel, &X.misc, &X.info, &X.dead, &X.alive_a, &X.alive_b, &X.counters, &X.emitters, &X.luts,
                          &X.list_add, &X.sort_keys, &X.list_distort, &X.subs };
    for (int i = 0; i < (int)ARRAY_COUNT(bufs); i++) vkr_buffer_destroy_now(bufs[i]);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VfxFrameRes *f = &X.fr[i];
        GpuBuffer *fb[] = { &f->ubo, &f->spawns, &f->prefix, &f->sprites, &f->decals, &f->instances, &f->readback };
        for (int k = 0; k < (int)ARRAY_COUNT(fb); k++) vkr_buffer_destroy_now(fb[k]);
    }
    destroy_atlas();
    vkr_target_destroy(&X.scene_copy);
    if (X.noise_view) vkDestroyImageView(R->device, X.noise_view, NULL);
    if (X.noise) vmaDestroyImage(R->vma, X.noise, X.noise_alloc);
    vkDestroySampler(R->device, X.atlas_sampler, NULL);
    vkDestroySampler(R->device, X.noise_sampler, NULL);
    vkDestroySampler(R->device, X.depth_sampler, NULL);
    vkDestroyQueryPool(R->device, X.qp, NULL);
    vkDestroyPipelineLayout(R->device, X.comp_layout, NULL);
    vkDestroyPipelineLayout(R->device, X.gfx_layout, NULL);
    vkDestroyDescriptorPool(R->device, X.pool, NULL);
    vkDestroyDescriptorSetLayout(R->device, X.layout, NULL);
    vkDestroyDescriptorSetLayout(R->device, X.fog_layout, NULL);
    memset(&X, 0, sizeof(X));
}

// ------------------------------------------------------------------ per frame

void render_vfx_frame(const RenderVfxFrame *in) {
    if (!X.layout || !R->frame_active) return;
    VfxFrameRes *f = &X.fr[R->frame_slot];
    X.in = *in;
    X.in.spawns = NULL;
    X.in.sprites = NULL;
    X.in.decals = NULL;
    X.in.instances = NULL;
    // Spawn records + prefix sums.
    int n = DG_MIN(in->spawn_count, VFX_MAX_GPU_SPAWNS);
    const VfxGpuSpawn *sp = (const VfxGpuSpawn *)in->spawns;
    VfxGpuSpawn *dst = (VfxGpuSpawn *)f->spawns.mapped;
    u32 *prefix = (u32 *)f->prefix.mapped;
    u32 total = 0;
    int out = 0;
    for (int i = 0; i < n; i++) {
        if (!sp[i].info[2]) continue;
        dst[out] = sp[i];
        prefix[out] = total;
        total += sp[i].info[2];
        out++;
    }
    f->spawn_particles = total;
    f->spawn_records = (u32)out;
    if (out > 0) X.frames_since_spawn = 0;
    int sa = DG_MIN(in->sprite_add, (int)VFX_MAX_SPRITES);
    int sb = DG_MIN(in->sprite_alpha, (int)VFX_MAX_SPRITES - sa);
    if (in->sprites && sa + sb > 0) memcpy(f->sprites.mapped, in->sprites, sizeof(RenderVfxSprite) * (size_t)(sa + sb));
    f->sprite_add = sa;
    f->sprite_alpha = sb;
    int dc = DG_MIN(in->decal_count, (int)VFX_MAX_GPU_DECALS);
    if (in->decals && dc > 0) memcpy(f->decals.mapped, in->decals, sizeof(RenderVfxDecal) * (size_t)dc);
    f->decal_count = dc;
    f->decal_dynamic = DG_CLAMP(in->decal_dynamic, 0, dc);
    int ic = DG_MIN(in->instance_count, VFX_MAX_INSTANCES);
    if (in->instances && ic > 0) memcpy(f->instances.mapped, in->instances, sizeof(vec4) * 2 * (size_t)ic);
    if (in->reset) X.need_reset = true;
    f->fed = true;
}

void render_vfx_stats(RenderVfxStats *out) { *out = X.stats; }
void render_set_exposure_boost(float b) { if (R) R->exposure_boost = b; }

static void read_back(VfxFrameRes *f) {
    // Results of this slot's previous use (its fence was waited in render_begin_frame).
    if (f->readback_valid) {
        GpuCounters c;
        memcpy(&c, f->readback.mapped, sizeof(c));
        u32 cur = (X.frame_index + 1) & 1;   // alive list written by that frame's update
        X.stats.alive = c.alive[0] > c.alive[1] ? c.alive[0] : c.alive[1];
        (void)cur;
        X.stats.emitted = c.emitted[0];
        X.stats.dropped = c.emitted[1];
        X.stats.add = c.add;
        X.stats.alpha = c.alpha;
        X.stats.distort = c.distort;
        X.last_alpha = c.alpha;
        X.last_distort = c.distort;
    }
    u64 ts[8];
    int base = (int)R->frame_slot * 8;
    if (f->readback_valid && vkGetQueryPoolResults(R->device, X.qp, (u32)base, 8, sizeof(ts), ts, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        float k = X.ts_period * 1e-6f;
        X.stats.ms_compute = (float)(ts[1] - ts[0]) * k;
        X.stats.ms_decals = (float)(ts[3] - ts[2]) * k;
        X.stats.ms_particles = (float)(ts[5] - ts[4]) * k;
        X.stats.ms_bloom = 0.0f;
        for (int i = 0; i < fg_pass_count(); i++)
            if (!strcmp(fg_pass_name(i), "bloom")) X.stats.ms_bloom = fg_pass_ms(i);   // post stack bloom
    }
    X.stats.pool = X.max_particles;
    X.stats.atlas_pages = X.pages;
    X.stats.atlas_textures = X.atlas_textures;
}

void vkx_timestamp(VkCommandBuffer cmd, int slot) {
    if (!X.qp) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, X.qp, R->frame_slot * 8 + (u32)slot);
}

void vkx_begin_frame_queries(VkCommandBuffer cmd) {
    if (X.qp) vkCmdResetQueryPool(cmd, X.qp, R->frame_slot * 8, 8);
}

void vkx_record_compute(VkCommandBuffer cmd) {
    if (!X.layout) return;
    VfxFrameRes *f = &X.fr[R->frame_slot];
    read_back(f);
    vkx_timestamp(cmd, 0);
    if (!X.ok) { vkx_timestamp(cmd, 1); f->readback_valid = true; return; }
    if (!f->fed) { f->spawn_particles = f->spawn_records = 0; f->sprite_add = f->sprite_alpha = f->decal_count = f->decal_dynamic = 0; }
    f->fed = false;
    // Sort size from the last known alpha count (2 frames old): next power of two with headroom.
    u32 want = X.last_alpha + X.last_alpha / 3 + 256;
    u32 cap = X.in.max_sorted > 0 ? (u32)X.in.max_sorted : VFX_MAX_SORT;
    if (cap > VFX_MAX_SORT) cap = VFX_MAX_SORT;
    u32 n = 512;
    while (n < want && n < cap) n <<= 1;
    X.sort_n = n;
    VfxUbo *u = (VfxUbo *)f->ubo.mapped;
    RenderCamera *c = &R->cam;
    u->view_proj = R->view_proj;
    u->prev_view_proj = X.depth_valid ? X.prev_vp : R->view_proj;
    u->inv_view_proj = mat4_inverse(R->view_proj);
    u->view = R->view;
    u->proj = R->proj;
    quat q = quat_euler(c->yaw, c->pitch, c->roll);
    vec3 right = quat_rotate(q, v3(1, 0, 0)), up = quat_rotate(q, v3(0, 1, 0)), fwd = quat_rotate(q, v3(0, 0, -1));
    float dt = X.in.dt;
    u->cam_pos = v4(c->pos.x, c->pos.y, c->pos.z, (float)g_engine.real_time);
    u->cam_right = v4(right.x, right.y, right.z, dt);
    u->cam_up = v4(up.x, up.y, up.z, X.in.timescale);
    u->cam_fwd = v4(fwd.x, fwd.y, fwd.z, 0);
    u->wind = v4(X.in.wind.x, X.in.wind.y, X.in.wind.z, X.in.exposure_visibility > 0 ? X.in.exposure_visibility : 1.0f);
    u->screen = v4((float)R->hdr.w, (float)R->hdr.h, 1.0f / (float)R->hdr.w, 1.0f / (float)R->hdr.h);
    u->depth_params = v4(c->znear, c->zfar, X.depth_valid ? 1.0f : 0.0f, 0.35f);
    u->counts[0] = f->spawn_particles;
    u->counts[1] = f->spawn_records;
    u->counts[2] = n;
    u->counts[3] = X.frame_index;
    u->caps[0] = X.max_particles;
    u->caps[1] = VFX_MAX_SORT;
    u->caps[2] = VFX_MAX_DISTORT;
    u->caps[3] = (X.in.lighting ? 1u : 0u) | (X.in.collision ? 2u : 0u);
    u->misc = v4(0, 0, (float)(f->sprite_add + f->sprite_alpha), (float)f->decal_count);
    X.prev_vp = R->view_proj;

    VkDescriptorSet set = f->set;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.comp_layout, 0, 1, &set, 0, NULL);
    // Make last frame's draws (vertex reads of the lists) visible before compute rewrites them,
    // and the scene depth readable.
    mem_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                CS, RW | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (X.need_reset) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_reset);
        vkCmdDispatch(cmd, (X.max_particles + 255) / 256, 1, 1);
        cs_barrier(cmd);
        X.need_reset = false;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_begin);
    vkCmdDispatch(cmd, 1, 1, 1);
    cs_barrier(cmd);
    if (f->spawn_particles) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_emit);
        vkCmdDispatch(cmd, (f->spawn_particles + 63) / 64, 1, 1);
        cs_barrier(cmd);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_emit_sub);
    vkCmdDispatch(cmd, VFX_MAX_SUBS / 64, 1, 1);
    cs_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_args);
    vkCmdDispatch(cmd, 1, 1, 1);
    mem_barrier(cmd, CS, RW, CS | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, RW | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_update);
    vkCmdDispatchIndirect(cmd, X.counters.buffer, offsetof(GpuCounters, update_args));
    cs_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_draw_args);
    vkCmdDispatch(cmd, 1, 1, 1);
    cs_barrier(cmd);
    // Bitonic sort of the alpha list (n entries, padded).
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.c_sort);
    u32 pc[4] = { 0, 0, n, 0 };
    vkCmdPushConstants(cmd, X.comp_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, (n + 255) / 256, 1, 1);
    cs_barrier(cmd);
    for (u32 k = 2; k <= n; k <<= 1)
        for (u32 j = k >> 1; j > 0; j >>= 1) {
            u32 p2[4] = { j, k, n, 1 };
            vkCmdPushConstants(cmd, X.comp_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p2), p2);
            vkCmdDispatch(cmd, (n + 255) / 256, 1, 1);
            cs_barrier(cmd);
        }
    // Stats readback (read 2 frames later) and hand-off to the draws.
    mem_barrier(cmd, CS, RW, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy cp = { 0, 0, sizeof(GpuCounters) };
    vkCmdCopyBuffer(cmd, X.counters.buffer, f->readback.buffer, 1, &cp);
    mem_barrier(cmd, CS | VK_PIPELINE_STAGE_2_TRANSFER_BIT, RW | VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    mem_barrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    f->readback_valid = true;
    X.frame_index++;
    X.depth_valid = true;
    X.frames_since_spawn++;
    X.stats.sort_n = n;
    vkx_timestamp(cmd, 1);
}

// Set 2: the froxel fog volume of the main view (the transparent pass reads it), else any 3D view
// (the shader then uses the frame's analytic fog: frame.post_fog.x / frame.fog).
static void push_fog(VkCommandBuffer cmd, bool main_view) {
    VkDescriptorImageInfo img;
    post_froxel_image(main_view, &img);
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.gfx_layout, 2, 1, &w);
}

static void bind_gfx(VkCommandBuffer cmd) {
    VkDescriptorSet sets[2] = { R->frames[R->frame_slot].frame_set, X.fr[R->frame_slot].set };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.gfx_layout, 0, 2, sets, 0, NULL);
}

static void push_mode(VkCommandBuffer cmd, u32 mode) {
    u32 pc[4] = { mode, 0, 0, 0 };
    vkCmdPushConstants(cmd, X.gfx_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
}

// The scope picture-in-picture (render.c exec_scope_pip): the same particles through the scope
// camera's frame set; sorted alpha + additive (no distortion: it refracts the main view's image).
void vkx_draw_particles_pip(VkCommandBuffer cmd, VkDescriptorSet frame_set) {
    VfxFrameRes *f = &X.fr[R->frame_slot];
    if (!X.ok) return;
    VkDescriptorSet sets[2] = { frame_set, f->set };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.gfx_layout, 0, 2, sets, 0, NULL);
    push_fog(cmd, false);   // the scope camera's frame: analytic fog only
    u32 pc[4] = { 1, 1, 0, 0 };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.g_alpha);
    vkCmdPushConstants(cmd, X.gfx_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDrawIndirect(cmd, X.counters.buffer, offsetof(GpuCounters, draw_alpha), 1, 16);
    if (f->sprite_alpha > 0) {
        pc[0] = 3;
        vkCmdPushConstants(cmd, X.gfx_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
        vkCmdDraw(cmd, 6, (u32)f->sprite_alpha, 0, (u32)f->sprite_add);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.g_add);
    pc[0] = 0;
    vkCmdPushConstants(cmd, X.gfx_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdDrawIndirect(cmd, X.counters.buffer, offsetof(GpuCounters, draw_add), 1, 16);
    if (f->sprite_add > 0) {
        pc[0] = 3;
        vkCmdPushConstants(cmd, X.gfx_layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
        vkCmdDraw(cmd, 6, (u32)f->sprite_add, 0, 0);
    }
    R->stats.draw_calls += 4;
}

// Static decals before the characters; the skinned ones (wounds, stains) after them, so they
// project onto the character's own depth.
void vkx_draw_decals(VkCommandBuffer cmd, bool dynamic) {
    if (!dynamic) vkx_timestamp(cmd, 2);
    VfxFrameRes *f = &X.fr[R->frame_slot];
    int ns = f->decal_count - f->decal_dynamic;
    int first = dynamic ? ns : 0, count = dynamic ? f->decal_dynamic : ns;
    if (X.ok && count > 0) {
        bind_gfx(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.g_decal);
        vkCmdDraw(cmd, 36, (u32)count, 0, (u32)first);
        R->stats.draw_calls++;
    }
    if (!dynamic) vkx_timestamp(cmd, 3);
}

RenderTarget *vkx_scene_copy(void) { return X.layout && X.scene_copy.image ? &X.scene_copy : NULL; }

// Always copied: distortion particles refract it and TAA compares against it (opaque vs.
// transparent pixels, "hdr_opaque"). Runs inside a frame graph pass that holds the HDR target
// as a colour attachment; the layouts are restored before returning.
void vkx_copy_scene(VkCommandBuffer cmd) {
    if (!X.layout || !X.scene_copy.image) return;
    vkr_image_barrier(cmd, R->hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT, 1);
    vkr_image_barrier(cmd, X.scene_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
    VkImageCopy ic = { 0 };
    ic.srcSubresource.aspectMask = ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = ic.dstSubresource.layerCount = 1;
    ic.extent.width = R->hdr.w;
    ic.extent.height = R->hdr.h;
    ic.extent.depth = 1;
    vkCmdCopyImage(cmd, R->hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, X.scene_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    vkr_image_barrier(cmd, X.scene_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
    vkr_image_barrier(cmd, R->hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, 1);
}

void vkx_draw_particles(VkCommandBuffer cmd) {
    vkx_timestamp(cmd, 4);
    VfxFrameRes *f = &X.fr[R->frame_slot];
    if (X.ok) {
        bind_gfx(cmd);
        push_fog(cmd, true);
        // Distortion first (refracts the scene copy), then sorted alpha (GPU, then CPU sprites),
        // then additive (order-independent).
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.g_distort);
        push_mode(cmd, 2);
        vkCmdDrawIndirect(cmd, X.counters.buffer, offsetof(GpuCounters, draw_distort), 1, 16);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.g_alpha);
        push_mode(cmd, 1);
        vkCmdDrawIndirect(cmd, X.counters.buffer, offsetof(GpuCounters, draw_alpha), 1, 16);
        if (f->sprite_alpha > 0) {
            push_mode(cmd, 3);
            vkCmdDraw(cmd, 6, (u32)f->sprite_alpha, 0, (u32)f->sprite_add);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.g_add);
        push_mode(cmd, 0);
        vkCmdDrawIndirect(cmd, X.counters.buffer, offsetof(GpuCounters, draw_add), 1, 16);
        if (f->sprite_add > 0) {
            push_mode(cmd, 3);
            vkCmdDraw(cmd, 6, (u32)f->sprite_add, 0, 0);
        }
        R->stats.draw_calls += 5;
    }
    vkx_timestamp(cmd, 5);
}
