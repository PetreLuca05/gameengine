// In-game phone (docs/PHONE.md): a held camera item whose screen shows a live render target, a
// companion web page on the player's real phone (QR pairing, live feed, orientation, photo /
// video / zoom), photos + videos + gallery. Game-side glue of the pure modules in this folder
// and src/net/http_ws.
//
//   build_cmd    phone_fill_cmd        landscape / driven rotation / recording -> UserCmd
//   hotbar       phone_wheel           camera app: the wheel zooms; the panel: scroll / zoom
//   game_render  phone_frame           after the hands: server, orientation, lens view, capture
//                phone_draw            HUD hints, the pairing panel
//   hands.c      phone_draw_held       the phone model with its live screen
#ifndef DG_PHONE_H
#define DG_PHONE_H

#include "core/base.h"
#include "core/dgmath.h"

struct World;
struct Player;
struct RenderCamera;
struct ItemDef;
struct UserCmd;

void phone_register(void);       // cvars + commands (game_render_init)
void phone_shutdown(void);
void phone_fill_cmd(struct UserCmd *c);
bool phone_wheel(int dir);       // true = consumed (zoom in camera mode)
void phone_frame(struct World *w, const struct RenderCamera *cam, const struct Player *lp);
void phone_draw(void);
void phone_draw_held(const struct ItemDef *it, vec3 pos, quat rot, bool viewmodel, u32 flags);

// State queries (HUD, settings, tests).
bool phone_holding(void);        // the local player holds a camera item
bool phone_camera_mode(void);    // ... raised (aiming)
bool phone_paired(void);         // a companion page is connected
const char *phone_status_text(void);
bool phone_ui_wants_mouse(void);   // the cursor is out for the HUD phone panel (phoneui, Alt)
bool phone_ui_blocks_game(void);   // gamepad navigation of the panel holds the game controls
bool phone_ui_escape(void);        // Esc with the cursor on the panel: back (true = consumed)
// The character about to be drawn holds a phone: its replicated screen (characters.c).
void phone_set_draw_state(u8 app, u8 flags, u8 zoom);

#endif
