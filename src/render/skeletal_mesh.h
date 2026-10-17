// SkeletalMesh asset + SkinnedMeshInstance (Unreal USkeletalMesh / Unity SkinnedMeshRenderer): skinned
// geometry bound BY BONE NAME to the one Skeleton asset (src/anim/skeleton.h). Pure CPU data: no render
// API here (the GPU meshes are built by src/gameplay/characters.c), so tools and tests link it too.
//
//   SkeletalMesh        vertices + skin weights, LODs, materials, and its OWN bone list (Unreal
//                       RequiredBones / Unity bones[]): each entry is a Skeleton bone or a mesh-only leaf
//                       bone (hair, cloth, straps) riding on its parent. Vertex joints index this list, so
//                       a head part skins with ~5 bones, not 65. Baked from an FBX (assets/cache/, or a
//                       standalone .skm written by tools/bake_mesh), with an optional bone-name alias
//                       table ("mixamorig:Hips" -> "Hips") and mesh-local sockets.
//   Palette gather      one SkeletonInstance per character holds the pose + the shared palette; every
//                       part gathers its own (smaller) palette from it through its bone map. Parts never
//                       evaluate animation.
//
// Compatibility (Unreal's rule): every skinned bone must exist in the Skeleton by name (after aliasing)
// with the same parent chain; skeleton bones the mesh lacks are simply not skinned; extra bones become
// mesh-only leaves; renamed bones / another hierarchy are rejected (rig_validate, src/assets/character.h).
// Proportions: vertices are retargeted onto the Skeleton reference pose at bake (per-bone bind deltas are
// kept for the report). See docs/CHARACTERS.md.
#ifndef DG_SKELETAL_MESH_H
#define DG_SKELETAL_MESH_H

struct cJSON;

#include "anim/anim_mesh.h"
#include "anim/skeleton.h"

#define SKM_MAX_BONES ANIM_MAX_BONES
#define SKM_MAX_ALIASES 32
#define SKM_MAX_SOCKETS 8
#define SKM_FILE_VERSION 2   // 2: vertices may stay in the file's bind space (SkmBone.bind_fix)

typedef struct BoneAlias { char from[48], to[48]; } BoneAlias;
typedef struct BoneAliasTable { BoneAlias a[SKM_MAX_ALIASES]; int count; } BoneAliasTable;

typedef struct SkmBone {
    char name[48];          // skeleton name (aliases applied) or the mesh-only bone's own name
    i16 skel;               // skeleton bone index, -1 = mesh-only leaf bone
    i16 parent;             // mesh-only: parent entry in this list (always earlier); skeleton bones: -1
    Xform local;            // mesh-only: bind transform relative to the parent (reference pose space)
    Xform bind_model;       // bind pose on the Skeleton reference pose (model space)
    Xform bind_delta;       // file bind vs the Skeleton reference pose: file bind = bind_delta * bind_model
    // Per-bone retarget (has_fix): the mesh's vertices are in the file's bind space and this bone's
    // palette entry is the skeleton's `world * inverse(bind_model)` times bind_fix = inverse(bind_delta),
    // i.e. `world * inverse(file bind)`: every influence moves the vertex rigidly from its own bind
    // (Unreal's per-mesh inverse bind matrices). Without it the vertices were retargeted at bake.
    mat4 bind_fix;
    bool has_fix;
} SkmBone;

typedef struct SkeletalMesh {
    SkinMeshData data;      // LODs / materials; SkinVertex.joints index bones[]
    char skeleton_id[32];
    u32 skeleton_hash;      // Skeleton.hash this mesh was bound to (compatibility check at load)
    u32 source_hash;        // FNV of the source FBX (0 = unknown / derived)
    int bone_count;
    SkmBone bones[SKM_MAX_BONES];
    BoneAliasTable aliases;
    SkelSocket sockets[SKM_MAX_SOCKETS];   // mesh-local: SkelSocket.bone indexes bones[]
    int socket_count;
    BoneSet skinned;        // skeleton bones with weights in LOD0 (diagnostics / inspector)
    // Bake report summary (the full text comes from tools/bake_mesh).
    int missing, extra, aliased;
    float max_bind_delta_m, max_bind_delta_deg;
    // Largest distance between a vertex's blended bake-time retarget and one of its influences' own
    // retarget (the error bound of baking the retarget into the vertices; above 0.1 mm the mesh keeps
    // its file bind and the palette retargets per bone instead, SkmBone.bind_fix).
    float retarget_blend_err_m;
} SkeletalMesh;

void skm_free(SkeletalMesh *m);
// Deep copy (LOD arrays duplicated).
void skm_copy(SkeletalMesh *dst, const SkeletalMesh *src);

// ------------------------------------------------------------------ aliases

// Parses { "from": "to", ... } (cJSON object). Returns the number of entries added.
int skm_alias_parse(BoneAliasTable *t, const struct cJSON *obj);
// Skeleton name for a file bone name (exact or namespace-less match), or `name` itself.
const char *skm_alias_resolve(const BoneAliasTable *t, const char *name);
// Renames a file skeleton's bones through the table (for rig_validate) and re-finalizes it.
void skm_alias_apply_skeleton(const BoneAliasTable *t, Skeleton *s);
u32 skm_alias_hash(const BoneAliasTable *t);

// ------------------------------------------------------------------ bones

int skm_find_bone(const SkeletalMesh *m, const char *name);
// Index of a skeleton bone in the mesh's list (-1 if the mesh doesn't carry it).
int skm_bone_of_skel(const SkeletalMesh *m, int skel_bone);
// Appends a skeleton bone to the list (returns its index; existing entries are reused).
int skm_add_skel_bone(SkeletalMesh *m, const Skeleton *s, int skel_bone);
// Nearest skeleton bone of a mesh bone (mesh-only bones resolve through their parents).
int skm_skeleton_bone(const SkeletalMesh *m, int mesh_bone);
// Mesh joint -> skeleton bone map (out[SKM_MAX_BONES]) for code that reasons about skeleton bones.
void skm_joint_map(const SkeletalMesh *m, i16 *out);
// Skeleton bone a vertex mostly follows.
int skm_vertex_bone(const SkeletalMesh *m, const SkinVertex *v);
// Owner bone of a triangle (region cuts): the skeleton bone of its most rigidly bound vertex.
int skm_triangle_bone(const SkeletalMesh *m, const SkinLod *L, u32 first_index);
int skm_mesh_only_count(const SkeletalMesh *m);

// After a bake / load: LOD2+ joints folded onto the Skeleton's reduced LOD bone set, the skinned set,
// mesh-local sockets resolved.
void skm_finalize(SkeletalMesh *m, const SkeletonAsset *a);
// Drops bones no vertex uses (mesh-only parents of used bones stay) and remaps the joints.
void skm_compact_bones(SkeletalMesh *m);

// ------------------------------------------------------------------ palette

// The part's palette from the shared skeleton palette (cheap gather: one copy per skeleton bone, one
// transform per mesh-only bone). leaf_local: optional per mesh bone local override for mesh-only bones
// (secondary motion layer; NULL = rigid on the parent). Returns the bone count written to out.
int skm_gather_palette(const SkeletalMesh *m, const SkeletonInstance *si, const Xform *leaf_local, mat4 *out);
// World transform of a mesh bone (skeleton bones: the instance's; mesh-only: through their parents).
Xform skm_bone_world(const SkeletalMesh *m, const SkeletonInstance *si, int mesh_bone);
// Mesh-local socket ("backpack" on one jacket) in world space.
bool skm_socket_world(const SkeletalMesh *m, const SkeletonInstance *si, const char *name, Xform *out);

// ------------------------------------------------------------------ derived meshes (modular parts)

// New mesh from the triangles whose owner bone (skm_triangle_bone) is in `region` (all LODs), bone list
// compacted: disjoint regions tile the source exactly. Splits a full body into head / torso / legs / hands parts when the art isn't split.
bool skm_extract_region(const SkeletalMesh *src, const BoneSet *region, SkeletalMesh *out);
// Placeholder variety: push vertices out along their normal by `inflate` meters and scale them by
// `scale` about their dominant bone's bind position (shapes stay on the skeleton).
void skm_deform(SkeletalMesh *m, float inflate, vec3 scale);
// Index buffer of `lod` without the triangles whose three vertices follow bones in `hidden`
// (hide-body-under-clothing). Returns the index count written to out (capacity: lod icount).
u32 skm_indices_without(const SkeletalMesh *m, int lod, const BoneSet *hidden, u32 *out);

// ------------------------------------------------------------------ files

// .skm: deterministic binary (header + mesh + LOD arrays). Caller frees *out_data.
bool skm_serialize(const SkeletalMesh *m, u8 **out_data, size_t *out_size);
bool skm_deserialize(SkeletalMesh *m, const u8 *data, size_t size, char *err, int err_size);
bool skm_write(const SkeletalMesh *m, const char *path);
bool skm_read(SkeletalMesh *m, const char *path, char *err, int err_size);

// ------------------------------------------------------------------ instances + batching

// One per part on a character: the mesh + the character's SkeletonInstance (never its own pose).
typedef struct SkinnedMeshInstance {
    const SkeletalMesh *mesh;
    const SkeletonInstance *skel;
    u32 gpu;                // renderer-side id of the built GPU part (characters.c), 0 = none
    const Xform *leaf_local;
} SkinnedMeshInstance;

// Instanced draw batching (render.c): draws sorted by (texture, mesh) and merged into instanced
// batches. keys[i] = texture << 32 | mesh. order[] receives the draw order; batches[] the runs
// (first index into order, count). Returns the batch count.
typedef struct SkinBatchRun { u64 key; u32 first, count; } SkinBatchRun;
u32 skin_batch_build(const u64 *keys, u32 n, u32 *order, SkinBatchRun *batches);

#endif
