// Humanoid retargeting onto the canonical Skeleton (Unity Avatar-style mapping), used by
// tools/retarget, tools/bake_mesh (alias suggestions) and the docs' "other rigs" workflow.
//
//   1. Name mapping: each bone of another humanoid rig (Mixamo, UE mannequin, Unity humanoid, Blender
//      rigify-ish names...) is matched to a canonical bone by normalized name, synonym table and end-bone
//      patterns. The result is a BoneAliasTable: drop it next to the model as <Model>.json { "aliases" }
//      and the model bakes into a compatible SkeletalMesh (src/render/skeletal_mesh.h).
//   2. Base-pose correction: per mapped bone, the model-space rotation that takes the source rig's bind
//      onto the Skeleton's retarget base pose (so a source pose maps onto the canonical rig).
//   3. Clip retargeting: per-frame application of 2 + hips height scaling. retarget_pose implements
//      one pose; retarget_clip is the (stub) batch API for tools.
#ifndef DG_RETARGET_H
#define DG_RETARGET_H

#include "skeleton.h"
#include "render/skeletal_mesh.h"

typedef struct RetargetMap {
    int src_count;
    i16 canon_of[ANIM_MAX_BONES];     // source bone -> canonical bone (-1 = unmapped)
    i16 src_of[ANIM_MAX_BONES];       // canonical bone -> source bone (-1 = none)
    quat correction[ANIM_MAX_BONES];  // per canonical bone: model-space rotation offset (src bind -> canonical base pose)
    float height_ratio;               // canonical height / source height (root translation scale)
    int mapped, renamed;              // bones mapped / of which by a different name
} RetargetMap;

// Canonical bone for a source bone name (normalized names, synonyms, end-bone patterns); -1 if none.
int retarget_match_name(const Skeleton *canon, const char *src_name);
// Maps a whole source rig (exact names first, then retarget_match_name; parents must map consistently).
// Writes the renames into `aliases` (may be NULL) and a human readable report (may be NULL).
int retarget_map_skeleton(const SkeletonAsset *canon, const Skeleton *src, RetargetMap *map, BoneAliasTable *aliases, char *report, int report_size);
// Base-pose correction for every mapped bone (fills map->correction, map->height_ratio).
void retarget_base_pose(const SkeletonAsset *canon, const Skeleton *src, RetargetMap *map);
// One source pose (local, src rig) -> canonical local pose. Unmapped canonical bones keep the base pose.
void retarget_pose(const SkeletonAsset *canon, const Skeleton *src, const RetargetMap *map, const Pose *src_pose, Pose *out);
// Batch clip retarget (tools): STUB, returns false until clip import from foreign rigs is needed.
bool retarget_clip(const SkeletonAsset *canon, const Skeleton *src, const RetargetMap *map, const Clip *src_clip, Clip *out);

#endif
