// Per-material ambient occlusion (docs/RENDERING_POST.md "Foliage-aware AO"). Pure: shared by render.h
// (Material) and the vgeo GPU mirror (VgMaterial).
//
// A material's AO settings reach every shading path as one byte: the alpha of its GPU emissive word
// (PushLit.pbr.z / VgMaterial.emissive_rgb, whose rgb alone is the emissive colour). shaders/common.glsl
// decodes it (mat_ao_*):
//   bits 0-4  (1 - ao_strength) x 31        0 = the full screen-space AO (zero-initialised = default)
//   bit 5     MAT_AO_BYTE_THIN               alpha-tested / foliage: thin occluder in the GTAO search
//   bit 6     MAT_AO_BYTE_MIN                baked AO and screen-space AO combine as min() (else multiply)
//   bit 7     MAT_AO_BYTE_VERTEX             the vertex colour alpha is baked AO (not opacity)
// (0xFF, an RGBA8 colour's opaque alpha, counts as unset in vgeo material tables: vg_material_ao_check.)
#ifndef DG_MATERIAL_AO_H
#define DG_MATERIAL_AO_H

#include "core/base.h"

// ao_strength: 0 = automatic (1, or MATERIAL_AO_FOLIAGE for alpha-tested / foliage materials);
// MATERIAL_AO_OFF = no screen-space AO on the material; otherwise 0..1 (lerp(1, ao, ao_strength)).
#define MATERIAL_AO_FOLIAGE 0.3f
#define MATERIAL_AO_OFF (-1.0f)

#define MAT_AO_BYTE_STRENGTH 0x1Fu
#define MAT_AO_BYTE_THIN 0x20u
#define MAT_AO_BYTE_MIN 0x40u
#define MAT_AO_BYTE_VERTEX 0x80u

// How baked AO (vertex / occlusion map / surface layers / grass clump) meets the screen-space AO.
// Auto: min for foliage (a leaf card's baked self-shadowing already is its occlusion), multiply otherwise.
typedef enum MaterialAoCombine { MAT_AO_COMBINE_AUTO, MAT_AO_COMBINE_MIN, MAT_AO_COMBINE_MULTIPLY } MaterialAoCombine;

// The effective 0..1 weight of an ao_strength value (see above).
DG_INLINE float material_ao_weight(float ao_strength, bool foliage) {
    if (ao_strength == 0.0f) return foliage ? MATERIAL_AO_FOLIAGE : 1.0f;
    if (ao_strength < 0.0f) return 0.0f;
    return ao_strength > 1.0f ? 1.0f : ao_strength;
}

DG_INLINE u32 material_ao_byte(float ao_strength, int combine, bool vertex_ao, bool foliage) {
    u32 b = (u32)((1.0f - material_ao_weight(ao_strength, foliage)) * 31.0f + 0.5f);
    if (foliage) b |= MAT_AO_BYTE_THIN;
    if (combine == MAT_AO_COMBINE_MIN || (combine == MAT_AO_COMBINE_AUTO && foliage)) b |= MAT_AO_BYTE_MIN;
    if (vertex_ao) b |= MAT_AO_BYTE_VERTEX;
    return b;
}

// The weight a byte carries (the shaders' mat_ao_strength).
DG_INLINE float material_ao_byte_weight(u32 b) { return 1.0f - (float)(b & MAT_AO_BYTE_STRENGTH) / 31.0f; }

// An emissive word (RGBA8, rgb = colour) with the AO byte in its alpha.
DG_INLINE u32 material_ao_word(u32 emissive_rgb, u32 ao_byte) { return (emissive_rgb & 0x00FFFFFFu) | (ao_byte & 0xFFu) << 24; }

// "min" / "multiply" / "auto" (data files); unknown = auto.
DG_INLINE int material_ao_combine_parse(const char *s) {
    if (!s) return MAT_AO_COMBINE_AUTO;
    if (s[0] == 'm' && s[1] == 'i') return MAT_AO_COMBINE_MIN;
    if (s[0] == 'm' && s[1] == 'u') return MAT_AO_COMBINE_MULTIPLY;
    return MAT_AO_COMBINE_AUTO;
}

#endif
