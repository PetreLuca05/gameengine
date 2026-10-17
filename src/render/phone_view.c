// Phone camera views (see phone_view.h): per view, this frame's request -> scene pass from the
// lens (render.c, like the scope picture-in-picture) -> sky -> light post (phone_tonemap.comp)
// -> optional copy into a host-cached buffer of the current frame slot, harvested once that
// slot's fence has passed (two frames later).
#include "phone_view.h"
#include "post/post_internal.h"
#include "core/log.h"

typedef struct PhoneTarget {
    RenderTarget scene, screen, panel;   // persistent (scene: storage + transfer src, screen / panel: storage + sampled)
    TextureId screen_tex, panel_tex;
    int w, h, pw, ph;
} PhoneTarget;

typedef struct PhoneReadback {
    GpuBuffer buf[FRAMES_IN_FLIGHT];
    int w[FRAMES_IN_FLIGHT], h[FRAMES_IN_FLIGHT];
    bool pending[FRAMES_IN_FLIGHT];
    int ready_slot;               // slot holding the newest completed copy (-1 none)
    u32 serial;
} PhoneReadback;

static struct {
    RenderPhoneView req[RENDER_PHONE_VIEWS];
    bool want[RENDER_PHONE_VIEWS];
    PhoneTarget t[RENDER_PHONE_VIEWS];
    PhoneReadback rb[RENDER_PHONE_VIEWS];
    // per frame graph build
    FgRes hdr[RENDER_PHONE_VIEWS], depth[RENDER_PHONE_VIEWS], scene_res[RENDER_PHONE_VIEWS], screen_res[RENDER_PHONE_VIEWS], panel_res[RENDER_PHONE_VIEWS];
    int pass_scene[RENDER_PHONE_VIEWS], pass_post[RENDER_PHONE_VIEWS];
    int slot;
    bool init;
} PV;

int render_alt_scene_pass(int slot, const char *name, FgRes color, FgRes depth, u32 w, u32 h, mat4 view, mat4 proj, vec3 cam, struct FrameData *fd);

static void pv_init(void) {
    if (PV.init) return;
    memset(&PV, 0, sizeof(PV));
    for (int v = 0; v < RENDER_PHONE_VIEWS; v++) PV.rb[v].ready_slot = -1;
    PV.init = true;
}

void render_phone_view(const RenderPhoneView *v) {
    pv_init();
    if (!R->frame_active || v->view < 0 || v->view >= RENDER_PHONE_VIEWS || v->width < 16 || v->height < 16) return;
    PV.req[v->view] = *v;
    PV.want[v->view] = true;
}

TextureId render_phone_screen(int view) {
    if (!PV.init || view < 0 || view >= RENDER_PHONE_VIEWS) return 0;
    return PV.t[view].screen_tex;
}

TextureId render_phone_panel(int view) {
    if (!PV.init || view < 0 || view >= RENDER_PHONE_VIEWS) return 0;
    return PV.t[view].panel_tex;
}

bool render_phone_readback(int view, const u8 **rgba, int *w, int *h, u32 *serial) {
    if (!PV.init || view < 0 || view >= RENDER_PHONE_VIEWS) return false;
    PhoneReadback *r = &PV.rb[view];
    if (r->ready_slot < 0) return false;
    *rgba = (const u8 *)r->buf[r->ready_slot].mapped;
    *w = r->w[r->ready_slot];
    *h = r->h[r->ready_slot];
    *serial = r->serial;
    return *rgba != NULL;
}

float render_phone_gpu_ms(int view) {
    if (!PV.init || view < 0 || view >= RENDER_PHONE_VIEWS) return 0;
    float ms = 0;
    for (int i = 0; i < fg_pass_count(); i++) {
        const char *n = fg_pass_name(i);
        if (n && !strncmp(n, "phone", 5) && n[strlen(n) - 1] == (char)('0' + view)) ms += fg_pass_ms(i);
    }
    return ms;
}

static void destroy_target(PhoneTarget *t) {
    if (t->screen_tex) vkr_texture_unwrap(t->screen_tex);
    if (t->scene.image) vkr_defer_image(t->scene.image, t->scene.view, t->scene.alloc);
    if (t->screen.image) vkr_defer_image(t->screen.image, t->screen.view, t->screen.alloc);
    RenderTarget panel = t->panel;
    TextureId ptex = t->panel_tex;
    int pw = t->pw, ph = t->ph;
    memset(t, 0, sizeof(*t));
    t->panel = panel;   // the panel image has its own size (ensure_panel)
    t->panel_tex = ptex;
    t->pw = pw;
    t->ph = ph;
}

static bool ensure_panel(PhoneTarget *t, int w, int h) {
    if (t->panel.image && t->pw == w && t->ph == h) return true;
    if (t->panel_tex) vkr_texture_unwrap(t->panel_tex);
    if (t->panel.image) vkr_defer_image(t->panel.image, t->panel.view, t->panel.alloc);
    t->panel_tex = 0;
    memset(&t->panel, 0, sizeof(t->panel));
    t->panel = vkr_target_create((u32)w, (u32)h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!t->panel.image) return false;
    t->panel_tex = vkr_texture_wrap(t->panel.image, t->panel.view, t->panel.w, t->panel.h, t->panel.format);
    t->pw = w;
    t->ph = h;
    return t->panel_tex != 0;
}

static bool ensure_target(PhoneTarget *t, int w, int h) {
    if (t->scene.image && t->w == w && t->h == h) return true;
    destroy_target(t);
    t->scene = vkr_target_create((u32)w, (u32)h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
    t->screen = vkr_target_create((u32)w, (u32)h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!t->scene.image || !t->screen.image) return false;
    t->screen_tex = vkr_texture_wrap(t->screen.image, t->screen.view, t->screen.w, t->screen.h, t->screen.format);
    t->w = w;
    t->h = h;
    return t->screen_tex != 0;
}

static void exec_post(VkCommandBuffer cmd, void *u) {
    int v = (int)(intptr_t)u;
    const RenderPhoneView *rq = &PV.req[v];
    PostBind b = { 0 };
    pb_ubo(&b);
    if (rq->ui_only) pb_tex(&b, 0, P.black, P.linear);
    else pb_tex_fg(&b, 0, PV.hdr[v], P.linear);
    bool ov = rq->overlay && R->textures[rq->overlay].used && R->textures[rq->overlay].ready;
    pb_tex(&b, 1, ov ? R->textures[rq->overlay].view : P.black, P.linear);
    pb_img_fg(&b, 8, PV.scene_res[v]);
    pb_img_fg(&b, 9, PV.screen_res[v]);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = rq->exposure_bias;
    pc.p[1] = rq->flash;
    pc.p[2] = rq->vignette;
    pc.u[0] = ov ? 1u : 0u;
    pc.u[1] = rq->mirror ? 1u : 0u;
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "phone_tonemap", (u32)PV.t[v].w, (u32)PV.t[v].h, 1, 8);
}

// HUD panel: the screen image box-filtered to the panel's pixels, rounded corners as alpha.
static void exec_panel(VkCommandBuffer cmd, void *u) {
    int v = (int)(intptr_t)u;
    const RenderPhoneView *rq = &PV.req[v];
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, PV.screen_res[v], P.linear);
    pb_img_fg(&b, 8, PV.panel_res[v]);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = rq->panel_radius;
    post_push_constants(cmd, &pc, 128);
    post_dispatch(cmd, "phone_panel", (u32)PV.t[v].pw, (u32)PV.t[v].ph, 1, 8);
}

static void exec_copy(VkCommandBuffer cmd, void *u) {
    int v = (int)(intptr_t)u;
    PhoneReadback *r = &PV.rb[v];
    VkBufferImageCopy region = { 0 };
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (u32)PV.t[v].w;
    region.imageExtent.height = (u32)PV.t[v].h;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, fg_image(PV.scene_res[v]), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->buf[PV.slot].buffer, 1, &region);
}

static void exec_none(VkCommandBuffer cmd, void *u) {
    (void)cmd;
    (void)u;
}

void phone_view_build(struct FrameData *fd) {
    pv_init();
    PV.slot = R->frame_slot;
    for (int v = 0; v < RENDER_PHONE_VIEWS; v++) {
        PV.hdr[v] = PV.depth[v] = PV.scene_res[v] = PV.screen_res[v] = PV.panel_res[v] = FG_NONE;
        if (!PV.want[v]) continue;
        PV.want[v] = false;
        const RenderPhoneView *rq = &PV.req[v];
        PhoneTarget *t = &PV.t[v];
        if (!ensure_target(t, rq->width, rq->height)) { log_warn("phone view: target creation failed"); continue; }
        PV.scene_res[v] = fg_import("phone_scene", t->scene.image, t->scene.view, t->scene.format, t->scene.w, t->scene.h, VK_IMAGE_ASPECT_COLOR_BIT, &t->scene.layout);
        PV.screen_res[v] = fg_import("phone_screen", t->screen.image, t->screen.view, t->screen.format, t->screen.w, t->screen.h, VK_IMAGE_ASPECT_COLOR_BIT,
                                     &t->screen.layout);
        bool panel = rq->panel_w >= 8 && rq->panel_h >= 8 && ensure_panel(t, rq->panel_w, rq->panel_h);
        if (panel)
            PV.panel_res[v] = fg_import("phone_panel", t->panel.image, t->panel.view, t->panel.format, t->panel.w, t->panel.h, VK_IMAGE_ASPECT_COLOR_BIT, &t->panel.layout);
        if (!rq->ui_only) {
        FgTexDesc hd = { "phone_hdr", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, (u32)t->w, (u32)t->h, 1, 1, 0 };
        PV.hdr[v] = fg_texture(&hd);
        hd.name = "phone_depth";
        hd.format = R->depth.format;
        PV.depth[v] = fg_texture(&hd);
        // Lens camera: -Z forward, +Y up of rq->rot.
        vec3 fwd = quat_rotate(rq->rot, v3(0, 0, -1)), up = quat_rotate(rq->rot, v3(0, 1, 0));
        mat4 view = mat4_look_at(rq->pos, v3_add(rq->pos, fwd), up);
        float aspect = (float)t->w / (float)t->h;
        mat4 proj = mat4_perspective(clampf(rq->fov_deg, 2.0f, 120.0f) * DG_DEG2RAD, aspect, 0.05f, R->cam.zfar);
        int p = render_alt_scene_pass(v, v == 0 ? "phone_scene0" : "phone_scene1", PV.hdr[v], PV.depth[v], (u32)t->w, (u32)t->h, view, proj, rq->pos, fd);
        if (p < 0) continue;
        post_view_sky(v, PV.hdr[v], PV.depth[v], mat4_inverse(mat4_mul(proj, view)), rq->pos);
        }
        int q = fg_pass(v == 0 ? "phone_post0" : "phone_post1", exec_post, (void *)(intptr_t)v);
        if (!rq->ui_only) fg_read(q, PV.hdr[v], FG_SAMPLED);
        fg_write(q, PV.scene_res[v], FG_STORAGE);
        fg_write(q, PV.screen_res[v], FG_STORAGE);
        if (panel) {
            int pp = fg_pass(v == 0 ? "phone_panel0" : "phone_panel1", exec_panel, (void *)(intptr_t)v);
            fg_read(pp, PV.screen_res[v], FG_SAMPLED);
            fg_write(pp, PV.panel_res[v], FG_STORAGE);
        }
        if (rq->readback && !rq->ui_only) {
            PhoneReadback *r = &PV.rb[v];
            VkDeviceSize need = (VkDeviceSize)t->w * (VkDeviceSize)t->h * 4;
            if (r->buf[PV.slot].size < need) {
                if (r->buf[PV.slot].buffer) vkr_defer_buffer(&r->buf[PV.slot]);
                r->buf[PV.slot] = vkr_buffer_create_readback(need);
                if (r->ready_slot == PV.slot) r->ready_slot = -1;
            }
            int c = fg_pass(v == 0 ? "phone_copy0" : "phone_copy1", exec_copy, (void *)(intptr_t)v);
            fg_read(c, PV.scene_res[v], FG_TRANSFER_SRC);
            if (c >= 0) {
                r->pending[PV.slot] = true;
                r->w[PV.slot] = t->w;
                r->h[PV.slot] = t->h;
            }
        }
        // The screen texture is sampled by next frame's draws (lit pipeline: SHADER_READ_ONLY).
        int s = fg_pass(v == 0 ? "phone_ready0" : "phone_ready1", exec_none, NULL);
        fg_read(s, PV.screen_res[v], FG_SAMPLED);
        fg_read(s, PV.scene_res[v], FG_SAMPLED);
        if (panel) fg_read(s, PV.panel_res[v], FG_SAMPLED);
    }
}

bool phone_view_pending(void) {
    if (!PV.init) return false;
    for (int v = 0; v < RENDER_PHONE_VIEWS; v++)
        if (PV.want[v]) return true;
    return false;
}

void phone_view_frame_begin(int slot) {
    if (!PV.init) return;
    for (int v = 0; v < RENDER_PHONE_VIEWS; v++) {
        PhoneReadback *r = &PV.rb[v];
        if (!r->pending[slot]) continue;
        r->pending[slot] = false;
        vmaInvalidateAllocation(R->vma, r->buf[slot].alloc, 0, VK_WHOLE_SIZE);
        r->ready_slot = slot;
        r->serial++;
    }
}

void phone_view_shutdown(void) {
    if (!PV.init) return;
    for (int v = 0; v < RENDER_PHONE_VIEWS; v++) {
        PhoneTarget *t = &PV.t[v];
        if (t->screen_tex) vkr_texture_unwrap(t->screen_tex);
        if (t->panel_tex) vkr_texture_unwrap(t->panel_tex);
        vkr_target_destroy(&t->scene);
        vkr_target_destroy(&t->screen);
        vkr_target_destroy(&t->panel);
        for (int s = 0; s < FRAMES_IN_FLIGHT; s++) vkr_buffer_destroy_now(&PV.rb[v].buf[s]);
    }
    memset(&PV, 0, sizeof(PV));
}
