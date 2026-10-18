// Virtualized geometry, skinned meshes (docs/VGEO.md "Skinned meshes"): palette slots and the CPU
// mirror of the skinned bounds / skinning the shaders do.
//
// A palette slot holds one character's palette (mat4 per canonical-skeleton bone, column-major,
// bind space -> instance space: the game's `bone_world * inverse_bind`, minus the instance transform;
// with an identity instance at the origin it is the game's world-space palette as-is) and last
// frame's, for velocity. Every vgeo skinned instance of the character (body, parts) references the
// slot through VgInstance.extra (vgeo_scene_add_skinned_instance). A slot with no palette (never set,
// or cleared) culls its instances, so a pool of instances can be registered once and switched on and
// off per frame.
//
// Per frame: vg_palettes_begin(frame) -> vg_palettes_set(slot, ...) for every animated slot ->
// vg_palettes_record(cmd) before the first cull of the frame. The palettes are copied through a host
// staging buffer per frame in flight (reused when that frame's fence has signalled) into one device
// buffer: each slot alternates between two regions (current / previous), so the previous palette is
// simply the region written last time. A slot not set in a frame keeps its palette with previous =
// current (no motion). The record starts with a barrier against every earlier shader read (the
// previous frame may still read the region being overwritten), then copies, then a barrier to the
// shaders.
#ifndef DG_VGEO_SKIN_H
#define DG_VGEO_SKIN_H

#include "vgeo_scene.h"

typedef struct VgPalettes {
    const VgContext *ctx;
    u32 max_slots;
    VgBuffer bones;              // binding 20: mat4[max_slots * 2 * VGEO_MAX_BONES] (slot s region h at (2 s + h) * 128)
    VgBuffer slots;              // binding 21: VgPaletteSlot[max_slots]
    VgBuffer staging[VG_MAX_FRAMES];   // host: mat4[max_slots * 128] (this frame's sets) + the slot table
    float *cpu_bones;            // CPU mirror of `bones` (reference cut, tests)
    VgPaletteSlot *cpu_slots;    // CPU mirror of `slots`
    u8 *half;                    // per slot: region of the current palette
    u8 *set;                     // per slot: set this frame
    float (*bone_bounds)[VGEO_MAX_BONES][4];   // per slot: bind-space bone influence spheres (has_bounds)
    u8 *has_bounds;
    u32 frame;
    bool ok;
} VgPalettes;

bool vg_palettes_init(VgPalettes *p, const VgContext *ctx, u32 max_slots);
void vg_palettes_shutdown(VgPalettes *p);
// Binds the palettes to a scene: its cullers / rasterisers / resolve read bindings 20 / 21 from them
// (without palettes those bindings hold dummies and skinned instances must not be drawn).
void vg_palettes_attach(VgPalettes *p, VgScene *s);
// Start of a frame's uploads; frame < ctx->frames, and that frame's previous submission has completed.
void vg_palettes_begin(VgPalettes *p, u32 frame);
// This frame's palette of a slot: bone_count (<= 128) column-major mat4s.
void vg_palettes_set(VgPalettes *p, u32 slot, const float *palette, u32 bone_count);
// No palette: the slot's instances are culled (until the next set).
void vg_palettes_clear(VgPalettes *p, u32 slot);
// Optional, once per character (the slot's meshes): the union of their vgeo_bone_bounds
// (vgeo_bone_bounds_merge). With them the slot's root-deviation bound is per bone (tight: the LOD /
// instance spheres grow by about the largest bone displacement relative to the root); without
// (NULL) it is the generic k0 + k1 |v|, valid for any mesh but loose for long limbs. Every mesh
// drawn with the slot must be covered by the bounds given.
void vg_palettes_set_bone_bounds(VgPalettes *p, u32 slot, const float (*bounds)[4]);
void vgeo_bone_bounds_merge(float dst[VGEO_MAX_BONES][4], const float src[VGEO_MAX_BONES][4]);
// Uploads this frame's palettes and slot table (before the frame's first vg_cull_record).
void vg_palettes_record(VgPalettes *p, VkCommandBuffer cmd);

// ------------------------------------------------------------------ CPU mirror (shared with the reference cut, tests)

// Slot bounds of a palette (VgPaletteSlot.max_scale / k0 / k1), from spectral-norm bounds
// sqrt(|A^T A|_inf) of every bone's linear part A_b and of A_b - A_0. Generic (bone_bounds NULL):
// k0 = max |t_b - t_0|, k1 = max |A_b - A_0|. With bone influence spheres (c_b, r_b):
// k0 = max_b |(P_b - P_0) c_b| + |A_b - A_0| r_b, k1 = 0.
void vgeo_palette_bounds(const float *palette, u32 bone_count, const float (*bone_bounds)[4], float *max_scale, float *k0, float *k1);
// Skin matrix of a vertex: sum_k (w_k / 255) palette[first + j_k] (same order as the shaders).
void vgeo_skin_matrix(const float *palette, const u8 joints[4], const u8 weights[4], float out[16]);
void vgeo_mat4_point(const float m[16], const float p[3], float out[3]);
void vgeo_mat4_dir(const float m[16], const float d[3], float out[3]);

// Instance transform as the shaders decode it (vg_instance_xf): scale is the error / bounds scale
// (instance scale x slot max_scale when skinned), pscale the instance's own scale.
typedef struct VgXfCpu {
    float pos[3], rot[4], scale, pscale;
    bool skinned, active;        // active: static, or skinned with a palette
    u32 first, prev_first;       // skinned: the slot's palette regions
    float root[16], skin_s, skin_k;   // skinned: P_0, max_scale, k0 + k1 (|mesh centre| + mesh radius)
    float wpo;                   // the mesh's max world-position offset (added to every mesh-space sphere)
} VgXfCpu;
void vgeo_xf_decode(VgXfCpu *x, const VgInstance *in, const VgMesh *mesh, const VgPalettes *p);
// Mesh-space sphere -> world (skinned: (P_0 c, r max_scale + k) in instance space first: contains the
// skinned vertices of anything the bind-space sphere contains), + the mesh's max WPO.
void vgeo_xf_sphere(const VgXfCpu *x, const float c[3], float r, float out[4]);
// Instance-space sphere -> world (the instance transform only).
void vgeo_xf_instance_sphere(const VgXfCpu *x, const float c[3], float r, float out[4]);
// Animated culling sphere of a skinned cluster, instance space: the box around the union over its
// bones of AABB(palette[b] * box_b) (conservative for linear blend skinning).
void vgeo_skin_cluster_sphere(const float *palette, const VgBoneBox *bones, u32 n, float out[4]);

#endif
