// In-game HUD: crosshair, health/armor, ammo, hotbar, interaction prompt, hit markers,
// damage indicators, hints, kill feed, dummy health bars, FPS + net stats overlays.
#ifndef DG_HUD_H
#define DG_HUD_H

void hud_draw(void);
void hud_register(void);            // HUD cvars (hud/hud_compass.c)
// Bottom of the compass block last frame (pixels): the day / night clock sits under it.
float hud_compass_bottom(void);

#endif
