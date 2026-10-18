#include "sky.h"
#include "render/post/post_internal.h"
#include "core/log.h"
#include "core/profiler.h"
#include <math.h>
#include <string.h>

// Must match SkyParams in shaders/sky/sky_common.glsl (std430).
typedef struct SkyGpu {
    vec4 sun_dir, moon_dir, sun_disc, moon_disc, cel_x, cel_y, cel_z, clouds, clouds2, cam, ground, ap, cshadow, env, key, clouds3;
} SkyGpu;

typedef struct SkyImage {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;            // all mips (sampling)
    VkImageView mips[8];         // per mip (storage)
    u32 w, h, d, mips_n;
    VkFormat fmt;
} SkyImage;

#define ENV_SIZE 128
#define ENV_MIPS 6
#define SH_VEC4 20

// Cached passes (PF-0026, docs/perf/FINDINGS.md): a pass whose inputs did not change since its last
// run is skipped and its image keeps that run's texels. The key holds the exact bits of every
// SkyParams / PostUBO field the pass's shader reads (shaders/sky/*.comp and the sky_common.glsl
// functions they call: keep these lists in sync) and the versions of the images it samples (bumped
// by every run of the pass writing them). Equal keys = the same arithmetic on the same values: the
// same texels. perf_fix_disable PF-0026 runs every pass every frame again.
#define SKY_KEY_MAX 40
typedef struct SkyKey { u32 n; u32 v[SKY_KEY_MAX]; } SkyKey;
typedef struct SkyCache { bool valid; SkyKey key; } SkyCache;

static void key_f(SkyKey *k, const float *f, u32 n) {
    for (u32 i = 0; i < n && k->n < SKY_KEY_MAX; i++) memcpy(&k->v[k->n++], &f[i], 4);
}
static void key_v4(SkyKey *k, vec4 v) { key_f(k, &v.x, 4); }
static void key_u(SkyKey *k, u32 u) { if (k->n < SKY_KEY_MAX) k->v[k->n++] = u; }
// The pass must run: the fix is off, it never ran, an input differs from its last run, or the key
// filled up (a longer key would be cut short: never trust it).
static bool cache_stale(const SkyCache *c, const SkyKey *k) {
    return !prof_fix_enabled(PFX_SKY_CACHE) || !c->valid || k->n >= SKY_KEY_MAX || c->key.n != k->n || memcmp(c->key.v, k->v, sizeof(u32) * k->n) != 0;
}

static struct {
    bool ok, images_init, luts_valid, env_valid;
    SkyImage trans, ms, view, ap, env_next, env_cur, cshadow, clouds, milky;
    bool milky_baked, clouds_valid;
    // PF-0026: per cached pass its last key, the image versions, this frame's decisions (sky_build)
    SkyCache c_view, c_aerial, c_clouds, c_cshadow, c_env[4];
    SkyKey env_key;              // this frame's environment rows key (exec_env)
    u32 lut_ver, view_ver, clouds_ver, cshadow_ver, env_next_ver, env_finish_ver;
    bool run_view, run_aerial, run_clouds, run_cshadow;
    int last_pass;               // this frame's last sky pass (sky_last_pass)
    GpuBuffer params[FRAMES_IN_FLIGHT];
    GpuBuffer sh;                // host visible: readback for tests
    SkyGpu cpu;
    float last_mie;
    u32 frame, refilters;
    int slice;                   // env rows being refilled (0..3)
    int env_frames;              // frames since the start of a full cycle
    vec2 cs_origin;
    float cs_size;
} SK;

static SkyImage image_create(VkImageType type, u32 w, u32 h, u32 d, u32 mips, VkFormat fmt) {
    SkyImage im;
    memset(&im, 0, sizeof(im));
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = type;
    ici.format = fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = d;
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkd_share_image(&ici);   // written on the async compute queue, read by the lit passes (PF-0027)
    VmaAllocationCreateInfo aci = { 0 };
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(R->vma, &ici, &aci, &im.image, &im.alloc, NULL));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = im.image;
    vci.viewType = type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = mips;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &im.view));
    for (u32 m = 0; m < mips && m < 8; m++) {
        vci.subresourceRange.baseMipLevel = m;
        vci.subresourceRange.levelCount = 1;
        VK_CHECK(vkCreateImageView(R->device, &vci, NULL, &im.mips[m]));
    }
    im.w = w;
    im.h = h;
    im.d = d;
    im.mips_n = mips;
    im.fmt = fmt;
    return im;
}

static void image_destroy(SkyImage *im) {
    if (!im->image) return;
    for (u32 m = 0; m < im->mips_n && m < 8; m++) vkDestroyImageView(R->device, im->mips[m], NULL);
    vkDestroyImageView(R->device, im->view, NULL);
    vmaDestroyImage(R->vma, im->image, im->alloc);
    memset(im, 0, sizeof(*im));
}

bool sky_init(void) {
    if (SK.ok) return true;
    SK.trans = image_create(VK_IMAGE_TYPE_2D, 256, 64, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT);
    SK.ms = image_create(VK_IMAGE_TYPE_2D, 32, 32, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT);
    SK.view = image_create(VK_IMAGE_TYPE_2D, 192, 108, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT);
    SK.ap = image_create(VK_IMAGE_TYPE_3D, 32, 32, 32, 1, VK_FORMAT_R16G16B16A16_SFLOAT);
    SK.env_next = image_create(VK_IMAGE_TYPE_2D, ENV_SIZE, ENV_SIZE, 1, ENV_MIPS, VK_FORMAT_R16G16B16A16_SFLOAT);
    SK.env_cur = image_create(VK_IMAGE_TYPE_2D, ENV_SIZE, ENV_SIZE, 1, ENV_MIPS, VK_FORMAT_R16G16B16A16_SFLOAT);
    SK.cshadow = image_create(VK_IMAGE_TYPE_2D, 256, 256, 1, 1, VK_FORMAT_R32_SFLOAT);
    SK.clouds = image_create(VK_IMAGE_TYPE_2D, 512, 512, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT);
    SK.milky = image_create(VK_IMAGE_TYPE_2D, 1024, 512, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) SK.params[i] = vkr_buffer_create(sizeof(SkyGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    SK.sh = vkr_buffer_create(sizeof(vec4) * SH_VEC4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    memset(SK.sh.mapped, 0, sizeof(vec4) * SH_VEC4);
    SK.last_mie = -1.0f;
    SK.cs_size = 2048.0f;
    SK.ok = true;
    log_info("sky: atmosphere LUTs, environment %dx%d (%d mips), cloud shadow 256^2", ENV_SIZE, ENV_SIZE, ENV_MIPS);
    return true;
}

void sky_shutdown(void) {
    if (!SK.ok) return;
    vkDeviceWaitIdle(R->device);
    image_destroy(&SK.trans);
    image_destroy(&SK.ms);
    image_destroy(&SK.view);
    image_destroy(&SK.ap);
    image_destroy(&SK.env_next);
    image_destroy(&SK.env_cur);
    image_destroy(&SK.cshadow);
    image_destroy(&SK.clouds);
    image_destroy(&SK.milky);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) vkr_buffer_destroy_now(&SK.params[i]);
    vkr_buffer_destroy_now(&SK.sh);
    memset(&SK, 0, sizeof(SK));
}

bool sky_active(void) { return SK.ok && R->env.atmosphere; }
VkImageView sky_env_view(void) { return SK.env_cur.view; }
VkImageView sky_cloud_shadow_view(void) { return SK.cshadow.view; }
VkImageView sky_aerial_view(void) { return SK.ap.view; }
VkBuffer sky_sh_buffer(void) { return SK.sh.buffer; }
VkDeviceSize sky_sh_size(void) { return sizeof(vec4) * SH_VEC4; }
int sky_env_mips(void) { return ENV_MIPS; }
u32 sky_refilter_count(void) { return SK.refilters; }
u32 sky_lut_builds(void) { return SK.lut_ver; }
void sky_cloud_shadow_region(vec2 *origin, float *size) { *origin = SK.cs_origin; *size = SK.cs_size; }

float sky_sh_luminance(float *latest) {
    if (!SK.ok) return 0.0f;
    const vec4 *v = (const vec4 *)SK.sh.mapped;
    if (latest) *latest = v[18].y;
    return v[18].x;
}

// ------------------------------------------------------------------ passes

static void barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    // fg_stages: the async compute queue (PF-0027) keeps only its stages (the lit passes that read
    // these images on the graphics queue follow through the frame graph's timeline waits).
    mb.srcStageMask = fg_stages(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = fg_stages(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

static void init_images(VkCommandBuffer cmd) {
    if (SK.images_init) return;
    SkyImage *all[] = { &SK.trans, &SK.ms, &SK.view, &SK.ap, &SK.env_next, &SK.env_cur, &SK.cshadow, &SK.clouds, &SK.milky };
    for (int i = 0; i < (int)ARRAY_COUNT(all); i++) {
        vkr_image_barrier(cmd, all[i]->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, all[i]->mips_n);
        VkClearColorValue cv = { { 0, 0, 0, 0 } };
        if (all[i] == &SK.cshadow) cv.float32[0] = 1.0f;
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, all[i]->mips_n, 0, 1 };
        vkCmdClearColorImage(cmd, all[i]->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rr);
    }
    barrier(cmd);
    SK.images_init = true;
}

// Common bindings of the sky shaders.
static void bind(VkCommandBuffer cmd, PostBind *b) {
    pb_ubo(b);
    pb_buf(b, 13, SK.params[R->frame_slot].buffer, sizeof(SkyGpu), false);
    pb_push(cmd, b, VK_PIPELINE_BIND_POINT_COMPUTE);
}

static void push_u(VkCommandBuffer cmd, u32 a, u32 c) {
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    pc.u[0] = a;
    pc.u[1] = c;
    post_push_constants(cmd, &pc, 128);
}

static void exec_luts(VkCommandBuffer cmd, void *u) {
    (void)u;
    init_images(cmd);
    PostBind b = { 0 };
    pb_img(&b, 8, SK.trans.view);
    bind(cmd, &b);
    post_dispatch(cmd, "sky/transmittance", 256, 64, 1, 8);
    barrier(cmd);
    PostBind b2 = { 0 };
    pb_tex_general(&b2, 0, SK.trans.view, P.linear);
    pb_img(&b2, 8, SK.ms.view);
    bind(cmd, &b2);
    post_dispatch(cmd, "sky/multiscatter", 32, 32, 1, 8);
    if (!SK.milky_baked) {
        PostBind b3 = { 0 };
        pb_img(&b3, 8, SK.milky.view);
        bind(cmd, &b3);
        post_dispatch(cmd, "sky/milky", 1024, 512, 1, 8);
        SK.milky_baked = true;
    }
    barrier(cmd);
}

static void exec_view(VkCommandBuffer cmd, void *u) {
    (void)u;
    init_images(cmd);
    if (SK.run_view) {
        PostBind b = { 0 };
        pb_tex_general(&b, 0, SK.trans.view, P.linear);
        pb_tex_general(&b, 1, SK.ms.view, P.linear);
        pb_img(&b, 8, SK.view.view);
        bind(cmd, &b);
        post_dispatch(cmd, "sky/skyview", 192, 108, 1, 8);
    }
    if (SK.run_aerial) {
        PostBind b2 = { 0 };
        pb_tex_general(&b2, 0, SK.trans.view, P.linear);
        pb_tex_general(&b2, 1, SK.ms.view, P.linear);
        pb_img(&b2, 8, SK.ap.view);
        bind(cmd, &b2);
        post_dispatch(cmd, "sky/aerial", 32, 32, 1, 8);
    }
    if (SK.run_view || SK.run_aerial) barrier(cmd);
}

static void exec_clouds(VkCommandBuffer cmd, void *u) {
    (void)u;
    init_images(cmd);
    if (!SK.run_clouds) return;
    PostBind b = { 0 };
    pb_tex_general(&b, 0, SK.trans.view, P.linear);
    pb_tex_general(&b, 2, SK.view.view, P.linear_repeat);
    pb_img(&b, 8, SK.clouds.view);
    bind(cmd, &b);
    post_dispatch(cmd, "sky/clouds", 512, 512, 1, 8);
    barrier(cmd);
}

static void exec_cshadow(VkCommandBuffer cmd, void *u) {
    (void)u;
    init_images(cmd);
    if (!SK.run_cshadow) return;
    PostBind b = { 0 };
    pb_img(&b, 8, SK.cshadow.view);
    bind(cmd, &b);
    post_dispatch(cmd, "sky/cloud_shadow", 256, 256, 1, 8);
    barrier(cmd);
}

// Refill rows [32 rows x the slice] of the next map (SkyParams env.y = slice, or the full
// height in one go with rows = 128 on the first frame).
static void env_render(VkCommandBuffer cmd, u32 rows) {
    PostBind b = { 0 };
    pb_tex_general(&b, 0, SK.trans.view, P.linear);
    pb_tex_general(&b, 2, SK.view.view, P.linear_repeat);
    pb_tex_general(&b, 5, SK.clouds.view, P.linear);
    pb_tex_general(&b, 7, SK.cshadow.view, P.linear);
    pb_img(&b, 8, SK.env_next.mips[0]);
    bind(cmd, &b);
    push_u(cmd, 0, 0);
    VkPipeline p = post_compute_pipe("sky/env_render");
    if (!p) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdDispatch(cmd, ENV_SIZE / 8, rows / 8, 1);
}

static void env_finish_cycle(VkCommandBuffer cmd) {
    // Mips of the fresh map, then its SH.
    for (int m = 1; m < ENV_MIPS; m++) {
        barrier(cmd);
        PostBind b = { 0 };
        pb_tex_general(&b, 4, SK.env_next.view, P.linear);
        pb_img(&b, 8, SK.env_next.mips[m]);
        bind(cmd, &b);
        push_u(cmd, (u32)(m - 1), 0);
        u32 s = ENV_SIZE >> m;
        post_dispatch(cmd, "sky/env_mip", s, s, 1, 8);
    }
    barrier(cmd);
    PostBind b = { 0 };
    pb_tex_general(&b, 4, SK.env_next.view, P.linear);
    pb_buf(&b, 14, SK.sh.buffer, sky_sh_size(), false);
    bind(cmd, &b);
    push_u(cmd, 0, 0);
    VkPipeline p = post_compute_pipe("sky/env_sh");
    if (p) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    SK.refilters++;
}

static void env_blend(VkCommandBuffer cmd, bool snap) {
    barrier(cmd);
    for (int m = 0; m < ENV_MIPS; m++) {
        PostBind b = { 0 };
        pb_img(&b, 8, SK.env_next.mips[m]);
        pb_img(&b, 9, SK.env_cur.mips[m]);
        bind(cmd, &b);
        push_u(cmd, (u32)m, snap ? 1u : 0u);
        u32 s = ENV_SIZE >> m;
        post_dispatch(cmd, "sky/env_blend", s, s, 1, 8);
    }
    PostBind b = { 0 };
    pb_tex_general(&b, 4, SK.env_next.view, P.linear);
    pb_buf(&b, 14, SK.sh.buffer, sky_sh_size(), false);
    bind(cmd, &b);
    push_u(cmd, 1, snap ? 1u : 0u);
    VkPipeline p = post_compute_pipe("sky/env_sh");
    if (p) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    barrier(cmd);
}

static void exec_env(VkCommandBuffer cmd, void *u) {
    (void)u;
    init_images(cmd);
    SkyGpu *g = (SkyGpu *)SK.params[R->frame_slot].mapped;
    if (!SK.env_valid) {
        // First frame: the whole map at once, snapped (nothing to ease from).
        g->env.y = 0.0f;
        env_render(cmd, ENV_SIZE);
        env_finish_cycle(cmd);
        env_blend(cmd, true);
        SK.env_valid = true;
        SK.slice = 0;
        for (int s = 0; s < 4; s++) SK.c_env[s] = (SkyCache){ true, SK.env_key };
        SK.env_finish_ver = ++SK.env_next_ver;
        return;
    }
    // PF-0026: a quarter's rows are re-rendered only when their inputs changed since they were last
    // written; a cycle whose rows all stayed keeps the mips and the SH projection it already has. The
    // easing runs every frame (it converges on the GPU: no host-side test of its fixed point).
    SkyCache *c = &SK.c_env[SK.slice];
    if (cache_stale(c, &SK.env_key)) {
        env_render(cmd, 32);
        *c = (SkyCache){ true, SK.env_key };
        SK.env_next_ver++;
    }
    if (SK.slice == 3) {
        if (!prof_fix_enabled(PFX_SKY_CACHE) || SK.env_finish_ver != SK.env_next_ver) {
            env_finish_cycle(cmd);
            SK.env_finish_ver = SK.env_next_ver;
        } else {
            SK.refilters++;   // the same cycle again (sky_refilter_count counts cycles)
        }
    }
    env_blend(cmd, false);
    SK.slice = (SK.slice + 1) & 3;
}

// ------------------------------------------------------------------ frame

static vec4 v4f(vec3 v, float w) { return v4(v.x, v.y, v.z, w); }

// The sky passes need nothing from the frame's graphics work: they run on the async compute queue
// (PF-0027) while the shadow maps render; the lit passes wait for the last one.
static int sky_pass_async(int p) {
    fg_async(p);
    if (p >= 0) SK.last_pass = p;
    return p;
}

void sky_build(void) {
    SK.last_pass = -1;
    if (!sky_active()) return;
    const RenderEnvironment *e = &R->env;
    SkyGpu *g = (SkyGpu *)SK.params[R->frame_slot].mapped;
    memset(g, 0, sizeof(*g));
    g->sun_dir = v4f(v3_norm(e->sun_true_dir), e->sun_illuminance);
    g->moon_dir = v4f(v3_norm(e->moon_dir), e->moon_illuminance);
    float rs = 0.004652f * fmaxf(e->sun_size, 0.1f), rm = 0.004521f * fmaxf(e->moon_size, 0.1f);
    g->sun_disc = v4(cosf(rs), rs, 1.0f, 0.6f);
    g->moon_disc = v4(cosf(rm), rm, e->moon_full_illuminance, 0.012f);
    g->cel_x = v4f(e->cel_x, e->star_brightness);
    g->cel_y = v4f(e->cel_y, e->milky_way);
    g->cel_z = v4f(e->cel_z, e->night_floor);
    g->clouds = v4(e->cloud_cover, e->cloud_altitude, e->cloud_offset.x, e->cloud_offset.y);
    g->clouds3 = v4(e->cloud_softness, e->cloud_darkness, 0.0f, 0.0f);
    g->clouds2 = v4(e->cloud_density > 0.0f ? e->cloud_density : 0.92f, P.in.time, e->cloud_scale > 1.0f ? e->cloud_scale : 3000.0f, e->cloud_shadow_tau);
    vec3 cam = R->cam.pos;
    g->cam = v4(cam.x, cam.y, cam.z, fmaxf(cam.y * 0.001f, 0.0f) + 0.002f);
    vec3 ga = v3_len2(e->ground_albedo) > 0 ? e->ground_albedo : v3(0.25f, 0.24f, 0.22f);   // snow / wet ground (shadow_sky.c)
    g->ground = v4(ga.x, ga.y, ga.z, e->ambient_scale);
    // Aerosols in steps of 0.1: the transmittance / multiple scattering LUTs are rebuilt only when
    // the step changes (every pass reads the same quantized value).
    float mie_q = roundf(fmaxf(e->mie_scale, 0.0f) * 10.0f) / 10.0f;
    g->ap = v4(e->aerial_max > 10.0f ? e->aerial_max : 3000.0f, 0.001f * fmaxf(e->aerial_scale, 0.01f), mie_q, (float)SK.frame);
    // Cloud shadow region: around the camera, snapped to its texels (stable under motion).
    float texel = SK.cs_size / 256.0f;
    SK.cs_origin = v2(floorf(cam.x / texel) * texel - SK.cs_size * 0.5f, floorf(cam.z / texel) * texel - SK.cs_size * 0.5f);
    g->cshadow = v4(SK.cs_origin.x, SK.cs_origin.y, SK.cs_size, 1.0f / SK.cs_size);
    float k = 1.0f - expf(-fmaxf(P.in.dt, 0.0f) / 0.35f);   // environment ease (s)
    g->env = v4(k, (float)SK.slice, (float)ENV_MIPS, (float)e->sky_debug);
    g->key = v4f(v3_norm(e->sun_dir), e->moon_key ? 1.0f : 0.0f);
    SK.cpu = *g;
    SK.frame++;
    if (!SK.luts_valid || fabsf(SK.last_mie - mie_q) > 1e-4f) {
        int p = fg_pass("sky_luts", exec_luts, NULL);
        if (p >= 0) { SK.luts_valid = true; SK.last_mie = mie_q; SK.lut_ver++; }
        sky_pass_async(p);
    }
    // PF-0026: each pass runs when its key (SkyKey: the fields its shader reads, the versions of what
    // it samples) differs from its last run's. A skipped pass keeps its pass node (empty: the
    // profiler's scope stays comparable) and its image the last run's texels.
    // Sky view (skyview.comp): the lights, the camera altitude, the aerosols, the LUTs.
    SkyKey kv = { 0 };
    key_v4(&kv, g->sun_dir);
    key_v4(&kv, g->moon_dir);
    key_f(&kv, &g->cam.w, 1);
    key_f(&kv, &g->ap.z, 1);
    key_u(&kv, SK.lut_ver);
    key_u(&kv, P.pipe_gen);
    // Aerial perspective (aerial.comp): the same + its range / scale and the camera rays (PostUBO
    // inv_view_proj, jittered under TAA: it changes every frame then, cam_pos).
    SkyKey ka = kv;
    key_f(&ka, &g->ap.x, 2);
    key_f(&ka, P.in.inv_view_proj.m, 16);
    key_f(&ka, &P.in.cam_pos.x, 3);
    SK.run_view = cache_stale(&SK.c_view, &kv);
    SK.run_aerial = cache_stale(&SK.c_aerial, &ka);
    if (sky_pass_async(fg_pass("sky_view", exec_view, NULL)) >= 0) {
        if (SK.run_view) { SK.c_view = (SkyCache){ true, kv }; SK.view_ver++; }
        if (SK.run_aerial) SK.c_aerial = (SkyCache){ true, ka };
    }
    // Without cloud cover cloud_density() returns 0 before reading anything else (sky_common.glsl:
    // clamp(cover) <= 0.001): the cloud map is 0 everywhere and the cloud shadow one value for the key
    // light, whatever the drift (it never stops: cloud_wind_min), the camera or the layer. A NaN cover
    // fails the test and keeps the full keys.
    bool no_cover = g->clouds.x <= 0.001f;
    // The cloud map changes slowly: every other frame (clouds.comp: the lights, the layer, the camera
    // position (where the view rays meet the layer) and altitude, the LUTs, the sky view).
    if (!SK.clouds_valid || (SK.frame & 1u) == 0u) {
        SkyKey kc = { 0 };
        key_u(&kc, no_cover ? 1u : 0u);
        if (!no_cover) {
            key_v4(&kc, g->sun_dir);
            key_v4(&kc, g->moon_dir);
            key_v4(&kc, g->clouds);
            key_f(&kc, &g->clouds2.x, 1);
            key_f(&kc, &g->clouds2.z, 1);
            key_f(&kc, &g->clouds3.x, 2);
            key_v4(&kc, g->cam);
            key_u(&kc, SK.lut_ver);
            key_u(&kc, SK.view_ver);
        }
        key_u(&kc, P.pipe_gen);
        SK.run_clouds = cache_stale(&SK.c_clouds, &kc);
        if (sky_pass_async(fg_pass("sky_clouds", exec_clouds, NULL)) >= 0 && SK.run_clouds) { SK.c_clouds = (SkyCache){ true, kc }; SK.clouds_ver++; }
        SK.clouds_valid = true;
    }
    // Cloud shadow (cloud_shadow.comp): its snapped region, the key light, the layer.
    SkyKey ks = { 0 };
    key_u(&ks, no_cover ? 1u : 0u);
    key_f(&ks, &g->key.x, 3);
    key_f(&ks, &g->clouds2.x, 1);   // 0 x opacity x optical depth: a non-finite one would still matter
    key_f(&ks, &g->clouds2.w, 1);
    if (!no_cover) {
        key_f(&ks, &g->cshadow.x, 3);
        key_v4(&ks, g->clouds);
        key_f(&ks, &g->clouds2.z, 1);
        key_f(&ks, &g->clouds3.x, 1);
    }
    key_u(&ks, P.pipe_gen);
    SK.run_cshadow = cache_stale(&SK.c_cshadow, &ks);
    if (sky_pass_async(fg_pass("sky_cshadow", exec_cshadow, NULL)) >= 0 && SK.run_cshadow) { SK.c_cshadow = (SkyCache){ true, ks }; SK.cshadow_ver++; }
    // Environment rows (env_render.comp, the slice is the cache entry): the lights, the night floor, the
    // ground albedo, the camera altitude, everything it samples (exec_env decides).
    SkyKey ke = { 0 };
    key_v4(&ke, g->sun_dir);
    key_v4(&ke, g->moon_dir);
    key_f(&ke, &g->cel_z.w, 1);
    key_v4(&ke, g->ground);
    key_f(&ke, &g->cam.w, 1);
    key_u(&ke, SK.lut_ver);
    key_u(&ke, SK.view_ver);
    key_u(&ke, SK.clouds_ver);
    key_u(&ke, SK.cshadow_ver);
    key_u(&ke, P.pipe_gen);
    SK.env_key = ke;
    sky_pass_async(fg_pass("sky_env", exec_env, NULL));
}

int sky_last_pass(void) { return sky_active() ? SK.last_pass : -1; }

void sky_render_exec(VkCommandBuffer cmd, FgRes color, FgRes depth, mat4 inv_vp, vec3 cam, bool pip) {
    PostBind b = { 0 };
    pb_tex_general(&b, 0, SK.trans.view, P.linear);
    pb_tex_general(&b, 1, SK.ms.view, P.linear);
    pb_tex_general(&b, 2, SK.view.view, P.linear_repeat);
    pb_tex_general(&b, 3, SK.milky.view, P.linear_repeat);
    pb_tex_general(&b, 4, SK.env_cur.view, P.linear);
    pb_tex_general(&b, 5, SK.clouds.view, P.linear);
    pb_tex_fg(&b, 6, depth, P.point);
    pb_tex_general(&b, 7, SK.cshadow.view, P.linear);
    pb_img_fg(&b, 8, color);
    pb_tex(&b, 15, P.moon, P.linear_repeat);
    bind(cmd, &b);
    struct { float p[16]; u32 u[16]; } pc;
    memset(&pc, 0, sizeof(pc));
    memcpy(pc.p, inv_vp.m, sizeof(float) * 16);
    float c3[3] = { cam.x, cam.y, cam.z };
    memcpy(&pc.u[8], c3, 12);
    pc.u[12] = pip ? 1u : 0u;
    post_push_constants(cmd, &pc, 128);
    u32 w, h;
    fg_extent(color, &w, &h);
    post_dispatch(cmd, "sky/sky_render", w, h, 1, 8);
}

vec3 sky_irradiance_cpu(vec3 d) {
    if (!SK.ok) return v3(0, 0, 0);
    const vec4 *c = (const vec4 *)SK.sh.mapped;
    const float A0 = 3.14159265f, A1 = 2.0943951f, A2 = 0.785398163f;
    vec3 e = v3_scale(v3(c[0].x, c[0].y, c[0].z), 0.282095f * A0);
    float y[9] = { 0, 0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x, 1.092548f * d.x * d.y, 1.092548f * d.y * d.z,
                   0.315392f * (3.0f * d.z * d.z - 1.0f), 1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y) };
    for (int i = 1; i < 9; i++) {
        float a = i < 4 ? A1 : A2;
        e = v3_add(e, v3_scale(v3(c[i].x, c[i].y, c[i].z), y[i] * a));
    }
    return v3_max(e, v3(0, 0, 0));
}

void sky_bind_aerial(PostBind *b, u32 binding) { pb_tex_general(b, binding, SK.ap.view, P.linear); }
