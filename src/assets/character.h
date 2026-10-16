// Character definitions (assets/characters/*.json) and rig validation against the shared,
// canonical skeleton. A character = the Skeleton + one or more SkeletalMesh parts in slots + its anim
// set / damage / ragdoll tuning. Adding a model = one FBX + one JSON line, no code:
//
//   { "id": "zombie_walker", "character_id": 3, "kind": "zombie",
//     "mesh": "assets/human/Dummy.fbx", "materials": { "Alpha_Body_MAT": [0.4, 0.5, 0.35] },
//     "anim_set": "zombie", "part_mult": { "head": 4.0, "limbs": 0.5 },
//     "ragdoll_scale": 1.0, "scale": 1.0, "hp": 120, "move_speed": 1.2 }
//
// Modular (docs/CHARACTERS.md): "parts": { "<slot>": <mesh> | [<mesh>, ...] } with slots body, head, torso,
// legs, hands, hair, gear_*; a <mesh> is a path (.fbx / .skm) or
//   { "mesh": path, "id": "head_b", "weight": 2, "region": "head", "inflate": 0.01, "scale": [1, 1, 1],
//     "materials": { ... }, "hide": ["hair"] }
// "region" cuts the part out of a full-body mesh (skeleton bone group); "hide" removes those bone groups
// from the character's OTHER parts (body under clothing). "mesh" alone = a single "body" part.
#ifndef DG_CHARACTER_H
#define DG_CHARACTER_H

#include "anim/anim.h"
#include "anim/skeleton.h"
#include "anim/body_parts.h"
#include "anim/anim_mesh.h"
#include "render/skeletal_mesh.h"

#define CHAR_LOD_FILES 4
#define CHAR_MAX_SOCKETS SKEL_MAX_SOCKETS

// Sockets live on the Skeleton asset (assets/skeletons/human.json); a character may override or
// add some ("sockets" / "eye_offset" in its JSON). character_resolve_sockets merges both.
typedef SkelSocket CharSocket;

typedef enum CharKind { CK_PLAYER, CK_ZOMBIE, CK_DUMMY } CharKind;

// Hit surface per body part (flesh effects): what a bullet meets first.
typedef enum CharSurface { CSURF_FLESH, CSURF_CLOTH, CSURF_ARMOR, CSURF_BONE, CSURF_COUNT } CharSurface;
// "blood": { "color": [0.30, 0.02, 0.015], "darkness": 0.55, "amount": 1.0, "mist": 1.0, "decay": 1.0 }
//   color / darkness: wet blood colour (darkness multiplies it down); amount scales droplets,
//   splatter and pools; mist scales the aerosol; decay scales how fast wounds stop bleeding.
// "surface": { "torso": "cloth", "limbs": "cloth", "head": "flesh" }   (part names or groups)
// "armor":   { "head": { "type": "helmet", "hits": 2 } }  sparks, no blood until it breaks
typedef struct CharBlood { vec3 color; float darkness, amount, mist, decay; } CharBlood;

// Part slots (one SkeletalMesh each, all skinned from the character's one SkeletonInstance). The order
// is the wire order of the replicated part selection (one u8 per slot).
typedef enum CharSlot {
    CSLOT_BODY, CSLOT_HEAD, CSLOT_TORSO, CSLOT_LEGS, CSLOT_HANDS, CSLOT_HAIR,
    CSLOT_GEAR_0, CSLOT_GEAR_1, CSLOT_GEAR_2, CSLOT_GEAR_3,
    CSLOT_COUNT
} CharSlot;
#define CHAR_MAX_PART_OPTIONS 32        // the body slot of players holds their own mesh + every skin
#define CHAR_PART_NONE 0          // selection byte: 0 = slot empty, k = option k - 1
#define CHAR_MAX_HIDE 4

const char *char_slot_name(int slot);
// "body", "head", ..., "gear_0".."gear_3"; -1 if unknown.
int char_slot_from_name(const char *name);

// Where a part's geometry comes from: a baked mesh, optionally cut / deformed (placeholder sets).
typedef struct MeshRef {
    char path[128];            // .fbx (baked through assets/cache) or .skm (tools/bake_mesh)
    char region[24];           // skeleton bone group to cut out of the mesh ("" = whole mesh)
    float inflate;             // m along the normals
    vec3 scale;                // about each vertex's dominant bone
} MeshRef;
u32 mesh_ref_hash(const MeshRef *r);
bool mesh_ref_is_plain(const MeshRef *r);
// Parses a path string or a { "mesh", "region", "inflate", "scale" } object.
bool mesh_ref_parse(MeshRef *r, const struct cJSON *e);
void mesh_ref_describe(const MeshRef *r, char *out, size_t n);

typedef struct CharPartOption {
    char id[32];               // console / UI name ("head_b", the skin id, or the file stem)
    MeshRef ref;
    int mesh;                  // AnimLibrary mesh index (-1 = unavailable)
    int skin;                  // AnimLibrary skin (texture atlas) or -1
    float weight;              // random variety weight (0 = only when chosen explicitly)
    bool is_default;           // picked instead of a random option (skins.json "player" / "npc")
    int material_count;        // material tints for this part (falls back to the character's)
    char material_names[SKIN_MAX_MATERIALS][48];
    vec4 material_colors[SKIN_MAX_MATERIALS];
    char hide_names[CHAR_MAX_HIDE][24];
    int hide_count;
    BoneSet hide;              // resolved: skeleton bones whose triangles the OTHER parts drop
} CharPartOption;

typedef struct CharPartSlot {
    int option_count;
    CharPartOption options[CHAR_MAX_PART_OPTIONS];
} CharPartSlot;

typedef struct CharacterDef {
    u32 hash;
    char id[32];
    u8 net_id;                     // character_id on the wire
    u8 kind;
    char mesh_path[128];
    char lod_paths[CHAR_LOD_FILES][128];   // optional explicit LOD files
    int mesh;                      // index into the library's meshes
    int material_count;
    char material_names[SKIN_MAX_MATERIALS][48];
    vec4 material_colors[SKIN_MAX_MATERIALS];
    char anim_set[32];
    int anim_set_index;
    float part_mult[BP_COUNT];
    CharBlood blood;
    u8 part_surface[BP_COUNT];     // CharSurface
    u8 armor_hits[BP_COUNT];       // > 0: armour on that part absorbs this many hits before breaking
    float ragdoll_scale;
    float scale;
    float hp;
    float move_speed;
    float lod_bias;
    CharSocket sockets[CHAR_MAX_SOCKETS];
    int socket_count;
    CharPartSlot slots[CSLOT_COUNT];
    int part_option_count;         // over all slots
    bool valid;                    // at least one part mesh bound + rig validated
} CharacterDef;

// Resolves socket bones against the skeleton (after loading) and adds the skeleton's sockets the
// character doesn't override. A missing "camera" socket gets the default eye socket on the Head bone.
void character_resolve_sockets(CharacterDef *d, const SkeletonAsset *a);
const CharSocket *character_socket(const CharacterDef *d, const char *name);

// Parts --------------------------------------------------------------------
// Resolves each option's "hide" groups against the skeleton.
void character_resolve_parts(CharacterDef *d, const SkeletonAsset *a);
// Deterministic part selection for a seed (entity id / spawn seed): default options win, otherwise a
// weighted random pick among weight > 0 options. out[CSLOT_COUNT].
void character_pick_parts(const CharacterDef *d, u32 seed, u8 *out);
// Option index in a slot by id, mesh path or file stem (-1 if missing).
int character_find_option(const CharacterDef *d, int slot, const char *name);
const CharPartOption *character_part(const CharacterDef *d, const u8 *sel, int slot);
// Skeleton bones the selected parts other than `slot` hide (slot -1 = all of them).
void character_hidden_bones(const CharacterDef *d, const u8 *sel, int slot, BoneSet *out);
// Material colours of a part as worn (the mesh's own, overridden by the option's tints by name).
void character_part_tints(const CharPartOption *o, const SkinMeshData *m, vec4 *out);
// Identity of a part's GPU meshes (render cache / instancing): characters wearing the same mesh with
// the same tints, texture and hidden bones share one GPU part, so their draws batch.
u32 character_part_key(int mesh, const vec4 *tints, int tint_count, u32 texture, const BoneSet *hidden);
// Appends an option to a slot (false when full).
bool character_add_option(CharacterDef *d, int slot, const CharPartOption *o);
// World transform of a socket from bone world transforms (false if missing).
bool character_socket_world(const CharacterDef *d, const char *name, const Xform *bone_world, Xform *out);

// Expands a part name or group ("arms", "hands", "legs", "feet", "limbs", "torso", "all") into
// out[BP_COUNT] flags; returns how many parts matched.
int character_part_group(const char *name, bool *out);
const char *char_surface_name(int s);
bool character_def_parse(CharacterDef *d, const char *json_text, const char *source, char *err, int err_size);

// Rig validation ------------------------------------------------------------
typedef struct RigReport {
    bool ok;
    int identical;                 // 1 = same set + hierarchy
    int missing, extra, hierarchy_errors, proportion_warnings;
    char text[8192];
    int len;
} RigReport;

// Compares a model's skeleton with the canonical one by bone name.
//   identical / subset (missing bones take the canonical bind) / extra leaf bones -> ok
//   renamed bones or a different hierarchy -> error (listed)
//   bone lengths differing by more than tolerance (e.g. 0.05) -> warning
bool rig_validate(const Skeleton *canon, const Skeleton *model, float tolerance, RigReport *r);

#endif
