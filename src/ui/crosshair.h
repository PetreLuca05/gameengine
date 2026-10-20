// Dynamic crosshair. The gap is the exact screen projection of the bullet cone:
//     gap_px = tan(spread / 2) * (screen_h / 2) / tan(fov_v / 2)
// so every bullet lands inside the drawn lines (cone = full angle, see gameplay/recoil.h).
// It sits on where the next bullet goes: the shots leave the first-person camera along the view
// (docs/AIM.md), so that is the screen centre, except while the fire animation's camera kick turns the
// view (the crosshair stays on the true aim point). Style from assets/ui/crosshair.json + crosshair_* cvars.
#ifndef DG_CROSSHAIR_H
#define DG_CROSSHAIR_H

#include "core/base.h"

void crosshair_init(void);          // cvars, style file, crosshair_reload command
bool crosshair_reload(void);
void crosshair_draw(float dt);      // HUD pass (after the world camera is set)
float crosshair_hitmarker_duration(bool kill);
int crosshair_color_count(void);
const char *crosshair_color_name(int i);

#endif
