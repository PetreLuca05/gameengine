// Arms layer tuner (console: arms_tuner; docs/CAMERA_HOLDPOINT.md "The arms layer").
#ifndef DG_ARMS_TUNER_H
#define DG_ARMS_TUNER_H

#include "core/base.h"

void arms_tuner_init(void);
void arms_tuner_draw(void);
bool arms_tuner_open(void);

#endif
