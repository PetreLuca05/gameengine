// The Stats tab (docs/STATS.md "Stats tab"): pause menu > Stats (a centred modal) and the main
// menu's Stats tab. Sub-pages Overview, Performance, Weapons, Movement, Match history, Activity;
// a player selector (any connected player's public stats) with a compare mode. Charts from
// ui_chart.h, rebuilt when the tab opens or the stats change (at most once a second while open).
#ifndef DG_UI_STATS_TAB_H
#define DG_UI_STATS_TAB_H

#include "ui/gui/gui_widgets.h"

void stats_tab_register(void);            // commands (stats_tab)
void stats_tab_open(void);                // charts rebuilt, the player list refreshed
bool stats_tab_draw(GuiRect r);           // the tab's content in r (main menu); true = close
bool stats_tab_draw_modal(GuiRect r);     // pause menu: the modal frame + the content; true = close
void stats_tab_confirm_reset(bool yes);   // CONFIRM_STATS_RESET

#endif
