// Cosmetics (docs/MENU_LOBBY.md "Adding a cosmetic"): assets/cosmetics/*.json lists items per
// category (the modular character's part slots + weapon skins), their rarity and how they unlock.
// Equipping one picks a part option of the player character (assets/characters/player.json
// "parts") or a skin for the body slot; the selection reaches everyone as the replicated part
// selection (RPC_SET_PART). Pure apart from the directory listing (tests/test_lobby.c).
#ifndef DG_COSMETICS_H
#define DG_COSMETICS_H

#include "core/base.h"
#include "assets/character.h"

#define COS_MAX_ITEMS 128
#define COS_SLOT_WEAPON CSLOT_COUNT   // weapon skins (stub: no weapon skin rendering yet)
#define COS_SLOTS (CSLOT_COUNT + 1)

typedef enum CosRarity { CR_COMMON, CR_UNCOMMON, CR_RARE, CR_EPIC, CR_LEGENDARY, CR_COUNT } CosRarity;
typedef enum CosUnlock { CU_DEFAULT, CU_LOCKED, CU_STEAM } CosUnlock;

typedef struct CosmeticItem {
    char id[32];
    char name[48];
    int slot;                 // CharSlot, or COS_SLOT_WEAPON
    char option[32];          // the character part option id (body: a skin id)
    int rarity;               // CosRarity
    int unlock;               // CosUnlock
    u32 steam_item;           // CU_STEAM: Steam Inventory item definition (hook stub)
    char icon[96];            // optional png ("" = tinted placeholder)
    u32 tint;                 // RGBA8: placeholder icon colour
    char description[128];
} CosmeticItem;

typedef struct CosmeticDB {
    CosmeticItem items[COS_MAX_ITEMS];
    int count;
} CosmeticDB;

// What the player wears: one item id per cosmetic slot ("" = nothing / the default).
typedef struct CosmeticLoadout {
    char equipped[COS_SLOTS][32];
} CosmeticLoadout;

// Items the profile unlocked beyond the defaults (drops, Steam Inventory).
#define COS_MAX_UNLOCKS 128
typedef struct CosmeticUnlocks {
    char ids[COS_MAX_UNLOCKS][32];
    int count;
} CosmeticUnlocks;

const char *cosmetics_slot_name(int slot);          // "Head", ..., "Weapon skins"
const char *cosmetics_rarity_name(int rarity);
int cosmetics_rarity_from_name(const char *s);      // -1 if unknown

// Adds the items of one file ({ "items": [ ... ] }). false + err on a malformed item (nothing added).
bool cosmetics_parse(CosmeticDB *db, const char *json, const char *file, char *err, int err_size);
int cosmetics_load(CosmeticDB *db, const char *dir);
const CosmeticItem *cosmetics_find(const CosmeticDB *db, const char *id);
// Owned: default unlocks, the profile's unlocks and (later) Steam Inventory items.
bool cosmetics_owned(const CosmeticItem *it, const CosmeticUnlocks *u);
// Steam Inventory hook (stub: always false until the item definitions exist on Steam).
bool cosmetics_steam_owns(u32 item_def);

// Equip / unequip. false when the item is unknown or not owned.
bool cosmetics_equip(const CosmeticDB *db, CosmeticLoadout *lo, const CosmeticUnlocks *u, const char *id);
void cosmetics_unequip(CosmeticLoadout *lo, int slot);
bool cosmetics_is_equipped(const CosmeticLoadout *lo, const char *id);

// The loadout as the character's part options: out[slot] = option index, -1 = empty / default.
// Returns how many equipped items have no matching option in def (not shown).
int cosmetics_to_parts(const CosmeticDB *db, const CosmeticLoadout *lo, const CharacterDef *def, int *out);
// Reverse: the cosmetics a part selection (CHAR_PART_NONE / k = option k - 1) corresponds to.
void cosmetics_from_parts(const CosmeticDB *db, const CharacterDef *def, const u8 *sel, CosmeticLoadout *out);
// cl_parts value ("head:helmet_olive torso:jacket_tan") for the non-body slots, cl_skin for the body.
void cosmetics_parts_string(const CosmeticDB *db, const CosmeticLoadout *lo, char *out, int size);
const char *cosmetics_body_skin(const CosmeticDB *db, const CosmeticLoadout *lo);   // "" = default
u32 cosmetics_hash(const CosmeticDB *db);

#endif
