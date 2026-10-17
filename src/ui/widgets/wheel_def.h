// Radial wheel definitions (assets/ui/wheels/*.json, docs/UI_WHEELS_HUD.md "Wheel JSON"): which
// slots a wheel shows. "static" wheels list their slots (pings, door actions); "inventory" wheels
// fill from the carried items matching a filter (healing, throwables), one slot per item type.
// Pure (tests/test_hud.c).
#ifndef DG_WHEEL_DEF_H
#define DG_WHEEL_DEF_H

#include "core/base.h"
#include "radial_math.h"

#define WHEEL_MAX_DEFS 16

typedef enum WheelSource { WHEEL_STATIC, WHEEL_INVENTORY } WheelSource;

typedef struct WheelSlotDef {
    char id[32];           // action id (static) or item id (inventory "order")
    char label[32];
    char icon[24];         // vector icon name (hud_icons.c), empty = the item icon / initials
    char desc[96];
    char sub[24];          // sub-wheel id: selecting the slot opens that wheel instead
    int color;             // theme ping colour index (0..7), -1 = default
} WheelSlotDef;

typedef struct WheelDef {
    char id[24];
    char title[32];
    int source;            // WheelSource
    char filter_kind[16];  // inventory: item kind ("consumable", "throwable")
    char filter_tag[16];   // inventory: "heal" (heal / armor > 0, not vehicle items) or "" = any
    int min_slots, max_slots;   // drawn slot count = clamp(items, min, max): empties fill to min
    int default_slot;      // static: the slot selected on open (door: "open")
    float hold_time;       // seconds the key is held before the wheel opens (< 0 = the cvar)
    WheelSlotDef slots[RADIAL_MAX_SLOTS];   // static slots, or the inventory display order (by item id)
    int slot_count;
} WheelDef;

typedef struct WheelLib {
    WheelDef defs[WHEEL_MAX_DEFS];
    int count;
} WheelLib;

// Parses one wheel file. Returns false (and err) on a bad file; def keeps defaults then.
bool wheel_def_parse(WheelDef *def, const char *json, char *err, int err_size);
const WheelDef *wheel_lib_find(const WheelLib *lib, const char *id);
// Adds or replaces (by id).
bool wheel_lib_add(WheelLib *lib, const WheelDef *def);
// Position of an item id in an inventory wheel's "order" list (-1: not listed, goes after).
int wheel_def_order(const WheelDef *def, const char *item_id);
int wheel_slot_index(const WheelDef *def, const char *id);

#endif
