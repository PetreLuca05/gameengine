// Classic renderer of the vgeo scene content (docs/CLASSIC_RENDERER.md): the meshes, materials,
// instances and placement chunks of the vgeo scene tables (vk_vgeo.c), drawn the traditional way.
// The only renderer of that content (the virtualized geometry path lives on branch `nanite`).
//
//   load     every mesh's LOD chain as plain vertex / index buffers: cuts of its DAG at the exported
//            discrete LOD errors, decoded with vertex colours (wind weights) and split per material
//            (meshes without colours and with one material use the exported LODs as they are);
//            impostor atlases as two renderer textures
//   frame    CPU culling over the 8 m placement cells (frustum, per-cell foliage fade / shadow
//            distance), then per instance: frustum, density / fade (the vgeo hash), impostor switch,
//            distance LOD pick. Instances are bucketed per (mesh, LOD) into a per-frame instance
//            stream (48 B each): one instanced draw per bucket and material
//   passes   G-buffer prepass (alpha test + fade dither on masked / dithered materials, back faces
//            kept for two-sided), lit pass at depth EQUAL (opaque materials without discard: early-z;
//            masked ones test again: distant cards can share a depth value), sun cascades and
//            point / spot light tiles (depth only, masked materials alpha-tested). Wind WPO
//            (shaders/vgeo/wpo.glsl, the same snippets) and the grass displacement buffer run in
//            the vertex shader (shaders/classic_vert.glsl); impostors are camera-facing quads that
//            blend the 3 nearest atlas frames (shaders/classic_impostor.glsl).
//   wind     the foliage wind (docs/FOLIAGE_WIND.md): wpo_veg with the material's wind
//            parameters (push constants), the wind field (fallback + tile ring, this frame's and last
//            frame's for velocity, layers, debug view) in a per-frame-slot uniform buffer (set 4)
//   LOD      docs/LOD.md (lod_active): ranges from assets/render/lod.json x tier x era; LOD, impostor,
//            foliage density and shadow distance switches cross-fade over a distance band: an instance
//            in a band is drawn as two or three parts (impostor / coarser / finer LOD) that split the
//            dither threshold range [0, 1) between them (ClInst misc.z), so their coverage sums to 1;
//            fading parts batch apart (key bit 3) and draw through the dithered pipelines. A LOD that is
//            not resident yet draws the nearest resident one (the finer one is prefetched). The local
//            light tiles and r_lod_legacy keep the cuts from before.
//   casters  sun cascades take each mesh's caster class (shadow_casters.h, docs/SHADOWS.md "Caster
//            cascade limits", PF-0013): grass / ground cover only in the near cascade, small plants in
//            the near two, dithering out before the end of their last one; trees everywhere.
#include "core/profiler.h"
#include "vk_internal.h"
#include "lod_fade.h"
#include "shadow_casters.h"
#include "core/platform.h"
#include "fidelity/fidelity.h"
#include "vgeo/vgeo_scene.h"
#include "vgeo/foliage_support.h"
#include "vgeo/vgeo_impostor.h"
#include "vgeo/vgeo_tex.h"
#include "core/jobs.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CL_MATS 8
#define CL_IMPOSTOR 7u              // key LOD slot of the impostor bucket
#define CL_KEY_FADE 8u              // key bit: a fading part (dithered pipelines)
#define CL_KEY_SHIFT 4              // key = mesh << 4 | fade << 3 | LOD
#define CL_PARTS 3                  // parts of one instance at most (impostor, coarser, finer)
#define CL_VIEWS 6                  // main view + 4 sun cascades + the scope picture-in-picture
#define CL_VIEW_PIP 5               // after the cascades (views 1 .. 4)
#define CL_IMPOSTOR_TRIS 256        // meshes this heavy (coarsest LOD) use r_classic_impostor_px

typedef struct ClSection { u32 first_index, index_count, material; } ClSection;
typedef struct ClLod {
    MeshId gpu;
    u32 ready_frame;                // drawable from this frame index (the upload is recorded at the next frame start)
    ClSection sec[CL_MATS];
    u32 nsec, tris;
} ClLod;
typedef struct ClMesh {
    bool used;
    u32 lod_count;
    float lod_error[VGEO_MAX_LODS];
    ClLod lods[VGEO_MAX_LODS];
    // exported discrete LODs, uploaded on first use (meshes without colours, one material)
    VgDiscreteLod src[VGEO_MAX_LODS];
    const VgLodVertex *verts;
    const u32 *indices;
    bool own;
    // impostor atlas (albedo sRGB + normals), set 3 of the impostor pipelines
    TextureId imp_albedo, imp_normal;
    VkDescriptorSet imp_set;
    u32 imp_grid;
    float imp_center[3], imp_radius;
    u32 coarse_tris;                // triangles of the coarsest LOD: small meshes switch to impostors late
} ClMesh;

// Per-instance vertex stream (binding 1): position + scale, rotation, misc: wind phase, fade alpha (foliage
// density / shadow distance), the kept dither interval (lod_iv_pack, 0 = all), debug (LOD + 8 x role).
typedef struct ClInst { float pos[4], rot[4], misc[4]; } ClInst;
// Set 4 binding 0 (shaders/classic_common.glsl ClWind): the VgViewGpu wind fields of the frame.
typedef struct ClWindGpu {
    float wind[4], wind_prev[4], tile[4], tile_prev[4], disp[4], time[4];
    u32 ring, ring_prev, layers, debug;
} ClWindGpu;
typedef struct ClBatch { u32 key, first, count; } ClBatch;   // key = mesh << 4 | fade << 3 | LOD (CL_IMPOSTOR)
typedef struct ClView {
    ClBatch *batches;
    u32 batch_count, batch_cap, instances;
} ClView;
typedef struct ClTask { const VgInstance *inst; u32 n, off, out; } ClTask;
static struct {
    ClTask *t;
    u32 n, cap;
    const struct ClCull *v;
    const VgScene *s;
    u32 parts;                      // output slots per instance (CL_PARTS with the fades, else 1)
} T;   // culling tasks (cull_cells)

enum { CP_GB, CP_GB_2S, CP_GB_CLIP, CP_GB_CLIP_2S, CP_LIT, CP_LIT_2S, CP_LIT_CLIP, CP_LIT_CLIP_2S, CP_SHADOW, CP_SHADOW_MASKED, CP_IMP_GB, CP_IMP_LIT,
       CP_PIP, CP_PIP_2S, CP_PIP_CLIP, CP_PIP_CLIP_2S, CP_PIP_IMP, CP_COUNT };   // CP_PIP*: the scope PiP, forward (lit + depth write)

static struct {
    bool ok;
    ClMesh *mesh;
    u32 mesh_cap;
    const VgTextures *vtex;         // the vgeo texture table (module-owned slots: masks, tree textures, atlases)
    TextureId modtex[VG_TEX_OWN];   // vgeo module slot -> renderer texture wrapping its image (0 = not yet)
    u32 modver[VG_TEX_OWN];         // the slot's version when wrapped
    VkDescriptorSetLayout set3_layout, set4_layout;
    VkDescriptorPool set4_pool;
    VkDescriptorSet set4[FRAMES_IN_FLIGHT];
    GpuBuffer wind_ubo[FRAMES_IN_FLIGHT];
    VkImageView set4_views[FRAMES_IN_FLIGHT][VG_WIND_RING];   // the tile images the slot's set samples (NULL: white)
    VkPipelineLayout layout;
    VkPipeline pipe[CP_COUNT];
    VkDescriptorSet set_default, set_disp;
    VkImageView disp_view;
    GpuBuffer inst[FRAMES_IN_FLIGHT];
    u32 inst_cap[FRAMES_IN_FLIGHT], inst_used;
    ClInst *tmp;                    // gathered instances of one view
    u32 *tmp_key;
    u32 tmp_n, tmp_cap;
    u32 *count;                     // per key
    u32 count_cap;
    ClView view[CL_VIEWS];
    const VgScene *scene;
    float cell_fade[1 << 16], cell_shadow[1 << 16];   // placement cells: largest foliage fade end / shadow distance (0 = unlimited)
    u8 cell_scc[1 << 16];           // placement cells: the shadow caster classes of their meshes (bit per SCC_*)
    struct ClChunk { float min[3], max[3], fade, shadow; u32 scc; } *chunk;   // per placement chunk: bounds, the max of its cells' limits, their classes
    u32 chunk_cap;
    float disp[4];
    u32 ncasc;
    float density;
    float cpu_ms;
    u32 draws, tris, tris_shadow, local_instances;
    u64 gpu_bytes, cpu_bytes;      // LOD buffers + impostor textures / CPU copies of exported LODs (classic_stats)
    Cvar *impostor_px;
    bool lod_new;                   // this frame's main view / cascades use docs/LOD.md
    u32 lod_n[5], lod_fading;       // main view parts per LOD 0..3 / impostor, parts in a fade (render_classic_lod_counts)
    bool counted;                   // lod_n holds a culled main view (render_vgeo_counts)
    float eff_lod_px, eff_shadow_px, eff_imp_px, eff_small_px;   // this frame's effective thresholds (classic_stats)
    SccTable scc;                   // caster cascade limits (assets/render/shadow_casters.json)
    int scc_mode;                   // this frame's r_shadow_caster_limit (0: off, or PF-0013 disabled)
    SccView scc_view[4];            // this frame's per cascade (classic_stats)
} C;

// ------------------------------------------------------------------ pipelines

static VkPipeline cl_pipeline(const char *vert, const char *frag, bool impostor, bool shadow, bool two_sided, VkCompareOp cmp, bool depth_write, int colors) {
    VkShaderModule vs = vkp_load_shader(vert), fs = frag ? vkp_load_shader(frag) : VK_NULL_HANDLE;
    if (!vs || (frag && !fs)) {
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
    // binding 0: mesh vertices (not for impostors), binding 1: the instance stream
    VkVertexInputBindingDescription bind[2] = { { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX }, { 1, sizeof(ClInst), VK_VERTEX_INPUT_RATE_INSTANCE } };
    VkVertexInputAttributeDescription at[7];
    u32 na = 0;
    if (!impostor) {
        at[na++] = (VkVertexInputAttributeDescription){ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
        at[na++] = (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) };
        at[na++] = (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv) };
        at[na++] = (VkVertexInputAttributeDescription){ 3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, color) };
    }
    at[na++] = (VkVertexInputAttributeDescription){ 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ClInst, pos) };
    at[na++] = (VkVertexInputAttributeDescription){ 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ClInst, rot) };
    at[na++] = (VkVertexInputAttributeDescription){ 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ClInst, misc) };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = impostor ? 1 : 2;
    vi.pVertexBindingDescriptions = impostor ? &bind[1] : bind;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = at;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = (two_sided || shadow || impostor) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = shadow;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = depth_write;
    ds.depthCompareOp = cmp;
    VkPipelineColorBlendAttachmentState cba[3];
    memset(cba, 0, sizeof(cba));
    for (int i = 0; i < 3; i++) cba[i].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = (u32)colors;
    cb.pAttachments = cba;
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = shadow ? 3 : 2;
    dy.pDynamicStates = dyn;
    VkFormat gb[3] = { GB_NORMAL_FORMAT, GB_MATERIAL_FORMAT, GB_VELOCITY_FORMAT }, hdr = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = (u32)colors;
    rci.pColorAttachmentFormats = colors == 3 ? gb : &hdr;
    rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &rci;
    gp.stageCount = frag ? 2 : 1;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = C.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(R->device, R->pipeline_cache, 1, &gp, NULL, &p) != VK_SUCCESS) log_error("classic: pipeline %s / %s failed", vert, frag ? frag : "-");
    vkDestroyShaderModule(R->device, vs, NULL);
    if (fs) vkDestroyShaderModule(R->device, fs, NULL);
    return p;
}

static void cl_write_set(VkDescriptorSet set, VkImageView v0, VkImageLayout l0, VkImageView v1, VkSampler s0) {
    VkDescriptorImageInfo im[2] = { { s0, v0, l0 }, { R->sampler_linear, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[i].dstSet = set;
        w[i].dstBinding = (u32)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &im[i];
    }
    vkUpdateDescriptorSets(R->device, 2, w, 0, NULL);
}

static VkDescriptorSet cl_alloc_set(VkImageView v0, VkImageLayout l0, VkImageView v1, VkSampler s0) {
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = R->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &C.set3_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(R->device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    cl_write_set(set, v0, l0, v1, s0);
    return set;
}

void vkc_init(void) {
    C.impostor_px = cvar_reg_float("r_classic_impostor_px", 64.0f, 0.0f, 1024.0f, CVAR_LOCKED,
                                   "classic renderer: trees with an impostor atlas draw it below this projected size (pixels across, 0 = never)");
    {   // sun shadow caster cascade limits (shadow_casters.h)
        char path[512], err[160] = "";
        platform_data_path("assets/render/shadow_casters.json", path, sizeof(path));
        size_t n = 0;
        char *txt = platform_read_file(path, &n);
        if (!txt) {
            scc_defaults(&C.scc);
            log_warn("classic: %s missing: built-in caster cascade limits", path);
        } else {
            if (!scc_parse(&C.scc, txt, err, sizeof(err))) log_error("classic: %s: %s (built-in caster cascade limits)", path, err);
            free(txt);
        }
    }
    VkDescriptorSetLayoutBinding b[2];
    memset(b, 0, sizeof(b));
    for (u32 i = 0; i < 2; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &C.set3_layout));
    // set 4: the frame's wind (uniform buffer) and the wind tile ring, one set per frame slot
    VkDescriptorSetLayoutBinding b4[2];
    memset(b4, 0, sizeof(b4));
    b4[0].binding = 0;
    b4[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b4[0].descriptorCount = 1;
    b4[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b4[1].binding = 1;
    b4[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b4[1].descriptorCount = VG_WIND_RING;
    b4[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    li.bindingCount = 2;
    li.pBindings = b4;
    VK_CHECK(vkCreateDescriptorSetLayout(R->device, &li, NULL, &C.set4_layout));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES_IN_FLIGHT }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES_IN_FLIGHT * VG_WIND_RING } };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = FRAMES_IN_FLIGHT;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(R->device, &pci, NULL, &C.set4_pool));
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; f++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = C.set4_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &C.set4_layout;
        VK_CHECK(vkAllocateDescriptorSets(R->device, &ai, &C.set4[f]));
        C.wind_ubo[f] = vkr_buffer_create(sizeof(ClWindGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        memset(C.wind_ubo[f].mapped, 0, sizeof(ClWindGpu));
        VkDescriptorBufferInfo bi = { C.wind_ubo[f].buffer, 0, sizeof(ClWindGpu) };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = C.set4[f];
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(R->device, 1, &w, 0, NULL);   // the ring (binding 1) at the slot's first frame
    }
    // set 5: the glTF maps of a material (the legacy lit layout's set 3, docs/PBR.md)
    VkDescriptorSetLayout sets[6] = { R->frame_layout, R->tex_layout, R->lit_post_layout, C.set3_layout, C.set4_layout, R->mat_layout };
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 6;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(R->device, &pli, NULL, &C.layout));
    VkCompareOp le = VK_COMPARE_OP_LESS_OR_EQUAL, eq = VK_COMPARE_OP_EQUAL;
    C.pipe[CP_GB] = cl_pipeline("classic.vert", "classic_gbuffer.frag", false, false, false, le, true, 3);
    C.pipe[CP_GB_2S] = cl_pipeline("classic.vert", "classic_gbuffer.frag", false, false, true, le, true, 3);
    C.pipe[CP_GB_CLIP] = cl_pipeline("classic.vert", "classic_gbuffer_clip.frag", false, false, false, le, true, 3);
    C.pipe[CP_GB_CLIP_2S] = cl_pipeline("classic.vert", "classic_gbuffer_clip.frag", false, false, true, le, true, 3);
    C.pipe[CP_LIT] = cl_pipeline("classic.vert", "classic_lit.frag", false, false, false, eq, false, 1);
    C.pipe[CP_LIT_2S] = cl_pipeline("classic.vert", "classic_lit.frag", false, false, true, eq, false, 1);
    C.pipe[CP_LIT_CLIP] = cl_pipeline("classic.vert", "classic_lit_clip.frag", false, false, false, eq, false, 1);
    C.pipe[CP_LIT_CLIP_2S] = cl_pipeline("classic.vert", "classic_lit_clip.frag", false, false, true, eq, false, 1);
    C.pipe[CP_SHADOW] = cl_pipeline("classic_shadow.vert", NULL, false, true, true, le, true, 0);
    C.pipe[CP_SHADOW_MASKED] = cl_pipeline("classic_shadow.vert", "classic_shadow_masked.frag", false, true, true, le, true, 0);
    C.pipe[CP_IMP_GB] = cl_pipeline("classic_impostor.vert", "classic_impostor_gbuffer.frag", true, false, true, le, true, 3);
    C.pipe[CP_IMP_LIT] = cl_pipeline("classic_impostor.vert", "classic_impostor_lit.frag", true, false, true, eq, false, 1);
    // scope picture-in-picture: lit forward with depth write (no prepass in the PiP)
    C.pipe[CP_PIP] = cl_pipeline("classic.vert", "classic_lit.frag", false, false, false, le, true, 1);
    C.pipe[CP_PIP_2S] = cl_pipeline("classic.vert", "classic_lit.frag", false, false, true, le, true, 1);
    C.pipe[CP_PIP_CLIP] = cl_pipeline("classic.vert", "classic_lit_clip.frag", false, false, false, le, true, 1);
    C.pipe[CP_PIP_CLIP_2S] = cl_pipeline("classic.vert", "classic_lit_clip.frag", false, false, true, le, true, 1);
    C.pipe[CP_PIP_IMP] = cl_pipeline("classic_impostor.vert", "classic_impostor_lit.frag", true, false, true, le, true, 1);
    for (int i = 0; i < CP_COUNT; i++)
        if (!C.pipe[i]) { log_error("classic: renderer unavailable (pipeline %d)", i); return; }
    C.ok = true;   // the default set 3 (white) is written at the first frame (the white texture comes later)
    log_info("classic: instanced renderer ready");
}

void vkc_shutdown(void) {
    if (!C.layout) return;
    vkDeviceWaitIdle(R->device);
    vkam_shutdown();
    for (int i = 0; i < CP_COUNT; i++) if (C.pipe[i]) vkDestroyPipeline(R->device, C.pipe[i], NULL);
    vkDestroyPipelineLayout(R->device, C.layout, NULL);
    vkDestroyDescriptorSetLayout(R->device, C.set3_layout, NULL);
    if (C.set4_layout) vkDestroyDescriptorSetLayout(R->device, C.set4_layout, NULL);
    if (C.set4_pool) vkDestroyDescriptorPool(R->device, C.set4_pool, NULL);
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; f++) if (C.wind_ubo[f].buffer) vkr_buffer_destroy_now(&C.wind_ubo[f]);
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; f++) if (C.inst[f].buffer) vkr_buffer_destroy_now(&C.inst[f]);
    for (u32 k = 0; k < VG_TEX_OWN; k++) if (C.modtex[k]) vkr_texture_unwrap(C.modtex[k]);   // the images are the module's
    for (u32 i = 0; i < C.mesh_cap; i++)
        if (C.mesh[i].own) { free((void *)C.mesh[i].verts); free((void *)C.mesh[i].indices); }
    for (int v = 0; v < CL_VIEWS; v++) free(C.view[v].batches);
    free(C.mesh);
    free(C.tmp);
    free(C.tmp_key);
    free(C.count);
    free(T.t);
    free(C.chunk);
    memset(&T, 0, sizeof(T));
    memset(&C, 0, sizeof(C));
}

// ------------------------------------------------------------------ meshes

static ClMesh *cl_mesh_slot(u32 mesh) {
    if (mesh >= C.mesh_cap) {
        u32 cap = C.mesh_cap ? C.mesh_cap : 16;
        while (cap <= mesh) cap *= 2;
        C.mesh = (ClMesh *)realloc(C.mesh, sizeof(ClMesh) * cap);
        memset(C.mesh + C.mesh_cap, 0, sizeof(ClMesh) * (cap - C.mesh_cap));
        C.mesh_cap = cap;
    }
    return &C.mesh[mesh];
}

// One LOD from the DAG: the clusters of the cut at the LOD's error, decoded (positions, normals, uv,
// vertex colours) and their triangles grouped per material.
static void cl_build_cut(ClLod *L, const VgBuilt *b, float error) {
    u32 n = vgeo_cut(b, error, NULL, 0);
    if (!n) return;
    VgCutRef *refs = (VgCutRef *)malloc(sizeof(VgCutRef) * n);
    vgeo_cut(b, error, refs, n);
    VgDecoded *d = (VgDecoded *)malloc(sizeof(VgDecoded));
    u32 nm = DG_CLAMP(b->hdr.material_count, 1u, (u32)CL_MATS);
    u32 vcap = n * VGEO_MAX_CLUSTER_VERTS, vn = 0;
    Vertex *v = (Vertex *)malloc(sizeof(Vertex) * vcap);
    u32 *idx[CL_MATS], icount[CL_MATS];
    for (u32 m = 0; m < nm; m++) { idx[m] = (u32 *)malloc(sizeof(u32) * 3 * n * VGEO_MAX_CLUSTER_TRIS); icount[m] = 0; }
    for (u32 i = 0; i < n; i++) {
        vgeo_decode_cluster(b, refs[i].page, refs[i].index, d);
        u32 m = DG_MIN(d->material, nm - 1), base = vn;
        for (u32 k = 0; k < d->vert_count; k++, vn++) {
            v[vn].pos = v3(d->pos[k][0], d->pos[k][1], d->pos[k][2]);
            v[vn].normal = v3(d->normal[k][0], d->normal[k][1], d->normal[k][2]);
            v[vn].uv = v2(d->uv[k][0], d->uv[k][1]);
            v[vn].color = d->color[k];
        }
        for (u32 t = 0; t < d->tri_count; t++)
            for (int c = 0; c < 3; c++) idx[m][icount[m]++] = base + d->tri[t][c];
    }
    u32 total = 0;
    for (u32 m = 0; m < nm; m++) total += icount[m];
    u32 *all = (u32 *)malloc(sizeof(u32) * (total + 1));
    L->nsec = 0;
    u32 off = 0;
    for (u32 m = 0; m < nm; m++) {
        if (!icount[m]) continue;
        memcpy(all + off, idx[m], sizeof(u32) * icount[m]);
        L->sec[L->nsec++] = (ClSection){ off, icount[m], m };
        off += icount[m];
    }
    L->gpu = render_mesh_create(v, vn, all, total);
    if (L->gpu) C.gpu_bytes += sizeof(Vertex) * (u64)vn + 4ull * total;
    L->tris = total / 3;
    L->ready_frame = R->frame_index + 2;
    for (u32 m = 0; m < nm; m++) free(idx[m]);
    free(all);
    free(v);
    free(d);
    free(refs);
}

// An exported discrete LOD (uploaded on first use).
static void cl_build_exported(ClMesh *cm, u32 k) {
    ClLod *L = &cm->lods[k];
    const VgDiscreteLod *l = &cm->src[k];
    if (!l->index_count) return;
    Vertex *v = (Vertex *)malloc(sizeof(Vertex) * (l->vertex_count + 1));
    for (u32 i = 0; i < l->vertex_count; i++) {
        const VgLodVertex *s = &cm->verts[l->first_vertex + i];
        v[i].pos = v3(s->pos[0], s->pos[1], s->pos[2]);
        v[i].normal = v3(s->normal[0], s->normal[1], s->normal[2]);
        v[i].uv = v2(s->uv[0], s->uv[1]);
        v[i].color = 0xFFFFFFFFu;
    }
    L->gpu = render_mesh_create(v, l->vertex_count, cm->indices + l->first_index, l->index_count);
    if (L->gpu) C.gpu_bytes += sizeof(Vertex) * (u64)l->vertex_count + 4ull * l->index_count;
    L->sec[0] = (ClSection){ 0, l->index_count, 0 };
    L->nsec = 1;
    L->tris = l->index_count / 3;
    L->ready_frame = R->frame_index + 2;
    free(v);
}

static TextureId cl_texture_mips(const u8 *rgba, u32 size, u32 keep_levels, bool srgb) {
    u32 levels;
    size_t bytes = vg_tex_mip_bytes(size, size, &levels);
    u8 *mips = (u8 *)malloc(bytes);
    vg_tex_build_mips(rgba, size, size, mips);
    u32 n = keep_levels ? DG_MIN(keep_levels, levels) : levels;
    size_t ls[16];
    for (u32 l = 0; l < n && l < 16; l++) {
        u32 s = DG_MAX(size >> l, 1u);
        ls[l] = (size_t)s * s * 4;
        C.gpu_bytes += ls[l];
    }
    TextureId id = render_texture_create_raw((int)size, (int)size, (int)DG_MIN(n, 16u), srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, mips, ls);
    free(mips);
    return id;
}

void vkc_set_textures(const VgTextures *t) { C.vtex = t; }
// The renderer recreated its material sampler (r_aniso / r_texture_quality) and rewrote every texture
// set (wrapped ones with its clamp sampler): re-apply the slots' samplers, and rewrite this module's own
// sets, which hold the material sampler too (the default / displacement sets, the impostor atlases;
// the local light shadow tiles draw through them too). The caller waited for the device.
void vkc_samplers_changed(void) {
    memset(C.modver, 0xFF, sizeof(C.modver));
    memset(C.set4_views, 0, sizeof(C.set4_views));   // the wind ring's sets hold the module's repeat sampler
    VkImageView white = R->textures[R->white_tex].view;
    if (C.set_default) cl_write_set(C.set_default, white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, white, R->sampler_linear);
    if (C.set_disp && C.disp_view) cl_write_set(C.set_disp, C.disp_view, VK_IMAGE_LAYOUT_GENERAL, white, R->sampler_linear);
    for (u32 i = 0; i < C.mesh_cap; i++) {
        ClMesh *cm = &C.mesh[i];
        if (cm->imp_set)
            cl_write_set(cm->imp_set, R->textures[cm->imp_normal].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, R->textures[cm->imp_albedo].view, R->sampler_linear);
    }
}

// A vgeo module texture slot as a renderer texture: a wrap of the slot's own image and view, sampled
// with the slot's sampler (repeat / clamp), rewritten when the slot changes. No copy.
static TextureId module_texture(u32 index) {
    if (!C.vtex || index < VG_TEX_GAME || index >= VG_TEX_COUNT) return 0;
    u32 k = index - VG_TEX_GAME;
    const VgOwnTex *o = &C.vtex->own[k];
    const VgTexEntry *e = &C.vtex->e[index];
    if (!o->used || !e->view) return 0;
    if (C.modtex[k] && C.modver[k] == C.vtex->ver[index]) return C.modtex[k];
    if (!C.modtex[k]) C.modtex[k] = vkr_texture_wrap(o->image, e->view, o->w, o->h, VK_FORMAT_R8G8B8A8_SRGB);
    else vkr_texture_rewrap(C.modtex[k], o->image, e->view, o->w, o->h);
    if (!C.modtex[k]) return 0;
    vkr_write_tex_set(R->textures[C.modtex[k]].set, e->view, e->sampler ? e->sampler : R->sampler_linear);
    R->textures[C.modtex[k]].mips = o->mips;
    C.modver[k] = C.vtex->ver[index];
    return C.modtex[k];
}

void vkc_register_mesh(u32 mesh, const VgBuilt *b, bool persistent) {
    if (!C.ok) return;
    ClMesh *cm = cl_mesh_slot(mesh);
    if (cm->own) { free((void *)cm->verts); free((void *)cm->indices); }
    memset(cm, 0, sizeof(*cm));
    cm->lod_count = DG_MIN(b->hdr.lod_count, (u32)VGEO_MAX_LODS);
    if (!cm->lod_count || (b->hdr.flags & VGF_SKINNED)) return;
    cm->used = true;
    for (u32 k = 0; k < cm->lod_count; k++) cm->lod_error[k] = b->lods[k].error;
    bool decode = ((b->hdr.flags & VGF_HAS_COLOR) || b->hdr.material_count > 1) && b->page_data;
    if (decode) {
        for (u32 k = 0; k < cm->lod_count; k++) cl_build_cut(&cm->lods[k], b, b->lods[k].error);
    } else {
        memcpy(cm->src, b->lods, sizeof(cm->src));
        u32 nv = 0, ni = 0;
        for (u32 k = 0; k < cm->lod_count; k++) {
            nv = DG_MAX(nv, b->lods[k].first_vertex + b->lods[k].vertex_count);
            ni = DG_MAX(ni, b->lods[k].first_index + b->lods[k].index_count);
        }
        cm->own = !persistent;
        if (persistent) {
            cm->verts = b->lod_vertices;
            cm->indices = b->lod_indices;
        } else {
            VgLodVertex *v = (VgLodVertex *)malloc(sizeof(VgLodVertex) * (nv + 1));
            u32 *ix = (u32 *)malloc(sizeof(u32) * (ni + 1));
            memcpy(v, b->lod_vertices, sizeof(VgLodVertex) * nv);
            memcpy(ix, b->lod_indices, sizeof(u32) * ni);
            cm->verts = v;
            cm->indices = ix;
            C.cpu_bytes += sizeof(VgLodVertex) * (u64)nv + 4ull * ni;
        }
    }
    cm->coarse_tris = decode ? cm->lods[cm->lod_count - 1].tris : b->lods[cm->lod_count - 1].tris;
    const u8 *alb, *nrm;
    const VgImpostorHeader *h = vgeo_impostor(b, &alb, &nrm);
    if (h) {
        u32 keep = 1, levels;
        vg_tex_mip_bytes(h->size, h->size, &levels);
        while (keep < levels && (h->size / h->grid) >> keep >= 4) keep++;   // coarser mips would mix frames
        cm->imp_albedo = cl_texture_mips(alb, h->size, keep, true);
        cm->imp_normal = cl_texture_mips(nrm, h->size, keep, false);
        if (cm->imp_albedo && cm->imp_normal)
            cm->imp_set = cl_alloc_set(R->textures[cm->imp_normal].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, R->textures[cm->imp_albedo].view, R->sampler_linear);
        cm->imp_grid = h->grid;
        memcpy(cm->imp_center, h->center, 12);
        cm->imp_radius = h->radius;
    }
    log_info("classic: mesh %u: %u LODs (%u .. %u tris, %s), impostor %s", mesh, cm->lod_count, decode ? cm->lods[0].tris : b->lods[0].tris, cm->coarse_tris,
             decode ? "decoded DAG cuts" : "exported LODs", h ? (cm->coarse_tris >= CL_IMPOSTOR_TRIS ? "r_classic_impostor_px" : "r_vgeo_impostor_px") : "none");
}

// Placement chunk loaded (after its commit): per cell, the largest fade end / shadow distance of
// its instances' meshes (0 = one of them is unlimited), for whole-cell distance culling.
void vkc_chunk_loaded(const VgScene *s, u32 handle) {
    if (handle >= s->pl.chunk_count) return;
    const VgPlacementChunk *ch = &s->pl.chunks[handle];
    for (u32 k = 0; k < ch->cell_count; k++) {
        u32 ci = ch->cells[k];
        if (ci >= (1u << 16)) continue;
        const VgCell *c = &s->pl.cells[ci];
        float fade = 0, shadow = 0;
        bool unlimited_fade = false, unlimited_shadow = false;
        u32 scc = 0;
        for (u32 i = c->first; i < c->first + c->count; i++) {
            const VgMesh *m = &s->meshes[s->pl.inst[i].mesh];
            u32 sc = VGMF_SHADOW_CLASS(m->fflags);
            scc |= 1u << (sc < SCC_COUNT ? sc : SCC_NONE);
            if (!(m->fflags & VGMF_FOLIAGE) || m->fade_end <= m->fade_start) unlimited_fade = true;
            else fade = fmaxf(fade, m->fade_end);
            if (m->shadow_max_dist <= 0) unlimited_shadow = true;
            else shadow = fmaxf(shadow, m->shadow_max_dist);
        }
        C.cell_fade[ci] = unlimited_fade ? 0 : fade;
        C.cell_shadow[ci] = unlimited_shadow ? 0 : shadow;
        C.cell_scc[ci] = (u8)scc;
    }
    // the chunk: union of its cells, largest limits (0 if any cell is unlimited)
    if (handle >= C.chunk_cap) {
        u32 cap = C.chunk_cap ? C.chunk_cap : 256;
        while (cap <= handle) cap *= 2;
        C.chunk = (struct ClChunk *)realloc(C.chunk, sizeof(*C.chunk) * cap);
        for (u32 i = C.chunk_cap; i < cap; i++) C.chunk[i] = (struct ClChunk){ { -1e30f, -1e30f, -1e30f }, { 1e30f, 1e30f, 1e30f }, 0, 0, (1u << SCC_COUNT) - 1u };   // not seen: never skipped
        C.chunk_cap = cap;
    }
    struct ClChunk *cc = &C.chunk[handle];
    for (int a = 0; a < 3; a++) cc->min[a] = 1e30f, cc->max[a] = -1e30f;
    cc->fade = cc->shadow = 0;
    cc->scc = 0;
    bool fade_inf = false, shadow_inf = false;
    for (u32 k = 0; k < ch->cell_count; k++) {
        u32 ci = ch->cells[k];
        const VgCell *c = &s->pl.cells[ci];
        for (int a = 0; a < 3; a++) cc->min[a] = fminf(cc->min[a], c->min[a]), cc->max[a] = fmaxf(cc->max[a], c->max[a]);
        float f = ci < (1u << 16) ? C.cell_fade[ci] : 0, sh = ci < (1u << 16) ? C.cell_shadow[ci] : 0;
        fade_inf |= f <= 0;
        shadow_inf |= sh <= 0;
        cc->scc |= ci < (1u << 16) ? C.cell_scc[ci] : (1u << SCC_COUNT) - 1u;
        cc->fade = fmaxf(cc->fade, f);
        cc->shadow = fmaxf(cc->shadow, sh);
    }
    if (fade_inf) cc->fade = 0;
    if (shadow_inf) cc->shadow = 0;
}

// ------------------------------------------------------------------ culling

static void planes_of(mat4 m, vec4 out[6]) {
    float *a = m.m;
    vec4 r0 = v4(a[0], a[4], a[8], a[12]), r1 = v4(a[1], a[5], a[9], a[13]), r2 = v4(a[2], a[6], a[10], a[14]), r3 = v4(a[3], a[7], a[11], a[15]);
    out[0] = v4(r3.x + r0.x, r3.y + r0.y, r3.z + r0.z, r3.w + r0.w);
    out[1] = v4(r3.x - r0.x, r3.y - r0.y, r3.z - r0.z, r3.w - r0.w);
    out[2] = v4(r3.x + r1.x, r3.y + r1.y, r3.z + r1.z, r3.w + r1.w);
    out[3] = v4(r3.x - r1.x, r3.y - r1.y, r3.z - r1.z, r3.w - r1.w);
    out[4] = r2;
    out[5] = v4(r3.x - r2.x, r3.y - r2.y, r3.z - r2.z, r3.w - r2.w);
    for (int i = 0; i < 6; i++) {
        float l = sqrtf(out[i].x * out[i].x + out[i].y * out[i].y + out[i].z * out[i].z);
        out[i] = v4(out[i].x / l, out[i].y / l, out[i].z / l, out[i].w / l);
    }
}

static bool sphere_in(const vec4 *pl, int n, vec3 c, float r) {
    for (int k = 0; k < n; k++)
        if (pl[k].x * c.x + pl[k].y * c.y + pl[k].z * c.z + pl[k].w < -r) return false;
    return true;
}

static bool box_in(const vec4 *pl, int n, const float mn[3], const float mx[3]) {
    for (int k = 0; k < n; k++) {
        float x = pl[k].x >= 0 ? mx[0] : mn[0], y = pl[k].y >= 0 ? mx[1] : mn[1], z = pl[k].z >= 0 ? mx[2] : mn[2];
        if (pl[k].x * x + pl[k].y * y + pl[k].z * z + pl[k].w < 0) return false;
    }
    return true;
}

static float box_dist(const float mn[3], const float mx[3], vec3 p) {
    float dx = fmaxf(fmaxf(mn[0] - p.x, p.x - mx[0]), 0), dy = fmaxf(fmaxf(mn[1] - p.y, p.y - mx[1]), 0), dz = fmaxf(fmaxf(mn[2] - p.z, p.z - mx[2]), 0);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void tmp_reserve(u32 n) {
    if (n <= C.tmp_cap) return;
    u32 cap = C.tmp_cap ? C.tmp_cap : 4096;
    while (cap < n) cap *= 2;
    C.tmp = (ClInst *)realloc(C.tmp, sizeof(ClInst) * cap);
    C.tmp_key = (u32 *)realloc(C.tmp_key, sizeof(u32) * cap);
    C.tmp_cap = cap;
}

// What a view culls with: planes, the eye (fade / density / shadow distance are from the camera),
// LOD scale (pixels per metre of error at distance 1, or per metre flat for ortho views).
typedef struct ClCull {
    vec4 planes[6];
    vec3 eye, lod_eye;              // lod_eye: LOD distance origin (the light for local shadows)
    float px_scale, lod_px, impostor_px, small_impostor_px, density, znear;
    bool ortho, shadow, era2010;
    vec4 sphere;                    // local light shadows: casters must touch it (w > 0)
    // docs/LOD.md (legacy: the cuts from before): bands (octaves), foliage fade range / band scales,
    // instance fade width, shrink, shadow distance scale and fade fraction, whole-cell limit scales
    bool legacy;
    float band, imp_band, fol_k, fol_band_k, fol_instance, fol_shrink, shadow_k, shadow_band, fade_lim_k, shadow_lim_k;
    u32 layers;                     // shadow views: the light's culling mask (0 = every layer)
    vec4 rcv[CSM_RCV_MAX];          // sun cascades: the receiver volume (render.c, PF-0006), nrcv planes (0: none)
    int nrcv;
    bool scc_on;                    // sun cascades: the caster classes' cascade limits (PF-0013)
    SccView scc;
} ClCull;
static ClCull g_main_cull;          // the last frame's main view (classic_fade_probe)
static bool g_main_cull_ok;

static float clamp01f(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// Foliage density / distance fade (docs/LOD.md): the material's fade range and band scaled, a gentler
// (smoothstep) falloff, each instance fading over fol_instance of it (lod_foliage_alpha: opaque before
// the fade starts).
static float cl_foliage_alpha(const ClCull *v, const VgMesh *m, u32 fp, float dist) {
    if (!(m->fflags & VGMF_FOLIAGE)) return 1.0f;
    float u = 1.0f;
    if (m->fade_end > m->fade_start) {
        float s0 = m->fade_start * v->fol_k, s1 = s0 + (m->fade_end - m->fade_start) * v->fol_band_k;
        u = clamp01f((s1 - dist) / fmaxf(s1 - s0, 1e-3f));
        u = u * u * (3.0f - 2.0f * u);
    }
    return lod_foliage_alpha(v->density, u, ((float)VG_INST_HASH(fp) + 0.5f) / 256.0f, v->fol_instance);
}

// One part of an instance: debug = LOD (CL_IMPOSTOR -> 4) + 8 x role (1 incoming / coarser, 2 outgoing
// / finer, 3 alpha fade only).
static void emit_part(ClInst *out, u32 *key, u32 *n, u32 mesh, u32 lod, const float p[3], const float q[4], float sc, float phase, float alpha, float iv) {
    ClInst *o = &out[*n];
    o->pos[0] = p[0], o->pos[1] = p[1], o->pos[2] = p[2], o->pos[3] = sc;
    memcpy(o->rot, q, 16);
    o->misc[0] = phase;
    o->misc[1] = alpha;
    o->misc[2] = iv;
    float role = iv != 0.0f ? (fmodf(iv, 512.0f) == 0.0f ? 1.0f : 2.0f) : (alpha < 1.0f ? 3.0f : 0.0f);
    o->misc[3] = (float)(lod == CL_IMPOSTOR ? 4u : lod) + 8.0f * role;
    bool fading = iv != 0.0f || alpha < 1.0f;
    key[(*n)++] = mesh << CL_KEY_SHIFT | (fading ? CL_KEY_FADE : 0u) | lod;
}

// One instance against a view: appends to out / key at *n when kept (thread-safe: reads only).
static void cull_instance(const ClCull *v, const VgScene *s, const VgInstance *in, ClInst *out, u32 *key, u32 *n) {
    u32 iflags = in->flags_phase >> 16;
    if (iflags & VGI_SKINNED) return;
    // Unity-style Cast Shadows Off / Shadows Only, the light's culling mask (docs/SHADOWS_UNITY_MODEL.md)
    if (v->shadow ? (iflags & VGI_NO_CAST) != 0 : (iflags & VGI_SHADOWS_ONLY) != 0) return;
    if (v->shadow && v->layers && !((v->layers >> VGI_LAYER(iflags)) & 1u)) return;
    if (in->mesh >= C.mesh_cap || !C.mesh[in->mesh].used || in->mesh >= s->mesh_count) return;
    const ClMesh *cm = &C.mesh[in->mesh];
    const VgMesh *m = &s->meshes[in->mesh];
    float p[3], q[4], sc;
    vgeo_instance_decode(in, p, q, &sc);
    quat qq = { q[0], q[1], q[2], q[3] };
    vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(m->center[0] * sc, m->center[1] * sc, m->center[2] * sc)));
    float r = m->radius * sc + m->max_wpo * fmaxf(sc, 1.0f);   // vg_wpo_margin
    if (v->sphere.w > 0) {
        float rr = r + v->sphere.w;
        if (v3_len2(v3_sub(c, v3(v->sphere.x, v->sphere.y, v->sphere.z))) > rr * rr) return;
    }
    if (!sphere_in(v->planes, 6, c, r)) return;
    if (v->nrcv && !sphere_in(v->rcv, v->nrcv, c, r)) return;
    float dx = p[0] - v->eye.x, dy = p[1] - v->eye.y, dz = p[2] - v->eye.z, dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float casc_a = 1.0f;   // PF-0013: the caster class's cascade limit (0: not in this cascade / beyond its end)
    if (v->scc_on) {
        u32 sc = VGMF_SHADOW_CLASS(m->fflags);
        casc_a = scc_alpha(&v->scc, sc < SCC_COUNT ? (int)sc : SCC_NONE, dist);
        if (casc_a <= 0.0f) return;
    }
    if (!v->legacy) {   // docs/LOD.md
        float alpha = casc_a, shrink = 1.0f;
        if (v->shadow && m->shadow_max_dist > 0) {   // the shadow fades out over the end of its (scaled) distance
            float sd = m->shadow_max_dist * v->shadow_k;
            if (dist > sd) return;
            alpha *= v->shadow_band > 0 ? clamp01f((sd - dist) / (sd * v->shadow_band)) : 1.0f;
        }
        if (m->fflags & VGMF_FOLIAGE) {   // density / distance fade: dither + the blades shrink
            float fa = cl_foliage_alpha(v, m, in->flags_phase, dist);
            if (fa <= 0) return;
            alpha *= fa;
            shrink = v->fol_shrink + (1.0f - v->fol_shrink) * fa;
        }
        if (alpha <= 0) return;
        float dl = fmaxf(v3_dist(c, v->lod_eye), v->znear), dcl = fmaxf(dl - r, v->znear);
        float imp_px = cm->coarse_tris >= CL_IMPOSTOR_TRIS ? v->impostor_px : v->small_impostor_px;
        u32 ks = m->impostor_grid >> 16;
        if (ks) imp_px *= (float)ks / 16.0f;
        float iw = 0;   // impostor share: fades in over imp_band octaves past the switch size
        if (!v->shadow && cm->imp_set && imp_px > 0) {
            float size = 2.0f * m->radius * sc * v->px_scale / dl;
            if (size < imp_px) iw = lod_band_weight(log2f(imp_px / fmaxf(size, 1e-6f)), v->imp_band);
        }
        float k = v->ortho ? sc * v->px_scale : sc * v->px_scale / dcl, cw = 0;
        int first = v->era2010 && cm->lod_count > 1 ? 1 : 0;
        u32 lod = iw >= 1.0f ? 0u : (u32)lod_pick_band(cm->lod_error, (int)cm->lod_count, first, k, v->lod_px, v->band, &cw);
        float phase = (float)VG_INST_PHASE(in->flags_phase) / 256.0f, sd = sc * shrink;
        // parts: impostor [0, iw), coarser LOD [iw, mid), finer LOD [mid, 1)
        float mid = iw + (1.0f - iw) * cw;
        if (iw > 0) emit_part(out, key, n, in->mesh, CL_IMPOSTOR, p, q, sd, phase, alpha, iw < 1.0f ? lod_iv_pack(0, iw) : 0.0f);
        if (iw < 1.0f) {
            if (cw > 0) emit_part(out, key, n, in->mesh, lod + 1, p, q, sd, phase, alpha, cw < 1.0f || iw > 0 ? lod_iv_pack(iw, mid) : 0.0f);
            if (mid < 1.0f) emit_part(out, key, n, in->mesh, lod, p, q, sd, phase, alpha, mid > 0 ? lod_iv_pack(mid, 1.0f) : 0.0f);
        }
        return;
    }
    if (v->shadow && m->shadow_max_dist > 0 && dist > m->shadow_max_dist) return;
    float alpha = (m->fflags & VGMF_FOLIAGE) ? vgeo_foliage_alpha(m, in->flags_phase, dist, v->density) : 1.0f;
    if (alpha <= 0) return;
    u32 lod;
    float dc = fmaxf(v3_dist(c, v->lod_eye) - r, v->znear);
    // Impostors: trees (coarsest LOD >= CL_IMPOSTOR_TRIS) below r_classic_impostor_px, small cover
    // (grass tufts: a few cards) below r_vgeo_impostor_px.
    float imp_px = cm->coarse_tris >= CL_IMPOSTOR_TRIS ? v->impostor_px : v->small_impostor_px;
    u32 ks = m->impostor_grid >> 16;   // the mesh's switch scale (1/16 units, 0 = 1x), as vgeo_use_impostor
    if (ks) imp_px *= (float)ks / 16.0f;
    if (!v->shadow && cm->imp_set && imp_px > 0 && 2.0f * m->radius * sc * v->px_scale / fmaxf(v3_dist(c, v->lod_eye), v->znear) < imp_px) {
        lod = CL_IMPOSTOR;
    } else {
        lod = v->era2010 && cm->lod_count > 1 ? 1 : 0;
        float k = v->ortho ? sc * v->px_scale : sc * v->px_scale / dc;
        while (lod + 1 < cm->lod_count && cm->lod_error[lod + 1] * k <= v->lod_px) lod++;
    }
    ClInst *o = &out[*n];
    o->pos[0] = p[0], o->pos[1] = p[1], o->pos[2] = p[2], o->pos[3] = sc;
    memcpy(o->rot, q, 16);
    o->misc[0] = (float)VG_INST_PHASE(in->flags_phase) / 256.0f;
    o->misc[1] = alpha;
    o->misc[2] = o->misc[3] = 0;
    key[(*n)++] = in->mesh << CL_KEY_SHIFT | lod;
}

// The cells a view keeps become tasks (their instances), culled in parallel on the job system: task
// i writes at most its instance count from offset off, then the outputs are packed in task order
// (the result does not depend on the thread count).
static void task_add(const VgInstance *inst, u32 n) {
    if (!n) return;
    if (T.n >= T.cap) {
        T.cap = T.cap ? T.cap * 2 : 1024;
        T.t = (ClTask *)realloc(T.t, sizeof(ClTask) * T.cap);
    }
    u32 off = T.n ? T.t[T.n - 1].off + T.t[T.n - 1].n * T.parts : 0;
    T.t[T.n++] = (ClTask){ inst, n, off, 0 };
}

static void cull_range(void *ctx, int begin, int end) {
    (void)ctx;
    for (int i = begin; i < end; i++) {
        ClTask *t = &T.t[i];
        t->out = 0;
        for (u32 k = 0; k < t->n; k++) cull_instance(T.v, T.s, &t->inst[k], C.tmp + t->off, C.tmp_key + t->off, &t->out);
    }
}

static void cell_tasks(const ClCull *v, const VgScene *s) {
    T.n = 0;
    T.parts = v->legacy ? 1u : CL_PARTS;
    float lim_k = v->legacy ? 1.0f : (v->shadow ? v->shadow_lim_k : v->fade_lim_k);   // docs/LOD.md: scaled distances
    for (u32 ci = 0; ci < s->cell_count; ci++) {
        const VgCell *cl = &s->cells[ci];
        u32 n = cl->count & VG_CELL_COUNT;
        if (!n || !box_in(v->planes, 6, cl->min, cl->max)) continue;
        if (v->nrcv && !box_in(v->rcv, v->nrcv, cl->min, cl->max)) continue;
        if (v->sphere.w > 0 && box_dist(cl->min, cl->max, v3(v->sphere.x, v->sphere.y, v->sphere.z)) > v->sphere.w) continue;
        if (cl->first < s->instance_count) task_add(&s->instances[cl->first], DG_MIN(n, s->instance_count - cl->first));
    }
    const VgPlacements *pl = &s->pl;
    for (u32 h = 0; h < pl->chunk_count; h++) {
        const VgPlacementChunk *ch = &pl->chunks[h];
        if (!ch->used) continue;
        if (h < C.chunk_cap) {   // whole chunk: distance limit, frustum, light sphere
            const struct ClChunk *cc = &C.chunk[h];
            float lim = (v->shadow ? cc->shadow : cc->fade) * lim_k;
            if (lim > 0 && box_dist(cc->min, cc->max, v->eye) > lim) continue;
            if (v->scc_on) {   // PF-0013: no class of the chunk casts into this cascade, or not that far
                float e = scc_mask_end(&v->scc, cc->scc);
                if (e < 0 || (e > 0 && box_dist(cc->min, cc->max, v->eye) > e)) continue;
            }
            if (!box_in(v->planes, 6, cc->min, cc->max)) continue;
            if (v->nrcv && !box_in(v->rcv, v->nrcv, cc->min, cc->max)) continue;
            if (v->sphere.w > 0 && box_dist(cc->min, cc->max, v3(v->sphere.x, v->sphere.y, v->sphere.z)) > v->sphere.w) continue;
        }
        for (u32 k = 0; k < ch->cell_count; k++) {
            u32 ci = ch->cells[k];
            const VgCell *cl = &pl->cells[ci];
            if (!cl->count) continue;
            float lim = ci < (1u << 16) ? (v->shadow ? C.cell_shadow[ci] : C.cell_fade[ci]) * lim_k : 0;
            if (lim > 0 && box_dist(cl->min, cl->max, v->eye) > lim) continue;
            if (v->scc_on && ci < (1u << 16)) {
                float e = scc_mask_end(&v->scc, C.cell_scc[ci]);
                if (e < 0 || (e > 0 && box_dist(cl->min, cl->max, v->eye) > e)) continue;
            }
            if (!box_in(v->planes, 6, cl->min, cl->max)) continue;
            if (v->nrcv && !box_in(v->rcv, v->nrcv, cl->min, cl->max)) continue;
            if (v->sphere.w > 0 && box_dist(cl->min, cl->max, v3(v->sphere.x, v->sphere.y, v->sphere.z)) > v->sphere.w) continue;
            task_add(&pl->inst[cl->first], cl->count);
        }
    }
}

static void cull_cells(const ClCull *v, const VgScene *s) {
    cell_tasks(v, s);
    if (!T.n) return;
    tmp_reserve(T.t[T.n - 1].off + T.t[T.n - 1].n * T.parts);
    T.v = v;
    T.s = s;
    jobs_parallel_for((int)T.n, 8, cull_range, NULL);
    u32 w = 0;
    for (u32 i = 0; i < T.n; i++) {
        const ClTask *t = &T.t[i];
        if (t->out && t->off != w) {
            memmove(C.tmp + w, C.tmp + t->off, sizeof(ClInst) * t->out);
            memmove(C.tmp_key + w, C.tmp_key + t->off, sizeof(u32) * t->out);
        }
        w += t->out;
    }
    C.tmp_n = w;
}

// docs/perf/FINDINGS.md PF-0009: the views of a frame (scope PiP, main, sun cascades) culled in one
// parallel pass instead of one jobs_parallel_for per view, each waiting for its slowest task before
// the next view starts. Each view keeps its own task list and output buffer; the outputs are packed
// in task order and emitted in the old view order, so every view's instance stream (and the
// instance buffer layout) is the one the per-view passes produce.
typedef struct ClMulti {
    const ClCull *v;
    ClTask *t;
    u32 n, cap, parts, base;
    ClInst *tmp;
    u32 *key, tmp_cap;
} ClMulti;
static struct { ClMulti m[CL_VIEWS]; int n; const VgScene *s; } MV;

static void cull_range_multi(void *ctx, int begin, int end) {
    (void)ctx;
    int vi = 0;
    for (int i = begin; i < end; i++) {
        while (vi + 1 < MV.n && (u32)i >= MV.m[vi + 1].base) vi++;
        ClMulti *m = &MV.m[vi];
        ClTask *t = &m->t[(u32)i - m->base];
        t->out = 0;
        for (u32 k = 0; k < t->n; k++) cull_instance(m->v, MV.s, &t->inst[k], m->tmp + t->off, m->key + t->off, &t->out);
    }
}

static void emit_view(ClView *out);

static void emit_view_depth_order(ClView *out, const mat4 *vp, int row);

// light_vps[i]: view i's instances ordered front to back along row depth_rows[i] of that matrix (NULL:
// plain key order).
static void cull_views(const ClCull *views, ClView *const *outs, const mat4 *const *light_vps, const int *depth_rows, int nv, const VgScene *s) {
    MV.n = nv;
    MV.s = s;
    u32 total = 0;
    for (int i = 0; i < nv; i++) {
        ClMulti *m = &MV.m[i];
        m->v = &views[i];
        // the task list of this view in its own buffer (cell_tasks fills T)
        ClTask *tt = T.t;
        u32 tc = T.cap;
        T.t = m->t, T.cap = m->cap;
        cell_tasks(&views[i], s);
        m->t = T.t, m->cap = T.cap, m->n = T.n, m->parts = T.parts;
        T.t = tt, T.cap = tc, T.n = 0;
        m->base = total;
        total += m->n;
        u32 need = m->n ? m->t[m->n - 1].off + m->t[m->n - 1].n * m->parts : 0;
        if (need > m->tmp_cap) {
            u32 cap = m->tmp_cap ? m->tmp_cap : 4096;
            while (cap < need) cap *= 2;
            m->tmp = (ClInst *)realloc(m->tmp, sizeof(ClInst) * cap);
            m->key = (u32 *)realloc(m->key, sizeof(u32) * cap);
            m->tmp_cap = cap;
        }
    }
    if (total) jobs_parallel_for((int)total, 8, cull_range_multi, NULL);
    for (int i = 0; i < nv; i++) {
        ClMulti *m = &MV.m[i];
        u32 w = 0;
        for (u32 k = 0; k < m->n; k++) {
            const ClTask *t = &m->t[k];
            if (t->out && t->off != w) {
                memmove(m->tmp + w, m->tmp + t->off, sizeof(ClInst) * t->out);
                memmove(m->key + w, m->key + t->off, sizeof(u32) * t->out);
            }
            w += t->out;
        }
        // emit_view reads C.tmp / C.tmp_key / C.tmp_n: this view's buffers for the call
        ClInst *ct = C.tmp;
        u32 *ck = C.tmp_key, cc = C.tmp_cap;
        C.tmp = m->tmp, C.tmp_key = m->key, C.tmp_cap = m->tmp_cap, C.tmp_n = w;
        if (light_vps && light_vps[i]) emit_view_depth_order(outs[i], light_vps[i], depth_rows[i]);
        else emit_view(outs[i]);
        C.tmp = ct, C.tmp_key = ck, C.tmp_cap = cc, C.tmp_n = 0;
    }
}

// Instance buffer space of this frame slot (grows; a replaced buffer stays alive until the frames
// using it are done).
static ClInst *inst_alloc(u32 n, u32 *first) {
    u32 f = R->frame_slot;
    if (C.inst_used + n > C.inst_cap[f]) {
        u32 cap = C.inst_cap[f] ? C.inst_cap[f] : 65536;
        while (cap < C.inst_used + n) cap *= 2;
        GpuBuffer nb = vkr_buffer_create(sizeof(ClInst) * (VkDeviceSize)cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        if (!nb.mapped) return NULL;
        if (C.inst[f].buffer) {
            memcpy(nb.mapped, C.inst[f].mapped, sizeof(ClInst) * C.inst_used);
            vkr_defer_buffer(&C.inst[f]);
        }
        C.inst[f] = nb;
        C.inst_cap[f] = cap;
    }
    *first = C.inst_used;
    C.inst_used += n;
    return (ClInst *)C.inst[f].mapped + *first;
}

// Sorts the gathered instances by key (counting sort) into the instance stream: one batch per key.
static void emit_view(ClView *out) {
    out->batch_count = 0;
    out->instances = C.tmp_n;
    if (!C.tmp_n) return;
    u32 nkeys = C.mesh_cap << CL_KEY_SHIFT;
    if (C.count_cap < nkeys) {
        C.count = (u32 *)realloc(C.count, sizeof(u32) * nkeys);
        C.count_cap = nkeys;
    }
    memset(C.count, 0, sizeof(u32) * nkeys);
    for (u32 i = 0; i < C.tmp_n; i++) C.count[C.tmp_key[i]]++;
    u32 first;
    ClInst *dst = inst_alloc(C.tmp_n, &first);
    if (!dst) return;
    u32 run = 0;
    for (u32 k = 0; k < nkeys; k++) {
        if (!C.count[k]) continue;
        if (out->batch_count >= out->batch_cap) {
            out->batch_cap = out->batch_cap ? out->batch_cap * 2 : 64;
            out->batches = (ClBatch *)realloc(out->batches, sizeof(ClBatch) * out->batch_cap);
        }
        out->batches[out->batch_count++] = (ClBatch){ k, first + run, C.count[k] };
        u32 c = C.count[k];
        C.count[k] = run;   // write cursor
        run += c;
    }
    for (u32 i = 0; i < C.tmp_n; i++) dst[C.count[C.tmp_key[i]]++] = C.tmp[i];
}

// docs/perf/FINDINGS.md PF-0012: a sun cascade's instances, each batch ordered front to back from the
// light (32 light-depth buckets over the view's own depth range, stable within a bucket). Dense
// forests overdraw their shadow tiles ~40 times with alpha-tested cards; drawn canopy first, the
// fragments behind already written depth fail the early depth test before their alpha-test shader
// runs. A depth-only pass keeps the minimum depth of every fragment whatever the draw order (the
// fragments, their depths and their alpha tests do not depend on it): the map is bit-identical.
//
// PF-0015: the main view the same way with the camera's clip w (its view depth) for row 3: nearest
// instances (and big meshes) first, so the prepass's alpha-tested cards behind them fail early depth.
// The prepass keeps the nearest fragment of every pixel whatever the order (exact depth ties aside),
// and the lit pass shades at depth EQUAL what the prepass kept.
#define CL_DEPTH_BUCKETS 32u
static int cmp_batch_size(const void *pa, const void *pb) {
    const ClBatch *x = (const ClBatch *)pa, *y = (const ClBatch *)pb;
    u32 mx = x->key >> CL_KEY_SHIFT, my = y->key >> CL_KEY_SHIFT;
    float rx = C.scene && mx < C.scene->mesh_count ? C.scene->meshes[mx].radius : 0.0f;
    float ry = C.scene && my < C.scene->mesh_count ? C.scene->meshes[my].radius : 0.0f;
    if (rx != ry) return rx > ry ? -1 : 1;
    return x->key < y->key ? -1 : (x->key > y->key ? 1 : 0);
}
static void emit_view_depth_order(ClView *out, const mat4 *vp, int row) {
    out->batch_count = 0;
    out->instances = C.tmp_n;
    if (!C.tmp_n) return;
    const float *m = vp->m + row;   // column-major: row 2 = the NDC depth (light ortho: w = 1), row 3 = the clip w (camera)
    float zlo = 1e30f, zhi = -1e30f;
    static u8 *bucket;
    static u32 bucket_cap;
    if (bucket_cap < C.tmp_n) {
        bucket_cap = C.tmp_n * 2;
        bucket = (u8 *)realloc(bucket, bucket_cap);
    }
    for (u32 i = 0; i < C.tmp_n; i++) {
        const float *q = C.tmp[i].pos;
        float z = m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12];
        if (row == 3) z = log2f(fmaxf(z, 0.25f));   // camera distance: the buckets fine near, coarse far
        zlo = fminf(zlo, z), zhi = fmaxf(zhi, z);
    }
    float k = zhi > zlo ? (float)CL_DEPTH_BUCKETS / (zhi - zlo) : 0.0f;
    for (u32 i = 0; i < C.tmp_n; i++) {
        const float *q = C.tmp[i].pos;
        float z = m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12];
        if (row == 3) z = log2f(fmaxf(z, 0.25f));   // camera distance: the buckets fine near, coarse far
        int b = (int)((z - zlo) * k);
        bucket[i] = (u8)(b < 0 ? 0 : (b >= (int)CL_DEPTH_BUCKETS ? CL_DEPTH_BUCKETS - 1 : b));
    }
    u32 nkeys = C.mesh_cap << CL_KEY_SHIFT, n2 = nkeys * CL_DEPTH_BUCKETS;
    static u32 *count2;
    static u32 count2_cap;
    if (count2_cap < n2) {
        count2 = (u32 *)realloc(count2, sizeof(u32) * n2);
        count2_cap = n2;
    }
    memset(count2, 0, sizeof(u32) * n2);
    for (u32 i = 0; i < C.tmp_n; i++) count2[C.tmp_key[i] * CL_DEPTH_BUCKETS + bucket[i]]++;
    u32 first;
    ClInst *dst = inst_alloc(C.tmp_n, &first);
    if (!dst) return;
    u32 run = 0;
    for (u32 key = 0; key < nkeys; key++) {
        u32 *c = &count2[key * CL_DEPTH_BUCKETS], total = 0;
        for (u32 b = 0; b < CL_DEPTH_BUCKETS; b++) total += c[b];
        if (!total) continue;
        if (out->batch_count >= out->batch_cap) {
            out->batch_cap = out->batch_cap ? out->batch_cap * 2 : 64;
            out->batches = (ClBatch *)realloc(out->batches, sizeof(ClBatch) * out->batch_cap);
        }
        out->batches[out->batch_count++] = (ClBatch){ key, first + run, total };
        for (u32 b = 0; b < CL_DEPTH_BUCKETS; b++) {   // write cursors: buckets nearest the light first
            u32 n = c[b];
            c[b] = run;
            run += n;
        }
    }
    for (u32 i = 0; i < C.tmp_n; i++) dst[count2[C.tmp_key[i] * CL_DEPTH_BUCKETS + bucket[i]]++] = C.tmp[i];
    // Large meshes (trees) before small ones (bushes, grass under the canopy): the same batches.
    qsort(out->batches, out->batch_count, sizeof(ClBatch), cmp_batch_size);
}

static void build_lods(const ClView *v) {
    for (u32 b = 0; b < v->batch_count; b++) {
        u32 mesh = v->batches[b].key >> CL_KEY_SHIFT, lod = v->batches[b].key & 7u;
        ClMesh *cm = &C.mesh[mesh];
        if (lod != CL_IMPOSTOR && !cm->lods[lod].gpu && cm->verts) cl_build_exported(cm, lod);
        // docs/LOD.md: prefetch the next finer LOD, so it is resident before the band reaches it
        if (C.lod_new && lod != CL_IMPOSTOR && lod > 0 && !cm->lods[lod - 1].gpu && cm->verts) cl_build_exported(cm, lod - 1);
    }
}

// Main view parts per LOD / impostor and the fading ones (docs/LOD.md, lod_dolly).
static void count_view(const ClView *v) {
    memset(C.lod_n, 0, sizeof(C.lod_n));
    C.lod_fading = 0;
    C.counted = true;
    for (u32 b = 0; b < v->batch_count; b++) {
        u32 lod = v->batches[b].key & 7u;
        C.lod_n[lod == CL_IMPOSTOR ? 4 : DG_MIN(lod, 3u)] += v->batches[b].count;
        if (v->batches[b].key & CL_KEY_FADE) C.lod_fading += v->batches[b].count;
    }
}

void render_classic_lod_counts(u32 n[5], u32 *fading) {
    memcpy(n, C.lod_n, sizeof(C.lod_n));
    *fading = C.lod_fading;
}

// visibility_probe (docs/SETTINGS_AUDIT.md): the last main view's parts (every LOD and impostor),
// the ones in a fade, the impostors.
bool render_vgeo_counts(u32 *visible, u32 *faded, u32 *impostors) {
    if (!C.counted) return false;
    *visible = C.lod_n[0] + C.lod_n[1] + C.lod_n[2] + C.lod_n[3] + C.lod_n[4];
    *faded = C.lod_fading;
    *impostors = C.lod_n[4];
    return true;
}

// A threshold with the Engine Fidelity level's own scaling taken out (docs/LOD.md: the tier comes from
// lod.json instead): cur x modern default / level default.
static float without_level_scale(int feature, float cur) {
    if (!fid_loaded()) return cur;
    FidBounds b = fid_bounds(fid_level(), fid_era(), feature), m = fid_bounds(FID_MODERN, fid_era(), feature);
    if (b.era_lacks || b.def <= 0 || m.def <= 0) return cur;
    return cur * m.def / b.def;
}

// Sun cascade k: orthographic, LOD by the cascade's texel size, shadow distance, no impostors, its
// receiver volume (PF-0006), the caster classes it takes (PF-0013).
static void cascade_view(ClCull *v, u32 k, u32 tile, float shadow_px) {
    planes_of(R->shadow_vp_ndc[k], v->planes);   // the untiled matrix (render.c)
    v->ortho = v->shadow = true;
    v->layers = R->sun_shadow.culling_mask == SHADOW_LAYERS_ALL ? 0u : (R->sun_shadow.culling_mask | 0x100u);
    v->px_scale = (float)tile / (2.0f * R->shadow_radius[k]);   // the tile's half extent (render.c)
    v->lod_px = shadow_px;
    v->era2010 = false;
    v->nrcv = R->shadow_rcv_n[k];
    memcpy(v->rcv, R->shadow_rcv[k], sizeof(vec4) * (size_t)v->nrcv);
    v->scc_on = C.scc_mode > 0;
    v->scc = C.scc_view[k];
}

// docs/perf/FINDINGS.md PF-0013 (a visual trade-off): this frame's caster cascade limits per cascade
// (shadow_casters.h). The mode is the Engine Fidelity tier's r_shadow_caster_limit; perf_fix_disable
// PF-0013 turns it off for the A side of perf_verify.
static void caster_limits(u32 ncasc) {
    C.scc_mode = prof_fix_enabled(PFX_SHADOW_CASTER_LIMIT) && R->r_shadow_caster_limit ? R->r_shadow_caster_limit->i : 0;
    float splits[4] = { R->shadow_splits.x, R->shadow_splits.y, R->shadow_splits.z, R->shadow_splits.w };
    bool wide = R->shadow_near_w < 1.0f;   // cascade 0 fading into cascade 1: the limited classes go one further
    for (u32 k = 0; k < 4; k++) scc_view(&C.scc, C.scc_mode, (int)k, (int)ncasc, splits, wide, R->shq.border, &C.scc_view[k]);
}

void vkc_frame(const VgScene *s, const VgDisplace *disp, float density, float lod_px, float shadow_lod_px, float small_impostor_px, bool era2010, bool shadows) {
    C.scene = NULL;
    C.inst_used = 0;
    C.draws = C.tris = C.tris_shadow = C.local_instances = 0;
    C.lod_new = lod_active();
    for (int v = 0; v < CL_VIEWS; v++) C.view[v].batch_count = C.view[v].instances = 0;
    if (!C.ok || !s || !C.mesh_cap) return;
    if (!C.set_default) {
        VkImageView white = R->textures[R->white_tex].view;
        C.set_default = cl_alloc_set(white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, white, R->sampler_linear);
        if (!C.set_default) return;
    }
    u64 t0 = SDL_GetPerformanceCounter();
    C.scene = s;
    C.density = density;
    C.ncasc = 0;
    memset(C.disp, 0, sizeof(C.disp));
    if (disp && disp->ok && disp->valid) {
        VgViewGpu tv;
        memset(&tv, 0, sizeof(tv));
        vg_displace_view(disp, &tv);
        memcpy(C.disp, tv.disp, 16);
        if (C.disp_view != disp->view) {
            C.disp_view = disp->view;
            C.set_disp = cl_alloc_set(disp->view, VK_IMAGE_LAYOUT_GENERAL, R->textures[R->white_tex].view, R->sampler_linear);
        }
    }
    // main view
    ClCull v;
    memset(&v, 0, sizeof(v));
    planes_of(R->view_proj_nj, v.planes);
    v.eye = v.lod_eye = R->cam.pos;
    v.px_scale = RENDER_LOD_REF_H / (2.0f * tanf(R->cam.fov_deg * DG_DEG2RAD * 0.5f));   // resolution independent (V14)
    v.lod_px = lod_px;
    v.impostor_px = C.impostor_px->f;
    v.small_impostor_px = small_impostor_px;
    v.density = density;
    v.znear = R->cam.znear;
    v.era2010 = era2010;
    v.legacy = !C.lod_new;
    float shadow_px = shadow_lod_px;
    if (C.lod_new) {
        // docs/LOD.md: lod.json ranges (the same at every level); the shadow-view geometry error is
        // still a fidelity tier: its level scaling is taken out (without_level_scale)
        const LodTable *lt = lod_table();
        float band_k = lt->band > 0 ? lod_band() / lt->band : 1.0f;
        float s_st = lod_scale(LODC_STATIC), s_im = lod_scale(LODC_IMPOSTOR), s_fo = lod_scale(LODC_FOLIAGE), s_sh = lod_scale(LODC_SHADOW);
        v.lod_px = lod_px / s_st;   // locked (docs/SETTINGS_AUDIT.md): no level scale to take out
        v.impostor_px = C.impostor_px->f / s_im;
        v.small_impostor_px = small_impostor_px / s_im;
        shadow_px = without_level_scale(FF_VGEO_SHADOW_ERROR_PX, shadow_lod_px) / s_sh;
        v.band = lod_band();
        v.imp_band = lt->imp_band * band_k;
        v.fol_k = s_fo;
        v.fol_band_k = s_fo * lt->fol_band / lt->cat[LODC_FOLIAGE];
        v.fol_instance = lt->fol_instance;
        v.fol_shrink = lt->fol_shrink;
        v.shadow_k = s_sh;
        v.shadow_band = lt->shadow_band * band_k;
        v.fade_lim_k = fmaxf(v.fol_k, v.fol_band_k);
        v.shadow_lim_k = s_sh;
    }
    C.eff_lod_px = v.lod_px, C.eff_shadow_px = shadow_px, C.eff_imp_px = v.impostor_px, C.eff_small_px = v.small_impostor_px;
    g_main_cull = v, g_main_cull_ok = true;
    // Scope picture-in-picture: its own LOD / impostor / foliage selection from the PiP projection, so
    // a magnified distant object gets the detail the lens shows (docs/LOD.md "Scopes").
    mat4 pip_vp;
    vec3 pip_eye;
    float pip_px;
    bool batched = prof_fix_enabled(PFX_CLASSIC_CULL_BATCH);
    bool cascades = shadows && (R->shadow_active[0] || R->shadow_active[1] || R->shadow_active[2] || R->shadow_active[3]);
    u32 tile = R->shadow_size / 2;
    if (batched) {
        PROF_BEGIN("classic:cull_views");
        static ClCull cv[CL_VIEWS];
        ClView *outs[CL_VIEWS];
        const mat4 *lvps[CL_VIEWS] = { 0 };
        bool light_order = prof_fix_enabled(PFX_SHADOW_LIGHT_ORDER);
        int rows[CL_VIEWS] = { 0 };
        int nv = 0;
        bool pip = render_scope_cull(&pip_vp, &pip_eye, &pip_px);
        if (pip) {
            ClCull *pv = &cv[nv];
            *pv = v;
            planes_of(pip_vp, pv->planes);
            pv->eye = pv->lod_eye = pip_eye;
            pv->px_scale = pip_px;
            pv->znear = 0.3f;
            outs[nv++] = &C.view[CL_VIEW_PIP];
        }
        cv[nv] = v;
        if (prof_fix_enabled(PFX_VIEW_DEPTH_ORDER)) lvps[nv] = &R->view_proj_nj, rows[nv] = 3;
        outs[nv++] = &C.view[0];
        if (cascades) {
            C.ncasc = (u32)DG_CLAMP((int)R->shq.cascades, 1, 4);
            caster_limits(C.ncasc);
            for (u32 k = 0; k < C.ncasc; k++) {
                cv[nv] = v;
                cascade_view(&cv[nv], k, tile, shadow_px);
                lvps[nv] = light_order ? &R->shadow_vp_ndc[k] : NULL;
                rows[nv] = 2;
                outs[nv++] = &C.view[1 + k];
            }
        }
        cull_views(cv, outs, lvps, rows, nv, s);
        if (pip) build_lods(&C.view[CL_VIEW_PIP]);
        count_view(&C.view[0]);
        PROF_END();
    } else if (render_scope_cull(&pip_vp, &pip_eye, &pip_px)) {
        ClCull pv = v;
        planes_of(pip_vp, pv.planes);
        pv.eye = pv.lod_eye = pip_eye;
        pv.px_scale = pip_px;
        pv.znear = 0.3f;
        C.tmp_n = 0;
        cull_cells(&pv, s);
        emit_view(&C.view[CL_VIEW_PIP]);
        build_lods(&C.view[CL_VIEW_PIP]);
    }
    if (!batched) {
        PROF_BEGIN("classic:cull_main");
        C.tmp_n = 0;
        cull_cells(&v, s);
        if (prof_fix_enabled(PFX_VIEW_DEPTH_ORDER)) emit_view_depth_order(&C.view[0], &R->view_proj_nj, 3);
        else emit_view(&C.view[0]);
        count_view(&C.view[0]);
        PROF_END();
        PROF_BEGIN("classic:cull_cascades");
        if (cascades) {
            C.ncasc = (u32)DG_CLAMP((int)R->shq.cascades, 1, 4);
            caster_limits(C.ncasc);
            for (u32 k = 0; k < C.ncasc; k++) {
                cascade_view(&v, k, tile, shadow_px);
                C.tmp_n = 0;
                cull_cells(&v, s);
                if (prof_fix_enabled(PFX_SHADOW_LIGHT_ORDER)) emit_view_depth_order(&C.view[1 + k], &R->shadow_vp_ndc[k], 2);
                else emit_view(&C.view[1 + k]);
            }
        }
        PROF_END();
    }
    for (int i = 0; i < CL_VIEW_PIP; i++) build_lods(&C.view[i]);
    C.cpu_ms = (float)((double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency() * 1000.0);
}

// The frame's wind (after its tile upload, before any classic draw): the foliage fields of a vgeo
// view (wind outside the tile, this frame's and last frame's; time; layers; debug view; displacement
// buffer) and the tile ring (w NULL: no tile) into this frame slot's set 4.
void vkc_wind_frame(const VgViewGpu *v, const VgWindTex *w) {
    if (!C.ok) return;
    u32 f = R->frame_slot;
    ClWindGpu *g = (ClWindGpu *)C.wind_ubo[f].mapped;
    memcpy(g->wind, v->wind, 16);
    memcpy(g->wind_prev, v->wind_prev, 16);
    memcpy(g->tile, v->wind_tile, 16);
    memcpy(g->tile_prev, v->wind_tile_prev, 16);
    memcpy(g->disp, v->disp, 16);
    g->time[0] = v->foliage[0];
    g->time[1] = v->foliage[1];
    g->time[2] = g->time[3] = 0;
    g->layers = v->wind_layers;
    g->debug = v->wind_debug;
    bool ring = w && w->ok;
    g->ring = ring ? w->cur : 0;
    g->ring_prev = ring ? (w->cur + VG_WIND_RING - 1) % VG_WIND_RING : 0;
    if (!ring) g->tile[3] = g->tile_prev[3] = 0;
    // the ring's images (fixed): written once per slot (its last use has finished), white until the tile exists
    VkDescriptorImageInfo im[VG_WIND_RING];
    bool changed = false;
    for (u32 k = 0; k < VG_WIND_RING; k++) {
        VkImageView view = ring ? w->view[k] : R->textures[R->white_tex].view;
        if (view != C.set4_views[f][k]) changed = true;
        im[k] = (VkDescriptorImageInfo){ ring ? C.vtex->repeat : R->sampler_linear, view, ring ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }
    if (changed && im[0].imageView && (!ring || C.vtex)) {
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = C.set4[f];
        wr.dstBinding = 1;
        wr.descriptorCount = VG_WIND_RING;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = im;
        vkUpdateDescriptorSets(R->device, 1, &wr, 0, NULL);
        for (u32 k = 0; k < VG_WIND_RING; k++) C.set4_views[f][k] = im[k].imageView;
    }
}

// ------------------------------------------------------------------ drawing

// Any texture a vgeo material references: renderer ids as they are, module-owned slots wrapped.
static TextureId resolve_texture(u32 t) {
    if (t >= VG_TEX_GAME) t = module_texture(t);
    if (!t || t >= MAX_TEXTURES || !R->textures[t].used || !R->textures[t].ready) return 0;
    return t;
}

// The texture of a material draw: its albedo; a masked material without one uses its mask (its alpha
// is the cutout, its colour then tints nothing: white). A separate mask beside an albedo is not
// supported here (one sampler per draw): the albedo's alpha is tested (warned once).
static TextureId mat_texture(const VgMaterial *m) {
    TextureId a = resolve_texture(m->albedo_tex);
    if ((m->flags & VGM_MASKED) && m->mask_tex && m->mask_tex != m->albedo_tex) {
        if (!a) return resolve_texture(m->mask_tex);
        static bool warned;
        if (!warned) { warned = true; log_warn("classic: a masked material has a separate mask texture: its albedo alpha is tested instead"); }
    }
    return a;
}

// The sampler the texture set of a material draw holds: a module slot's own (repeat / clamp), else the renderer's.
static VkSampler mat_sampler(const VgMaterial *m, TextureId t) {
    u32 src = resolve_texture(m->albedo_tex) == t ? m->albedo_tex : m->mask_tex;
    if (src >= VG_TEX_GAME && C.vtex && src < VG_TEX_COUNT && C.vtex->e[src].sampler) return C.vtex->e[src].sampler;
    return src >= VG_TEX_GAME ? R->sampler_linear : vkr_texture_sampler(t);
}

// The glTF maps of a material (docs/PBR.md) as renderer textures: the MAT_*_TEX flags whose texture
// resolves (a map that does not, e.g. still uploading, is dropped: no undefined index reaches a shader)
// and the descriptor set of the combination (vkr_material_set; 0 = all white).
static u32 mat_maps(const VgMaterial *m, u32 *set) {
    *set = 0;
    if ((m->flags & MAT_COVER) && m->mr_tex) {
        // ground cover: its nta map (normal, thickness, AO; shaders/cover.glsl) in the first slot, no map flag
        Material pm;
        memset(&pm, 0, sizeof(pm));
        pm.mr_tex = resolve_texture(m->mr_tex);
        if (pm.mr_tex) {
            pm.flags = MAT_MR_TEX;
            *set = vkr_material_set(&pm);
        }
        return 0;
    }
    if (!(m->flags & MAT_PBR_MAPS)) return 0;
    Material pm;
    memset(&pm, 0, sizeof(pm));
    const u32 flag[4] = { MAT_MR_TEX, MAT_NORMAL_TEX, MAT_OCCLUSION_TEX, MAT_EMISSIVE_TEX };
    const u32 src[4] = { m->mr_tex, m->normal_tex, m->occlusion_tex, m->emissive_tex };
    TextureId *dst[4] = { &pm.mr_tex, &pm.normal_tex, &pm.occlusion_tex, &pm.emissive_tex };
    for (int i = 0; i < 4; i++) {
        TextureId t = (m->flags & flag[i]) && src[i] ? resolve_texture(src[i]) : 0;
        if (t) { pm.flags |= flag[i]; *dst[i] = t; }
    }
    if (pm.flags) *set = vkr_material_set(&pm);
    return *set ? pm.flags : 0;   // out of map sets: drawn without maps
}

// Push constants of a material draw (shaders/classic_common.glsl); maps: the MAT_*_TEX flags of mat_maps.
static void mat_push(PushLit *pc, const VgMaterial *m, TextureId tex, u32 maps, u32 shadow_view) {
    memset(pc, 0, sizeof(*pc));
    memcpy(&pc->model.m[0], m->wpo_params, 16);
    memcpy(&pc->model.m[4], m->wind_params, 32);   // model[1], model[2]
    pc->model.m[13] = (m->flags & VGM_MASKED) && tex ? m->alpha_cutoff : 0.0f;
    pc->model.m[14] = (float)(R->frame_index & 63u);
    pc->model.m[15] = (m->flags & VGM_DITHER_FADE) ? 1.0f : 0.0f;
    pc->color = v4(m->color[0], m->color[1], m->color[2], 1.0f);
    pc->params = v4(m->roughness, m->metallic, m->emissive, m->tex_scale > 0 ? m->tex_scale : 1.0f);
    pc->flags[0] = ((m->flags & ~(u32)(VGM_FLAGS | MAT_PBR_MAPS)) & ~(tex ? 0u : (u32)MAT_TEXTURED)) | maps;
    pc->flags[1] = m->wpo;
    pc->flags[3] = shadow_view;
    if (maps) {
        memcpy(&pc->pbr[0], &m->normal_scale, 4);
        memcpy(&pc->pbr[1], &m->occlusion_strength, 4);
    }
    pc->pbr[2] = m->emissive_rgb;   // its alpha: the AO byte (render/material_ao.h), with or without maps
}

static bool lod_ready(const ClLod *L) { return L->gpu && R->meshes[L->gpu].used && R->frame_index >= L->ready_frame; }

// pass 0 G-buffer, 1 lit, 2 shadow (view 1-4: cascade, or a local tile list), 3 scope PiP (lit forward)
static void draw_view(VkCommandBuffer cmd, const ClView *v, int pass, u32 shadow_view) {
    if (!C.scene || !v->batch_count) return;
    const VgScene *s = C.scene;
    VkDescriptorSet bound3 = C.set_default;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 3, 1, &C.set_default, 0, NULL);
    Material none;
    memset(&none, 0, sizeof(none));
    u32 bound5 = vkr_material_set(&none);   // set 5: no maps
    if (pass != 2) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 5, 1, &R->mat_sets[bound5].set, 0, NULL);
    int base = pass == 0 ? CP_GB : (pass == 1 ? CP_LIT : (pass == 3 ? CP_PIP : CP_SHADOW));
    if (!C.set4_views[R->frame_slot][0]) return;   // the frame's wind was never written (no draw before vkc_wind_frame)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 4, 1, &C.set4[R->frame_slot], 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 1, 1, &C.inst[R->frame_slot].buffer, &zero);
    // pipeline groups: plain one-sided first (early-z), then two-sided, then clipped
    int groups = pass == 2 ? 2 : 4;
    for (int g = 0; g < groups; g++) {
        int pipe = base + g;
        bool pipe_bound = false;
        TextureId bound_tex = ~0u;
        for (u32 b = 0; b < v->batch_count; b++) {
            const ClBatch *bt = &v->batches[b];
            u32 mesh = bt->key >> CL_KEY_SHIFT, lod = bt->key & 7u;
            bool fading = (bt->key & CL_KEY_FADE) != 0;
            if (lod == CL_IMPOSTOR) continue;
            const ClMesh *cm = &C.mesh[mesh];
            const ClLod *L = &cm->lods[lod];
            if (!lod_ready(L)) {
                if (!C.lod_new) continue;
                // docs/LOD.md streaming: the nearest resident LOD (coarser first) until this one is in
                u32 ready = 0;
                for (u32 l = 0; l < cm->lod_count; l++) ready |= lod_ready(&cm->lods[l]) ? 1u << l : 0u;
                int rl = lod_resident_pick((int)lod, ready, (int)cm->lod_count);
                if (rl < 0) continue;
                L = &cm->lods[rl];
            }
            const VgMesh *vm = &s->meshes[mesh];
            bool mesh_bound = false;
            for (u32 k = 0; k < L->nsec; k++) {
                const ClSection *sec = &L->sec[k];
                u32 mi = vm->material_base + sec->material;
                const VgMaterial *m = &s->materials[mi < s->material_count ? mi : 0];
                TextureId tex = mat_texture(m);
                bool two = (m->flags & VGM_TWO_SIDED) != 0;
                bool clip = fading || ((m->flags & VGM_MASKED) && tex) || (m->flags & VGM_DITHER_FADE);
                int want;
                if (pass == 2) want = fading || ((m->flags & VGM_MASKED) && tex) ? 1 : 0;
                else want = (clip ? 2 : 0) + (two ? 1 : 0);
                if (want != g) continue;
                if (!pipe_bound) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.pipe[pipe]);
                    pipe_bound = true;
                }
                if (!mesh_bound) {
                    GpuMesh *gm = &R->meshes[L->gpu];
                    vkCmdBindVertexBuffers(cmd, 0, 1, &gm->vb.buffer, &zero);
                    vkCmdBindIndexBuffer(cmd, gm->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                    mesh_bound = true;
                }
                TextureId t = tex ? tex : R->white_tex;
                // the shadow views and the prepass only read the alpha: its R8 mask once built (PF-0014)
                bool alpha_only = (m->flags & VGM_MASKED) && tex && (pass == 2 || (pass == 0 && g >= 2));
                TextureId bind_key = alpha_only ? t | 0x80000000u : t;
                if (bind_key != bound_tex) {
                    VkDescriptorSet ts = alpha_only ? vkam_set(t, mat_sampler(m, t)) : VK_NULL_HANDLE;
                    if (!ts) ts = R->textures[t].set;
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 1, 1, &ts, 0, NULL);
                    bound_tex = bind_key;
                }
                VkDescriptorSet want3 = (m->wpo & WPO_DISPLACE) && C.set_disp && C.disp[3] > 0.5f ? C.set_disp : C.set_default;
                if (want3 != bound3) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 3, 1, &want3, 0, NULL);
                    bound3 = want3;
                }
                u32 ms = 0, maps = pass == 2 ? 0 : mat_maps(m, &ms);
                if (pass != 2 && ms != bound5) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 5, 1, &R->mat_sets[ms].set, 0, NULL);
                    bound5 = ms;
                }
                PushLit pc;
                mat_push(&pc, m, tex, maps, shadow_view);
                if (fading) pc.model.m[15] = 1.0f;   // a fading part dithers whatever its material
                vkCmdPushConstants(cmd, C.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
                vkCmdDrawIndexed(cmd, sec->index_count, bt->count, sec->first_index, 0, bt->first);
                R->stats.draw_calls++;
                R->stats.triangles += sec->index_count / 3 * bt->count;
                C.draws++;
                C.tris += sec->index_count / 3 * bt->count;
                if (pass == 2) C.tris_shadow += sec->index_count / 3 * bt->count;
            }
        }
    }
    if (pass == 2) return;
    // impostors
    bool pipe_bound = false;
    for (u32 b = 0; b < v->batch_count; b++) {
        const ClBatch *bt = &v->batches[b];
        if ((bt->key & 7u) != CL_IMPOSTOR) continue;
        const ClMesh *cm = &C.mesh[bt->key >> CL_KEY_SHIFT];
        if (!cm->imp_set || !R->textures[cm->imp_albedo].ready || !R->textures[cm->imp_normal].ready) continue;
        if (!pipe_bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.pipe[pass == 0 ? CP_IMP_GB : (pass == 3 ? CP_PIP_IMP : CP_IMP_LIT)]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
            pipe_bound = true;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, C.layout, 3, 1, &cm->imp_set, 0, NULL);
        const VgMesh *vm = &s->meshes[bt->key >> CL_KEY_SHIFT];
        const VgMaterial *m = &s->materials[vm->material_base < s->material_count ? vm->material_base : 0];
        PushLit pc;
        memset(&pc, 0, sizeof(pc));
        pc.model.m[0] = cm->imp_center[0];
        pc.model.m[1] = cm->imp_center[1];
        pc.model.m[2] = cm->imp_center[2];
        pc.model.m[3] = cm->imp_radius;
        pc.model.m[4] = (float)cm->imp_grid;
        pc.model.m[14] = (float)(R->frame_index & 63u);
        pc.model.m[15] = (vm->fflags & VGMF_DITHER) || (bt->key & CL_KEY_FADE) ? 1.0f : 0.0f;
        float gain = C.lod_new ? lod_table()->imp_gain : 1.0f;   // docs/LOD.md: matches the mesh's mean colour
        pc.color = v4(gain, gain, gain, 1);
        pc.params = v4(m->roughness, m->metallic, m->emissive, 1.0f);
        pc.flags[0] = m->flags & ~(u32)(VGM_FLAGS | MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER | MAT_PBR_MAPS);
        vkCmdPushConstants(cmd, C.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
        vkCmdDraw(cmd, 6, bt->count, 0, bt->first);
        R->stats.draw_calls++;
        R->stats.triangles += 2 * bt->count;
        C.draws++;
        C.tris += 2 * bt->count;
    }
}

void vkc_draw_gbuffer(VkCommandBuffer cmd) { draw_view(cmd, &C.view[0], 0, 0); }
void vkc_draw_lit(VkCommandBuffer cmd) { draw_view(cmd, &C.view[0], 1, 0); }
void vkc_draw_pip(VkCommandBuffer cmd) { draw_view(cmd, &C.view[CL_VIEW_PIP], 3, 0); }
void vkc_draw_shadow(VkCommandBuffer cmd, u32 cascade) {
    if (cascade < C.ncasc) draw_view(cmd, &C.view[1 + cascade], 2, cascade);
}

// Point / spot light shadow tile: the instances touching the light's sphere and the tile frustum, LOD
// by distance to the light (~2 texels of error at the tile's resolution), instanced, masked
// materials alpha-tested. Inside the atlas pass (frame set bound); returns the draw count.
u32 vkc_draw_local_shadow(VkCommandBuffer cmd, vec4 sphere, const vec4 planes[6], u32 view, u32 tile_px, u32 layers) {
    if (!C.scene) return 0;
    static ClView lv;
    ClCull v;
    memset(&v, 0, sizeof(v));
    memcpy(v.planes, planes, sizeof(v.planes));
    v.eye = R->cam.pos;
    v.lod_eye = v3(sphere.x, sphere.y, sphere.z);
    v.sphere = sphere;
    v.px_scale = (float)tile_px * 0.5f;
    v.lod_px = 2.0f;
    v.density = C.density;
    v.znear = 0.05f;
    v.shadow = true;
    v.legacy = true;   // the cuts from before docs/LOD.md
    v.layers = (layers & SHADOW_LAYERS_ALL) == SHADOW_LAYERS_ALL ? 0u : (layers | 0x100u);
    C.tmp_n = 0;
    cull_cells(&v, C.scene);
    emit_view(&lv);
    build_lods(&lv);
    C.local_instances += lv.instances;
    u32 before = C.draws;
    draw_view(cmd, &lv, 2, view);
    return C.draws - before;
}

// classic_fade_probe (docs/LOD.md "Tests", tools/tests/lod_check.py): every scene instance (level and
// placements) within `radius` of the last main view's eye, in every direction, through that view's
// cull_instance. Counts the parts drawn with alpha < 1 (density / distance / shadow fade: dithered)
// outside a LOD cross-fade, split into trees (impostor-sized meshes) and other foliage, and the parts
// inside a cross-fade (a kept interval: impostor / LOD band).
typedef struct ClProbe { u32 kept, parts, alpha_tree, alpha_other, xfade; float min_alpha, min_d; } ClProbe;

static void probe_range(const ClCull *v, const VgScene *s, const VgInstance *inst, u32 count, float radius, ClProbe *pr) {
    for (u32 i = 0; i < count; i++) {
        const VgInstance *in = &inst[i];
        if (in->mesh >= C.mesh_cap || !C.mesh[in->mesh].used) continue;
        float p[3], q[4], sc;
        vgeo_instance_decode(in, p, q, &sc);
        float d = v3_dist(v3(p[0], p[1], p[2]), v->eye);
        if (d > radius) continue;
        ClInst out[CL_PARTS];
        u32 key[CL_PARTS], n = 0;
        cull_instance(v, s, in, out, key, &n);
        pr->kept += n > 0;
        for (u32 k = 0; k < n; k++) {
            pr->parts++;
            if (out[k].misc[2] != 0.0f) { pr->xfade++; continue; }
            if (out[k].misc[1] >= 1.0f) continue;
            if (C.mesh[in->mesh].coarse_tris >= CL_IMPOSTOR_TRIS) pr->alpha_tree++;
            else pr->alpha_other++;
            if (out[k].misc[1] < pr->min_alpha) pr->min_alpha = out[k].misc[1], pr->min_d = d;
        }
    }
}

void vkc_fade_probe(float radius) {
    if (!C.ok || !C.scene || !g_main_cull_ok) { log_info("classic_fade_probe: no frame"); return; }
    ClCull v = g_main_cull;
    memset(v.planes, 0, sizeof(v.planes));   // every direction
    const VgScene *s = C.scene;
    ClProbe pr = { 0, 0, 0, 0, 0, 1.0f, 0 };
    probe_range(&v, s, s->instances, s->instance_count, radius, &pr);
    const VgPlacements *pl = &s->pl;
    for (u32 h = 0; h < pl->chunk_count; h++) {
        const VgPlacementChunk *ch = &pl->chunks[h];
        if (!ch->used) continue;
        for (u32 k = 0; k < ch->cell_count; k++) {
            const VgCell *cl = &pl->cells[ch->cells[k]];
            if (cl->count) probe_range(&v, s, &pl->inst[cl->first], cl->count, radius, &pr);
        }
    }
    log_info("classic_fade_probe: radius %.0f m: %u instances, %u parts; alpha < 1 outside a cross-fade: trees / bushes %u, other %u (min alpha %.3f at %.1f m); in a cross-fade %u",
             (double)radius, pr.kept, pr.parts, pr.alpha_tree, pr.alpha_other, (double)pr.min_alpha, (double)pr.min_d, pr.xfade);
}

void vkc_stats(void) {
    if (!C.ok) { log_info("classic: unavailable"); return; }
    u32 meshes = 0, imp = 0;
    for (u32 i = 0; i < C.mesh_cap; i++) {
        meshes += C.mesh[i].used;
        imp += C.mesh[i].imp_set != VK_NULL_HANDLE;
    }
    log_info("classic: GPU %.1f MB (LOD buffers + impostor atlases, every vgeo mesh), CPU %.1f MB (exported LOD copies)", (double)C.gpu_bytes / 1048576.0,
             (double)C.cpu_bytes / 1048576.0);
    log_info("classic: %u meshes (%u with impostors); last frame: main %u instances in %u batches, cascades %u / %u / %u / %u instances, local shadows %u; %u draws, %.2fM tris (%.2fM in shadow views), "
             "CPU cull %.3f ms",
             meshes, imp, C.view[0].instances, C.view[0].batch_count, C.view[1].instances, C.view[2].instances, C.view[3].instances, C.view[4].instances, C.local_instances, C.draws,
             (double)C.tris / 1e6, (double)C.tris_shadow / 1e6, (double)C.cpu_ms);
    log_info("classic: LOD %s: error %.2f px (shadows %.2f), impostors below %.1f px (trees x species scale) / %.1f px (small cover); main view parts LOD0-3 %u / %u / %u / %u, "
             "impostors %u, fading %u",
             C.lod_new ? "docs/LOD.md" : "legacy", (double)C.eff_lod_px, (double)C.eff_shadow_px, (double)C.eff_imp_px, (double)C.eff_small_px, C.lod_n[0], C.lod_n[1], C.lod_n[2],
             C.lod_n[3], C.lod_n[4], C.lod_fading);
    // PF-0013: per class the last cascade it cast into and where it ended (docs/SHADOWS.md "Caster cascade limits")
    for (int c = SCC_GROUND; c < SCC_COUNT; c++) {
        int last = -1;
        float end = 0.0f;
        for (u32 k = 0; k < C.ncasc; k++)
            if (C.scc_view[k].allow >> c & 1u) last = (int)k, end = C.scc_view[k].end_m[c];
        log_info("classic: caster limit mode %d: %-6s cascades 0..%d of %u, %s%.1f m", C.scc_mode, scc_class_name(c), last, C.ncasc, end > 0 ? "ends at " : "unlimited ", (double)end);
    }
}
