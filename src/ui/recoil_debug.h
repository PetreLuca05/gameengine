// wpn_recoil_debug overlay: live recoil state panel, pattern preview, spread cone drawn in the
// world at the aim distance, and the per-bullet cone check counters (see wpn_magdump).
#ifndef DG_RECOIL_DEBUG_H
#define DG_RECOIL_DEBUG_H

#include "core/dgmath.h"

void recoil_debug_init(void);
void recoil_debug_draw(vec3 cam_pos);   // call after the camera is set, before dbg_submit

#endif
