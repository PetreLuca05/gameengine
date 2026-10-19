// Observer client (docs/SPECTATE.md): watching another player through the local-player pipeline.
//
// In first / third person the watched player becomes this machine's *view slot* (game_view_slot):
// the camera, hold point, hands, sights, crosshair, HUD and post feedback all present it exactly as
// they present the local player. On a client it is re-simulated from its own relayed commands
// (game_sim_player + chars_local_predict, first = true once per command, so shots, recoil kicks and
// sounds fire as on the target), extrapolated with the last command while commands are late and
// reconciled against the host's local data like the local player. On the host it is simply the
// authoritative player. The free camera is a noclip camera of the observer's own.
#ifndef DG_SPECTATOR_H
#define DG_SPECTATOR_H

#include "net/spectate.h"
#include "render/render.h"
#include "gameplay/inventory.h"

struct World;

void spectator_init(void);                 // cvars + console commands (spectate, spec_next ...)
void spectator_reset(void);                // a new world (map load / connect)

// Net layer (client).
void spectator_cl_block(struct World *w, const SpecBlock *b, u32 tick);
void spectator_cl_local_data(struct World *w, int slot, const SpecLocalData *d, const SpecBlock *b);
void spectator_cl_cmds(struct World *w, Reader *r);
void spectator_cl_inventory(struct World *w, int slot, u32 revision, u16 hands, const InvTree *tree);

// Game loop.
void spectator_frame(float dt);            // input (spectator controls), death cam -> auto spectate, free camera
void spectator_tick(struct World *w);      // per simulation tick, after the local player
bool spectator_controls_input(void);       // the observer's input drives spectator controls only
void spectator_dump_tick(struct World *w, u32 local_seq);   // spec_dump: per-tick state hash (determinism test)

// Presentation (game_render.c / hud.c).
int spectator_mode(void);                  // SpecMode this machine is in
int spectator_target(void);
int spectator_view_slot(void);             // watched player in first / third person, -1 otherwise
bool spectator_first_person(void);
bool spectator_view_interp(PlayerMove *out, float alpha);   // the watched player's render state
bool spectator_view_angles(float alpha, float *yaw, float *pitch);
const UserCmd *spectator_view_cmd(void);   // its latest command (ADS, use, sway time)
vec2 spectator_frame_look(void);           // its view change this frame (hands look lag)
bool spectator_take_use_press(void);       // its use press since the last frame (hands reach)
bool spectator_free_camera(RenderCamera *cam);
void spectator_blend_camera(RenderCamera *cam, float dt);   // inertialized target switches
float spectator_delay_ms(void);            // estimated input -> screen delay of the watched player
void spectator_note_kick(int slot, u32 seq, vec2 kick_rad);   // game_sim_player: a command's recoil view kick

// The spectator bar (hud.c).
typedef struct SpectatorHud {
    int mode, target;
    char name[32];
    bool dead;                 // the local player is dead
    float delay_ms;
    bool show_delay;
    bool can_respawn;          // Respawn now (sv_respawn_mode 1)
    bool waiting_respawn;      // the timer ran out, waiting for the request
    float respawn_s;           // time until respawn (0 = none / waiting)
    float death_cam_left;      // death camera time left before spectating starts
    bool first_person_denied;  // an enemy: third person only
} SpectatorHud;
bool spectator_hud(SpectatorHud *out);     // false: nothing to show

#endif
