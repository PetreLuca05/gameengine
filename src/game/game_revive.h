// Client presentation of the revive loop (docs/REVIVE.md): the fabricator chamber (glass, liquid,
// the reconstruction effect on the player's own character) and the death sequence.
#ifndef DG_GAME_REVIVE_H
#define DG_GAME_REVIVE_H

#include "world/world.h"
#include "gameplay/revive/revive.h"

// Draws the chamber glass / liquid and the reconstructing body (from draw_fabricator).
void game_revive_draw_chamber(World *w, int i, vec3 pos, quat rot, const FabView *fv, float pulse);
// Per frame: the reconstruction bodies (the player's own character dissolving in), motes, bubbles,
// mist, the completion burst and camera bump; reconstruction_preview in the dev room.
void game_revive_frame(World *w, float dt);
// Displayed reconstruction progress of a fabricator (the replicated state, or the preview), < 0 = none.
float game_revive_display_progress(World *w, EntityId fab, int *player_slot);

// Death sequence (game_death.c).
struct PostSettings;
struct DeathFx;
void game_death_register(void);
void game_death_frame(float dt);                 // after the frame's ticks
void game_death_post(struct PostSettings *s);    // after the gameplay feedback (game_post.c)
bool game_death_hud_hidden(void);
const struct DeathFx *game_death_fx(void);
int game_death_stage(void);

#endif
