// Night vision goggles: the image-intensifier post profile (docs/NIGHT_OPTICS.md "Image intensifier",
// the model in nvg_model.c). While the goggles are down it replaces exposure / bloom / flare /
// tonemap / grade: the linear HDR before exposure (plus the near-IR image) is the photocathode's
// input and the output is the phosphor's monochrome image in the tube's circle.
//
//   nvg_flux      photocathode flux at the tube's internal resolution (GaAs weights + NIR)
//   nvg_reduce    ABC metering: log-average + peak inside the field -> readback (two frames late)
//   CPU           nvg_step: the ABC gain spring, whiteout, autogating, tube protection
//   nvg_halo      quarter resolution: the fixed-angle halo around bright points (High / Modern)
//   nvg_tube      photoelectron noise (scintillation), MCP spread, saturation, whiteout, fixed
//                 pattern, phosphor persistence (ping-pong history)
//   nvg_display   output resolution: MTF, fiber plate, blemishes, edge, phosphor colour, the mask,
//                 the flip transition from the normal image
//
// r_night_optics (Engine Fidelity tier): 0 Low = half the tube resolution, no halo, no persistence,
// blemishes only; 1 Medium = 16 halo taps; 2 High; 3 Modern = full resolution, 32 taps, chicken wire.
#include "post_internal.h"
#include "render/post/nvg_model.h"
#include "core/cvar.h"
#include "core/log.h"
#include <math.h>
#include <string.h>

#define NIR_BUF_SIZE ((3 + 24 + 16) * 16)   // nvg_nir.comp's Nir block

static struct {
    NvgState abc;
    bool abc_live;                     // goggles running last frame (else the ABC restarts)
    GpuBuffer buf;                     // device: reduce output
    GpuBuffer rb[FRAMES_IN_FLIGHT];
    GpuBuffer nirbuf[FRAMES_IN_FLIGHT];  // host: the near-IR pass's lights (nvg_nir.comp, binding 14)
    u32 rb_frame[FRAMES_IN_FLIGHT];    // frame each readback slot was written (0 = none)
    bool shutoff;                      // the tube protection tripped (game_post consumes it)
    NvgPostStats stats;
    FgRes color, normal, nir, flux, halo, hist_in, hist_out, tube, out;
    FgRes nir_img;                     // this frame's near-IR image
    float gain, f_out, f_flux, halo_uv[2];
    int quality;
    u32 frame;
    Cvar *quality_cv;
    bool init;
} N;

void post_nvg_init(void) {
    N.quality_cv = cvar_reg_int("r_night_optics", 3, 0, 3, CVAR_TIER,
                                "night vision / thermal optics quality: 0 Low (half-res tube, no halo / persistence, 384 x 288 thermal) .. 3 Modern");
    N.buf = vkr_buffer_create(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        N.rb[i] = vkr_buffer_create_readback(64);
        N.nirbuf[i] = vkr_buffer_create(NIR_BUF_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    N.init = true;
}

void post_nvg_shutdown(void) {
    if (!N.init) return;
    vkr_buffer_destroy_now(&N.buf);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkr_buffer_destroy_now(&N.rb[i]);
        vkr_buffer_destroy_now(&N.nirbuf[i]);
    }
    Cvar *q = N.quality_cv;
    memset(&N, 0, sizeof(N));
    N.quality_cv = q;
}

int post_night_optics_quality(void) { return N.quality_cv ? DG_CLAMP(N.quality_cv->i, 0, 3) : 3; }

bool post_nvg_take_shutoff(void) {
    bool s = N.shutoff;
    N.shutoff = false;
    return s;
}

const NvgPostStats *post_nvg_stats(void) { return &N.stats; }

// ---------------------------------------------------------------- passes

static NvgVariant var(void) { return P.set.nvg.v; }

static void push(VkCommandBuffer cmd, const float *p, int np, const u32 *u, int nu) {
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    memcpy(pc.p, p, sizeof(float) * (size_t)np);
    if (u) memcpy(pc.u, u, sizeof(u32) * (size_t)nu);
    post_push_constants(cmd, &pc, 128);
}

static void exec_flux(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, N.color, P.linear);
    if (fg_valid(N.nir)) pb_tex_fg(&b, 1, N.nir, P.linear);
    else pb_tex(&b, 1, P.black, P.linear);
    pb_img_fg(&b, 8, N.flux);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    const PostSettings *s = &P.set;
    float p[8] = { s->nvg.w_gaas[0], s->nvg.w_gaas[1], s->nvg.w_gaas[2], s->nvg.k_vis, s->nvg.k_nir, fg_valid(N.nir) ? 1.0f : 0.0f };
    push(cmd, p, 8, NULL, 0);
    u32 w, h;
    fg_extent(N.flux, &w, &h);
    post_dispatch(cmd, "nvg_flux", w, h, 1, 8);
}

static void exec_reduce(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, N.flux, P.point);
    pb_buf(&b, 13, N.buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    NvgVariant v = var();
    float p[8] = { N.f_flux, (float)v.tubes, v.circle_deg, v.tube_sep_deg, v.ebi };
    push(cmd, p, 8, NULL, 0);
    post_dispatch(cmd, "nvg_reduce", 16, 16, 1, 16);
    post_buffer_barrier(cmd);
    VkBufferCopy c = { 0, 0, 64 };
    vkCmdCopyBuffer(cmd, N.buf.buffer, N.rb[R->frame_slot].buffer, 1, &c);
    post_buffer_barrier(cmd);
}

static void exec_halo(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, N.flux, P.linear);
    pb_img_fg(&b, 8, N.halo);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    // the halo starts where the screen saturates; its brightness follows the source's flux
    float p[8] = { N.gain, 0.6f * P.set.nvg.y_sat, N.halo_uv[0], N.halo_uv[1], 0.35f };
    u32 uu[4] = { N.quality >= 3 ? 32u : 16u };
    push(cmd, p, 8, uu, 4);
    u32 w, h;
    fg_extent(N.halo, &w, &h);
    post_dispatch(cmd, "nvg_halo", w, h, 1, 8);
}

static void exec_tube(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, N.flux, P.point);
    if (fg_valid(N.halo)) pb_tex_fg(&b, 1, N.halo, P.linear);
    else pb_tex(&b, 1, P.black, P.linear);
    if (fg_valid(N.hist_in)) pb_tex_fg(&b, 2, N.hist_in, P.linear);
    else pb_tex(&b, 2, P.black, P.linear);
    pb_img_fg(&b, 8, N.tube);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    NvgVariant v = var();
    float dt = DG_CLAMP(P.in.dt, 1.0f / 480.0f, 0.1f);
    float decay = fg_valid(N.hist_in) && v.persistence > 0.0f ? expf(-dt / v.persistence) : 0.0f;
    float p[16] = {
        N.gain, P.set.nvg.y_sat, N.abc.whiteout, N.abc.flicker,
        v.ebi, v.photons * dt * 60.0f, v.noise_factor, decay,
        N.quality >= 1 ? 0.06f : 0.03f, fg_history_valid(N.hist_in) ? 1.0f : 0.0f, fg_valid(N.halo) ? 1.0f : 0.0f, 0.0f,
    };
    u32 uu[4] = { N.frame, P.set.nvg.seed * 2654435761u + 17u };
    push(cmd, p, 16, uu, 4);
    u32 w, h;
    fg_extent(N.tube, &w, &h);
    post_dispatch(cmd, "nvg_tube", w, h, 1, 8);
}

static void exec_display(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, N.tube, P.linear);
    if (fg_valid(N.normal)) pb_tex_fg(&b, 1, N.normal, P.linear);
    else pb_tex(&b, 1, P.black, P.linear);
    pb_img_fg(&b, 8, N.out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    NvgVariant v = var();
    u32 tw, th, ow, oh;
    fg_extent(N.tube, &tw, &th);
    fg_extent(N.out, &ow, &oh);
    float mtf = 1.0f / (float)DG_MAX(tw, 1u);   // sigma ~ one tube pixel
    // the fiber plate's honeycomb period: ~1/180 of the field's diameter on screen
    float per = DG_MAX(2.0f * N.f_out * tanf(v.circle_deg * 0.5f * 0.017453293f) / 180.0f, 1.5f);
    float p[16] = {
        N.f_out, (float)v.tubes, v.circle_deg, v.tube_sep_deg,
        v.phosphor[0], v.phosphor[1], v.phosphor[2], P.set.nvg.floor,
        P.set.nvg.amount, mtf, N.quality >= 1 ? 1.0f : 0.0f, fg_valid(N.normal) ? 1.0f : 0.0f,
        (float)(3 + (P.set.nvg.seed % 5u)), per * (N.quality >= 3 ? 1.0f : 1.0f), 0.0f, 0.0f,
    };
    u32 uu[4] = { P.set.nvg.seed };
    push(cmd, p, 16, uu, 4);
    post_dispatch(cmd, "nvg_display", ow, oh, 1, 8);
}

static FgRes tex(const char *name, VkFormat f, float scale) {
    FgTexDesc d = { name, f, FG_SIZE_OUTPUT, 0, 0, 0, 1, scale };
    return fg_texture(&d);
}

static FgRes hist(const char *name, float scale) {
    FgTexDesc d = { name, VK_FORMAT_R16_SFLOAT, FG_SIZE_OUTPUT, 0, 0, 0, 1, scale };
    return fg_history(&d);
}

// The ABC state from the metering two frames ago (CPU), then the passes. normal = this frame's
// normal image (the flip blends from it; FG_NONE when the goggles are fully down).
FgRes post_nvg_build(FgRes color, FgRes nir, FgRes normal) {
    const PostSettings *s = &P.set;
    NvgVariant v = var();
    N.frame++;
    N.quality = post_night_optics_quality();
    N.color = color;
    N.nir = nir;
    N.normal = normal;
    N.flux = N.halo = N.hist_in = N.hist_out = N.tube = N.out = FG_NONE;
    // metering readback (written FRAMES_IN_FLIGHT frames ago: its fence has passed)
    const float *rb = (const float *)N.rb[R->frame_slot].mapped;
    bool have = rb && N.rb_frame[R->frame_slot] && N.abc_live;
    float phi_mean = have ? exp2f(rb[0]) : 0.0f;
    float phi_current = have ? rb[1] : 0.0f;
    phi_current += s->nvg.flash;
    NvgConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.y_sat = s->nvg.y_sat;
    cfg.overload = s->nvg.overload;
    if (!N.abc_live || !have) {
        if (!N.abc_live) nvg_state_reset(&N.abc, &v, have ? phi_mean : 2e-3f);
    } else {
        NvgInput in = { phi_mean, phi_current, DG_CLAMP(P.in.dt, 0.0f, 0.1f) };
        nvg_step(&N.abc, &cfg, &v, &in);
        if (N.abc.shutoff) N.shutoff = true;
    }
    N.abc_live = true;
    N.gain = nvg_gain(&N.abc);
    N.stats = (NvgPostStats){ .phi_mean = phi_mean, .phi_current = phi_current, .gain = N.gain, .log_gain = N.abc.log_gain,
                              .whiteout = N.abc.whiteout, .flicker = N.abc.flicker, .overbright = N.abc.overbright,
                              .quality = N.quality, .nir = nir >= 0 };
    // the tube's internal resolution: tube_px across one circle (less at Low), never above the output
    u32 ow = P.in.output_w, oh = P.in.output_h;
    N.f_out = P.in.proj.m[0] * (float)ow * 0.5f;
    float circle_px = 2.0f * N.f_out * tanf(v.circle_deg * 0.5f * 0.017453293f);
    float tube_px = v.tube_px * (N.quality == 0 ? 0.5f : N.quality == 1 ? 0.75f : 1.0f);
    float scale = DG_CLAMP(tube_px / fmaxf(circle_px, 1.0f), 0.25f, 1.0f);
    scale = ceilf(scale * 16.0f) / 16.0f;   // steps: no reallocation per FOV change
    N.f_flux = N.f_out * scale;
    N.stats.tube_scale = scale;
    N.stats.halo_px = nvg_halo_px(&v, 2.0f * atanf(1.0f / fmaxf(P.in.proj.m[0], 1e-4f)) * 57.29578f, (float)ow);
    N.flux = tex("nvg_flux", VK_FORMAT_R16_SFLOAT, scale);
    int p = fg_pass("nvg_flux", exec_flux, NULL);
    fg_read(p, color, FG_SAMPLED);
    fg_read(p, nir, FG_SAMPLED);
    fg_write(p, N.flux, FG_STORAGE);
    p = fg_pass("nvg_reduce", exec_reduce, NULL);
    fg_read(p, N.flux, FG_SAMPLED);
    if (p >= 0) N.rb_frame[R->frame_slot] = N.frame;
    if (N.quality >= 1) {
        N.halo = tex("nvg_halo", VK_FORMAT_R16_SFLOAT, scale * 0.25f);
        // the radius as a uv offset of the flux image: an angle, the same at every resolution
        float r_px = N.stats.halo_px;
        N.halo_uv[0] = r_px / (float)ow;
        N.halo_uv[1] = r_px / (float)oh;
        p = fg_pass("nvg_halo", exec_halo, NULL);
        fg_read(p, N.flux, FG_SAMPLED);
        fg_write(p, N.halo, FG_STORAGE);
        if (p < 0) N.halo = FG_NONE;
    }
    if (N.quality >= 1 && v.persistence > 0.0f) {
        bool parity = (N.frame & 1u) != 0;
        N.hist_in = hist(parity ? "nvg_hist_a" : "nvg_hist_b", scale);
        N.hist_out = hist(parity ? "nvg_hist_b" : "nvg_hist_a", scale);
        N.tube = N.hist_out;
    } else {
        N.tube = tex("nvg_tube", VK_FORMAT_R16_SFLOAT, scale);
    }
    p = fg_pass("nvg_tube", exec_tube, NULL);
    fg_read(p, N.flux, FG_SAMPLED);
    fg_read(p, N.halo, FG_SAMPLED);
    fg_read(p, N.hist_in, FG_SAMPLED);
    fg_write(p, N.tube, FG_STORAGE);
    N.out = tex("nvg_out", VK_FORMAT_R16G16B16A16_SFLOAT, 1.0f);
    p = fg_pass("nvg_display", exec_display, NULL);
    fg_read(p, N.tube, FG_SAMPLED);
    fg_read(p, normal, FG_SAMPLED);
    fg_write(p, N.out, FG_STORAGE);
    DG_UNUSED(oh);
    return p >= 0 ? N.out : (normal >= 0 ? normal : color);
}

// ---------------------------------------------------------------- near-IR image

static void exec_nir(VkCommandBuffer cmd, void *u) {
    (void)u;
    const PostSettings *s = &P.set;
    float *d = (float *)N.nirbuf[R->frame_slot].mapped;
    memset(d, 0, NIR_BUF_SIZE);
    int ns = DG_CLAMP(s->nvg.ir_count, 0, 8);
    u32 nl = DG_MIN(R->light_count, (u32)MAX_POINT_LIGHTS);
    d[0] = (float)ns; d[1] = (float)nl; d[2] = s->nvg.ir_moon; d[3] = s->nvg.ir_sky;
    d[4] = s->nvg.haze; d[5] = s->nvg.rain; d[6] = s->nvg.sky_glow; d[7] = s->nvg.refl_foliage;
    d[8] = s->nvg.refl_default; d[9] = s->nvg.refl_metal;
    for (int i = 0; i < ns; i++) {
        float *o = d + 12 + i * 12;
        o[0] = s->nvg.ir[i].pos.x; o[1] = s->nvg.ir[i].pos.y; o[2] = s->nvg.ir[i].pos.z; o[3] = s->nvg.ir[i].range;
        o[4] = s->nvg.ir[i].dir.x; o[5] = s->nvg.ir[i].dir.y; o[6] = s->nvg.ir[i].dir.z; o[7] = s->nvg.ir[i].cos_outer;
        o[8] = s->nvg.ir[i].intensity; o[9] = s->nvg.ir[i].cos_inner;
    }
    for (u32 i = 0; i < nl; i++) d[12 + 24 * 4 + i] = R->light_ir[i];
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.in.depth, P.point);
    pb_tex_fg(&b, 1, P.in.normal, P.point);
    pb_tex_fg(&b, 2, P.in.material, P.point);
    pb_tex(&b, 16, R->shadow.view, P.shadow_cmp);
    pb_buf(&b, 14, N.nirbuf[R->frame_slot].buffer, NIR_BUF_SIZE, false);
    pb_img_fg(&b, 8, N.nir_img);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(N.nir_img, &w, &h);
    post_dispatch(cmd, "nvg_nir", w, h, 1, 8);
}

// The near-IR image at half render resolution (Low: a quarter): built when the goggles run.
FgRes post_nvg_nir(void) {
    N.nir_img = FG_NONE;
    if (!P.set.nvg.nir || P.set.nvg.amount <= 0.0f || P.in.normal < 0 || P.in.material < 0) return FG_NONE;   // (fg_valid: only after allocation)
    FgTexDesc d = { "nvg_nir", VK_FORMAT_R16_SFLOAT, post_night_optics_quality() == 0 ? FG_SIZE_QUARTER : FG_SIZE_HALF, 0, 0, 0, 1, 0 };
    N.nir_img = fg_texture(&d);
    int p = fg_pass("nvg_nir", exec_nir, NULL);
    fg_read(p, P.in.depth, FG_SAMPLED);
    fg_read(p, P.in.normal, FG_SAMPLED);
    fg_read(p, P.in.material, FG_SAMPLED);
    fg_write(p, N.nir_img, FG_STORAGE);
    if (p < 0) N.nir_img = FG_NONE;
    return N.nir_img;
}

// The goggles are off: the next time they run, the ABC starts from the scene (no stale gain).
void post_nvg_idle(void) { N.abc_live = false; }
