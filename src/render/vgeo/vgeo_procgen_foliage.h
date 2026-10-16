// Procedural foliage test content (docs/VGEO.md "Foliage support"): a grass tuft (~60 triangles: 5
// crossed cards) and a bush (~600 triangles: a tapered trunk and 46 leaf cards), with wind weights in
// the vertex colour (R bend / trunk, G branch, B flutter) and noise alpha masks (grass blades, leaves).
// Deterministic for a seed; generated at run time, never committed. Pure C (no renderer).
#ifndef DG_VGEO_PROCGEN_FOLIAGE_H
#define DG_VGEO_PROCGEN_FOLIAGE_H

#include "vgeo_build.h"

// The test kinds (VG_FOLIAGE_KINDS: tuft, bush) and the open-world map kinds after them
// (docs/ZOMBIE_MAP.md "Vegetation"): a conifer (~14 m: tapered trunk + drooping branch cards in
// whorls), a broadleaf tree (~11 m: trunk, limbs, a leaf-card crown), a bare dead tree and a crop clump
// (wheat, ~1.3 m crossed cards).
enum VgFoliageKind { VG_FOLIAGE_TUFT, VG_FOLIAGE_BUSH, VG_FOLIAGE_KINDS, VG_FOLIAGE_PINE = VG_FOLIAGE_KINDS, VG_FOLIAGE_BROADLEAF, VG_FOLIAGE_DEAD, VG_FOLIAGE_CROP,
                     VG_FOLIAGE_ALL_KINDS };

typedef struct VgFoliageMesh {
    float *pos, *normal, *uv;    // xyz, xyz, uv per vertex
    u32 *color;                  // RGBA8 wind weights
    u32 *indices;
    u16 *tri_material;           // tuft: 0 grass; bush: 0 trunk, 1 leaves
    u32 vertex_count, index_count, material_count;
    u32 vcap, icap;
} VgFoliageMesh;

void vgeo_gen_foliage(VgFoliageMesh *m, int kind, u32 seed);
void vgeo_foliage_mesh_free(VgFoliageMesh *m);
// The builder input of a generated mesh (pointers into m).
void vgeo_foliage_source(const VgFoliageMesh *m, VgSource *out);
// The kind's materials (flags, colours, WPO snippet + parameters, fades, shadow distance, cutoff);
// the caller fills the texture indices (mask_tex / albedo_tex: the masks below). Returns the count.
u32 vgeo_foliage_materials(int kind, VgMaterial out[2]);
// Alpha masks (RGBA8, sRGB colour, linear alpha): kind's material `material` (a texture per masked
// material: grass blades, leaves; the bush trunk has none: returns false).
bool vgeo_gen_foliage_mask(u8 *rgba, u32 size, int kind, u32 material, u32 seed);
// Impostor bake materials of a kind (colours + masks): masks[material] (size x size, may be NULL).
void vgeo_foliage_impostor_materials(int kind, const u8 *const masks[2], u32 size, struct VgImpostorMaterial out[2]);
const char *vgeo_foliage_name(int kind);   // "test_tuft", "test_bush", "tree_pine", "tree_broadleaf", "tree_dead", "crop_wheat"
// Trunk collision capsule of a kind (radius, height in mesh units); false for kinds without one.
bool vgeo_foliage_capsule(int kind, float *radius, float *height);

#endif
