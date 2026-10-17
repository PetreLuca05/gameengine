// Client presentation of the revive loop (docs/REVIVE.md): the fabricator chamber (glass, liquid,
// the reconstruction effect on the player's own character) and the death sequence.
#ifndef DG_GAME_REVIVE_H
#define DG_GAME_REVIVE_H

#include "world/world.h"
#include "gameplay/revive/revive.h"

// Draws the chamber glass / liquid and the reconstructing body (from draw_fabricator).
void game_revive_draw_chamber(World *w, int i, vec3 pos, quat rot, const FabView *fv, float pulse);

#endif
