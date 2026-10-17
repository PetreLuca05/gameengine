// Looks on the GPU (docs/LOOKS.md): one 32^3 LUT per look, baked on the CPU from the analytic
// stack (look.c) at startup and again whenever invalidated (look_tune edits, Engine Fidelity
// changes: render_looks_invalidate). The tonemap pass (and final_direct.frag at Fidelity Off)
// replaces its tonemapper with one fetch of the active look's LUT.
//
// Preview strip (Settings > Look): when the UI asks for it this frame (render_look_preview), a small
// compute pass grades a low-res copy of the current scene under each look into one RGBA8 strip,
// side by side in LookId order, sRGB-encoded for the UI.
#include "post_internal.h"
#include "look.h"
#include "core/log.h"
#include "core/platform.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(POST_LOOKS == LOOK_COUNT, "POST_LOOKS mirrors LOOK_COUNT");

#define PREVIEW_TILE_H 108u

static struct {
    // preview strip
    RenderTarget strip;
    TextureId strip_tex;
    bool wanted, written;
    u32 tile_w;
    FgRes strip_res, src;
    bool direct;
} L;

static void bake_all(void) {
    double t0 = platform_time();
    float *rgb = (float *)malloc(sizeof(float) * 3 * LOOK_LUT_N * LOOK_LUT_N * LOOK_LUT_N);
    for (int i = 0; i < POST_LOOKS; i++) {
        post_image_destroy(&P.look_img[i], &P.look_alloc[i], &P.look_lut[i]);
        look_bake_lut(look_get(i), rgb);
        post_lut3d_create(&P.look_img[i], &P.look_alloc[i], &P.look_lut[i], rgb, LOOK_LUT_N);
    }
    free(rgb);
    P.looks_dirty = false;
    log_info("post: %d look LUTs baked (%d^3, %.0f ms)", POST_LOOKS, LOOK_LUT_N, (platform_time() - t0) * 1000.0);
}

void post_looks_init(void) {
    memset(&L, 0, sizeof(L));
    L.strip_res = L.src = FG_NONE;
    bake_all();
}

void post_looks_shutdown(void) {
    for (int i = 0; i < POST_LOOKS; i++) post_image_destroy(&P.look_img[i], &P.look_alloc[i], &P.look_lut[i]);
    if (L.strip_tex) {
        vkr_texture_unwrap(L.strip_tex);
        vkr_target_destroy(&L.strip);
    }
    memset(&L, 0, sizeof(L));
}

void post_looks_update(void) {
    if (!P.looks_dirty) return;
    vkDeviceWaitIdle(R->device);   // the old LUTs may be in flight
    bake_all();
}

void render_looks_invalidate(void) { P.looks_dirty = true; }

VkImageView post_look_view(int look) { return look >= 0 && look < POST_LOOKS && P.look_lut[look] ? P.look_lut[look] : (P.look_lut[0] ? P.look_lut[0] : P.lut); }

// ------------------------------------------------------------------ preview strip

TextureId render_look_preview(int *tiles, float *tile_aspect) {
    L.wanted = true;
    if (tiles) *tiles = POST_LOOKS;
    if (tile_aspect) *tile_aspect = L.tile_w ? (float)L.tile_w / (float)PREVIEW_TILE_H : 16.0f / 9.0f;
    return L.written ? L.strip_tex : 0;   // nothing to show before the first strip is graded
}

static void exec_preview(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, L.src, P.linear);
    for (int i = 0; i < POST_LOOKS; i++) pb_tex(&b, 1 + (u32)i, post_look_view(i), P.linear);
    pb_img(&b, 8, L.strip.view);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = POST_LOOKS;
    pc.u[1] = L.tile_w;
    pc.u[2] = L.direct ? 1u : 0u;
    pc.p[0] = P.direct_exposure;
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "look_preview", L.strip.w, L.strip.h, 1, 8);
}

void post_look_preview_build(FgRes color, bool direct) {
    L.strip_res = FG_NONE;
    if (!L.wanted || color == FG_NONE) { L.wanted = false; return; }
    L.wanted = false;
    float aspect = (float)P.in.output_w / (float)DG_MAX(1u, P.in.output_h);
    u32 tw = (u32)DG_CLAMP((int)(PREVIEW_TILE_H * aspect + 0.5f), 120, 260);
    if (!L.strip_tex || L.strip.w != tw * POST_LOOKS) {
        if (L.strip_tex) vkr_defer_image(L.strip.image, L.strip.view, L.strip.alloc);
        L.strip = vkr_target_create(tw * POST_LOOKS, PREVIEW_TILE_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
        if (L.strip_tex) vkr_texture_rewrap(L.strip_tex, L.strip.image, L.strip.view, L.strip.w, L.strip.h);
        else L.strip_tex = vkr_texture_wrap(L.strip.image, L.strip.view, L.strip.w, L.strip.h, L.strip.format);
        L.written = false;
    }
    L.tile_w = tw;
    L.src = color;
    L.direct = direct;
    L.strip_res = fg_import("look_preview", L.strip.image, L.strip.view, L.strip.format, L.strip.w, L.strip.h, VK_IMAGE_ASPECT_COLOR_BIT, &L.strip.layout);
    int p = fg_pass("look_preview", exec_preview, NULL);
    fg_read(p, color, FG_SAMPLED);
    fg_write(p, L.strip_res, FG_STORAGE);
    if (p < 0) { L.strip_res = FG_NONE; return; }
    L.written = true;
}

void post_look_final_reads(int pass) {
    if (L.strip_res != FG_NONE) fg_read(pass, L.strip_res, FG_SAMPLED);
}
