// Virtualized geometry in the frame (docs/VGEO.md "In the renderer"): the vgeo module on the
// renderer's device, its static scene and the main view's culling, in the frame graph:
//
//   vgeo_cull (before the prepass)  cells -> instances -> BVH -> clusters against last frame's Hi-Z
//   vgeo_hiz  (after the prepass)   MAX Hi-Z of this frame's depth: next frame's occlusion history
//
// Phase 2: culling and LOD selection only (the visible lists are not rasterised until Phase 3).
// Per frame the CPU uploads one view and records a fixed number of dispatches; counters come back
// through a per-frame readback (vgeo_stats). Test content: vgeo_test_scene.
#include "vk_internal.h"
#include <SDL3/SDL.h>
#include "graph/fg.h"
#include "debug_draw.h"
#include "vgeo/vgeo_cull.h"
#include "vgeo/vgeo_procgen.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VKG_DEBUG_MAX 4096u   // visible clusters drawn by r_vgeo_debug_spheres

static struct {
    bool ok;
    VgContext ctx;
    VgScene scene;
    VgCuller cull;
    FgRes hiz, depth;
    VgViewGpu frozen;
    bool have_frozen;
    u32 test_meshes;             // procedural rocks registered by vgeo_test_scene
    VgBuffer dbg_rb[FRAMES_IN_FLIGHT];
    u32 dbg_count[FRAMES_IN_FLIGHT];
    // Graphics (Phase 3): the renderer's sets 0-2 + push constants (layout_lit compatible), vgeo set 3.
    VkPipelineLayout gfx_layout;
    VkPipeline p_gbuffer, p_lit, p_shadow, p_shadow_args;
    bool raster_ok;              // the visibility buffer is filled this frame (resolve / lit draws)
    bool shadow_culled;          // this frame's cascade views were culled (vkg_record_shadow_cull)
    Cvar *r_vgeo, *error_px, *shadow_error_px, *sw_px, *occlusion, *freeze, *debug, *debug_spheres, *shadows;
} G;

// ------------------------------------------------------------------ graphics pipelines

static VkPipeline gfx_pipeline(const char *vert, const char *frag, const VkFormat *colors, u32 ncolors, bool depth_test, bool depth_write, VkCompareOp cmp,
                               bool depth_bias) {
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
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = depth_bias;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = depth_test;
    ds.depthWriteEnable = depth_write;
    ds.depthCompareOp = cmp;
    VkPipelineColorBlendAttachmentState cba[4];
    memset(cba, 0, sizeof(cba));
    for (int i = 0; i < 4; i++) cba[i].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = ncolors;
    cb.pAttachments = cba;
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = depth_bias ? 3 : 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rci.colorAttachmentCount = ncolors;
    rci.pColorAttachmentFormats = colors;
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
    gp.layout = G.gfx_layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(R->device, VK_NULL_HANDLE, 1, &gp, NULL, &p) != VK_SUCCESS) log_error("vgeo: pipeline %s / %s failed", vert, frag ? frag : "-");
    vkDestroyShaderModule(R->device, vs, NULL);
    if (fs) vkDestroyShaderModule(R->device, fs, NULL);
    return p;
}

static bool create_gfx(void) {
    VkDescriptorSetLayout sets[4] = { R->frame_layout, R->tex_layout, R->lit_post_layout, G.cull.layout };
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 4;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(R->device, &pli, NULL, &G.gfx_layout) != VK_SUCCESS) return false;
    VkFormat gb[3] = { GB_NORMAL_FORMAT, GB_MATERIAL_FORMAT, GB_VELOCITY_FORMAT }, hdr = VK_FORMAT_R16G16B16A16_SFLOAT;
    G.p_gbuffer = gfx_pipeline("vgeo_fullscreen.vert", "vgeo_gbuffer.frag", gb, 3, true, true, VK_COMPARE_OP_LESS_OR_EQUAL, false);
    G.p_lit = gfx_pipeline("vgeo_fullscreen.vert", "vgeo_lit.frag", &hdr, 1, true, false, VK_COMPARE_OP_EQUAL, false);
    G.p_shadow = gfx_pipeline("vgeo_shadow.vert", NULL, NULL, 0, true, true, VK_COMPARE_OP_LESS_OR_EQUAL, true);
    G.p_shadow_args = vgc_compute_pipeline(&G.ctx, G.cull.pl, "vgeo/vg_shadow_args.comp");
    return G.p_gbuffer && G.p_lit && G.p_shadow && G.p_shadow_args;
}

static void destroy_gfx(void) {
    VkPipeline ps[] = { G.p_gbuffer, G.p_lit, G.p_shadow, G.p_shadow_args };
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) if (ps[i]) vkDestroyPipeline(R->device, ps[i], NULL);
    if (G.gfx_layout) vkDestroyPipelineLayout(R->device, G.gfx_layout, NULL);
    G.gfx_layout = VK_NULL_HANDLE;
}

static VgCullCaps game_caps(void) {
    VgCullCaps c = { 1u << 19, 1u << 20, 1u << 20, 1u << 19, 4096 };
    return c;
}

// ------------------------------------------------------------------ console

static void cmd_vgeo_stats(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G.ok) { log_info("vgeo: unavailable on this device"); return; }
    VgScene *s = &G.scene;
    log_info("vgeo: %u meshes, %u instances, %u cells, %u pages resident (%.0f MB), BVH depth %u", s->mesh_count, s->instance_count, s->cell_count, s->page_count,
             (double)s->page_count * VGEO_PAGE_SIZE / (1024.0 * 1024.0), s->max_depth);
    VgCounters c;
    if (!vg_cull_counters(&G.cull, 0, &c)) { log_info("vgeo: no culled frame yet"); return; }
    log_info("vgeo: pass 1: cells %u (+%u occluded), instances %u (+%u occluded), nodes %u (+%u occluded), clusters selected %u",
             c.n[VGN_CELLS_VIS], c.n[VGN_CELLS_OCC], c.n[VGN_INST_VIS], c.n[VGN_INST_OCC], c.n[VGN_NODES_VISITED], c.n[VGN_NODES_OCC], c.n[VGN_CLUSTERS_SELECTED]);
    log_info("vgeo: visible clusters HW %u / SW %u (pass 1: %u / %u), occluded %u, triangles %u, page requests %u, overflow %u", c.n[VGN_HW], c.n[VGN_SW],
             c.n[VGN_HW_P1], c.n[VGN_SW_P1], c.n[VGN_CL_OCC], c.n[VGN_TRIS], c.n[VGN_PAGE_REQ], c.n[VGN_OVERFLOW]);
    for (int i = 0; i < fg_pass_count(); i++)
        if (!strncmp(fg_pass_name(i), "vgeo_", 5)) log_info("vgeo: pass %s %.3f ms", fg_pass_name(i), fg_pass_ms(i));
}

// vgeo_test_scene [count=20000] [spacing=6] [meshes=4]: procedural rocks scattered around the camera.
static void cmd_vgeo_test_scene(int argc, const char **argv) {
    if (!G.ok) { log_info("vgeo: unavailable on this device"); return; }
    u32 count = argc > 1 ? (u32)atoi(argv[1]) : 20000u;
    float spacing = argc > 2 ? (float)atof(argv[2]) : 6.0f;
    u32 meshes = argc > 3 ? (u32)atoi(argv[3]) : 4u;
    if (meshes < 1) meshes = 1;
    u64 t0 = SDL_GetTicksNS();
    while (G.test_meshes < meshes) {
        VgGenMesh g;
        memset(&g, 0, sizeof(g));
        float c[3] = { 0, 0, 0 };
        vgeo_gen_rock(&g, 100 + G.test_meshes, 63, 1.0f, c, 0.25f + 0.05f * (float)(G.test_meshes % 4));
        VgSource src;
        memset(&src, 0, sizeof(src));
        src.vertex_count = g.vertex_count;
        src.pos = g.pos;
        src.index_count = g.index_count;
        src.indices = g.indices;
        VgBuildOptions opt;
        memset(&opt, 0, sizeof(opt));
        VgBuilt b;
        char err[256];
        bool ok = vgeo_build(&src, &opt, &b, err, sizeof(err));
        vgeo_gen_free(&g);
        if (!ok) { log_error("vgeo: rock build failed: %s", err); return; }
        vgeo_scene_add_mesh(&G.scene, &b);
        vgeo_free(&b);
        G.test_meshes++;
    }
    vgeo_scene_clear_instances(&G.scene);
    u32 side = (u32)ceilf(sqrtf((float)count));
    u32 rng = 1234;
    vec3 o = R->cam.pos;
    for (u32 i = 0; i < count; i++) {
        rng = rng * 1664525u + 1013904223u;
        float jx = (float)(rng >> 8) / 16777216.0f;
        rng = rng * 1664525u + 1013904223u;
        float jz = (float)(rng >> 8) / 16777216.0f;
        float p[3] = { o.x + ((float)(i % side) - (float)side * 0.5f) * spacing + jx * spacing * 0.5f, 0.0f,
                       o.z + ((float)(i / side) - (float)side * 0.5f) * spacing + jz * spacing * 0.5f };
        quat q = quat_yaw(jx * 6.2831853f + jz);
        float r[4] = { q.x, q.y, q.z, q.w };
        vgeo_scene_add_instance(&G.scene, i % G.test_meshes, p, r, 0.4f + jz * 2.0f);
    }
    if (!vgeo_scene_commit(&G.scene)) { log_error("vgeo: scene upload failed"); return; }
    log_info("vgeo: test scene of %u rock instances (%u meshes, %u cells) around (%.0f, %.0f) in %.0f ms", count, G.test_meshes, G.scene.cell_count, o.x, o.z,
             (double)(SDL_GetTicksNS() - t0) / 1e6);
}

static void cmd_vgeo_clear(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G.ok) return;
    vgeo_scene_clear_instances(&G.scene);
    vgeo_scene_commit(&G.scene);
    log_info("vgeo: instances cleared");
}

// ------------------------------------------------------------------ lifetime

void vkg_init(void) {
    G.r_vgeo = cvar_reg_bool("r_vgeo", true, CVAR_ARCHIVE, "virtualized geometry (docs/VGEO.md)");
    G.error_px = cvar_reg_float("r_vgeo_error_px", 1.0f, 0.25f, 16.0f, CVAR_ARCHIVE, "vgeo: allowed projected LOD error (pixels)");
    G.shadow_error_px = cvar_reg_float("r_vgeo_shadow_error_px", 2.0f, 0.25f, 32.0f, CVAR_ARCHIVE, "vgeo: projected LOD error in shadow views (pixels, Phase 3)");
    G.sw_px = cvar_reg_float("r_vgeo_sw_px", 32.0f, 0.0f, 512.0f, CVAR_ARCHIVE, "vgeo: clusters smaller than this (pixels across) go to the software rasteriser");
    G.occlusion = cvar_reg_int("r_vgeo_occlusion", 2, 0, 2, CVAR_ARCHIVE, "vgeo: occlusion culling 0 off, 1 last frame's Hi-Z, 2 two-pass (pass 2 from Phase 3)");
    G.freeze = cvar_reg_bool("r_vgeo_freeze", false, 0, "vgeo: freeze the culling camera");
    G.debug = cvar_reg_int("r_vgeo_debug", 0, 0, 8, 0, "vgeo debug view (Phase 3): 1 cluster, 2 triangle, 3 LOD, 4 error, 5 SW/HW, 6 residency, 7 overdraw, 8 instance");
    G.debug_spheres = cvar_reg_bool("r_vgeo_debug_spheres", false, 0, "vgeo: draw the culling spheres of up to 4096 visible clusters (HW green, SW cyan)");
    G.shadows = cvar_reg_bool("r_vgeo_shadows", true, CVAR_ARCHIVE, "vgeo: draw vgeo clusters into the sun shadow cascades");
    cmd_register("vgeo_stats", cmd_vgeo_stats, "virtualized geometry: scene, last frame's culling counters and pass timings");
    cmd_register("vgeo_test_scene", cmd_vgeo_test_scene, "vgeo_test_scene [count=20000] [spacing=6] [meshes=4]: procedural rock field around the camera");
    cmd_register("vgeo_clear", cmd_vgeo_clear, "remove every vgeo instance");
    if (!R->vg_scalar || !R->vg_int64) {
        log_warn("vgeo: disabled (needs scalarBlockLayout + shaderBufferInt64Atomics; the 2015 / 2010 era LODs are used instead)");
        return;
    }
    VgContext *c = &G.ctx;
    c->gpu = R->gpu;
    c->device = R->device;
    c->queue = R->queue;
    c->queue_family = R->queue_family;
    c->vma = R->vma;
    c->frames = FRAMES_IN_FLIGHT;
    c->timestamp_period = R->gpu_props.limits.timestampPeriod;
    c->load_shader = vkp_load_shader;
    c->int64_atomics = R->vg_int64;
    c->draw_indirect_count = R->vg_indirect_count;
    c->descriptor_indexing = R->vg_descriptor_indexing;
    c->mesh_shader = R->vg_mesh_shader;
    vgeo_scene_init(&G.scene, c);
    VgCullCaps caps = game_caps();
    if (!vg_cull_init(&G.cull, c, &G.scene, &caps)) {
        log_error("vgeo: culling pipelines unavailable");
        vg_cull_shutdown(&G.cull);
        return;
    }
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; f++) G.dbg_rb[f] = vgc_buffer(c, 8ull * 2 * VKG_DEBUG_MAX + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
    if (!create_gfx()) {
        log_error("vgeo: graphics pipelines unavailable");
        destroy_gfx();
        vg_cull_shutdown(&G.cull);
        return;
    }
    G.ok = true;
    log_info("vgeo: culling ready (indirect count %d, mesh shader %d)", c->draw_indirect_count, c->mesh_shader);
}

void vkg_shutdown(void) {
    if (!G.ok) return;
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; f++) vgc_buffer_free(&G.ctx, &G.dbg_rb[f]);
    vkDeviceWaitIdle(R->device);
    destroy_gfx();
    vg_cull_shutdown(&G.cull);
    vgeo_scene_free(&G.scene);
    G.ok = false;
}

static bool active(void) { return G.ok && G.r_vgeo->i && G.scene.instance_count > 0; }

// ------------------------------------------------------------------ debug spheres

// Draws the clusters copied out by the last use of this frame slot (fence already waited).
static void debug_spheres(u32 frame) {
    u32 n = G.dbg_count[frame];
    G.dbg_count[frame] = 0;
    if (!n || !G.debug_spheres->i) return;
    vmaInvalidateAllocation(G.ctx.vma, G.dbg_rb[frame].alloc, 0, VK_WHOLE_SIZE);
    const u32 *e = (const u32 *)G.dbg_rb[frame].mapped;
    u32 hw = DG_MIN(e[0], VKG_DEBUG_MAX), sw = DG_MIN(e[1], VKG_DEBUG_MAX);
    for (u32 i = 0; i < hw + sw; i++) {
        const u32 *it = e + 4 + (i < hw ? i : VKG_DEBUG_MAX + (i - hw)) * 2;   // HW block, then SW block
        u32 inst = it[0], packed = it[1];
        if (inst >= G.scene.instance_count || (packed >> VG_CLUSTER_BITS) >= G.scene.page_count) continue;
        const VgCluster *cl = vgeo_scene_cluster(&G.scene, packed >> VG_CLUSTER_BITS, packed & ((1u << VG_CLUSTER_BITS) - 1));
        float p[3], q[4], s;
        vgeo_instance_decode(&G.scene.instances[inst], p, q, &s);
        quat qq = { q[0], q[1], q[2], q[3] };
        vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(cl->center[0] * s, cl->center[1] * s, cl->center[2] * s)));
        dbg_sphere(c, cl->radius * s, i < hw ? DBG_GREEN : DBG_CYAN, 0.0f);
    }
}

// ------------------------------------------------------------------ frame graph

static void exec_cull(VkCommandBuffer cmd, void *u) {
    (void)u;
    u32 frame = R->frame_slot;
    vg_cull_collect(&G.cull, frame);
    debug_spheres(frame);
    VgViewGpu v;
    float fov = R->cam.fov_deg * DG_DEG2RAD;
    float eye[3] = { R->cam.pos.x, R->cam.pos.y, R->cam.pos.z };
    u32 flags = 0;
    if (G.occlusion->i == 0) flags |= VGV_NO_OCCLUSION;
    vgeo_view_setup(&v, R->view_proj_nj.m, R->prev_view_proj_nj.m, eye, (float)R->hdr.h / (2.0f * tanf(fov * 0.5f)), R->cam.znear, R->hdr.w, R->hdr.h,
                    G.error_px->f, G.sw_px->f, flags);
    if (G.freeze->i && G.have_frozen) v = G.frozen;
    else { G.frozen = v; G.have_frozen = true; }
    VgHizRef prev = { 0 };
    bool hist = G.hiz != FG_NONE && fg_history_valid(G.hiz) && !R->camera_cut && G.occlusion->i > 0;
    if (hist) {
        prev.view = fg_view(G.hiz);
        prev.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        prev.w = R->hdr.w;
        prev.h = R->hdr.h;
        prev.mips = fg_mips(G.hiz);
    }
    // Pass 2 needs this frame's Hi-Z from the visibility buffer (Phase 3): until then one pass.
    vg_cull_record(&G.cull, cmd, frame, 0, &v, hist ? &prev : NULL, NULL);
    if (G.debug_spheres->i) {
        VgViewRes *r = &G.cull.v[0];
        VkBufferCopy cc = { offsetof(VgCounters, n) + VGN_HW * 4, 0, 8 };
        vkCmdCopyBuffer(cmd, r->counters.buffer, G.dbg_rb[frame].buffer, 1, &cc);
        VkBufferCopy hc[2] = { { 0, 16, 8ull * VKG_DEBUG_MAX }, { 8ull * G.cull.caps.visible, 16 + 8ull * VKG_DEBUG_MAX, 8ull * VKG_DEBUG_MAX } };
        vkCmdCopyBuffer(cmd, r->visible.buffer, G.dbg_rb[frame].buffer, 2, hc);
        G.dbg_count[frame] = 1;
    }
}

static void exec_hiz(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkImageView mips[16];
    u32 n = fg_mips(G.hiz);
    if (n > 16) n = 16;
    for (u32 m = 0; m < n; m++) mips[m] = fg_mip_view(G.hiz, m);
    vg_hiz_record(&G.cull, cmd, R->frame_slot, fg_view(G.depth), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, mips, n, R->hdr.w, R->hdr.h);
}

void vkg_graph_cull(int depth_res) {
    G.hiz = FG_NONE;
    G.depth = depth_res;
    if (!active()) return;
    u32 hw, hh, hm;
    vg_hiz_size(R->hdr.w, R->hdr.h, &hw, &hh, &hm);
    FgTexDesc d = { "vgeo_hiz", VK_FORMAT_R32_SFLOAT, FG_SIZE_ABS, hw, hh, 0, DG_MIN(hm, (u32)FG_MAX_MIPS), 0 };
    G.hiz = fg_history(&d);
    int p = fg_pass("vgeo_cull", exec_cull, NULL);
    if (p < 0) return;
    fg_read(p, G.hiz, FG_SAMPLED);
}

void vkg_graph_hiz(int depth_res) {
    if (!active() || G.hiz == FG_NONE || G.occlusion->i == 0) return;
    int p = fg_pass("vgeo_hiz", exec_hiz, NULL);
    if (p < 0) return;
    fg_read(p, depth_res, FG_SAMPLED);
    fg_write(p, G.hiz, FG_STORAGE);
}

// ------------------------------------------------------------------ sun shadow cascades

// Before the shadow pass (outside rendering): one orthographic vgeo view per cascade (slots 1-3),
// frustum + LOD only, every cluster to the HW list, and its draw args.
void vkg_record_shadow_cull(VkCommandBuffer cmd) {
    G.shadow_culled = false;
    if (!active() || !G.shadows->i || R->r_shadow_quality->i <= 0) return;
    u32 frame = R->frame_slot;
    u32 tile = R->shadow_size / 2;
    for (u32 k = 0; k < 3; k++) {
        // untile: the cascade matrices map into their 2 x 2 atlas tile (render.c)
        mat4 t = mat4_identity();
        t.m[0] = 0.5f;
        t.m[5] = 0.5f;
        t.m[12] = (k & 1) ? 0.5f : -0.5f;
        t.m[13] = (k >> 1) ? 0.5f : -0.5f;
        mat4 vp = mat4_mul(mat4_inverse(t), R->shadow_vps[k]);
        float radius = (&R->shadow_splits.x)[k];
        float eye[3] = { R->cam.pos.x, R->cam.pos.y, R->cam.pos.z };
        VgViewGpu v;
        vgeo_view_setup(&v, vp.m, vp.m, eye, (float)tile / (2.0f * radius), 0.0f, tile, tile, G.shadow_error_px->f, 0.0f,
                        VGV_ORTHO | VGV_NO_OCCLUSION | VGV_NO_CONE);
        memcpy(v.raster_view_proj, R->shadow_vps[k].m, 64);
        vg_cull_record(&G.cull, cmd, frame, 1 + k, &v, NULL, NULL);
        VgPush pc;
        memset(&pc, 0, sizeof(pc));
        pc.view = 1 + k;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.p_shadow_args);
        VkDescriptorSet set = vg_cull_set(&G.cull, frame, 1 + k);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.cull.pl, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cmd, G.cull.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    G.shadow_culled = true;
}

// Inside the shadow pass, cascade k (viewport / scissor / depth bias / frame set already set).
void vkg_draw_shadow(VkCommandBuffer cmd, u32 k) {
    if (!G.shadow_culled) return;
    VkDescriptorSet set = vg_cull_set(&G.cull, R->frame_slot, 1 + k);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.p_shadow);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.gfx_layout, 3, 1, &set, 0, NULL);
    PushLit pc;
    memset(&pc, 0, sizeof(pc));
    pc.flags[3] = k;
    vkCmdPushConstants(cmd, G.gfx_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
    vkCmdDrawIndirect(cmd, G.cull.v[1 + k].counters.buffer, VGA_DRAW_HW * 16, 1, 16);
}

// ------------------------------------------------------------------ resolve

// Inside the prepass, before the legacy draws (G-buffer attachments cleared, frame set bound).
void vkg_draw_gbuffer(VkCommandBuffer cmd) {
    if (!G.raster_ok) return;
    VkDescriptorSet set = vg_cull_set(&G.cull, R->frame_slot, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.p_gbuffer);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.gfx_layout, 3, 1, &set, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// Inside the opaque pass (HDR + depth, frame set + lit inputs bound): shades the vgeo pixels.
void vkg_draw_lit(VkCommandBuffer cmd) {
    if (!G.raster_ok) return;
    VkDescriptorSet set = vg_cull_set(&G.cull, R->frame_slot, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.p_lit);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.gfx_layout, 1, 1, &R->textures[R->white_tex].set, 0, NULL);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.gfx_layout, 3, 1, &set, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
