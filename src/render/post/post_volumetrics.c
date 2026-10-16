// Volumetric lighting & fog: froxel grid (inject + temporal -> integrate) before the lit pass,
// applied to the opaque scene after it (fog_apply) so transparency / particles sit inside it.
// Grid by quality: Low 80x45x48, High 160x90x64, Ultra 240x135x96 (camera-relative, 80 m deep,
// exponential slices).
#include "post_internal.h"
#include "render/sky/sky.h"
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
    pc.u[1] = P.set.vol_local_lights ? 1u : 0u;   // fog-only without (Engine Fidelity Medium)
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

// Fog targets: the main HDR (froxels, aerial perspective, analytic sky fog without froxels) or
// the scope picture-in-picture (analytic sky fog: its geometry is fogged by the lit shaders).
typedef struct FogTarget { FgRes color, depth; mat4 inv_vp; vec3 cam; bool vol, ap, analytic; } FogTarget;
static FogTarget g_fog_main, g_fog_pip;

// The lit shaders' analytic fog (render.c: frame.fog) is on when the volumetrics are off.
static bool analytic_fog(bool pip) {
    if (R->env.fog_density <= 0.0f || (!P.in.sky_on && !sky_active())) return false;
    return pip || !(P.set.volumetrics > 0 && fg_pass_enabled("vol_inject"));
}

static void exec_fog_apply(VkCommandBuffer cmd, void *u);

// Sky targets: the main HDR (+ depth) or the scope picture-in-picture.
typedef struct SkyTarget { FgRes color, depth; mat4 inv_vp; vec3 cam; } SkyTarget;
static SkyTarget g_sky_main, g_sky_pip, g_sky_view[2];

static void exec_sky(VkCommandBuffer cmd, void *u) {
    const SkyTarget *t = (const SkyTarget *)u;
    if (sky_active()) {
        // Time-of-day atmosphere (src/render/sky).
        sky_render_exec(cmd, t->color, t->depth, t->inv_vp, t->cam, t != &g_sky_main);
        return;
    }
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, t->depth, P.point);
    pb_img_fg(&b, 8, t->color);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    memcpy(pc.p, t->inv_vp.m, sizeof(float) * 16);
    float zen[4] = { P.in.sky_zenith.x, P.in.sky_zenith.y, P.in.sky_zenith.z, P.in.clouds };
    float hor[4] = { P.in.sky_horizon.x, P.in.sky_horizon.y, P.in.sky_horizon.z, P.in.time };
    float cam[3] = { t->cam.x, t->cam.y, t->cam.z };
    memcpy(&pc.u[0], zen, 16);
    memcpy(&pc.u[4], hor, 16);
    memcpy(&pc.u[8], cam, 12);
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(t->color, &w, &h);
    post_dispatch(cmd, "sky", w, h, 1, 8);
}

void post_sky(void) {
    if (!P.ok || (!P.in.sky_on && !sky_active())) return;
    g_sky_main = (SkyTarget){ P.in.hdr, P.in.depth, P.in.inv_view_proj, P.in.cam_pos };
    int p = fg_pass("sky", exec_sky, &g_sky_main);
    fg_read(p, P.in.depth, FG_SAMPLED);
    fg_write(p, P.in.hdr, FG_STORAGE);
}

void post_scope_sky(FgRes color, FgRes depth, mat4 inv_vp, vec3 cam) {
    if (!P.ok || (!P.in.sky_on && !sky_active()) || color < 0) return;
    g_sky_pip = (SkyTarget){ color, depth, inv_vp, cam };
    int p = fg_pass("scope_pip_sky", exec_sky, &g_sky_pip);
    fg_read(p, depth, FG_SAMPLED);
    fg_write(p, color, FG_STORAGE);
    // The scope's geometry has the analytic fog of the lit shaders: the same over its sky.
    if (!analytic_fog(true)) return;
    g_fog_pip = (FogTarget){ color, depth, inv_vp, cam, false, false, true };
    p = fg_pass("scope_pip_fog", exec_fog_apply, &g_fog_pip);
    fg_read(p, depth, FG_SAMPLED);
    fg_write(p, color, FG_STORAGE);
}

// The same sky into a phone camera view (src/render/phone_view.c).
void post_view_sky(int view, FgRes color, FgRes depth, mat4 inv_vp, vec3 cam) {
    if (!P.ok || (!P.in.sky_on && !sky_active()) || color < 0) return;
    SkyTarget *t = &g_sky_view[view & 1];
    *t = (SkyTarget){ color, depth, inv_vp, cam };
    int p = fg_pass(view ? "phone_sky1" : "phone_sky0", exec_sky, t);
    fg_read(p, depth, FG_SAMPLED);
    fg_write(p, color, FG_STORAGE);
}

static void exec_fog_apply(VkCommandBuffer cmd, void *u) {
    const FogTarget *t = (const FogTarget *)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    // Bindings 0 / 2 must be 3D views either way (the colour grading LUT stands in).
    if (t->vol) pb_tex_fg(&b, 0, P.froxels, P.linear);
    else pb_tex(&b, 0, P.lut, P.linear);
    pb_tex_fg(&b, 1, t->depth, P.point);
    if (t->ap) sky_bind_aerial(&b, 2);
    else pb_tex(&b, 2, P.lut, P.linear);
    pb_img_fg(&b, 8, t->color);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    memcpy(pc.p, t->inv_vp.m, sizeof(float) * 16);
    pc.u[0] = t->vol ? 1u : 0u;
    pc.u[1] = t->ap ? 1u : 0u;
    pc.u[2] = t->analytic ? 1u : 0u;
    float f1[4] = { R->env.aerial_max > 10.0f ? R->env.aerial_max : 3000.0f, P.in.zfar, R->env.fog_density, 0.0f };
    float col[3] = { R->env.fog_color.x, R->env.fog_color.y, R->env.fog_color.z };
    float cam[3] = { t->cam.x, t->cam.y, t->cam.z };
    memcpy(&pc.u[4], f1, 16);
    memcpy(&pc.u[8], col, 12);
    memcpy(&pc.u[12], cam, 12);
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(t->color, &w, &h);
    post_dispatch(cmd, "fog_apply", w, h, 1, 8);
}

void post_fog_apply(void) {
    if (!P.ok) return;
    FogTarget *t = &g_fog_main;
    *t = (FogTarget){ P.in.hdr, P.in.depth, P.in.inv_view_proj, P.in.cam_pos, P.froxels != FG_NONE, sky_active(), analytic_fog(false) };
    if (!t->vol && !t->ap && !t->analytic) return;
    int p = fg_pass("fog_apply", exec_fog_apply, t);
    if (t->vol) fg_read(p, P.froxels, FG_SAMPLED);
    fg_read(p, P.in.depth, FG_SAMPLED);
    fg_write(p, P.in.hdr, FG_STORAGE);
}

// Particles / blended draws after fog_apply: the froxel volume (any 3D view when there is none).
bool post_froxel_image(bool use, VkDescriptorImageInfo *out) {
    bool ok = use && P.ok && P.froxels != FG_NONE && fg_valid(P.froxels);
    *out = (VkDescriptorImageInfo){ P.linear, ok ? fg_view(P.froxels) : P.lut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    return ok;
}
