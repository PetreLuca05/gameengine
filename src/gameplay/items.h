// Data-driven item and weapon definitions (data/items.json, data/weapons/*.json).
#ifndef DG_ITEMS_H
#define DG_ITEMS_H

#include "core/base.h"
#include "core/dgmath.h"
#include "gameplay/recoil.h"
#include "anim/procedural.h"
#include "anim/body_parts.h"

#define MAX_ITEM_DEFS 128
#define MAX_WEAPON_DEFS 32
#define ITEM_NONE 0

typedef enum ItemKind { ITEM_MISC, ITEM_AMMO, ITEM_CONSUMABLE, ITEM_WEAPON, ITEM_KEY } ItemKind;
typedef enum DamageType { DMG_GENERIC, DMG_BULLET, DMG_MELEE, DMG_EXPLOSIVE, DMG_FALL, DMG_COUNT } DamageType;
typedef enum WeaponKind { WPN_MELEE, WPN_HITSCAN, WPN_PROJECTILE } WeaponKind;
typedef enum VisShape { VIS_NONE, VIS_BOX, VIS_CYLINDER, VIS_SPHERE, VIS_CAPSULE } VisShape;

typedef struct ItemDef {
    char id[32];
    char name[48];
    char desc[128];
    ItemKind kind;
    int max_stack;
    vec4 color;
    VisShape shape;        // pickup visual
    vec3 size;             // half extents
    float heal, armor, use_time;
    int weapon;            // index into weapon defs (ITEM_WEAPON), -1 otherwise
    char hold_style[16];   // third-person hold style for non-weapon items ("item_small", "item_large")
    ItemSockets sockets;   // held-item sockets (non-weapons)
} ItemDef;

typedef struct WeaponModelPart { vec3 offset; vec3 half; vec4 color; } WeaponModelPart;

typedef struct WeaponDef {
    char id[32];
    WeaponKind kind;
    DamageType damage_type;
    float damage;
    float fire_rate;          // shots (or swings) per second (guns: rpm / 60)
    bool automatic;           // melee: hold to keep swinging
    int mag_size;
    u16 ammo_item;
    float reload_time;
    int pellets;
    float range, impulse, headshot_mult;
    float projectile_speed, projectile_gravity;
    float swing_time, active_start, active_end, reach, sweep_radius, knockback, swing_arc;
    float ads_fov;            // FOV while aiming (0 = no ADS)
    char pose[32];            // hand pose clip
    char sound_fire[32], sound_reload[32];
    WeaponModelPart parts[6];
    int part_count;
    vec3 grip_offset;         // where the right hand holds it (model space)
    vec3 support_offset;      // where the left hand supports it (model space)
    vec3 muzzle_offset;
    bool bolt_action;         // cycles the bolt after each shot (shot-event hook)
    RecoilParams recoil;      // guns: recoil, spread, fire modes, ADS/sway feel ("recoil" block)
    // Third-person procedural hold ("hold" block): style + item sockets (shared with the viewmodel grip data).
    char hold_style[16];
    ItemSockets sockets;
    // Per-body-part damage multipliers ("part_mult"), 1.0 = unlisted; range falloff.
    float part_mult[BP_COUNT];
    float falloff_start, falloff_end, falloff_min;
    char file[64];            // source file (data/weapons/*.json)
} WeaponDef;

typedef struct ItemDB {
    ItemDef items[MAX_ITEM_DEFS];
    int item_count;    // items[0] is the "none" item
    WeaponDef weapons[MAX_WEAPON_DEFS];
    int weapon_count;
} ItemDB;

// weapons_path: a directory of per-weapon JSON files (loaded sorted by file name so weapon
// indices are identical on every machine) or a single JSON file with a "weapons" array.
bool items_load(ItemDB *db, const char *items_path, const char *weapons_path);
// Re-reads only the weapon definitions in place (live tuning). Item ids must not change.
bool weapons_reload_defs(ItemDB *db, const char *weapons_path);
u16 item_find(const ItemDB *db, const char *id);
const ItemDef *item_def(const ItemDB *db, u16 item);
const WeaponDef *item_weapon(const ItemDB *db, u16 item); // NULL if not a weapon
const char *damage_type_name(DamageType t);

#endif
