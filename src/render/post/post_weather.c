// Lens droplets (docs/WEATHER.md "Rain on the camera"): the game simulates up to
// RENDER_LENS_DROPS droplets in screen space (src/game/game_weather.c: spawn, grow, merge, run down
// with gravity and camera motion, evaporate) and hands them over with render_set_weather. Here:
//   lens_drops (half res)   : a height / normal map of the drops (shaders/post/lens_drops.comp)
//   lens_apply (output res):  refracts the HDR image through it, darkened rims, a sky highlight
// After the sights, before bloom / exposure (so drops catch highlights and bloom). The scope
// picture-in-picture is a separate render and never gets drops.
#include "post_internal.h"
#include <string.h>

static struct {
    GpuBuffer buf[FRAMES_IN_FLIGHT];
    bool buf_ok;
    FgRes src, map, out;
    int count;
    float strength;
} L;

static void ensure_bufs(void) {
    if (L.buf_ok) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) L.buf[i] = vkr_buffer_create(sizeof(RenderLensDrop) * RENDER_LENS_DROPS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    L.buf_ok = true;
}

void post_weather_shutdown(void) {
    if (!L.buf_ok) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkr_buffer_destroy_now(&L.buf[i]);
    L.buf_ok = false;
}

static void exec_drops(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_img_fg(&b, 8, L.map);
    pb_buf(&b, 13, L.buf[R->frame_slot].buffer, sizeof(RenderLensDrop) * RENDER_LENS_DROPS, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = (u32)L.count;
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(L.map, &w, &h);
    post_dispatch(cmd, "lens_drops", w, h, 1, 8);
}

static void exec_apply(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, L.src, P.linear);
    pb_tex_fg(&b, 1, L.map, P.linear);
    pb_img_fg(&b, 8, L.out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = L.strength;
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(L.out, &w, &h);
    post_dispatch(cmd, "lens_apply", w, h, 1, 8);
}

FgRes post_lens_build(FgRes color) {
    const RenderWeather *wx = render_weather_state();
    if (!wx || wx->lens_strength <= 0.0f || wx->drop_count <= 0 || color < 0) return color;
    ensure_bufs();
    L.count = DG_MIN(wx->drop_count, RENDER_LENS_DROPS);
    memcpy(L.buf[R->frame_slot].mapped, wx->drops, sizeof(RenderLensDrop) * (size_t)L.count);
    L.strength = wx->lens_strength;
    L.src = color;
    FgTexDesc md = { "lens_map", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT, 0, 0, 0, 1, 0.5f };
    L.map = fg_texture(&md);
    FgTexDesc od = { "lens_out", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT, 0, 0, 0, 1, 0 };
    L.out = fg_texture(&od);
    int p0 = fg_pass("lens_drops", exec_drops, NULL);
    fg_write(p0, L.map, FG_STORAGE);
    int p1 = fg_pass("lens_apply", exec_apply, NULL);
    fg_read(p1, L.src, FG_SAMPLED);
    fg_read(p1, L.map, FG_SAMPLED);
    fg_write(p1, L.out, FG_STORAGE);
    if (p0 < 0 || p1 < 0) return color;
    return L.out;
}
