// Shared by src/render/post/*.c. One universal push-descriptor set layout for every post
// shader (no descriptor allocation / churn):
//   0..7   combined image samplers (any dimension; 7 is the sun shadow map, compare sampler)
//   8..11  storage images
//   12     PostUBO (this frame)
//   13,14  storage buffers (histogram, exposure, focus, ...)
#ifndef DG_POST_INTERNAL_H
#define DG_POST_INTERNAL_H

#include "post.h"

#define POST_MAX_PIPES 96

// Must match shaders/post/post_common.glsl (std140).
typedef struct PostUBO {
    mat4 view, proj, view_proj, inv_view_proj, inv_proj, view_proj_nj, prev_view_proj_nj, inv_view_proj_nj, shadow_vp;
    vec4 cam_pos;        // w time
    vec4 screen;         // render w, h, 1/w, 1/h
    vec4 output;         // output w, h, 1/w, 1/h
    vec4 jitter;         // NDC: xy current, zw previous
    vec4 depth;          // near, far, tan(fov_y/2), frame index
    vec4 sun_dir;        // w intensity
    vec4 sun_color;      // w shadows enabled
    vec4 sky;            // w ambient intensity
    vec4 ground;         // w light count
    vec4 misc;           // x dt, y camera cut (reset temporal), z ADS, w aspect
    vec4 s_ao;           // radius, intensity, quality, steps
    vec4 s_ssr;          // intensity, max roughness, quality, thickness
    vec4 s_gi;           // intensity, quality, contact length, contact steps
    vec4 s_vol;          // density, height, falloff, anisotropy
    vec4 s_vol2;         // albedo rgb, sun shafts
    vec4 s_vol3;         // froxel w, h, d, max distance
    vec4 s_mb;           // shutter, mode, max blur px, tile size
    vec4 s_dof;          // focus distance, f-stop, focal length mm, max coc px
    vec4 s_dof2;         // autofocus, bokeh, mode, sensor height mm
    vec4 s_bloom;        // intensity, dirt, mips, quality
    vec4 s_exp;          // compensation EV, manual EV, adapt up, adapt down
    vec4 s_exp2;         // min EV, max EV, low percentile, high percentile
    vec4 s_lens;         // flare, anamorphic, chromatic, vignette
    vec4 s_lens2;        // distortion, grain, sharpen, unused
    vec4 s_tm;           // tonemapper, wb temp, wb tint, saturation
    vec4 s_lift;         // w contrast
    vec4 s_gamma;        // w lut weight
    vec4 s_gain;         // w colorblind mode
    vec4 s_out;          // hdr output, paper white nits, max nits, user brightness
    vec4 s_fb;           // damage, low health, flash, hitmarker
    vec4 s_fb2;          // sprint, underwater, night vision, desaturate
    vec4 s_fb_tint;      // rgb, w user gamma
    vec4 s_dbg;          // view, split, debug channel scale, fog ambient (weather)
    vec4 lights[MAX_POINT_LIGHTS * 2];   // pos radius, color intensity
} PostUBO;

typedef struct PostBind {
    VkWriteDescriptorSet w[16];
    VkDescriptorImageInfo img[16];
    VkDescriptorBufferInfo buf[16];
    int n;
} PostBind;

typedef struct PostPipe {
    char name[48];
    bool compute;
    VkFormat color;        // graphics: color attachment format
    int blend;             // graphics: 0 none, 1 additive, 2 premultiplied alpha
    VkPipeline pipe;
} PostPipe;

typedef struct PostCtx {
    bool ok;
    PostSettings set, pending;
    PostFrameIn in;
    VkDescriptorSetLayout layout;
    VkPipelineLayout pl;
    PostPipe pipes[POST_MAX_PIPES];
    int pipe_count;
    GpuBuffer ubo[FRAMES_IN_FLIGHT];
    GpuBuffer exposure_buf;           // device: [0] exposure (linear), [1] EV, [2] focus distance, [3] focus target
    GpuBuffer exposure_rb[FRAMES_IN_FLIGHT];
    GpuBuffer histogram_buf;          // 256 bins
    VkSampler linear, point, linear_repeat, shadow_cmp;
    // shared textures
    VkImage white_img, black_img, noise_img, lut_img, dirt_img, smaa_area_img, smaa_search_img;
    VmaAllocation white_alloc, black_alloc, noise_alloc, lut_alloc, dirt_alloc, smaa_area_alloc, smaa_search_alloc;
    VkImageView white, black, noise, lut, dirt, smaa_area, smaa_search;
    int lut_count;
    // per frame
    FgRes hiz, ao, contact, ssr, ssgi, froxels, color_prev, depth_prev, vol_history;
    float last_ev;
    bool exposure_init;
    char exr_path[512];
    int exr_pending;
} PostCtx;

extern PostCtx P;

// Pipelines (lazily created, rebuilt on shader hot reload). Names are shaders/post/<name>.
VkPipeline post_compute_pipe(const char *name);
VkPipeline post_graphics_pipe(const char *frag, VkFormat color, int blend);
// Descriptor push helpers.
void pb_tex(PostBind *b, u32 binding, VkImageView view, VkSampler s);
void pb_tex_fg(PostBind *b, u32 binding, FgRes r, VkSampler s);   // falls back to white when r is invalid
void pb_tex_general(PostBind *b, u32 binding, VkImageView view, VkSampler s);   // persistent image kept in GENERAL layout (sky)
void pb_tex_mip(PostBind *b, u32 binding, FgRes r, u32 mip, VkSampler s);   // GENERAL layout (mip chains)
void pb_img(PostBind *b, u32 binding, VkImageView view);
void pb_img_fg(PostBind *b, u32 binding, FgRes r);
void pb_buf(PostBind *b, u32 binding, VkBuffer buf, VkDeviceSize size, bool uniform);
void pb_ubo(PostBind *b);
void pb_push(VkCommandBuffer cmd, PostBind *b, VkPipelineBindPoint bp);
void post_push_constants(VkCommandBuffer cmd, const void *data, u32 size);
void post_dispatch(VkCommandBuffer cmd, const char *pipe, u32 w, u32 h, u32 d, u32 group);   // ceil(w / group) x ceil(h / group) x d
void post_fullscreen(VkCommandBuffer cmd, VkPipeline pipe, VkImageView color, u32 w, u32 h, bool load);
float post_halton(u32 index, u32 base);
void post_buffer_barrier(VkCommandBuffer cmd);

// Pass registration (each file).
void post_ss_build(PostLitInputs *out);       // post_screen_space.c: hi-z, contact, gtao, ssgi, ssr
void post_vol_build(void);                     // post_volumetrics.c
FgRes post_aa_build(FgRes color);              // post_aa.c: TAA / TAA-U / FXAA / SMAA / FSR1 (-> output res)
FgRes post_mb_build(FgRes color);              // post_camera.c: motion blur
FgRes post_dof_build(FgRes color);             // post_camera.c: depth of field
FgRes post_sights_build(FgRes color);          // post_sights.c: red dot / scope (after DoF, before bloom)
FgRes post_lens_build(FgRes color);           // post_weather.c: lens droplets (after the sights)
FgRes post_bloom_build(FgRes color);           // post_bloom.c (returns the bloom texture)
void post_exposure_build(FgRes color);         // post_exposure.c
FgRes post_flare_build(FgRes bloom);           // post_bloom.c
FgRes post_tonemap_build(FgRes color, FgRes bloom, FgRes flare);   // post_tonemap.c -> LDR / HDR10 (output res)
FgRes post_debug_coc(void);                    // post_camera.c (debug views)
FgRes post_debug_mb_tiles(void);
u32 post_bloom_mips(void);
void post_buffers_init_build(void);            // post_exposure.c: zero the exposure / histogram buffers once
void post_textures_init(void);                 // post_textures.c: noise, LUT, dirt, SMAA tables
void post_textures_shutdown(void);
bool post_lut_load_cube(const char *path, int slot);

#endif
