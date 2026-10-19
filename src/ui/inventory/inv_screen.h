// Inventory screen internals (docs/INVENTORY.md "Screen"): cell images, the editor panel, shared
// helpers. The screen's public entry points stay in ui/screens/screens.h (inventory_draw, ...).
#ifndef DG_INV_SCREEN_H
#define DG_INV_SCREEN_H

#include "inv_ui.h"
#include "render/render.h"
#include "ui/gui/gui.h"
#include "ui/gui/gui_draw.h"
#include "ui/gui/gui_widgets.h"

// Cell images (inv_cells.c): the item at its grid aspect (unrotated); false: not ready.
void inv_cells_init(void);
void inv_cells_tick(void);
bool inv_cells_get(u16 item, TextureId *tex, vec4 *uv);

// Editor panel (inv_editor.c): item set, fill, intents, fit map. Drawn over the screen.
void inv_editor_init(void);
bool inv_editor_open(void);
void inv_editor_draw(void);
u16 inv_editor_fit_item(void);   // the item whose fits the grids show (0 none)

// Screen helpers shared with the editor (inv_screen.c).
InvTree *inv_screen_tree(int side);           // the local player's tree / the open loot tree
int inv_screen_send(InvIntent in);            // predicted result; toast + sound on a rejection
void inv_screen_toast(const char *msg);
// The item image at its grid aspect into r (rotated: turned a quarter), icon / initials fallback.
void inv_draw_item_image(GuiRect r, const ItemDB *db, const InvSlot *s, bool rotated, float alpha);

#endif
