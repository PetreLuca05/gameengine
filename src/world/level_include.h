// Level includes (docs/ZOMBIE_MAP.md "Level includes"): a level file pulls other levels' content into
// itself with "include": ["devroom"] or [{"level": "devroom", "ground": {...}}]. Resolving merges the
// included level (its own includes first) into the including document, so every reader of the level
// file (the brushes / entities of level.c, the post volumes, water, vehicles, weather, puddles, the sun
// shadow) sees one document:
//   - arrays (brushes, entities, labels, spawns, vehicles, water, post_volumes, ...) are appended after
//     the including level's own entries;
//   - objects merge key by key, the including level's values winning (env, world, ...);
//   - anything else is taken only where the including level has no value ("room", "weather", ...).
// The content keeps its coordinates (no offset). "ground" flattens an open world under the included
// content: its footprint (brushes, entities, vehicles, water, the room, "footprint_areas"), grown by a
// margin, becomes pad features of the "world" block (flat ground at "level", no trees, "material").
// Pure (cJSON only): the files come from a callback.
#ifndef DG_LEVEL_INCLUDE_H
#define DG_LEVEL_INCLUDE_H

#include "core/base.h"

struct cJSON;
// The parsed data/levels/<level>.json (the caller frees it), or NULL.
typedef struct cJSON *(*LevelReadFn)(const char *level, void *user);

// Resolves root's "include" in place (removing the key). False (err set) on a missing level, a cycle or
// a malformed entry; root then holds what was merged so far.
bool level_include_resolve(struct cJSON *root, LevelReadFn read, void *user, char *err, size_t err_size);

typedef struct LevelGround {
    float level;      // pad height (m)
    float cell;       // footprint raster (m)
    float margin;     // grown around every element (m)
    float blend;      // pad blend into the terrain (m)
    float room_sink;  // the room floor's pad sits this far below `level` (its floor box covers it)
    char material[24];
} LevelGround;

void level_ground_defaults(LevelGround *g);
// The footprint of a level's content as pad features appended to `features` (a cJSON array). Returns
// the number of pads.
int level_footprint_pads(const struct cJSON *level, const LevelGround *g, struct cJSON *features);

#endif
