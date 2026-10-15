// Menus (Nuklear): pause, settings (graphics/controls/audio), multiplayer, inventory, containers.
#ifndef DG_MENUS_H
#define DG_MENUS_H

#include "core/base.h"

void menus_init(void);
void menus_draw(void);
void menus_toggle_pause(void);
void menus_toggle_inventory(void);
void menus_close_all(void);
bool menus_wants_mouse(void);   // any menu that needs a free cursor is open
void menus_tick(float dt);      // settings confirm countdown

#endif
