// Post-processing / screen-space stack (frame graph nodes, src/render/post/*.c, shaders/post/*).
//
// Pass order (see docs/RENDERING_POST.md for the reasoning and budgets):
//   prepass (depth, normals, material, velocity)  ->  hi-z  ->  contact shadows  ->  GTAO (+ blur)  ->
//   SSGI  ->  SSR  ->  volumetric froxels  ->  lit opaque (consumes AO / contact / SSR / SSGI)  ->
//   fog apply  ->  transparency + particles  ->  viewmodel  ->  TAA / TAA-U / FXAA / SMAA / FSR1  ->
//   motion blur  ->  depth of field  ->  bloom  ->  auto exposure  ->  lens flare  ->  tonemap +
//   grade (+ CAS)  ->  final (grain, dither, HDR10 / sRGB) + UI
// Everything after the lit pass is linear HDR until the tonemapper.
#ifndef DG_POST_H
#define DG_POST_H

#include "render/vk_internal.h"
#include "render/graph/fg.h"
#include "render/post/post_settings.h"

// Frame inputs owned by the renderer (imported / created in render.c's frame graph).
typedef struct PostFrameIn {
    FgRes hdr, depth, normal, material, velocity, hdr_opaque, vm_depth;
    FgRes swap;                   // swapchain (final pass)
    u32 render_w, render_h, output_w, output_h;
    mat4 view, proj, view_proj, inv_view_proj, view_proj_nj, prev_view_proj_nj, inv_proj;
    vec3 cam_pos;
    vec2 jitter, prev_jitter;     // NDC
    float znear, zfar, fov_y;
    vec3 sun_dir, sun_color;
    float sun_intensity;
    vec3 sky, ground;
    float ambient;
    float dt, time;
    u32 frame;
    bool camera_cut;              // teleport / respawn / map change: reset temporal history
    bool ads;
    float ads_focus_dist;         // ADS: distance to the crosshair target
    bool sky_on;                  // procedural sky where depth = 1
    vec3 sky_zenith, sky_horizon;
    float clouds;
} PostFrameIn;

// Outputs the lit pass consumes (render resolution).
typedef struct PostLitInputs { FgRes ao, contact, ssr, ssgi; bool ao_on, contact_on, ssr_on, ssgi_on; } PostLitInputs;

bool post_init(void);
void post_shutdown(void);
bool post_create_pipelines(void);
void post_targets_changed(void);

// Frame graph construction (called by render_end_frame in order).
void post_begin_frame(const PostFrameIn *in);
void post_screen_space(PostLitInputs *out);        // hi-z, contact, gtao, ssgi, ssr
void post_volumetrics(void);                       // volumetric froxels (they sample the sun shadow map: after its marker)
void post_sky(void);                               // after the opaque scene: procedural sky on empty pixels
void post_scope_sky(FgRes color, FgRes depth, mat4 inv_vp, vec3 cam);
void post_view_sky(int view, FgRes color, FgRes depth, mat4 inv_vp, vec3 cam);   // phone camera views   // the same sky into the scope PiP target
// Weapon sight of this frame (post_sights.c): drawn after the DoF, before bloom.
void post_set_sight(const RenderSight *s, FgRes pip, mat4 pip_from_view, mat4 vm_proj, float pip_fov);
bool post_sights_ads_dof(float *focus, float *fstop, float *near_blur);   // ADS depth of field active (complete aim)
// The weapon's sharp depth (m along the arms camera; 0 none) and the arms projection's depth terms
// (m10, m14) for reading the viewmodel depth back as a distance.
float post_sights_vm_focus(float *m10, float *m14);
void post_sights_shutdown(void);
void post_weather_shutdown(void);
void post_fog_apply(void);                         // after the lit opaque pass
void post_capture_scene(FgRes hdr);                // after transparency: colour pyramid + depth history (next frame)
FgRes post_resolve(void);                          // AA / upscale ... tonemap -> LDR (output res)
void post_final_draw(VkCommandBuffer cmd, FgRes ldr); // inside the swapchain rendering pass
void post_push_lit_inputs(VkCommandBuffer cmd, const PostLitInputs *in, bool froxel_fog);
FgRes post_froxels(void);                          // integrated froxel volume (particles)
bool post_froxel_image(bool use, VkDescriptorImageInfo *out);   // its view for a draw after fog_apply (false: none / !use, any 3D view)
VkSampler post_linear_sampler(void);
VkImageView post_white_view(void);
VkImageView post_black_view(void);
// Projection jitter of this frame (render pixels, Halton 2,3) when a temporal pass is active.
vec2 post_jitter_pixels(u32 frame);
bool post_wants_jitter(void);
float post_render_scale(void);
// Engine Fidelity (docs/ENGINE_FIDELITY.md): Off = direct output (no post passes at all), and what
// this frame's settings need from the scene passes (nothing scheduled that nobody reads).
bool post_direct(void);
bool post_needs_velocity(void);                    // G-buffer velocity: TAA / TAA-U, motion blur, temporal AO / SSR / SSGI
bool post_needs_depth_history(void);               // last frame's depth: the temporal passes and TAA
float post_direct_exposure(void);                  // Off: fixed exposure multiplier (CPU, from the sun / sky light)
// Diagnostics (profiler overlay, tests)
float post_exposure_ev(void);                      // last known exposure (EV100), host readback
bool post_lut_load_cube(const char *path, int slot);   // NULL = identity
bool post_hdr_capture(const char *exr_path);       // next frame: write the linear HDR output as EXR
bool post_exr_capture_armed(void);                 // this frame copies an EXR capture (wait for the fence)
void post_exr_finish(void);                        // after the fence: write the file

#endif
