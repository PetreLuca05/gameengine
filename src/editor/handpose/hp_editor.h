// Hand pose editor (scene handpose, docs/HANDPOSE.md): every hand pose entry in a studio level,
// the local player's character posed by the gameplay pipeline holding / using it, an orbit or
// first-person camera framed on the hands and the item, gizmos, finger UI, contact solver, undo,
// save. Left: entries; right: properties; bottom: states, hands, playback, actions.
#ifndef DG_HP_EDITOR_H
#define DG_HP_EDITOR_H

#include "core/base.h"
#include "render/render.h"

void hp_editor_init(void);                 // commands: scene, handpose_editor
bool hp_editor_open(void);
void hp_editor_draw(void);                 // every frame (menus_draw): preview, overlays, panels
// Camera override (game_render.c): true when the editor's camera (orbit / framing) is in use.
bool hp_editor_camera(RenderCamera *cam);
// The camera the frame was rendered with (picking / projection).
void hp_editor_rendered_camera(const RenderCamera *cam);

#endif
