// Window/video settings: resolution, window mode, FPS limit. Backed by cvars.
#ifndef DG_VIDEO_H
#define DG_VIDEO_H

#include "core/base.h"

void video_register_cvars(void);
void video_apply(void);            // applies r_width/r_height/r_mode to the window
int video_max_fps(void);
const char *video_config_path(void);

#endif
