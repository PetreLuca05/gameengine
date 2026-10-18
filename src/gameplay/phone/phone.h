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
void phone_tick(float dt);       // main loop, every frame: server, page events, portal code, status
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

// Pairing lives in the menus (docs/PHONE.md "Phone flow"): the Phone card (main menu Player tab,
// pause menu, the main menu footer) and the Connect portal modal (SCR_PHONE, src/ui/mainmenu/mm_phone.c).
typedef struct PhonePairInfo {
    int card;                  // PhoneCard (phone_os.h)
    int mode;                  // PhoneMode
    bool portal, connected, stored, server, code;
    float token_left, portal_t;  // s
    float portal_connected;      // s since a device connected on the open portal (-1: it did not)
    const char *device;        // the connected / stored device's name ("" unknown)
    const char *url, *address; // the pairing URL (with the code), ip:port
    const char *error;         // why there is no code / the last change
    u32 qr;                    // TextureId of the QR code (0 none)
} PhonePairInfo;
PhonePairInfo phone_pair_info(void);
void phone_portal_show(void);    // the portal opened: the server + a pairing code (no device yet)
void phone_portal_hide(void);
void phone_decline(void);        // "Use in-game phone": phone_mode ingame for the session
void phone_disconnect(void);     // the card's Disconnect: the page's socket closes, the in-game phone
void phone_forget(void);         // Forget device: the session ends, the stored device is deleted

#endif
