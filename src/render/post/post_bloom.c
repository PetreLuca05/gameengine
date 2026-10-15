// Bloom and lens flare (output resolution chain, half res down to 1/64 or 1/128):
//   bloom_down  13-tap downsample (Karis average on the first mip), every mip of "bloom_down"
//   bloom_up    tent upsample + accumulate into "bloom_up"; the tonemapper mixes up-mip 0 with
//               the scene by bloom_intensity (energy conserving, no threshold) + lens dirt
//   flare       ghosts / halo / anamorphic streak from the quarter-res bloom mip
// Both chains stay in GENERAL layout; mips are synchronised with fg_mip_barrier.
#include "post_internal.h"
#include <string.h>

static struct {
    FgRes color, down, up, flare;
    u32 mips;
} B;

static void exec_bloom(VkCommandBuffer cmd, void *u) {
    (void)u;
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    u32 w, h;
    fg_extent(B.down, &w, &h);
    for (u32 m = 0; m < B.mips; m++) {
        PostBind b = { 0 };
        pb_ubo(&b);
        pb_tex_fg(&b, 0, B.color, P.linear);
        if (m > 0) pb_tex_mip(&b, 1, B.down, m - 1, P.linear);
        else pb_tex(&b, 1, P.black, P.linear);
        pb_img(&b, 8, fg_mip_view(B.down, m));
        pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
        pc.u[0] = m;
        post_push_constants(cmd, &pc, 128);
        post_dispatch(cmd, "bloom_down", DG_MAX(1u, w >> m), DG_MAX(1u, h >> m), 1, 8);
        fg_mip_barrier(cmd, B.down);
    }
    // up[last] = down[last] + nothing; then up[i] = down[i] + tent(up[i + 1]).
    for (int m = (int)B.mips - 1; m >= 0; m--) {
        PostBind b = { 0 };
        pb_ubo(&b);
        pb_tex_mip(&b, 0, B.down, (u32)m, P.linear);
        if (m == (int)B.mips - 1) pb_tex(&b, 1, P.black, P.linear);
        else pb_tex_mip(&b, 1, B.up, (u32)m + 1, P.linear);
        pb_img(&b, 8, fg_mip_view(B.up, (u32)m));
        pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
        pc.u[0] = (u32)m;
        post_push_constants(cmd, &pc, 128);
        post_dispatch(cmd, "bloom_up", DG_MAX(1u, w >> m), DG_MAX(1u, h >> m), 1, 8);
        fg_mip_barrier(cmd, B.up);
    }
}

static void exec_flare(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_mip(&b, 0, B.down, B.mips > 1 ? 1 : 0, P.linear);
    pb_img_fg(&b, 8, B.flare);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(B.flare, &w, &h);
    post_dispatch(cmd, "flare", w, h, 1, 8);
}

FgRes post_bloom_build(FgRes color) {
    memset(&B, 0xff, sizeof(B));
    B.mips = 0;
    const PostSettings *s = &P.set;
    if (s->bloom <= 0 || s->bloom_intensity <= 0.0f) return FG_NONE;
    B.color = color;
    B.mips = s->bloom >= 2 ? 7 : 5;
    FgTexDesc d = { "bloom_down", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT, 0, 0, 0, B.mips, 0.5f };
    B.down = fg_texture(&d);
    d.name = "bloom_up";
    B.up = fg_texture(&d);
    int p = fg_pass("bloom", exec_bloom, NULL);
    fg_read(p, color, FG_SAMPLED);
    fg_write(p, B.down, FG_STORAGE);
    fg_write(p, B.up, FG_STORAGE);
    if (p < 0) return FG_NONE;
    B.mips = DG_MIN(B.mips, fg_mips(B.down));
    return B.up;
}

u32 post_bloom_mips(void) { return B.mips; }

FgRes post_flare_build(FgRes bloom) {
    const PostSettings *s = &P.set;
    if (bloom == FG_NONE || (s->flare <= 0.0f && s->anamorphic <= 0.0f)) return FG_NONE;
    FgTexDesc d = { "flare", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT, 0, 0, 0, 1, 0.25f };
    B.flare = fg_texture(&d);
    int p = fg_pass("flare", exec_flare, NULL);
    fg_read(p, B.down, FG_STORAGE);
    fg_write(p, B.flare, FG_STORAGE);
    return p >= 0 ? B.flare : FG_NONE;
}
