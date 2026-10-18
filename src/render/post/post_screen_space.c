// Screen-space lighting passes (frame graph nodes, before the lit opaque pass):
//   hiz       closest-depth pyramid (SSR traversal)
//   contact   sun contact shadows (full res)
//   gtao      ambient occlusion, half (or full) res, thin-occluder aware -> separable bilateral blur
//             (depth + normal, x then y) -> temporal (ping-pong history) -> bilateral upsample (half res)
//   ssgi      diffuse GI (Ultra), half res -> temporal -> upsample
//   ssr       reflections, half (Low) / full (High) res -> temporal -> (upsample)
// They read the G-buffer prepass and last frame's colour pyramid / depth (post_capture_scene).
#include "post_internal.h"
#include <string.h>

typedef struct SsCtx {
    FgRes depth, normal, material, velocity, hiz, contact;
    FgRes ao_half, ao_blur_x, ao_blur, ao_hist_in, ao_hist_out, ao;
    FgRes gi_half, gi_hist_in, gi_hist_out, gi;
    FgRes ssr_raw, ssr_hist_in, ssr_hist_out, ssr;
    FgRes color_prev, depth_prev;
    bool ssr_half;
} SsCtx;
static SsCtx S;

static void push2(VkCommandBuffer cmd, u32 a, u32 b, float fa, float fb) {
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.p[0] = fa;
    pc.p[1] = fb;
    pc.u[0] = a;
    pc.u[1] = b;
    post_push_constants(cmd, &pc, 128);
}

// ------------------------------------------------------------------ hi-z

static void exec_hiz(VkCommandBuffer cmd, void *u) {
    (void)u;
    u32 w, h;
    fg_extent(S.hiz, &w, &h);
    u32 mips = fg_mips(S.hiz);
    for (u32 m = 0; m < mips; m++) {
        PostBind b = { 0 };
        pb_ubo(&b);
        pb_tex_fg(&b, 0, S.depth, P.point);
        pb_img(&b, 8, fg_mip_view(S.hiz, m == 0 ? 0 : m - 1));
        pb_img(&b, 9, fg_mip_view(S.hiz, m));
        pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
        push2(cmd, m, 0, 0, 0);
        post_dispatch(cmd, "hiz", DG_MAX(1u, w >> m), DG_MAX(1u, h >> m), 1, 8);
        fg_mip_barrier(cmd, S.hiz);
    }
}

// ------------------------------------------------------------------ contact shadows

static void exec_contact(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, S.depth, P.point);
    pb_tex_fg(&b, 1, S.material, P.point);
    pb_tex(&b, 2, P.noise, P.point);
    pb_tex_fg(&b, 3, S.normal, P.point);
    pb_img_fg(&b, 8, S.contact);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(S.contact, &w, &h);
    post_dispatch(cmd, "contact", w, h, 1, 8);
}

// ------------------------------------------------------------------ generic temporal + upsample

typedef struct TempJob { FgRes cur, hist_in, hist_out; float weight, depth_tol; bool r_only; } TempJob;
typedef struct UpJob { FgRes low, out; bool r_only; } UpJob;

static void run_temporal(VkCommandBuffer cmd, const TempJob *j) {
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, j->cur, P.linear);
    pb_tex_fg(&b, 1, j->hist_in, P.linear);
    pb_tex_fg(&b, 2, S.velocity, P.point);
    pb_tex_fg(&b, 3, S.depth, P.point);
    pb_tex_fg(&b, 4, fg_valid(S.depth_prev) ? S.depth_prev : S.depth, P.point);
    pb_img_fg(&b, 8, j->hist_out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    bool valid = fg_history_valid(j->hist_in) && fg_valid(S.depth_prev) && fg_history_valid(S.depth_prev);
    push2(cmd, valid ? 1u : 0u, j->r_only ? 1u : 4u, j->weight, j->depth_tol);
    u32 w, h;
    fg_extent(j->hist_out, &w, &h);
    post_dispatch(cmd, "temporal", w, h, 1, 8);
}

static void run_upsample(VkCommandBuffer cmd, const UpJob *j) {
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, j->low, P.linear);
    pb_tex_fg(&b, 1, S.depth, P.point);
    pb_img_fg(&b, 8, j->out);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    push2(cmd, j->r_only ? 1u : 4u, 0, 0, 0);
    u32 w, h;
    fg_extent(j->out, &w, &h);
    post_dispatch(cmd, "upsample", w, h, 1, 8);
}

// ------------------------------------------------------------------ gtao

static void exec_gtao(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, S.depth, P.point);
    pb_tex_fg(&b, 1, S.normal, P.point);
    pb_tex_fg(&b, 2, S.material, P.point);
    pb_tex(&b, 3, P.noise, P.point);
    pb_img_fg(&b, 8, S.ao_half);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(S.ao_half, &w, &h);
    post_dispatch(cmd, "gtao", w, h, 1, 8);
}
// Separable bilateral blur (ao_blur.comp): u = 0 horizontal (raw -> blur_x), 1 vertical (blur_x -> blur).
static void exec_ao_blur(VkCommandBuffer cmd, void *u) {
    u32 axis = (u32)(uintptr_t)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, axis ? S.ao_blur_x : S.ao_half, P.point);
    pb_tex_fg(&b, 1, S.depth, P.point);
    pb_tex_fg(&b, 2, S.normal, P.point);
    pb_img_fg(&b, 8, axis ? S.ao_blur : S.ao_blur_x);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    push2(cmd, axis, 0, 0, 0);
    u32 w, h;
    fg_extent(S.ao_blur, &w, &h);
    post_dispatch(cmd, "ao_blur", w, h, 1, 8);
}
static void exec_ao_temporal(VkCommandBuffer cmd, void *u) {
    (void)u;
    TempJob j = { S.ao_blur, S.ao_hist_in, S.ao_hist_out, P.set.ao_temporal, 0.08f, true };
    run_temporal(cmd, &j);
}
static void exec_ao_up(VkCommandBuffer cmd, void *u) {
    (void)u;
    UpJob j = { S.ao_hist_out, S.ao, true };
    run_upsample(cmd, &j);
}

// ------------------------------------------------------------------ ssgi

static void exec_ssgi(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, S.depth, P.point);
    pb_tex_fg(&b, 1, S.normal, P.point);
    pb_tex_fg(&b, 2, S.material, P.point);
    pb_tex_fg(&b, 3, S.color_prev, P.linear);
    pb_tex_fg(&b, 4, S.velocity, P.point);
    pb_tex(&b, 5, P.noise, P.point);
    pb_img_fg(&b, 8, S.gi_half);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(S.gi_half, &w, &h);
    post_dispatch(cmd, "ssgi", w, h, 1, 8);
}
static void exec_gi_temporal(VkCommandBuffer cmd, void *u) {
    (void)u;
    TempJob j = { S.gi_half, S.gi_hist_in, S.gi_hist_out, 0.94f, 0.1f, false };
    run_temporal(cmd, &j);
}
static void exec_gi_up(VkCommandBuffer cmd, void *u) {
    (void)u;
    UpJob j = { S.gi_hist_out, S.gi, false };
    run_upsample(cmd, &j);
}

// ------------------------------------------------------------------ ssr

static void exec_ssr(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, S.depth, P.point);
    pb_tex_fg(&b, 1, S.normal, P.point);
    pb_tex_fg(&b, 2, S.material, P.point);
    pb_tex_fg(&b, 3, S.hiz, P.point);
    pb_tex_fg(&b, 4, S.color_prev, P.linear);
    pb_tex_fg(&b, 5, S.velocity, P.point);
    pb_tex(&b, 6, P.noise, P.point);
    pb_img_fg(&b, 8, S.ssr_raw);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(S.ssr_raw, &w, &h);
    post_dispatch(cmd, "ssr", w, h, 1, 8);
}
static void exec_ssr_temporal(VkCommandBuffer cmd, void *u) {
    (void)u;
    TempJob j = { S.ssr_raw, S.ssr_hist_in, S.ssr_hist_out, 0.85f, 0.05f, false };
    run_temporal(cmd, &j);
}

// ------------------------------------------------------------------ build

static FgRes tex(const char *name, VkFormat f, FgSize size, u32 mips) {
    FgTexDesc d = { name, f, size, 0, 0, 0, mips, 0 };
    return fg_texture(&d);
}
static FgRes hist(const char *name, VkFormat f, FgSize size) {
    FgTexDesc d = { name, f, size, 0, 0, 0, 1, 0 };
    return fg_history(&d);
}

void post_ss_build(PostLitInputs *out) {
    memset(out, 0, sizeof(*out));
    memset(&S, 0, sizeof(S));
    const PostSettings *s = &P.set;
    S.depth = P.in.depth;
    S.normal = P.in.normal;
    S.material = P.in.material;
    S.velocity = P.in.velocity;
    S.hiz = S.contact = S.ao = S.gi = S.ssr = FG_NONE;
    // Last frame's colour pyramid / depth (written by post_capture_scene), declared only when this
    // frame has a reader (Engine Fidelity: nothing allocated that nobody uses).
    S.color_prev = S.depth_prev = FG_NONE;
    if (s->ssr || s->ssgi) {
        FgTexDesc cd = { "color_prev", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_RENDER, 0, 0, 0, 6, 0 };
        S.color_prev = fg_history(&cd);
    }
    if (post_needs_depth_history()) {
        FgTexDesc dd = { "depth_prev", VK_FORMAT_R32_SFLOAT, FG_SIZE_RENDER, 0, 0, 0, 1, 0 };
        S.depth_prev = fg_history(&dd);
    }
    P.color_prev = S.color_prev;
    P.depth_prev = S.depth_prev;
    bool parity = (P.in.frame & 1u) != 0;

    if (s->ssr) {
        S.hiz = tex("hiz", VK_FORMAT_R32_SFLOAT, FG_SIZE_RENDER, FG_MAX_MIPS);
        int p = fg_pass("hiz", exec_hiz, NULL);
        fg_read(p, S.depth, FG_SAMPLED);
        fg_write(p, S.hiz, FG_STORAGE);
        if (p < 0) S.hiz = FG_NONE;
    }
    if (s->contact_shadows) {
        S.contact = tex("contact", VK_FORMAT_R8_UNORM, s->contact_shadows >= 2 ? FG_SIZE_RENDER : FG_SIZE_HALF, 1);
        int p = fg_pass("contact", exec_contact, NULL);
        fg_read(p, S.depth, FG_SAMPLED);
        fg_read(p, S.material, FG_SAMPLED);
        fg_read(p, S.normal, FG_SAMPLED);
        fg_write(p, S.contact, FG_STORAGE);
        if (p >= 0) { out->contact = S.contact; out->contact_on = true; }
    }
    if (s->ao) {
        // GTAO (raw) -> blur x -> blur y -> temporal -> upsample; at full resolution the lit pass samples the
        // last stage (the temporal history, or the blur without temporal accumulation). r_post_disable
        // ao_blur_x / ao_blur_y / gtao_temporal skips a stage (its input goes on unchanged).
        FgSize asz = s->ao_full_res ? FG_SIZE_RENDER : FG_SIZE_HALF;
        S.ao_half = tex("ao_raw", VK_FORMAT_R16_SFLOAT, asz, 1);
        int p = fg_pass("gtao", exec_gtao, NULL);
        fg_read(p, S.depth, FG_SAMPLED);
        fg_read(p, S.normal, FG_SAMPLED);
        fg_read(p, S.material, FG_SAMPLED);
        fg_write(p, S.ao_half, FG_STORAGE);
        FgRes cur = S.ao_half;
        P.ao_raw = S.ao_half;
        int bx = p >= 0 && s->ao_blur_radius > 0 ? fg_pass("ao_blur_x", exec_ao_blur, (void *)(uintptr_t)0) : -1;
        if (bx >= 0) {
            S.ao_blur_x = tex("ao_blur_x", VK_FORMAT_R16_SFLOAT, asz, 1);
            fg_read(bx, S.ao_half, FG_SAMPLED);
            fg_read(bx, S.depth, FG_SAMPLED);
            fg_read(bx, S.normal, FG_SAMPLED);
            fg_write(bx, S.ao_blur_x, FG_STORAGE);
            cur = S.ao_blur_x;
            int by = fg_pass("ao_blur_y", exec_ao_blur, (void *)(uintptr_t)1);
            if (by >= 0) {
                S.ao_blur = tex("ao_blur", VK_FORMAT_R16_SFLOAT, asz, 1);
                fg_read(by, S.ao_blur_x, FG_SAMPLED);
                fg_read(by, S.depth, FG_SAMPLED);
                fg_read(by, S.normal, FG_SAMPLED);
                fg_write(by, S.ao_blur, FG_STORAGE);
                cur = S.ao_blur;
            }
        }
        S.ao_blur = cur;
        P.ao_blur = cur;
        bool ok = p >= 0;
        if (p >= 0 && s->ao_temporal > 0.0f && S.depth_prev != FG_NONE) {
            S.ao_hist_in = hist(parity ? "ao_hist_a" : "ao_hist_b", VK_FORMAT_R16_SFLOAT, asz);
            S.ao_hist_out = hist(parity ? "ao_hist_b" : "ao_hist_a", VK_FORMAT_R16_SFLOAT, asz);
            int t = fg_pass("gtao_temporal", exec_ao_temporal, NULL);
            fg_read(t, S.ao_blur, FG_SAMPLED);
            fg_read(t, S.ao_hist_in, FG_SAMPLED);
            fg_read(t, S.velocity, FG_SAMPLED);
            fg_read(t, S.depth, FG_SAMPLED);
            fg_read(t, S.depth_prev, FG_SAMPLED);
            fg_write(t, S.ao_hist_out, FG_STORAGE);
            if (t >= 0) cur = S.ao_hist_out;
        }
        if (!s->ao_full_res) {
            S.ao_hist_out = cur;   // the upsample's input
            S.ao = tex("ao", VK_FORMAT_R8_UNORM, FG_SIZE_RENDER, 1);
            int u = fg_pass("gtao_upsample", exec_ao_up, NULL);
            fg_read(u, cur, FG_SAMPLED);
            fg_read(u, S.depth, FG_SAMPLED);
            fg_write(u, S.ao, FG_STORAGE);
            ok = ok && u >= 0;
        } else {
            S.ao = cur;
        }
        if (ok) { out->ao = S.ao; out->ao_on = true; }
    }
    if (s->ssgi) {
        S.gi_half = tex("ssgi_half", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_HALF, 1);
        S.gi_hist_in = hist(parity ? "ssgi_hist_a" : "ssgi_hist_b", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_HALF);
        S.gi_hist_out = hist(parity ? "ssgi_hist_b" : "ssgi_hist_a", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_HALF);
        S.gi = tex("ssgi", VK_FORMAT_R16G16B16A16_SFLOAT, FG_SIZE_RENDER, 1);
        int p = fg_pass("ssgi", exec_ssgi, NULL);
        fg_read(p, S.depth, FG_SAMPLED);
        fg_read(p, S.normal, FG_SAMPLED);
        fg_read(p, S.material, FG_SAMPLED);
        fg_read(p, S.color_prev, FG_SAMPLED);
        fg_read(p, S.velocity, FG_SAMPLED);
        fg_write(p, S.gi_half, FG_STORAGE);
        int t = fg_pass("ssgi_temporal", exec_gi_temporal, NULL);
        fg_read(t, S.gi_half, FG_SAMPLED);
        fg_read(t, S.gi_hist_in, FG_SAMPLED);
        fg_read(t, S.velocity, FG_SAMPLED);
        fg_read(t, S.depth, FG_SAMPLED);
        fg_read(t, S.depth_prev, FG_SAMPLED);
        fg_write(t, S.gi_hist_out, FG_STORAGE);
        int u = fg_pass("ssgi_upsample", exec_gi_up, NULL);
        fg_read(u, S.gi_hist_out, FG_SAMPLED);
        fg_read(u, S.depth, FG_SAMPLED);
        fg_write(u, S.gi, FG_STORAGE);
        if (p >= 0 && t >= 0 && u >= 0 && fg_history_valid(S.color_prev)) { out->ssgi = S.gi; out->ssgi_on = true; }
    }
    if (s->ssr && S.hiz != FG_NONE) {
        S.ssr_half = s->ssr < 2;
        FgSize sz = S.ssr_half ? FG_SIZE_HALF : FG_SIZE_RENDER;
        S.ssr_raw = tex("ssr_raw", VK_FORMAT_R16G16B16A16_SFLOAT, sz, 1);
        S.ssr_hist_in = hist(parity ? "ssr_hist_a" : "ssr_hist_b", VK_FORMAT_R16G16B16A16_SFLOAT, sz);
        S.ssr_hist_out = hist(parity ? "ssr_hist_b" : "ssr_hist_a", VK_FORMAT_R16G16B16A16_SFLOAT, sz);
        int p = fg_pass("ssr", exec_ssr, NULL);
        fg_read(p, S.depth, FG_SAMPLED);
        fg_read(p, S.normal, FG_SAMPLED);
        fg_read(p, S.material, FG_SAMPLED);
        fg_read(p, S.hiz, FG_SAMPLED);
        fg_read(p, S.color_prev, FG_SAMPLED);
        fg_read(p, S.velocity, FG_SAMPLED);
        fg_write(p, S.ssr_raw, FG_STORAGE);
        int t = fg_pass("ssr_temporal", exec_ssr_temporal, NULL);
        fg_read(t, S.ssr_raw, FG_SAMPLED);
        fg_read(t, S.ssr_hist_in, FG_SAMPLED);
        fg_read(t, S.velocity, FG_SAMPLED);
        fg_read(t, S.depth, FG_SAMPLED);
        fg_read(t, S.depth_prev, FG_SAMPLED);
        fg_write(t, S.ssr_hist_out, FG_STORAGE);
        // Half resolution (low quality) is sampled bilinearly by the lit pass: a bilateral upsample
        // pass costs more than it gains at this quality.
        S.ssr = S.ssr_hist_out;
        if (p >= 0 && t >= 0 && S.ssr != FG_NONE && fg_history_valid(S.color_prev)) { out->ssr = S.ssr; out->ssr_on = true; }
    }
    P.ao = out->ao;
    P.contact = out->contact;
    P.ssr = out->ssr;
    P.ssgi = out->ssgi;
}

// ------------------------------------------------------------------ scene capture (end of the world passes)

static FgRes g_cap_src;
static void exec_pyramid(VkCommandBuffer cmd, void *u) {
    (void)u;
    u32 w, h;
    fg_extent(P.color_prev, &w, &h);
    u32 mips = fg_mips(P.color_prev);
    for (u32 m = 0; m < mips; m++) {
        PostBind b = { 0 };
        pb_ubo(&b);
        pb_tex_fg(&b, 0, g_cap_src, P.point);
        pb_img(&b, 8, fg_mip_view(P.color_prev, m == 0 ? 0 : m - 1));
        pb_img(&b, 9, fg_mip_view(P.color_prev, m));
        pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
        push2(cmd, m, 0, 0, 0);
        post_dispatch(cmd, "color_pyramid", DG_MAX(1u, w >> m), DG_MAX(1u, h >> m), 1, 8);
        fg_mip_barrier(cmd, P.color_prev);
    }
}
static void exec_copy_depth(VkCommandBuffer cmd, void *u) {
    (void)u;
    PostBind b = { 0 };
    pb_ubo(&b);
    pb_tex_fg(&b, 0, P.in.depth, P.point);
    pb_img_fg(&b, 8, P.depth_prev);
    pb_push(cmd, &b, VK_PIPELINE_BIND_POINT_COMPUTE);
    u32 w, h;
    fg_extent(P.depth_prev, &w, &h);
    post_dispatch(cmd, "copy_depth", w, h, 1, 8);
}

// Called after the world (opaque + fog + transparency), before the viewmodel: this frame's
// colour / depth become next frame's SSR / SSGI / temporal inputs.
void post_capture_scene(FgRes hdr) {
    if (!P.ok || P.set.direct_output) return;
    g_cap_src = hdr;
    if ((P.set.ssr || P.set.ssgi) && P.color_prev != FG_NONE) {
        int p = fg_pass("color_pyramid", exec_pyramid, NULL);
        fg_read(p, hdr, FG_SAMPLED);
        fg_write(p, P.color_prev, FG_STORAGE);
    }
    if (P.depth_prev == FG_NONE) return;   // no temporal pass / TAA reads it next frame
    int d = fg_pass("depth_history", exec_copy_depth, NULL);
    fg_read(d, P.in.depth, FG_SAMPLED);
    fg_write(d, P.depth_prev, FG_STORAGE);
}

void post_screen_space(PostLitInputs *out) {
    if (!P.ok || P.set.direct_output) {   // Engine Fidelity Off: no screen-space passes, no froxels
        memset(out, 0, sizeof(*out));
        out->ao = out->contact = out->ssr = out->ssgi = FG_NONE;
        return;
    }
    post_ss_build(out);
    post_vol_build();
}
