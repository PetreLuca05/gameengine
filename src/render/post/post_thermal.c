// Thermal scope sensor passes (docs/NIGHT_OPTICS.md "Thermal sensor"; the model's CPU half is
// render/thermal_sensor.c, the temperature state game_thermal.c). The PiP renders band radiance at the
// core's resolution (lit_body.glsl thermal_shade); then:
//   thermal_core      sky, germanium blur, DDE, NETD, drifting fixed pattern, smear, NUC hold (ping-pong history)
//   thermal_reduce    AGC metering: radiance percentiles + the centre spot -> readback (two frames late)
//   thermal_display   digital zoom crop (nearest), AGC -> palette, vignette, OSD + crosshair, at 2x the core
// The display image replaces the PiP for the scope composite (post_sights.c).
#include "post_internal.h"
#include "world/thermal.h"
#include "render/thermal_sensor.h"
#include <math.h>
#include <string.h>

#define TB_HEAD 0
#define TB_INV_VP 64
#define TB_CAM 128
#define TB_CORE 144
#define TB_LUT 160
#define TB_PAL (TB_LUT + 256 * 4)
#define TB_OSD (TB_PAL + 8 * 256 * 4)
#define TB_SIZE (TB_OSD + RT_OSD_CHARS * 4)

static struct {
    GpuBuffer buf[FRAMES_IN_FLIGHT];       // host: the per-frame block (thermal_common.glsl)
    GpuBuffer meas;                        // device: thermal_reduce output
    GpuBuffer rb[FRAMES_IN_FLIGHT];
    u32 rb_frame[FRAMES_IN_FLIGHT];
    u32 pal[8][256];                       // RGBA8
    float lo, hi, spot;                    // last measurement
    bool measured;
    FgRes pip, depth, hist_in, hist_out, disp;
    u32 frame;
    bool init;
} TH;

void post_thermal_init(void) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        TH.buf[i] = vkr_buffer_create(TB_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        TH.rb[i] = vkr_buffer_create_readback(64);
    }
    TH.meas = vkr_buffer_create(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    for (int p = 0; p < 8; p++)
        for (int i = 0; i < 256; i++) TH.pal[p][i] = 0xff000000u | (u32)i * 0x010101u;   // White Hot until the game's arrive
    TH.init = true;
}

void post_thermal_shutdown(void) {
    if (!TH.init) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkr_buffer_destroy_now(&TH.buf[i]);
        vkr_buffer_destroy_now(&TH.rb[i]);
    }
    vkr_buffer_destroy_now(&TH.meas);
    memset(&TH, 0, sizeof(TH));
}

void render_thermal_palettes(const u8 (*rgb)[256][3], int count) {
    for (int p = 0; p < 8 && p < count; p++)
        for (int i = 0; i < 256; i++)
            TH.pal[p][i] = 0xff000000u | (u32)rgb[p][i][0] | (u32)rgb[p][i][1] << 8 | (u32)rgb[p][i][2] << 16;
}

bool render_thermal_measure(float *lo, float *hi, float *spot) {
    if (!TH.measured) return false;
    *lo = TH.lo;
    *hi = TH.hi;
    *spot = TH.spot;
    return true;
}

static void fill_block(const RenderThermal *t, const mat4 *inv_vp, vec3 cam, u32 core, u32 disp) {
    u8 *b = (u8 *)TH.buf[R->frame_slot].mapped;
    float *f = (float *)b;
    float dt = DG_CLAMP(P.in.dt, 1.0f / 480.0f, 0.1f);
    float head[16] = {
        t->sky_depression, t->sky_exponent, t->air, 1.0f,
        dt, THERMAL_SMEAR_TAU, THERMAL_NETD, t->drift,
        t->agc_lo, t->agc_hi, (float)DG_CLAMP(t->zoom, 1, 4), (float)DG_CLAMP(t->palette, 0, 7),
        t->frozen ? 1.0f : 0.0f, t->dead ? 1.0f : 0.0f, fg_history_valid(TH.hist_in) ? 1.0f : 0.0f, 0.0f,
    };
    int n = 0;
    while (n < RT_OSD_CHARS && t->osd[n]) n++;
    head[15] = (float)n;
    memcpy(f, head, sizeof(head));
    memcpy(b + TB_INV_VP, inv_vp->m, 64);
    float cv[8] = { cam.x, cam.y, cam.z, 0.0f, (float)core, (float)disp, (float)(t->seed & 0xffffu), (float)post_night_optics_quality() };
    memcpy(b + TB_CAM, cv, sizeof(cv));
    memcpy(b + TB_LUT, thermal_lut(), 256 * 4);
    memcpy(b + TB_PAL, TH.pal, sizeof(TH.pal));
    u32 *o = (u32 *)(b + TB_OSD);
    for (int i = 0; i < RT_OSD_CHARS; i++) o[i] = i < n ? (u8)t->osd[i] : 0u;
}

static void exec_core(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, TH.pip, P.point);
    pb_tex_fg(&b, 1, TH.depth, P.point);
    if (TH.hist_in >= 0 && fg_valid(TH.hist_in)) pb_tex_fg(&b, 2, TH.hist_in, P.point);
    else pb_tex(&b, 2, P.black, P.point);
    pb_img_fg(&b, 8, TH.hist_out);
    pb_buf(&b, 14, TH.buf[R->frame_slot].buffer, TB_SIZE, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(TH.hist_out, &w, &h);
    post_dispatch(cmd, "thermal_core", w, h, 1, 8);
}

static void exec_reduce(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, TH.hist_out, P.point);
    pb_buf(&b, 13, TH.meas.buffer, 64, false);
    pb_buf(&b, 14, TH.buf[R->frame_slot].buffer, TB_SIZE, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    post_dispatch(cmd, "thermal_reduce", 16, 16, 1, 16);
    post_buffer_barrier(cmd);
    VkBufferCopy c = { 0, 0, 64 };
    vkCmdCopyBuffer(cmd, TH.meas.buffer, TH.rb[R->frame_slot].buffer, 1, &c);
    post_buffer_barrier(cmd);
}

static void exec_display(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, TH.hist_out, P.point);
    pb_img_fg(&b, 8, TH.disp);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_buf(&b, 14, TH.buf[R->frame_slot].buffer, TB_SIZE, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = (u32)render_thermal()->debug_view;
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(TH.disp, &w, &h);
    post_dispatch(cmd, "thermal_display", w, h, 1, 8);
}

// The sensor on this frame's thermal PiP (radiance + depth, core square size): the display image.
FgRes post_thermal_build(FgRes pip, FgRes depth, mat4 inv_vp, vec3 cam) {
    const RenderThermal *t = render_thermal();
    TH.frame++;
    // the measurement written FRAMES_IN_FLIGHT frames ago
    const float *rb = (const float *)TH.rb[R->frame_slot].mapped;
    if (rb && TH.rb_frame[R->frame_slot] && rb[3] > 0.0f) {
        TH.lo = rb[0];
        TH.hi = rb[1];
        TH.spot = rb[2];
        TH.measured = true;
    }
    TH.pip = pip;
    TH.depth = depth;
    u32 core = DG_MAX((u32)DG_MIN(t->core_w, t->core_h), 64u);
    u32 disp = DG_CLAMP(core * 2u, 256u, 1024u);
    bool parity = (TH.frame & 1u) != 0;
    FgTexDesc hd = { parity ? "thermal_core_a" : "thermal_core_b", VK_FORMAT_R32_SFLOAT, FG_SIZE_ABS, core, core, 1, 1, 0 };
    TH.hist_in = fg_history(&hd);
    hd.name = parity ? "thermal_core_b" : "thermal_core_a";
    TH.hist_out = fg_history(&hd);
    fill_block(t, &inv_vp, cam, core, disp);
    int p = fg_pass("thermal_core", exec_core, NULL);
    fg_read(p, pip, FG_SAMPLED);
    fg_read(p, depth, FG_SAMPLED);
    fg_read(p, TH.hist_in, FG_SAMPLED);
    fg_write(p, TH.hist_out, FG_STORAGE);
    if (p < 0) return pip;
    p = fg_pass("thermal_reduce", exec_reduce, NULL);
    fg_read(p, TH.hist_out, FG_SAMPLED);
    if (p >= 0) TH.rb_frame[R->frame_slot] = TH.frame;
    FgTexDesc dd = { "thermal_display", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_ABS, disp, disp, 1, 1, 0 };
    TH.disp = fg_texture(&dd);
    p = fg_pass("thermal_display", exec_display, NULL);
    fg_read(p, TH.hist_out, FG_SAMPLED);
    fg_write(p, TH.disp, FG_STORAGE);
    return p >= 0 ? TH.disp : pip;
}
