// Weapon attachments: optics (red dot, 4x, 8x), muzzle devices, underbarrel grips, magazines,
// stocks. Definitions live in assets/attachments/*.json, weapon slots in data/weapons/*.json
// ("attachment_slots"); see docs/ATTACHMENTS.md.
//
// Each weapon instance (InvSlot.att) carries one attachment id per slot. Everything derived from
// them is a pure function of (weapon def, ids): the host applies the stat modifiers in the weapon
// tick, the predicting client computes the same values from its replicated inventory, and remote
// machines only need the ids (Entity attach[], replicated per slot) to draw the parts.
//
// Placement: attachment space -> item space = slot socket x T(0, 0, rail_offset) x inverse(mount).
// The optic's sight point becomes the item's ADS sight socket, so the exact ADS transform
//   item (camera space) = T(0, 0, -eye_relief) x inverse(sight)
// holds for every optic by construction (holdpoint_ads_item).
#ifndef DG_ATTACHMENTS_H
#define DG_ATTACHMENTS_H

#include "items.h"

struct cJSON;

// Loads assets/attachments (sorted by file name: ids agree on every machine).
bool attachments_load(ItemDB *db, const char *dir);
// Hold style parameters (iron sight eye relief per style): set once the animation library is loaded.
void attach_set_hold_params(const ProcParams *p);
// Weapon JSON "attachment_slots" block (items.c calls it while parsing a weapon).
void attachments_parse_weapon_slots(WeaponDef *d, const struct cJSON *w);
// After weapons and attachments are loaded: default attachment ids per weapon.
void attachments_resolve(ItemDB *db);

const AttachmentDef *attach_def(const ItemDB *db, u8 id);   // NULL for 0 / invalid
u8 attach_find(const ItemDB *db, const char *id);
int attach_slot_from_name(const char *s);                    // -1 if unknown
const char *attach_slot_name(int slot);
bool attach_allowed(const WeaponDef *wd, const AttachmentDef *a);
// Attachment space -> item space for attachment a on weapon wd (identity if the slot is missing).
Xform attach_placement(const WeaponDef *wd, const AttachmentDef *a);
// Copy of base with the attachments applied: stats, muzzle, sight socket + eye relief, left grip,
// hidden parts. att may be NULL (= none). out->att_applied records the ids.
void attach_weapon_effective(const ItemDB *db, const WeaponDef *base, const u8 *att, WeaponDef *out);
// The mounted optic (NULL = iron sights).
const AttachmentDef *attach_optic(const ItemDB *db, const u8 *att);
// Default ids for a new instance of weapon wd (all 0 for non-weapons).
void attach_defaults(const WeaponDef *wd, u8 out[ATT_SLOTS]);

// ADS never changes the world camera FOV (docs/ATTACHMENTS.md "FOV and sensitivity"):
//   sight zoom:  magnification the sight shows, only inside a scope's lens (irons / red dots 1)
//   sight FOV:   the base FOV at that zoom (sensitivity scaling)
//   ADS vm zoom: how much the arms / weapon projection narrows once aimed (tan ratio, >= 1)
float attach_sight_zoom(const AttachmentDef *optic);
float attach_sight_fov(const AttachmentDef *optic, float base_fov);
float attach_ads_vm_zoom(const WeaponDef *eff, const AttachmentDef *optic);
// The mild ADS zoom of the world camera (tan ratio, 1..1.5): the optic's "world_zoom", the weapon's
// "ads_world_zoom" (iron sights), else def (cl_ads_world_zoom).
float attach_ads_world_zoom(const WeaponDef *eff, const AttachmentDef *optic, float def);

// Visual: every mounted attachment's parts (item space boxes), for drawing.
typedef struct AttachDrawPart { vec3 center; quat rot; vec3 half; vec4 color; u8 slot; } AttachDrawPart;
int attach_draw_parts(const ItemDB *db, const WeaponDef *base, const u8 *att, AttachDrawPart *out, int max);

#endif
