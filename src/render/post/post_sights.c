// Weapon sights after the depth of field (sharp, excluded from DoF) and before bloom (the
// emissive dot / illuminated reticle blooms): shaders/sights/*.frag, fullscreen triangles
// scissored to the lens.
//   red dot   glass.frag (multiply: glass tint)  ->  reddot.frag (add: collimated MOA dot,
//             fresnel reflection, dust)
//   scope     scope.frag (premultiplied over): the picture-in-picture image magnified through the
//             ocular (chromatic aberration, eye box / scope shadow, reticle FFP / SFP). The
//             magnification exists only there: the main view stays at the base FOV.
// Geometry is analytic: the lens disc is intersected per pixel in view space with the viewmodel
// projection (the item is drawn with it), occluded by the viewmodel depth; the dot and the
// magnified image use the main projection (they show the world).
#include "post_internal.h"
#include "gameplay/items.h"
#include <math.h>

// Must match shaders/sights/sight_common.glsl (std430).
typedef struct SightGpu {
    vec4 lens;          // view-space lens centre, radius
    vec4 fwd;           // view-space optic forward, kind
    vec4 up;            // xyz, ads_t
    vec4 right;         // xyz, complete
    vec4 dotp;          // view-space dot point (main), dot radius (rad)
    vec4 color;         // reticle rgb, brightness
    vec4 tint;          // glass tint rgb, dust
    vec4 glass;         // fresnel, chromatic (px), reticle shape, outer radius (rad)
    vec4 scope;         // k (magnification vs the main view), k nominal (SFP), eye box (m), eye box depth (m)
    vec4 eye;           // lens-space eye offset x, y (m), relief error (m), illuminated
    vec4 ray_main0, ray_main_du, ray_main_dv;   // main projection: view ray (z = -1) = r0 + u du + v dv
    vec4 ray_vm0, ray_vm_du, ray_vm_dv;         // viewmodel projection
    vec4 vmdepth;       // vm proj m10, m14, debug view, ffp
    vec4 misc;          // tan(full-screen sight FOV / 2), aspect, time, output height
    vec4 render;        // render w, h, output w, h
    mat4 pip_from_view; // main view space -> PiP clip
} SightGpu;

static struct {
    RenderSight s;
    FgRes pip;
    mat4 pip_from_view, vm_proj;
    float pip_fov;
    GpuBuffer buf[FRAMES_IN_FLIGHT];
    bool buf_ok;
    FgRes target;
    VkRect2D scissor;
    bool full;
} SG;

void post_set_sight(const RenderSight *s, FgRes pip, mat4 pip_from_view, mat4 vm_proj, float pip_fov) {
    SG.s = *s;
    SG.pip = pip;
    SG.pip_from_view = pip_from_view;
    SG.vm_proj = vm_proj;
    SG.pip_fov = pip_fov;
}

bool post_sights_ads_dof(float *focus, float *fstop, float *near_blur) {
    if (!SG.s.ads_dof) return false;
    *focus = SG.s.dof_focus;
    *fstop = SG.s.dof_fstop;
    *near_blur = SG.s.dof_near_blur;
    return true;
}

float post_sights_vm_focus(float *m10, float *m14) {
    *m10 = SG.vm_proj.m[10];
    *m14 = SG.vm_proj.m[14];
    return SG.s.ads_dof ? fmaxf(SG.s.dof_vm_focus, 0.0f) : 0.0f;
}

static vec3 view_point(vec3 p) { return mat4_mul_point(P.in.view, p); }
static vec3 view_dir(vec3 d) {
    vec4 r = mat4_mul_v4(P.in.view, v4(d.x, d.y, d.z, 0.0f));
    return v3_norm(v3(r.x, r.y, r.z));
}

// View-space ray (z = -1) through NDC (x, y) of projection inv.
static vec3 ray_at(mat4 inv, float x, float y) {
    vec4 v = mat4_mul_v4(inv, v4(x, y, 1.0f, 1.0f));
    vec3 d = v3(v.x / v.w, v.y / v.w, v.z / v.w);
    return v3_scale(d, -1.0f / d.z);
}

static void ray_basis(mat4 proj, vec4 *r0, vec4 *du, vec4 *dv) {
    mat4 inv = mat4_inverse(proj);
    vec3 a = ray_at(inv, -1, -1), b = ray_at(inv, 1, -1), c = ray_at(inv, -1, 1);
    *r0 = v4(a.x, a.y, a.z, 0);
    *du = v4(b.x - a.x, b.y - a.y, b.z - a.z, 0);
    *dv = v4(c.x - a.x, c.y - a.y, c.z - a.z, 0);
}

static void ensure_bufs(void) {
    if (SG.buf_ok) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) SG.buf[i] = vkr_buffer_create(sizeof(SightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    SG.buf_ok = true;
}

void post_sights_shutdown(void) {
    if (!SG.buf_ok) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkr_buffer_destroy_now(&SG.buf[i]);
    SG.buf_ok = false;
}

static void bind_common(VkCommandBuffer cmd) {
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.in.vm_depth, P.point);
    if (SG.pip >= 0) pb_tex_fg(&b, 1, SG.pip, P.linear);
    else pb_tex(&b, 1, P.black, P.linear);
    pb_tex(&b, 2, P.noise, P.linear_repeat);
    pb_buf(&b, 13, SG.buf[R->frame_slot].buffer, sizeof(SightGpu), false);
    pb_buf(&b, 14, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_GRAPHICS);
}

static void draw_sight(VkCommandBuffer cmd, const char *frag, int blend) {
    VkPipeline pipe = post_graphics_pipe(frag, VK_FORMAT_R16G16B16A16_SFLOAT, blend);
    if (!pipe) return;
    bind_common(cmd);
    u32 w, h;
    fg_extent(SG.target, &w, &h);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = fg_view(SG.target);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &SG.scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

static void exec_glass(VkCommandBuffer cmd, void *u) { (void)u; draw_sight(cmd, "sights/glass", 3); }
static void exec_reddot(VkCommandBuffer cmd, void *u) { (void)u; draw_sight(cmd, "sights/reddot", 1); }
static void exec_scope(VkCommandBuffer cmd, void *u) { (void)u; draw_sight(cmd, "sights/scope", 2); }

// Lens bounds on screen (viewmodel projection) -> scissor; false if behind / off screen.
static bool lens_scissor(const SightGpu *g, u32 w, u32 h) {
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    vec3 c = v3(g->lens.x, g->lens.y, g->lens.z), up = v3(g->up.x, g->up.y, g->up.z), rt = v3(g->right.x, g->right.y, g->right.z);
    float r = g->lens.w * 1.6f;
    for (int i = 0; i < 4; i++) {
        vec3 p = v3_add(c, v3_add(v3_scale(rt, (i & 1) ? r : -r), v3_scale(up, (i & 2) ? r : -r)));
        if (p.z > -0.005f) return false;
        vec4 cl = mat4_mul_v4(SG.vm_proj, v4(p.x, p.y, p.z, 1));
        float x = (cl.x / cl.w * 0.5f + 0.5f) * (float)w, y = (cl.y / cl.w * 0.5f + 0.5f) * (float)h;
        mnx = fminf(mnx, x); mny = fminf(mny, y); mxx = fmaxf(mxx, x); mxy = fmaxf(mxy, y);
    }
    int x0 = DG_CLAMP((int)floorf(mnx) - 4, 0, (int)w), y0 = DG_CLAMP((int)floorf(mny) - 4, 0, (int)h);
    int x1 = DG_CLAMP((int)ceilf(mxx) + 4, 0, (int)w), y1 = DG_CLAMP((int)ceilf(mxy) + 4, 0, (int)h);
    if (x1 <= x0 || y1 <= y0) return false;
    SG.scissor = (VkRect2D){ { x0, y0 }, { (u32)(x1 - x0), (u32)(y1 - y0) } };
    return true;
}

FgRes post_sights_build(FgRes color) {
    const RenderSight *s = &SG.s;
    if (!P.ok || s->kind == RSIGHT_NONE || color < 0) return color;
    ensure_bufs();
    SightGpu *g = (SightGpu *)SG.buf[R->frame_slot].mapped;
    memset(g, 0, sizeof(*g));
    vec3 lens = view_point(s->lens_pos), fwd = view_dir(s->fwd), up = view_dir(s->up), rt = view_dir(s->right);
    g->lens = v4(lens.x, lens.y, lens.z, s->lens_radius);
    g->fwd = v4(fwd.x, fwd.y, fwd.z, (float)s->kind);
    g->up = v4(up.x, up.y, up.z, s->ads_t);
    g->right = v4(rt.x, rt.y, rt.z, s->complete ? 1.0f : 0.0f);
    vec3 dp = view_point(s->dot_point);
    float moa = DG_DEG2RAD / 60.0f;
    g->dotp = v4(dp.x, dp.y, dp.z, s->reticle_moa * 0.5f * moa);
    g->color = v4(s->reticle_color.x, s->reticle_color.y, s->reticle_color.z, s->brightness);
    g->tint = v4(s->tint.x, s->tint.y, s->tint.z, s->dust);
    g->glass = v4(s->fresnel, s->chromatic, (float)s->reticle_shape, s->reticle_outer_moa * 0.5f * moa);
    // Magnification relative to the main view: a world direction at tangent t from the axis is
    // shown at tangent t * k on screen (k = zoom at the base FOV, rescaled to the main FOV).
    float tan_main = tanf(P.in.fov_y * 0.5f), tan_base = tanf(fmaxf(s->base_fov, 10.0f) * 0.5f * DG_DEG2RAD);
    float k = fmaxf(s->zoom, 1.0f) * tan_main / fmaxf(tan_base, 1e-4f);
    // SFP: the reticle is fixed in the eyepiece, i.e. it scales with the weapon projection the
    // ocular is drawn with (authored at the scope's 1.15 main zoom).
    float tan_vm = 1.0f / fmaxf(fabsf(SG.vm_proj.m[5]), 1e-4f);
    float k_nom = fmaxf(s->zoom, 1.0f) / 1.15f * tan_main / fmaxf(tan_vm, 1e-4f);
    g->scope = v4(k, k_nom, fmaxf(s->eye_box, 1e-4f), fmaxf(s->eye_box_depth, 1e-4f));
    // Eye box: the eye relative to the optic's own axis (the picture follows the eye -> sight line,
    // the scope shadow reacts to the gun's residual sway about the reticle).
    vec3 e = v3_neg(lens);   // eye (view origin) relative to the lens
    vec3 afwd = fwd, aup = up, art = rt;
    if (v3_len2(s->axis_fwd) > 0.5f) { afwd = view_dir(s->axis_fwd); aup = view_dir(s->axis_up); art = view_dir(s->axis_right); }
    g->eye = v4(v3_dot(e, art), v3_dot(e, aup), v3_dot(e, v3_neg(afwd)) - s->eye_relief, s->illuminated ? 1.0f : 0.0f);
    ray_basis(P.in.proj, &g->ray_main0, &g->ray_main_du, &g->ray_main_dv);
    ray_basis(SG.vm_proj, &g->ray_vm0, &g->ray_vm_du, &g->ray_vm_dv);
    g->vmdepth = v4(SG.vm_proj.m[10], SG.vm_proj.m[14], (float)s->debug, s->ffp ? 1.0f : 0.0f);
    u32 w, h;
    fg_extent(color, &w, &h);
    g->misc = v4(tanf(fmaxf(s->sight_fov, 0.5f) * 0.5f * DG_DEG2RAD), (float)w / (float)DG_MAX(1u, h), P.in.time, (float)h);
    g->render = v4((float)P.in.render_w, (float)P.in.render_h, (float)w, (float)h);
    g->pip_from_view = SG.pip_from_view;
    SG.target = color;
    // Full screen: the raw PiP debug view.
    SG.full = s->debug == 2;
    if (SG.full) SG.scissor = (VkRect2D){ { 0, 0 }, { w, h } };
    else if (!lens_scissor(g, w, h)) return color;
    if (s->kind == RSIGHT_REDDOT) {
        int p0 = fg_pass("sight_glass", exec_glass, NULL);
        fg_read(p0, P.in.vm_depth, FG_SAMPLED);
        fg_write(p0, color, FG_COLOR);
        int p1 = fg_pass("sight_reddot", exec_reddot, NULL);
        fg_read(p1, P.in.vm_depth, FG_SAMPLED);
        fg_write(p1, color, FG_COLOR);
    } else {
        int p0 = fg_pass("sight_scope", exec_scope, NULL);
        fg_read(p0, P.in.vm_depth, FG_SAMPLED);
        if (SG.pip >= 0) fg_read(p0, SG.pip, FG_SAMPLED);
        fg_write(p0, color, FG_COLOR);
    }
    return color;
}
