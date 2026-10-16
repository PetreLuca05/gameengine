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
    Cvar *r_vgeo, *error_px, *shadow_error_px, *sw_px, *occlusion, *freeze, *debug, *debug_spheres;
} G;

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
    G.ok = true;
    log_info("vgeo: culling ready (indirect count %d, mesh shader %d)", c->draw_indirect_count, c->mesh_shader);
}

void vkg_shutdown(void) {
    if (!G.ok) return;
    for (u32 f = 0; f < FRAMES_IN_FLIGHT; f++) vgc_buffer_free(&G.ctx, &G.dbg_rb[f]);
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
