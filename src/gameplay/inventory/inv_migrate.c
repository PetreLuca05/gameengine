#include "inv_migrate.h"
#include "inv_intent.h"
#include <string.h>

// The items.json order format-1 saves index (item 0 is "none"); after it, the generated attachment
// items in the same order as today's.
static const char *k_v1_items[] = {
    "ammo_9mm", "ammo_rifle", "ammo_shell", "ammo_bolt", "ammo_762", "medkit", "bandage", "armor", "painkillers", "frag_grenade",
    "flare", "molotov", "canned_food", "water_bottle", "pistol", "rifle", "smg", "lmg", "dmr", "sniper", "shotgun", "crossbow",
    "bat", "knife", "key_red", "key_blue", "scrap", "battery", "cloth", "duct_tape", "mech_parts", "seeds", "syringe",
    "dna_sample", "fishing_kit", "phone", "fuel_can", "tire_repair", "spare_tire", "energy_bar", "backpack", "backpack_hiking",
    "sling_bag", "rig_chest", "plate_carrier", "helmet", "headset", "balaclava", "glasses", "armband", "secure_alpha",
    "ammo_case", "med_case", "flashlight"
};
#define V1_ITEMS ((int)ARRAY_COUNT(k_v1_items))
// Format 1's equipment slots (loc - INV_LOC_EQUIP) -> today's; EQ_NONE: into the grid.
static const u8 k_v1_slot[13] = { EQ_PRIMARY, EQ_SECONDARY, EQ_HOLSTER, EQ_NONE, EQ_HEADWEAR, EQ_NONE, EQ_NONE, EQ_NONE,
                                  EQ_BODY_ARMOR, EQ_NONE, EQ_BACKPACK, EQ_NONE, EQ_NONE };

u16 inv_v1_item(const ItemDB *db, int idx) {
    if (idx <= 0) return 0;
    if (idx <= V1_ITEMS) return item_find(db, k_v1_items[idx - 1]);
    int first = 0;   // today's first generated attachment item
    for (int i = 1; i < db->item_count && !first; i++)
        if (db->items[i].kind == ITEM_ATTACHMENT && !strncmp(db->items[i].id, "att_", 4)) first = i;
    int k = first ? first + (idx - V1_ITEMS - 1) : 0;
    return k > 0 && k < db->item_count ? (u16)k : 0;
}

static InvItem *add(InvTree *t, const ItemDB *db, const InvV1Entry *e, u16 item) {
    InvSlot s;
    memset(&s, 0, sizeof(s));
    s.item = item;
    s.count = (u16)DG_CLAMP(e->count, 1, 65535);
    s.ammo = (i16)e->ammo;
    memcpy(s.att, e->att, ATT_SLOTS);
    InvItem *it = inv_alloc(t, db, &s);
    if (it) it->dur = (u16)e->dur;
    return it;
}

// Into the grid: its own cell (when given), else the first free one.
static bool place(InvTree *t, const ItemDB *db, InvItem *it, int x, int y, int rot) {
    u16 g = inv_grid(t, db);
    if (g == INV_NO_GRID) return false;
    it->loc = INV_LOC_NONE;
    if (x >= 0) {
        InvPlace p = { g, 0, (u8)x, (u8)y, (u8)rot };
        if (inv_check_place(t, db, it, p, 0, NULL) == IIR_OK) {
            it->parent = g; it->loc = 0; it->x = (u8)x; it->y = (u8)y;
            it->flags = (u8)((it->flags & ~INVF_ROT) | (rot ? INVF_ROT : 0));
            return true;
        }
    }
    it->flags &= (u8)~INVF_ROT;
    InvPlace p;
    if (!inv_find_space(t, db, it, g, &p)) return false;
    it->parent = p.parent; it->loc = p.loc; it->x = p.x; it->y = p.y;
    it->flags = (u8)((it->flags & ~INVF_ROT) | (p.rot ? INVF_ROT : 0));
    return true;
}

int inv_migrate_v1(InvTree *t, const ItemDB *db, int root, const InvV1Entry *e, int n) {
    inv_tree_init(t, root);
    bool body = inv_root_def(t, db)->equip;
    int dropped = 0;
    bool done[INV_MAX_ITEMS] = { false };
    n = DG_MIN(n, INV_MAX_ITEMS);
    int worn_pack = -1;   // the old uid of the worn backpack (its contents keep their cells)
    if (body) {
        // What was worn: the weapon slots, helmet, plate carrier, the backpack first (it is the grid).
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n; i++) {
                int os = e[i].loc - INV_LOC_EQUIP;
                if (done[i] || e[i].parent || os < 0 || os >= 13) continue;
                int slot = k_v1_slot[os];
                if ((pass == 0) != (slot == EQ_BACKPACK) || slot == EQ_NONE) continue;
                u16 item = inv_v1_item(db, e[i].item);
                if (!item || !inv_equip_accepts(slot, item_def(db, item)) || inv_equipped(t, slot)) continue;
                InvItem *it = add(t, db, &e[i], item);
                if (!it) continue;
                it->loc = (u8)(INV_LOC_EQUIP + slot);
                it->flags = (u8)(e[i].flags & INVF_FOLDED);
                done[i] = true;
                if (slot == EQ_BACKPACK) worn_pack = e[i].uid;
            }
        inv_ensure_pack(t, db);
    }
    // The grid: the worn pack's contents in their cells first, then everything else that still exists.
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            bool own = body ? (worn_pack >= 0 && e[i].parent == worn_pack) : (!e[i].parent && e[i].loc == 0);
            if (pass == 0 && !own) continue;
            done[i] = true;
            u16 item = inv_v1_item(db, e[i].item);
            const ItemDef *d = item_def(db, item);
            if (!item || d->default_pack || (d->builtin && body)) { dropped += item ? 0 : 1; continue; }
            InvItem *it = add(t, db, &e[i], item);
            if (!it) { dropped++; continue; }
            it->flags = (u8)(e[i].flags & (INVF_FOLDED | INVF_ROT));
            // A weapon from a slot that is gone (melee) or a pocket: a free weapon slot first.
            int slot = body ? inv_equip_target(t, db, it) : EQ_NONE;
            if (slot != EQ_NONE && slot != EQ_BACKPACK && !inv_equipped(t, slot)) {
                it->loc = (u8)(INV_LOC_EQUIP + slot);
                it->flags &= (u8)~INVF_ROT;
                continue;
            }
            if (!place(t, db, it, own ? e[i].x : -1, e[i].y, (e[i].flags & INVF_ROT) ? 1 : 0)) {
                memset(it, 0, sizeof(*it));
                dropped++;
            }
        }
    inv_normalize(t, db);
    return dropped;
}
