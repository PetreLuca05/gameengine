// Dynamic crosshair. The gap is the exact screen projection of the bullet cone:
//     gap_px = tan(spread / 2) * (screen_h / 2) / tan(fov_v / 2)
// so every bullet lands inside the drawn lines (cone = full angle, see gameplay/recoil.h).
// The crosshair sits on the projection of the true aim point: the camera kick spring moves the
// view but not the aim, so the crosshair drifts against the kick (crosshair_follow_kick 1 pins
// it to the screen center instead). Style from assets/ui/crosshair.json + crosshair_* cvars.
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
