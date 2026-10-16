// Hand pose editor scene (scene handpose, docs/HANDPOSE.md "Editor"). The local player holds the
// selected entry's item (equipped into hotbar slot 1 on the host, restored on leave) in the chosen
// hold point state (holdpoint_force_state, no sway), seen from an orbit camera around the hand (the
// body is drawn whole, third person) or from the normal first-person view. Every edit goes straight
// into the live DB (g_engine.anim->handposes): the runtime hold resolves it on the next tick with
// the same code (handpose_resolve_over), so the preview is the game. Field logic and undo live in
// hp_edit.c (pure); saving goes through hp_system (source tree + runtime copy, canonical writer).
#include "hp_editor.h"
#include "hp_edit.h"
#include "hp_system.h"
#include "anim/anim_library.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/scene.h"
#include "gameplay/attachments.h"
#include "gameplay/characters.h"
#include "gameplay/items.h"
#include "gameplay/weapons.h"
#include "render/debug_draw.h"
#include "ui/ui.h"
#include "nuklear_config.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIRTY 128

static struct {
    // selection
    u32 sel;                   // entry hash
    int hand, state;           // edited layer: hand 0 left / 1 right, HandPoseState
    // list filters
    int cat;                   // -1 = all
    bool f_unreviewed, f_auto, f_orphan, f_edited;
    char filter[48];
    // view
    bool fp_view, focus_item;
    float yaw, pitch, dist, fov;   // orbit around the focus, yaw relative to the player's view
    bool dragging;
    // preview: what the local player holds while the scene is open
    u32 preview_for;           // entry hash the preview was set up for
    char preview_text[160];
    bool saved_valid;
    World *saved_world;
    InvSlot saved_slot;
    u8 saved_hotbar;
    int saved_force, saved_static;
    // editing
    HpUndo undo;
    u32 dirty[MAX_DIRTY];
    int dirty_count;
    int preset, add_finger;
    char status[200];
    Cvar *save_dir;
} E = { .hand = 1, .state = HPS_HIP, .cat = -1, .yaw = 2.4f, .pitch = -0.25f, .dist = 0.75f, .fov = 45.0f };

static float S(float v) { return v * ui_scale(); }
static HandPoseDB *db(void) { return g_engine.anim ? &g_engine.anim->handposes : NULL; }
static int sel_index(void) { return db() ? handpose_find_hash(db(), E.sel) : -1; }

static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status, sizeof(E.status), fmt, ap);
    va_end(ap);
    log_info("hpe: %s", E.status);
}

// ------------------------------------------------------------------ dirty set (edited, not saved)

static bool is_dirty(u32 h) {
    for (int i = 0; i < E.dirty_count; i++) if (E.dirty[i] == h) return true;
    return false;
}
static void set_dirty(u32 h, bool on) {
    for (int i = 0; i < E.dirty_count; i++)
        if (E.dirty[i] == h) {
            if (!on) E.dirty[i] = E.dirty[--E.dirty_count];
            return;
        }
    if (on && E.dirty_count < MAX_DIRTY) E.dirty[E.dirty_count++] = h;
}

// ------------------------------------------------------------------ preview item

static const WeaponDef *weapon_by_id(const ItemDB *items, const char *id) {
    for (int i = 0; i < items->weapon_count; i++) if (!strcmp(items->weapons[i].id, id)) return &items->weapons[i];
    return NULL;
}
static u16 item_for_weapon(const ItemDB *items, const WeaponDef *wd) {
    int wi = (int)(wd - items->weapons);
    for (int i = 1; i < items->item_count; i++) if (items->items[i].weapon == wi) return (u16)i;
    return item_find(items, wd->id);
}

// The item (and attachments) that shows the entry: a weapon / item itself, an attachment mounted
// on the first weapon that takes it, a hold style on its first weapon / item. Vehicle controls,
// interactables and grip styles have no item: they preview while the player uses them.
static u16 preview_item(const HandPoseEntry *e, u8 att[ATT_SLOTS], char *why, int why_size) {
    Game *g = game();
    const ItemDB *items = &g->items;
    memset(att, 0, ATT_SLOTS);
    switch (e->category) {
    case HPC_WEAPON: {
        const WeaponDef *wd = weapon_by_id(items, e->id);
        if (!wd) break;
        memcpy(att, wd->att_default, ATT_SLOTS);
        return item_for_weapon(items, wd);
    }
    case HPC_ITEM: return item_find(items, e->id);
    case HPC_ATTACHMENT: {
        u8 id = attach_find(items, e->id + 4);   // "att_<id>"
        const AttachmentDef *a = attach_def(items, id);
        if (!a) break;
        for (int i = 0; i < items->weapon_count; i++) {
            const WeaponDef *wd = &items->weapons[i];
            if (!attach_allowed(wd, a)) continue;
            memcpy(att, wd->att_default, ATT_SLOTS);
            att[a->slot] = id;
            return item_for_weapon(items, wd);
        }
        break;
    }
    case HPC_STYLE: {
        const char *name = e->id + 6;   // "style/<name>"
        if (!strncmp(name, "grip_", 5)) {
            snprintf(why, (size_t)why_size, "grip style: previews on the free hand while using an interactable");
            return 0;
        }
        for (int i = 0; i < items->weapon_count; i++)
            if (!strcmp(items->weapons[i].hold_style, name)) {
                memcpy(att, items->weapons[i].att_default, ATT_SLOTS);
                return item_for_weapon(items, &items->weapons[i]);
            }
        for (int i = 1; i < items->item_count; i++)
            if (items->items[i].weapon < 0 && items->items[i].has_hold && !strcmp(items->items[i].hold_style, name)) return (u16)i;
        break;
    }
    case HPC_VEHICLE: snprintf(why, (size_t)why_size, "vehicle controls: sit in the vehicle to preview (the scene stays open)"); return 0;
    case HPC_INTERACT: snprintf(why, (size_t)why_size, "interactable: use one to preview (the scene stays open)"); return 0;
    default: break;
    }
    if (!why[0]) snprintf(why, (size_t)why_size, "no item found for %s", e->id);
    return 0;
}

static void restore_slot(void) {
    Game *g = game();
    Player *p = game_local_player();
    if (!E.saved_valid || !g || g->world != E.saved_world || !p) { E.saved_valid = false; return; }
    p->inv.slots[0] = E.saved_slot;
    p->inv.revision++;
    g->hotbar_slot = E.saved_hotbar;
    E.saved_valid = false;
}

static void update_preview(void) {
    HandPoseDB *d = db();
    int i = sel_index();
    if (!d || i < 0 || E.preview_for == E.sel) return;
    E.preview_for = E.sel;
    E.preview_text[0] = 0;
    Game *g = game();
    Player *p = game_local_player();
    const HandPoseEntry *e = &d->e[i];
    u8 att[ATT_SLOTS];
    u16 item = preview_item(e, att, E.preview_text, sizeof(E.preview_text));
    if (!item) return;
    if (!p || g->mode != GM_HOST) { snprintf(E.preview_text, sizeof(E.preview_text), "preview needs a local host player"); return; }
    if (!E.saved_valid) {
        E.saved_valid = true;
        E.saved_world = g->world;
        E.saved_slot = p->inv.slots[0];
        E.saved_hotbar = g->hotbar_slot;
    }
    InvSlot *s = &p->inv.slots[0];
    memset(s, 0, sizeof(*s));
    s->item = item;
    s->count = 1;
    const WeaponDef *wd = item_weapon(&g->items, item);
    s->ammo = wd ? (i16)wd->mag_size : 0;
    memcpy(s->att, att, ATT_SLOTS);
    p->inv.revision++;
    g->hotbar_slot = 0;
    snprintf(E.preview_text, sizeof(E.preview_text), "holding %s", item_def(&g->items, item)->id);
}

// The hold point state the preview forces for the edited layer state (default -> hip).
static int force_state(int hps) { return (hps >= HPS_HIP && hps <= HPS_INSPECT) ? hps - 1 : HP_HIP; }

// ------------------------------------------------------------------ the local body

static CharacterInst *local_char(void) {
    Game *g = game();
    Player *p = game_local_player();
    if (!g || !g->world || !p) return NULL;
    CharacterInst *c = char_get(g->world, p->ent);
    return c && c->have_pose ? c : NULL;
}

static Xform model_to_world(const CharacterInst *c) {
    const Skeleton *s = &g_engine.anim->skel;
    int b = s->role[BR_HIPS];
    if (c->pose_cur && b >= 0) return xf_mul(c->pose_cur[b], xf_inverse(c->rt.final_model[b]));
    return xf_make(c->root.t, quat_yaw(c->rt.proc.render_body_yaw));
}

// Entry space -> character model space (the held item, or the attachment's frame on it).
static bool entry_frame(const CharacterInst *c, const HandPoseEntry *e, Xform *out) {
    if (!c->rt.proc.has_item || e->relative) return false;
    Xform sp = xf_identity();
    const WeaponDef *wd = weapon_equipped_def(game()->world, game()->local_slot);
    if (e->category == HPC_ATTACHMENT && wd && wd->sockets.has_pose_space[0]) sp = wd->sockets.pose_space[0];
    *out = xf_mul(c->rt.proc.item_model, sp);
    return true;
}

// What an unset socket / pole resolves to at runtime: the item's grip (in entry space) and the
// hold style's elbow hint. Seeds the override when one is first set.
static void edit_defaults(const HandPoseEntry *e, HpEditDefaults *d) {
    for (int h = 0; h < 2; h++) { d->socket[h] = xf_identity(); d->pole[h] = v3(h ? 0.5f : -0.5f, -1, 0); }
    Game *g = game();
    if (!g || e->relative) return;
    const ItemSockets *it = NULL;
    const char *style = NULL;
    const WeaponDef *wd = g->world ? weapon_equipped_def(g->world, g->local_slot) : NULL;
    Player *p = game_local_player();
    const ItemDef *id = p ? item_def(&g->items, p->inv.slots[g->hotbar_slot < INV_SLOTS ? g->hotbar_slot : 0].item) : NULL;
    if (wd) { it = &wd->sockets; style = wd->hold_style; }
    else if (id && id->has_hold) { it = &id->sockets; style = id->hold_style; }
    if (it) {
        for (int h = 0; h < 2; h++) {
            Xform grip = h ? it->grip_r : it->grip_l;
            d->socket[h] = it->has_pose_space[h] && e->category == HPC_ATTACHMENT ? xf_mul(xf_inverse(it->pose_space[h]), grip) : grip;
        }
    }
    const ProcParams *pp = &g_engine.anim->proc;
    int si = style ? proc_hold_style_index(pp, style) : -1;
    if (si >= 0) {
        d->pole[0] = proc_hold_style(pp, si)->elbow_l;
        d->pole[1] = proc_hold_style(pp, si)->elbow_r;
    }
}

// Item boxes (model space) for the contact solver: weapon parts + mounted attachments, or the
// held item's box.
static int item_boxes(const CharacterInst *c, HandBox *out, int max) {
    Game *g = game();
    Player *p = game_local_player();
    if (!p || !c->rt.proc.has_item) return 0;
    Xform im = c->rt.proc.item_model;
    const InvSlot *slot = &p->inv.slots[g->hotbar_slot < INV_SLOTS ? g->hotbar_slot : 0];
    const WeaponDef *wd = item_weapon(&g->items, slot->item);
    int n = 0;
    if (wd && wd->part_count) {
        for (int i = 0; i < wd->part_count && n < max; i++, n++) out[n] = (HandBox){ xf_point(im, wd->parts[i].offset), wd->parts[i].half, im.r };
        AttachDrawPart parts[16];
        int k = attach_draw_parts(&g->items, wd, slot->att, parts, 16);
        for (int i = 0; i < k && n < max; i++, n++) out[n] = (HandBox){ xf_point(im, parts[i].center), parts[i].half, quat_mul(im.r, parts[i].rot) };
        return n;
    }
    vec3 ctr, half;
    item_held_box(item_def(&g->items, slot->item), &ctr, &half);
    out[0] = (HandBox){ xf_point(im, ctr), half, im.r };
    return 1;
}

// ------------------------------------------------------------------ edits

// Before an edit: an undo step (one per drag of a control: same key).
static void begin_edit(int entry, u32 key) { hp_undo_push(&E.undo, db(), entry, key ? key ^ (E.sel * 31u) ^ (u32)(E.hand * 7 + E.state * 101) : 0); }
static void end_edit(int entry) {
    HandPoseDB *d = db();
    hp_edit_commit(d);
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    set_dirty(d->e[entry].hash, true);
}

static HpEditDefaults sel_defaults(int entry) {
    HpEditDefaults d;
    edit_defaults(&db()->e[entry], &d);
    return d;
}

static void edit_joint(int entry, int f, int k, HandJoint v) {
    begin_edit(entry, 0x1000u + (u32)(f * 3 + k));
    hp_edit_set_joint(db(), entry, E.hand, E.state, f, k, v);
    end_edit(entry);
}

static bool reset_field(int entry, u16 bit) {
    if (!hp_field_overridden(db(), entry, E.hand, E.state, bit)) return false;
    begin_edit(entry, 0);
    hp_edit_reset(db(), entry, E.hand, E.state, bit);
    end_edit(entry);
    return true;
}

static bool apply_preset(int entry, const char *name) {
    begin_edit(entry, 0);
    bool ok = hp_edit_preset(db(), entry, E.hand, E.state, name);
    end_edit(entry);
    return ok;
}

static void mirror(int entry) {
    begin_edit(entry, 0);
    hp_edit_mirror(db(), entry, E.hand);
    end_edit(entry);
    status("mirrored %s -> %s (every state)", E.hand ? "right" : "left", E.hand ? "left" : "right");
}

// Closes the fingers onto the held item: fingers with a contact point stop at the curl closest to
// it (or where they touch), with all = true the others close until they touch. The solve starts
// from the body's pose this tick with the fingers back at the animation pose (the hand IK kept).
static int solve_contacts(int entry, bool all) {
    HandPoseDB *d = db();
    CharacterInst *c = local_char();
    const HandPoseEntry *e = &d->e[entry];
    Xform ef;
    if (!c || !entry_frame(c, e, &ef)) { status("contact solve: needs the entry's item in hand (relative entries: not supported)"); return 0; }
    const AnimLibrary *lib = g_engine.anim;
    const Skeleton *s = &lib->skel;
    static Pose pose;
    static Xform model[ANIM_MAX_BONES];
    pose_copy(&pose, &c->rt.final);
    int hb = s->role[E.hand ? BR_R_HAND : BR_L_HAND];
    for (int b = 0; b < s->count; b++)
        if (b != hb && skel_is_descendant(s, b, hb)) pose.local[b] = c->rt.base.local[b];
    pose_to_model(s, &pose, model);
    HandBox boxes[24];
    int nb = item_boxes(c, boxes, 24);
    HandPose p;
    handpose_resolve(d, entry, E.hand, E.state, &p);
    HandFingers j;
    memcpy(j, p.j, sizeof(j));
    begin_edit(entry, 0);
    int solved = 0;
    float worst = 0;
    for (int f = 0; f < HAND_FINGERS; f++) {
        vec3 tgt;
        bool has = false;
        for (int k = 0; k < p.contact_count; k++)
            if (p.contacts[k].finger == f) { tgt = xf_point(ef, p.contacts[k].point); has = true; }
        if (!has && !all) continue;
        HandContactResult r = handpose_contact_solve(s, &pose, model, E.hand, f, j, &lib->hands, boxes, nb, has ? &tgt : NULL, 0.008f, 0.0005f);
        HandPoseLayer *l = hp_edit_touch(d, entry, E.hand, E.state, (u16)HPL_FINGER(f), NULL);
        if (l) memcpy(l->j[f], j[f], sizeof(l->j[f]));
        worst = fmaxf(worst, r.tip_dist);
        solved++;
    }
    end_edit(entry);
    status("contact solve: %d finger(s) on %d box(es), largest surface gap %.1f mm", solved, nb, worst * 1000.0f);
    return solved;
}

static bool add_contact_at_tip(int entry, int finger) {
    HandPoseDB *d = db();
    CharacterInst *c = local_char();
    Xform ef;
    if (!c || !entry_frame(c, &d->e[entry], &ef)) { status("add contact: needs the entry's item in hand"); return false; }
    vec3 tip = handpose_fingertip(&g_engine.anim->skel, c->rt.final_model, E.hand, finger);
    begin_edit(entry, 0);
    bool ok = hp_edit_add_contact(d, entry, E.hand, E.state, finger, xf_point(xf_inverse(ef), tip));
    end_edit(entry);
    if (!ok) status("add contact: the list is full (%d)", HANDPOSE_CONTACTS);
    return ok;
}

static bool save_entry(int entry) {
    HandPoseDB *d = db();
    if (!d || entry < 0 || entry >= d->count) return false;
    HandPoseEntry *e = &d->e[entry];
    bool was = e->reviewed;
    e->reviewed = true;
    bool ok;
    char where[700];
    if (E.save_dir && E.save_dir->str[0]) {
        ok = hp_write_entry(e, E.save_dir->str, NULL);   // tests: an explicit directory only
        snprintf(where, sizeof(where), "%s", E.save_dir->str);
        handpose_system_note_own_write();
    } else {
        ok = handpose_system_save(entry);
        snprintf(where, sizeof(where), "%s", handpose_system_src_dir() ? handpose_system_src_dir() : handpose_system_rt_dir());
    }
    if (!ok) e->reviewed = was;
    else set_dirty(e->hash, false);
    char rel[200];
    handpose_entry_rel_path(e, "", rel, sizeof(rel));
    status("%s %s%s", ok ? "saved" : "cannot write", where, rel);
    return ok;
}

// Back to the file on disk (the runtime copy): layers and flags as saved. Undoable.
static bool revert_entry(int entry) {
    HandPoseDB *d = db();
    HandPoseEntry *e = &d->e[entry];
    if (!e->on_disk) { status("revert: %s has no file (generated default)", e->id); return false; }
    char rel[200], path[700];
    handpose_entry_rel_path(e, HANDPOSE_DIR, rel, sizeof(rel));
    platform_data_path(rel, path, sizeof(path));
    begin_edit(entry, 0);
    HandPoseEntry keep = *e;
    handpose_entry_clear_layers(e);
    char err[160];
    bool ok = handpose_entry_load_file(d, path, err, sizeof(err));
    e = &d->e[handpose_find_hash(d, keep.hash)];
    e->seen = keep.seen;
    e->orphan = keep.orphan;
    if (!ok) { memcpy(e->layer, keep.layer, sizeof(e->layer)); status("revert: %s", err); }
    handpose_link(d);
    end_edit(entry);
    set_dirty(e->hash, false);
    if (ok) status("reverted %s", e->id);
    return ok;
}

static void undo_redo(bool redo) {
    HandPoseDB *d = db();
    int i = redo ? hp_redo(&E.undo, d) : hp_undo(&E.undo, d);
    if (i < 0) { status("nothing to %s", redo ? "redo" : "undo"); return; }
    E.sel = d->e[i].hash;
    end_edit(i);
    status("%s (%s)", redo ? "redo" : "undo", d->e[i].id);
}

// ------------------------------------------------------------------ camera + markers

static bool focus_point(CharacterInst *c, vec3 *out) {
    Game *g = game();
    Player *p = game_local_player();
    if (E.focus_item && c->rt.proc.has_item) {
        vec3 mn, mx;
        const InvSlot *slot = &p->inv.slots[g->hotbar_slot < INV_SLOTS ? g->hotbar_slot : 0];
        vec3 ctr = item_model_bounds(&g->items, slot->item, &mn, &mx) ? v3_scale(v3_add(mn, mx), 0.5f) : v3(0, 0, 0);
        *out = xf_point(model_to_world(c), xf_point(c->rt.proc.item_model, ctr));
        return true;
    }
    return chars_palm_world(g->world, p->ent, E.hand, out);
}

static void draw_markers(CharacterInst *c) {
    HandPoseDB *d = db();
    int i = sel_index();
    if (!d || i < 0) return;
    Xform m2w = model_to_world(c);
    const Skeleton *s = &g_engine.anim->skel;
    // The palm frame of the edited hand (red = right, green = up, blue = back / -forward).
    int hb = s->role[E.hand ? BR_R_HAND : BR_L_HAND];
    if (hb >= 0) {
        Xform palm = xf_mul(m2w, xf_mul(c->rt.final_model[hb], g_engine.anim->hands.palm[E.hand]));
        dbg_line(palm.t, v3_madd(palm.t, quat_rotate(palm.r, v3(1, 0, 0)), 0.05f), DBG_RED, 0, false);
        dbg_line(palm.t, v3_madd(palm.t, quat_rotate(palm.r, v3(0, 1, 0)), 0.05f), DBG_GREEN, 0, false);
        dbg_line(palm.t, v3_madd(palm.t, quat_rotate(palm.r, v3(0, 0, 1)), 0.05f), DBG_BLUE, 0, false);
    }
    // Contact points (yellow) and the fingertip each one belongs to.
    Xform ef;
    if (!entry_frame(c, &d->e[i], &ef)) return;
    HandPose p;
    handpose_resolve(d, i, E.hand, E.state, &p);
    for (int k = 0; k < p.contact_count; k++) {
        vec3 w = xf_point(m2w, xf_point(ef, p.contacts[k].point));
        dbg_cross(w, 0.008f, DBG_YELLOW, 0);
        vec3 tip = xf_point(m2w, handpose_fingertip(s, c->rt.final_model, E.hand, p.contacts[k].finger));
        dbg_line(tip, w, DBG_YELLOW, 0, false);
    }
}

static bool scene_cam(World *w, RenderCamera *cam) {
    (void)w;
    update_preview();
    cvar_set_int(cvar_find("holdpoint_force_state"), force_state(E.state));
    CharacterInst *c = local_char();
    if (!c) return false;
    draw_markers(c);
    if (E.fp_view) return false;
    vec3 target;
    if (!focus_point(c, &target)) return false;
    Player *p = game_local_player();
    float yaw = p->sim.move.yaw + E.yaw;
    vec3 fwd = dir_from_angles(yaw, E.pitch);
    cam->pos = v3_madd(target, fwd, -E.dist);
    cam->yaw = yaw;
    cam->pitch = E.pitch;
    cam->roll = 0;
    cam->fov_deg = E.fov;
    cam->vm_fov_deg = E.fov;
    cam->ads_t = 0;
    cam->znear = fminf(cam->znear > 0 ? cam->znear : 0.05f, 0.02f);
    return true;
}

// Mouse in the viewport (not over a panel): left / right drag orbits, the wheel zooms.
static void orbit_input(struct nk_context *ctx) {
    const struct nk_input *in = &ctx->input;
    bool down = nk_input_is_mouse_down(in, NK_BUTTON_LEFT) || nk_input_is_mouse_down(in, NK_BUTTON_RIGHT);
    if (!down) E.dragging = false;
    else if (!E.dragging && (nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT) || nk_input_is_mouse_pressed(in, NK_BUTTON_RIGHT)) && !ui_mouse_over_window()) E.dragging = true;
    if (E.dragging) {
        E.yaw -= in->mouse.delta.x * 0.006f;
        E.pitch = clampf(E.pitch - in->mouse.delta.y * 0.006f, -1.5f, 1.5f);
    }
    if (!ui_mouse_over_window() && in->mouse.scroll_delta.y != 0) E.dist = clampf(E.dist * powf(0.9f, in->mouse.scroll_delta.y), 0.15f, 4.0f);
}

// ------------------------------------------------------------------ panels

static bool entry_visible(const HandPoseEntry *e) {
    if (E.cat >= 0 && e->category != E.cat) return false;
    if (E.f_unreviewed && e->reviewed) return false;
    if (E.f_auto && !e->auto_placed) return false;
    if (E.f_orphan && !e->orphan) return false;
    if (E.f_edited && !is_dirty(e->hash)) return false;
    return !E.filter[0] || strstr(e->id, E.filter) || strstr(e->label, E.filter);
}

static void draw_list(struct nk_context *ctx) {
    HandPoseDB *d = db();
    if (nk_begin(ctx, "HAND POSES", nk_rect(S(10), S(10), S(300), ui_height() - S(20)), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        static const char *cats[HPC_COUNT + 1] = { "all" };
        for (int c = 0; c < HPC_COUNT; c++) cats[c + 1] = handpose_category_name(c);
        nk_layout_row_dynamic(ctx, S(24), 2);
        E.cat = nk_combo(ctx, cats, HPC_COUNT + 1, E.cat + 1, (int)S(20), nk_vec2(S(160), S(200))) - 1;
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, E.filter, sizeof(E.filter), nk_filter_default);
        nk_layout_row_dynamic(ctx, S(20), 2);
        E.f_unreviewed = nk_check_label(ctx, "unreviewed", E.f_unreviewed);
        E.f_auto = nk_check_label(ctx, "auto-placed", E.f_auto);
        E.f_orphan = nk_check_label(ctx, "orphaned", E.f_orphan);
        E.f_edited = nk_check_label(ctx, "edited", E.f_edited);
        HandPoseScanReport r;
        hp_count(d, &r);
        char b[160];
        nk_layout_row_dynamic(ctx, S(16), 1);
        snprintf(b, sizeof(b), "%d entries, %d unreviewed", d->count, r.unreviewed);
        nk_label(ctx, b, NK_TEXT_LEFT);
        snprintf(b, sizeof(b), "%d auto-placed, %d orphaned, %d edited", r.auto_placed, r.orphans, E.dirty_count);
        nk_label(ctx, b, NK_TEXT_LEFT);
        nk_label(ctx, "* unreviewed  A auto  O orphan", NK_TEXT_LEFT);
        nk_label(ctx, "+ overrides   ! edited (not saved)", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, ui_height() - S(240), 1);
        if (nk_group_begin(ctx, "entries", NK_WINDOW_BORDER)) {
            nk_layout_row_dynamic(ctx, S(18), 1);
            for (int i = 0; i < d->count; i++) {
                const HandPoseEntry *e = &d->e[i];
                if (!entry_visible(e)) continue;
                snprintf(b, sizeof(b), "%c%c%c%c%c %s", e->reviewed ? ' ' : '*', e->auto_placed ? 'A' : ' ', e->orphan ? 'O' : ' ',
                         handpose_entry_has_overrides(e) ? '+' : ' ', is_dirty(e->hash) ? '!' : ' ', e->id);
                nk_bool on = e->hash == E.sel;
                if (nk_selectable_label(ctx, b, NK_TEXT_LEFT, &on) && on && e->hash != E.sel) { E.sel = e->hash; hp_undo_break(&E.undo); }
            }
            nk_group_end(ctx);
        }
    }
    nk_end(ctx);
}

// Row header of a field: label, where its value comes from, the reset button when overridden.
static void field_header(struct nk_context *ctx, int entry, const char *label, u16 bit) {
    HandPoseDB *d = db();
    bool ov = hp_field_overridden(d, entry, E.hand, E.state, bit);
    char src[120], b[160];
    hp_field_source_text(d, hp_field_source(d, entry, E.hand, E.state, bit), src, sizeof(src));
    if (ov) snprintf(b, sizeof(b), hp_field_shadowed(d, entry, E.hand, E.state, bit) ? "override (hidden by %s)" : "override", src);
    else snprintf(b, sizeof(b), "inherited: %s", src);
    float ratio[3] = { 0.30f, 0.52f, 0.18f };
    nk_layout_row(ctx, NK_DYNAMIC, S(18), 3, ratio);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_label_colored(ctx, b, NK_TEXT_LEFT, ov ? nk_rgb(255, 200, 90) : nk_rgb(150, 150, 150));
    if (ov) { if (nk_button_label(ctx, "reset")) reset_field(entry, bit); }
    else nk_label(ctx, "", NK_TEXT_LEFT);
}

static bool vec3_row(struct nk_context *ctx, vec3 *v, float lo, float hi, float step) {
    vec3 o = *v;
    nk_layout_row_dynamic(ctx, S(20), 3);
    nk_property_float(ctx, "#x", lo, &v->x, hi, step, step * 0.2f);
    nk_property_float(ctx, "#y", lo, &v->y, hi, step, step * 0.2f);
    nk_property_float(ctx, "#z", lo, &v->z, hi, step, step * 0.2f);
    return memcmp(&o, v, sizeof(o)) != 0;
}

// Position (m) + rotation (pitch / yaw / roll deg) of an Xform field.
static void xform_field(struct nk_context *ctx, int entry, const char *label, u16 bit, Xform cur, u32 key) {
    field_header(ctx, entry, label, bit);
    vec3 t = cur.t, e = handpose_quat_to_euler_deg(cur.r);
    bool ct = vec3_row(ctx, &t, -1.0f, 1.0f, 0.001f);
    bool cr = vec3_row(ctx, &e, -180.0f, 180.0f, 0.25f);
    if (!ct && !cr) return;
    begin_edit(entry, key);
    HpEditDefaults def = sel_defaults(entry);
    HandPoseLayer *l = hp_edit_touch(db(), entry, E.hand, E.state, bit, &def);
    Xform *x = bit == HPL_SOCKET ? &l->socket : &l->wrist;
    x->t = t;
    if (cr) x->r = quat_norm(handpose_euler_deg_to_quat(e));
    end_edit(entry);
}

static void draw_fingers(struct nk_context *ctx, int entry, const HandPose *p) {
    static const char *jn[2][3] = { { "CMC", "MCP", "IP" }, { "MCP", "PIP", "DIP" } };
    for (int f = 0; f < HAND_FINGERS; f++) {
        if (!nk_tree_push_id(ctx, NK_TREE_NODE, handpose_finger_name(f), f == HF_INDEX ? NK_MAXIMIZED : NK_MINIMIZED, f)) continue;
        field_header(ctx, entry, "joints", (u16)HPL_FINGER(f));
        float ratio[4] = { 0.16f, 0.28f, 0.28f, 0.28f };
        nk_layout_row(ctx, NK_DYNAMIC, S(14), 4, ratio);
        nk_label(ctx, "", NK_TEXT_LEFT);
        nk_label(ctx, "curl", NK_TEXT_CENTERED);
        nk_label(ctx, "spread", NK_TEXT_CENTERED);
        nk_label(ctx, "twist", NK_TEXT_CENTERED);
        for (int k = 0; k < HAND_FINGER_JOINTS; k++) {
            HandJoint lo, hi, v = p->j[f][k];
            handpose_joint_limits(f, k, &lo, &hi);
            nk_layout_row(ctx, NK_DYNAMIC, S(20), 4, ratio);
            nk_label(ctx, jn[f == HF_THUMB ? 0 : 1][k], NK_TEXT_LEFT);
            nk_property_float(ctx, "#", lo.curl, &v.curl, hi.curl, 1.0f, 0.25f);
            nk_property_float(ctx, "#", lo.spread, &v.spread, hi.spread, 0.5f, 0.15f);
            nk_property_float(ctx, "#", lo.twist, &v.twist, hi.twist, 0.5f, 0.15f);
            if (memcmp(&v, &p->j[f][k], sizeof(v))) edit_joint(entry, f, k, v);
        }
        nk_tree_pop(ctx);
    }
}

static void draw_contacts(struct nk_context *ctx, int entry, const HandPose *p) {
    static const char *fingers[HAND_FINGERS];
    for (int f = 0; f < HAND_FINGERS; f++) fingers[f] = handpose_finger_name(f);
    field_header(ctx, entry, "points", HPL_CONTACTS);
    HandPoseDB *d = db();
    for (int k = 0; k < p->contact_count; k++) {
        float ratio[5] = { 0.22f, 0.22f, 0.22f, 0.22f, 0.12f };
        nk_layout_row(ctx, NK_DYNAMIC, S(20), 5, ratio);
        HandContact c = p->contacts[k];
        int f = nk_combo(ctx, fingers, HAND_FINGERS, c.finger, (int)S(18), nk_vec2(S(120), S(140)));
        vec3 pt = c.point;
        nk_property_float(ctx, "#", -1, &pt.x, 1, 0.001f, 0.0002f);
        nk_property_float(ctx, "#", -1, &pt.y, 1, 0.001f, 0.0002f);
        nk_property_float(ctx, "#", -1, &pt.z, 1, 0.001f, 0.0002f);
        bool del = nk_button_label(ctx, "x");
        if (f != c.finger || memcmp(&pt, &c.point, sizeof(pt)) || del) {
            begin_edit(entry, del ? 0 : 0x2000u + (u32)k);
            HandPoseLayer *l = hp_edit_touch(d, entry, E.hand, E.state, HPL_CONTACTS, NULL);
            if (del) hp_edit_remove_contact(d, entry, E.hand, E.state, k);
            else { l->contacts[k].finger = (u8)f; l->contacts[k].point = pt; }
            end_edit(entry);
            break;
        }
    }
    nk_layout_row_dynamic(ctx, S(24), 2);
    E.add_finger = nk_combo(ctx, fingers, HAND_FINGERS, E.add_finger, (int)S(20), nk_vec2(S(140), S(140)));
    if (nk_button_label(ctx, "add at fingertip")) add_contact_at_tip(entry, E.add_finger);
    if (nk_button_label(ctx, "solve contacts")) solve_contacts(entry, false);
    if (nk_button_label(ctx, "close all fingers")) solve_contacts(entry, true);
}

static void draw_editor(struct nk_context *ctx) {
    HandPoseDB *d = db();
    int i = sel_index();
    float w = S(470);
    if (nk_begin(ctx, "HAND POSE", nk_rect(ui_width() - w - S(10), S(10), w, ui_height() - S(20)), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        char b[256];
        if (i < 0) {
            nk_layout_row_dynamic(ctx, S(20), 1);
            nk_label(ctx, "select an entry in the list", NK_TEXT_LEFT);
            nk_end(ctx);
            return;
        }
        const HandPoseEntry *e = &d->e[i];
        nk_layout_row_dynamic(ctx, S(18), 1);
        snprintf(b, sizeof(b), "%s  (%s, %s)%s%s%s", e->id, handpose_category_name(e->category), e->label, e->reviewed ? "" : "  unreviewed", e->auto_placed ? "  auto-placed" : "",
                 e->orphan ? "  ORPHAN" : "");
        nk_label(ctx, b, NK_TEXT_LEFT);
        snprintf(b, sizeof(b), "source %s", e->source[0] ? e->source : "-");
        nk_label(ctx, b, NK_TEXT_LEFT);
        snprintf(b, sizeof(b), "inherits %s%s", e->inherits[0] ? e->inherits : "-", e->relative ? "  (socket relative to the gameplay target)" : "");
        nk_label(ctx, b, NK_TEXT_LEFT);
        if (E.preview_text[0]) nk_label_colored(ctx, E.preview_text, NK_TEXT_LEFT, nk_rgb(140, 200, 255));
        // Hand + state (the edited layer). Marks: used by the source (*), overrides here (+).
        nk_layout_row_dynamic(ctx, S(24), 3);
        if (nk_option_label(ctx, (e->hands & 1) ? "left" : "left (unused)", E.hand == 0)) E.hand = 0;
        if (nk_option_label(ctx, (e->hands & 2) ? "right" : "right (unused)", E.hand == 1)) E.hand = 1;
        static char snames[HPS_COUNT][40];
        static const char *sp[HPS_COUNT];
        for (int s = 0; s < HPS_COUNT; s++) {
            snprintf(snames[s], sizeof(snames[s]), "%s%s%s", handpose_state_name(s), (e->states >> s) & 1 ? " *" : "", e->layer[E.hand][s].set ? " +" : "");
            sp[s] = snames[s];
        }
        E.state = nk_combo(ctx, sp, HPS_COUNT, E.state, (int)S(20), nk_vec2(S(220), S(340)));
        // File actions.
        nk_layout_row_dynamic(ctx, S(24), 5);
        if (nk_button_label(ctx, "Save")) save_entry(i);
        if (nk_button_label(ctx, "Revert")) revert_entry(i);
        if (nk_button_label(ctx, "Undo")) undo_redo(false);
        if (nk_button_label(ctx, "Redo")) undo_redo(true);
        if (nk_button_label(ctx, "Leave")) { scene_leave(); nk_end(ctx); return; }
        i = sel_index();   // undo / revert may have reordered nothing, but stay safe
        if (i < 0) { nk_end(ctx); return; }
        e = &d->e[i];
        if (E.status[0]) { nk_layout_row_dynamic(ctx, S(18), 1); nk_label(ctx, E.status, NK_TEXT_LEFT); }
        if (e->orphan) {
            nk_layout_row_dynamic(ctx, S(24), 1);
            if (nk_button_label(ctx, "delete orphan (file + entry)")) {
                const char *src = handpose_system_src_dir();
                if (hp_remove_orphan(d, i, src ? src : handpose_system_rt_dir(), handpose_system_rt_dir())) { status("orphan removed"); nk_end(ctx); return; }
            }
        }
        HandPose p;
        handpose_resolve(d, i, E.hand, E.state, &p);
        HpEditDefaults def = sel_defaults(i);
        if (nk_tree_push(ctx, NK_TREE_TAB, "Hand", NK_MAXIMIZED)) {
            xform_field(ctx, i, "socket", HPL_SOCKET, p.has_socket ? p.socket : def.socket[E.hand], 0x100);
            xform_field(ctx, i, "wrist offset", HPL_WRIST, p.wrist, 0x200);
            field_header(ctx, i, "pole hint", HPL_POLE);
            vec3 pole = p.has_pole ? p.pole : def.pole[E.hand];
            if (vec3_row(ctx, &pole, -2, 2, 0.01f)) {
                begin_edit(i, 0x300);
                hp_edit_touch(d, i, E.hand, E.state, HPL_POLE, &def)->pole = pole;
                end_edit(i);
            }
            field_header(ctx, i, "IK weight", HPL_IKW);
            float ikw = p.ik_weight;
            nk_layout_row_dynamic(ctx, S(20), 1);
            nk_property_float(ctx, "#weight", 0, &ikw, 1, 0.01f, 0.005f);
            if (ikw != p.ik_weight) {
                begin_edit(i, 0x400);
                hp_edit_touch(d, i, E.hand, E.state, HPL_IKW, NULL)->ik_weight = saturate(ikw);
                end_edit(i);
            }
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Fingers", NK_MAXIMIZED)) {
            static const char *presets[32];
            int pc = handpose_preset_count() < 32 ? handpose_preset_count() : 32;
            for (int k = 0; k < pc; k++) presets[k] = handpose_preset_name(k);
            nk_layout_row_dynamic(ctx, S(24), 3);
            E.preset = nk_combo(ctx, presets, pc, DG_CLAMP(E.preset, 0, pc - 1), (int)S(20), nk_vec2(S(180), S(260)));
            if (nk_button_label(ctx, "apply preset")) { apply_preset(i, presets[E.preset]); status("preset %s", presets[E.preset]); }
            snprintf(b, sizeof(b), "mirror %s -> %s", E.hand ? "R" : "L", E.hand ? "L" : "R");
            if (nk_button_label(ctx, b)) mirror(i);
            draw_fingers(ctx, i, &p);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Contacts", NK_MINIMIZED)) {
            draw_contacts(ctx, i, &p);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "View", NK_MINIMIZED)) {
            nk_layout_row_dynamic(ctx, S(22), 4);
            if (nk_option_label(ctx, "orbit", !E.fp_view)) E.fp_view = false;
            if (nk_option_label(ctx, "first person", E.fp_view)) E.fp_view = true;
            if (nk_option_label(ctx, "on hand", !E.focus_item)) E.focus_item = false;
            if (nk_option_label(ctx, "on item", E.focus_item)) E.focus_item = true;
            nk_layout_row_dynamic(ctx, S(20), 4);
            float yd = E.yaw * DG_RAD2DEG, pd = E.pitch * DG_RAD2DEG;
            nk_property_float(ctx, "#yaw", -360, &yd, 360, 1, 0.5f);
            nk_property_float(ctx, "#pitch", -85, &pd, 85, 1, 0.5f);
            nk_property_float(ctx, "#dist", 0.15f, &E.dist, 4, 0.01f, 0.005f);
            nk_property_float(ctx, "#fov", 15, &E.fov, 90, 1, 0.5f);
            E.yaw = yd * DG_DEG2RAD;
            E.pitch = pd * DG_DEG2RAD;
            nk_layout_row_dynamic(ctx, S(16), 1);
            nk_label(ctx, "drag in the view to orbit, wheel to zoom", NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

static void scene_draw_ui(void) {
    if (!db() || !game() || !game()->world) return;
    struct nk_context *ctx = ui_nk();
    const struct nk_input *in = &ctx->input;
    if (!nk_input_is_mouse_down(in, NK_BUTTON_LEFT)) hp_undo_break(&E.undo);   // one undo step per drag
    if (nk_input_is_key_pressed(in, NK_KEY_TEXT_UNDO) && !nk_item_is_any_active(ctx)) undo_redo(false);
    if (nk_input_is_key_pressed(in, NK_KEY_TEXT_REDO) && !nk_item_is_any_active(ctx)) undo_redo(true);
    orbit_input(ctx);
    draw_list(ctx);
    draw_editor(ctx);
}

// ------------------------------------------------------------------ scene

static bool scene_enter_fn(void) {
    if (!db() || !game() || !game()->world || !game_local_player()) { log_warn("scene handpose: needs a running game with a local player"); return false; }
    Cvar *fs = cvar_find("holdpoint_force_state"), *st = cvar_find("holdpoint_static");
    E.saved_force = fs ? fs->i : -1;
    E.saved_static = st ? st->i : 0;
    if (st) cvar_set_int(st, 1);
    E.preview_for = 0;
    E.status[0] = 0;
    if (sel_index() < 0) {
        // Start on the first unreviewed weapon (else the first entry).
        HandPoseDB *d = db();
        for (int i = 0; i < d->count && !E.sel; i++) if (d->e[i].category == HPC_WEAPON && !d->e[i].reviewed) E.sel = d->e[i].hash;
        if (!E.sel && d->count) E.sel = d->e[0].hash;
    }
    return true;
}

static void scene_leave_fn(void) {
    restore_slot();
    cvar_set_int(cvar_find("holdpoint_force_state"), E.saved_force);
    cvar_set_int(cvar_find("holdpoint_static"), E.saved_static);
    if (E.dirty_count) log_warn("scene handpose: %d edited entr%s not saved (they stay in memory until handpose_reload)", E.dirty_count, E.dirty_count == 1 ? "y" : "ies");
}

static const GameScene k_scene = {
    .name = "handpose",
    .desc = "hand pose editor: every weapon / item / attachment / vehicle / interactable hand pose (docs/HANDPOSE.md)",
    .enter = scene_enter_fn,
    .leave = scene_leave_fn,
    .camera = scene_cam,
    .draw = scene_draw_ui,
    .free_cursor = true,
    .hide_hud = true,
};

// ------------------------------------------------------------------ hpe command (scripts / tests)

static u16 field_bit(const char *s) {
    if (!strcmp(s, "socket")) return HPL_SOCKET;
    if (!strcmp(s, "wrist")) return HPL_WRIST;
    if (!strcmp(s, "pole")) return HPL_POLE;
    if (!strcmp(s, "ikw")) return HPL_IKW;
    if (!strcmp(s, "contacts")) return HPL_CONTACTS;
    for (int f = 0; f < HAND_FINGERS; f++) if (!strcmp(s, handpose_finger_name(f))) return (u16)HPL_FINGER(f);
    return 0;
}
static int finger_from(const char *s) {
    for (int f = 0; f < HAND_FINGERS; f++) if (!strcmp(s, handpose_finger_name(f))) return f;
    return atoi(s);
}

static void dump(int i) {
    HandPose p;
    handpose_resolve(db(), i, E.hand, E.state, &p);
    const HandPoseEntry *e = &db()->e[i];
    log_info("hpe: %s %s %s: set %04x, reviewed %d, ikw %.2f, socket %s, contacts %d", e->id, E.hand ? "right" : "left", handpose_state_name(E.state),
             e->layer[E.hand][E.state].set, e->reviewed, p.ik_weight, p.has_socket ? "override" : "item grip", p.contact_count);
    for (int f = 0; f < HAND_FINGERS; f++)
        log_info("hpe:   %-6s %6.1f %5.1f %5.1f | %6.1f %5.1f %5.1f | %6.1f %5.1f %5.1f  (%s)", handpose_finger_name(f), p.j[f][0].curl, p.j[f][0].spread, p.j[f][0].twist,
                 p.j[f][1].curl, p.j[f][1].spread, p.j[f][1].twist, p.j[f][2].curl, p.j[f][2].spread, p.j[f][2].twist,
                 hp_field_overridden(db(), i, E.hand, E.state, (u16)HPL_FINGER(f)) ? "override" : "inherited");
}

static void cmd_hpe(int argc, const char **argv) {
    HandPoseDB *d = db();
    if (!d || argc < 2) {
        log_info("hpe select <id> | hand l|r | state <name> | joint <finger> <0-2> <curl> [spread] [twist] | preset <name> | mirror | reset <field> | contact <finger> | "
                 "solve [all] | undo | redo | save | revert | view orbit|fp|item|hand|<yaw> <pitch> <dist> | dump");
        return;
    }
    const char *c = argv[1];
    if (!strcmp(c, "select") && argc > 2) {
        int i = handpose_find(d, argv[2]);
        if (i < 0) { log_warn("hpe: no entry '%s'", argv[2]); return; }
        E.sel = d->e[i].hash;
        hp_undo_break(&E.undo);
        return;
    }
    if (!strcmp(c, "view") && argc > 2) {
        if (!strcmp(argv[2], "fp")) E.fp_view = true;
        else if (!strcmp(argv[2], "orbit")) E.fp_view = false;
        else if (!strcmp(argv[2], "item")) E.focus_item = true;
        else if (!strcmp(argv[2], "hand")) E.focus_item = false;
        else if (argc > 4) { E.yaw = (float)atof(argv[2]) * DG_DEG2RAD; E.pitch = (float)atof(argv[3]) * DG_DEG2RAD; E.dist = (float)atof(argv[4]); }
        return;
    }
    if (!strcmp(c, "undo") || !strcmp(c, "redo")) { undo_redo(c[0] == 'r'); return; }
    int i = sel_index();
    if (i < 0) { log_warn("hpe: no entry selected (hpe select <id>)"); return; }
    if (!strcmp(c, "hand") && argc > 2) E.hand = argv[2][0] == 'l' ? 0 : 1;
    else if (!strcmp(c, "state") && argc > 2) {
        int s = handpose_state_from_name(argv[2]);
        if (s < 0) log_warn("hpe: unknown state '%s'", argv[2]);
        else E.state = s;
    } else if (!strcmp(c, "joint") && argc > 4) {
        int f = finger_from(argv[2]), k = atoi(argv[3]);
        HandPose p;
        handpose_resolve(d, i, E.hand, E.state, &p);
        if (f < 0 || f >= HAND_FINGERS || k < 0 || k > 2) { log_warn("hpe joint: bad finger / joint"); return; }
        HandJoint v = p.j[f][k];
        v.curl = (float)atof(argv[4]);
        if (argc > 5) v.spread = (float)atof(argv[5]);
        if (argc > 6) v.twist = (float)atof(argv[6]);
        hp_undo_break(&E.undo);
        edit_joint(i, f, k, v);
    } else if (!strcmp(c, "preset") && argc > 2) {
        if (!apply_preset(i, argv[2])) log_warn("hpe: unknown preset '%s'", argv[2]);
    } else if (!strcmp(c, "mirror")) mirror(i);
    else if (!strcmp(c, "reset") && argc > 2) {
        u16 bit = field_bit(argv[2]);
        if (!bit || !reset_field(i, bit)) log_warn("hpe reset: '%s' is not overridden here", argv[2]);
    } else if (!strcmp(c, "contact") && argc > 2) add_contact_at_tip(i, finger_from(argv[2]));
    else if (!strcmp(c, "solve")) solve_contacts(i, argc > 2 && !strcmp(argv[2], "all"));
    else if (!strcmp(c, "save")) save_entry(i);
    else if (!strcmp(c, "revert")) revert_entry(i);
    else if (!strcmp(c, "dump")) dump(i);
    else log_warn("hpe: unknown command '%s'", c);
}

void hp_editor_register(void) {
    scene_register(&k_scene);
    E.save_dir = cvar_reg_string("handpose_editor_dir", "", 0, "hand pose editor: save into this directory only (tests); empty = assets/handposes in the source tree + the runtime copy");
    cmd_register("hpe", cmd_hpe, "hand pose editor (scene handpose) commands for scripts: hpe with no arguments lists them");
}
