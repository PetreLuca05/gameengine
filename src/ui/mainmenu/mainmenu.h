// Main menu (docs/MENU_LOBBY.md): shown at launch (after a splash) and on "Leave to main menu".
// The background is a lit 3D scene (the dev room with the player's character, the selected map's
// season / time / weather, a slow camera drift) drawn by the normal pipeline; the UI is the game's
// UI (src/ui/gui, assets/ui/screens/mainmenu*.json) on the screen stack as SCR_MAINMENU.
//
//   top tab bar: Play | Player | Cosmetics | Maps | Settings | Credits      [power: quit]
//   Play: Host game, Join (server browser), Continue (stub), Quick play
//   while in a lobby the content is the lobby screen; the game's loading screen follows
#ifndef DG_MAINMENU_H
#define DG_MAINMENU_H

#include "core/base.h"
#include "ui/gui/gui_widgets.h"

void mainmenu_init(void);               // commands, the background scene (menus_init)
bool mainmenu_enabled(void);            // the menu flow is on (not --test, unless --menu / +mainmenu)
void mainmenu_boot(void);               // startup: background world, splash, main menu
// Back to the main menu: background world, the menu screen on the stack. message: a notice
// ("Disconnected: ...") shown once; NULL = none.
void mainmenu_enter(const char *message);
bool mainmenu_active(void);             // SCR_MAINMENU is on the stack
void mainmenu_session_started(void);    // the game is on: the menu and loading screen close
void mainmenu_leave_scene(void);        // before a game map loads (the background scene ends)
const char *mainmenu_selected_map(void);
void mainmenu_lobby_settings_changed(void);   // the lobby's map / season / time changed: background follows

// Screen hooks (menus.c)
bool mainmenu_draw(bool top);           // SCR_MAINMENU; true = it wants to close (never)
void mainmenu_tick(float dt);
bool mainmenu_chat_draw(bool top);      // SCR_CHAT (Enter in game): true = close
void mainmenu_chat_open(void);
bool mainmenu_lobby_page_draw(GuiRect r);   // pause > Lobby (players, chat, invite, kick): true = close
void mainmenu_chat_feed_draw(void);     // in game: recent chat lines fade in / out above the vitals
void mainmenu_reload_layouts(void);     // ui_reload

#endif
