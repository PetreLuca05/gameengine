// Functions shared between the game/*.c files (not part of the public game API).
#ifndef DG_GAME_INTERNAL_H
#define DG_GAME_INTERNAL_H

#include "game.h"

void game_sim_player(World *w, int slot, const UserCmd *cmd, float dt, bool first);
void game_cl_reconcile(const PlayerSim *server, u32 ack_seq);
int game_spawn_player(World *w, int slot, const char *name, bool local, int peer);
void game_remove_player(World *w, int slot);
void game_respawn_player(World *w, int slot);
void game_create_player_character(World *w, int slot, vec3 feet);

// game_events.c
void game_add_hint(const char *text);
void game_hud_tick(float dt);

// game_render.c
void game_render_init(void);
void game_render_world(World *w, float alpha, vec3 cam_pos, vec3 cam_fwd);
MeshId game_capsule_mesh(float radius, float half_height);

#endif
