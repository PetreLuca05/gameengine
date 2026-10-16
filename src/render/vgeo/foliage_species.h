// Imported foliage species (docs/FOLIAGE_WIND.md "Species"): data/foliage/species.json, one entry per
// asset:
//   { "name": "treeit_oak", "source": "assets/treefbx/oak.fbx", "preset": "tree" | "bush" | "grass",
//     "unit": 0.01 (OBJ: metres per unit), "wind_channels": { "flutter": "r", "phase": "g", "branch": "b" }
//     | "auto", "normalize_branch": true, "materials": { "<name substring>": "bark" | "leaf" },
//     "wind": { "trunk_stiffness", "branch_stiffness", "leaf_stiffness", "full_wind", "main_freq",
//               "detail_freq", "flutter_freq", "max_bend_deg", "settle_time", "response" },
//     "capsule": [radius, height], "fade": [start, end], "shadow_max_dist": m, "alpha_cutoff": 0.5,
//     "roughness": [bark, leaf], "impostor": true }
// Wind parameters not given take the preset's defaults for the imported height (vgeo_wind_defaults).
// Pure (cJSON; the renderer builds, textures and registers them: render_foliage_species_types).
#ifndef DG_FOLIAGE_SPECIES_H
#define DG_FOLIAGE_SPECIES_H

#include "foliage_import.h"
#include "foliage_wind.h"

#define VG_SPECIES_MAX 32

typedef struct VgSpecies {
    char name[48];
    char source[160];
    int preset;                      // VgWindPreset
    VgFoliageImportOptions import;
    float wind_set[11];              // VgWindParams fields given in the JSON (NAN = default), VgWindParams order
    bool capsule;
    float capsule_r, capsule_h;
    float fade_start, fade_end, shadow_max_dist, alpha_cutoff;
    float rough_bark, rough_leaf;
    bool impostor;
    u32 key;                         // FNV-1a of the entry's JSON text (cache key part)
} VgSpecies;

typedef struct VgSpeciesLib {
    VgSpecies s[VG_SPECIES_MAX];
    int count;
} VgSpeciesLib;

// Parses the species file text. Returns false (err) on a syntax error; bad entries are skipped with a
// message in err (still true).
bool vgeo_species_parse(const char *json, VgSpeciesLib *lib, char *err, int err_size);
int vgeo_species_find(const VgSpeciesLib *lib, const char *name);
// The wind parameters of an imported species: the preset's defaults for its height, then the JSON's.
void vgeo_species_wind(const VgSpecies *sp, float height, VgWindParams *out);

#endif
