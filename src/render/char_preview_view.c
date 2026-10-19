// Inventory character preview view (see char_preview_view.h): this frame's request -> scene pass
// (render.c render_char_preview_pass: the preview's skinned + captured static draws, its own frame
// UBO with the studio rig) at supersample x the size -> resolve (post/char_preview.comp) into the
// persistent RGBA8 target the UI samples in the same frame.
#include "char_preview_view.h"
#include "post/post_internal.h"
#include "core/log.h"

static struct {
    RenderCharPreview req;
    bool want;                // requested this frame
    RenderTarget target;      // persistent: storage (the resolve) + sampled (the UI)
    TextureId tex;
    int w, h;
    FgRes hdr, depth, out;    // this frame's graph resources
} CP = { .hdr = FG_NONE, .depth = FG_NONE, .out = FG_NONE };

static void destroy_target(void) {
    if (CP.tex) vkr_texture_unwrap(CP.tex);   // the descriptor set is freed once no frame uses it
    if (CP.target.image) vkr_defer_image(CP.target.image, CP.target.view, CP.target.alloc);
    memset(&CP.target, 0, sizeof(CP.target));
    CP.tex = 0;
    CP.w = CP.h = 0;
}

static bool ensure_target(int w, int h) {
    if (CP.target.image && CP.w == w && CP.h == h) return true;
    destroy_target();
    CP.target = vkr_target_create((u32)w, (u32)h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!CP.target.image) return false;
    CP.tex = vkr_texture_wrap(CP.target.image, CP.target.view, CP.target.w, CP.target.h, CP.target.format);
    CP.w = w;
    CP.h = h;
    return CP.tex != 0;
}

void render_char_preview(const RenderCharPreview *v) {
    if (!R->frame_active || v->width < 8 || v->height < 8) return;
    int w = DG_MIN(v->width, CHAR_PREVIEW_MAX_SIZE), h = DG_MIN(v->height, CHAR_PREVIEW_MAX_SIZE);
    if (!ensure_target(w, h)) { log_warn("char preview: target creation failed (%dx%d)", w, h); return; }
    CP.req = *v;
    CP.req.width = w;
    CP.req.height = h;
    // Samples per side: the HDR target stays within the device's 2D limit.
    int s = DG_CLAMP(v->supersample, 1, CHAR_PREVIEW_MAX_SS);
    u32 lim = R->gpu_props.limits.maxImageDimension2D;
    while (s > 1 && ((u32)(w * s) > lim || (u32)(h * s) > lim)) s--;
    CP.req.supersample = s;
    CP.want = true;
}

TextureId render_char_preview_texture(void) { return CP.tex; }
bool char_preview_pending(void) { return CP.want; }

static void exec_resolve(VkCommandBuffer cmd, void *u) {
    (void)u;
    const RenderCharPreview *rq = &CP.req;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, CP.hdr, P.point);
    pb_tex_fg(&b, 1, CP.depth, P.point);
    pb_img_fg(&b, 8, CP.out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = rq->exposure;
    pc.p[1] = rq->saturation;
    pc.u[0] = (u32)rq->supersample;
    pc.u[1] = (u32)rq->tonemap;
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "char_preview", (u32)CP.w, (u32)CP.h, 1, 8);
}

void char_preview_view_build(struct FrameData *fd) {
    CP.hdr = CP.depth = CP.out = FG_NONE;
    if (!CP.want) return;
    CP.want = false;
    if (!CP.target.image) return;
    u32 s = (u32)CP.req.supersample, sw = (u32)CP.w * s, sh = (u32)CP.h * s;
    FgTexDesc hd = { "char_preview_hdr", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, sw, sh, 1, 1, 0 };
    CP.hdr = fg_texture(&hd);
    hd.name = "char_preview_depth";
    hd.format = R->depth.format;
    CP.depth = fg_texture(&hd);
    if (render_char_preview_pass(&CP.req, CP.hdr, CP.depth, sw, sh, fd) < 0) return;
    CP.out = fg_import("char_preview", CP.target.image, CP.target.view, CP.target.format, CP.target.w, CP.target.h, VK_IMAGE_ASPECT_COLOR_BIT, &CP.target.layout);
    int q = fg_pass("char_preview_resolve", exec_resolve, NULL);
    fg_read(q, CP.hdr, FG_SAMPLED);
    fg_read(q, CP.depth, FG_SAMPLED);
    fg_write(q, CP.out, FG_STORAGE);
    if (q < 0) CP.out = FG_NONE;
}

void char_preview_final_reads(int pass) {
    if (CP.out != FG_NONE) fg_read(pass, CP.out, FG_SAMPLED);
}

void char_preview_view_shutdown(void) {
    if (CP.tex) vkr_texture_unwrap(CP.tex);
    vkr_target_destroy(&CP.target);
    memset(&CP, 0, sizeof(CP));
    CP.hdr = CP.depth = CP.out = FG_NONE;
}
