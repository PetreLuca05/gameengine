// characters.c internals shared with the inventory character preview (char_preview.c).
#ifndef DG_CHARACTERS_INTERNAL_H
#define DG_CHARACTERS_INTERNAL_H

#include "characters.h"

// A player's starting parts on a CharacterInst outside w->chars (def, is_player and slot set): the
// default player skin, the other slots empty.
void chars_preview_pick_parts(CharacterInst *c);
// Every part of c (the selection drawn for c->parts, the own player's cl_skin / cl_parts included) at
// LOD0 from the pose in c->si, into the character preview view only (SkinnedDraw.view).
void chars_preview_draw(CharacterInst *c);
// c->worn on the pose in c->si (render_draw: the caller captures them for the preview).
void chars_preview_draw_worn(const World *w, const CharacterInst *c);
// char_worn: the fit of an item in a worn slot on c's body mesh (NULL: the Skeleton) as text (bone,
// offset, rotation, scale).
bool chars_worn_fit(const World *w, const CharacterInst *c, u16 item, int slot, char *out, size_t n);
void char_preview_shutdown(void);   // char_preview.c: frees the preview instance
void char_preview_set_frozen(bool on);   // tests: the preview's pose holds still (same image every frame)

#endif
