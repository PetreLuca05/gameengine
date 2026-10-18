// Building kit in the world (docs/BUILDING_KIT.md "Engine integration"): a level's "kit" / "prefabs"
// (src/world/kit_layout.c) become static draws (one GPU mesh per piece and material slot, shared by every
// instance), static collision (UBX_ boxes / UCX_ convex meshes) and entities spawned at sockets (doors,
// lights, switches, containers: assets/kits/building/kit.json "entity"). Pieces hot reload: a re-exported
// .glb (mtime) or kit_reload rebuilds its meshes and every instance's draws and collision in place.
#ifndef DG_KIT_WORLD_H
#define DG_KIT_WORLD_H

#include "world/world.h"

struct cJSON;

void kit_world_register(void);
// level.c, at the end of level_load (after the brush draws): the level's kit instances.
void kit_world_level(World *w, const struct cJSON *level_root);
// Per frame (dev): pieces whose file changed reload (every ~1 s).
void kit_world_poll(World *w);
// Reloads one piece (NULL = every loaded piece) and rebuilds the instances; false if nothing reloaded.
bool kit_world_reload(World *w, const char *piece);
int kit_world_instance_count(void);

#endif
