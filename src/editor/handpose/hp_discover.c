// Hand pose discovery: sources -> stub grips / cases, orphans, report (docs/HANDPOSE.md).
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

static HandPoseSource *push(HandPoseSourceList *l) {
    if (l->count >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        HandPoseSource *n = (HandPoseSource *)realloc(l->v, sizeof(HandPoseSource) * (size_t)cap);
        if (!n) return NULL;
        l->v = n;
        l->cap = cap;
    }
    HandPoseSource *s = &l->v[l->count++];
    memset(s, 0, sizeof(*s));
    s->auto_socket[0] = s->auto_socket[1] = xf_identity();
    return s;
}

HandPoseSource *hp_sources_add(HandPoseSourceList *l, int category, const char *name) {
    for (int i = 0; i < l->count; i++)
        if (!l->v[i].is_case && l->v[i].category == category && !strcmp(l->v[i].name, name)) return &l->v[i];
    HandPoseSource *s = push(l);
    if (!s) return NULL;
    s->category = (u8)category;
    dg_strcpy(s->name, sizeof(s->name), name);
    dg_strcpy(s->label, sizeof(s->label), name);
    l->categories |= 1u << category;
    return s;
}

HandPoseSource *hp_sources_add_case(HandPoseSourceList *l, int kind, const char *id, const char *bind) {
    for (int i = 0; i < l->count; i++)
        if (l->v[i].is_case && !strcmp(l->v[i].bind, bind)) return &l->v[i];
    HandPoseSource *s = push(l);
    if (!s) return NULL;
    s->is_case = true;
    s->kind = (u8)kind;
    dg_strcpy(s->name, sizeof(s->name), id);
    dg_strcpy(s->bind, sizeof(s->bind), bind);
    dg_strcpy(s->label, sizeof(s->label), id);
    s->hands = 2;
    l->case_kinds |= 1u << kind;
    return s;
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
    l->case_kinds |= (1u << HCK_RELOAD) | (1u << HCK_ITEM);
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
        // Every gun reloads: the clip owns the hands, the case holds its hand-off markers.
        if (wd->kind != WPN_MELEE) {
            char id[48], bind[64];
            snprintf(id, sizeof(id), "reload_%s", wd->id);
            snprintf(bind, sizeof(bind), "reload:%s", wd->id);
            HandPoseSource *c = hp_sources_add_case(l, HCK_RELOAD, id, bind);
            if (!c) return;
            snprintf(c->label, sizeof(c->label), "Reload: %s", it ? it->name : wd->id);
            dg_strcpy(c->source, sizeof(c->source), s->source);
            dg_strcpy(c->item, sizeof(c->item), wd->id);
            snprintf(c->inherits, sizeof(c->inherits), "style/%s", wd->hold_style);
            c->hands = s->hands;
            c->duration = wd->reload_time;
        }
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
        if (d->hold_style_alt[0] && style_left(p, d->hold_style_alt)) s->hands |= 1;   // phone: two-handed landscape
        if (d->camera) {
            // The phone in portrait: a view of its universal grip in the phone hold state.
            HandPoseSource *c = hp_sources_add_case(l, HCK_ITEM, "phone_portrait", "item:phone:portrait");
            if (!c) return;
            dg_strcpy(c->label, sizeof(c->label), "Phone: portrait");
            dg_strcpy(c->source, sizeof(c->source), "data/items.json");
            dg_strcpy(c->item, sizeof(c->item), d->id);
            dg_strcpy(c->hold_state, sizeof(c->hold_state), "phone_portrait");
            snprintf(c->inherits, sizeof(c->inherits), "style/%s", d->hold_style);
        }
    }
    for (int a = 1; a <= db->attachment_count; a++) {
        const AttachmentDef *ad = &db->attachments[a];
        if (!ad->has_grip_l) continue;
        HandPoseSource *s = hp_sources_add(l, HPC_ATTACHMENT, ad->id);
        if (!s) return;
        dg_strcpy(s->label, sizeof(s->label), ad->name);
        source_path(s->source, sizeof(s->source), "assets/attachments", ad->file);
        s->hands = 1;   // the support hand moves onto the grip (a sub-grip over the weapon's left hand)
        // It inherits the first host's style (the runtime base is the style of the weapon actually holding it).
        for (int w = 0; w < db->weapon_count && !s->inherits[0]; w++)
            if (attach_allowed(&db->weapons[w], ad)) snprintf(s->inherits, sizeof(s->inherits), "style/%s", db->weapons[w].hold_style);
    }
}

void hp_source_from_vehicle(HandPoseSourceList *l, const VehicleDef *d, const char *source) {
    l->case_kinds |= 1u << HCK_VEHICLE;
    int driver = -1;
    for (int i = 0; i < VEH_MAX_SEATS && i < d->seat_count; i++) if (d->seats[i].driver && driver < 0) driver = i;
    if (driver < 0) return;
    char id[48], bind[64];
    snprintf(id, sizeof(id), "vehicle_%s_driver", d->name);
    snprintf(bind, sizeof(bind), "vehicle:%s:driver", d->name);
    HandPoseSource *s = hp_sources_add_case(l, HCK_VEHICLE, id, bind);
    if (!s) return;
    snprintf(s->label, sizeof(s->label), "%s: driver", d->display[0] ? d->display : d->name);
    dg_strcpy(s->source, sizeof(s->source), source);
    dg_strcpy(s->inherits, sizeof(s->inherits), "style/grip_grab");
    dg_strcpy(s->vehicle, sizeof(s->vehicle), d->name);
    s->seat = (i8)driver;
    s->hands = 3;
}

int hp_sources_from_vehicle_dir(HandPoseSourceList *l, const char *dir_rel) {
    l->case_kinds |= 1u << HCK_VEHICLE;
    char dir[512];
    platform_data_path(dir_rel, dir, sizeof(dir));
    int count = 0, n = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    char first_passenger[24] = "";
    int passenger_seat = -1;
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
            for (int s = 0; s < d.seat_count && !first_passenger[0]; s++)
                if (!d.seats[s].driver && d.kind == VK_CAR) { dg_strcpy(first_passenger, sizeof(first_passenger), d.name); passenger_seat = s; }
            n++;
        }
        free(text);
    }
    SDL_free(files);
    if (first_passenger[0]) {
        // One passenger case for every vehicle (hands relative to the seat).
        HandPoseSource *s = hp_sources_add_case(l, HCK_VEHICLE, "vehicle_passenger", "vehicle:passenger");
        if (s) {
            dg_strcpy(s->label, sizeof(s->label), "Vehicle: passenger");
            dg_strcpy(s->source, sizeof(s->source), "assets/vehicles");
            dg_strcpy(s->inherits, sizeof(s->inherits), "style/grip_relaxed");
            dg_strcpy(s->vehicle, sizeof(s->vehicle), first_passenger);
            s->seat = (i8)passenger_seat;
            s->hands = 3;
        }
    }
    return n;
}

void hp_source_from_interactable(HandPoseSourceList *l, const char *name, int hand_mode, float hold_time, bool has_hand_target, const char *source) {
    char id[48], bind[64];
    snprintf(id, sizeof(id), "use_%s", name);
    snprintf(bind, sizeof(bind), "interactable:%s", name);
    HandPoseSource *s = hp_sources_add_case(l, HCK_INTERACT, id, bind);
    if (!s) return;
    snprintf(s->label, sizeof(s->label), "Interactable: %s", name);
    dg_strcpy(s->source, sizeof(s->source), source);
    dg_strcpy(s->prop, sizeof(s->prop), name);
    // HANDS_POINT presses with a finger, the others grab. One-handed interactions use the free hand
    // (the left one while holding an item), so both hands get a pose.
    dg_strcpy(s->inherits, sizeof(s->inherits), hand_mode == 2 ? "style/grip_point" : "style/grip_grab");
    s->hands = 3;
    s->hold_time = hold_time;
    s->auto_place = !has_hand_target;
    if (s->auto_place) {
        // No hand target in code: the target is the top of the entity's bounds facing the user
        // (hands_interact_target); the default socket grabs it palm down, hands apart when both are used.
        float apart = hand_mode == 1 ? 0.14f : 0.0f;
        s->auto_socket[0] = xf_make(v3(-apart, 0.02f, 0), quat_identity());
        s->auto_socket[1] = xf_make(v3(apart, 0.02f, 0), quat_identity());
    }
}

void hp_sources_builtin(HandPoseSourceList *l) {
    HandPoseSource *s = hp_sources_add_case(l, HCK_MANTLE, "mantle_ledge", "mantle");
    if (s) {
        dg_strcpy(s->label, sizeof(s->label), "Ledge: mantle / hang");
        dg_strcpy(s->source, sizeof(s->source), "src/player/movement.c");
        dg_strcpy(s->inherits, sizeof(s->inherits), "style/grip_palm");
        dg_strcpy(s->prop, sizeof(s->prop), "ledge");
        s->hands = 3;
    }
    s = hp_sources_add_case(l, HCK_STUB, "ladder_rung", "ladder");
    if (s) {
        dg_strcpy(s->label, sizeof(s->label), "Ladder rung (stub)");
        dg_strcpy(s->source, sizeof(s->source), "none yet");
        dg_strcpy(s->inherits, sizeof(s->inherits), "style/grip_grab");
        dg_strcpy(s->prop, sizeof(s->prop), "ladder");
        s->hands = 3;
    }
}

// ------------------------------------------------------------------ scan

void hp_count(const HandPoseDB *db, HandPoseScanReport *r) {
    r->unreviewed = r->auto_placed = r->orphans = r->cases = r->cases_unreviewed = 0;
    for (int i = 0; i < db->count; i++) {
        const HandPoseEntry *e = &db->e[i];
        if (e->category == HPC_STYLE) continue;
        if (!e->reviewed) r->unreviewed++;
        if (e->auto_placed) r->auto_placed++;
        if (e->orphan) r->orphans++;
    }
    for (int i = 0; i < db->case_count; i++) {
        const HandPoseCase *c = &db->c[i];
        r->cases++;
        if (!c->reviewed) { r->unreviewed++; r->cases_unreviewed++; }
        if (c->auto_placed) r->auto_placed++;
        if (c->orphan) r->orphans++;
    }
}

// After a stub write attempt: `exists` = the file was there before (never overwritten).
static void write_new(const char *path, bool exists, bool wrote, bool *on_disk, HandPoseScanReport *r) {
    if (exists) log_warn("handpose: %s exists but wasn't loaded (parse error?): not overwritten", path);
    else if (wrote) { *on_disk = true; r->written++; }
    else log_warn("handpose: cannot write %s", path);
}

// The source a bind names ("interactable:door/open" -> "interactable:door").
static void bind_base(const char *bind, char *out, int n) {
    dg_strcpy(out, (size_t)n, bind);
    char *slash = strchr(out, '/');
    if (slash) *slash = 0;
}

static int kind_of_bind(const char *bind) {
    if (!strncmp(bind, "interactable:", 13)) return HCK_INTERACT;
    if (!strncmp(bind, "vehicle:", 8)) return HCK_VEHICLE;
    if (!strncmp(bind, "reload:", 7)) return HCK_RELOAD;
    if (!strncmp(bind, "item:", 5)) return HCK_ITEM;
    if (!strcmp(bind, "mantle")) return HCK_MANTLE;
    return HCK_STUB;
}

void hp_scan(HandPoseDB *db, const HandPoseSourceList *src, const HandPoseScanOpts *o, HandPoseScanReport *r) {
    HandPoseScanReport tmp;
    if (!r) r = &tmp;
    memset(r, 0, sizeof(*r));
    r->sources = src->count;
    for (int i = 0; i < db->count; i++) db->e[i].seen = false;
    for (int i = 0; i < db->case_count; i++) db->c[i].seen = false;
    for (int k = 0; k < src->count; k++) {
        const HandPoseSource *s = &src->v[k];
        if (s->is_case) {
            // Any case bound to the source (an authored one such as door_knob, or the stub).
            int i = -1;
            for (int q = 0; q < db->case_count; q++) {
                char b[64];
                bind_base(db->c[q].bind, b, sizeof(b));
                if (!strcmp(b, s->bind)) { db->c[q].seen = true; db->c[q].orphan = false; if (i < 0) i = q; }
            }
            if (i >= 0) { r->existing++; continue; }
            i = handpose_case_add(db, s->name, s->kind);
            if (i < 0) continue;
            HandPoseCase *c = &db->c[i];
            dg_strcpy(c->bind, sizeof(c->bind), s->bind);
            c->bind_hash = dg_hash_str(c->bind);
            dg_strcpy(c->label, sizeof(c->label), s->label);
            dg_strcpy(c->source, sizeof(c->source), s->source);
            dg_strcpy(c->inherits, sizeof(c->inherits), s->inherits);
            dg_strcpy(c->prop, sizeof(c->prop), s->prop);
            dg_strcpy(c->vehicle, sizeof(c->vehicle), s->vehicle);
            dg_strcpy(c->item, sizeof(c->item), s->item);
            dg_strcpy(c->hold_state, sizeof(c->hold_state), s->hold_state);
            c->seat = s->seat;
            c->hands = s->hands;
            c->seen = true;
            c->auto_placed = s->auto_place;
            if (s->kind == HCK_RELOAD && s->duration > 0) {
                // The reload's timeline: hands handed to the clip early, taken back near the end.
                c->duration = s->duration;
                c->contact = fminf(0.15f, s->duration * 0.2f);
                c->release = fmaxf(c->contact, s->duration - 0.2f);
                c->use = 0.5f * (c->contact + c->release);
            }
            if (s->hold_time > 0) {
                // A held interaction: the hold segment spans the hold time (+ the 0.25 s the runtime adds).
                c->release = c->contact + s->hold_time + 0.25f;
                c->use = c->contact + (c->release - c->contact) * 0.5f;
                c->duration = c->release + 0.22f;
            }
            if (s->auto_place) {
                // One key on the auto-placed sockets (palm down on top of the bounds).
                HandCaseKey *key = &c->keys[c->key_count++];
                memset(key, 0, sizeof(*key));
                key->t = c->contact;
                dg_strcpy(key->name, sizeof(key->name), "contact");
                for (int h = 0; h < 2; h++) {
                    handpose_layer_clear(&key->hand[h]);
                    if (s->hands & (1 << h)) { key->hand[h].set = HPL_SOCKET; key->hand[h].socket = s->auto_socket[h]; }
                }
            }
            if (r->created < (int)ARRAY_COUNT(r->created_ids)) dg_strcpy(r->created_ids[r->created], sizeof(r->created_ids[0]), c->id);
            r->created++;
            if (o && o->write_dir) {
                char rel[200];
                handpose_case_rel_path(c, "", rel, sizeof(rel));
                char path[700];
                snprintf(path, sizeof(path), "%s%s", o->write_dir, rel);
                bool exists = platform_file_exists(path);
                write_new(path, exists, !exists && hp_write_case(c, o->write_dir, o->copy_dir), &c->on_disk, r);
            }
            continue;
        }
        char id[96];
        handpose_entry_id(id, sizeof(id), s->category, s->name);
        int i = handpose_find(db, id);
        bool created = i < 0;
        if (created) {
            i = handpose_add(db, id, s->category);
            if (i < 0) continue;
            HandPoseEntry *e = &db->e[i];
            dg_strcpy(e->inherits, sizeof(e->inherits), s->inherits);
            e->reviewed = false;
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
        dg_strcpy(e->label, sizeof(e->label), s->label);
        dg_strcpy(e->source, sizeof(e->source), s->source);
        if (!e->inherits[0]) dg_strcpy(e->inherits, sizeof(e->inherits), s->inherits);
        if (created && o && o->write_dir) {
            char rel[200], path[700];
            handpose_entry_rel_path(e, "", rel, sizeof(rel));
            snprintf(path, sizeof(path), "%s%s", o->write_dir, rel);
            bool exists = platform_file_exists(path);
            write_new(path, exists, !exists && hp_write_entry(e, o->write_dir, o->copy_dir), &e->on_disk, r);
        }
    }
    // Orphans: grips of a covered category / cases of a covered kind whose source wasn't found.
    for (int i = 0; i < db->count; i++) {
        HandPoseEntry *e = &db->e[i];
        if (e->category == HPC_STYLE || !(src->categories & (1u << e->category))) continue;
        e->orphan = !e->seen;
    }
    for (int i = 0; i < db->case_count; i++) {
        HandPoseCase *c = &db->c[i];
        int k = kind_of_bind(c->bind);
        if (!c->bind[0] || k == HCK_STUB || !(src->case_kinds & (1u << k))) continue;
        c->orphan = !c->seen;
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
        log_info("handpose: %d sources, %d created (%d written), %d grips + %d cases, %d unreviewed, %d auto-placed, %d orphaned", r->sources, r->created, r->written,
                 db->count, db->case_count, r->unreviewed, r->auto_placed, r->orphans);
}

static bool write_both(const char *rel, const char *dir, const char *copy_dir, const char *sub, char *(*writer)(const void *), const void *obj) {
    const char *dirs[2] = { dir, copy_dir };
    bool ok = dir != NULL;
    char *txt = writer(obj);
    if (!txt) return false;
    for (int k = 0; k < 2; k++) {
        if (!dirs[k] || (k == 1 && dir && !strcmp(dirs[0], dirs[1]))) continue;
        char path[700], sd[700];
        snprintf(path, sizeof(path), "%s%s", dirs[k], rel);
        snprintf(sd, sizeof(sd), "%s/%s", dirs[k], sub);
        SDL_CreateDirectory(sd);   // creates dirs[k] too
        bool w = platform_write_file(path, txt, strlen(txt));
        if (k == 0) ok = w;
    }
    free(txt);
    return ok;
}
static char *write_entry_cb(const void *e) { return handpose_entry_write((const HandPoseEntry *)e); }
static char *write_case_cb(const void *c) { return handpose_case_write((const HandPoseCase *)c); }

bool hp_write_entry(const HandPoseEntry *e, const char *dir, const char *copy_dir) {
    char rel[200];
    handpose_entry_rel_path(e, "", rel, sizeof(rel));
    return write_both(rel, dir, copy_dir, e->category == HPC_STYLE ? "styles" : "items", write_entry_cb, e);
}

bool hp_write_case(const HandPoseCase *c, const char *dir, const char *copy_dir) {
    char rel[200];
    handpose_case_rel_path(c, "", rel, sizeof(rel));
    return write_both(rel, dir, copy_dir, "cases", write_case_cb, c);
}

static void remove_files(const char *rel, bool on_disk, const char *dir, const char *copy_dir) {
    const char *dirs[2] = { dir, copy_dir };
    for (int k = 0; k < 2 && on_disk; k++) {
        if (!dirs[k] || (k == 1 && dir && !strcmp(dirs[0], dirs[1]))) continue;
        char path[700];
        snprintf(path, sizeof(path), "%s%s", dirs[k], rel);
        if (!platform_file_exists(path)) continue;
        if (remove(path) != 0) log_warn("handpose: cannot remove %s", path);
        else log_info("handpose: removed orphan %s", path);
    }
}

bool hp_remove_orphan(HandPoseDB *db, int entry, const char *dir, const char *copy_dir) {
    if (entry < 0 || entry >= db->count || !db->e[entry].orphan) return false;
    char rel[200];
    handpose_entry_rel_path(&db->e[entry], "", rel, sizeof(rel));
    remove_files(rel, db->e[entry].on_disk, dir, copy_dir);
    memmove(&db->e[entry], &db->e[entry + 1], sizeof(HandPoseEntry) * (size_t)(db->count - entry - 1));
    db->count--;
    handpose_link(db);
    handpose_rehash(db);
    return true;
}

bool hp_remove_orphan_case(HandPoseDB *db, int ci, const char *dir, const char *copy_dir) {
    if (ci < 0 || ci >= db->case_count || !db->c[ci].orphan) return false;
    char rel[200];
    handpose_case_rel_path(&db->c[ci], "", rel, sizeof(rel));
    remove_files(rel, db->c[ci].on_disk, dir, copy_dir);
    memmove(&db->c[ci], &db->c[ci + 1], sizeof(HandPoseCase) * (size_t)(db->case_count - ci - 1));
    db->case_count--;
    handpose_link(db);
    handpose_rehash(db);
    return true;
}

int hp_report_text(const HandPoseDB *db, char *buf, int size) {
    int n = 0;
#define OUT(...) do { if (n < size) n += snprintf(buf + n, (size_t)(size - n), __VA_ARGS__); } while (0)
    HandPoseScanReport c;
    hp_count(db, &c);
    OUT("hand poses: %d grips, %d cases, %d unreviewed, %d auto-placed, %d orphaned (hash %08x)\n", db->count, db->case_count, c.unreviewed, c.auto_placed, c.orphans, db->hash);
    static const char *titles[3] = { "unreviewed", "auto-placed (placed by discovery, need review)", "orphaned (source gone: remove in the editor)" };
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
        for (int i = 0; i < db->case_count; i++) {
            const HandPoseCase *cs = &db->c[i];
            bool hit = pass == 0 ? !cs->reviewed : pass == 1 ? cs->auto_placed : cs->orphan;
            if (!hit) continue;
            if (!k++) OUT("%s:\n", titles[pass]);
            OUT("  %-28s case/%-7s %s%s\n", cs->id, handpose_case_kind_name(cs->kind), cs->bind, cs->on_disk ? "" : "  (not saved)");
        }
    }
#undef OUT
    return n;
}
