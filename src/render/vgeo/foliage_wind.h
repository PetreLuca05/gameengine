// Vegetation wind animation (docs/FOLIAGE_WIND.md): the GPU Gems 3 chapter 16 model (Sousa, "Vegetation
// Procedural Animation and Shading in Crysis") driven by per-vertex wind weights in the vertex colour,
// in one canonical layout (the Tree It / Crysis one; imports remap other sources to it):
//
//   R  leaf edge flutter weight (0 at the stem, 1 at the card tip; 0 on bark)
//   G  per-branch phase (constant per branch, leaves inherit their branch's; 0 on the trunk)
//   B  branch bend weight (0 on the trunk, growing along each branch)
//   A  unused
//
// Three layers, in this order: main bending (the whole plant along the wind, (h / H)^2, length
// preserved about the pivot), detail bending (branches, B x four-frequency smoothed triangle waves,
// G phase), edge flutter (R x the same waves along the vertex normal). Wind comes from the wind
// field tile (src/world/wind.h) sampled at the instance pivot: instant wind (detail, flutter) and the
// settled bend spring (main bending; per-asset settle_time mixes them).
//
// CPU mirror of shaders/vgeo/wpo.glsl wpo_veg: the tests compare them, and vgeo_wpo_max bounds them.
// Pure (no renderer).
#ifndef DG_FOLIAGE_WIND_H
#define DG_FOLIAGE_WIND_H

#include "core/base.h"

// Wind layers (VgViewGpu.wind_misc.z, from r_foliage_wind / the fidelity tier and the debug solo).
enum VgWindLayer { VGW_MAIN = 1u << 0, VGW_DETAIL = 1u << 1, VGW_FLUTTER = 1u << 2, VGW_NORMALS = 1u << 3, VGW_ALL = 15u };

// Per-asset parameters (JSON "wind": {...}). The three *_stiffness values are each layer's gain
// (linear in the displacement: the names follow the Crysis "stiffness" channels; higher = moves more).
typedef struct VgWindParams {
    float trunk_stiffness;       // main bend: tan(lean angle) at full response
    float branch_stiffness;      // detail bend: m at branch weight 1, full response (scaled by the instance)
    float leaf_stiffness;        // flutter: m at flutter weight 1, full response (scaled by the instance)
    float full_wind;             // m/s: the wind that reaches full response
    float main_freq, detail_freq, flutter_freq;   // Hz: trunk sway, branches, leaf edges
    float max_bend_deg;          // main bend cap (deg from vertical at the tip)
    float settle_time;           // s (0..WIND_SETTLE_PERIOD): 0 follows the instant wind, max = the settled spring
    float response;              // response curve exponent: (s / full_wind)^response (< 1 saturates early: grass)
    float height;                // m (mesh units, scale 1): pivot to top, the main bend's H
    float pad;
} VgWindParams;

// Species-size defaults: grass (blades / small plants), bush, tree (big trees need strong wind).
enum VgWindPreset { VGWP_GRASS, VGWP_BUSH, VGWP_TREE };
void vgeo_wind_defaults(VgWindParams *p, int preset, float height);

// Material packing: wpo_params[4] = (trunk, branch, leaf, full_wind); wind_params[8] = (height,
// main_freq, detail_freq, flutter_freq, tan(max_bend), settle mix 0..1, response, 0).
void vgeo_wind_pack(const VgWindParams *p, float wpo_params[4], float wind_params[8]);
void vgeo_wind_unpack(const float wpo_params[4], const float wind_params[8], VgWindParams *p);

// One vertex's input (world space). wind = the field at the pivot: instant x, z, settled bend x, z
// (m/s, wind_tile_sample); disp = the displacement buffer's push at p (NULL: none); layers = VgWindLayer.
typedef struct VgWindVertex {
    float p[3], n[3];            // undisplaced world position, world normal (unit)
    float ip[3], pscale;         // instance pivot, uniform scale
    float col[4];                // canonical wind weights (0..1)
    float phase;                 // instance phase 0..1 (VG_INST_PHASE / 256)
} VgWindVertex;

// Offset of the vertex (world, m) and, when n_out != NULL, the perturbed normal (VGW_NORMALS).
void vgeo_wind_eval(const float wpo_params[4], const float wind_params[8], const VgWindVertex *v, const float wind[4], float t, u32 layers,
                    const float disp[2], float out[3], float n_out[3]);
// Declared bound of vgeo_wind_eval per layer (m, at instance scale 1, any wind, the displacement
// buffer excluded): main, detail, flutter. Their sum is the mesh's max_wpo (vgeo_wpo_max).
void vgeo_wind_layer_max(const float wpo_params[4], const float wind_params[8], float out[3]);

// The chapter's smoothed triangle wave (signed, -1..1): SmoothCurve(TriangleWave(x)) * 2 - 1.
float vgeo_smooth_triangle(float x);

#endif
