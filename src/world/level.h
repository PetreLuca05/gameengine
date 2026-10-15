// Level loading from a JSON description (static brushes + entities). See data/levels/devroom.json.
#ifndef DG_LEVEL_H
#define DG_LEVEL_H

#include "world.h"

// Builds static geometry/collision on any world; spawns entities only on the authority.
bool level_load(World *w, const char *path);
void level_draw_static(const World *w, vec3 cam_pos, vec3 cam_fwd);

#endif
