// Public settings of the post-processing stack (game -> renderer). Everything the Graphics menu,
// post volumes and gameplay feedback can change lives in PostSettings; the game blends the final
// set every frame and hands it over with render_post_settings (src/game/game_post.c).
#ifndef DG_POST_SETTINGS_H
#define DG_POST_SETTINGS_H

#include "core/dgmath.h"

typedef enum PostAA { POST_AA_OFF, POST_AA_FXAA, POST_AA_SMAA, POST_AA_TAA } PostAA;
typedef enum PostUpscaler { POST_UP_OFF, POST_UP_TAAU, POST_UP_FSR1, POST_UP_FSR2, POST_UP_DLSS } PostUpscaler;
typedef enum PostTonemap { POST_TM_ACES, POST_TM_AGX, POST_TM_NEUTRAL } PostTonemap;

// Every knob of the stack. The game blends it every frame from the settings (cvars), the post
// volumes (global + local trigger volumes) and the gameplay feedback (render_post_settings).
typedef struct PostSettings {
    // anti-aliasing / upscaling
    int aa, upscaler;
    float render_scale;            // 0.5 .. 1 (upscaled to the output)
    float sharpen;                 // CAS 0..1
    // screen-space lighting
    int contact_shadows;
    int ao;                        // 0 off, 1 low, 2 high
    float ao_radius, ao_intensity;
    int ssgi;
    float ssgi_intensity;
    int ssr;                       // 0 off, 1 low, 2 high
    float ssr_intensity, ssr_max_roughness;
    // volumetrics
    int volumetrics;               // 0 off, 1 low, 2 high, 3 ultra
    float fog_density, fog_height, fog_falloff, fog_anisotropy, sun_shafts;
    float fog_ambient;             // 0 = thin haze (sun-lit) .. 1 = thick weather fog lit by the whole sky
    vec3 fog_albedo;
    // camera
    int motion_blur;               // 0 off, 1 camera, 2 camera + objects
    float mb_intensity;            // shutter fraction (0.5 = 180 deg)
    int dof;                       // 0 off, 1 ADS only, 2 full
    float dof_focus, dof_fstop, dof_focal_mm, dof_max_coc;
    int dof_autofocus, dof_bokeh;  // bokeh 0 circle, 1 hexagon
    // bloom / exposure / lens
    int bloom;                     // 0 off, 1 low, 2 high
    float bloom_intensity, bloom_dirt;
    int exposure_auto;
    float exposure_comp, exposure_manual, adapt_up, adapt_down, ev_min, ev_max, hist_low, hist_high;
    float flare, anamorphic, chromatic, vignette, distortion, grain;
    // tonemap / grade
    int tonemapper;
    float wb_temp, wb_tint;        // -1..1
    vec3 lift, gamma, gain;
    float saturation, contrast;
    int lut;                       // 0 = baked default, 1.. = loaded .cube files
    float lut_weight;
    int colorblind;                // 0 off, 1 protan, 2 deutan, 3 tritan (correction filter)
    float brightness, user_gamma;
    // output
    bool hdr_output;
    float paper_white, max_nits;
    // gameplay feedback (0..1 weights, blended in by game_post.c)
    float fb_damage, fb_lowhealth, fb_flash, fb_hitmarker, fb_sprint, fb_underwater, fb_nightvision;
    vec3 fb_tint;
    float fb_desaturate;
    // debug
    int debug_view;                // index into post_debug_names (0 = off)
    bool compare_split;            // left half: no post (tonemap only), right half: full stack
} PostSettings;

void post_settings_default(PostSettings *s);

void render_post_settings(const PostSettings *s);   // applied at the next render_end_frame
const PostSettings *post_settings(void);           // the set in use this frame
const char *post_debug_name(int i);                // r_post_debug views
int post_debug_count(void);

#endif
