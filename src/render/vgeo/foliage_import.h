// Foliage import (docs/FOLIAGE_WIND.md "Import"): Tree It FBX / OBJ exports (or any FBX / OBJ with
// wind weights in the vertex colour) -> vgeo builder input with the canonical wind channels of
// foliage_wind.h (R flutter, G phase, B branch). Per asset (data/foliage/species.json):
//   - "wind_channels": { "flutter": "r", "phase": "g", "branch": "b" } remaps the source channels
//     ("r" "g" "b" "a", "none" = infer that channel, "auto" for the whole block = detect)
//   - the branch channel is normalised to a max of 1 (Tree It exports differ per tree: pine 0.10, oak 0.74)
//   - no vertex colours (or "none"): the weights are inferred from the geometry (flutter along each card
//     from its trunk-side end, phase per piece, branch = distance from the trunk axis)
//   - parts are bark (rigid, opaque) or leaf cards (masked, two-sided) by name, else by shape
// Pure (ufbx, no renderer). Tree It writes cm: FBX carries its unit, OBJ needs "unit": 0.01.
#ifndef DG_FOLIAGE_IMPORT_H
#define DG_FOLIAGE_IMPORT_H

#include "vgeo_import.h"

enum VgFoliageChannel { VGFC_FLUTTER, VGFC_PHASE, VGFC_BRANCH, VGFC_COUNT };
#define VGFC_NONE 4                  // source channel index: infer
enum VgFoliageMatKind { VGFM_BARK, VGFM_LEAF };

typedef struct VgFoliageImportOptions {
    int channels[VGFC_COUNT];        // source channel per canonical one: 0 R, 1 G, 2 B, 3 A, VGFC_NONE
    bool detect;                     // detect the mapping from the file (vgeo_foliage_detect_channels)
    float unit;                      // metres per file unit (0: FBX: the file's unit; OBJ: 1)
    bool normalize_branch;           // branch channel scaled to max 1 (default true)
    bool ignore_colors;              // infer every channel
    struct { char pattern[32]; u8 kind; } kinds[8];   // material / mesh name substring -> VgFoliageMatKind
    int kind_count;
} VgFoliageImportOptions;
void vgeo_foliage_import_defaults(VgFoliageImportOptions *o);   // Tree It: R G B, normalised, no overrides

typedef struct VgFoliageAsset {
    VgImported im;                   // metres, pivot (trunk base) at the origin, y up; material_names
    u32 *color;                      // canonical RGBA8 wind weights per vertex
    u8 *material_kind;               // per material (VgFoliageMatKind)
    char (*textures)[64];            // per material: albedo file name next to the source ("" = none)
    float height, radius;            // m: top above the pivot, max distance from the trunk axis
    float branch_scale;              // normalisation applied to the branch channel
    bool inferred;                   // some channel was inferred
    int channels[VGFC_COUNT];        // the mapping used
} VgFoliageAsset;
bool vgeo_foliage_import(const char *path, const VgFoliageImportOptions *o, VgFoliageAsset *out, char *err, int err_size);
void vgeo_foliage_asset_free(VgFoliageAsset *a);
void vgeo_foliage_asset_source(const VgFoliageAsset *a, VgSource *src);   // pointers into a

// Channel detection from the data alone (the evidence of docs/FOLIAGE_WIND.md):
//   phase    the channel most constant per connected piece over bark and cards (pooled within-piece
//            variance fraction), with >= 3 distinct values
//   flutter  of the others, ~0 on the bark and ~0 at one end of most cards (the stem end)
//   branch   the remaining one (reported: its correlation with the distance to the trunk axis on bark)
typedef struct VgFoliageChannelReport {
    bool has_color;
    int flutter, phase, branch;      // source channel indices (0..3), -1 = undecided
    float flutter_bark_max;          // the flutter channel's max on bark parts
    float flutter_uv_corr;           // its correlation with the card uv v (Tree It: + towards the tip)
    float flutter_card_zero;         // fraction of cards whose min is ~0
    float phase_const;               // 1 - pooled within-piece variance fraction of the phase channel
    float branch_radial_corr;        // branch channel vs distance from the trunk axis, bark parts
    float branch_max;
    char note[128];
} VgFoliageChannelReport;
bool vgeo_foliage_detect_channels(const char *path, VgFoliageChannelReport *r, char *err, int err_size);

// Parses "r" / "g" / "b" / "a" / "none" (-1: invalid).
int vgeo_foliage_channel_parse(const char *s);

#endif
