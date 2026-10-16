// Game screens (docs/UI.md): pause menu with centred sub-menus (settings, multiplayer,
// inventory), confirm dialogs, the weapon bench. The screen stack and pause logic are in menus.c.
#ifndef DG_MENUS_H
#define DG_MENUS_H

#include "core/base.h"

union SDL_Event;

void menus_init(void);
void menus_draw(void);
void menus_toggle_pause(void);  // Esc with no screen open: pause (Esc with screens: "back", handled by the UI)
void menus_toggle_inventory(void);
void menus_close_all(void);
bool menus_wants_mouse(void);   // any menu that needs a free cursor is open
bool menus_open(void);          // any game screen is open (the HUD hides)
bool menus_paused(void);        // the pause menu stops the simulation (single player / host alone)
bool menus_can_pause(void);
void menus_tick(float dt);      // UI frame start, global keys, settings countdown
bool menus_handle_event(const union SDL_Event *ev);
bool menus_dev_tools_open(void);   // Nuklear developer editors (they take the input)
void menus_frame_end(void);        // flushes the game UI (ui_render)

#endif
