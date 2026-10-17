// Pause menu > Commands (docs/UI_WHEELS_HUD.md "Commands tab"): the console registry as a list
// (search, categories, favorites, recent, inline cvar editing, reset, copy) over an embedded
// console that shares the ~ console's history, completion and hints. Destructive commands ask
// first. Shown with developer 1 or con_enable 1.
#ifndef DG_COMMANDS_TAB_H
#define DG_COMMANDS_TAB_H

#include "ui/gui/gui_widgets.h"

void commands_tab_register(void);
bool commands_tab_available(void);        // developer or con_enable
void commands_tab_open(void);
bool commands_tab_draw(GuiRect r);        // true = close
void commands_tab_confirm_result(bool yes);   // CONFIRM_COMMAND
// Scripted tests: the filter / category the list shows, and how many rows match.
void commands_tab_set_filter(const char *search, const char *category);
int commands_tab_visible_count(void);

#endif
