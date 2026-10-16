// Character definitions (assets/characters/*.json) and rig validation against the shared,
// canonical skeleton. Adding a model = one FBX + one JSON, no code:
//
//   { "id": "zombie_walker", "character_id": 3, "kind": "zombie",
//     "mesh": "assets/human/Dummy.fbx", "materials": { "Alpha_Body_MAT": [0.4, 0.5, 0.35] },
//     "anim_set": "zombie", "part_mult": { "head": 4.0, "limbs": 0.5 },
//     "ragdoll_scale": 1.0, "scale": 1.0, "hp": 120, "move_speed": 1.2 }
#ifndef DG_CHARACTER_H
#define DG_CHARACTER_H

#include "anim/anim.h"
#include "anim/body_parts.h"
#include "anim/anim_mesh.h"

#define CHAR_LOD_FILES 4
#define CHAR_MAX_SOCKETS 8

// Named attach point parented to a bone (like an Unreal Engine skeletal mesh socket): relative
// location / rotation in the bone's space. JSON:
//   "sockets": { "camera": { "bone": "Head", "offset": [0, 0.10, -0.10], "rotation": [0, 0, 0], "space": "bind" } }
// "space": "bone" = offset / rotation in the bone's local frame; "bind" (default) = model-space
// offset at the bind pose (character faces -Z, +Y up), converted to the bone's frame at load.
typedef struct CharSocket {
    char name[24];
    char bone_name[48];
    int bone;                      // resolved skeleton index (-1 = unresolved)
    vec3 offset, rotation_deg;
    bool bind_space;
    Xform rel;                     // bone space
} CharSocket;

typedef enum CharKind { CK_PLAYER, CK_ZOMBIE, CK_DUMMY } CharKind;

// Hit surface per body part (flesh effects): what a bullet meets first.
typedef enum CharSurface { CSURF_FLESH, CSURF_CLOTH, CSURF_ARMOR, CSURF_BONE, CSURF_COUNT } CharSurface;
// "blood": { "color": [0.30, 0.02, 0.015], "darkness": 0.55, "amount": 1.0, "mist": 1.0, "decay": 1.0 }
//   color / darkness: wet blood colour (darkness multiplies it down); amount scales droplets,
//   splatter and pools; mist scales the aerosol; decay scales how fast wounds stop bleeding.
// "surface": { "torso": "cloth", "limbs": "cloth", "head": "flesh" }   (part names or groups)
// "armor":   { "head": { "type": "helmet", "hits": 2 } }  sparks, no blood until it breaks
typedef struct CharBlood { vec3 color; float darkness, amount, mist, decay; } CharBlood;

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
    bool valid;                    // mesh bound + rig validated
} CharacterDef;

// Resolves socket bones against the skeleton (after loading). Missing "camera" sockets on player
// characters get the default eye socket on the Head bone.
void character_resolve_sockets(CharacterDef *d, const Skeleton *s);
const CharSocket *character_socket(const CharacterDef *d, const char *name);
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
