// AMD FidelityFX Super Resolution 2.2 (r_upscaler 3): temporal upscaling of the jittered HDR frame to
// the output resolution, or native-resolution AA when the render scale is 1. The SDK's own compute
// shaders (third_party/fsr2, MIT; CMakeLists builds one permutation per pass: HDR input, render-
// resolution unjittered motion vectors, standard depth, FP32, no RCAS: the final pass sharpens) are
// dispatched from the frame graph; this file is the host side of ffx_fsr2.cpp in C (constants, jitter
// phases, resources, the dispatch order). Each pass's descriptor set layout comes from its SPIR-V
// (spv_reflect.c): set 0 holds the two samplers (a regular set, written once), set 1 the resources
// (push descriptors).
//   fsr2_reset        on a reset (first frame, resize, camera cut): the persistent images cleared
//   fsr2_prep         (ours, shaders/post/fsr2_prep.comp) depth with the viewmodel, reactive mask, exposure
//   fsr2_luminance    SPD: scene luminance mips (shading change, mip 5), FSR 2's own auto exposure
//   fsr2_reconstruct  dilated motion vectors / depth, the reconstructed previous depth, luma history
//   fsr2_depth_clip   disocclusion, dilated reactive masks, the prepared colour (YCoCg)
//   fsr2_lock         new locks (thin features); clears the reconstructed depth for the next frame
//   fsr2_accumulate   the upscaled colour (its history) and the output
// docs/RENDERING_POST.md "FSR 2".
#include "post_internal.h"
#include "render/spv_reflect.h"
#include "core/log.h"
#include "core/platform.h"
#include "../../../third_party/fsr2/ffx_fsr2_maximum_bias.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// What a pass's binding refers to.
typedef enum FsrRes {
    FR_COLOR, FR_DEPTH, FR_MV, FR_EXPOSURE, FR_REACTIVE, FR_TCMASK,
    FR_RECON_DEPTH, FR_DIL_MV, FR_PREV_DIL_MV, FR_DIL_DEPTH, FR_PREP_COLOR,
    FR_LUMA_PREV, FR_LUMA_CUR, FR_LOCK_LUMA, FR_NEW_LOCKS, FR_LOCK_PREV, FR_LOCK_CUR,
    FR_UPS_PREV, FR_UPS_CUR, FR_OUTPUT, FR_LUM, FR_LUM_MIP_SC, FR_LUM_MIP5, FR_AUTO_EXP, FR_SPD_ATOMIC,
    FR_DIL_REACTIVE, FR_LANCZOS, FR_BIAS, FR_CB_FSR2, FR_CB_SPD, FR_COUNT
} FsrRes;

typedef struct FsrSlot { u8 binding, res; } FsrSlot;

// The bindings of each pass (the FSR2_BIND_* defines of third_party/fsr2/ffx_fsr2_*_pass.glsl, as
// the host table of ffx_fsr2.cpp maps them; SRVs of ping-pong resources read the previous frame).
static const FsrSlot k_luminance[] = { { 0, FR_COLOR }, { 1, FR_SPD_ATOMIC }, { 2, FR_LUM_MIP_SC }, { 3, FR_LUM_MIP5 }, { 4, FR_AUTO_EXP }, { 5, FR_CB_FSR2 }, { 6, FR_CB_SPD } };
static const FsrSlot k_reconstruct[] = { { 0, FR_MV }, { 1, FR_DEPTH }, { 2, FR_COLOR }, { 3, FR_EXPOSURE }, { 4, FR_LUMA_PREV }, { 5, FR_RECON_DEPTH }, { 6, FR_DIL_MV },
                                         { 7, FR_DIL_DEPTH }, { 8, FR_PREP_COLOR }, { 9, FR_LUMA_CUR }, { 11, FR_LOCK_LUMA }, { 12, FR_CB_FSR2 } };
static const FsrSlot k_depth_clip[] = { { 0, FR_RECON_DEPTH }, { 1, FR_DIL_MV }, { 2, FR_DIL_DEPTH }, { 3, FR_REACTIVE }, { 4, FR_TCMASK }, { 5, FR_PREP_COLOR }, { 6, FR_PREV_DIL_MV },
                                        { 7, FR_MV }, { 8, FR_COLOR }, { 9, FR_DEPTH }, { 10, FR_EXPOSURE }, { 12, FR_DIL_REACTIVE }, { 13, FR_PREP_COLOR }, { 14, FR_CB_FSR2 } };
static const FsrSlot k_lock[] = { { 0, FR_LOCK_LUMA }, { 1, FR_NEW_LOCKS }, { 2, FR_RECON_DEPTH }, { 3, FR_CB_FSR2 } };
static const FsrSlot k_accumulate[] = { { 0, FR_EXPOSURE }, { 1, FR_DIL_REACTIVE }, { 2, FR_DIL_MV }, { 3, FR_UPS_PREV }, { 4, FR_LOCK_PREV }, { 6, FR_PREP_COLOR },
                                        { 8, FR_LANCZOS }, { 9, FR_BIAS }, { 10, FR_LUM }, { 11, FR_AUTO_EXP }, { 12, FR_LUMA_PREV }, { 13, FR_UPS_CUR },
                                        { 14, FR_LOCK_CUR }, { 15, FR_OUTPUT }, { 16, FR_NEW_LOCKS }, { 17, FR_LUMA_CUR }, { 18, FR_CB_FSR2 } };

enum { FP_LUMINANCE, FP_RECONSTRUCT, FP_DEPTH_CLIP, FP_LOCK, FP_ACCUMULATE, FP_COUNT };

typedef struct FsrPass {
    const char *shader;              // shaders/fsr2/<shader>.comp.spv
    const char *name;                // frame graph pass
    const FsrSlot *slots;
    int nslots;
    SpvBinding refl[32];
    int nrefl;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe;
} FsrPass;

#define SLOTS(a) a, (int)(sizeof(a) / sizeof(a[0]))
static FsrPass g_pass[FP_COUNT] = {
    { "compute_luminance_pyramid", "fsr2_luminance", SLOTS(k_luminance) },
    { "reconstruct_previous_depth", "fsr2_reconstruct", SLOTS(k_reconstruct) },
    { "depth_clip", "fsr2_depth_clip", SLOTS(k_depth_clip) },
    { "lock", "fsr2_lock", SLOTS(k_lock) },
    { "accumulate", "fsr2_accumulate", SLOTS(k_accumulate) },
};

// Must match cbFSR2_t (third_party/fsr2/ffx_fsr2_callbacks_glsl.h, std140: the vec4 sits at 48).
typedef struct Fsr2Constants {
    i32 render_size[2], max_render_size[2], display_size[2], input_color_dims[2], luma_mip_dims[2];
    i32 luma_mip_level, frame_index;
    float device_to_view_depth[4];
    float jitter[2], mv_scale[2], downscale[2], mv_jitter_cancel[2];
    float pre_exposure, prev_pre_exposure, tan_half_fov, jitter_seq_len, delta_time, dynres_change, view_to_meters;
} Fsr2Constants;
typedef struct Fsr2SpdConstants { u32 mips, num_work_groups, work_group_offset[2], render_size[2]; } Fsr2SpdConstants;
#define CB_SPD_OFFSET 256   // the largest minUniformBufferOffsetAlignment the spec allows

static struct {
    bool init, failed;
    VkDescriptorSetLayout sampler_dsl;
    VkDescriptorPool pool;
    VkDescriptorSet sampler_set;
    GpuBuffer ubo[FRAMES_IN_FLIGHT];
    VkImage lut_img[2];
    VmaAllocation lut_alloc[2];
    VkImageView lut_view[2];       // Lanczos 2 (128 x 1), maximum upsample bias (16 x 16), half floats
    // per frame
    FgRes r[FR_COUNT];
    u32 frame_index;
    bool reset, vm;
    u32 groups_spd[2], render_w, render_h, out_w, out_h;
} F;

static u16 half_bits(float f) {
    u32 x;
    memcpy(&x, &f, 4);
    u32 s = (x >> 16) & 0x8000u, e = (x >> 23) & 0xFFu, m = x & 0x7FFFFFu;
    if (e < 113) return (u16)s;   // tiny values flush to zero (the LUTs hold none)
    if (e > 142) return (u16)(s | 0x7C00u);
    return (u16)(s | ((e - 112) << 10) | (m >> 13));
}

static float lanczos2(float x) {
    const float pi = 3.14159265f;
    return x == 0.0f ? 1.0f : (sinf(pi * x) / (pi * x)) * (sinf(0.5f * pi * x) / (0.5f * pi * x));
}

static bool create_pass(FsrPass *p) {
    char path[512];
    snprintf(path, sizeof(path), "%sshaders/fsr2/%s.comp.spv", platform_base_dir(), p->shader);
    size_t size = 0;
    char *code = platform_read_file(path, &size);
    if (!code || size % 4) { free(code); log_error("fsr2: cannot load %s", path); return false; }
    p->nrefl = spv_reflect_bindings((const u32 *)code, size / 4, 1, p->refl, 32);
    if (p->nrefl <= 0) { free(code); log_error("fsr2: %s: no resources reflected", p->shader); return false; }
    // Every binding the shader declares needs a resource of ours.
    VkDescriptorSetLayoutBinding b[32];
    for (int i = 0; i < p->nrefl; i++) {
        bool mapped = false;
        for (int k = 0; k < p->nslots && !mapped; k++) mapped = p->slots[k].binding == p->refl[i].binding;
        if (!mapped) { free(code); log_error("fsr2: %s: binding %u has no resource", p->shader, p->refl[i].binding); return false; }
        static const VkDescriptorType types[] = { VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
        b[i] = (VkDescriptorSetLayoutBinding){ p->refl[i].binding, types[p->refl[i].kind], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = (u32)p->nrefl;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &p->dsl));
    VkDescriptorSetLayout sets[2] = { F.sampler_dsl, p->dsl };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &p->pl));
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = size;
    mi.pCode = (const u32 *)code;
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(R->device, &mi, NULL, &m);
    free(code);
    if (r != VK_SUCCESS) { log_error("fsr2: %s: shader module failed (%d)", p->shader, (int)r); return false; }
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = p->pl;
    r = vkCreateComputePipelines(R->device, R->pipeline_cache, 1, &ci, NULL, &p->pipe);
    vkDestroyShaderModule(R->device, m, NULL);
    if (r != VK_SUCCESS) { log_error("fsr2: %s: pipeline failed (%d)", p->shader, (int)r); p->pipe = VK_NULL_HANDLE; return false; }
    return true;
}

static bool fsr2_init(void) {
    if (F.init) return !F.failed;
    F.init = true;
    // Set 0: s_PointClamp, s_LinearClamp (a regular set: one push descriptor set per layout).
    VkDescriptorSetLayoutBinding sb[2] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                           { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = sb;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &F.sampler_dsl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(R->device, &pci, NULL, &F.pool));
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = F.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &F.sampler_dsl;
    VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &F.sampler_set));
    VkDescriptorImageInfo ii[2] = { { P.point, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED }, { P.linear, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED } };
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstSet = F.sampler_set;
        w[i].dstBinding = (u32)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(R->device, 2, w, 0, NULL);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) F.ubo[i] = vkr_buffer_create(512, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    // The look-up tables (ffx_fsr2.cpp fsr2Create), as half floats: the bias table holds bias / 2.
    u16 lanczos[128], bias[16 * 16];
    for (int i = 0; i < 128; i++) lanczos[i] = half_bits(lanczos2(2.0f * (float)i / 127.0f));
    for (int i = 0; i < 16 * 16; i++) bias[i] = half_bits(ffxFsr2MaximumBias[i] / 2.0f);
    post_texture_make(&F.lut_img[0], &F.lut_alloc[0], &F.lut_view[0], VK_FORMAT_R16_SFLOAT, 128, 1, lanczos, sizeof(lanczos));
    post_texture_make(&F.lut_img[1], &F.lut_alloc[1], &F.lut_view[1], VK_FORMAT_R16_SFLOAT, 16, 16, bias, sizeof(bias));
    for (int p = 0; p < FP_COUNT; p++)
        if (!create_pass(&g_pass[p])) F.failed = true;
    if (F.failed) log_error("fsr2: passes unavailable - falling back to TAA-U");
    else log_info("fsr2: FidelityFX FSR 2.2 ready (%d passes)", FP_COUNT);
    return !F.failed;
}

void post_fsr2_shutdown(void) {
    if (!F.init) return;
    for (int p = 0; p < FP_COUNT; p++) {
        FsrPass *s = &g_pass[p];
        if (s->pipe) vkDestroyPipeline(R->device, s->pipe, NULL);
        if (s->pl) vkDestroyPipelineLayout(R->device, s->pl, NULL);
        if (s->dsl) vkDestroyDescriptorSetLayout(R->device, s->dsl, NULL);
        s->pipe = VK_NULL_HANDLE;
        s->pl = VK_NULL_HANDLE;
        s->dsl = VK_NULL_HANDLE;
    }
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkr_buffer_destroy_now(&F.ubo[i]);
    for (int i = 0; i < 2; i++) {
        if (F.lut_view[i]) vkDestroyImageView(R->device, F.lut_view[i], NULL);
        if (F.lut_img[i]) vmaDestroyImage(R->vma, F.lut_img[i], F.lut_alloc[i]);
    }
    if (F.pool) vkDestroyDescriptorPool(R->device, F.pool, NULL);
    if (F.sampler_dsl) vkDestroyDescriptorSetLayout(R->device, F.sampler_dsl, NULL);
    memset(&F, 0, sizeof(F));
}

// ffxFsr2GetJitterPhaseCount: 8 x (display / render)^2 phases of Halton(2, 3).
int post_fsr2_jitter_phases(u32 render_w, u32 display_w) {
    float r = render_w ? (float)display_w / (float)render_w : 1.0f;
    return DG_MAX(1, (int)(8.0f * r * r));
}

// ------------------------------------------------------------------ passes

static bool slot_is_storage(const FsrPass *p, int binding) {
    for (int i = 0; i < p->nrefl; i++) if ((int)p->refl[i].binding == binding) return p->refl[i].kind == SPV_DESC_STORAGE_IMAGE;
    return false;
}
static bool res_written(const FsrPass *p, int res) {
    for (int k = 0; k < p->nslots; k++) if (p->slots[k].res == res && slot_is_storage(p, p->slots[k].binding)) return true;
    return false;
}

static VkImageView res_view(int res, bool storage) {
    switch (res) {
    case FR_TCMASK: return P.black;
    case FR_LANCZOS: return F.lut_view[0];
    case FR_BIAS: return F.lut_view[1];
    case FR_LUM_MIP_SC: return fg_mip_view(F.r[FR_LUM], 4);
    case FR_LUM_MIP5: return fg_mip_view(F.r[FR_LUM], 5);
    default: break;
    }
    (void)storage;
    return fg_valid(F.r[res]) ? fg_view(F.r[res]) : P.white;
}

static void exec_fsr_pass(VkCommandBuffer cmd, void *u) {
    FsrPass *p = &g_pass[(int)(intptr_t)u];
    VkWriteDescriptorSet w[32];
    VkDescriptorImageInfo img[32];
    VkDescriptorBufferInfo buf[32];
    int n = 0;
    for (int i = 0; i < p->nrefl; i++) {
        int res = -1;
        for (int k = 0; k < p->nslots; k++) if (p->slots[k].binding == p->refl[i].binding) res = p->slots[k].res;
        w[n] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[n].dstBinding = p->refl[i].binding;
        w[n].descriptorCount = 1;
        if (p->refl[i].kind == SPV_DESC_UNIFORM_BUFFER) {
            buf[n] = (VkDescriptorBufferInfo){ F.ubo[R->frame_slot].buffer, res == FR_CB_SPD ? CB_SPD_OFFSET : 0, res == FR_CB_SPD ? sizeof(Fsr2SpdConstants) : sizeof(Fsr2Constants) };
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[n].pBufferInfo = &buf[n];
        } else {
            bool storage = p->refl[i].kind == SPV_DESC_STORAGE_IMAGE;
            // A sampled view of an image this pass also writes is in GENERAL (the frame graph's choice).
            bool general = storage || (res >= 0 && res != FR_TCMASK && res != FR_LANCZOS && res != FR_BIAS && res_written(p, res));
            img[n] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, res_view(res, storage), general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            w[n].descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[n].pImageInfo = &img[n];
        }
        n++;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pl, 0, 1, &F.sampler_set, 0, NULL);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pl, 1, (u32)n, w);
    u32 gx, gy;
    switch ((int)(intptr_t)u) {
    case FP_LUMINANCE: gx = F.groups_spd[0], gy = F.groups_spd[1]; break;
    case FP_ACCUMULATE: gx = (F.out_w + 7) / 8, gy = (F.out_h + 7) / 8; break;
    default: gx = (F.render_w + 7) / 8, gy = (F.render_h + 7) / 8; break;
    }
    vkCmdDispatch(cmd, gx, gy, 1);
}

static void exec_prep(VkCommandBuffer cmd, void *u) {
    (void)u;
    post_buffer_barrier(cmd);   // last frame's exposure (exposure.comp) into this frame's reads
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.in.depth, P.point);
    pb_tex_fg(&b, 1, F.vm ? P.in.vm_depth : FG_NONE, P.point);
    pb_tex_fg(&b, 2, P.in.hdr, P.point);
    pb_tex_fg(&b, 3, fg_valid(P.in.hdr_opaque) ? P.in.hdr_opaque : P.in.hdr, P.point);
    pb_img_fg(&b, 8, F.r[FR_DEPTH]);
    pb_img_fg(&b, 9, F.r[FR_REACTIVE]);
    pb_img_fg(&b, 10, F.r[FR_EXPOSURE]);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = 0.01f;     // render.c R->vm_proj
    pc.p[1] = 100.0f;
    pc.u[0] = fg_valid(P.in.hdr_opaque) ? 1u : 0u;
    pc.u[1] = F.vm ? 1u : 0u;
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "fsr2_prep", F.render_w, F.render_h, 1, 8);
}

// The persistent images FSR 2 expects in a known state (ffx_fsr2.cpp: first execution / reset).
static void exec_reset(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    static const struct { int res; float v[2]; bool uint_bits; } k[] = {
        { FR_LOCK_PREV, { 0, 0 } }, { FR_UPS_PREV, { 0, 0 } }, { FR_LUMA_PREV, { 0, 0 } }, { FR_PREV_DIL_MV, { 0, 0 } },
        { FR_AUTO_EXP, { -1.0f, 1e8f } }, { FR_NEW_LOCKS, { 0, 0 } }, { FR_SPD_ATOMIC, { 0, 0 }, true }, { FR_RECON_DEPTH, { 1.0f, 0 }, true },
    };
    for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); i++) {
        VkClearColorValue cv;
        memset(&cv, 0, sizeof(cv));
        if (k[i].uint_bits) memcpy(&cv.uint32[0], &k[i].v[0], 4);   // the far depth's bits (atomic min); 0 for the counter
        else cv.float32[0] = k[i].v[0], cv.float32[1] = k[i].v[1];
        vkCmdClearColorImage(cmd, fg_image(F.r[k[i].res]), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
    }
}

static FgRes tex(const char *name, VkFormat f, FgSize s, u32 mips) {
    FgTexDesc d = { name, f, s, 0, 0, 0, mips, 0 };
    return fg_texture(&d);
}
static FgRes hist(const char *name, VkFormat f, FgSize s) {
    FgTexDesc d = { name, f, s, s == FG_SIZE_ABS ? 1u : 0u, s == FG_SIZE_ABS ? 1u : 0u, 0, 1, 0 };
    return fg_history(&d);
}

// Constants of this frame (ffx_fsr2.cpp fsr2Dispatch), into this slot's uniform buffer.
static void write_constants(void) {
    const PostFrameIn *in = &P.in;
    Fsr2Constants c;
    memset(&c, 0, sizeof(c));
    c.render_size[0] = c.max_render_size[0] = c.input_color_dims[0] = (i32)F.render_w;
    c.render_size[1] = c.max_render_size[1] = c.input_color_dims[1] = (i32)F.render_h;
    c.display_size[0] = (i32)F.out_w;
    c.display_size[1] = (i32)F.out_h;
    c.luma_mip_level = 4;   // FFX_FSR2_SHADING_CHANGE_MIP_LEVEL
    c.luma_mip_dims[0] = (i32)(F.render_w / (2u << 4));
    c.luma_mip_dims[1] = (i32)(F.render_h / (2u << 4));
    c.frame_index = (i32)F.frame_index;
    // Device depth -> view depth (standard depth, finite far plane).
    float n = in->znear, f = in->zfar, q = f / (n - f);
    float aspect = (float)F.render_w / (float)F.render_h;
    float cot = 1.0f / tanf(0.5f * in->fov_y);
    c.device_to_view_depth[0] = -q;
    c.device_to_view_depth[1] = q * n;
    c.device_to_view_depth[2] = aspect / cot;
    c.device_to_view_depth[3] = 1.0f / cot;
    // The jitter in render pixels: R->jitter is the NDC translation (2 px / size; Vulkan NDC y points
    // down, the same direction as the pixels).
    c.jitter[0] = in->jitter.x * 0.5f * (float)F.render_w;
    c.jitter[1] = in->jitter.y * 0.5f * (float)F.render_h;
    // Our velocity is current - previous in UV units; FSR 2 adds its motion vector to reach the
    // previous position.
    c.mv_scale[0] = c.mv_scale[1] = -1.0f;
    c.downscale[0] = (float)F.render_w / (float)F.out_w;
    c.downscale[1] = (float)F.render_h / (float)F.out_h;
    c.pre_exposure = c.prev_pre_exposure = 1.0f;
    c.tan_half_fov = tanf(0.5f * in->fov_y) * aspect;   // horizontal
    c.jitter_seq_len = (float)post_fsr2_jitter_phases(F.render_w, F.out_w);
    c.delta_time = clampf(in->dt, 0.0f, 1.0f);
    c.view_to_meters = 1.0f;
    Fsr2SpdConstants s;
    memset(&s, 0, sizeof(s));
    // SpdSetup over the render rectangle: 64 x 64 pixels per group, mips from the larger side.
    u32 gx = (F.render_w + 63) / 64, gy = (F.render_h + 63) / 64;
    F.groups_spd[0] = gx;
    F.groups_spd[1] = gy;
    s.num_work_groups = gx * gy;
    s.mips = (u32)fminf(floorf(log2f((float)DG_MAX(F.render_w, F.render_h))), 12.0f);
    s.render_size[0] = F.render_w;
    s.render_size[1] = F.render_h;
    u8 *dst = (u8 *)F.ubo[R->frame_slot].mapped;
    memcpy(dst, &c, sizeof(c));
    memcpy(dst + CB_SPD_OFFSET, &s, sizeof(s));
}

static void declare(int pass, int id) {
    const FsrPass *p = &g_pass[id];
    for (int k = 0; k < p->nslots; k++) {
        int res = p->slots[k].res;
        FgRes r = res == FR_LUM_MIP_SC || res == FR_LUM_MIP5 ? F.r[FR_LUM] : (res < FR_COUNT ? F.r[res] : FG_NONE);
        if (res == FR_TCMASK || res == FR_LANCZOS || res == FR_BIAS || res == FR_CB_FSR2 || res == FR_CB_SPD || r == FG_NONE) continue;
        if (slot_is_storage(p, p->slots[k].binding)) fg_write(pass, r, FG_STORAGE);
        else fg_read(pass, r, FG_SAMPLED);
    }
}

bool post_fsr2_available(void) { return fsr2_init(); }

FgRes post_fsr2_build(FgRes color) {
    if (!fsr2_init()) return FG_NONE;
    memset(F.r, 0xFF, sizeof(F.r));   // FG_NONE
    fg_extent(color, &F.render_w, &F.render_h);
    F.out_w = P.in.output_w;
    F.out_h = P.in.output_h;
    F.vm = fg_valid(P.in.vm_depth);
    bool odd = (P.in.frame & 1u) != 0;
    F.r[FR_COLOR] = color;
    F.r[FR_MV] = P.in.velocity;
    // Ping-pong histories (ffx_fsr2.cpp: odd frames read 2, write 1).
    F.r[FR_LOCK_PREV] = hist(odd ? "fsr2_lock_b" : "fsr2_lock_a", VK_FORMAT_R16G16_SFLOAT, FG_SIZE_OUTPUT);
    F.r[FR_LOCK_CUR] = hist(odd ? "fsr2_lock_a" : "fsr2_lock_b", VK_FORMAT_R16G16_SFLOAT, FG_SIZE_OUTPUT);
    F.r[FR_UPS_PREV] = hist(odd ? "fsr2_upscaled_b" : "fsr2_upscaled_a", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT);
    F.r[FR_UPS_CUR] = hist(odd ? "fsr2_upscaled_a" : "fsr2_upscaled_b", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT);
    F.r[FR_LUMA_PREV] = hist(odd ? "fsr2_luma_b" : "fsr2_luma_a", VK_FORMAT_R8G8B8A8_UNORM, FG_SIZE_OUTPUT);
    F.r[FR_LUMA_CUR] = hist(odd ? "fsr2_luma_a" : "fsr2_luma_b", VK_FORMAT_R8G8B8A8_UNORM, FG_SIZE_OUTPUT);
    F.r[FR_PREV_DIL_MV] = hist(odd ? "fsr2_mv_b" : "fsr2_mv_a", VK_FORMAT_R16G16_SFLOAT, FG_SIZE_RENDER);
    F.r[FR_DIL_MV] = hist(odd ? "fsr2_mv_a" : "fsr2_mv_b", VK_FORMAT_R16G16_SFLOAT, FG_SIZE_RENDER);
    // Persistent, one image: cleared by the passes for the next frame (reconstructed depth: lock; new
    // locks: accumulate; the SPD counter: its last group) or temporal (FSR 2's exposure).
    F.r[FR_RECON_DEPTH] = hist("fsr2_recon_depth", VK_FORMAT_R32_UINT, FG_SIZE_RENDER);
    F.r[FR_NEW_LOCKS] = hist("fsr2_new_locks", VK_FORMAT_R8_UNORM, FG_SIZE_OUTPUT);
    F.r[FR_AUTO_EXP] = hist("fsr2_auto_exposure", VK_FORMAT_R32G32_SFLOAT, FG_SIZE_ABS);
    F.r[FR_SPD_ATOMIC] = hist("fsr2_spd_counter", VK_FORMAT_R32_UINT, FG_SIZE_ABS);
    // This frame only.
    F.r[FR_DEPTH] = tex("fsr2_depth", VK_FORMAT_R32_SFLOAT, FG_SIZE_RENDER, 1);
    F.r[FR_REACTIVE] = tex("fsr2_reactive", VK_FORMAT_R8_UNORM, FG_SIZE_RENDER, 1);
    {
        FgTexDesc d = { "fsr2_exposure", VK_FORMAT_R32G32_SFLOAT, FG_SIZE_ABS, 1, 1, 0, 1, 0 };
        F.r[FR_EXPOSURE] = fg_texture(&d);
    }
    F.r[FR_DIL_DEPTH] = tex("fsr2_dilated_depth", VK_FORMAT_R32_SFLOAT, FG_SIZE_RENDER, 1);
    F.r[FR_PREP_COLOR] = tex("fsr2_prepared_color", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_RENDER, 1);
    F.r[FR_LOCK_LUMA] = tex("fsr2_lock_luma", VK_FORMAT_R16_SFLOAT, FG_SIZE_RENDER, 1);
    F.r[FR_DIL_REACTIVE] = tex("fsr2_dilated_reactive", VK_FORMAT_R8G8_UNORM, FG_SIZE_RENDER, 1);
    F.r[FR_LUM] = tex("fsr2_luminance", VK_FORMAT_R16_SFLOAT, FG_SIZE_HALF, FG_MAX_MIPS);
    F.r[FR_OUTPUT] = tex("fsr2_output", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT, 1);
    if (F.render_w < 64 || F.render_h < 64) return FG_NONE;   // no luminance mip 5 to write (TAA-U then)
    F.reset = P.in.camera_cut || !fg_history_valid(F.r[FR_UPS_PREV]) || !fg_history_valid(F.r[FR_LOCK_PREV]) || !fg_history_valid(F.r[FR_RECON_DEPTH]) ||
              !fg_history_valid(F.r[FR_AUTO_EXP]) || !fg_history_valid(F.r[FR_NEW_LOCKS]) || !fg_history_valid(F.r[FR_SPD_ATOMIC]) ||
              !fg_history_valid(F.r[FR_PREV_DIL_MV]) || !fg_history_valid(F.r[FR_LUMA_PREV]);
    F.frame_index = F.reset ? 0 : F.frame_index + 1;
    write_constants();
    if (F.reset) {
        int p = fg_pass("fsr2_reset", exec_reset, NULL);
        static const int cleared[] = { FR_LOCK_PREV, FR_UPS_PREV, FR_LUMA_PREV, FR_PREV_DIL_MV, FR_AUTO_EXP, FR_NEW_LOCKS, FR_SPD_ATOMIC, FR_RECON_DEPTH };
        for (int i = 0; i < (int)(sizeof(cleared) / sizeof(cleared[0])); i++) fg_write(p, F.r[cleared[i]], FG_TRANSFER_DST);
    }
    int prep = fg_pass("fsr2_prep", exec_prep, NULL);
    fg_read(prep, P.in.depth, FG_SAMPLED);
    if (F.vm) fg_read(prep, P.in.vm_depth, FG_SAMPLED);
    fg_read(prep, P.in.hdr, FG_SAMPLED);
    if (fg_valid(P.in.hdr_opaque)) fg_read(prep, P.in.hdr_opaque, FG_SAMPLED);
    fg_write(prep, F.r[FR_DEPTH], FG_STORAGE);
    fg_write(prep, F.r[FR_REACTIVE], FG_STORAGE);
    fg_write(prep, F.r[FR_EXPOSURE], FG_STORAGE);
    bool ok = prep >= 0;
    for (int i = 0; i < FP_COUNT; i++) {
        int p = fg_pass(g_pass[i].name, exec_fsr_pass, (void *)(intptr_t)i);
        declare(p, i);
        ok = ok && p >= 0;
    }
    return ok ? F.r[FR_OUTPUT] : FG_NONE;
}
