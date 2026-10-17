// Cascaded sun shadow layout (docs/SHADOWS.md "Cascades"): one formula for every Engine Fidelity
// preset, so the presets only differ in resolution, cascade count and reach:
//
//   splits     practical scheme: s_k = lambda x near (D / near)^(k / N) + (1 - lambda) x (near + (D - near) k / N),
//              the last one is exactly the shadow distance D; the shader picks the cascade by the
//              distance from the camera and blends over the last CSM_BLEND of each one, the last
//              cascade fades out over the last CSM_FADE of D
//   fit        the bounding sphere of the view sector (the camera frustum cut at distance s_k): its
//              radius only depends on the split and the fitting field of view, never on the camera
//              rotation, so the texel size of a cascade is constant
//   snap       the sphere centre is snapped to whole texels in light space: camera motion moves the
//              tile by whole texels and edges never swim
//   reach      the light-space box reaches CSM_REACH metres towards the light: tall and off-screen
//              casters (towers on the horizon at a low sun) always cast
//   bias       per cascade from its texel size alone (normal offset, constant, receiver plane), raised
//              at a low sun; the same numbers on every preset in texels
//   viewmodel  a dedicated tile around the first-person rig (arms, held item, attachments), snapped,
//              sub-millimetre texels
//
// Pure: no renderer (tests/test_shadow_csm.c). render.c compute_matrices builds the frame from it.
#ifndef DG_SHADOW_CSM_H
#define DG_SHADOW_CSM_H

#include "core/base.h"
#include "core/dgmath.h"

#define CSM_MAX 3
#define CSM_NEAR 1.0f          // split scheme near distance (m)
#define CSM_LAMBDA 0.8f        // log / uniform blend of the split scheme
#define CSM_BLEND 0.12f        // cascade k blends into k + 1 over its last 12 %
#define CSM_FADE 0.10f         // the last cascade fades out over the last 10 % of the distance
#define CSM_REACH 3000.0f      // light-space box towards the light (m): casters outside the view
#define CSM_FIT_FOV_MIN 90.0f  // fitting vertical field of view floor (deg): ADS zoom never resizes a cascade
#define CSM_EDGE_TEXELS 6.0f   // tile margin around the sphere: the shader's 4-texel border + the snap
#define CSM_VM_RADIUS 0.9f     // viewmodel tile half extent (m) around the rig
#define CSM_VM_SKIP 0.6f       // viewmodel receivers ignore world-cascade casters closer than this (the body copy)
#define CSM_VM_NORMAL_M 0.004f // viewmodel tile bias floors (m): normal offset, constant
#define CSM_VM_CONST_M 0.002f

// Texels per cascade tile by r_shadow_quality (the atlas is 2 x 2 tiles). Viewmodel tile size.
u32 csm_tile_texels(int quality);
u32 csm_vm_texels(int quality);

typedef struct CsmParams {
    int cascades;           // 1 .. 3
    u32 tile;               // texels per tile
    float distance;         // m: shadow distance (the last split)
    float fov_y_deg;        // camera vertical field of view (the fit uses at least CSM_FIT_FOV_MIN)
    float aspect;           // width / height
} CsmParams;

typedef struct CsmLayout {
    int count;
    float split[CSM_MAX];   // selection distance from the camera (m); split[count - 1] = distance
    float radius[CSM_MAX];  // bounding sphere radius = tile half extent (m)
    float ahead[CSM_MAX];   // sphere centre along the view direction (m)
    float texel_m[CSM_MAX]; // world size of one texel (m)
} CsmLayout;

// Bounding sphere of the view sector of radius s (apex at the camera, corner half angle atan(tan_corner)).
void csm_sector_sphere(float s, float tan_corner, float *ahead, float *radius);
void csm_layout(const CsmParams *p, CsmLayout *out);

// One fitted tile: light view (rotation only), snapped ortho, NDC matrix (before the atlas tile).
typedef struct CsmTile {
    mat4 view, proj, vp;
    vec3 center;            // snapped sphere centre (world)
    float radius, texel_m;
    float span;             // metres of light-space depth per NDC unit
} CsmTile;

mat4 csm_light_view(vec3 to_light);
// Square tile of `texels` around (center, radius) seen from the light; `back` m of depth behind the
// sphere centre, CSM_REACH towards the light.
void csm_fit(const mat4 *lview, vec3 center, float radius, u32 texels, float back, CsmTile *out);

// Bias in metres from the texel size (one formula; the shader applies it per cascade).
//   normal_m   receiver moved along its normal by normal_m x sin(n, l)
//   const_m    receiver depth pulled towards the light
// Both grow at a low sun (elevation under ~15 deg: the grazing receivers).
typedef struct CsmBias { float normal_m, const_m; } CsmBias;
CsmBias csm_bias(float texel_m, float sun_elev);
// The viewmodel tile: the same terms with a floor at the scale of skin / cloth / metal detail (the
// rig's meshes interpenetrate at the cuffs and grips by a few millimetres; its texels are < 1 mm).
CsmBias csm_bias_vm(float texel_m, float sun_elev);
float csm_low_sun(float sun_elev);   // 1 .. 3

// Filter half width (texels) for a penumbra of half width pen_m on a tile of texel_m, clamped to the
// tier's [min, max]. The same metres on every cascade (no seams) until a clamp applies.
float csm_kernel_texels(float pen_m, float texel_m, float min_tex, float max_tex);

// Selection the shader makes (lighting.glsl shadow_factor): the cascade for a receiver at distance d,
// the blend weight into the next one, the distance fade (1 = full shadow).
int csm_select(const CsmLayout *l, float d, int first, float *blend, float *fade);

// Viewmodel tile: around the rig's bounds centre (world), fixed radius.
void csm_fit_vm(const mat4 *lview, vec3 rig_center, u32 texels, CsmTile *out);

// Preset table check (docs/SHADOWS.md "Per-preset tables"): the layout follows the formula, the
// cascades reach the distance, texel density of cascade 0 at least min_tpm texels per metre,
// neighbouring cascades within max_ratio of each other. False + err.
bool csm_validate(const CsmParams *p, float min_tpm, float max_ratio, char *err, int err_size);

#endif
