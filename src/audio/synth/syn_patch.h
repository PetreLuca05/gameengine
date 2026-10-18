// Patch files (assets/audio/patches/*.json, docs/AUDIO_PROCEDURAL.md "Patch format"):
//   { "name": "gun_blast", "loop": false, "max_dur": 0,
//     "params": { "energy": 1500, "tilt": [0, -12, 12] },          value or [default, lo, hi]
//     "nodes": [ { "id": "n", "type": "noise", "color": "pink" },
//                { "id": "e", "type": "env", "in": "n", "stages": [[0.4, 1], [18, 0, 4]] },
//                { "id": "lp", "type": "biquad", "mode": "lp", "in": "e", "hz": "$lp_hz" } ],
//     "out": "lp" }
// Node values: a number, "$param" / "$param*k" / "$param*k+o", {"param": p, "scale", "offset"} or
// {"mod": node, "scale", "offset"} (block-rate modulation by that node's output).
#ifndef DG_SYN_PATCH_H
#define DG_SYN_PATCH_H

#include "syn_graph.h"

typedef int (*SynSampleResolveFn)(const char *name, void *user);   // sample bank id, -1 missing

// Parses (not compiled). false + err on malformed JSON, unknown types / options / parameters / ids.
bool syn_patch_parse(const char *json, SynPatch *out, SynSampleResolveFn resolve, void *user, char *err, int err_size);
// The patch back as JSON (the editor's save); returns a malloc'd string (free it).
char *syn_patch_write(const SynPatch *p, const char *const *sample_names, int sample_count);

#endif
