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

typedef enum ItemKind { ITEM_MISC, ITEM_AMMO, ITEM_CONSUMABLE, ITEM_WEAPON, ITEM_KEY, ITEM_ATTACHMENT, ITEM_KIND_COUNT } ItemKind;
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
    u8 attachment;         // ITEM_ATTACHMENT: the attachment id it mounts (gameplay/inv_ops.c)
    char icon[64];         // UI icon override (assets path, "icon" in the item JSON); empty = baked icon
    u8 vehicle_use;        // VehicleUse: consumables used on a vehicle (docs/VEHICLES.md)
    float fuel;            // VUSE_FUEL: litres
    // Camera items (the phone, docs/PHONE.md): aiming raises it into camera mode. Item space as
    // for weapons (origin = grip_r, -Z = the lens direction, +Z = the screen side, +Y = top).
    bool camera;
    bool builtin;              // always carried, outside the inventory (taken out with its own key)
    char hold_style_alt[16];   // "hold": { "style_landscape" }: two-handed landscape hold style
    struct {
        vec3 body_center, body_half;   // the phone body (box)
        Xform screen;                  // screen centre, facing +Z of this frame
        vec2 screen_half;              // half width / height (m)
        Xform lens;                    // back camera: looks down -Z of this frame
        float fov_deg;                 // 1x vertical FOV of the lens
    } cam;
} ItemDef;

typedef enum VehicleUse { VUSE_NONE, VUSE_FUEL, VUSE_TIRE_REPAIR, VUSE_SPARE_TIRE } VehicleUse;

typedef struct WeaponModelPart { vec3 offset; vec3 half; vec4 color; } WeaponModelPart;

// ------------------------------------------------------------------ attachments
// assets/attachments/*.json (docs/ATTACHMENTS.md). Ids are 1-based u8 indices (0 = none), loaded
// sorted by file name so they agree on every machine (replicated per slot).
#define ATT_SLOTS 5
#define MAX_ATTACHMENTS 32
#define ATT_MAX_PARTS 10
typedef enum AttachSlot { ATT_OPTIC, ATT_MUZZLE, ATT_UNDERBARREL, ATT_MAGAZINE, ATT_STOCK } AttachSlot;
typedef enum SightKind { SIGHT_IRON, SIGHT_REDDOT, SIGHT_SCOPE } SightKind;
typedef enum ReticleShape { RET_DOT, RET_CIRCLE_DOT, RET_DUPLEX, RET_MILDOT } ReticleShape;

typedef struct Reticle {
    int shape;                // ReticleShape
    float moa;                // dot diameter (MOA); scopes: centre dot / line thickness
    float outer_moa;          // circle-dot ring / duplex post gap (MOA)
    vec3 color;               // linear
    float brightness;         // emissive multiplier (x sight_brightness cvar)
    bool ffp;                 // scopes: first focal plane (reticle scales with zoom)
    bool illuminated;         // scopes: emissive centre
} Reticle;

typedef struct AttachmentDef {
    char id[32], name[48];
    char file[64];
    int slot;                 // AttachSlot
    int sight;                // SightKind (optics)
    Xform mount;              // attachment space: the frame placed on the weapon's slot socket
    vec3 sight_pt;            // attachment space: ADS eye-side point on the optic axis
    float eye_relief;         // m from the eye to sight_pt; <= 0 = keep the weapon's stock weld
    float zoom;               // magnification (1 = none)
    float ads_fov;            // explicit sight FOV (deg at cl_fov 75); 0 = 2 atan(tan(base / 2) / zoom)
    float main_zoom;          // scopes (picture-in-picture): zoom of the main view around the scope
    float rail_offset;        // m along the rail, + = towards the stock (clamped to the slot's rail)
    bool rail_auto;           // scopes: slide on the rail so the stock weld gives exactly eye_relief
    Reticle reticle;
    float parallax_m;         // red dot: parallax-free distance (the dot converges on the bore line there)
    // Lens / window (attachment space, axis = -Z): red dot window or scope ocular, scope objective.
    vec3 lens_pos;
    float lens_radius;
    vec3 objective_pos;
    float objective_radius;
    float eye_box;            // scopes: lateral eye position tolerance (m) before the scope shadow closes in
    float eye_box_depth;      // scopes: eye relief tolerance (m)
    vec3 glass_tint;
    float dust, fresnel;
    float chromatic;          // scopes: edge chromatic aberration (pixels at the rim)
    float dof_near_blur;      // ADS: near-field (weapon) blur, CoC pixels
    // Modifiers (applied by the host and by the predicting client from the same inventory).
    float ads_time;           // multiplier
    float sway_scale;         // multiplier on idle / scope sway
    float recoil_v, recoil_h, cam_kick, vm_kick;   // multipliers
    float spread_hip, spread_ads;
    float damage, range;
    float mag_mult;           // magazine capacity multiplier
    float reload_time;        // multiplier
    float muzzle_len;         // m the muzzle moves forward (muzzle devices)
    int muzzle_device;        // VfxMuzzleDevice, -1 = keep
    bool has_grip_l;
    Xform grip_l;             // underbarrel grips: left hand socket (attachment space)
    WeaponModelPart parts[ATT_MAX_PARTS];
    int part_count;
    bool emissive_parts;      // unused (lens glow is drawn by the sight pass)
} AttachmentDef;

// A weapon's attachment slot ("attachment_slots" block): socket in item space, rail range,
// allowed attachment ids (empty = any of that slot type), weapon parts hidden while filled.
typedef struct WeaponAttachSlot {
    bool present;
    Xform socket;
    float rail_min, rail_max;
    char allow[8][32];
    int allow_count;
    u8 hide_parts;            // bit mask over WeaponDef.parts
    char def_id[32];          // default attachment (new weapons spawn with it)
} WeaponAttachSlot;

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
    bool one_hand_capable;    // can be fired one-handed while driving (pistols, SMGs: docs/VEHICLES.md)
    // VFX ("vfx" block): muzzle flash type x device, impact caliber (src/vfx/vfx_library.h enums).
    int vfx_muzzle_type, vfx_muzzle_device, vfx_caliber;
    RecoilParams recoil;      // guns: recoil, spread, fire modes, ADS/sway feel ("recoil" block)
    // Third-person procedural hold ("hold" block): style + item sockets (shared with the viewmodel grip data).
    char hold_style[16];
    ItemSockets sockets;
    // Per-body-part damage multipliers ("part_mult"), 1.0 = unlisted; range falloff.
    float part_mult[BP_COUNT];
    float falloff_start, falloff_end, falloff_min;
    // Attachments: slots + the iron sight (the default sight, sockets.sight / its eye relief).
    WeaponAttachSlot att_slots[ATT_SLOTS];
    vec3 iron_sight;          // the iron sight socket (sockets.sight before any optic)
    u8 att_default[ATT_SLOTS];  // default attachment per slot (ids, resolved after loading)
    u8 att_applied[ATT_SLOTS];  // effective copies (attach_weapon_effective): what was applied
    char file[64];            // source file (data/weapons/*.json)
} WeaponDef;

typedef struct ItemDB {
    ItemDef items[MAX_ITEM_DEFS];
    int item_count;    // items[0] is the "none" item
    WeaponDef weapons[MAX_WEAPON_DEFS];
    int weapon_count;
    AttachmentDef attachments[MAX_ATTACHMENTS + 1];   // [0] unused (id 0 = none)
    int attachment_count;                             // highest valid id
    u16 builtin_phone;     // the camera item every player always carries ("builtin": true), 0 = none
} ItemDB;

// weapons_path: a directory of per-weapon JSON files (loaded sorted by file name so weapon
// indices are identical on every machine) or a single JSON file with a "weapons" array.
bool items_load(ItemDB *db, const char *items_path, const char *weapons_path);
// Re-reads only the weapon definitions in place (live tuning). Item ids must not change.
bool weapons_reload_defs(ItemDB *db, const char *weapons_path);
u16 item_find(const ItemDB *db, const char *id);
// The model drawn in the hands (and on the ground): non-weapons are a palm-sized box (pickup sizes
// are world-scale, held items are capped at 7 cm half extent) offset from the grip; weapons are
// their part boxes. Bounds are in the item's own space (-Z forward, +Y up, origin = the grip).
void item_held_box(const ItemDef *it, vec3 *center, vec3 *half);
bool item_model_bounds(const ItemDB *db, u16 item, vec3 *mn, vec3 *mx);
const ItemDef *item_def(const ItemDB *db, u16 item);
const WeaponDef *item_weapon(const ItemDB *db, u16 item); // NULL if not a weapon
const char *damage_type_name(DamageType t);
const char *item_kind_name(ItemKind k);
// Attachment items (ITEM_ATTACHMENT): one per attachment definition ("att_<id>"), created after
// loading unless items.json defines one ("kind": "attachment", "attachment": "<id>").
u16 item_for_attachment(const ItemDB *db, u8 att_id);
// Palm frame from finger / back-of-hand directions (-Z = fingers, +Y = back), orthonormalized.
quat item_palm_frame(vec3 fingers, vec3 back);

#endif
