// Camera effects after AA / upscaling (output resolution):
//   motion blur    McGuire 2012 reconstruction: tile max (16 px) -> neighbour max -> gather.
//                  Mode 1 camera only (depth reprojection), 2 camera + objects (velocity
//                  buffer). Shutter fraction = mb_intensity, clamped to 40 px. Viewmodel sharp.
//   depth of field thin-lens CoC, GPU autofocus on the crosshair (exposure_buf[2]), half-res
//                  near / far gather (circle or hexagonal bokeh), full-res composite. Mode 1
//                  only while aiming down sights, 2 always.
#include "post_internal.h"
#include <string.h>

static struct {
    FgRes mb_in, tiles, neighbor, mb_out;      // separate inputs: both chains are built before either executes
    FgRes dof_in, coc, half, far, near, dof_out;
} C;

static void exec_mb_tiles(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.in.velocity, P.point);
    pb_tex_fg(&b, 1, P.in.depth, P.point);
    pb_tex_fg(&b, 2, P.in.material, P.point);
    pb_img_fg(&b, 8, C.tiles);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    post_dispatch(cmd, "mb_tiles", P.in.render_w, P.in.render_h, 1, 16);
}

static void exec_mb_neighbor(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, C.tiles, P.point);
    pb_img_fg(&b, 8, C.neighbor);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(C.neighbor, &w, &h);
    post_dispatch(cmd, "mb_neighbor", w, h, 1, 8);
}

static void exec_mb_blur(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, C.mb_in, P.linear);
    pb_tex_fg(&b, 1, C.neighbor, P.point);
    pb_tex_fg(&b, 2, P.in.velocity, P.point);
    pb_tex_fg(&b, 3, P.in.depth, P.point);
    pb_tex_fg(&b, 4, P.in.material, P.point);
    pb_tex(&b, 5, P.noise, P.point);
    pb_img_fg(&b, 8, C.mb_out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(C.mb_out, &w, &h);
    post_dispatch(cmd, "mb_blur", w, h, 1, 8);
}

static FgRes tex(const char *name, VkFormat f, FgSize s) {
    FgTexDesc d = { name, f, s, 0, 0, 0, 1, 0 };
    return fg_texture(&d);
}

FgRes post_mb_build(FgRes color) {
    memset(&C, 0xff, sizeof(C));   // every FgRes = FG_NONE
    const PostSettings *s = &P.set;
    if (s->motion_blur <= 0 || s->mb_intensity <= 0.0f || P.in.camera_cut) return color;
    C.mb_in = color;
    u32 tw = (P.in.render_w + 15) / 16, th = (P.in.render_h + 15) / 16;
    FgTexDesc td = { "mb_tiles", VK_FORMAT_R16G16_SFLOAT, FG_SIZE_ABS, tw, th, 1, 1, 0 };
    C.tiles = fg_texture(&td);
    td.name = "mb_neighbor";
    C.neighbor = fg_texture(&td);
    C.mb_out = tex("motion_blur", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT);
    int p0 = fg_pass("mb_tiles", exec_mb_tiles, NULL);
    fg_read(p0, P.in.velocity, FG_SAMPLED);
    fg_read(p0, P.in.depth, FG_SAMPLED);
    fg_read(p0, P.in.material, FG_SAMPLED);
    fg_write(p0, C.tiles, FG_STORAGE);
    int p1 = fg_pass("mb_neighbor", exec_mb_neighbor, NULL);
    fg_read(p1, C.tiles, FG_SAMPLED);
    fg_write(p1, C.neighbor, FG_STORAGE);
    int p2 = fg_pass("mb_blur", exec_mb_blur, NULL);
    fg_read(p2, color, FG_SAMPLED);
    fg_read(p2, C.neighbor, FG_SAMPLED);
    fg_read(p2, P.in.velocity, FG_SAMPLED);
    fg_read(p2, P.in.depth, FG_SAMPLED);
    fg_read(p2, P.in.material, FG_SAMPLED);
    fg_write(p2, C.mb_out, FG_STORAGE);
    return (p0 >= 0 && p1 >= 0 && p2 >= 0) ? C.mb_out : color;
}

// ------------------------------------------------------------------ depth of field

static void exec_dof_focus(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.in.depth, P.point);
    pb_tex_fg(&b, 1, P.in.material, P.point);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = (P.in.ads && P.in.ads_focus_dist > 0.0f) ? P.in.ads_focus_dist : 0.0f;
    pc.p[1] = P.in.ads ? 12.0f : 5.0f;
    pc.u[0] = P.set.dof_autofocus ? 1u : 0u;
    float af, as, an;
    if (post_sights_ads_dof(&af, &as, &an)) {
        // ADS (aim complete): fixed far focus, applied at once.
        pc.p[0] = af;
        pc.p[1] = 1e4f;
        pc.u[0] = 0u;
    }
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "dof_focus", 8, 8, 1, 8);
    post_buffer_barrier(cmd);
}

static void exec_dof_coc(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.in.depth, P.point);
    pb_tex_fg(&b, 1, P.in.material, P.point);
    pb_img_fg(&b, 8, C.coc);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    float af, as, an;
    if (post_sights_ads_dof(&af, &as, &an)) {
        pc.p[0] = as;      // f-stop override
        pc.p[1] = an;      // near-field (viewmodel) blur, CoC px
        pc.u[0] = 1u;
    }
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(C.coc, &w, &h);
    post_dispatch(cmd, "dof_coc", w, h, 1, 8);
}

static void exec_dof_down(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, C.dof_in, P.linear);
    pb_tex_fg(&b, 1, C.coc, P.point);
    pb_img_fg(&b, 8, C.half);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(C.half, &w, &h);
    post_dispatch(cmd, "dof_down", w, h, 1, 8);
}

static void exec_dof_gather(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, C.half, P.linear);
    pb_tex(&b, 1, P.noise, P.point);
    pb_img_fg(&b, 8, C.far);
    pb_img_fg(&b, 9, C.near);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(C.far, &w, &h);
    post_dispatch(cmd, "dof_gather", w, h, 1, 8);
}

static void exec_dof_composite(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, C.dof_in, P.point);
    pb_tex_fg(&b, 1, C.coc, P.point);
    pb_tex_fg(&b, 2, C.far, P.linear);
    pb_tex_fg(&b, 3, C.near, P.linear);
    pb_img_fg(&b, 8, C.dof_out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(C.dof_out, &w, &h);
    post_dispatch(cmd, "dof_composite", w, h, 1, 8);
}

FgRes post_dof_build(FgRes color) {
    const PostSettings *s = &P.set;
    // ADS depth of field (sights, aim complete) overrides the user's mode while it lasts; the mode
    // applies again the frame the aim is no longer complete.
    float af, as, an;
    bool ads_dof = post_sights_ads_dof(&af, &as, &an);
    if (!ads_dof && (s->dof <= 0 || (s->dof == 1 && !P.in.ads))) return color;
    C.dof_in = color;
    C.coc = tex("dof_coc", VK_FORMAT_R16_SFLOAT, FG_SIZE_OUTPUT);
    FgTexDesc hd = { "dof_half", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT, 0, 0, 0, 1, 0.5f };
    C.half = fg_texture(&hd);
    hd.name = "dof_far";
    C.far = fg_texture(&hd);
    hd.name = "dof_near";
    C.near = fg_texture(&hd);
    C.dof_out = tex("dof", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT);
    int pf = fg_pass("dof_focus", exec_dof_focus, NULL);
    fg_read(pf, P.in.depth, FG_SAMPLED);
    fg_read(pf, P.in.material, FG_SAMPLED);
    int p0 = fg_pass("dof_coc", exec_dof_coc, NULL);
    fg_read(p0, P.in.depth, FG_SAMPLED);
    fg_read(p0, P.in.material, FG_SAMPLED);
    fg_write(p0, C.coc, FG_STORAGE);
    int p1 = fg_pass("dof_down", exec_dof_down, NULL);
    fg_read(p1, color, FG_SAMPLED);
    fg_read(p1, C.coc, FG_SAMPLED);
    fg_write(p1, C.half, FG_STORAGE);
    int p2 = fg_pass("dof_gather", exec_dof_gather, NULL);
    fg_read(p2, C.half, FG_SAMPLED);
    fg_write(p2, C.far, FG_STORAGE);
    fg_write(p2, C.near, FG_STORAGE);
    int p3 = fg_pass("dof_composite", exec_dof_composite, NULL);
    fg_read(p3, color, FG_SAMPLED);
    fg_read(p3, C.coc, FG_SAMPLED);
    fg_read(p3, C.far, FG_SAMPLED);
    fg_read(p3, C.near, FG_SAMPLED);
    fg_write(p3, C.dof_out, FG_STORAGE);
    return (p0 >= 0 && p1 >= 0 && p2 >= 0 && p3 >= 0) ? C.dof_out : color;
}

FgRes post_debug_coc(void) { return C.coc; }
FgRes post_debug_mb_tiles(void) { return C.neighbor; }
