// Virtualized geometry impostors (docs/VGEO.md "Foliage support"): an offline, deterministic CPU bake of
// a mesh into a hemi-octahedral atlas, stored in the .vgeo file's impostor section (VGF_HAS_IMPOSTOR).
// Pure C, no GPU (build_vgeo --impostor, the procedural foliage, tests).
//
//   grid x grid frames; frame (i, j) is an orthographic view from direction
//   vgeo_hemi_oct_decode((i, j) / (grid - 1)) (mesh space, towards the viewer, y >= 0), axes
//   vgeo_impostor_basis, covering the mesh sphere (centre +- radius). Atlas texel row 0 is the top
//   of a frame. The leaf clusters (full detail) are rasterised with a depth test at 2x and box
//   filtered (fractional coverage), masked materials alpha-tested, empty texels dilated.
//   albedo: RGBA8 sRGB, rgb = material colour x texture (linear before encoding), a = coverage
//   normal: RGBA8, mesh-space normal x 0.5 + 0.5 (facing the viewer), a = coverage
// The runtime samples it in shaders/vgeo/vg_impostor.glsl with the same directions and axes.
#ifndef DG_VGEO_IMPOSTOR_H
#define DG_VGEO_IMPOSTOR_H

#include "vgeo_build.h"

// Section layout: header, then albedo[size * size * 4], then normal[size * size * 4].
typedef struct VgImpostorHeader { u32 size, grid, flags, pad; float center[3], radius; } VgImpostorHeader;

typedef struct VgImpostorMaterial {
    float color[4];              // linear
    const u8 *rgba;              // optional texture (RGBA8, sRGB colour, linear alpha), sampled at uv * tex_scale (wrap)
    u32 w, h;
    float tex_scale;             // 0 = 1
    bool masked;                 // alpha test against cutoff
    float cutoff;
} VgImpostorMaterial;

typedef struct VgImpostorOptions {
    u32 size, grid;              // atlas side (px, 0 = 1024), frames per side (0 = 8)
    const VgImpostorMaterial *materials;   // per mesh material (NULL / short: white, opaque)
    u32 material_count;
} VgImpostorOptions;

// Bakes the atlas into b (b->impostor, VGF_HAS_IMPOSTOR; replaces an earlier one).
bool vgeo_impostor_bake(VgBuilt *b, const VgImpostorOptions *o);
// The section of a built / loaded mesh (NULL when it has none); albedo / normal point into it.
const VgImpostorHeader *vgeo_impostor(const VgBuilt *b, const u8 **albedo, const u8 **normal);

// The frame directions and axes (mirrors of vg_impostor.glsl).
void vgeo_hemi_oct_decode(float u, float v, float out[3]);
void vgeo_hemi_oct_encode(const float d[3], float *u, float *v);
void vgeo_impostor_basis(const float f[3], float r[3], float u[3]);

#endif
