// End of the post chain: post_resolve() builds everything after the viewmodel (AA / upscale ->
// motion blur -> DoF -> exposure -> bloom -> flare -> tonemap / grade, or a debug view) and
// returns the display-linear output-resolution image that post_final_draw() encodes into the
// swapchain (CAS, grain, dither, sRGB / PQ). Also the linear HDR EXR capture (photo mode).
#include "post_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    FgRes color, bloom, flare, ldr, compare;
    FgRes dbg_src;
    int dbg_view;
    u32 dbg_level;
    // EXR capture
    FgRes exr_src;
    GpuBuffer exr_buf;
    u32 exr_w, exr_h;
    bool exr_armed;
} T;

static void exec_tonemap(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, T.color, P.linear);
    pb_tex_fg(&b, 1, T.bloom, P.linear);
    pb_tex_fg(&b, 2, T.flare, P.linear);
    pb_tex(&b, 3, P.dirt, P.linear);
    pb_tex(&b, 4, P.lut, P.linear);
    pb_tex(&b, 5, P.noise, P.point);
    pb_tex_fg(&b, 6, T.compare, P.linear);
    pb_img_fg(&b, 8, T.ldr);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = fg_valid(T.bloom) ? 1u : 0u;
    pc.u[1] = fg_valid(T.flare) ? 1u : 0u;
    pc.u[2] = post_bloom_mips();
    pc.u[3] = fg_valid(T.compare) ? 1u : 0u;
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(T.ldr, &w, &h);
    post_dispatch(cmd, "tonemap", w, h, 1, 8);
}

static void exec_debug(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    bool vol = T.dbg_view == 13 || T.dbg_view == 17;
    if (!vol && T.dbg_view == 9) pb_tex_mip(&b, 0, T.dbg_src, 0, P.point);
    else if (!vol) pb_tex_fg(&b, 0, T.dbg_src, P.point);
    else pb_tex(&b, 0, P.black, P.point);
    if (vol && fg_valid(T.dbg_src)) pb_tex_fg(&b, 1, T.dbg_src, P.linear);
    else pb_tex(&b, 1, P.lut, P.linear);   // any 3D texture
    pb_tex_fg(&b, 2, P.in.depth, P.point);
    pb_img_fg(&b, 8, T.ldr);
    pb_buf(&b, 13, P.exposure_buf.buffer, 64, false);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = (u32)T.dbg_view;
    pc.u[1] = T.dbg_level;
    pc.p[0] = 1.0f;
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(T.ldr, &w, &h);
    post_dispatch(cmd, "debug_view", w, h, 1, 8);
}

static void exec_exr_copy(VkCommandBuffer cmd, void *u) {
    (void)u;
    VkBufferImageCopy region = { 0 };
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = T.exr_w;
    region.imageExtent.height = T.exr_h;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, fg_image(T.exr_src), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, T.exr_buf.buffer, 1, &region);
}

// Which frame graph resource a debug view shows (and the hi-z mip / froxel slice).
static FgRes debug_source(int v, FgRes aa_out) {
    switch (v) {
    case 1: return P.ao;
    case 2: return P.contact;
    case 3: return P.ssr;
    case 4: return P.ssgi;
    case 5: return P.in.velocity;
    case 6: return P.in.normal;
    case 7: return P.in.material;
    case 8: return P.in.depth;
    case 9: return P.hiz;
    case 10: return post_debug_coc();
    case 11: return T.color;
    case 12: return aa_out;
    case 13: case 17: return P.froxels;
    case 14: return T.bloom;
    case 15: return T.flare;
    case 16: return post_debug_mb_tiles();
    default: return FG_NONE;
    }
}

static FgRes tex(const char *name, VkFormat f, FgSize s) {
    FgTexDesc d = { name, f, s, 0, 0, 0, 1, 0 };
    return fg_texture(&d);
}

FgRes post_resolve(void) {
    memset(&T, 0xff, sizeof(T));
    T.dbg_view = 0;
    T.dbg_level = 0;
    T.exr_armed = false;
    memset(&T.exr_buf, 0, sizeof(T.exr_buf));
    post_buffers_init_build();
    FgRes c = post_aa_build(P.in.hdr);
    FgRes aa_out = c;
    c = post_mb_build(c);
    c = post_dof_build(c);
    c = post_sights_build(c);
    c = post_lens_build(c);
    T.color = c;
    post_exposure_build(c);
    T.bloom = post_bloom_build(c);
    T.flare = post_flare_build(T.bloom);
    T.compare = P.set.compare_split ? P.in.hdr : FG_NONE;
    T.ldr = tex("ldr", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_OUTPUT);
    int p = fg_pass("tonemap", exec_tonemap, NULL);
    fg_read(p, T.color, FG_SAMPLED);
    fg_read(p, T.bloom, FG_SAMPLED);
    fg_read(p, T.flare, FG_SAMPLED);
    fg_read(p, T.compare, FG_SAMPLED);
    fg_write(p, T.ldr, FG_STORAGE);
    if (p < 0) T.ldr = T.color;   // r_post_disable tonemap: show the linear HDR as is
    // r_post_debug: overwrite the output with the selected intermediate.
    int v = P.set.debug_view;
    if (v > 0 && v < post_debug_count() && p >= 0) {
        T.dbg_view = v;
        T.dbg_src = debug_source(v, aa_out);
        if (v == 9) T.dbg_level = 0;
        if (v == 13) T.dbg_level = 1000;   // froxel slice at 80 % of the depth range
        if (T.dbg_src >= 0) {
            int d = fg_pass("debug_view", exec_debug, NULL);
            fg_read(d, T.dbg_src, v == 9 ? FG_STORAGE : FG_SAMPLED);
            fg_read(d, P.in.depth, FG_SAMPLED);
            fg_write(d, T.ldr, FG_STORAGE);
        }
    }
    // Photo mode / F-key capture: linear HDR (pre-bloom, pre-tonemap) to EXR.
    if (P.exr_pending > 0 && P.exr_path[0]) {
        bool at_render = T.color == P.in.hdr;
        T.exr_w = at_render ? P.in.render_w : P.in.output_w;
        T.exr_h = at_render ? P.in.render_h : P.in.output_h;
        T.exr_buf = vkr_buffer_create_readback((VkDeviceSize)T.exr_w * T.exr_h * 8);
        T.exr_src = T.color;
        int e = fg_pass("exr_capture", exec_exr_copy, NULL);
        fg_read(e, T.color, FG_TRANSFER_SRC);
        T.exr_armed = e >= 0;
        P.exr_pending = 0;
    }
    return T.ldr;
}

bool post_hdr_capture(const char *exr_path) {
    if (!P.ok || !exr_path || !exr_path[0]) return false;
    snprintf(P.exr_path, sizeof(P.exr_path), "%s", exr_path);
    P.exr_pending = 1;
    return true;
}

bool post_exr_capture_armed(void) { return T.exr_armed; }

// ------------------------------------------------------------------ EXR writer
// Minimal OpenEXR 2 scanline file: HALF RGBA, no compression (readable by every EXR tool).

static u16 f32_to_f16(float v) {
    u32 x;
    memcpy(&x, &v, 4);
    u32 sign = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xFF) - 127 + 15;
    u32 m = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return (u16)(sign | 0x7C00u | (m ? 0x200u : 0u));
    if (e <= 0) return (u16)sign;
    if (e >= 31) return (u16)(sign | 0x7C00u);
    return (u16)(sign | ((u32)e << 10) | (m >> 13));
}
static float f16_to_f32(u16 h) {
    u32 sign = (u32)(h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, x;
    if (e == 0) {
        if (m == 0) x = sign;
        else {   // subnormal
            float f = (float)m / 1024.0f / 16384.0f;
            return (h & 0x8000u) ? -f : f;
        }
    } else if (e == 31) x = sign | 0x7F800000u | (m << 13);
    else x = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

static void put_u32(u8 **p, u32 v) { memcpy(*p, &v, 4); *p += 4; }
static void put_u64(u8 **p, u64 v) { memcpy(*p, &v, 8); *p += 8; }
static void put_str(u8 **p, const char *s) { size_t n = strlen(s) + 1; memcpy(*p, s, n); *p += n; }
static void put_attr(u8 **p, const char *name, const char *type, u32 size, const void *data) {
    put_str(p, name);
    put_str(p, type);
    put_u32(p, size);
    memcpy(*p, data, size);
    *p += size;
}

static bool write_exr(const char *path, const u16 *rgba, u32 w, u32 h, float scale) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    u8 hdr[1024];
    u8 *p = hdr;
    put_u32(&p, 20000630u);
    put_u32(&p, 2u);
    // chlist: A B G R (alphabetical), each: name, pixel type HALF=1, pLinear u8 + 3 reserved, xSampling, ySampling
    u8 ch[128];
    u8 *c = ch;
    const char *names[4] = { "A", "B", "G", "R" };
    for (int i = 0; i < 4; i++) {
        put_str(&c, names[i]);
        put_u32(&c, 1u);
        put_u32(&c, 0u);
        put_u32(&c, 1u);
        put_u32(&c, 1u);
    }
    *c++ = 0;
    put_attr(&p, "channels", "chlist", (u32)(c - ch), ch);
    u8 comp = 0;
    put_attr(&p, "compression", "compression", 1, &comp);
    int box[4] = { 0, 0, (int)w - 1, (int)h - 1 };
    put_attr(&p, "dataWindow", "box2i", 16, box);
    put_attr(&p, "displayWindow", "box2i", 16, box);
    u8 lo = 0;
    put_attr(&p, "lineOrder", "lineOrder", 1, &lo);
    float par = 1.0f;
    put_attr(&p, "pixelAspectRatio", "float", 4, &par);
    float swc[2] = { 0, 0 };
    put_attr(&p, "screenWindowCenter", "v2f", 8, swc);
    float sww = 1.0f;
    put_attr(&p, "screenWindowWidth", "float", 4, &sww);
    *p++ = 0;
    size_t header = (size_t)(p - hdr);
    size_t line_bytes = 8 + (size_t)w * 2 * 4;
    fwrite(hdr, 1, header, f);
    u64 off = header + (u64)h * 8;
    for (u32 y = 0; y < h; y++) {
        u8 tmp[8];
        u8 *t = tmp;
        put_u64(&t, off + (u64)y * line_bytes);
        fwrite(tmp, 1, 8, f);
    }
    u16 *line = (u16 *)malloc((size_t)w * 2 * 4);
    bool scaled = fabsf(scale - 1.0f) > 1e-6f;
    for (u32 y = 0; y < h; y++) {
        const u16 *src = rgba + (size_t)y * w * 4;
        static const int order[4] = { 3, 2, 1, 0 };   // A B G R from RGBA
        for (int k = 0; k < 4; k++)
            for (u32 x = 0; x < w; x++) {
                u16 v = src[x * 4 + order[k]];
                if (scaled && k > 0) v = f32_to_f16(f16_to_f32(v) * scale);
                if (k == 0) v = 0x3C00;   // alpha 1
                line[(size_t)k * w + x] = v;
            }
        int32_t yy = (int32_t)y, sz = (int32_t)(w * 2 * 4);
        fwrite(&yy, 4, 1, f);
        fwrite(&sz, 4, 1, f);
        fwrite(line, 2, (size_t)w * 4, f);
    }
    free(line);
    fclose(f);
    return true;
}

// After the frame's fence: write the captured image (exposed, linear Rec.709).
void post_exr_finish(void) {
    if (!T.exr_armed || !T.exr_buf.buffer) return;
    vmaInvalidateAllocation(R->vma, T.exr_buf.alloc, 0, VK_WHOLE_SIZE);
    float ev = P.last_ev;
    float lavg = powf(2.0f, ev) * 12.5f / 100.0f;
    float expo = P.set.exposure_auto ? 0.18f / fmaxf(lavg, 1e-6f) * powf(2.0f, P.set.exposure_comp) : 0.18f / (powf(2.0f, P.set.exposure_manual) * 0.125f);
    if (!P.exposure_init || !(expo > 0.0f) || !isfinite(expo)) expo = 1.0f;
    if (write_exr(P.exr_path, (const u16 *)T.exr_buf.mapped, T.exr_w, T.exr_h, expo)) log_info("post: HDR capture saved: %s (%ux%u, exposure %.3f)", P.exr_path, T.exr_w, T.exr_h, expo);
    else log_error("post: failed to write %s", P.exr_path);
    vkr_buffer_destroy_now(&T.exr_buf);
    T.exr_armed = false;
    P.exr_path[0] = 0;
}

// ------------------------------------------------------------------ final

void post_final_draw(VkCommandBuffer cmd, FgRes ldr) {
    VkPipeline p = post_graphics_pipe("final", R->swap_format, 0);
    if (!p) return;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, ldr, P.point);
    pb_tex(&b, 1, P.noise, P.point);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_GRAPHICS);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = P.set.debug_view > 0 ? 1u : 0u;
    post_push_constants(cmd, &pc, 128);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
