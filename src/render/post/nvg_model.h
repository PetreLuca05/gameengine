// Night vision goggles: the image-intensifier model (docs/NIGHT_OPTICS.md "Image intensifier").
// Not a green filter: a Gen-3 tube counts photons (visible red + near-IR) and amplifies them
// ~10^4 times, with the artefacts that come with that:
//
//   ABC          automatic brightness control: the gain follows the scene (log domain) as a damped
//                spring: it overshoots and settles, slower than the eye
//   saturation   the screen saturates (Y = Ysat (1 - exp(-G phi / Ysat))): bright sources clip
//   whiteout     an overload (muzzle flash, headlights, a flashlight into the lens) washes the image
//                out (fast attack) and it recovers over whiteout_recover s; autogating flickers
//                while the gain drops fast; extreme brightness for protect_s s shuts the tube off
//   halo         a fixed-size soft disc around bright points (electrons bouncing at the MCP
//                input): its size is an angle, the same on screen at any resolution
//   scintillation  shot noise: N photoelectrons per tube pixel per frame (Poisson, MCP excess
//                noise factor F): relative noise sqrt(F / N) grows as the light falls; EBI keeps
//                a dark tube sparkling
//   mask         one 40 deg circle per tube; a dual-tube binocular shows two overlapping circles
//                (figure 8); black outside. The world FOV does not change
//
// Variants (assets/nvg/nvg.json): phosphor colour (P43 green, P45 white), tubes, field, halo,
// whiteout recovery, persistence, battery runtime. The per-pixel stages run in
// shaders/post/nvg_*.comp with these constants; the state below runs on the CPU (the mean flux is
// read back from the GPU two frames late, which is in the spirit of a slow ABC). Pure
// (tests/test_night_optics.c).
#ifndef DG_NVG_MODEL_H
#define DG_NVG_MODEL_H

#include "core/base.h"

#define NVG_VARIANTS 8

typedef struct NvgVariant {
    char id[24];                     // "p43", "p45", "dual"
    char label[32];
    float phosphor[3];               // linear RGB of the phosphor at full output
    int tubes;                       // 1 (monocular / single circle) or 2 (binocular figure 8)
    float circle_deg;                // field of one tube (diameter)
    float tube_sep_deg;              // dual: centre offset of each circle from the view centre
    float halo_deg;                  // halo radius
    float whiteout_recover;          // s: back under 5 % whiteout after an overload ends
    float persistence;               // s: phosphor decay time constant
    float gain_min, gain_max;        // ABC range (engine luminance units -> screen)
    float target;                    // ABC mean output target (0..1 screen)
    float settle_down, settle_up;    // s: gain falling (scene brighter) / rising (scene darker)
    float damping;                   // ABC spring damping ratio (< 1: overshoot)
    float tube_px;                   // tube resolution across one circle (Modern)
    float photons;                   // photoelectrons per tube pixel per frame at flux 1 (60 Hz)
    float noise_factor;              // MCP excess noise factor F
    float ebi;                       // equivalent background illumination (flux)
    float protect_s;                 // s of extreme brightness before the tube protection shuts it off
    float runtime_h;                 // battery: hours of real time on one AA
    float illum_drain;               // battery drain multiplier with the illuminator on
    float illum_range, illum_angle;  // IR illuminator: m, deg (full cone)
    float illum_intensity;
} NvgVariant;

typedef struct NvgConfig {
    NvgVariant v[NVG_VARIANTS];
    int count;
    float ir_moon, ir_sky;           // near-IR share of moonlight / of starlight + airglow (x the visible)
    float k_vis, k_nir;              // photocathode weights of the visible image / the NIR image
    float w_gaas[3];                 // photocathode spectral weights on linear RGB
    float y_sat;                     // screen saturation
    float overload;                  // G * photocurrent / y_sat above which whiteout builds (log2 scale starts here)
    float flip_time;                 // s: flip-down / flip-up hand case
} NvgConfig;

void nvg_config_default(NvgConfig *c);
bool nvg_config_load(NvgConfig *c, const char *json_text);
const NvgVariant *nvg_variant(const NvgConfig *c, const char *id);   // the first variant if not found

// ABC + whiteout + protection state.
typedef struct NvgState {
    float log_gain, vel;             // log2 gain and its rate
    float whiteout;                  // 0..1
    float overbright;                // s accumulated at the protection threshold
    float flicker;                   // 0..1 dip this frame (autogating)
    bool valid;
    bool shutoff;                    // the protection tripped this frame
    u32 frame;
} NvgState;

typedef struct NvgInput {
    float phi_mean;                  // mean photocathode flux in the circle (log average)
    float phi_current;               // linear mean flux = the tube's photocurrent (a flashlight in the face, headlights,
                                     // the own muzzle flash: much of the field very bright): drives the whiteout
    float dt;
} NvgInput;

void nvg_state_reset(NvgState *s, const NvgVariant *v, float phi_mean);
void nvg_step(NvgState *s, const NvgConfig *c, const NvgVariant *v, const NvgInput *in);
float nvg_gain(const NvgState *s);
// Gain the ABC wants for a mean flux (clamped to the variant's range).
float nvg_target_log_gain(const NvgVariant *v, float phi_mean);
// Screen output (0..1, before the phosphor colour) of a flux at a gain, with the whiteout.
float nvg_screen(const NvgConfig *c, const NvgState *s, float gain, float phi);

// Scintillation: expected photoelectrons and the relative noise (std / mean) at a flux.
float nvg_photoelectrons(const NvgVariant *v, float phi, float dt);
float nvg_noise_rel(const NvgVariant *v, float phi, float dt);
// One noisy sample of the output at a flux (mean 1 x signal): Poisson x MCP gain (exponential).
float nvg_noise_sample(const NvgVariant *v, float phi, float dt, Rng *rng);

// Halo radius in pixels for a horizontal FOV and screen width (constant angle).
float nvg_halo_px(const NvgVariant *v, float hfov_deg, float width_px);
// Tube mask at a pixel (0 outside .. 1 inside, 0.4 deg soft edge).
float nvg_mask(const NvgVariant *v, float px, float py, float w, float h, float hfov_deg);
// Angle (deg) of a pixel from the view centre along x / y (same focal length for both axes).
void nvg_pixel_angles(float px, float py, float w, float h, float hfov_deg, float *ax, float *ay);

// Battery (the item's charge 0..1).
float nvg_battery_drain(const NvgVariant *v, float charge, bool illuminator, float dt);

#endif
