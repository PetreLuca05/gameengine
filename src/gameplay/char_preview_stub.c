// TEMPORARY (branch inv-simple): stands in for branch char-preview until it is merged.
#include "characters.h"

void chars_set_worn(World *w, int player_slot, const u16 worn[CWORN_COUNT]) { (void)w; (void)player_slot; (void)worn; }
TextureId chars_preview(World *w, int player_slot, const CharPreviewIn *in) { (void)w; (void)player_slot; (void)in; return 0; }
