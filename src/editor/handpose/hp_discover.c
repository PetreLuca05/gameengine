// Hand pose discovery: sources -> stub entries, orphans, report (docs/HANDPOSE.md).
#include "hp_discover.h"
#include "anim/procedural.h"
#include "gameplay/items.h"
#include "gameplay/attachments.h"
#include "gameplay/vehicle_def.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

void hp_sources_free(HandPoseSourceList *l) { free(l->v); memset(l, 0, sizeof(*l)); }

HandPoseSource *hp_sources_add(HandPoseSourceList *l, int category, const char *name) {
    for (int i = 0; i < l->count; i++)
        if (l->v[i].category == category && !strcmp(l->v[i].name, name)) return &l->v[i];
    if (l->count >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        HandPoseSource *n = (HandPoseSource *)realloc(l->v, sizeof(HandPoseSource) * (size_t)cap);
        if (!n) return NULL;
        l->v = n;
        l->cap = cap;
    }
    HandPoseSource *s = &l->v[l->count++];
    memset(s, 0, sizeof(*s));
    s->category = (u8)category;
    dg_strcpy(s->name, sizeof(s->name), name);
    dg_strcpy(s->label, sizeof(s->label), name);
    s->auto_socket[0] = s->auto_socket[1] = xf_identity();
    l->categories |= 1u << category;
    return s;
}

u32 hp_style_states(const ProcParams *p, const char *style) {
    u32 st = 1u << HPS_DEFAULT;
    int i = p ? proc_hold_style_index(p, style) : -1;
    if (i < 0) return st | (1u << HPS_HIP);
    const HoldStyle *hs = proc_hold_style(p, i);
    for (int s = 0; s < HP_STATE_COUNT; s++) if (hs->hp.state[s].set) st |= 1u << HPS_FROM_HOLD(s);
    return st;
}

static bool style_left(const ProcParams *p, const char *style) {
    int i = p ? proc_hold_style_index(p, style) : -1;
    return i >= 0 && proc_hold_style(p, i)->left_hand;
}

static void source_path(char *out, int n, const char *dir, const char *file) {
    if (strchr(file, '/') || strchr(file, '\\')) snprintf(out, (size_t)n, "%s", file);
    else snprintf(out, (size_t)n, "%s/%s", dir, file);
}

void hp_sources_from_items(HandPoseSourceList *l, const ItemDB *db, const ProcParams *p) {
    l->categories |= (1u << HPC_WEAPON) | (1u << HPC_ITEM) | (1u << HPC_ATTACHMENT);
    for (int i = 0; i < db->weapon_count; i++) {
        const WeaponDef *wd = &db->weapons[i];
        if (!wd->has_hold) continue;
        HandPoseSource *s = hp_sources_add(l, HPC_WEAPON, wd->id);
        if (!s) return;
        const ItemDef *it = NULL;
        for (int k = 1; k < db->item_count; k++) if (db->items[k].weapon == i) it = &db->items[k];
        dg_strcpy(s->label, sizeof(s->label), it ? it->name : wd->id);
        source_path(s->source, sizeof(s->source), "data/weapons", wd->file);
        snprintf(s->inherits, sizeof(s->inherits), "style/%s", wd->hold_style);
        s->hands = (u8)(2 | (wd->sockets.has_left && style_left(p, wd->hold_style) ? 1 : 0));
        s->states = hp_style_states(p, wd->hold_style);
        // Guns aim down sights (the ADS hold point is computed from the sight, never "set").
        if (wd->kind != WPN_MELEE) s->states |= 1u << HPS_ADS;
    }
    for (int i = 1; i < db->item_count; i++) {
        const ItemDef *d = &db->items[i];
        if (!d->has_hold || d->weapon >= 0 || d->kind == ITEM_WEAPON) continue;
        HandPoseSource *s = hp_sources_add(l, HPC_ITEM, d->id);
        if (!s) return;
        dg_strcpy(s->label, sizeof(s->label), d->name);
        dg_strcpy(s->source, sizeof(s->source), "data/items.json");
        snprintf(s->inherits, sizeof(s->inherits), "style/%s", d->hold_style);
        s->hands = (u8)(2 | (d->sockets.has_left && style_left(p, d->hold_style) ? 1 : 0));
        s->states = hp_style_states(p, d->hold_style);
        if (d->hold_style_alt[0]) {
            // Phone: the landscape style (two-handed camera mode).
            s->states |= hp_style_states(p, d->hold_style_alt);
            if (style_left(p, d->hold_style_alt)) s->hands |= 1;
        }
    }
    for (int a = 1; a <= db->attachment_count; a++) {
        const AttachmentDef *ad = &db->attachments[a];
        if (!ad->has_grip_l) continue;
        HandPoseSource *s = hp_sources_add(l, HPC_ATTACHMENT, ad->id);
        if (!s) return;
        dg_strcpy(s->label, sizeof(s->label), ad->name);
        source_path(s->source, sizeof(s->source), "assets/attachments", ad->file);
        s->hands = 1;   // the support hand moves onto the grip
        // States of every weapon it mounts on; it inherits the first host's style (the runtime
        // base is the style of the weapon actually holding it).
        s->states = 1u << HPS_DEFAULT;
        for (int w = 0; w < db->weapon_count; w++) {
            if (!attach_allowed(&db->weapons[w], ad)) continue;
            s->states |= hp_style_states(p, db->weapons[w].hold_style) | (db->weapons[w].kind != WPN_MELEE ? 1u << HPS_ADS : 0);
            if (!s->inherits[0]) snprintf(s->inherits, sizeof(s->inherits), "style/%s", db->weapons[w].hold_style);
        }
    }
}

void hp_source_from_vehicle(HandPoseSourceList *l, const VehicleDef *d, const char *source) {
    bool driver = false;
    for (int i = 0; i < VEH_MAX_SEATS; i++) driver |= d->seats[i].driver;
    if (!driver) return;
    HandPoseSource *s = hp_sources_add(l, HPC_VEHICLE, d->name);
    if (!s) return;
    dg_strcpy(s->label, sizeof(s->label), d->display[0] ? d->display : d->name);
    dg_strcpy(s->source, sizeof(s->source), source);
    dg_strcpy(s->inherits, sizeof(s->inherits), "style/grip_grab");
    s->hands = 3;
    s->states = (1u << HPS_DEFAULT) | (1u << HPS_DRIVE) | (1u << HPS_DRIVE_ONEHAND);
    s->relative = true;   // the steering wheel / handlebar grips are computed by the vehicle code
}

int hp_sources_from_vehicle_dir(HandPoseSourceList *l, const char *dir_rel) {
    l->categories |= 1u << HPC_VEHICLE;
    char dir[512];
    platform_data_path(dir_rel, dir, sizeof(dir));
    int count = 0, n = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    for (int i = 0; files && i < count; i++) {
        char path[768], rel[300];
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        snprintf(rel, sizeof(rel), "%s/%s", dir_rel, files[i]);
        size_t size = 0;
        char *text = platform_read_file(path, &size);
        if (!text) continue;
        VehicleDef d;
        vehicle_def_defaults(&d, strstr(text, "\"moto\"") ? VK_MOTO : VK_CAR);
        if (vehicle_def_parse(&d, text, size)) {
            if (!d.name[0]) {
                dg_strcpy(d.name, sizeof(d.name), files[i]);
                char *dot = strrchr(d.name, '.');
                if (dot) *dot = 0;
            }
            hp_source_from_vehicle(l, &d, rel);
            n++;
        }
        free(text);
    }
    SDL_free(files);
    return n;
}

void hp_source_from_interactable(HandPoseSourceList *l, const char *name, int hand_mode, float hold_time, bool has_hand_target, const char *source) {
    l->categories |= 1u << HPC_INTERACT;
    HandPoseSource *s = hp_sources_add(l, HPC_INTERACT, name);
    if (!s) return;
    snprintf(s->label, sizeof(s->label), "Interactable: %s", name);
    dg_strcpy(s->source, sizeof(s->source), source);
    // HANDS_POINT presses with a finger, the others grab. One-handed interactions use the free hand
    // (the left one while holding an item), so both hands get a pose.
    dg_strcpy(s->inherits, sizeof(s->inherits), hand_mode == 2 ? "style/grip_point" : "style/grip_grab");
    s->hands = 3;
    s->states = (1u << HPS_DEFAULT) | (1u << HPS_USE) | (hold_time > 0 ? 1u << HPS_HOLD : 0);
    s->relative = true;
    s->auto_place = !has_hand_target;
    if (s->auto_place) {
        // No hand target in code: the target is the top of the entity's bounds facing the user
        // (hands_interact_target); the default socket grabs it palm down, hands apart when both are used.
        float apart = hand_mode == 1 ? 0.14f : 0.0f;
        s->auto_socket[0] = xf_make(v3(-apart, 0.02f, 0), quat_identity());
        s->auto_socket[1] = xf_make(v3(apart, 0.02f, 0), quat_identity());
    }
}

// ------------------------------------------------------------------ scan

void hp_count(const HandPoseDB *db, HandPoseScanReport *r) {
    r->unreviewed = r->auto_placed = r->orphans = 0;
    for (int i = 0; i < db->count; i++) {
        const HandPoseEntry *e = &db->e[i];
        if (e->category == HPC_STYLE) continue;
        if (!e->reviewed) r->unreviewed++;
        if (e->auto_placed) r->auto_placed++;
        if (e->orphan) r->orphans++;
    }
}

void hp_scan(HandPoseDB *db, const HandPoseSourceList *src, const HandPoseScanOpts *o, HandPoseScanReport *r) {
    HandPoseScanReport tmp;
    if (!r) r = &tmp;
    memset(r, 0, sizeof(*r));
    r->sources = src->count;
    for (int i = 0; i < db->count; i++) db->e[i].seen = false;
    for (int k = 0; k < src->count; k++) {
        const HandPoseSource *s = &src->v[k];
        char id[96];
        handpose_entry_id(id, sizeof(id), s->category, s->name);
        int i = handpose_find(db, id);
        bool created = i < 0;
        if (created) {
            i = handpose_add(db, id, s->category);
            if (i < 0) continue;
            HandPoseEntry *e = &db->e[i];
            dg_strcpy(e->inherits, sizeof(e->inherits), s->inherits);
            e->relative = s->relative;
            e->auto_placed = s->auto_place;
            e->reviewed = false;
            if (s->auto_place)
                for (int h = 0; h < 2; h++)
                    if (s->hands & (1 << h)) {
                        e->layer[h][HPS_DEFAULT].set |= HPL_SOCKET;
                        e->layer[h][HPS_DEFAULT].socket = s->auto_socket[h];
                    }
            if (r->created < (int)ARRAY_COUNT(r->created_ids)) dg_strcpy(r->created_ids[r->created], sizeof(r->created_ids[0]), id);
            r->created++;
        } else {
            r->existing++;
        }
        HandPoseEntry *e = &db->e[i];
        e->seen = true;
        e->orphan = false;
        e->category = s->category;
        e->hands = s->hands;
        e->states = s->states;
        dg_strcpy(e->label, sizeof(e->label), s->label);
        dg_strcpy(e->source, sizeof(e->source), s->source);
        if (!e->inherits[0]) dg_strcpy(e->inherits, sizeof(e->inherits), s->inherits);
        if (created && o && o->write_dir) {
            char rel[200], path[700];
            handpose_entry_rel_path(e, "", rel, sizeof(rel));   // "/<id>.json"
            snprintf(path, sizeof(path), "%s%s", o->write_dir, rel);
            if (platform_file_exists(path)) {
                log_warn("handpose: %s exists but wasn't loaded (parse error?): not overwritten", path);
            } else if (hp_write_entry(e, o->write_dir, o->copy_dir)) {
                e->on_disk = true;
                r->written++;
            } else {
                log_warn("handpose: cannot write %s", path);
            }
        }
    }
    // Orphans: entries of a covered category whose source wasn't found.
    for (int i = 0; i < db->count; i++) {
        HandPoseEntry *e = &db->e[i];
        if (e->category == HPC_STYLE || !(src->categories & (1u << e->category))) continue;
        e->orphan = !e->seen;
    }
    handpose_link(db);
    handpose_rehash(db);
    int created = r->created, written = r->written, existing = r->existing, sources = r->sources;
    char ids[32][48];
    memcpy(ids, r->created_ids, sizeof(ids));
    hp_count(db, r);
    r->created = created;
    r->written = written;
    r->existing = existing;
    r->sources = sources;
    memcpy(r->created_ids, ids, sizeof(ids));
    if (o && o->log)
        log_info("handpose: %d sources, %d entries created (%d written), %d unreviewed, %d auto-placed, %d orphaned", r->sources, r->created, r->written,
                 r->unreviewed, r->auto_placed, r->orphans);
}

bool hp_write_entry(const HandPoseEntry *e, const char *dir, const char *copy_dir) {
    const char *dirs[2] = { dir, copy_dir };
    bool ok = dir != NULL;
    for (int k = 0; k < 2; k++) {
        if (!dirs[k] || (k == 1 && dir && !strcmp(dirs[0], dirs[1]))) continue;
        char rel[200], path[700], sub[700];
        handpose_entry_rel_path(e, "", rel, sizeof(rel));
        snprintf(path, sizeof(path), "%s%s", dirs[k], rel);
        snprintf(sub, sizeof(sub), "%s/styles", dirs[k]);
        SDL_CreateDirectory(sub);   // creates dirs[k] too
        bool w = handpose_entry_save_file(e, path);
        if (k == 0) ok = w;
    }
    return ok;
}

bool hp_remove_orphan(HandPoseDB *db, int entry, const char *dir, const char *copy_dir) {
    if (entry < 0 || entry >= db->count || !db->e[entry].orphan) return false;
    HandPoseEntry *e = &db->e[entry];
    const char *dirs[2] = { dir, copy_dir };
    for (int k = 0; k < 2 && e->on_disk; k++) {
        if (!dirs[k] || (k == 1 && dir && !strcmp(dirs[0], dirs[1]))) continue;
        char rel[200], path[700];
        handpose_entry_rel_path(e, "", rel, sizeof(rel));
        snprintf(path, sizeof(path), "%s%s", dirs[k], rel);
        if (!platform_file_exists(path)) continue;
        if (remove(path) != 0) log_warn("handpose: cannot remove %s", path);
        else log_info("handpose: removed orphan %s", path);
    }
    memmove(&db->e[entry], &db->e[entry + 1], sizeof(HandPoseEntry) * (size_t)(db->count - entry - 1));
    db->count--;
    handpose_link(db);
    handpose_rehash(db);
    return true;
}

int hp_report_text(const HandPoseDB *db, char *buf, int size) {
    int n = 0;
#define OUT(...) do { if (n < size) n += snprintf(buf + n, (size_t)(size - n), __VA_ARGS__); } while (0)
    HandPoseScanReport c;
    hp_count(db, &c);
    OUT("hand poses: %d entries, %d unreviewed, %d auto-placed, %d orphaned (hash %08x)\n", db->count, c.unreviewed, c.auto_placed, c.orphans, db->hash);
    static const char *titles[3] = { "unreviewed", "auto-placed (sockets placed by discovery, need review)", "orphaned (source gone: remove in the editor)" };
    for (int pass = 0; pass < 3; pass++) {
        int k = 0;
        for (int i = 0; i < db->count; i++) {
            const HandPoseEntry *e = &db->e[i];
            if (e->category == HPC_STYLE) continue;
            bool hit = pass == 0 ? !e->reviewed : pass == 1 ? e->auto_placed : e->orphan;
            if (!hit) continue;
            if (!k++) OUT("%s:\n", titles[pass]);
            OUT("  %-28s %-12s %s%s\n", e->id, handpose_category_name(e->category), e->source, e->on_disk ? "" : "  (not saved)");
        }
    }
#undef OUT
    return n;
}
