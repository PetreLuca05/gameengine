// Imported tree / bush foliage (docs/VGEO.md "Tree models"): the Tree It exports in assets/treefbx
// (FBX 6.1 / OBJ, CC BY 4.0) turned into vgeo foliage meshes. Pure C, no renderer (the game,
// tools/build_vgeo --trees).
//
//   source mesh (ufbx) -> per texture material (bark opaque; leaf / branch cards alpha-masked and
//   two-sided when the texture has a cut-out alpha) -> metres, ground at y = 0, the trunk base at
//   x = z = 0, scaled to the species height -> wind weights in the vertex colour (R trunk sway by
//   height, G branch bounce by distance from the trunk, B leaf flutter on the cards) -> vgeo build
//   (its DAG is the LOD chain: the files' own LODs are not used) -> impostor bake with the textures
//   -> trunk capsule from the bark near the ground.
// Results are cached (VgTreeCacheHeader + the .vgeo bytes): the build writes them to
// assets/vgeo/trees/<name>.dgtree, a run that misses (or finds a stale key) builds and writes
// assets/cache/<name>.tree.<key>.dgc.
#ifndef DG_VGEO_TREE_H
#define DG_VGEO_TREE_H

#include "vgeo_build.h"
#include "vgeo_procgen_foliage.h"

#define VG_TREE_VERSION 2        // bump when the import's output changes (cache key)
#define VG_TREE_MAX_MATERIALS 4
#define VG_TREE_TEX_MAX 512      // textures are box-filtered down to this side (GPU memory, impostor bake)

// Roles the open-world scatter picks from (render.h RENDER_TREE_*).
enum VgTreeRole { VG_TREE_CONIFER, VG_TREE_BROADLEAF, VG_TREE_DEAD, VG_TREE_BUSH, VG_TREE_FLOWER, VG_TREE_ROLES };

typedef struct VgTreeSpecies {
    const char *name;            // foliage type name ("tree_oak")
    const char *file;            // source mesh in the tree directory
    u8 role;                     // VgTreeRole
    float weight;                // relative pick weight within the role
    float height;                // m: ground to top at instance scale 1
    float sway, branch, flutter; // WPO_TREE amplitudes (m at full wind, weight 1)
    float fade_start, fade_end, shadow_max;   // m
    u32 atlas;                   // impostor atlas px (8 x 8 frames; 0 = none)
    float impostor_scale;        // switch size x r_vgeo_impostor_px (vgeo_impostor_scale)
    bool capsule;                // trunk collider
} VgTreeSpecies;

u32 vgeo_tree_species_count(void);
const VgTreeSpecies *vgeo_tree_species(u32 i);

// An RGBA8 texture (sRGB colour, linear alpha), box-filtered down to <= max_side.
typedef struct VgTreeTexture { u8 *rgba; u32 w, h; } VgTreeTexture;
bool vgeo_tree_texture_load(const char *path, u32 max_side, VgTreeTexture *out);
// Alpha-tested mips keep their coverage: every level of a packed RGBA8 chain (vg_tex_build_mips layout)
// scales its alpha so the share of texels >= cutoff matches the base level's (cards would thin out
// and vanish with distance otherwise).
void vgeo_tree_mip_coverage(u8 *mips, u32 w, u32 h, u32 levels, float cutoff);

typedef struct VgTree {
    const VgTreeSpecies *sp;
    VgBuilt b;                   // clusters, DAG, discrete LODs, impostor
    u32 material_count;
    char tex[VG_TREE_MAX_MATERIALS][48];   // albedo texture file per material ("oak leaf.png")
    bool masked[VG_TREE_MAX_MATERIALS];
    VgTreeTexture texture[VG_TREE_MAX_MATERIALS];   // loaded by vgeo_tree_load_or_build
    float cap_r, cap_h;          // trunk capsule (m at scale 1)
    float src_height;            // source mesh height (file units), the scale applied
    float scale;
    bool from_cache;
} VgTree;

// Source mesh -> foliage mesh in metres with wind weights (meta: materials, capsule, scale).
bool vgeo_tree_import(const VgTreeSpecies *sp, const char *dir, VgFoliageMesh *out, VgTree *meta, char *err, int err_size);
// The cache key of a species (source file bytes, species shape parameters, versions); 0 = unreadable.
u32 vgeo_tree_key(const VgTreeSpecies *sp, const char *dir);
// Loads the species from built_dir (<name>.dgtree) or cache_dir (<name>.tree.<key>.dgc) when the key
// matches, else imports, builds, bakes and writes cache_dir (NULL: no cache). Both dirs end with a
// separator. Loads the material textures too (VG_TREE_TEX_MAX).
bool vgeo_tree_load_or_build(const VgTreeSpecies *sp, const char *dir, const char *built_dir, const char *cache_dir, VgTree *out, char *err, int err_size);
// Writes the cache file of a built tree.
bool vgeo_tree_write(const VgTree *t, u32 key, const char *path);
// The species' vgeo materials (flags, WPO, fades; albedo_tex / mask_tex left 0 for the caller).
void vgeo_tree_materials(const VgTree *t, VgMaterial out[VG_TREE_MAX_MATERIALS]);
void vgeo_tree_free(VgTree *t);

#endif
