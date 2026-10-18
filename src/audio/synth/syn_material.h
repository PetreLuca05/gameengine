// Acoustic materials (assets/audio/materials.json, docs/AUDIO_PROCEDURAL.md "Materials"): one table
// shared by synthesis (modal impact / footstep resonators, contact time, noise burst) and the
// environment (wall absorption per band, outdoor reflectivity, transmission loss through walls).
// Indices 0..16 follow SurfaceMaterial (vfx/vfx_types.h); the rest are audio-only surfaces
// (footsteps on snow, mud, leaves; the metal roof under rain).
#ifndef DG_SYN_MATERIAL_H
#define DG_SYN_MATERIAL_H

#include "core/base.h"

#define SYN_MAT_MODES 16
#define SYN_MAT_MAX 24
#define SYN_BANDS 3              // < 250 Hz, 250 Hz .. 2 kHz, > 2 kHz

enum {
    SYN_MAT_DEFAULT, SYN_MAT_CONCRETE, SYN_MAT_BRICK, SYN_MAT_PLASTER, SYN_MAT_METAL_THIN, SYN_MAT_METAL_THICK,
    SYN_MAT_WOOD, SYN_MAT_DIRT, SYN_MAT_SAND, SYN_MAT_GRAVEL, SYN_MAT_GRASS, SYN_MAT_GLASS, SYN_MAT_WATER,
    SYN_MAT_FLESH, SYN_MAT_CLOTH, SYN_MAT_TILE, SYN_MAT_ASPHALT,
    SYN_MAT_SURF_COUNT,          // == SURF_COUNT
    SYN_MAT_SNOW = SYN_MAT_SURF_COUNT, SYN_MAT_MUD, SYN_MAT_LEAVES, SYN_MAT_ROOF_METAL, SYN_MAT_FOLIAGE,
    SYN_MAT_BUILTIN_COUNT
};

typedef struct SynMaterial {
    char name[16];
    int nmodes;
    float f[SYN_MAT_MODES], t60[SYN_MAT_MODES], g[SYN_MAT_MODES];   // Hz, s, linear
    float contact_ms;            // impulse width of a hit (soft = wide = darker)
    float noise;                 // broadband burst level next to the modes (0..1)
    float noise_hz;              // burst band centre
    float absorb[SYN_BANDS];     // wall absorption coefficient per band (0..1)
    float reflect;               // outdoor echo reflectivity (0..1)
    float tl_base[SYN_BANDS];    // transmission loss of a thin layer, dB
    float tl_per_cm[SYN_BANDS];  // + per cm of thickness, dB
    float tl_max;                // dB cap of one wall
} SynMaterial;

typedef struct SynMaterialTable {
    int count;
    SynMaterial m[SYN_MAT_MAX];
} SynMaterialTable;

// Built-in defaults (also what the JSON overrides), and the active table.
void syn_material_defaults(SynMaterialTable *t);
bool syn_material_parse(SynMaterialTable *t, const char *json, char *err, int err_size);
void syn_material_set_table(const SynMaterialTable *t);
const SynMaterialTable *syn_material_table(void);
const SynMaterial *syn_material(int index);   // clamped; never NULL
int syn_material_find(const char *name);       // -1 unknown
// Transmission loss (dB, per band) through `thickness_m` of material.
void syn_material_tl(const SynMaterial *m, float thickness_m, float out_db[SYN_BANDS]);

#endif
