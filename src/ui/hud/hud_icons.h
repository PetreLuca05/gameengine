// HUD vector icons (docs/UI_WHEELS_HUD.md "Icons"): small line-drawn glyphs for pings, door actions,
// the kill feed and the compass, drawn with the UI draw list in one colour so they follow the theme.
#ifndef DG_HUD_ICONS_H
#define DG_HUD_ICONS_H

#include "core/base.h"

// Draws icon `name` centred at (cx, cy) within a size x size box. False: unknown name (nothing drawn).
bool hud_icon(const char *name, float cx, float cy, float size, u32 color);
bool hud_icon_exists(const char *name);

#endif
