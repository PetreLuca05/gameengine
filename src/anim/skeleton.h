// Skeleton asset + skeleton instance (Unreal USkeleton / Unity Avatar): the one canonical rig every
// human model, clip, controller, ragdoll, hitbox and socket set is authored against.
//
//   SkeletonAsset     bones (the Skeleton: hierarchy, names, reference pose, name -> index), sockets,
//                     virtual bones, bone groups / masks, LOD bone sets, retarget base pose, hash.
//                     Baked from the rig FBX (assets/human/Dummy.fbx) + the authored metadata in
//                     assets/skeletons/<id>.json; cached as assets/skeletons/<id>.skel (tools/bake_skeleton)
//                     or in assets/cache/. Everything else per skeleton (clips, anim sets, controller,
//                     ragdoll, hitboxes, IK, hold styles) lives in the AnimLibrary built on it.
//   SkeletonInstance  one per character, whatever its mesh part count: the drawn pose (bone world
//                     transforms) and the shared skinning palette every SkeletalMesh part gathers from,
//                     plus socket / virtual bone queries. Meshes never animate on their own.
//
// See docs/CHARACTERS.md.
#ifndef DG_SKELETON_H
#define DG_SKELETON_H

#include "anim.h"

#define SKEL_MAX_SOCKETS 32
#define SKEL_MAX_VBONES 16
#define SKEL_MAX_GROUPS 24
#define SKEL_LOD_COUNT 4
#define SKEL_FILE_VERSION 1

// Bit set over skeleton bones (ANIM_MAX_BONES = 128).
typedef struct BoneSet { u64 bits[2]; } BoneSet;
DG_INLINE bool boneset_has(const BoneSet *s, int b) { return b >= 0 && b < 128 && ((s->bits[b >> 6] >> (b & 63)) & 1u); }
DG_INLINE void boneset_add(BoneSet *s, int b) { if (b >= 0 && b < 128) s->bits[b >> 6] |= (u64)1 << (b & 63); }
DG_INLINE void boneset_or(BoneSet *a, const BoneSet *b) { a->bits[0] |= b->bits[0]; a->bits[1] |= b->bits[1]; }
DG_INLINE bool boneset_empty(const BoneSet *s) { return !s->bits[0] && !s->bits[1]; }
int boneset_count(const BoneSet *s);

// Named attach point parented to a bone (Unreal skeletal mesh socket): relative location / rotation
// in the bone's space. "space": "bone" = offset / rotation in the bone's local frame; "bind" (default)
// = model-space offset at the bind pose (character faces -Z, +Y up), converted to the bone's frame.
typedef struct SkelSocket {
    char name[24];
    char bone_name[48];
    int bone;                      // resolved skeleton index (-1 = unresolved)
    vec3 offset, rotation_deg;
    bool bind_space;
    Xform rel;                     // bone space
} SkelSocket;

// Unreal-style virtual bone: a bone parented to `source` that follows `target`, without changing
// the hierarchy (clips, poses and the wire format never see it). Used as IK targets: e.g. the left
// hand expressed in the right hand's space is the two-handed hold's glue target.
typedef struct VirtualBone {
    char name[48];
    char source_name[48], target_name[48];
    i16 source, target;            // skeleton indices (-1 = unresolved)
} VirtualBone;

// Named bone group (mask): upper_body, lower_body, arms, legs, hands, fingers, head, spine, hair...
typedef struct BoneGroup { char name[24]; BoneSet bones; } BoneGroup;

typedef struct SkeletonAsset {
    Skeleton skel;                 // keep first: AnimLibrary overlays lib->skel on it
    char id[32];                   // "human"
    char source[128];              // rig FBX (data-relative)
    u32 source_hash;               // FNV of the rig FBX bytes (0 = unknown)
    u32 meta_hash;                 // sockets + virtual bones + groups + base pose
    SkelSocket sockets[SKEL_MAX_SOCKETS];
    int socket_count;
    VirtualBone vbones[SKEL_MAX_VBONES];
    int vbone_count;
    BoneGroup groups[SKEL_MAX_GROUPS];
    int group_count;
    BoneSet lod_bones[SKEL_LOD_COUNT];        // bones animated / skinned at each LOD
    Xform base_pose[ANIM_MAX_BONES];          // retarget base pose (local): the reference other rigs are mapped onto
    mat4 inv_bind[ANIM_MAX_BONES];            // inverse model-space bind (palette = world * inv_bind)
} SkeletonAsset;

// Builds the derived data (inverse binds, LOD sets, default groups, base pose = bind) from a loaded
// Skeleton. Metadata (sockets, virtual bones, extra groups) is cleared.
void skel_asset_from_skeleton(SkeletonAsset *a, const Skeleton *s, const char *id, const char *source, u32 source_hash);
// Applies assets/skeletons/<id>.json text ({"sockets", "virtual_bones", "groups", "base_pose"}).
// Unknown bones are reported in err (non-fatal: the entry stays unresolved). Returns false on a JSON error.
bool skel_asset_apply_meta(SkeletonAsset *a, const char *json_text, char *err, int err_size);
// Loads the skeleton `id`: assets/skeletons/<id>.json names the rig FBX ("source"); bones come from
// assets/skeletons/<id>.skel when its source hash matches (or the FBX is absent), else from the FBX
// through the bake cache (fallback_source when the JSON names none). The JSON metadata is applied last (live edits need no re-bake).
bool skel_asset_load(SkeletonAsset *a, const char *id, const char *fallback_source, char *err, int err_size);
// Binary .skel (deterministic: same inputs, same bytes). Caller frees *out_data.
bool skel_asset_serialize(const SkeletonAsset *a, u8 **out_data, size_t *out_size);
bool skel_asset_deserialize(SkeletonAsset *a, const u8 *data, size_t size, char *err, int err_size);
bool skel_asset_write(const SkeletonAsset *a, const char *path);
bool skel_asset_read(SkeletonAsset *a, const char *path, char *err, int err_size);
// Stable identity of the asset (bones + metadata) for the network manifest.
u32 skel_asset_hash(const SkeletonAsset *a);

int skel_asset_find_socket(const SkeletonAsset *a, const char *name);
const SkelSocket *skel_asset_socket(const SkeletonAsset *a, const char *name);
int skel_asset_find_vbone(const SkeletonAsset *a, const char *name);
int skel_asset_find_group(const SkeletonAsset *a, const char *name);
// Group as a 0/1 BoneMask (false if the group doesn't exist).
bool skel_asset_group_mask(const SkeletonAsset *a, const char *name, BoneMask *out);

// Socket helpers (also used for per-character / per-mesh sockets).
void skel_socket_resolve(SkelSocket *k, const Skeleton *s);
// World transform of a socket from bone world transforms (bone scale scales the offset only).
Xform skel_socket_world(const SkelSocket *k, const Xform *bone_world);
// Virtual bone transform from model / world bone transforms (= the target's, parented to the source).
Xform skel_vbone_xform(const VirtualBone *v, const Xform *bones);
// ... and relative to its source bone (what an IK solve in the source's space wants).
Xform skel_vbone_local(const VirtualBone *v, const Xform *bones);

// ------------------------------------------------------------------ instance

typedef struct SkeletonInstance {
    const SkeletonAsset *asset;
    int count;
    Xform world[ANIM_MAX_BONES];   // the pose being drawn (bone world transforms, character scale included)
    mat4 skin[ANIM_MAX_BONES];     // shared palette: world * inv_bind (every mesh part gathers from it)
    u32 pose_version;              // bumped by skel_inst_set_pose
    u32 palette_builds;            // diagnostics / tests: palettes built (one per pose, not per part)
} SkeletonInstance;

void skel_inst_init(SkeletonInstance *si, const SkeletonAsset *a);
// Sets the pose (bone world transforms) and rebuilds the shared palette once.
void skel_inst_set_pose(SkeletonInstance *si, const Xform *world);
bool skel_inst_socket_world(const SkeletonInstance *si, const char *name, Xform *out);
bool skel_inst_vbone_world(const SkeletonInstance *si, const char *name, Xform *out);

#endif
