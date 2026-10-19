// Item icons (docs/UI.md "Icons and preview"): one atlas of square tiles (128 px, 256 px on
// high-DPI). Each tile comes from, in order: the item's "icon" override (items.json), the baked
// PNG assets/ui/icons/<size>/<id>.png (tools/bake_icons), or a live bake through the UI item
// view renderer (one per frame) when neither exists.
#ifndef DG_ICONS_H
#define DG_ICONS_H

#include "core/base.h"
#include "core/dgmath.h"
#include "render/render.h"

void icons_init(void);
void icons_reload(void);
void icons_rerender(void);   // re-render every icon live from the current item models (item_reload)
void icons_tick(void);                                     // lazy setup + progressive live bakes
bool icons_get(u16 item, TextureId *tex, vec4 *uv);        // false: not ready (draw the fallback)
int icons_missing(void);                                   // items whose icon is still a live bake / missing

#endif
