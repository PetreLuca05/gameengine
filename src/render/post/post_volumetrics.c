// Volumetric lighting & fog: froxel grid (inject + temporal -> integrate) before the lit pass,
// applied to the opaque scene after it (fog_apply) so transparency / particles sit inside it.
// Grid by quality: Low 80x45x48, High 160x90x64, Ultra 240x135x96 (camera-relative, 80 m deep,
// exponential slices).
#include "post_internal.h"
#include <string.h>

static struct { FgRes scatter, hist_in, hist_out, integrated; u32 w, h, d; } V;

static void exec_inject(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, V.hist_in, P.linear);
    pb_tex(&b, 1, P.noise, P.point);
    pb_tex(&b, 7, R->shadow.view, P.shadow_cmp);
    pb_img_fg(&b, 8, V.hist_out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = fg_history_valid(V.hist_in) ? 1u : 0u;
    post_push_constants(cmd, &pc, 128);
    VkPipeline p = post_compute_pipe("vol_inject");
    if (!p) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdDispatch(cmd, (V.w + 3) / 4, (V.h + 3) / 4, (V.d + 3) / 4);
}

static void exec_integrate(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, V.hist_out, P.point);
    pb_img_fg(&b, 8, V.integrated);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    post_dispatch(cmd, "vol_integrate", V.w, V.h, 1, 8);
}

void post_vol_build(void) {
    memset(&V, 0, sizeof(V));
    V.integrated = FG_NONE;
    P.froxels = FG_NONE;
    const PostSettings *s = &P.set;
    if (!s->volumetrics || s->fog_density <= 0.0f) return;
    static const u32 dims[4][3] = { { 0, 0, 0 }, { 80, 45, 48 }, { 160, 90, 64 }, { 240, 135, 96 } };
    int q = DG_CLAMP(s->volumetrics, 1, 3);
    // Keep the froxel aspect close to the screen's.
    float aspect = (float)P.in.render_w / (float)DG_MAX(1u, P.in.render_h);
    V.h = dims[q][1];
    V.w = DG_MAX(16u, (u32)((float)V.h * aspect + 0.5f));
    V.d = dims[q][2];
    PostUBO *ubo = (PostUBO *)P.ubo[R->frame_slot].mapped;
    ubo->s_vol3 = v4((float)V.w, (float)V.h, (float)V.d, 80.0f);
    bool parity = (P.in.frame & 1u) != 0;
    FgTexDesc hd = { parity ? "froxel_hist_a" : "froxel_hist_b", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, V.w, V.h, V.d, 1, 0 };
    V.hist_in = fg_history(&hd);
    hd.name = parity ? "froxel_hist_b" : "froxel_hist_a";
    V.hist_out = fg_history(&hd);
    FgTexDesc id = { "froxel", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, V.w, V.h, V.d, 1, 0 };
    V.integrated = fg_texture(&id);
    int p = fg_pass("vol_inject", exec_inject, NULL);
    fg_read(p, V.hist_in, FG_SAMPLED);
    fg_write(p, V.hist_out, FG_STORAGE);
    int i = fg_pass("vol_integrate", exec_integrate, NULL);
    fg_read(i, V.hist_out, FG_SAMPLED);
    fg_write(i, V.integrated, FG_STORAGE);
    if (p >= 0 && i >= 0) P.froxels = V.integrated;
}

FgRes post_froxels(void) { return P.froxels; }

static void exec_fog_apply(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.froxels, P.linear);
    pb_tex_fg(&b, 1, P.in.depth, P.point);
    pb_img_fg(&b, 8, P.in.hdr);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    post_dispatch(cmd, "fog_apply", P.in.render_w, P.in.render_h, 1, 8);
}

void post_fog_apply(void) {
    if (!P.ok || P.froxels == FG_NONE) return;
    int p = fg_pass("fog_apply", exec_fog_apply, NULL);
    fg_read(p, P.froxels, FG_SAMPLED);
    fg_read(p, P.in.depth, FG_SAMPLED);
    fg_write(p, P.in.hdr, FG_STORAGE);
}
