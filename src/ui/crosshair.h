// Dynamic crosshair. The gap is the exact screen projection of the bullet cone:
//     gap_px = tan(spread / 2) * (screen_h / 2) / tan(fov_v / 2)
// so every bullet lands inside the drawn lines (cone = full angle, see gameplay/recoil.h).
// In first person it is exactly the screen centre: the gun's hold points the barrel at what the
// centre is on and the camera never turns off the aim line (docs/AIM.md). It moves only to where the
// barrel line ends when that line is obstructed (or with the deliberate reticle movers wpn_ads_sway /
// wpn_ads_bob / cam_kick_rot). Style from assets/ui/crosshair.json + crosshair_* cvars.
#ifndef DG_CROSSHAIR_H
#define DG_CROSSHAIR_H

#include "core/base.h"

void crosshair_init(void);          // cvars, style file, crosshair_reload command
bool crosshair_reload(void);
void crosshair_draw(float dt);      // HUD pass (after the world camera is set)
float crosshair_hitmarker_duration(void);
int crosshair_color_count(void);
const char *crosshair_color_name(int i);

#endif
