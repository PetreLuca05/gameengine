// HUD modules (docs/UI_WHEELS_HUD.md): kill counter + kill feed (hud_kills.c), compass with ping
// markers and coordinates (hud_compass.c), ping markers in the world (hud_pings.c). The settings
// are archived cvars (the Settings > HUD tab); styles follow src/ui/hud.c and the theme.
#ifndef DG_HUD_INTERNAL_H
#define DG_HUD_INTERNAL_H

#include "game/game.h"
#include "ui/gui/gui_widgets.h"

#define TH (gui_theme())

typedef struct HudCvars {
    Cvar *compass, *compass_arc, *coords, *killcount, *killcount_split, *killfeed, *ping_markers, *ping_compass, *opacity, *scale;
} HudCvars;

typedef struct HudCompassCfg {
    float fov_deg, tick_every, label_every, edge_fade, coords_hz, marker_max_dist;
    bool coords, markers_pings, markers_team;
} HudCompassCfg;

void hud_register(void);
const HudCvars *hud_cvars(void);
const HudCompassCfg *hud_compass_cfg(void);
void hud_compass_load(void);
// The last coordinates line (10 Hz), "" without a player (the pause menu copies it).
const char *hud_coords_text(void);
bool hud_copy_coords(void);   // to the clipboard; false without a position

float hud_kills_draw(Game *g, GuiRect safe);           // returns the bottom (killfeed)
float hud_compass_draw(Game *g, GuiRect safe);         // returns the bottom (coordinates)
void hud_pings_draw(Game *g, GuiRect safe);
// Caps label in the HUD style (hud.c).
void hud_caps(float x, float y, float size, u32 c, int align, const char *s);
// Item / vector icon in a box, one colour (the silhouette of the item's baked icon, else the named vector icon).
void hud_item_icon(u16 item, const char *fallback, float cx, float cy, float size, u32 color);

#endif
