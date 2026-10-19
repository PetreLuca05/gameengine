// Weapon muzzle socket placement in the hand pose editor (docs/HANDPOSE.md "Muzzle"). The pure
// part: reading / writing `hold.sockets.muzzle` in a weapon file's text without touching anything
// else in it (the data files are hand formatted), the number format, and the bore helpers (the
// barrel's centre line / front face from the weapon's model parts). No game state: the editor
// scene (hp_editor.c) and tests.
//
// The muzzle is in item space (origin = right grip, -Z = barrel forward, +Y up): shots leave it
// along -Z (docs/AIM.md). A mounted muzzle device replaces it with its own front face.
#ifndef DG_HP_MUZZLE_H
#define DG_HP_MUZZLE_H

#include "gameplay/items.h"

#define HP_MUZZLE_RES 0.0001f   // m: what the files keep (0.1 mm)

// v rounded to HP_MUZZLE_RES (what a save writes; no "-0").
vec3 hp_muzzle_round(vec3 v);
// "[0, 0.06, -0.6205]": 0.1 mm, trailing zeros trimmed.
void hp_muzzle_format(vec3 v, char *out, int size);
// hold.sockets.muzzle as written in a weapon file (false: not set / malformed).
bool hp_muzzle_json_read(const char *text, vec3 *out);
// The file's text with hold.sockets.muzzle set to v: the value replaced in place, or added as the
// last member of "sockets" (a "sockets" object added to "hold" when it has none), indented like its
// siblings, the file's line ending kept; every other byte unchanged. NULL when the text has no
// "hold" object (or is malformed). Caller frees.
char *hp_muzzle_json_patch(const char *text, vec3 v);

// The barrel: the forward-most (-Z) model part, the iron sight parts left out (-1: none).
int hp_muzzle_barrel_part(const WeaponDef *wd);
enum { HPM_BORE, HPM_TIP };
// Bore helpers (item space): HPM_BORE puts m on the barrel part's centre line (x, y; z kept),
// HPM_TIP on the centre of its front face. m unchanged when the weapon has no model parts.
vec3 hp_muzzle_snap(const WeaponDef *wd, vec3 m, int how);

#endif
