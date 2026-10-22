// Anti-aliasing and upscaling (frame graph nodes after the viewmodel pass):
//   TAA     temporal AA at render resolution (taa.comp, ping-pong history)
//   TAA-U   the same pass reconstructing at the output resolution (render scale 50..100 %)
//   FXAA    fxaa.comp, SMAA 1x (third_party/smaa: edges -> weights -> blend)
//   FSR1    AMD FidelityFX Super Resolution 1 EASU spatial upscale (third_party/fsr), after
//           TAA / FXAA / SMAA at render resolution. CAS sharpening runs in the final pass.
//   FSR 2   AMD FidelityFX FSR 2.2 temporal upscaling (post_fsr2.c), also at render scale 1 (native AA)
//   DLSS    a slot in PostUpscaler; the SDK is not integrated (FSR 2 runs, logged).
#include "post_internal.h"
#include <math.h>
#include <stdlib.h>   // abs / labs in ffx_a.h (clang: no implicit declarations)
#include <string.h>

#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

static struct {
    FgRes in, hist_in, hist_out, out;
    FgRes smaa_ldr, smaa_edges, smaa_weights;
    FgRes easu_in, easu_out;
    bool upscale;
} A;

static void exec_taa(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, A.in, P.point);
    pb_tex_fg(&b, 1, A.hist_in, P.linear);
    pb_tex_fg(&b, 2, P.in.velocity, P.point);
    pb_tex_fg(&b, 3, P.in.depth, P.point);
    pb_tex_fg(&b, 4, fg_valid(P.depth_prev) ? P.depth_prev : P.in.depth, P.point);
    pb_tex_fg(&b, 5, P.in.material, P.point);
    pb_tex_fg(&b, 6, fg_valid(P.in.hdr_opaque) ? P.in.hdr_opaque : A.in, P.point);
    pb_img_fg(&b, 8, A.hist_out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = (fg_history_valid(A.hist_in) && fg_history_valid(P.depth_prev)) ? 1u : 0u;
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(A.hist_out, &w, &h);
    post_dispatch(cmd, "taa", w, h, 1, 8);
}

static void exec_fxaa(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, A.in, P.linear);
    pb_img_fg(&b, 8, A.out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(A.out, &w, &h);
    post_dispatch(cmd, "fxaa", w, h, 1, 8);
}

static void smaa_metrics(VkCommandBuffer cmd) {
    u32 w, h;
    fg_extent(A.in, &w, &h);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = 1.0f / (float)w;
    pc.p[1] = 1.0f / (float)h;
    pc.p[2] = (float)w;
    pc.p[3] = (float)h;
    post_push_constants(cmd, &pc, 128);
}
static void exec_smaa_prep(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, A.in, P.point);
    pb_img_fg(&b, 8, A.smaa_ldr);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(A.smaa_ldr, &w, &h);
    post_dispatch(cmd, "smaa_prep", w, h, 1, 8);
}
static void exec_smaa_edges(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkPipeline p = post_graphics_pipe("smaa_edges", VK_FORMAT_R8G8_UNORM, 0);
    if (!p) return;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, A.smaa_ldr, P.linear);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_GRAPHICS);
    smaa_metrics(cmd);
    u32 w, h;
    fg_extent(A.smaa_edges, &w, &h);
    // Clear: SMAA only writes edge pixels (discard elsewhere).
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = fg_view(A.smaa_edges);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}
static void exec_smaa_weights(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkPipeline p = post_graphics_pipe("smaa_weights", VK_FORMAT_R8G8B8A8_UNORM, 0);
    if (!p) return;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, A.smaa_edges, P.linear);
    pb_tex(&b, 1, P.smaa_area, P.linear);
    pb_tex(&b, 2, P.smaa_search, P.point);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_GRAPHICS);
    smaa_metrics(cmd);
    u32 w, h;
    fg_extent(A.smaa_weights, &w, &h);
    post_fullscreen(cmd, p, fg_view(A.smaa_weights), w, h, false);
}
static void exec_smaa_blend(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkPipeline p = post_graphics_pipe("smaa_blend", VK_FORMAT_R16G16B16A16_SFLOAT, 0);
    if (!p) return;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, A.in, P.linear);
    pb_tex_fg(&b, 1, A.smaa_weights, P.linear);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_GRAPHICS);
    smaa_metrics(cmd);
    u32 w, h;
    fg_extent(A.out, &w, &h);
    post_fullscreen(cmd, p, fg_view(A.out), w, h, false);
}

static void exec_easu(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_tex_fg(&b, 0, A.easu_in, P.linear);
    pb_img_fg(&b, 8, A.easu_out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 iw, ih, ow, oh;
    fg_extent(A.easu_in, &iw, &ih);
    fg_extent(A.easu_out, &ow, &oh);
    struct { AU1 c[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    FsrEasuCon(pc.c, pc.c + 4, pc.c + 8, pc.c + 12, (AF1)iw, (AF1)ih, (AF1)iw, (AF1)ih, (AF1)ow, (AF1)oh);
    vkCmdPushConstants(cmd, P.pl, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    post_dispatch(cmd, "fsr_easu", ow, oh, 1, 8);
}

static FgRes tex(const char *name, VkFormat f, FgSize s) {
    FgTexDesc d = { name, f, s, 0, 0, 0, 1, 0 };
    return fg_texture(&d);
}

// The opaque HDR copy's reader (hdr_opaque: TAA / TAA-U compare against it); PF-0047 skips the copy
// when neither runs and no particle samples it.
bool post_aa_reads_opaque(void) {
    bool scaled = P.in.render_w != P.in.output_w || P.in.render_h != P.in.output_h;
    bool fsr2 = (P.set.upscaler == POST_UP_FSR2 || P.set.upscaler == POST_UP_DLSS) && post_fsr2_available();   // its reactive mask
    return P.set.aa == POST_AA_TAA || (scaled && P.set.upscaler == POST_UP_TAAU) || fsr2;
}

FgRes post_aa_build(FgRes color) {
    memset(&A, 0, sizeof(A));
    const PostSettings *s = &P.set;
    A.in = color;
    FgRes c = color;
    bool scaled = P.in.render_w != P.in.output_w || P.in.render_h != P.in.output_h;
    int up = s->upscaler;
    if (up == POST_UP_DLSS) {
        static int warned;
        if (!warned++) log_warn("post: DLSS SDK not integrated - using FSR 2");
        up = POST_UP_FSR2;
    }
    if (up == POST_UP_FSR2) {
        FgRes f = post_fsr2_build(color);
        if (fg_valid(f)) return f;
        up = POST_UP_TAAU;   // passes unavailable (logged once)
    }
    bool taau = scaled && up == POST_UP_TAAU;
    if (s->aa == POST_AA_TAA || taau) {
        bool parity = (P.in.frame & 1u) != 0;
        FgSize sz = taau ? FG_SIZE_OUTPUT : FG_SIZE_RENDER;
        FgTexDesc hd = { parity ? "taa_hist_a" : "taa_hist_b", VK_FORMAT_R16G16B16A16_SFLOAT, sz, 0, 0, 0, 1, 0 };
        A.hist_in = fg_history(&hd);
        hd.name = parity ? "taa_hist_b" : "taa_hist_a";
        A.hist_out = fg_history(&hd);
        int p = fg_pass(taau ? "taau" : "taa", exec_taa, NULL);
        fg_read(p, A.in, FG_SAMPLED);
        fg_read(p, A.hist_in, FG_SAMPLED);
        fg_read(p, P.in.velocity, FG_SAMPLED);
        fg_read(p, P.in.depth, FG_SAMPLED);
        fg_read(p, P.depth_prev, FG_SAMPLED);
        fg_read(p, P.in.material, FG_SAMPLED);
        fg_read(p, P.in.hdr_opaque, FG_SAMPLED);
        fg_write(p, A.hist_out, FG_STORAGE);
        if (p >= 0) c = A.hist_out;
    } else if (s->aa == POST_AA_FXAA) {
        A.out = tex("fxaa", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_RENDER);
        int p = fg_pass("fxaa", exec_fxaa, NULL);
        fg_read(p, A.in, FG_SAMPLED);
        fg_write(p, A.out, FG_STORAGE);
        if (p >= 0) c = A.out;
    } else if (s->aa == POST_AA_SMAA) {
        A.smaa_ldr = tex("smaa_ldr", VK_FORMAT_R8G8B8A8_UNORM, FG_SIZE_RENDER);
        A.smaa_edges = tex("smaa_edges", VK_FORMAT_R8G8_UNORM, FG_SIZE_RENDER);
        A.smaa_weights = tex("smaa_weights", VK_FORMAT_R8G8B8A8_UNORM, FG_SIZE_RENDER);
        A.out = tex("smaa", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_RENDER);
        int p0 = fg_pass("smaa_prep", exec_smaa_prep, NULL);
        fg_read(p0, A.in, FG_SAMPLED);
        fg_write(p0, A.smaa_ldr, FG_STORAGE);
        int p1 = fg_pass("smaa_edges", exec_smaa_edges, NULL);
        fg_read(p1, A.smaa_ldr, FG_SAMPLED);
        fg_write(p1, A.smaa_edges, FG_COLOR);
        int p2 = fg_pass("smaa_weights", exec_smaa_weights, NULL);
        fg_read(p2, A.smaa_edges, FG_SAMPLED);
        fg_write(p2, A.smaa_weights, FG_COLOR);
        int p3 = fg_pass("smaa_blend", exec_smaa_blend, NULL);
        fg_read(p3, A.in, FG_SAMPLED);
        fg_read(p3, A.smaa_weights, FG_SAMPLED);
        fg_write(p3, A.out, FG_COLOR);
        if (p0 >= 0 && p1 >= 0 && p2 >= 0 && p3 >= 0) c = A.out;
    }
    // Spatial upscale to the output (FSR 1 EASU); otherwise the tonemapper samples bilinearly.
    if (scaled && !taau && up == POST_UP_FSR1) {
        A.easu_in = c;
        A.easu_out = tex("fsr_easu", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT);
        int p = fg_pass("fsr_easu", exec_easu, NULL);
        fg_read(p, A.easu_in, FG_SAMPLED);
        fg_write(p, A.easu_out, FG_STORAGE);
        if (p >= 0) c = A.easu_out;
    }
    return c;
}
