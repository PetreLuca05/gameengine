// Maps tab globe (docs/MENU_GLOBE.md): an orthographic world globe behind the Maps tab's panels.
// Grey satellite / relief texture streamed from a tile pyramid (assets/globe/tiles.dgt), vector
// coasts, borders, lakes, rivers and admin layers (assets/globe/vectors.dgv), elevation read-outs
// (assets/globe/heights.json). Selecting a map flies there: an overview of the region's country,
// its unit (state / judet: dimmed surroundings, hatch, outline drawn in, name), then onto the
// place, which the lock lines frame. Drag rotates (a spring brings it back).
#ifndef DG_GLOBE_H
#define DG_GLOBE_H

#include "ui/gui/gui.h"
#include "world/maplist.h"

void globe_register(void);   // cvars + commands (startup)
void globe_shutdown(void);
// Fly to a map's "globe" place (NULL or no place: the globe idles, turning slowly).
void globe_select(const MapInfo *m);
// Draws the globe over the whole screen into the offscreen layer and shows it (call before the
// panels: they blur it). Centre (cx, cy) in pixels; drag inside `input`; alpha fades it in.
void globe_draw(float cx, float cy, GuiRect input, float alpha);
// "Travelling", "Locked", "Standby" (the footer hint).
const char *globe_phase_text(void);
// Attribution of the data sources (one line, shown under the globe).
const char *globe_credits(void);

#endif
