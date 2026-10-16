// VFX library: effect / emitter definitions (assets/vfx/effects/*.json), the texture registry
// (assets/vfx/tex/*.meta.json) and effect selection for gameplay hooks. Pure: no renderer, no
// world, so the tests and tools can use it.
#ifndef DG_VFX_LIBRARY_H
#define DG_VFX_LIBRARY_H

#include "vfx_types.h"

struct cJSON;

typedef enum VfxCaliber { VFX_CAL_PISTOL, VFX_CAL_SMG, VFX_CAL_RIFLE, VFX_CAL_SHOTGUN, VFX_CAL_SNIPER, VFX_CAL_MELEE, VFX_CAL_COUNT } VfxCaliber;
typedef enum VfxMuzzleType { VFX_MUZZLE_PISTOL, VFX_MUZZLE_SMG, VFX_MUZZLE_RIFLE, VFX_MUZZLE_LMG, VFX_MUZZLE_SHOTGUN, VFX_MUZZLE_SNIPER, VFX_MUZZLE_TYPE_COUNT } VfxMuzzleType;
typedef enum VfxMuzzleDevice { VFX_DEV_BARE, VFX_DEV_HIDER, VFX_DEV_COMP, VFX_DEV_SUPPRESSOR, VFX_DEV_COUNT } VfxMuzzleDevice;

typedef struct VfxLibrary {
    VfxEffectDef effects[VFX_MAX_EFFECTS];
    int effect_count;
    VfxEmitterDef emitters[VFX_MAX_EMITTERS];
    int emitter_count;
    VfxTextureMeta textures[VFX_MAX_TEXTURES];
    int texture_count;
    char dir[256];             // assets/vfx
    u32 revision;              // bumped on every (re)load
    int load_errors;
    // gameplay selection tables (resolved after loading)
    int impact[SURF_COUNT];            // effect per surface material
    int impact_head;                   // flesh headshot variant
    int muzzle[VFX_MUZZLE_TYPE_COUNT][VFX_DEV_COUNT];
} VfxLibrary;

// Loads every effect and texture meta under dir (assets/vfx). Missing textures fall back to the
// built-in soft sprite; errors are logged and counted, never fatal.
bool vfx_lib_load(VfxLibrary *lib, const char *dir);
// Parses one effect (pure; used by the editor save/reload and the tests). Returns the effect
// index or -1.
int vfx_lib_add_effect_json(VfxLibrary *lib, const char *json_text, const char *source);
bool vfx_lib_parse_texture_meta(VfxTextureMeta *m, const char *json_text, const char *name);
int vfx_lib_find_effect(const VfxLibrary *lib, const char *name);
int vfx_lib_find_texture(const VfxLibrary *lib, const char *name);
// Re-resolves texture / sub-effect / selection references (after loading or editing).
void vfx_lib_resolve(VfxLibrary *lib);
// Writes an effect back to JSON (editor save). Caller frees the string.
char *vfx_lib_effect_to_json(const VfxLibrary *lib, int effect);

// Surface materials.
const char *surface_name(int m);
int surface_from_name(const char *s);          // -1 if unknown
// Impact effect for a material (body part >= 0 and head -> headshot variant). Never -1 when an
// impact_default effect exists.
int vfx_lib_impact_effect(const VfxLibrary *lib, int material, bool headshot);
int vfx_lib_muzzle_effect(const VfxLibrary *lib, int type, int device);
const char *vfx_muzzle_type_name(int t);
const char *vfx_muzzle_device_name(int d);
int vfx_muzzle_type_from_name(const char *s);
int vfx_muzzle_device_from_name(const char *s);
const char *vfx_caliber_name(int c);
int vfx_caliber_from_name(const char *s);
// Caliber scaling of impacts: count, size, energy multipliers.
typedef struct VfxCaliberScale { float count, size, energy, decal; } VfxCaliberScale;
VfxCaliberScale vfx_caliber_scale(int caliber);

#endif
