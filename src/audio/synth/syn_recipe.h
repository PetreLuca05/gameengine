// Sound recipes (assets/audio/recipes/*.json, docs/AUDIO_PROCEDURAL.md "Recipe format"): up to four
// layers, each a patch (or a sample, which becomes a one-node patch) with parameter bindings from
// the triggering event's physical values, seeded random ranges, a blend gain and a condition.
// Hybrid is the point: a gunshot is a synthesized blast + crack + mechanism over a generated sample
// layer that carries the gun's mechanical character.
//
//   { "name": "gunshot", "bus": "sfx", "priority": 0.9, "echo": true,
//     "distance": { "min": 6, "max": 2500, "rolloff": 0.6 },
//     "directivity": { "front_db": 2, "rear_db": -6, "rear_lp": 3500 },
//     "inputs": { "energy": 1500, "barrel": 0.4, "device": 0 },
//     "layers": [
//       { "patch": "gun_blast", "bind": { "energy": "energy", "burst_ms": "lerp(barrel, 0.25, 22, 0.6, 14)" },
//         "random": { "energy": ["db", -1.5, 1.5] } },
//       { "sample": "shot_rifle", "gain_db": "device == 3 ? -30 : -6", "pitch": [0.97, 1.03] } ] }
//
// The library owns every patch and recipe. Reloading a patch keeps the old copy alive (the audio
// thread may be rendering it); recipes refer to patches by index, so they pick up the new one.
#ifndef DG_SYN_RECIPE_H
#define DG_SYN_RECIPE_H

#include "syn_graph.h"
#include "syn_expr.h"
#include "syn_patch.h"

#define RCP_MAX_INPUTS 24
#define RCP_MAX_LAYERS 4
#define RCP_MAX_BINDS 20
#define SYN_LIB_PATCHES 160
#define SYN_LIB_RECIPES 128
#define RCP_MAX_SAMPLES 24       // names in one "sample": [...] list (one per material)
#define SYN_MAX_VARIANTS 16      // recorded takes <name>_1 .. <name>_16
#define SYN_LIB_VARIANTS 256     // sample names whose takes were counted

typedef enum SynRandKind { SYN_RAND_NONE, SYN_RAND_DB, SYN_RAND_MUL, SYN_RAND_ADD } SynRandKind;
typedef enum SynBus { SYN_BUS_SFX, SYN_BUS_VOICE, SYN_BUS_UI, SYN_BUS_AMBIENCE, SYN_BUS_COUNT } SynBus;

typedef struct SynBind {
    int param;                   // patch parameter
    bool has_expr;
    SynExpr expr;
    u8 rand;                     // SynRandKind
    float rlo, rhi;
} SynBind;

typedef struct SynLayer {
    int patch;                   // library index (-1 for a dynamic sample layer until resolved)
    bool dyn;                    // "sample": "@<suffix>": the event's sample name + suffix (a weapon's sound_fire)
    char dyn_suffix[16];
    // "sample": "name" or ["a", "b", ...] picked by "sample_sel" (rounded, clamped): a fixed sample.
    // Any name, fixed or dynamic, with recorded takes <name>_1 .. <name>_N plays one take per seed.
    int nsamples;
    char sample[RCP_MAX_SAMPLES][24];
    bool has_sel;
    SynExpr sel;
    bool optional;               // dropped first under load
    bool has_when;
    SynExpr when, gain_db;
    int nbind;
    SynBind bind[RCP_MAX_BINDS];
} SynLayer;

typedef struct SynRecipe {
    char name[32];
    int ninputs;
    char input_name[RCP_MAX_INPUTS][20];
    float input_def[RCP_MAX_INPUTS];
    int nlayers;
    SynLayer layer[RCP_MAX_LAYERS];
    u8 bus;                      // SynBus
    float priority;              // 0..1
    bool loop, echo, doppler, spatial, env;
    int max_instances;
    float min_dist, max_dist, rolloff;
    float front_db, rear_db, rear_lp;   // directivity (forward = the emitter's forward)
    float reverb;                // send scale
    float stack_s;               // same recipe within this time folds into the louder (0.03 s)
} SynRecipe;

typedef struct SynLayerOut { bool on; const SynPatch *patch; float gain; bool optional; float params[SYN_MAX_PARAMS]; } SynLayerOut;
typedef struct SynRecipeOut { int nlayers; SynLayerOut layer[RCP_MAX_LAYERS]; } SynRecipeOut;

typedef struct SynLibrary {
    float sr;
    SynSampleResolveFn resolve;
    void *user;
    int npatches;
    SynPatch *patch[SYN_LIB_PATCHES];
    SynPatch *graveyard[SYN_LIB_PATCHES];   // replaced copies, freed at shutdown
    int ngrave;
    int nrecipes;
    SynRecipe *recipe[SYN_LIB_RECIPES];
    int nvar;                    // take counts per sample name (counted once: the bank only grows)
    u32 var_hash[SYN_LIB_VARIANTS];
    u8 var_count[SYN_LIB_VARIANTS];
} SynLibrary;

void syn_lib_init(SynLibrary *lib, float sr, SynSampleResolveFn resolve, void *user);
void syn_lib_free(SynLibrary *lib);
// Parse + compile a patch; replaces one of the same name. Returns its index, -1 + err.
int syn_lib_add_patch(SynLibrary *lib, const char *json, char *err, int err_size);
// Installs an already-parsed patch (the editor's live edits); compiles it. Index or -1.
int syn_lib_put_patch(SynLibrary *lib, const SynPatch *p, char *err, int err_size);
int syn_lib_find_patch(const SynLibrary *lib, const char *name);
// The implicit one-node patch "sample:<name>" (pitch, amp, gate); -1 when the sample is missing.
int syn_lib_sample_patch(SynLibrary *lib, const char *sample, bool loop);
// The number of recorded takes <sample>_1 .. <sample>_N (0: none, the sample is a single file).
int syn_lib_sample_variants(SynLibrary *lib, const char *sample);
// The sample patch for a name and seed: one of its takes when it has some (the same take for the
// same seed), else the sample itself; -1 when neither exists.
int syn_lib_sample_pick(SynLibrary *lib, const char *sample, u32 seed, bool loop);
int syn_lib_add_recipe(SynLibrary *lib, const char *json, char *err, int err_size);
int syn_lib_find_recipe(const SynLibrary *lib, const char *name);
// A one-layer recipe around a sample with the legacy defaults (audio_play): created on first use.
int syn_lib_sample_recipe(SynLibrary *lib, const char *sample, bool loop, bool spatial, int bus);

int syn_recipe_input(const SynRecipe *r, const char *name);   // -1 unknown
// Every layer's on / gain / parameters for these inputs (ninputs values) and seed. Deterministic:
// the same inputs and seed give the same result, so re-evaluating a playing voice keeps its random offsets.
// sample: the event's sample name for "@" layers (NULL: those layers are off).
void syn_recipe_eval(SynLibrary *lib, const SynRecipe *r, const float *inputs, u32 seed, const char *sample, SynRecipeOut *out);

#endif
