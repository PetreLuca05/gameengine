// Sun shadow caster cascade limits (docs/SHADOWS.md "Caster cascade limits", docs/perf/FINDINGS.md
// PF-0013): grass and small plants cast only into the near cascade(s), trees into all of them.
//
//   classes    every vgeo material (and so every mesh) has a caster class: ground (grass, ground
//              cover, flowers), small (bushes, ferns, reeds, wheat, small plants), tree, or none
//              (unclassified: never limited). Grass assets / model foliage blocks name it
//              ("shadow_class"); procedural kinds and Tree It species take it from their kind / role.
//              A mesh takes the least limited class of its materials.
//   limits     assets/render/shadow_casters.json: per r_shadow_caster_limit mode (1 the High / Modern
//              tiers, 2 Off .. Medium; 0 = off, every caster in every cascade) and class, the last
//              cascade the class casts into and an optional distance cap (m). A limited class ends at
//              the smaller of its last cascade's selection distance and the cap, dithering out over
//              the last `fade` of that distance (no line where its shadows end). While the near
//              cascade fades into cascade 1 (PCSS wide mode) the limited classes also cast into the
//              next cascade, so their reach (in metres) stays the same.
//
// Pure: no renderer (tests/test_shadow_casters.c). vk_classic.c applies it per cascade view.
#ifndef DG_SHADOW_CASTERS_H
#define DG_SHADOW_CASTERS_H

#include "core/base.h"

typedef enum ShadowCasterClass { SCC_NONE, SCC_GROUND, SCC_SMALL, SCC_TREE, SCC_COUNT } ShadowCasterClass;
#define SCC_MODES 3                 // r_shadow_caster_limit 0 (off), 1, 2
#define SCC_CASCADES 4

typedef struct SccLimit {
    int cascade;                    // the last cascade the class casts into (-1: every cascade)
    float max_m;                    // m from the camera: the class casts no shadow beyond (0: no cap)
} SccLimit;

typedef struct SccTable {
    float fade;                     // fraction of a limited class's end distance it dithers out over
    SccLimit lim[SCC_MODES][SCC_COUNT];   // mode 0 is always "no limit"
} SccTable;

void scc_defaults(SccTable *t);     // = assets/render/shadow_casters.json
// Parses the JSON over the defaults; false + err on a malformed table (unknown mode / class / key).
bool scc_parse(SccTable *t, const char *json, char *err, int err_size);

const char *scc_class_name(int c);  // "none" | "ground" | "small" | "tree"
int scc_class_from_name(const char *s);   // -1 on an unknown name
// The class of a mesh with materials of classes a and b: the least limited one (ground < small <
// tree < none).
int scc_merge(int a, int b);

// One sun cascade view: which classes cast into cascade k, and where each one ends.
typedef struct SccView {
    u32 allow;                      // bit per class: casts into this cascade
    float end_m[SCC_COUNT];         // m: the class casts nothing beyond (0: no limit)
    float fade_m[SCC_COUNT];        // m: it dithers out over [end - fade, end]
} SccView;

// Cascade k of ncasc with selection distances splits[ncasc] (the last = the shadow distance); wide:
// the near cascade fades into cascade 1 (its weight < 1); border: the cascade blend band (fraction
// of a cascade's range, lighting.glsl). Mode out of range = 0 (no limit).
void scc_view(const SccTable *t, int mode, int k, int ncasc, const float *splits, bool wide, float border, SccView *out);
// A caster of class c at distance d from the camera: its weight in the view (1 opaque .. 0 culled).
float scc_alpha(const SccView *v, int c, float d);
// The largest end distance over the classes in mask that cast into the view: whole-cell culling.
// 0: one of them is unlimited; -1: none of them casts here (skip the cell).
float scc_mask_end(const SccView *v, u32 mask);

#endif
