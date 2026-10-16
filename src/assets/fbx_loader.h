// FBX import via ufbx: skeleton, skinned meshes (with LODs) and baked animation clips.
//
// Space conversion happens here, once: file units -> meters, and a 180 degree yaw so the
// character faces -Z (the game's forward). The runtime only ever sees plain local
// pos / quat / scale per bone: FBX pre/post rotations, pivots and the Blender "Armature"
// object are resolved by evaluating world transforms and re-deriving locals.
#ifndef DG_FBX_LOADER_H
#define DG_FBX_LOADER_H

#include "anim/anim.h"
#include "anim/anim_mesh.h"

#define FBX_MAX_TAKES 16

typedef struct FbxBakeOptions {
    float sample_hz;        // 30 or 60
    bool force_loop;        // override loop detection
    int loop_override;      // -1 auto, 0 no, 1 yes
} FbxBakeOptions;

// Skeleton = every bone node of the file (topologically sorted). Returns false + err on failure.
bool fbx_load_skeleton(const char *path, Skeleton *out, char *err, int err_size);
// Skinned meshes of the file, bound to `canon` by bone name. Mesh nodes named *_LOD<n> become
// LOD n (several nodes with the same LOD are merged). Bones missing from canon are folded onto
// their nearest canonical ancestor.
bool fbx_load_skinned_mesh(const char *path, const Skeleton *canon, SkinMeshData *out, char *err, int err_size);
// Bakes every take of the file against `canon`. Clip name = file stem, or "stem:take" when the
// file has several takes. Returns the number of clips written to out (caller frees with clip_free).
int fbx_bake_clips(const char *path, const Skeleton *canon, const FbxBakeOptions *opt, Clip *out, int max, char *err, int err_size);
// Stem of a path ("assets/human/left turn 90.fbx" -> "left turn 90").
void fbx_file_stem(const char *path, char *out, size_t n);
// Default loop guess from the clip name (turns, jumps, deaths, hits... don't loop).
bool fbx_guess_loop(const char *name);
// FNV hash of a file's bytes (0 if unreadable).
u32 fbx_file_hash(const char *path);

#endif
