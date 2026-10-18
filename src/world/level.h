// Level loading from a JSON description (static brushes + entities). See data/levels/devroom.json.
#ifndef DG_LEVEL_H
#define DG_LEVEL_H

#include "world.h"

// Builds static geometry/collision on any world; spawns entities only on the authority.
bool level_load(World *w, const char *path);
void level_draw_static(const World *w, vec3 cam_pos, vec3 cam_fwd);

// A level file parsed with its "include"s resolved (level_include.h), or NULL (missing: logged unless
// quiet). Every reader of a level file goes through these, so included content reaches all of them.
// The caller frees it (cJSON_Delete).
struct cJSON;
struct cJSON *level_json_load_file(const char *path, bool quiet);
struct cJSON *level_json_load(const char *map);   // data/levels/<map>.json; NULL for an empty name

#endif
