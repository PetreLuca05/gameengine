// Animation asset library: the canonical skeleton and everything authored against it
// (clips, controller, hold styles, damage presets, anim sets, ragdoll, hitboxes) plus the
// character definitions and their meshes. Loaded once at startup (render-free, so the CLI
// tools and unit tests use it too).
//
// Network identity: every asset category gets a stable 32-bit hash; the combined manifest is
// exchanged on join and mismatches are refused with a per-category error.
#ifndef DG_ANIM_LIBRARY_H
#define DG_ANIM_LIBRARY_H

#include "anim.h"
#include "anim_mesh.h"
#include "controller.h"
#include "procedural.h"
#include "physics/ragdoll.h"
#include "gameplay/hitboxes.h"
#include "assets/character.h"

#define LIB_MAX_CLIPS 160
#define LIB_MAX_SETS 8
#define LIB_MAX_CHARS 24
#define LIB_MAX_MESHES 12

typedef enum AssetCategory {
    AC_SKELETON, AC_CLIPS, AC_CONTROLLER, AC_HOLD_STYLES, AC_ANIM_SETS, AC_HITBOXES, AC_RAGDOLL, AC_CHARACTERS,
    AC_CATEGORY_COUNT
} AssetCategory;

typedef struct AnimManifest { u32 hash[AC_CATEGORY_COUNT]; } AnimManifest;

typedef struct AnimLibrary {
    bool loaded;
    float sample_rate;
    char human_dir[256];         // "assets/human"
    Skeleton skel;
    Clip clips[LIB_MAX_CLIPS];
    int clip_count;
    ClipLookup lookup;
    ControllerDef ctrl;
    ProcParams proc;
    HandFrames hands;
    AnimBinding sets[LIB_MAX_SETS];
    int set_count;
    RagdollDef ragdoll;
    HitboxTable hitboxes;
    float bone_radius[ANIM_MAX_BONES];
    SkinMeshData meshes[LIB_MAX_MESHES];
    char mesh_paths[LIB_MAX_MESHES][128];
    int mesh_count;
    CharacterDef chars[LIB_MAX_CHARS];
    int char_count;
    AnimManifest manifest;
    u32 revision;                // bumped on every (re)load
} AnimLibrary;

bool anim_lib_load(AnimLibrary *lib, float sample_rate);
void anim_lib_free(AnimLibrary *lib);
// Hot reload pieces (recompute hashes; the caller warns about live sessions).
bool anim_lib_reload_meta(AnimLibrary *lib);     // anim sets, clips.json, controller, hold styles
bool anim_lib_rebake(AnimLibrary *lib);          // changed FBX files (cache by hash)
bool anim_lib_reload_characters(AnimLibrary *lib);
bool anim_lib_reload_hitboxes(AnimLibrary *lib);

int anim_lib_find_set(const AnimLibrary *lib, const char *name);
int anim_lib_find_clip(const AnimLibrary *lib, const char *name);
const CharacterDef *anim_lib_char_by_net(const AnimLibrary *lib, u8 net_id);
const CharacterDef *anim_lib_char_by_id(const AnimLibrary *lib, const char *id);
const char *asset_category_name(int c);
u32 anim_manifest_combined(const AnimManifest *m);
// Writes anim_sets/<name>_template.json listing every controller slot (+ weapon patterns).
bool anim_lib_write_template(const AnimLibrary *lib, const char *name, bool zombie);

#endif
