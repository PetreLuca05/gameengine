// In-game VFX editor (console: vfx_editor, key F7): live emitter tweaking + save to JSON.
#ifndef DG_VFX_EDITOR_H
#define DG_VFX_EDITOR_H

#include "core/base.h"

void vfx_editor_init(void);
void vfx_editor_draw(void);   // inside the UI frame (after menus)
bool vfx_editor_open(void);
void vfx_editor_toggle(void);

#endif
