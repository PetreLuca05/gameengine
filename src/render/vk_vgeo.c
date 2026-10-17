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
#include "vgeo/vgeo_stream.h"
#include "vgeo/vgeo_skin.h"
#include "vgeo/vgeo_procgen.h"
#include "vgeo/vgeo_import.h"
#include "vgeo/foliage_support.h"
#include "vgeo/vgeo_procgen_foliage.h"
#include "vgeo/vgeo_impostor.h"
#include "vgeo/vgeo_tree.h"
#include "fidelity/fidelity.h"
#include "assets/assets.h"
#include "core/platform.h"
#include "core/jobs.h"
#include "stb_image.h"
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
    VgStream stream;
    bool stream_ok;
    // Skinned characters (docs/VGEO.md "Characters"): renderer skinned meshes built to vgeo on a worker,
    // a pool of instances per mesh (one palette slot each), slots assigned to characters per frame.
    VgPalettes pal;
    bool pal_ok;
    u32 slot_mat_base, slots_used;
    struct SkinRec {
        MeshId mesh;
        int state;               // 0 free, 1 queued, 2 built (waiting to enter the scene), 3 in the scene, -1 failed
        VgBuilt built;
        u32 vmesh, first_slot, pool, want_pool, need;
        float bones[VGEO_MAX_BONES][4];
    } skin[128];
    u32 skin_count;
    struct SkinUse { u32 object_id; u16 rec; u16 pad; u32 frame; } uses[2048];   // slot -> character using it
    SDL_Thread *worker;
    SDL_Mutex *mutex;
    SDL_Semaphore *sem;
    int queued[128];
    u32 queue_head, queue_tail;
    bool quit;
    VgMaterial slot_mat[2048];
    u8 slot_mat_dirty[2048];
    u32 route_frame;
    Cvar *skinned;
    // vgeo_check_resolve: one frame's depth / velocity / visibility buffer read back and verified.
    struct {
        int state;               // 0 idle, 1 requested, 2 copied in frame slot `slot`
        u32 slot, w, h;
        VgBuffer depth, vel, vis;
        mat4 inv_raster_vp, vp_nj, prev_vp_nj;
        FgRes velocity;
    } check;
    Cvar *pool_mb, *pages_per_frame;
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
        const VgLodVertex *verts;
        const u32 *indices;
        bool own;                // verts / indices are copies (transient builds)
        MeshId gpu[VGEO_MAX_LODS];
        float radius;
    } *fb;
    u32 fb_cap;
    bool applying_era;
    // vgeo_bench: a fixed spline flythrough, per-frame pass timings to CSV + a summary.
    struct {
        int frame, frames;
        FILE *csv;
        double sum[24], worst_total, cpu_sum, shadow_sum[2], worst_foliage;
        char names[24][32];
        int npass;
        int path;                // 0 map vgeo_test, 1 the devroom foliage yard
    } bench;
    Cvar *era, *lod_px, *foliage_density;
    VgBuffer dbg_rb[FRAMES_IN_FLIGHT];
    u32 dbg_count[FRAMES_IN_FLIGHT];
    // Graphics (Phase 3): the renderer's sets 0-2 + push constants (layout_lit compatible), vgeo set 3.
    VkPipelineLayout gfx_layout;
    // Material textures: renderer texture ids index the vgeo texture table (binding 23 of the cull
    // sets, vgeo_tex.h), entered once a material needs the texture and it is ready (white until then).
    u8 tex_wanted[MAX_TEXTURES];
    u8 tex_entered[MAX_TEXTURES];
    VkPipeline p_gbuffer, p_lit, p_shadow, p_shadow_args, p_shadow_masked;
    // Foliage (docs/VGEO.md "Foliage support"): wind of the previous frame (velocity), the displacement
    // buffer, foliage types by name (placement chunks resolve their mesh names here), the check.
    Cvar *impostor_px, *foliage_wind, *foliage_wind_solo;
    float wind_cur[4], wind_prev[4], time_cur, time_prev;
    bool wind_init;
    VgWindTex wind;              // the wind field's tile (docs/FOLIAGE_WIND.md)
    bool wind_ok;
    bool wind_mat_dirty;         // render_foliage_wind_set: materials to re-upload this frame
    VgDisplace disp;
    bool disp_ok;
    // role / weight: the scatter role of a tree type (RENDER_TREE_*, -1 none) and its pick weight
    struct FoliageType { char name[48]; u32 mesh; bool capsule; float cap_r, cap_h; int role; float weight; } ftype[32];
    u32 ftype_count;
    bool test_types, map_types;
    u32 tree_roles;              // roles whose imported species were loaded (bits of VgTreeRole)
    struct { char name[64]; u32 tex; } tree_tex[80];   // imported tree textures by file (+ "#n" / "#orm" bark maps; shared by species)
    u32 tree_tex_count;
    size_t tree_tex_bytes;
    double disp_time;
    float push_test;             // foliage_push_test: seconds left
    bool need_commit;            // a placement chunk resolved a mesh the scene did not have
    // vgeo part of the sun shadows (outside the frame graph): timestamps around the cascade culls and
    // the cascade draws, per frame in flight; ms of the last completed frame.
    VkQueryPool sqp;
    bool sqp_written[FRAMES_IN_FLIGHT];
    float shadow_ms[2];          // cull, draw
    bool raster_ok;              // the visibility buffer is filled this frame (resolve / lit draws)
    bool pass2_pending;          // vkg_graph_cull added pass 1: vkg_graph_cull_pass2 adds the rest
    bool shadow_culled;          // this frame's cascade views were culled (vkg_record_shadow_cull)
    Cvar *r_vgeo, *error_px, *shadow_error_px, *sw_px, *occlusion, *freeze, *debug, *debug_spheres, *shadows;
} G;

static void fallback_register(u32 mesh, const VgBuilt *b, bool persistent);
static void skin_init(void);
static void skin_shutdown(void);
static void skin_record(VkCommandBuffer cmd);
static void skin_readd_pools(void);
static void vkg_register_era(void);
static void cmd_vgeo_bench(int argc, const char **argv);
static void cmd_vgeo_check_resolve(int argc, const char **argv);
static void cmd_foliage_push_test(int argc, const char **argv);
static void cmd_foliage_stats(int argc, const char **argv);
static void cmd_foliage_query(int argc, const char **argv);
static void cmd_foliage_check(int argc, const char **argv);

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

// A material uses texture `id` (the resolve samples it through the vgeo texture table).
static void tex_use(TextureId id) {
    if (id && id < MAX_TEXTURES) G.tex_wanted[id] = 1;
}

void vkg_samplers_changed(void) { memset(G.tex_entered, 0, sizeof(G.tex_entered)); }

// Start of a frame: textures wanted and ready enter the table (the cull sets pick them up when recorded).
static void tex_update(void) {
    for (u32 i = 1; i < MAX_TEXTURES; i++)
        if (G.tex_wanted[i] && !G.tex_entered[i] && R->textures[i].used && R->textures[i].ready) {
            vg_tex_set(&G.cull.tex, i, R->textures[i].view, R->sampler_linear, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            G.tex_entered[i] = 1;
        }
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
    G.p_shadow_masked = gfx_pipeline("vgeo_shadow_masked.vert", "vgeo_shadow_masked.frag", NULL, 0, true, true, VK_COMPARE_OP_LESS_OR_EQUAL, true);
    G.p_shadow_args = vgc_compute_pipeline(&G.ctx, G.cull.pl, "vgeo/vg_shadow_args.comp");
    return G.p_gbuffer && G.p_lit && G.p_shadow && G.p_shadow_args && G.p_shadow_masked;
}

static void destroy_gfx(void) {
    VkPipeline ps[] = { G.p_gbuffer, G.p_lit, G.p_shadow, G.p_shadow_args, G.p_shadow_masked };
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
    log_info("vgeo: %u meshes, %u instances, %u cells, %u pages, pool %u slots (%.0f MB), BVH depth %u", s->mesh_count, s->instance_count, s->cell_count, s->page_count,
             s->pool_slots, (double)s->pool_slots * VGEO_PAGE_SIZE / (1024.0 * 1024.0), s->max_depth);
    if (G.stream_ok) {
        const VgStreamStats *ss = vg_stream_stats(&G.stream);
        log_info("vgeo: streaming: %u resident, %u free, requested %u, loaded %u / evicted %u last frame (%llu / %llu total), %u waiting, cpu %.3f ms, gpu %.3f ms",
                 ss->resident, ss->free, ss->requested, ss->loaded, ss->evicted, (unsigned long long)ss->loaded_total, (unsigned long long)ss->evicted_total, ss->waiting,
                 ss->cpu_ms, ss->gpu_ms);
    }
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
        fallback_register(vgeo_scene_add_mesh(&G.scene, &b), &b, false);
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
    G.skinned = cvar_reg_bool("r_vgeo_characters", true, CVAR_ARCHIVE, "vgeo: draw skinned characters (main view) through vgeo");
    G.pool_mb = cvar_reg_int("r_vgeo_pool_mb", 512, 16, 8192, CVAR_ARCHIVE, "vgeo: GPU page pool budget (MB of 128 KiB pages; applied on the next scene upload)");
    G.pages_per_frame = cvar_reg_int("r_vgeo_stream_pages_per_frame", 64, 1, 1024, CVAR_ARCHIVE, "vgeo: pages streamed in per frame");
    G.impostor_px = cvar_reg_float("r_vgeo_impostor_px", 16.0f, 0.0f, 256.0f, CVAR_ARCHIVE,
                                   "vgeo: instances with an impostor atlas draw it below this projected size (pixels across, 0 = never)");
    G.foliage_wind = cvar_reg_int("r_foliage_wind", 3, 0, 3, CVAR_ARCHIVE,
                                  "foliage wind animation (docs/FOLIAGE_WIND.md): 0 none, 1 main bending, 2 + branch bending, 3 + leaf flutter and bent normals");
    G.foliage_wind_solo = cvar_reg_int("r_foliage_wind_solo", 0, 0, 3, 0, "foliage wind debug: 0 every layer (r_foliage_wind), 1 main bending only, 2 detail only, 3 flutter only");
    cmd_register("foliage_push_test", cmd_foliage_push_test, "foliage_push_test [seconds=10]: a ring of displacement-buffer pushes circling the camera");
    cmd_register("foliage_stats", cmd_foliage_stats, "foliage: placement chunks, region allocator audit, last frame's foliage counters");
    cmd_register("foliage_query", cmd_foliage_query, "foliage_query [radius=2]: placement instances around the camera (foliage_query API)");
    cmd_register("foliage_check", cmd_foliage_check, "foliage_check: scripted checks (Hi-Z behind the foliage yard wall, density stability over 600 frames)");
    cmd_register("vgeo_stats", cmd_vgeo_stats, "virtualized geometry: scene, last frame's culling counters and pass timings");
    cmd_register("vgeo_test_scene", cmd_vgeo_test_scene, "vgeo_test_scene [count=20000] [spacing=6] [meshes=4]: procedural rock field around the camera");
    cmd_register("vgeo_clear", cmd_vgeo_clear, "remove every vgeo instance");
    cmd_register("vgeo_check_resolve", cmd_vgeo_check_resolve, "verify the next frame's vgeo depth export and velocity against the visibility buffer");
    vkg_register_era();
    cmd_register("vgeo_bench", cmd_vgeo_bench, "vgeo_bench [frames=600] [csv] [vgeo_test|foliage]: spline flythrough of map vgeo_test (or the devroom foliage yard), per-pass GPU ms to CSV + summary");
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
    VgStreamConfig sc = { (u32)G.pool_mb->i, (u32)G.pages_per_frame->i };
    G.stream_ok = vg_stream_init(&G.stream, &G.scene, &G.cull, &sc);
    G.disp_ok = vg_displace_init(&G.disp, c, &G.cull.tex);
    if (!G.disp_ok) log_warn("vgeo: foliage displacement buffer unavailable");
    G.wind_ok = vg_wind_init(&G.wind, c, &G.cull.tex);
    if (!G.wind_ok) log_warn("vgeo: wind field tile unavailable (foliage uses the mean wind)");
    VkQueryPoolCreateInfo sq = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    sq.queryType = VK_QUERY_TYPE_TIMESTAMP;
    sq.queryCount = 8 * FRAMES_IN_FLIGHT;   // cull begin / end, 3 x draw begin / end
    vkCreateQueryPool(R->device, &sq, NULL, &G.sqp);
    skin_init();
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
    vgc_buffer_free(&G.ctx, &G.check.depth);
    vgc_buffer_free(&G.ctx, &G.check.vel);
    vgc_buffer_free(&G.ctx, &G.check.vis);
    skin_shutdown();
    if (G.disp_ok) vg_displace_shutdown(&G.disp);
    if (G.wind_ok) vg_wind_shutdown(&G.wind);
    G.wind_ok = false;
    G.disp_ok = false;
    if (G.sqp) vkDestroyQueryPool(R->device, G.sqp, NULL);
    G.sqp = VK_NULL_HANDLE;
    if (G.stream_ok) vg_stream_shutdown(&G.stream);
    G.stream_ok = false;
    vg_raster_shutdown(&G.raster);
    vg_cull_shutdown(&G.cull);
    vgeo_scene_free(&G.scene);
    for (u32 i = 0; i < G.fb_cap; i++)
        if (G.fb[i].own) { free((void *)G.fb[i].verts); free((void *)G.fb[i].indices); }
    vgeo_file_release_all();
    free(G.fb);
    G.fb = NULL;
    G.fb_cap = 0;
    G.ok = false;
}

static bool active(void) { return G.ok && G.r_vgeo->i && (G.scene.instance_count > 0 || G.scene.pl.live_chunks > 0 || G.scene.pl.dirty_cells_n > 0); }

// The foliage fields of a view (WPO wind / time this frame and last, density, impostor switch,
// displacement buffer, dither rotation).
// The wind outside the field's tile (docs/FOLIAGE_WIND.md): instant x, z, settled bend x, z.
static void foliage_wind_frame(void) {
    const RenderWeather *wx = &R->weather;
    float w[4] = { wx->wind_global.x, wx->wind_global.y, wx->wind_global.z, wx->wind_global.w };
    memcpy(G.wind_prev, G.wind_init ? G.wind_cur : w, 16);
    G.time_prev = G.wind_init ? G.time_cur : wx->time;
    memcpy(G.wind_cur, w, 16);
    G.time_cur = wx->time;
    G.wind_init = true;
}
static void view_foliage(VgViewGpu *v) {
    memcpy(v->wind, G.wind_cur, 16);
    memcpy(v->wind_prev, G.wind_prev, 16);
    v->foliage[0] = G.time_cur;
    v->foliage[1] = G.time_prev;
    v->foliage[2] = G.foliage_density->f;
    v->foliage[3] = G.impostor_px->f;
    v->frame_index = R->frame_index;
    if (G.disp_ok) vg_displace_view(&G.disp, v);
    if (G.wind_ok) vg_wind_view(&G.wind, v);
    // layers: the fidelity tier (r_foliage_wind), or one layer solo (tuning); the debug view
    int solo = R->weather.wind_solo ? R->weather.wind_solo : G.foliage_wind_solo->i;
    v->wind_layers = solo == 1 ? VGW_MAIN : solo == 2 ? VGW_DETAIL : solo == 3 ? VGW_FLUTTER : vgeo_wind_tier_layers(G.foliage_wind->i);
    v->wind_debug = (u32)DG_CLAMP(R->weather.wind_view, 0, 2);
}

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
        const VgInstance *vin = vgeo_scene_instance(&G.scene, inst);
        if (!vin || (packed >> VG_CLUSTER_BITS) >= G.scene.page_count) continue;
        const VgCluster *cl = vgeo_scene_cluster(&G.scene, packed >> VG_CLUSTER_BITS, packed & ((1u << VG_CLUSTER_BITS) - 1));
        float p[3], q[4], s;
        vgeo_instance_decode(vin, p, q, &s);
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
    if (G.pal_ok) v.slot_material_base = G.slot_mat_base;
    view_foliage(&v);
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

// This frame's Hi-Z from the pass 1 visibility buffer and the static legacy occluders' depth
// (render.c vgeo_occluders: walls count in pass 2 too), for pass 2 occlusion.
static void exec_hiz_cur(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkImageView mips[16];
    u32 n = DG_MIN(fg_mips(G.hiz_cur), 16u);
    for (u32 m = 0; m < n; m++) mips[m] = fg_mip_view(G.hiz_cur, m);
    vg_hiz_record_merged(&G.cull, cmd, R->frame_slot, fg_view(G.depth), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, mips, n, R->hdr.w, R->hdr.h);
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
    G.pass2_pending = false;
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
    if (G.hiz_cur != FG_NONE) fg_reference(p, G.hiz_cur, FG_SAMPLED);   // bound now, written below
    p = fg_pass("vgeo_raster", exec_raster1, NULL);
    G.pass2_pending = true;
}

bool vkg_wants_occluders(void) { return active() && G.hiz_cur != FG_NONE && G.pass2_pending; }

// After render.c's vgeo_occluders pass (static legacy depth in depth_res): this frame's Hi-Z, pass 2.
void vkg_graph_cull_pass2(int depth_res) {
    if (!G.pass2_pending) return;
    G.pass2_pending = false;
    int p;
    if (G.hiz_cur != FG_NONE) {
        p = fg_pass("vgeo_hiz_cur", exec_hiz_cur, NULL);
        fg_read(p, depth_res, FG_SAMPLED);
        fg_write(p, G.hiz_cur, FG_STORAGE);
    }
    p = fg_pass("vgeo_pass2", exec_pass2, NULL);
    if (G.hiz_cur != FG_NONE) fg_read(p, G.hiz_cur, FG_SAMPLED);
}

static void exec_check(VkCommandBuffer cmd, void *u) {
    (void)u;
    u32 w = R->hdr.w, h = R->hdr.h;
    VkDeviceSize n = (VkDeviceSize)w * h;
    if (G.check.depth.size < n * 4) {
        vgc_buffer_free(&G.ctx, &G.check.depth);
        vgc_buffer_free(&G.ctx, &G.check.vel);
        vgc_buffer_free(&G.ctx, &G.check.vis);
        G.check.depth = vgc_buffer(&G.ctx, n * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
        G.check.vel = vgc_buffer(&G.ctx, n * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
        G.check.vis = vgc_buffer(&G.ctx, n * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VGM_READBACK);
    }
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w;
    r.imageExtent.height = h;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, fg_image(G.depth), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.check.depth.buffer, 1, &r);
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyImageToBuffer(cmd, fg_image(G.check.velocity), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.check.vel.buffer, 1, &r);
    VkBufferCopy bc = { 0, 0, n * 8 };
    vkCmdCopyBuffer(cmd, G.raster.vis.buffer, G.check.vis.buffer, 1, &bc);
    G.check.slot = R->frame_slot;
    G.check.w = w;
    G.check.h = h;
    G.check.inv_raster_vp = mat4_inverse(mat4_mul(R->proj_j, R->view));
    G.check.vp_nj = R->view_proj_nj;
    G.check.prev_vp_nj = R->prev_view_proj_nj;
    G.check.state = 2;
}

// Depth export: every pixel the visibility buffer covers holds exactly its depth (or a nearer legacy
// one); velocity there equals the reprojection of that depth (current / previous unjittered views).
static void check_analyse(void) {
    vmaInvalidateAllocation(G.ctx.vma, G.check.depth.alloc, 0, VK_WHOLE_SIZE);
    vmaInvalidateAllocation(G.ctx.vma, G.check.vel.alloc, 0, VK_WHOLE_SIZE);
    vmaInvalidateAllocation(G.ctx.vma, G.check.vis.alloc, 0, VK_WHOLE_SIZE);
    const float *dep = (const float *)G.check.depth.mapped;
    const u16 *vel = (const u16 *)G.check.vel.mapped;
    const u64 *vis = (const u64 *)G.check.vis.mapped;
    u32 w = G.check.w, h = G.check.h, covered = 0, exact = 0, behind = 0, legacy = 0, vbad = 0;
    double vmax = 0, vsum = 0, vmag = 0;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u64 e = vis[(size_t)y * w + x];
            if (!e) continue;
            covered++;
            u32 hi = (u32)(e >> 32);
            float vd;
            memcpy(&vd, &hi, 4);
            vd = 1.0f - vd;
            float d = dep[(size_t)y * w + x];
            if (d > vd) { behind++; continue; }   // the export missed it
            if (d < vd) { legacy++; continue; }   // a legacy draw is nearer
            exact++;
            vec4 ndc = v4(((float)x + 0.5f) / (float)w * 2.0f - 1.0f, ((float)y + 0.5f) / (float)h * 2.0f - 1.0f, d, 1.0f);
            vec4 wp = mat4_mul_v4(G.check.inv_raster_vp, ndc);
            vec3 p = v3(wp.x / wp.w, wp.y / wp.w, wp.z / wp.w);
            vec4 c = mat4_mul_v4(G.check.vp_nj, v4(p.x, p.y, p.z, 1)), q = mat4_mul_v4(G.check.prev_vp_nj, v4(p.x, p.y, p.z, 1));
            float ex = (c.x / c.w - q.x / q.w) * 0.5f, ey = (c.y / c.w - q.y / q.w) * 0.5f;
            float gx = vgeo_half_to_float(vel[((size_t)y * w + x) * 2]), gy = vgeo_half_to_float(vel[((size_t)y * w + x) * 2 + 1]);
            double err = sqrt((double)(gx - ex) * (gx - ex) + (double)(gy - ey) * (gy - ey));
            vsum += err;
            vmag += sqrt((double)ex * ex + (double)ey * ey);
            if (err > vmax) vmax = err;
            if (err > 1e-3 + 0.01 * sqrt((double)ex * ex + (double)ey * ey)) vbad++;
        }
    log_info("vgeo_check_resolve: %u covered pixels: %u exact depth, %u under a nearer legacy surface, %u MISSING (depth behind); velocity vs reprojection: "
             "max %.2e, mean %.2e (UV; mean |velocity| %.2e), %u over tolerance -> %s",
             covered, exact, legacy, behind, vmax, exact ? vsum / exact : 0.0, exact ? vmag / exact : 0.0, vbad, behind == 0 && vbad == 0 && exact > 0 ? "PASS" : "FAIL");
    G.check.state = 0;
}

static void cmd_vgeo_check_resolve(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!active()) { log_info("vgeo_check_resolve: vgeo inactive"); return; }
    G.check.state = 1;
}

void vkg_graph_hiz(int depth_res, int velocity_res) {
    if (active() && G.check.state == 1 && velocity_res < 0) {
        log_info("vgeo_check_resolve: no velocity target this frame (Engine Fidelity level without temporal passes): skipped");
        G.check.state = 0;
    }
    if (active() && G.check.state == 1) {   // the raster runs in this graph (vgeo_raster)
        G.check.velocity = velocity_res;
        int c = fg_pass("vgeo_check", exec_check, NULL);
        fg_read(c, depth_res, FG_TRANSFER_SRC);
        fg_read(c, velocity_res, FG_TRANSFER_SRC);
    }
    if (!active() || G.hiz == FG_NONE || G.occlusion->i == 0) return;
    int p = fg_pass("vgeo_hiz", exec_hiz, NULL);
    if (p < 0) return;
    fg_read(p, depth_res, FG_SAMPLED);
    fg_write(p, G.hiz, FG_STORAGE);
}

// ------------------------------------------------------------------ sun shadow cascades

// Before the shadow pass (outside rendering): one orthographic vgeo view per cascade (slots 1-3),
// frustum + LOD only, every cluster to the HW list, and its draw args.
// Start of the frame's vgeo work (before every cull): last time's counters / debug readback of this
// frame slot (its fence has been waited), the visibility buffer size, page streaming.
static void frame_begin(VkCommandBuffer cmd) {
    u32 frame = R->frame_slot;
    vg_cull_prepare(&G.cull);   // after a commit (characters, level): before any of this frame's vgeo commands
    foliage_wind_frame();
    if (G.sqp && G.sqp_written[frame]) {   // this slot's fence has been waited: last time's shadow timings
        u64 ts[8];
        if (vkGetQueryPoolResults(R->device, G.sqp, frame * 8, 8, sizeof(ts), ts, sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            float k = R->gpu_props.limits.timestampPeriod * 1e-6f;
            G.shadow_ms[0] = (float)(ts[1] - ts[0]) * k;
            G.shadow_ms[1] = (float)((ts[3] - ts[2]) + (ts[5] - ts[4]) + (ts[7] - ts[6])) * k;
        }
        G.sqp_written[frame] = false;
    }
    if (G.check.state == 2 && G.check.slot == frame) check_analyse();   // this slot's fence has been waited
    tex_update();
    vg_cull_collect(&G.cull, frame);
    debug_spheres(frame);
    vg_raster_resize(&G.raster, R->hdr.w, R->hdr.h);
    if (G.stream_ok) {
        VgStreamConfig sc = { (u32)G.pool_mb->i, (u32)G.pages_per_frame->i };
        vg_stream_set_config(&G.stream, &sc);
        vg_stream_update(&G.stream, cmd, frame);
    }
    skin_record(cmd);
    // foliage: placement chunk changes, the displacement buffer around the camera
    vgeo_scene_placement_record(&G.scene, cmd, frame);
    if (G.disp_ok) {
        double now = (double)SDL_GetTicksNS() * 1e-9;
        float dt = G.disp_time > 0 ? (float)fmin(now - G.disp_time, 0.1) : 0.0f;
        G.disp_time = now;
        if (G.push_test > 0) {   // foliage_push_test: 6 pushes on a 4 m ring circling the camera
            G.push_test -= dt;
            for (int k = 0; k < 6; k++) {
                float a = (float)now * 1.3f + (float)k * 1.0471976f;
                float p[3] = { R->cam.pos.x + cosf(a) * 4.0f, R->cam.pos.y, R->cam.pos.z + sinf(a) * 4.0f };
                vg_displace_push(&G.disp, p, 1.5f, 3.0f);
            }
        }
        float cam[3] = { R->cam.pos.x, R->cam.pos.y, R->cam.pos.z };
        vg_displace_record(&G.disp, cmd, frame, cam, dt);
    }
    // the wind field's tile (the game updated it this frame), live-tuned wind materials
    if (G.wind_ok) vg_wind_record(&G.wind, cmd, frame, R->weather.wind_texels, R->weather.wind_ox, R->weather.wind_oz, R->weather.wind_texel);
    if (G.wind_mat_dirty && G.scene.b_materials.buffer && G.scene.material_count) {
        VkDeviceSize n = sizeof(VgMaterial) * (VkDeviceSize)G.scene.material_count;
        for (VkDeviceSize o = 0; o < n; o += 65536) vkCmdUpdateBuffer(cmd, G.scene.b_materials.buffer, o, DG_MIN(n - o, 65536), (const u8 *)G.scene.materials + o);
        VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        G.wind_mat_dirty = false;
    }
}

void vkg_record_shadow_cull(VkCommandBuffer cmd) {
    G.shadow_culled = false;
    if (!active()) return;
    frame_begin(cmd);
    if (!G.shadows->i || R->r_shadow_quality->i <= 0) return;
    u32 frame = R->frame_slot;
    u32 tile = R->shadow_size / 2;
    if (G.sqp) {
        vkCmdResetQueryPool(cmd, G.sqp, frame * 8, 8);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.sqp, frame * 8);
    }
    u32 ncasc = (u32)DG_CLAMP(R->r_shadow_cascades->i, 1, 3);   // Engine Fidelity: cascades drawn this frame
    for (u32 k = 0; k < ncasc; k++) {
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
                        VGV_ORTHO | VGV_NO_OCCLUSION | VGV_NO_CONE | VGV_NO_SKINNED);
        memcpy(v.raster_view_proj, R->shadow_vps[k].m, 64);
        view_foliage(&v);   // the same WPO, density and fade as the main view (cam_pos is the camera)
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
    if (G.sqp) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.sqp, frame * 8 + 1);
    // Cascades not drawn this frame: empty intervals keep the readback complete.
    for (u32 k = ncasc; k < 3 && G.sqp; k++) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.sqp, frame * 8 + 2 + k * 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.sqp, frame * 8 + 3 + k * 2);
    }
    G.shadow_culled = true;
}

// Inside the shadow pass, cascade k (viewport / scissor / depth bias / frame set already set).
void vkg_draw_shadow(VkCommandBuffer cmd, u32 k) {
    if (!G.shadow_culled) return;
    if (G.sqp) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.sqp, R->frame_slot * 8 + 2 + k * 2);
    VkDescriptorSet set = vg_cull_set(&G.cull, R->frame_slot, 1 + k);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.p_shadow);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.gfx_layout, 3, 1, &set, 0, NULL);
    PushLit pc;
    memset(&pc, 0, sizeof(pc));
    pc.flags[3] = k;
    vkCmdPushConstants(cmd, G.gfx_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
    vkCmdDrawIndirect(cmd, G.cull.v[1 + k].counters.buffer, VGA_DRAW_HW * 16, 1, 16);
    // masked clusters (the cascade view's SW half): alpha-tested
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.p_shadow_masked);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.gfx_layout, 3, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, G.gfx_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushLit), &pc);
    vkCmdDrawIndirect(cmd, G.cull.v[1 + k].counters.buffer, VGA_SW * 16, 1, 16);
    if (G.sqp) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.sqp, R->frame_slot * 8 + 3 + k * 2);
        if (k == 2) G.sqp_written[R->frame_slot] = true;
    }
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
    const VgBuilt *src = NULL;
    char file_path[512] = "";
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
        src = vgeo_file_get(platform_data_path(pl->mesh, file_path, sizeof(file_path)));
        if (!src) { log_warn("vgeo: %s missing or invalid (generated at build time by tools/build_vgeo)", pl->mesh); return ~0u; }
    } else {
        model = assets_model(assets_model_id(pl->mesh));
        if (!model || !model->vgeo) { log_warn("vgeo: model '%s' has no vgeo data (\"vgeo\": true in data/assets.json)", pl->mesh); return ~0u; }
        src = model->vgeo;
    }
    // .vgeo files stream their pages from the file (the pool holds what the views need)
    u32 mesh = file_path[0] ? vgeo_scene_add_mesh_file(&G.scene, src, file_path) : vgeo_scene_add_mesh(&G.scene, src);
    fallback_register(mesh, src, true);
    if (file_path[0]) vgeo_file_drop_pages(file_path);   // pages stream from the file from now on
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
            m.flags = mm->flags & (MAT_TEXTURED | MAT_TRIPLANAR | MAT_GRID | MAT_CHECKER);
            m.tex_scale = mm->tex_scale > 0 ? mm->tex_scale : 1.0f;
            if ((mm->flags & MAT_TEXTURED) && mm->texture) { m.albedo_tex = mm->texture; tex_use(mm->texture); }
            // glTF maps (docs/PBR.md): the resolve samples them through the texture table.
            const u32 map_flag[4] = { MAT_MR_TEX, MAT_NORMAL_TEX, MAT_OCCLUSION_TEX, MAT_EMISSIVE_TEX };
            const TextureId map_tex[4] = { mm->mr_tex, mm->normal_tex, mm->occlusion_tex, mm->emissive_tex };
            u32 *map_dst[4] = { &m.mr_tex, &m.normal_tex, &m.occlusion_tex, &m.emissive_tex };
            for (int t = 0; t < 4; t++)
                if ((mm->flags & map_flag[t]) && map_tex[t]) { m.flags |= map_flag[t]; *map_dst[t] = map_tex[t]; tex_use(map_tex[t]); }
            m.normal_scale = mm->normal_scale;
            m.occlusion_strength = mm->occlusion_strength;
            m.emissive_rgb = mm->emissive_rgb;
            // alphaMode MASK / doubleSided: the vgeo masked / two-sided paths (every raster path and the shadows)
            if (mm->flags & MAT_ALPHA_MASK) { m.flags |= VGM_MASKED; m.mask_tex = m.albedo_tex; m.alpha_cutoff = mm->alpha_cutoff; }
            if (mm->flags & (MAT_TWO_SIDED | MAT_ALPHA_MASK)) m.flags |= VGM_TWO_SIDED | MAT_TWO_SIDED;
            const ModelFoliage *fo = &model->foliage;
            if (fo->enabled) {   // manifest "foliage": the vgeo material flags and WPO parameters
                m.flags |= VGM_FOLIAGE | (fo->masked ? VGM_MASKED : 0) | (fo->two_sided ? VGM_TWO_SIDED : 0) | (fo->dither ? VGM_DITHER_FADE : 0);
                m.mask_tex = m.albedo_tex;
                m.alpha_cutoff = fo->alpha_cutoff;
                m.wpo = (u32)fo->wpo | (fo->displace ? WPO_DISPLACE : 0);
                memcpy(m.wpo_params, fo->wpo_params, 16);
                m.fade_start = fo->fade_start;
                m.fade_end = fo->fade_end;
                m.shadow_max_dist = fo->shadow_max_dist;
            }
        }
        u32 idx = vgeo_scene_add_material(&G.scene, &m);
        if (k == 0) base = idx;
    }
    vgeo_scene_set_mesh_materials(&G.scene, mesh, base);
    log_info("vgeo: level mesh %s: %u clusters, %u pages, %u tris", pl->mesh, src->hdr.cluster_count, src->hdr.page_count, src->stats.input_tris);
    dg_strcpy(G.named[G.named_count].name, sizeof(G.named[0].name), pl->mesh);
    G.named[G.named_count++].mesh = mesh;
    return mesh;
}

void render_vgeo_set_level(const RenderVgeoPlacement *p, int count) {
    if (!G.ok) return;
    vgeo_scene_clear_instances(&G.scene);
    skin_readd_pools();
    u32 total = 0;
    for (int i = 0; i < count; i++) {
        const RenderVgeoPlacement *pl = &p[i];
        u32 mesh = level_mesh(pl);
        if (mesh == ~0u) continue;
        u32 cap = (pl->grid[0] ? pl->grid[0] : 1) * (pl->grid[1] ? pl->grid[1] : 1);
        VgPlacementXf *xf = (VgPlacementXf *)malloc(sizeof(VgPlacementXf) * cap);
        float pp[3] = { pl->pos.x, pl->pos.y, pl->pos.z };
        u32 n = vgeo_placement_expand(pp, pl->yaw, pl->scale, pl->scale_jitter, pl->spacing, pl->grid[0], pl->grid[1], pl->seed, xf, cap);
        for (u32 k = 0; k < n; k++) vgeo_scene_add_instance(&G.scene, mesh, xf[k].pos, xf[k].rot, xf[k].scale);
        total += n;
        free(xf);
    }
    if (G.stream_ok) {   // the pool budget applies at the upload
        VgStreamConfig sc = { (u32)G.pool_mb->i, (u32)G.pages_per_frame->i };
        vg_stream_set_config(&G.stream, &sc);
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

// Compared after the Engine Fidelity bounds (docs/ENGINE_FIDELITY.md): Low scales the era's error
// thresholds, caps its foliage; the era still reads back as itself.
static bool era_eq(int f, float v, float era_v) { return fabsf(v - fid_clamp_current(f, era_v)) < 1e-4f; }

static int era_detect(void) {
    for (int e = 0; e < 4; e++) {
        const EraDef *d = &k_eras[e];
        if (era_eq(FF_VGEO, (float)G.r_vgeo->i, (float)d->vgeo) && era_eq(FF_VGEO_ERROR_PX, G.error_px->f, d->error_px) &&
            era_eq(FF_VGEO_SHADOW_ERROR_PX, G.shadow_error_px->f, d->shadow_px) && era_eq(FF_VGEO_LOD_PX, G.lod_px->f, d->lod_px) &&
            era_eq(FF_FOLIAGE_DENSITY, G.foliage_density->f, d->foliage))
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
    fid_enforce_all();   // the era's capability matrix (what it lacks is unavailable at every level)
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
    // Era-owned Engine Fidelity features take their defaults from this table.
    for (int e = 0; e < 4; e++) {
        fid_set_era_value(e, FF_VGEO, (float)k_eras[e].vgeo);
        fid_set_era_value(e, FF_VGEO_ERROR_PX, k_eras[e].error_px);
        fid_set_era_value(e, FF_VGEO_SHADOW_ERROR_PX, k_eras[e].shadow_px);
        fid_set_era_value(e, FF_VGEO_LOD_PX, k_eras[e].lod_px);
        fid_set_era_value(e, FF_FOLIAGE_DENSITY, k_eras[e].foliage);
    }
    Cvar *dep[5] = { G.r_vgeo, G.error_px, G.shadow_error_px, G.lod_px, G.foliage_density };
    for (int i = 0; i < 5; i++) dep[i]->on_change = on_era_value;
    on_era_value(NULL);
}

// Keeps a CPU copy of a mesh's exported LODs (the fallback uploads them when first drawn).
static void fallback_register(u32 mesh, const VgBuilt *b, bool persistent) {
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
    f->own = !persistent;
    if (persistent) {
        f->verts = b->lod_vertices;
        f->indices = b->lod_indices;
    } else {
        VgLodVertex *v = (VgLodVertex *)malloc(sizeof(VgLodVertex) * (nv + 1));
        u32 *ix = (u32 *)malloc(sizeof(u32) * (ni + 1));
        memcpy(v, b->lod_vertices, sizeof(VgLodVertex) * nv);
        memcpy(ix, b->lod_indices, sizeof(u32) * ni);
        f->verts = v;
        f->indices = ix;
    }
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
// Foliage in the fallback / local light shadows (CPU, per instance): drawn when its density / fade
// keeps it (vgeo_foliage_alpha, the same hash and distances as the GPU); masked meshes are not (the
// legacy pipelines have no alpha test).
static bool fallback_keep(const VgInstance *in, const VgMesh *m) {
    if (m->fflags & VGMF_MASKED) return false;
    if (!(m->fflags & VGMF_FOLIAGE)) return true;
    float dx = in->pos[0] - R->cam.pos.x, dy = in->pos[1] - R->cam.pos.y, dz = in->pos[2] - R->cam.pos.z;
    return vgeo_foliage_alpha(m, in->flags_phase, sqrtf(dx * dx + dy * dy + dz * dz), G.foliage_density->f) > 0;
}

void vkg_submit_fallback(void) {
    if (!G.ok || G.r_vgeo->i || !vgeo_scene_gpu_instances(&G.scene) || !G.fb) return;
    bool era2010 = G.era && G.era->i == 3;
    float ps = (float)R->hdr.h / (2.0f * tanf(R->cam.fov_deg * DG_DEG2RAD * 0.5f));
    u32 total = vgeo_scene_gpu_instances(&G.scene);
    for (u32 i = 0; i < total; i++) {
        const VgInstance *in = vgeo_scene_instance(&G.scene, i);
        if (i >= G.scene.instance_count) {   // placement region: live instances only
            u32 ri = i - G.scene.instance_count;
            if (in->cell >= G.scene.pl.cell_cap) continue;
            const VgCell *cl = &G.scene.pl.cells[in->cell];
            if (!cl->count || ri < cl->first || ri >= cl->first + cl->count) continue;
        }
        if (in->mesh >= G.fb_cap || !G.fb[in->mesh].lod_count) continue;
        struct FallbackMesh *f = &G.fb[in->mesh];
        float p[3], q[4], s;
        vgeo_instance_decode(in, p, q, &s);
        const VgMesh *m = &G.scene.meshes[in->mesh];
        if (!fallback_keep(in, m)) continue;
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
        // glTF maps (docs/PBR.md): the same textures as the resolve (the albedo only with maps, so meshes
        // drawn untextured here so far stay so)
        // (renderer texture ids only: module textures, e.g. the tree bark maps, are not legacy textures)
        if ((vm->flags & MAT_PBR_MAPS) && vm->albedo_tex < VG_TEX_GAME && vm->mr_tex < VG_TEX_GAME && vm->normal_tex < VG_TEX_GAME && vm->occlusion_tex < VG_TEX_GAME &&
            vm->emissive_tex < VG_TEX_GAME) {
            di.mat.flags = vm->flags & (MAT_PBR_MAPS | MAT_TEXTURED | MAT_TWO_SIDED);
            di.mat.texture = vm->albedo_tex;
            di.mat.mr_tex = vm->mr_tex;
            di.mat.normal_tex = vm->normal_tex;
            di.mat.occlusion_tex = vm->occlusion_tex;
            di.mat.emissive_tex = vm->emissive_tex;
            di.mat.normal_scale = vm->normal_scale;
            di.mat.occlusion_strength = vm->occlusion_strength;
            di.mat.emissive_rgb = vm->emissive_rgb;
        }
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

// The devroom foliage yard (foliage_test_scatter, x 60..380, z 80..400): from the wall into the yard,
// low over the grass, a turn at the far end, back higher up.
static const float k_bench_foliage[][5] = {
    { 220, 1.8f, 82, 180, -4 }, { 200, 1.8f, 160, 200, -6 }, { 150, 2.5f, 240, 250, -8 }, { 120, 6, 330, 160, -12 },
    { 220, 3, 380, 90, -6 },    { 330, 2, 300, 20, -5 },     { 300, 12, 180, -30, -20 }, { 220, 4, 100, 0, -10 }, { 220, 1.8f, 82, 180, -4 },
};

static float catmull(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

// Called by render_set_camera: replaces the camera while the bench runs.
static bool foliage_check_camera(RenderCamera *cam);

void vkg_bench_camera(RenderCamera *cam) {
    if (foliage_check_camera(cam)) return;
    if (G.bench.frames <= 0) return;
    const float (*path)[5] = G.bench.path ? k_bench_foliage : k_bench_path;
    int n = G.bench.path ? (int)(sizeof(k_bench_foliage) / sizeof(k_bench_foliage[0])) : (int)(sizeof(k_bench_path) / sizeof(k_bench_path[0]));
    float u = (float)G.bench.frame / (float)G.bench.frames * (float)(n - 1);
    int i = DG_CLAMP((int)u, 0, n - 2);
    float t = u - (float)i;
    float v[5];
    for (int k = 0; k < 5; k++) v[k] = catmull(path[i > 0 ? i - 1 : 0][k], path[i][k], path[i + 1][k], path[i + 2 < n ? i + 2 : n - 1][k], t);
    cam->pos = v3(v[0], v[1], v[2]);
    cam->yaw = -v[3] * DG_DEG2RAD;
    cam->pitch = v[4] * DG_DEG2RAD;
    cam->roll = 0;
}

static void foliage_check_frame(void);

// Once per frame (after fg_execute's timings of the previous frame are in).
void vkg_bench_frame(void) {
    foliage_check_frame();
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
        fprintf(G.bench.csv, ",vgeo_shadow_cull,vgeo_shadow_draw,vis_clusters,vis_tris,impostors,stream_cpu_ms,stream_loaded,stream_evicted,stream_waiting\n");
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
    // foliage budget view: every vgeo pass + the vgeo part of the sun shadows
    double fol = 0;
    for (int k = 0; k < G.bench.npass; k++)
        if (!strncmp(G.bench.names[k], "vgeo_", 5)) {
            for (int i = 0; i < fg_pass_count(); i++)
                if (!strcmp(fg_pass_name(i), G.bench.names[k])) fol += fg_pass_ms(i);
        }
    fol += G.shadow_ms[0] + G.shadow_ms[1];
    if (G.bench.frame >= 10) {
        G.bench.shadow_sum[0] += G.shadow_ms[0];
        G.bench.shadow_sum[1] += G.shadow_ms[1];
        if (fol > G.bench.worst_foliage) G.bench.worst_foliage = fol;
    }
    fprintf(G.bench.csv, ",%.4f,%.4f,%u,%u,%u", G.shadow_ms[0], G.shadow_ms[1], hc ? c.n[VGN_HW] + c.n[VGN_SW] : 0, hc ? c.n[VGN_TRIS] : 0, hc ? c.n[VGN_IMPOSTORS] : 0);
    const VgStreamStats *ss = G.stream_ok ? vg_stream_stats(&G.stream) : NULL;
    fprintf(G.bench.csv, ",%.3f,%u,%u,%u\n", ss ? ss->cpu_ms : 0.0f, ss ? ss->loaded : 0, ss ? ss->evicted : 0, ss ? ss->waiting : 0);
    if (G.bench.frame >= 10 && total > G.bench.worst_total) G.bench.worst_total = total;
    if (++G.bench.frame >= G.bench.frames) {
        int n = G.bench.frames - 10;
        log_info("vgeo_bench: %d frames, CPU %.2f ms avg; GPU ms avg per pass (frames 10+):", G.bench.frames, G.bench.cpu_sum / G.bench.frames);
        for (int k = 0; k < G.bench.npass; k++)
            if (!strncmp(G.bench.names[k], "vgeo_", 5) || !strcmp(G.bench.names[k], "prepass") || !strcmp(G.bench.names[k], "opaque") || !strcmp(G.bench.names[k], "final"))
                log_info("  %-16s %.3f", G.bench.names[k], G.bench.sum[k] / (n > 0 ? n : 1));
        log_info("  vgeo_shadow_cull %.3f, vgeo_shadow_draw %.3f (sun cascades, outside the frame graph)", G.bench.shadow_sum[0] / (n > 0 ? n : 1),
                 G.bench.shadow_sum[1] / (n > 0 ? n : 1));
        log_info("  worst vgeo + prepass + opaque frame: %.3f ms; worst vgeo passes + vgeo shadows: %.3f ms", G.bench.worst_total, G.bench.worst_foliage);
        fclose(G.bench.csv);
        G.bench.csv = NULL;
        G.bench.frames = 0;
    }
}

// vgeo_bench [frames=600] [csv=vgeo_bench.csv] [vgeo_test|foliage]
static void cmd_vgeo_bench(int argc, const char **argv) {
    if (G.bench.csv) fclose(G.bench.csv);
    memset(&G.bench, 0, sizeof(G.bench));
    G.bench.path = argc > 3 && !strcmp(argv[3], "foliage");
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
    if (!G.ok || !G.r_vgeo->i || !G.shadows->i || !vgeo_scene_gpu_instances(&G.scene) || !G.fb) return 0;
    vec3 lc = v3(sphere.x, sphere.y, sphere.z);
    float px_scale = (float)tile_px * 0.5f;   // 90 degree face: px per metre at distance 1
    u32 n = 0, bound = 0;
    u32 ncells = vgeo_scene_gpu_cells(&G.scene);
    for (u32 ci = 0; ci < ncells; ci++) {
        VgCell tmp;
        const VgCell *cl = vgeo_scene_gpu_cell(&G.scene, ci, &tmp);
        if (!cl || !cl->count) continue;
        vec3 mn = v3(cl->min[0], cl->min[1], cl->min[2]), mx = v3(cl->max[0], cl->max[1], cl->max[2]);
        vec3 q = v3_max(mn, v3_min(lc, mx));
        if (v3_len2(v3_sub(q, lc)) > sphere.w * sphere.w) continue;
        for (u32 i = cl->first; i < cl->first + cl->count; i++) {
            const VgInstance *in = vgeo_scene_instance(&G.scene, i);
            if (!in || in->mesh >= G.fb_cap || !G.fb[in->mesh].lod_count) continue;
            struct FallbackMesh *f = &G.fb[in->mesh];
            const VgMesh *m = &G.scene.meshes[in->mesh];
            if (!fallback_keep(in, m)) continue;
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

// ------------------------------------------------------------------ skinned characters

#define SKIN_SLOTS 2048u
#define SKIN_POOL0 4u

// Worker: builds queued skinned meshes (SkinVertex copy -> VgSource with joints / weights).
static int SDLCALL skin_worker(void *u) {
    (void)u;
    for (;;) {
        SDL_WaitSemaphore(G.sem);
        SDL_LockMutex(G.mutex);
        if (G.quit) { SDL_UnlockMutex(G.mutex); return 0; }
        int ri = G.queue_head != G.queue_tail ? G.queued[G.queue_head++ % 128] : -1;
        SDL_UnlockMutex(G.mutex);
        if (ri < 0) continue;
        struct SkinRec *r = &G.skin[ri];
        GpuMesh *gm = &R->meshes[r->mesh];
        u32 vn = gm->vertex_count, in = gm->index_count;
        float *pos = (float *)malloc(sizeof(float) * 3 * vn), *nrm = (float *)malloc(sizeof(float) * 3 * vn), *uv = (float *)malloc(sizeof(float) * 2 * vn);
        u8 *jo = (u8 *)malloc(4u * vn), *we = (u8 *)malloc(4u * vn);
        u32 *col = (u32 *)malloc(sizeof(u32) * vn);
        bool bad = false;
        for (u32 i = 0; i < vn; i++) {
            const SkinVertex *v = &gm->cpu_verts[i];
            memcpy(&pos[i * 3], &v->pos, 12);
            memcpy(&nrm[i * 3], &v->normal, 12);
            memcpy(&uv[i * 2], &v->uv, 8);
            memcpy(&jo[i * 4], v->joints, 4);
            memcpy(&we[i * 4], v->weights, 4);
            col[i] = v->color;
            for (int k = 0; k < 4; k++) bad |= v->joints[k] >= VGEO_MAX_BONES;
        }
        VgSource src;
        memset(&src, 0, sizeof(src));
        src.vertex_count = vn;
        src.pos = pos;
        src.normal = nrm;
        src.uv = uv;
        src.color = col;
        src.index_count = in;
        src.indices = gm->cpu_indices;
        src.joints = jo;
        src.weights_u8 = we;
        VgBuildOptions opt;
        memset(&opt, 0, sizeof(opt));
        opt.threads = 2;
        char err[128];
        bool ok = !bad && vgeo_build(&src, &opt, &r->built, err, sizeof(err));
        if (ok) vgeo_bone_bounds(&r->built, r->bones);
        free(pos); free(nrm); free(uv); free(jo); free(we); free(col);
        SDL_LockMutex(G.mutex);
        r->state = ok ? 2 : -1;
        SDL_UnlockMutex(G.mutex);
    }
}

static void skin_init(void) {
    if (!vg_palettes_init(&G.pal, &G.ctx, SKIN_SLOTS)) { log_warn("vgeo: palettes unavailable (characters stay legacy)"); return; }
    vg_palettes_attach(&G.pal, &G.scene);
    VgMaterial def;
    memset(&def, 0, sizeof(def));
    def.color[0] = def.color[1] = def.color[2] = def.color[3] = 1.0f;
    def.roughness = 0.6f;
    def.tex_scale = 1.0f;
    for (u32 i = 0; i < SKIN_SLOTS; i++) {
        u32 idx = vgeo_scene_add_material(&G.scene, &def);
        if (i == 0) G.slot_mat_base = idx;
        G.slot_mat[i] = def;
    }
    G.mutex = SDL_CreateMutex();
    G.sem = SDL_CreateSemaphore(0);
    G.worker = SDL_CreateThread(skin_worker, "vgeo_skin_build", NULL);
    G.pal_ok = true;
}

static void skin_shutdown(void) {
    if (!G.pal_ok) return;
    SDL_LockMutex(G.mutex);
    G.quit = true;
    SDL_UnlockMutex(G.mutex);
    SDL_SignalSemaphore(G.sem);
    SDL_WaitThread(G.worker, NULL);
    SDL_DestroySemaphore(G.sem);
    SDL_DestroyMutex(G.mutex);
    for (u32 i = 0; i < G.skin_count; i++)
        if (G.skin[i].state == 2 || G.skin[i].state == 3) vgeo_free(&G.skin[i].built);
    vg_palettes_shutdown(&G.pal);
    G.pal_ok = false;
}

static int skin_rec_of(MeshId mesh, bool create) {
    for (u32 i = 0; i < G.skin_count; i++)
        if (G.skin[i].mesh == mesh) return (int)i;
    if (!create || G.skin_count >= 128 || !R->meshes[mesh].cpu_verts) return -1;
    int ri = (int)G.skin_count++;
    struct SkinRec *r = &G.skin[ri];
    memset(r, 0, sizeof(*r));
    r->mesh = mesh;
    r->state = 1;
    SDL_LockMutex(G.mutex);
    G.queued[G.queue_tail++ % 128] = ri;
    SDL_UnlockMutex(G.mutex);
    SDL_SignalSemaphore(G.sem);
    return ri;
}

// Adds the pool instances of a mesh record (identity transform: the palette is world space).
static void skin_add_pool(struct SkinRec *r) {
    float pos[3] = { 0, 0, 0 }, rot[4] = { 0, 0, 0, 1 };
    for (u32 k = 0; k < r->pool; k++) {
        vgeo_scene_add_skinned_instance(&G.scene, r->vmesh, pos, rot, 1.0f, r->first_slot + k);
        vg_palettes_set_bone_bounds(&G.pal, r->first_slot + k, (const float (*)[4])r->bones);
        vg_palettes_clear(&G.pal, r->first_slot + k);
    }
}

// Built meshes enter the scene; pools that ran out grow (both re-commit the scene: rare, load-time-ish).
static void skin_integrate(void) {
    bool commit = false;
    for (u32 i = 0; i < G.skin_count; i++) {
        struct SkinRec *r = &G.skin[i];
        SDL_LockMutex(G.mutex);
        int st = r->state;
        SDL_UnlockMutex(G.mutex);
        if (st == 2) {
            u32 want = SKIN_POOL0;
            if (G.slots_used + want > SKIN_SLOTS) { r->state = -1; continue; }
            r->vmesh = vgeo_scene_add_mesh(&G.scene, &r->built);
            vgeo_scene_set_mesh_materials(&G.scene, r->vmesh, G.slot_mat_base);   // overridden per slot
            r->first_slot = G.slots_used;
            r->pool = want;
            for (u32 k = 0; k < want; k++) { G.uses[G.slots_used + k].rec = (u16)i; G.uses[G.slots_used + k].object_id = 0; }
            G.slots_used += want;
            skin_add_pool(r);
            r->state = 3;
            commit = true;
            log_info("vgeo: character mesh %u: %u clusters, %u bones, pool of %u", r->mesh, r->built.hdr.cluster_count, r->built.hdr.bone_count, r->pool);
        } else if (st == 3 && r->want_pool > r->pool && G.slots_used < SKIN_SLOTS) {
            // grow: new slots at the end of the slot range (the old ones keep their characters)
            u32 add = DG_MIN(r->want_pool - r->pool, SKIN_SLOTS - G.slots_used);
            float pos[3] = { 0, 0, 0 }, rot[4] = { 0, 0, 0, 1 };
            for (u32 k = 0; k < add; k++) {
                u32 slot = G.slots_used + k;
                vgeo_scene_add_skinned_instance(&G.scene, r->vmesh, pos, rot, 1.0f, slot);
                vg_palettes_set_bone_bounds(&G.pal, slot, (const float (*)[4])r->bones);
                vg_palettes_clear(&G.pal, slot);
                G.uses[slot].rec = (u16)i;
                G.uses[slot].object_id = 0;
            }
            G.slots_used += add;
            r->pool += add;
            commit = true;
        }
    }
    if (commit && !vgeo_scene_commit(&G.scene)) log_error("vgeo: scene upload failed");
}

// Re-adds every character pool after the scene's instances were cleared (level change).
static void skin_readd_pools(void) {
    float pos[3] = { 0, 0, 0 }, rot[4] = { 0, 0, 0, 1 };
    for (u32 slot = 0; slot < G.slots_used; slot++) {
        const struct SkinRec *r = &G.skin[G.uses[slot].rec];
        if (r->state == 3) vgeo_scene_add_skinned_instance(&G.scene, r->vmesh, pos, rot, 1.0f, slot);
    }
}

// Per frame, before the legacy skinned passes are built (render.c): which skinned draws vgeo takes
// (the main view only: shadows, scope / phone views keep the legacy path). routed[i] = 1 for those.
// Per character: a stable slot of its mesh's pool (object id), this frame's palette and material.
u32 vkg_route_skinned(const SkinnedDraw *draws, u32 n, const mat4 *bones, u8 *routed) {
    memset(routed, 0, n);
    if (!G.pal_ok || !G.ok) return 0;
    if (!G.r_vgeo->i || !G.skinned->i) {   // off: no slot keeps drawing its last palette
        bool any = false;
        for (u32 s = 0; s < G.slots_used; s++) any |= G.uses[s].object_id != 0;
        if (any) {
            vg_palettes_begin(&G.pal, R->frame_slot);
            for (u32 s = 0; s < G.slots_used; s++)
                if (G.uses[s].object_id) { vg_palettes_clear(&G.pal, s); G.uses[s].object_id = 0; }
        }
        return 0;
    }
    for (u32 i = 0; i < n; i++)   // queue builds for meshes seen for the first time
        if (!(draws[i].flags & MAT_VIEWMODEL)) skin_rec_of(draws[i].mesh, true);
    skin_integrate();
    if (!G.scene.instance_count) return 0;
    const RenderSight *sg = &R->sight;
    bool scope = sg->kind == RSIGHT_SCOPE_PIP && sg->ads_t >= 0.02f;
    u32 frame_id = ++G.route_frame, routed_n = 0;
    for (u32 k = 0; k < G.skin_count; k++) G.skin[k].need = 0;
    vg_palettes_begin(&G.pal, R->frame_slot);
    for (u32 i = 0; i < n && !scope; i++) {
        const SkinnedDraw *d = &draws[i];
        if ((d->flags & (MAT_VIEWMODEL | MAT_SHADOW_ONLY | MAT_HIGHLIGHT)) || d->fade != 0.0f || !d->object_id) continue;
        u32 bc = d->bone_count >> 20, off = d->bone_count & 0xFFFFFu;
        if (!bc || bc > VGEO_MAX_BONES) continue;
        int ri = skin_rec_of(d->mesh, false);
        if (ri < 0 || G.skin[ri].state != 3) continue;
        struct SkinRec *r = &G.skin[ri];
        r->need++;
        // this character's slot: the one it had (stable: previous palette = velocity), else a free one
        u32 slot = ~0u, free_slot = ~0u;
        for (u32 q = 0; q < G.slots_used; q++) {
            if (G.uses[q].rec != (u16)ri) continue;
            if (G.uses[q].object_id == d->object_id) { slot = q; break; }
            if (free_slot == ~0u && !G.uses[q].object_id) free_slot = q;
        }
        if (slot == ~0u) slot = free_slot;
        if (slot == ~0u) { r->want_pool = DG_MAX(r->want_pool, r->pool * 2); r->want_pool = DG_MAX(r->want_pool, r->need * 2); continue; }   // pool exhausted: grows next frame
        G.uses[slot].object_id = d->object_id;
        G.uses[slot].rec = (u16)ri;
        G.uses[slot].frame = frame_id;
        vg_palettes_set(&G.pal, slot, &bones[off].m[0], bc);
        VgMaterial m = G.slot_mat[slot];
        m.color[0] = d->color.x;
        m.color[1] = d->color.y;
        m.color[2] = d->color.z;
        m.color[3] = 1.0f;
        m.roughness = d->roughness;
        m.metallic = d->metallic;
        m.emissive = d->emissive;
        m.flags = d->flags & (MAT_TEXTURED | MAT_UNLIT);
        m.albedo_tex = (d->flags & MAT_TEXTURED) && d->texture ? d->texture : 0;
        if (m.albedo_tex) tex_use(m.albedo_tex);
        if (memcmp(&m, &G.slot_mat[slot], sizeof(m))) { G.slot_mat[slot] = m; G.slot_mat_dirty[slot] = 1; }
        routed[i] = 1;
        routed_n++;
    }
    // slots no character took this frame stop drawing
    for (u32 s = 0; s < G.slots_used; s++)
        if (G.uses[s].frame != frame_id && G.uses[s].object_id) {
            vg_palettes_clear(&G.pal, s);
            if (G.uses[s].frame + 2 < frame_id) G.uses[s].object_id = 0;   // gone: the slot is free again
        }
    return routed_n;
}

// Inside frame_begin (recording): this frame's palettes and changed slot materials.
static void skin_record(VkCommandBuffer cmd) {
    if (!G.pal_ok) return;
    for (u32 s = 0; s < G.slots_used; s++)
        if (G.slot_mat_dirty[s]) {
            G.scene.materials[G.slot_mat_base + s] = G.slot_mat[s];
            if (G.scene.b_materials.buffer)
                vkCmdUpdateBuffer(cmd, G.scene.b_materials.buffer, sizeof(VgMaterial) * (VkDeviceSize)(G.slot_mat_base + s), sizeof(VgMaterial), &G.slot_mat[s]);
            G.slot_mat_dirty[s] = 0;
        }
    vg_palettes_record(&G.pal, cmd);
}

// ------------------------------------------------------------------ foliage (docs/VGEO.md "Foliage support")

static int foliage_type_of_mesh(u32 mesh) {
    for (u32 i = 0; i < G.ftype_count; i++)
        if (G.ftype[i].mesh == mesh) return (int)i;
    return -1;
}

// ------------------------------------------------------------------ wind tuning (docs/FOLIAGE_WIND.md)

int render_foliage_type_count(void) { return G.ok ? (int)G.ftype_count : 0; }
const char *render_foliage_type_name(int i) { return G.ok && i >= 0 && (u32)i < G.ftype_count ? G.ftype[i].name : NULL; }

static void foliage_commit(void);

// The wind materials of a type's mesh: [*first, *first + count).
static bool type_wind_materials(const char *type, u32 *mesh, u32 *first, u32 *count) {
    if (!G.ok || !type) return false;
    for (u32 i = 0; i < G.ftype_count; i++)
        if (!strcmp(G.ftype[i].name, type)) {
            *mesh = G.ftype[i].mesh;
            *first = G.scene.meshes[*mesh].material_base;
            *count = G.scene.mesh_mat_count[*mesh];
            return true;
        }
    return false;
}

bool render_foliage_wind_get(const char *type, RenderFoliageWind *out) {
    u32 mesh, first, count;
    if (!type_wind_materials(type, &mesh, &first, &count)) return false;
    for (u32 k = first; k < first + count && k < G.scene.material_count; k++) {
        const VgMaterial *mt = &G.scene.materials[k];
        if (mt->wpo == WPO_NONE) continue;
        VgWindParams p;
        vgeo_wind_unpack(mt->wpo_params, mt->wind_params, &p);
        _Static_assert(sizeof(RenderFoliageWind) == sizeof(VgWindParams), "RenderFoliageWind mirrors VgWindParams");
        memcpy(out, &p, sizeof(p));
        return true;
    }
    return false;
}

// Live tuning: every wind material of the type takes the parameters; uploaded with the next frame
// (vkCmdUpdateBuffer). A larger bound than the mesh carries re-commits the scene (the culling spheres).
bool render_foliage_wind_set(const char *type, const RenderFoliageWind *w) {
    u32 mesh, first, count;
    if (!type_wind_materials(type, &mesh, &first, &count)) return false;
    VgWindParams p;
    memcpy(&p, w, sizeof(p));
    bool grow = false;
    for (u32 k = first; k < first + count && k < G.scene.material_count; k++) {
        VgMaterial *mt = &G.scene.materials[k];
        if (mt->wpo == WPO_NONE) continue;
        vgeo_wind_pack(&p, mt->wpo_params, mt->wind_params);
        mt->wpo_max = vgeo_wpo_max(mt);
        grow |= mt->wpo_max > G.scene.meshes[mesh].max_wpo;
    }
    G.wind_mat_dirty = true;
    if (grow) {
        G.need_commit = true;
        foliage_commit();
    }
    return true;
}

// The placement region (1M instances = 32 MB, 65536 cells: an open-world map's trees fill thousands
// of chunks), reserved on first use.
static void foliage_region(void) {
    if (G.scene.pl.inst_cap) return;
    vgeo_scene_reserve_placements(&G.scene, 1u << 20, 65536);
    G.need_commit = true;
}

static void foliage_commit(void) {
    if (!G.need_commit) return;
    G.need_commit = false;
    if (!vgeo_scene_commit(&G.scene)) log_error("vgeo: scene upload failed");
}

// Builds procedural foliage kinds [k0, k1) (vgeo_procgen_foliage.c): mesh, masks, impostor atlas,
// materials, the type table (trunk capsules).
// One kind's CPU work: the mesh / masks (generated serially: the generators share a seed state),
// then the cluster build and the impostor bake (the slow parts) in parallel on the job system.
typedef struct FoliageKindBuild {
    int k;
    VgFoliageMesh fm;
    VgSource src;
    VgBuilt b;
    bool ok, impostor;
    char err[256];
    VgMaterial mats[2];
    u32 nm;
    u8 *masks[2];
    u32 msz[2];             // mask size (px): the card textures load at their own size
    const u8 *mk[2];
    VgImpostorMaterial im[2];
    VgImpostorOptions io;
} FoliageKindBuild;

static void foliage_kind_job(void *ctx, int begin, int end) {
    FoliageKindBuild *kb = (FoliageKindBuild *)ctx;
    for (int i = begin; i < end; i++) {
        VgBuildOptions opt;
        memset(&opt, 0, sizeof(opt));
        kb[i].ok = vgeo_build(&kb[i].src, &opt, &kb[i].b, kb[i].err, sizeof(kb[i].err));
        if (kb[i].ok && kb[i].impostor) vgeo_impostor_bake(&kb[i].b, &kb[i].io);
    }
}

// Card textures drawn by tools/vfxgen/foliage_atlas.py (docs/TEXTURES.md) for the grass tuft and the
// wheat clump: RGBA (sRGB colour, coverage alpha), square. NULL (procedural mask) when missing.
static u8 *foliage_card_file(int kind, u32 material, u32 *size) {
    const char *name = material ? NULL : (kind == VG_FOLIAGE_TUFT ? "grass_blades" : (kind == VG_FOLIAGE_CROP ? "wheat" : NULL));
    if (!name) return NULL;
    char rel[96], path[512];
    snprintf(rel, sizeof(rel), "assets/textures/foliage/%s.png", name);
    int w, h, comp;
    u8 *px = stbi_load(platform_data_path(rel, path, sizeof(path)), &w, &h, &comp, 4);
    if (!px) return NULL;
    if (w != h || w < 16 || w > 2048) { stbi_image_free(px); return NULL; }
    u8 *out = (u8 *)malloc((size_t)w * (size_t)h * 4);
    memcpy(out, px, (size_t)w * (size_t)h * 4);
    stbi_image_free(px);
    *size = (u32)w;
    return out;
}

// Builds procedural foliage kinds [k0, k1) (vgeo_procgen_foliage.c): mesh, masks, impostor atlas
// (atlas px, 8 x 8 frames), materials, the type table (trunk capsules).
static void foliage_build_kinds(int k0, int k1, u32 atlas) {
    const u32 msize = 256;
    u32 levels;
    size_t mb = vg_tex_mip_bytes(msize, msize, &levels);
    u8 *mips = (u8 *)malloc(mb);
    int n = k1 - k0;
    FoliageKindBuild *kb = (FoliageKindBuild *)calloc((size_t)n, sizeof(FoliageKindBuild));
    for (int i = 0; i < n; i++) {
        FoliageKindBuild *q = &kb[i];
        q->k = k0 + i;
        vgeo_gen_foliage(&q->fm, q->k, 11 + (u32)q->k);
        vgeo_foliage_source(&q->fm, &q->src);
        q->nm = vgeo_foliage_materials(q->k, q->mats);
        for (u32 m = 0; m < q->nm; m++) {
            u32 fs = 0;
            u8 *card = foliage_card_file(q->k, m, &fs);
            if (card) {   // grass blades / wheat drawn offline
                q->masks[m] = card;
                q->msz[m] = fs;
                q->mk[m] = card;
                continue;
            }
            q->masks[m] = (u8 *)malloc((size_t)msize * msize * 4);
            q->msz[m] = msize;
            if (vgeo_gen_foliage_mask(q->masks[m], msize, q->k, m, 5)) q->mk[m] = q->masks[m];
        }
        vgeo_foliage_impostor_materials(q->k, q->mk, msize, q->im);
        for (u32 m = 0; m < q->nm; m++)
            if (q->mk[m]) q->im[m].w = q->im[m].h = q->msz[m];
        q->io = (VgImpostorOptions){ atlas, 8, q->im, 2 };
        q->impostor = q->k != VG_FOLIAGE_CROP;   // crops fade out before an impostor would be used
    }
    jobs_parallel_for(n, 1, foliage_kind_job, kb);
    for (int i = 0; i < n && G.ftype_count < 32; i++) {
        FoliageKindBuild *q = &kb[i];
        vgeo_foliage_mesh_free(&q->fm);
        if (!q->ok) { log_error("vgeo: %s build failed: %s", vgeo_foliage_name(q->k), q->err); continue; }
        // alpha masks (module textures)
        for (u32 m = 0; m < q->nm; m++) {
            if (!q->mk[m]) continue;
            u32 sz = q->msz[m], lv = levels;
            u8 *mp = mips;
            if (sz != msize) mp = (u8 *)malloc(vg_tex_mip_bytes(sz, sz, &lv));
            vg_tex_build_mips(q->masks[m], sz, sz, mp);
            u32 t = vg_tex_create(&G.cull.tex, sz, sz, lv, mp, true, true);
            if (mp != mips) free(mp);
            q->mats[m].mask_tex = q->mats[m].albedo_tex = t;
        }
        u32 mesh = vgeo_scene_add_mesh(&G.scene, &q->b);
        fallback_register(mesh, &q->b, false);
        u32 base = 0;
        for (u32 m = 0; m < q->nm; m++) {
            u32 idx = vgeo_scene_add_material(&G.scene, &q->mats[m]);
            if (!m) base = idx;
        }
        vgeo_scene_set_mesh_materials(&G.scene, mesh, base);
        if (q->impostor) vgeo_foliage_impostor_upload(&G.scene, &G.cull.tex, mesh, &q->b);
        struct FoliageType *ft = &G.ftype[G.ftype_count++];
        dg_strcpy(ft->name, sizeof(ft->name), vgeo_foliage_name(q->k));
        ft->mesh = mesh;
        ft->capsule = vgeo_foliage_capsule(q->k, &ft->cap_r, &ft->cap_h);   // the bush / tree trunks
        ft->role = -1;   // ground cover, not a tree role
        ft->weight = 0;
        log_info("vgeo: foliage type %s: %u tris, %u clusters, %u levels, impostor %s", ft->name, q->b.stats.input_tris, q->b.hdr.cluster_count, q->b.hdr.level_count,
                 q->impostor ? "8x8 frames" : "none");
        vgeo_free(&q->b);
    }
    for (int i = 0; i < n; i++) {
        free(kb[i].masks[0]);
        free(kb[i].masks[1]);
    }
    free(kb);
    free(mips);
}

// Imported trees (vgeo_tree.h, docs/VGEO.md "Tree models"): the species of the roles in role_mask not
// loaded yet, from the build's assets/vgeo/trees (else built into assets/cache) on the job system;
// textures shared by file, masks = the albedo alpha. Returns the species registered.
typedef struct TreeBuild { const VgTreeSpecies *sp; VgTree t; bool ok; char err[256]; const char *dir, *built, *cache; } TreeBuild;

static void tree_job(void *ctx, int begin, int end) {
    TreeBuild *tb = (TreeBuild *)ctx;
    for (int i = begin; i < end; i++) tb[i].ok = vgeo_tree_load_or_build(tb[i].sp, tb[i].dir, tb[i].built, tb[i].cache, &tb[i].t, tb[i].err, sizeof(tb[i].err));
}

// A tree texture of material k (map: 0 albedo, 1 bark normal, 2 bark ORM), shared by file name.
static u32 tree_texture(const VgTree *t, u32 k, int map) {
    static const char *const suffix[3] = { "", "#n", "#orm" };
    char name[64];
    snprintf(name, sizeof(name), "%s%s", t->tex[k], suffix[map]);
    for (u32 i = 0; i < G.tree_tex_count; i++)
        if (!strcmp(G.tree_tex[i].name, name)) return G.tree_tex[i].tex;
    const VgTreeTexture *x = map == 0 ? &t->texture[k] : map == 1 ? &t->normal[k] : &t->orm[k];
    if (!x->rgba || G.tree_tex_count >= sizeof(G.tree_tex) / sizeof(G.tree_tex[0])) return 0;
    u32 levels;
    u8 *mips = (u8 *)malloc(vg_tex_mip_bytes(x->w, x->h, &levels));
    vg_tex_build_mips(x->rgba, x->w, x->h, mips);
    if (map == 0 && t->masked[k]) vgeo_tree_mip_coverage(mips, x->w, x->h, levels, 0.5f);
    u32 id = vg_tex_create(&G.cull.tex, x->w, x->h, levels, mips, map == 0, true);   // the maps are linear
    free(mips);
    dg_strcpy(G.tree_tex[G.tree_tex_count].name, sizeof(G.tree_tex[0].name), name);
    G.tree_tex[G.tree_tex_count].tex = id;
    G.tree_tex_count++;
    G.tree_tex_bytes += vg_tex_mip_bytes(x->w, x->h, &levels);
    return id;
}

static int foliage_build_trees(u32 role_mask) {
    role_mask &= ~G.tree_roles;
    if (!role_mask) return 0;
    char dir[512], built[512], cache[512];
    platform_data_path("assets/treefbx/", dir, sizeof(dir));
    platform_data_path("assets/vgeo/trees/", built, sizeof(built));
    platform_data_path("assets/cache/", cache, sizeof(cache));
    u32 ns = vgeo_tree_species_count();
    TreeBuild *tb = (TreeBuild *)calloc(ns, sizeof(TreeBuild));
    int n = 0;
    for (u32 i = 0; i < ns; i++) {
        const VgTreeSpecies *sp = vgeo_tree_species(i);
        if (!(role_mask & (1u << sp->role))) continue;
        tb[n].sp = sp;
        tb[n].dir = dir, tb[n].built = built, tb[n].cache = cache;
        n++;
    }
    u64 t0 = SDL_GetTicksNS();
    jobs_parallel_for(n, 1, tree_job, tb);
    int reg = 0, cached = 0;
    for (int i = 0; i < n; i++) {
        TreeBuild *q = &tb[i];
        if (!q->ok) { log_error("vgeo: tree %s: %s", q->sp->name, q->err); continue; }
        if (G.ftype_count >= 32) { vgeo_tree_free(&q->t); continue; }
        VgMaterial mats[VG_TREE_MAX_MATERIALS];
        vgeo_tree_materials(&q->t, mats);
        u32 mesh = vgeo_scene_add_mesh(&G.scene, &q->t.b);
        fallback_register(mesh, &q->t.b, false);
        u32 base = 0;
        for (u32 k = 0; k < q->t.material_count; k++) {
            mats[k].albedo_tex = mats[k].mask_tex = tree_texture(&q->t, k, 0);
            if (q->t.normal[k].rgba) {   // bark maps (vgeo_tree_materials set the flags)
                mats[k].normal_tex = tree_texture(&q->t, k, 1);
                mats[k].mr_tex = mats[k].occlusion_tex = tree_texture(&q->t, k, 2);
            }
            u32 idx = vgeo_scene_add_material(&G.scene, &mats[k]);
            if (!k) base = idx;
        }
        vgeo_scene_set_mesh_materials(&G.scene, mesh, base);
        bool imp = vgeo_impostor(&q->t.b, NULL, NULL) != NULL;
        if (imp && vgeo_foliage_impostor_upload(&G.scene, &G.cull.tex, mesh, &q->t.b)) vgeo_impostor_scale(&G.scene, mesh, q->sp->impostor_scale);
        struct FoliageType *ft = &G.ftype[G.ftype_count++];
        dg_strcpy(ft->name, sizeof(ft->name), q->sp->name);
        ft->mesh = mesh;
        ft->capsule = q->sp->capsule;
        ft->cap_r = q->t.cap_r;
        ft->cap_h = q->t.cap_h;
        ft->role = q->sp->role;
        ft->weight = q->sp->weight;
        G.tree_roles |= 1u << q->sp->role;
        cached += q->t.from_cache;
        reg++;
        log_info("vgeo: tree %s: %u tris, %u clusters, %u levels, %u materials, capsule r %.2f m, impostor %s%s", ft->name, q->t.b.stats.input_tris, q->t.b.hdr.cluster_count,
                 q->t.b.hdr.level_count, q->t.material_count, (double)ft->cap_r, imp ? "8x8 frames" : "none", q->t.from_cache ? "" : " (built)");
        vgeo_tree_free(&q->t);
    }
    free(tb);
    u32 own = 0;
    for (u32 i = 0; i < VG_TEX_OWN; i++) own += G.cull.tex.own[i].used;
    log_info("vgeo: %d tree species (%d cached) in %.0f ms, %u tree textures (%.1f MB, albedo + bark maps), %u / %u module textures", reg, cached,
             (double)(SDL_GetTicksNS() - t0) / 1e6, G.tree_tex_count, (double)G.tree_tex_bytes / (1024.0 * 1024.0), own, VG_TEX_OWN);
    return reg;
}

int render_foliage_map_types(void) {
    if (!G.ok) return 0;
    if (G.map_types) return (int)G.ftype_count;
    render_foliage_test_types();   // grass tufts, bushes and flowers are part of the maps' cover
    u64 t0 = SDL_GetTicksNS();
    foliage_build_trees((1u << VG_TREE_CONIFER) | (1u << VG_TREE_BROADLEAF) | (1u << VG_TREE_DEAD));
    foliage_build_kinds(VG_FOLIAGE_CROP, VG_FOLIAGE_CROP + 1, 512);
    G.map_types = true;
    foliage_region();
    G.need_commit = true;
    foliage_commit();
    log_info("vgeo: foliage map types ready in %.0f ms", (double)(SDL_GetTicksNS() - t0) / 1e6);
    return (int)G.ftype_count;
}

int render_foliage_role_types(int role, char (*names)[48], float *weights, int max) {
    int n = 0;
    for (u32 i = 0; i < G.ftype_count && n < max; i++)
        if (G.ftype[i].role == role) {
            dg_strcpy(names[n], 48, G.ftype[i].name);
            if (weights) weights[n] = G.ftype[i].weight;
            n++;
        }
    return n;
}

int render_foliage_test_types(void) {
    if (!G.ok) return 0;
    if (G.test_types) return (int)G.ftype_count;
    u64 t0 = SDL_GetTicksNS();
    foliage_build_kinds(VG_FOLIAGE_TUFT, VG_FOLIAGE_TUFT + 1, 1024);   // the procedural bush is GPU test content only
    foliage_build_trees((1u << VG_TREE_BUSH) | (1u << VG_TREE_FLOWER));   // the imported bushes and flowers
    G.test_types = true;
    foliage_region();
    G.need_commit = true;
    foliage_commit();
    log_info("vgeo: foliage test types ready in %.0f ms", (double)(SDL_GetTicksNS() - t0) / 1e6);
    return (int)G.ftype_count;
}

void *render_foliage_test_chunk(int cx, int cz, float density, float x0, float z0, float x1, float z1, float (*height)(void *user, float x, float z), void *user,
                                size_t *size) {
    // tufts, the imported bushes and daisies (dropped at load when not registered)
    static const char names[5][48] = { "test_tuft", "bush_juniper", "bush_bamboo", "bush_acacia", "flower_daisy" };
    static const float weight[5] = { 9.0f, 0.45f, 0.15f, 0.1f, 0.3f }, scale[5][2] = { { 0.8f, 1.3f }, { 0.6f, 1.1f }, { 0.6f, 1.0f }, { 0.6f, 1.0f }, { 0.8f, 1.3f } };
    VgScatter sc;
    memset(&sc, 0, sizeof(sc));
    sc.x0 = x0, sc.z0 = z0, sc.x1 = x1, sc.z1 = z1;
    sc.density = density;
    sc.kinds = 5;
    for (int k = 0; k < 5; k++) {
        sc.weight[k] = weight[k];
        sc.scale[k][0] = scale[k][0], sc.scale[k][1] = scale[k][1];
    }
    sc.height = height;
    sc.user = user;
    sc.seed = 1234;
    u32 cap = (u32)(density * VGP_CHUNK_SIZE * VGP_CHUNK_SIZE * 1.3f) + 64;
    VgInstance *tmp = (VgInstance *)malloc(sizeof(VgInstance) * cap);
    u32 n = vgeo_foliage_scatter_chunk(&sc, cx, cz, tmp, cap);
    u8 *blob = n ? vgp_write(cx, cz, names, 5, tmp, n, size) : NULL;
    free(tmp);
    return blob;
}

void *render_foliage_make_chunk(int cx, int cz, const char (*names)[48], int name_count, const RenderFoliageInst *inst, int n, size_t *size) {
    *size = 0;
    if (n <= 0) return NULL;
    VgInstance *tmp = (VgInstance *)malloc(sizeof(VgInstance) * (size_t)n);
    float x0 = (float)cx * VGP_CHUNK_SIZE, z0 = (float)cz * VGP_CHUNK_SIZE;
    int m = 0;
    for (int i = 0; i < n; i++) {
        const RenderFoliageInst *s = &inst[i];
        if (s->pos.x < x0 || s->pos.z < z0 || s->pos.x >= x0 + VGP_CHUNK_SIZE || s->pos.z >= z0 + VGP_CHUNK_SIZE || s->kind >= name_count) continue;
        float p[3] = { s->pos.x, s->pos.y, s->pos.z };
        vgeo_foliage_instance(&tmp[m++], s->kind, p, s->yaw, s->scale);
    }
    u8 *blob = m ? vgp_write(cx, cz, names, (u32)name_count, tmp, (u32)m, size) : NULL;
    free(tmp);
    return blob;
}

// A placement chunk's mesh name: a registered foliage type, else a model with "vgeo": true (its
// "foliage" block sets the materials; registered as a type on first use).
static u32 foliage_resolve(const char *name, void *u) {
    (void)u;
    for (u32 i = 0; i < G.ftype_count; i++)
        if (!strcmp(G.ftype[i].name, name)) return G.ftype[i].mesh;
    if (G.ftype_count >= 32) return ~0u;
    const Model *model = assets_model(assets_model_id(name));
    if (!model || !model->vgeo) return ~0u;
    RenderVgeoPlacement pl;
    memset(&pl, 0, sizeof(pl));
    pl.mesh = name;
    pl.color = v3(1, 1, 1);
    pl.roughness = 0.8f;
    u32 before = G.scene.mesh_count, mesh = level_mesh(&pl);
    if (mesh == ~0u) return ~0u;
    if (G.scene.mesh_count != before) G.need_commit = true;
    struct FoliageType *ft = &G.ftype[G.ftype_count++];
    dg_strcpy(ft->name, sizeof(ft->name), name);
    ft->mesh = mesh;
    ft->capsule = model->foliage.capsule;
    ft->cap_r = model->foliage.capsule_radius;
    ft->cap_h = model->foliage.capsule_height;
    ft->role = -1;
    ft->weight = 0;
    return mesh;
}

int render_foliage_chunk_load(const void *data, size_t size) {
    if (!G.ok || !data) return -1;
    VgpView v;
    if (!vgp_parse((const u8 *)data, size, &v)) { log_warn("vgeo: invalid placement chunk (.vgp)"); return -1; }
    foliage_region();
    u32 h = vgeo_foliage_chunk_load(&G.scene, &v, foliage_resolve, NULL, (u32)(u16)(i16)v.hdr->chunk_x | (u32)(u16)(i16)v.hdr->chunk_z << 16);
    foliage_commit();   // a new mesh / the region: load time
    return h == ~0u ? -1 : (int)h;
}

void render_foliage_chunk_unload(int handle) {
    if (G.ok && handle >= 0) vgeo_scene_placement_remove(&G.scene, (u32)handle);
}

int render_foliage_chunk_capsules(int handle, RenderCapsule *out, int max) {
    if (!G.ok || handle < 0 || (u32)handle >= G.scene.pl.chunk_count || !G.scene.pl.chunks[handle].used) return 0;
    const VgPlacementChunk *ch = &G.scene.pl.chunks[handle];
    int n = 0;
    for (u32 i = ch->first; i < ch->first + ch->count && n < max; i++) {
        const VgInstance *in = &G.scene.pl.inst[i];
        int t = foliage_type_of_mesh(in->mesh);
        if (t < 0 || !G.ftype[t].capsule) continue;
        float s = vgeo_half_to_float(in->scale_h);
        out[n].a = v3(in->pos[0], in->pos[1], in->pos[2]);
        out[n].b = v3(in->pos[0], in->pos[1] + G.ftype[t].cap_h * s, in->pos[2]);
        out[n].radius = G.ftype[t].cap_r * s;
        n++;
    }
    return n;
}

int render_foliage_query(vec3 pos, float radius, RenderFoliageHit *out, int max) {
    if (!G.ok || max <= 0) return 0;
    VgFoliageHit hits[256];
    float p[3] = { pos.x, pos.y, pos.z };
    u32 n = vgeo_foliage_query(&G.scene, p, radius, hits, (u32)DG_MIN(max, 256));
    for (u32 i = 0; i < n; i++) {
        out[i].pos = v3(hits[i].pos[0], hits[i].pos[1], hits[i].pos[2]);
        out[i].scale = hits[i].scale;
        out[i].dist = hits[i].dist;
        out[i].instance = hits[i].instance;
        int t = foliage_type_of_mesh(hits[i].mesh);
        dg_strcpy(out[i].type, sizeof(out[i].type), t >= 0 ? G.ftype[t].name : "?");
    }
    return (int)n;
}

void render_foliage_push(vec3 pos, float radius, float strength) {
    if (!G.disp_ok) return;
    float p[3] = { pos.x, pos.y, pos.z };
    vg_displace_push(&G.disp, p, radius, strength);
}

void render_foliage_clear(void) {
    if (!G.ok) return;
    for (u32 h = 0; h < G.scene.pl.chunk_count; h++)
        if (G.scene.pl.chunks[h].used) vgeo_scene_placement_remove(&G.scene, h);
}

static void cmd_foliage_push_test(int argc, const char **argv) {
    G.push_test = argc > 1 ? (float)atof(argv[1]) : 10.0f;
    log_info("foliage_push_test: %.0f s of pushes circling the camera", (double)G.push_test);
}

static void cmd_foliage_stats(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G.ok) return;
    const VgPlacements *p = &G.scene.pl;
    char err[256] = "";
    bool ok = vgeo_scene_placement_audit(&G.scene, err, sizeof(err));
    log_info("foliage: %u types, %u placement chunks, %u instances (region %u instances / %u cells, %u cells free, %u free ranges), %llu uploads (%.1f MB), "
             "allocator audit %s%s",
             G.ftype_count, p->live_chunks, p->live_instances, p->inst_cap, p->cell_cap, p->free_cells_n, p->free_inst_n, (unsigned long long)p->uploads,
             (double)p->upload_bytes / (1024.0 * 1024.0), ok ? "PASS" : "FAIL: ", ok ? "" : err);
    VgCounters c;
    if (vg_cull_counters(&G.cull, 0, &c))
        log_info("foliage: last frame: %u + %u visible instances, %u faded (density / fade), %u impostors, %u HW + %u SW clusters, %u triangles; vgeo shadows %.3f + %.3f ms",
                 c.n[VGN_INST_VIS], c.n[VGN_INST_P2], c.n[VGN_INST_FADED], c.n[VGN_IMPOSTORS], c.n[VGN_HW], c.n[VGN_SW], c.n[VGN_TRIS], G.shadow_ms[0], G.shadow_ms[1]);
}

static void cmd_foliage_query(int argc, const char **argv) {
    float r = argc > 1 ? (float)atof(argv[1]) : 2.0f;
    RenderFoliageHit h[16];
    int n = render_foliage_query(R->cam.pos, r, h, 16);
    log_info("foliage_query: %d instances within %.1f m of (%.1f, %.1f)", n, (double)r, (double)R->cam.pos.x, (double)R->cam.pos.z);
    for (int i = 0; i < n; i++) log_info("  %s at (%.2f, %.2f, %.2f) scale %.2f, %.2f m", h[i].type, (double)h[i].pos.x, (double)h[i].pos.y, (double)h[i].pos.z, (double)h[i].scale, (double)h[i].dist);
}

// foliage_check (scripted, docs/VGEO.md "Foliage support"): the camera override runs
//   phase 1, 60 frames: behind the foliage yard wall (devroom), facing the yard: the Hi-Z must reject
//            >= 90 % of the foliage instances in the frustum (visible instances vs a CPU count)
//   phase 2, 620 frames: a static camera in the yard, occlusion off: the culled counts (kept by the
//            density hash and fades, impostors, clusters) identical in every frame
static struct {
    int phase, frame, occ_saved, changed, samples;
    u32 sig0, in_frustum, vis;
    double rej;
} FC;

static void cmd_foliage_check(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!G.ok || !G.scene.pl.live_chunks) { log_warn("foliage_check: no placement chunks (foliage_test_scatter first)"); return; }
    memset(&FC, 0, sizeof(FC));
    FC.phase = 1;
    FC.occ_saved = G.occlusion->i;
    log_info("foliage_check: started");
}

static bool foliage_check_camera(RenderCamera *cam) {
    if (!FC.phase) return false;
    if (FC.phase == 1) cam->pos = v3(220.0f, 1.7f, 75.3f);   // 0.45 m from the 8 m wall at z = 76 (devroom foliage yard)
    else cam->pos = v3(220.0f, 1.7f, 120.0f);
    cam->yaw = -180.0f * DG_DEG2RAD;   // facing +z (south, into the yard)
    cam->pitch = FC.phase == 1 ? 0.0f : -6.0f * DG_DEG2RAD;
    cam->roll = 0;
    return true;
}

static void foliage_check_frame(void) {
    if (!FC.phase) return;
    VgCounters c;
    bool hc = vg_cull_counters(&G.cull, 0, &c);
    if (FC.phase == 1 && FC.frame >= 40 && hc) {
        // CPU: foliage instances in this view's frustum (the culling view: unjittered)
        VgViewGpu v;
        float eye[3] = { R->cam.pos.x, R->cam.pos.y, R->cam.pos.z };
        vgeo_view_setup(&v, R->view_proj_nj.m, R->view_proj_nj.m, eye, 1, R->cam.znear, R->hdr.w, R->hdr.h, 1, 32, 0);
        u32 in = 0;
        const VgPlacements *p = &G.scene.pl;
        for (u32 cc = 0; cc < p->cell_cap; cc++)
            for (u32 i = p->cells[cc].first; i < p->cells[cc].first + p->cells[cc].count; i++) {
                const VgInstance *ins = &p->inst[i];
                const VgMesh *m = &G.scene.meshes[ins->mesh];
                VgXfCpu x;
                vgeo_xf_decode(&x, ins, m, NULL);
                float s[4];
                vgeo_xf_sphere(&x, m->center, m->radius, s);
                bool inside = true;
                for (int k = 0; k < 6 && inside; k++) inside = v.planes[k][0] * s[0] + v.planes[k][1] * s[1] + v.planes[k][2] * s[2] + v.planes[k][3] >= -s[3];
                float dx = ins->pos[0] - eye[0], dy = ins->pos[1] - eye[1], dz = ins->pos[2] - eye[2];
                if (inside && vgeo_foliage_alpha(m, ins->flags_phase, sqrtf(dx * dx + dy * dy + dz * dz), G.foliage_density->f) > 0) in++;
            }
        FC.in_frustum += in;
        FC.vis += c.n[VGN_INST_VIS] + c.n[VGN_INST_P2];
        FC.samples++;
    }
    if (FC.phase == 1 && ++FC.frame >= 60) {
        FC.rej = FC.in_frustum ? 1.0 - (double)FC.vis / (double)FC.in_frustum : 0;
        log_info("foliage_check hiz: %u foliage instances in the frustum, %u visible (behind the yard wall, %d frames): %.2f %% rejected -> %s", FC.in_frustum / DG_MAX(FC.samples, 1),
                 FC.vis / DG_MAX(FC.samples, 1), FC.samples, FC.rej * 100.0, FC.rej >= 0.9 && FC.in_frustum ? "PASS" : "FAIL");
        FC.phase = 2;
        FC.frame = 0;
        FC.samples = 0;
        cvar_set_int(G.occlusion, 0);
        return;
    }
    if (FC.phase == 2) {
        if (FC.frame >= 20 && hc) {
            u32 sig = c.n[VGN_INST_VIS] * 2654435761u ^ c.n[VGN_INST_FADED] * 40503u ^ c.n[VGN_IMPOSTORS] * 9973u ^ (c.n[VGN_HW] + c.n[VGN_SW]) * 31u ^ c.n[VGN_TRIS];
            if (!FC.samples) {
                FC.sig0 = sig;
                FC.vis = c.n[VGN_INST_VIS];
                FC.in_frustum = c.n[VGN_INST_FADED];
            } else FC.changed += sig != FC.sig0;
            FC.samples++;
        }
        if (++FC.frame >= 620) {
            log_info("foliage_check density: %d frames of a static camera: %u instances kept, %u faded, %d frames differ -> %s", FC.samples, FC.vis, FC.in_frustum,
                     FC.changed, FC.changed == 0 && FC.samples >= 600 && FC.vis > 0 ? "PASS" : "FAIL");
            cvar_set_int(G.occlusion, FC.occ_saved);
            FC.phase = 0;
        }
    }
}
