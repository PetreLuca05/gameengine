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

// game_holdpoint.c
void game_holdpoint_register(void);
void game_holdpoint_test_frame(void);
bool game_holdpoint_test_active(void);
bool holdpoint_save_all(const char *path_override);

// game_post.c: post-processing settings (presets, post volumes, gameplay feedback, photo mode)
typedef struct PostFrameInfo {
    vec3 cam_pos;
    bool ads, sprinting;
    float level_fog;
} PostFrameInfo;
void game_post_init(void);
void game_post_frame(const PostFrameInfo *fi);
void game_post_draw_overlay(void);
void game_post_draw_photo_ui(void);
void game_post_flash(float strength);
bool game_post_photo_mode(void);
void game_post_set_photo_mode(bool on);
int game_post_detect_preset(void);
const char *game_post_preset_name(int p);
void game_post_apply_preset(int p);
float game_post_bench_post_ms(void);

#endif
