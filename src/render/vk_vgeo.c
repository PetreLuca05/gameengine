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
#include "vgeo/vgeo_raster.h"
#include "vgeo/vgeo_procgen.h"
#include "assets/assets.h"
#include "core/platform.h"
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
    FgRes hiz, hiz_cur, depth;
    VgRaster raster;
    VgViewGpu frozen;
    bool have_frozen;
    u32 test_meshes;             // procedural rocks registered by vgeo_test_scene
    struct { char name[96]; u32 mesh; } named[64];   // level meshes loaded so far (by name)
    u32 named_count;
    // Era fallback (r_era 2015 / 2010): the exported discrete LODs per mesh, uploaded on first use
    // and drawn through the legacy path with a CPU LOD pick per instance.
    struct FallbackMesh {
        VgDiscreteLod lods[VGEO_MAX_LODS];
        u32 lod_count;
        VgLodVertex *verts;
        u32 *indices;
        MeshId gpu[VGEO_MAX_LODS];
        float radius;
    } *fb;
    u32 fb_cap;
    bool applying_era;
    // vgeo_bench: a fixed spline flythrough, per-frame pass timings to CSV + a summary.
    struct {
        int frame, frames;
        FILE *csv;
        double sum[24], worst_total, cpu_sum;
        char names[24][32];
        int npass;
    } bench;
    Cvar *era, *lod_px, *foliage_density;
    VgBuffer dbg_rb[FRAMES_IN_FLIGHT];
    u32 dbg_count[FRAMES_IN_FLIGHT];
    // Graphics (Phase 3): the renderer's sets 0-2 + push constants (layout_lit compatible), vgeo set 3.
    VkPipelineLayout gfx_layout;
    VkPipeline p_gbuffer, p_lit, p_shadow, p_shadow_args;
    bool raster_ok;              // the visibility buffer is filled this frame (resolve / lit draws)
    bool shadow_culled;          // this frame's cascade views were culled (vkg_record_shadow_cull)
    Cvar *r_vgeo, *error_px, *shadow_error_px, *sw_px, *occlusion, *freeze, *debug, *debug_spheres, *shadows;
} G;

static void fallback_register(u32 mesh, const VgBuilt *b);
static void vkg_register_era(void);
static void cmd_vgeo_bench(int argc, const char **argv);

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
        fallback_register(vgeo_scene_add_mesh(&G.scene, &b), &b);
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
    vkg_register_era();
    cmd_register("vgeo_bench", cmd_vgeo_bench, "vgeo_bench [frames=600] [csv]: spline flythrough of map vgeo_test, per-pass GPU ms to CSV + summary");
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
    if (!vg_raster_init(&G.raster, c, &G.cull)) {
        log_error("vgeo: rasterisers unavailable");
        vg_cull_shutdown(&G.cull);
        return;
    }
    vg_raster_resize(&G.raster, R->hdr.w, R->hdr.h);
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
    vg_raster_shutdown(&G.raster);
    vg_cull_shutdown(&G.cull);
    vgeo_scene_free(&G.scene);
    for (u32 i = 0; i < G.fb_cap; i++) { free(G.fb[i].verts); free(G.fb[i].indices); }
    free(G.fb);
    G.fb = NULL;
    G.fb_cap = 0;
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

static void full_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

static VgHizRef hiz_ref(FgRes r) {
    VgHizRef h = { fg_view(r), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, R->hdr.w, R->hdr.h, fg_mips(r) };
    return h;
}

// Pass 1: the main view against last frame's Hi-Z (its depth after the prepass: vgeo + legacy).
static void exec_cull(VkCommandBuffer cmd, void *u) {
    (void)u;
    u32 frame = R->frame_slot;
    vg_cull_collect(&G.cull, frame);
    debug_spheres(frame);
    vg_raster_resize(&G.raster, R->hdr.w, R->hdr.h);
    VgViewGpu v;
    float fov = R->cam.fov_deg * DG_DEG2RAD;
    float eye[3] = { R->cam.pos.x, R->cam.pos.y, R->cam.pos.z };
    u32 flags = 0;
    if (G.occlusion->i == 0) flags |= VGV_NO_OCCLUSION;
    if (G.occlusion->i >= 2) flags |= VGV_TWO_PASS;
    vgeo_view_setup(&v, R->view_proj_nj.m, R->prev_view_proj_nj.m, eye, (float)R->hdr.h / (2.0f * tanf(fov * 0.5f)), R->cam.znear, R->hdr.w, R->hdr.h,
                    G.error_px->f, G.sw_px->f, flags);
    if (G.freeze->i && G.have_frozen) v = G.frozen;
    else { G.frozen = v; G.have_frozen = true; }
    // Rasterised with this frame's jittered projection (the legacy prepass matches it); culled
    // (and velocities) unjittered.
    mat4 rvp = mat4_mul(R->proj_j, R->view);
    memcpy(v.raster_view_proj, rvp.m, 64);
    bool hist = G.hiz != FG_NONE && fg_history_valid(G.hiz) && !R->camera_cut && G.occlusion->i > 0;
    VgHizRef prev = hist ? hiz_ref(G.hiz) : (VgHizRef){ 0 };
    VgHizRef cur = G.hiz_cur != FG_NONE ? hiz_ref(G.hiz_cur) : (VgHizRef){ 0 };
    vg_cull_record_pass1(&G.cull, cmd, frame, 0, &v, hist ? &prev : NULL, G.hiz_cur != FG_NONE ? &cur : NULL);
    if (G.debug_spheres->i) {
        VgViewRes *r = &G.cull.v[0];
        VkBufferCopy cc = { offsetof(VgCounters, n) + VGN_HW * 4, 0, 8 };
        vkCmdCopyBuffer(cmd, r->counters.buffer, G.dbg_rb[frame].buffer, 1, &cc);
        VkBufferCopy hc[2] = { { 0, 16, 8ull * VKG_DEBUG_MAX }, { 8ull * G.cull.caps.visible, 16 + 8ull * VKG_DEBUG_MAX, 8ull * VKG_DEBUG_MAX } };
        vkCmdCopyBuffer(cmd, r->visible.buffer, G.dbg_rb[frame].buffer, 2, hc);
        G.dbg_count[frame] = 1;
    }
}

static void exec_raster1(VkCommandBuffer cmd, void *u) {
    (void)u;
    full_barrier(cmd);
    vg_raster_clear(&G.raster, cmd);
    vg_raster_record(&G.raster, cmd, R->frame_slot, 0, 1);
    G.raster_ok = true;
}

// This frame's Hi-Z from the pass 1 visibility buffer (pass 2 occlusion).
static void exec_hiz_cur(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkImageView mips[16];
    u32 n = DG_MIN(fg_mips(G.hiz_cur), 16u);
    for (u32 m = 0; m < n; m++) mips[m] = fg_mip_view(G.hiz_cur, m);
    vg_hiz_record(&G.cull, cmd, R->frame_slot, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, mips, n, R->hdr.w, R->hdr.h);
}

// Pass 2: pass 1's occluders against this frame's Hi-Z, rasterised into the same buffer.
static void exec_pass2(VkCommandBuffer cmd, void *u) {
    (void)u;
    full_barrier(cmd);
    vg_cull_record_pass2(&G.cull, cmd, R->frame_slot, 0);
    full_barrier(cmd);
    if (G.hiz_cur != FG_NONE) vg_raster_record(&G.raster, cmd, R->frame_slot, 0, 2);
    full_barrier(cmd);
}

// History for next frame's pass 1: this frame's depth after the prepass.
static void exec_hiz(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkImageView mips[16];
    u32 n = DG_MIN(fg_mips(G.hiz), 16u);
    for (u32 m = 0; m < n; m++) mips[m] = fg_mip_view(G.hiz, m);
    vg_hiz_record(&G.cull, cmd, R->frame_slot, fg_view(G.depth), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, mips, n, R->hdr.w, R->hdr.h);
}

void vkg_graph_cull(int depth_res) {
    G.hiz = G.hiz_cur = FG_NONE;
    G.depth = depth_res;
    G.raster_ok = false;
    if (!active()) return;
    u32 hw, hh, hm;
    vg_hiz_size(R->hdr.w, R->hdr.h, &hw, &hh, &hm);
    FgTexDesc d = { "vgeo_hiz", VK_FORMAT_R32_SFLOAT, FG_SIZE_ABS, hw, hh, 0, DG_MIN(hm, (u32)FG_MAX_MIPS), 0 };
    G.hiz = fg_history(&d);
    if (G.occlusion->i >= 2) {
        d.name = "vgeo_hiz_cur";
        G.hiz_cur = fg_texture(&d);
    }
    int p = fg_pass("vgeo_cull", exec_cull, NULL);
    if (p < 0) return;
    fg_read(p, G.hiz, FG_SAMPLED);
    if (G.hiz_cur != FG_NONE) fg_read(p, G.hiz_cur, FG_SAMPLED);   // bound now, written below
    p = fg_pass("vgeo_raster", exec_raster1, NULL);
    if (G.hiz_cur != FG_NONE) {
        p = fg_pass("vgeo_hiz_cur", exec_hiz_cur, NULL);
        fg_write(p, G.hiz_cur, FG_STORAGE);
    }
    p = fg_pass("vgeo_pass2", exec_pass2, NULL);
    if (G.hiz_cur != FG_NONE) fg_read(p, G.hiz_cur, FG_SAMPLED);
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

// ------------------------------------------------------------------ level placements

static bool name_is_file(const char *n) {
    size_t l = strlen(n);
    return l > 5 && !strcmp(n + l - 5, ".vgeo");
}

// Mesh index of a placement's mesh (loaded on first use), ~0 when unavailable.
static u32 level_mesh(const RenderVgeoPlacement *pl) {
    for (u32 i = 0; i < G.named_count; i++)
        if (!strcmp(G.named[i].name, pl->mesh)) return G.named[i].mesh;
    if (G.named_count >= 64) return ~0u;
    VgBuilt b;
    bool owned = false;
    const VgBuilt *src = NULL;
    VgMaterial mat;
    memset(&mat, 0, sizeof(mat));
    mat.color[0] = pl->color.x;
    mat.color[1] = pl->color.y;
    mat.color[2] = pl->color.z;
    mat.color[3] = 1.0f;
    mat.roughness = pl->roughness;
    mat.tex_scale = 1.0f;
    const Model *model = NULL;
    if (name_is_file(pl->mesh)) {
        char path[512];
        size_t size = 0;
        u8 *data = (u8 *)platform_read_file(platform_data_path(pl->mesh, path, sizeof(path)), &size);
        if (!data) { log_warn("vgeo: %s not found (generated at build time by tools/build_vgeo)", pl->mesh); return ~0u; }
        owned = vgeo_deserialize(data, size, &b);
        free(data);
        if (!owned) { log_error("vgeo: %s is not a valid .vgeo file (rebuild it: version %d)", pl->mesh, VGEO_FILE_VERSION); return ~0u; }
        src = &b;
    } else {
        model = assets_model(assets_model_id(pl->mesh));
        if (!model || !model->vgeo) { log_warn("vgeo: model '%s' has no vgeo data (\"vgeo\": true in data/assets.json)", pl->mesh); return ~0u; }
        src = model->vgeo;
    }
    u32 mesh = vgeo_scene_add_mesh(&G.scene, src);
    fallback_register(mesh, src);
    u32 nm = src->hdr.material_count ? src->hdr.material_count : 1, base = 0;
    for (u32 k = 0; k < nm; k++) {
        VgMaterial m = mat;
        if (model && model->mesh_count) {
            const Material *mm = &model->meshes[k < (u32)model->mesh_count ? k : (u32)model->mesh_count - 1].mat;
            m.color[0] = mm->color.x;
            m.color[1] = mm->color.y;
            m.color[2] = mm->color.z;
            m.roughness = mm->roughness;
            m.metallic = mm->metallic;
            m.emissive = mm->emissive;
        }
        u32 idx = vgeo_scene_add_material(&G.scene, &m);
        if (k == 0) base = idx;
    }
    vgeo_scene_set_mesh_materials(&G.scene, mesh, base);
    log_info("vgeo: level mesh %s: %u clusters, %u pages, %u tris", pl->mesh, src->hdr.cluster_count, src->hdr.page_count, src->stats.input_tris);
    if (owned) vgeo_free(&b);
    dg_strcpy(G.named[G.named_count].name, sizeof(G.named[0].name), pl->mesh);
    G.named[G.named_count++].mesh = mesh;
    return mesh;
}

void render_vgeo_set_level(const RenderVgeoPlacement *p, int count) {
    if (!G.ok) return;
    vgeo_scene_clear_instances(&G.scene);
    u32 total = 0;
    for (int i = 0; i < count; i++) {
        const RenderVgeoPlacement *pl = &p[i];
        u32 mesh = level_mesh(pl);
        if (mesh == ~0u) continue;
        u32 nx = pl->grid[0] ? pl->grid[0] : 1, nz = pl->grid[1] ? pl->grid[1] : 1;
        u32 h = pl->seed * 2654435761u + 1u;
        for (u32 z = 0; z < nz; z++)
            for (u32 x = 0; x < nx; x++) {
                h ^= h << 13;
                h ^= h >> 17;
                h ^= h << 5;
                float r0 = (float)(h & 0xFFFF) / 65535.0f, r1 = (float)(h >> 16) / 65535.0f;
                bool grid = nx * nz > 1;
                vec3 pos = v3_add(pl->pos, v3(((float)x - (float)(nx - 1) * 0.5f) * pl->spacing, 0, ((float)z - (float)(nz - 1) * 0.5f) * pl->spacing));
                quat q = quat_yaw(pl->yaw + (grid ? r0 * 6.2831853f : 0.0f));
                float sc = pl->scale * (1.0f + pl->scale_jitter * (r1 * 2.0f - 1.0f));
                float pp[3] = { pos.x, pos.y, pos.z }, rr[4] = { q.x, q.y, q.z, q.w };
                vgeo_scene_add_instance(&G.scene, mesh, pp, rr, sc);
                total++;
            }
    }
    if (!vgeo_scene_commit(&G.scene)) log_error("vgeo: scene upload failed");
    if (count) log_info("vgeo: level: %u instances of %u meshes, %u cells", total, G.named_count, G.scene.cell_count);
}

// ------------------------------------------------------------------ era presets + discrete LOD fallback

// r_era (docs/VGEO_PLAN.md section 6): every value is a plain cvar the preset sets; any other combination
// reads back as Custom (4).
typedef struct EraDef { int vgeo; float error_px, shadow_px, lod_px, foliage; } EraDef;
static const EraDef k_eras[4] = {
    { 1, 1.0f, 2.0f, 2.0f, 1.0f },   // modern: vgeo, 1 px
    { 1, 2.0f, 4.0f, 3.0f, 0.7f },   // 2020: vgeo, 2 px
    { 0, 1.0f, 2.0f, 4.0f, 0.4f },   // 2015: discrete DAG LODs (4), CPU LOD by screen size
    { 0, 1.0f, 2.0f, 12.0f, 0.2f },  // 2010: 3 LODs, more aggressive distances
};

static int era_detect(void) {
    for (int e = 0; e < 4; e++) {
        const EraDef *d = &k_eras[e];
        if (G.r_vgeo->i == d->vgeo && fabsf(G.error_px->f - d->error_px) < 1e-4f && fabsf(G.shadow_error_px->f - d->shadow_px) < 1e-4f &&
            fabsf(G.lod_px->f - d->lod_px) < 1e-4f && fabsf(G.foliage_density->f - d->foliage) < 1e-4f)
            return e;
    }
    return 4;
}

static void era_apply(int e) {
    const EraDef *d = &k_eras[DG_CLAMP(e, 0, 3)];
    G.applying_era = true;
    cvar_set_int(G.r_vgeo, d->vgeo);
    cvar_set_float(G.error_px, d->error_px);
    cvar_set_float(G.shadow_error_px, d->shadow_px);
    cvar_set_float(G.lod_px, d->lod_px);
    cvar_set_float(G.foliage_density, d->foliage);
    G.applying_era = false;
}

static void on_era(Cvar *c) {
    if (G.applying_era) return;
    if (c->i >= 0 && c->i <= 3 && era_detect() != c->i) era_apply(c->i);
}

static void on_era_value(Cvar *c) {
    (void)c;
    if (G.applying_era || !G.era) return;
    int e = era_detect();
    if (G.era->i != e) {
        G.applying_era = true;
        cvar_set_int(G.era, e);
        G.applying_era = false;
    }
}

static void vkg_register_era(void) {
    G.lod_px = cvar_reg_float("r_vgeo_lod_px", 2.0f, 0.25f, 64.0f, CVAR_ARCHIVE, "vgeo fallback (r_vgeo 0): projected error of the discrete LOD pick (pixels)");
    G.foliage_density = cvar_reg_float("r_foliage_density", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "foliage instance density (Phase 7)");
    G.era = cvar_reg_int("r_era", 0, 0, 4, CVAR_ARCHIVE, "geometry era preset: 0 modern (vgeo 1 px), 1 2020 (vgeo 2 px), 2 2015 (4 discrete LODs), 3 2010 (3 LODs), 4 custom");
    G.era->on_change = on_era;
    Cvar *dep[5] = { G.r_vgeo, G.error_px, G.shadow_error_px, G.lod_px, G.foliage_density };
    for (int i = 0; i < 5; i++) dep[i]->on_change = on_era_value;
    on_era_value(NULL);
}

// Keeps a CPU copy of a mesh's exported LODs (the fallback uploads them when first drawn).
static void fallback_register(u32 mesh, const VgBuilt *b) {
    if (mesh >= G.fb_cap) {
        u32 cap = G.fb_cap ? G.fb_cap : 16;
        while (cap <= mesh) cap *= 2;
        G.fb = (struct FallbackMesh *)realloc(G.fb, sizeof(*G.fb) * cap);
        memset(G.fb + G.fb_cap, 0, sizeof(*G.fb) * (cap - G.fb_cap));
        G.fb_cap = cap;
    }
    struct FallbackMesh *f = &G.fb[mesh];
    f->lod_count = b->hdr.lod_count;
    memcpy(f->lods, b->lods, sizeof(f->lods));
    u32 nv = 0, ni = 0;
    for (u32 k = 0; k < f->lod_count; k++) {
        nv = DG_MAX(nv, b->lods[k].first_vertex + b->lods[k].vertex_count);
        ni = DG_MAX(ni, b->lods[k].first_index + b->lods[k].index_count);
    }
    f->verts = (VgLodVertex *)malloc(sizeof(VgLodVertex) * (nv + 1));
    f->indices = (u32 *)malloc(sizeof(u32) * (ni + 1));
    memcpy(f->verts, b->lod_vertices, sizeof(VgLodVertex) * nv);
    memcpy(f->indices, b->lod_indices, sizeof(u32) * ni);
    f->radius = b->hdr.sphere[3];
}

static MeshId fallback_lod_mesh(struct FallbackMesh *f, u32 k) {
    if (f->gpu[k]) return f->gpu[k];
    const VgDiscreteLod *l = &f->lods[k];
    Vertex *v = (Vertex *)malloc(sizeof(Vertex) * (l->vertex_count + 1));
    for (u32 i = 0; i < l->vertex_count; i++) {
        const VgLodVertex *s = &f->verts[l->first_vertex + i];
        v[i].pos = v3(s->pos[0], s->pos[1], s->pos[2]);
        v[i].normal = v3(s->normal[0], s->normal[1], s->normal[2]);
        v[i].uv = v2(s->uv[0], s->uv[1]);
        v[i].color = 0xFFFFFFFFu;
    }
    u32 *idx = (u32 *)malloc(sizeof(u32) * (l->index_count + 1));
    for (u32 i = 0; i < l->index_count; i++) idx[i] = f->indices[l->first_index + i];   // local to the LOD's vertices
    f->gpu[k] = render_mesh_create(v, l->vertex_count, idx, l->index_count);
    free(v);
    free(idx);
    return f->gpu[k];
}

// Era fallback, once per frame after the camera is set (render.c): the coarsest discrete LOD whose
// projected error stays under r_vgeo_lod_px, per instance, through the legacy draw list. 2010 never
// draws LOD 0. CPU work per instance here is the fallback's cost (the vgeo path has none).
void vkg_submit_fallback(void) {
    if (!G.ok || G.r_vgeo->i || !G.scene.instance_count || !G.fb) return;
    bool era2010 = G.era && G.era->i == 3;
    float ps = (float)R->hdr.h / (2.0f * tanf(R->cam.fov_deg * DG_DEG2RAD * 0.5f));
    for (u32 i = 0; i < G.scene.instance_count; i++) {
        const VgInstance *in = &G.scene.instances[i];
        if (in->mesh >= G.fb_cap || !G.fb[in->mesh].lod_count) continue;
        struct FallbackMesh *f = &G.fb[in->mesh];
        float p[3], q[4], s;
        vgeo_instance_decode(in, p, q, &s);
        const VgMesh *m = &G.scene.meshes[in->mesh];
        quat qq = { q[0], q[1], q[2], q[3] };
        vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(m->center[0] * s, m->center[1] * s, m->center[2] * s)));
        float r = m->radius * s;
        float d = fmaxf(v3_dist(c, R->cam.pos) - r, R->cam.znear);
        u32 k = era2010 && f->lod_count > 1 ? 1 : 0;
        while (k + 1 < f->lod_count && f->lods[k + 1].error * s * ps / d <= G.lod_px->f) k++;
        MeshId mesh = fallback_lod_mesh(f, k);
        if (!mesh) continue;
        DrawItem di;
        memset(&di, 0, sizeof(di));
        di.mesh = mesh;
        di.model = mat4_trs(v3(p[0], p[1], p[2]), qq, v3s(s));
        const VgMaterial *vm = &G.scene.materials[m->material_base < G.scene.material_count ? m->material_base : 0];
        di.mat.color = v4(vm->color[0], vm->color[1], vm->color[2], 1.0f);
        di.mat.roughness = vm->roughness;
        di.mat.metallic = vm->metallic;
        di.mat.emissive = vm->emissive;
        di.mat.tex_scale = vm->tex_scale;
        render_draw(&di);
    }
}

// ------------------------------------------------------------------ vgeo_bench (flythrough)

// Fixed spline over the vgeo test scene (data/levels/vgeo_test.json): from the spawn north over the
// rock field, east to the building, south over the statue field, back.
static const float k_bench_path[][5] = {   // x, y, z, yaw (deg), pitch (deg)
    { 0, 2, 10, 0, -5 }, { 0, 6, -60, 0, -12 }, { 30, 20, -150, 60, -20 }, { 90, 8, -40, 160, -8 },
    { 110, 5, 0, 90, -5 }, { 60, 15, 80, 200, -15 }, { 0, 25, 160, 180, -25 }, { -60, 10, 120, 300, -10 }, { 0, 2, 10, 360, -5 },
};

static float catmull(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

// Called by render_set_camera: replaces the camera while the bench runs.
void vkg_bench_camera(RenderCamera *cam) {
    if (G.bench.frames <= 0) return;
    int n = (int)(sizeof(k_bench_path) / sizeof(k_bench_path[0]));
    float u = (float)G.bench.frame / (float)G.bench.frames * (float)(n - 1);
    int i = DG_CLAMP((int)u, 0, n - 2);
    float t = u - (float)i;
    float v[5];
    for (int k = 0; k < 5; k++)
        v[k] = catmull(k_bench_path[i > 0 ? i - 1 : 0][k], k_bench_path[i][k], k_bench_path[i + 1][k], k_bench_path[i + 2 < n ? i + 2 : n - 1][k], t);
    cam->pos = v3(v[0], v[1], v[2]);
    cam->yaw = -v[3] * DG_DEG2RAD;
    cam->pitch = v[4] * DG_DEG2RAD;
    cam->roll = 0;
}

// Once per frame (after fg_execute's timings of the previous frame are in).
void vkg_bench_frame(void) {
    if (G.bench.frames <= 0) return;
    if (G.bench.frame == 0) {
        G.bench.npass = 0;
        fprintf(G.bench.csv, "frame,cpu_ms");
        for (int i = 0; i < fg_pass_count() && G.bench.npass < 24; i++) {
            const char *nm = fg_pass_name(i);
            dg_strcpy(G.bench.names[G.bench.npass], 32, nm);
            fprintf(G.bench.csv, ",%s", nm);
            G.bench.npass++;
        }
        fprintf(G.bench.csv, ",vis_clusters,vis_tris\n");
    }
    VgCounters c;
    bool hc = vg_cull_counters(&G.cull, 0, &c);
    fprintf(G.bench.csv, "%d,%.3f", G.bench.frame, g_engine.frame_dt * 1000.0);
    G.bench.cpu_sum += g_engine.frame_dt * 1000.0;
    double total = 0;
    for (int k = 0; k < G.bench.npass; k++) {
        float ms = 0;
        for (int i = 0; i < fg_pass_count(); i++)
            if (!strcmp(fg_pass_name(i), G.bench.names[k])) ms = fg_pass_ms(i);
        fprintf(G.bench.csv, ",%.4f", ms);
        if (G.bench.frame >= 10) G.bench.sum[k] += ms;
        if (!strncmp(G.bench.names[k], "vgeo_", 5) || !strcmp(G.bench.names[k], "prepass") || !strcmp(G.bench.names[k], "opaque")) total += ms;
    }
    fprintf(G.bench.csv, ",%u,%u\n", hc ? c.n[VGN_HW] + c.n[VGN_SW] : 0, hc ? c.n[VGN_TRIS] : 0);
    if (G.bench.frame >= 10 && total > G.bench.worst_total) G.bench.worst_total = total;
    if (++G.bench.frame >= G.bench.frames) {
        int n = G.bench.frames - 10;
        log_info("vgeo_bench: %d frames, CPU %.2f ms avg; GPU ms avg per pass (frames 10+):", G.bench.frames, G.bench.cpu_sum / G.bench.frames);
        for (int k = 0; k < G.bench.npass; k++)
            if (!strncmp(G.bench.names[k], "vgeo_", 5) || !strcmp(G.bench.names[k], "prepass") || !strcmp(G.bench.names[k], "opaque") || !strcmp(G.bench.names[k], "final"))
                log_info("  %-16s %.3f", G.bench.names[k], G.bench.sum[k] / (n > 0 ? n : 1));
        log_info("  worst vgeo + prepass + opaque frame: %.3f ms", G.bench.worst_total);
        fclose(G.bench.csv);
        G.bench.csv = NULL;
        G.bench.frames = 0;
    }
}

// vgeo_bench [frames=600] [csv=vgeo_bench.csv]
static void cmd_vgeo_bench(int argc, const char **argv) {
    if (G.bench.csv) fclose(G.bench.csv);
    memset(&G.bench, 0, sizeof(G.bench));
    const char *path = argc > 2 ? argv[2] : "vgeo_bench.csv";
    G.bench.csv = fopen(path, "w");
    if (!G.bench.csv) { log_error("vgeo_bench: cannot write %s", path); return; }
    G.bench.frames = argc > 1 ? DG_MAX(atoi(argv[1]), 20) : 600;
    log_info("vgeo_bench: %d frames -> %s", G.bench.frames, path);
}

// ------------------------------------------------------------------ point / spot light shadows

static bool sphere_planes(const vec4 planes[6], vec3 c, float r) {
    for (int k = 0; k < 6; k++)
        if (planes[k].x * c.x + planes[k].y * c.y + planes[k].z * c.z + planes[k].w < -r) return false;
    return true;
}

// Local light shadow tile (vk_light_shadow.c): the vgeo instances of the placement cells touching the
// light's sphere and the tile frustum, as their discrete LOD meshes (picked by distance to the light,
// ~2 texels of error at the tile's resolution). Inside the atlas pass, PIPE_SHADOW bound by the caller.
u32 vkg_draw_local_shadow(VkCommandBuffer cmd, vec4 sphere, const vec4 planes[6], u32 cascade, u32 tile_px) {
    if (!G.ok || !G.r_vgeo->i || !G.shadows->i || !G.scene.instance_count || !G.fb) return 0;
    vec3 lc = v3(sphere.x, sphere.y, sphere.z);
    float px_scale = (float)tile_px * 0.5f;   // 90 degree face: px per metre at distance 1
    u32 n = 0, bound = 0;
    for (u32 ci = 0; ci < G.scene.cell_count; ci++) {
        const VgCell *cl = &G.scene.cells[ci];
        vec3 mn = v3(cl->min[0], cl->min[1], cl->min[2]), mx = v3(cl->max[0], cl->max[1], cl->max[2]);
        vec3 q = v3_max(mn, v3_min(lc, mx));
        if (v3_len2(v3_sub(q, lc)) > sphere.w * sphere.w) continue;
        for (u32 i = cl->first; i < cl->first + cl->count; i++) {
            const VgInstance *in = &G.scene.instances[i];
            if (in->mesh >= G.fb_cap || !G.fb[in->mesh].lod_count) continue;
            struct FallbackMesh *f = &G.fb[in->mesh];
            const VgMesh *m = &G.scene.meshes[in->mesh];
            float p[3], qr[4], s;
            vgeo_instance_decode(in, p, qr, &s);
            quat qq = { qr[0], qr[1], qr[2], qr[3] };
            vec3 c = v3_add(v3(p[0], p[1], p[2]), quat_rotate(qq, v3(m->center[0] * s, m->center[1] * s, m->center[2] * s)));
            float r = m->radius * s;
            if (v3_len2(v3_sub(c, lc)) > (r + sphere.w) * (r + sphere.w) || !sphere_planes(planes, c, r)) continue;
            float d = fmaxf(v3_dist(c, lc) - r, 0.05f);
            u32 k = 0;
            while (k + 1 < f->lod_count && f->lods[k + 1].error * s * px_scale / d <= 2.0f) k++;
            MeshId mesh = fallback_lod_mesh(f, k);
            if (!mesh || !R->meshes[mesh].used) continue;
            GpuMesh *gm = &R->meshes[mesh];
            if (mesh != bound) {
                VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &gm->vb.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, gm->ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                bound = mesh;
            }
            PushLit pc;
            memset(&pc, 0, sizeof(pc));
            pc.model = mat4_trs(v3(p[0], p[1], p[2]), qq, v3s(s));
            pc.color = v4(1, 1, 1, 1);
            pc.flags[3] = cascade;
            vkCmdPushConstants(cmd, R->layout_lit, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
            vkCmdDrawIndexed(cmd, gm->index_count, 1, 0, 0, 0);
            R->stats.draw_calls++;
            n++;
        }
    }
    return n;
}
