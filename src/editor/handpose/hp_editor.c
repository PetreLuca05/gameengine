// Hand pose editor scene (scene handpose, docs/HANDPOSE.md "Editor"): a control rig over the local
// player's arms - Root (item root / case anchor) -> IK_hand_L/R, Pole_L/R -> finger bones - edited
// with Blender-like transforms (G / R / S, axis constraints, snapping, typed values, gizmo), an
// outliner, an N-panel, and for cases a timeline over the simulated scene (hp_case.c). The world
// is frozen while the scene is open (game_freeze.c). Every edit goes straight into the live DB
// (g_engine.anim->handposes): the runtime resolves it on the next tick with the same code, so the
// preview is the game. Pure parts: hp_rig.c (controls -> data), hp_xform.c (modal transforms),
// hp_edit.c (fields, undo); saving goes through hp_system.
#include "hp_editor.h"
#include "hp_case.h"
#include "hp_edit.h"
#include "hp_rig.h"
#include "hp_system.h"
#include "hp_xform.h"
#include "anim/anim_library.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "game/scene.h"
#include "gameplay/attachments.h"
#include "gameplay/characters.h"

#include "gameplay/items.h"
#include "gameplay/weapons.h"
#include "player/hands.h"
#include "render/debug_draw.h"
#include "render/render.h"
#include "ui/ui.h"
#include "world/time_of_day.h"
#include "nuklear_config.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIRTY 128
#define GLUE_REFUSE_M 0.005f   // a grip / key whose hand ends further than this from its IK target can't be saved

enum { TOOL_SELECT, TOOL_MOVE, TOOL_ROTATE, TOOL_SCALE };
static const char *k_tool_names[] = { "Select", "Move", "Rotate", "Scale" };

static struct {
    // what is edited: a grip (entry) or a case (simulated scene)
    u32 sel_entry, sel_case;
    bool case_mode;
    int list_tab;              // 0 grips, 1 cases
    int cat;                   // grip list filter (-1 all)
    bool f_unreviewed, f_edited;
    char filter[48];
    int preview_state;         // HoldState shown for a grip (the grip is the same in every state)
    // view
    bool fp_view;
    int focus;                 // 0 the active control, 1 the item / anchor
    float yaw, pitch, dist, fov;
    bool orbiting;
    RenderCamera cam;
    bool cam_valid;
    // preview: what the local player holds while the scene is open
    u32 preview_for;
    char preview_text[160];
    bool saved_valid;
    World *saved_world;
    InvSlot saved_slot;
    u8 saved_hotbar;
    int saved_force, saved_static;
    bool neutral_light;
    // rig
    bool sel[HP_RIG_CTRLS];
    int active, hover;
    u8 locks[HP_RIG_CTRLS];
    bool mirror_live;
    int tool, orient, pivot;
    HpRigCtx ctx;
    bool ctx_valid;
    Xform world[HP_RIG_CTRLS];
    HpEditDefaults defs;
    // modal transform
    HpXform xf;
    bool xf_drag;              // started from a gizmo handle: releasing the button confirms
    HpRigCtx xf_ctx;
    HandJoint xf_j0[HP_RIG_CTRLS];
    HpTarget xf_target;
    HpUndoSnap *xf_snap;       // the target before the transform (cancel restores it)
    char xf_status[160];
    // clipboard (copy / paste transform)
    bool clip_valid;
    int clip_kind;
    HpLocal clip;
    // editing
    HpUndo undo;
    u32 dirty[MAX_DIRTY];
    int dirty_count;
    int preset;
    char status[220];
    Cvar *save_dir;
    // timeline
    bool onion;
    // keyboard edges
    u8 keys_prev[SDL_SCANCODE_COUNT];
    u8 key_inject[16];         // scripted presses (hpe key)
    int key_inject_n;
    vec2 mouse;
    bool mouse_inject;
    // world freeze check (tests)
    bool fz_valid;
    double fz_days;
    WeatherParams fz_wx;
    vec3 fz_pos[64];
    int fz_n;
    u32 fz_tick;
    double last_time;
} E = { .active = HP_RIG_IK(1), .hover = -1, .cat = -1, .yaw = 2.4f, .pitch = -0.25f, .dist = 0.9f, .fov = 45.0f, .tool = TOOL_MOVE, .orient = HPX_LOCAL,
        .pivot = HPX_PIVOT_MEDIAN, .neutral_light = true, .onion = true };

static float S(float v) { return v * ui_scale(); }
static HandPoseDB *db(void) { return g_engine.anim ? &g_engine.anim->handposes : NULL; }
static int sel_entry_index(void) { return db() ? handpose_find_hash(db(), E.sel_entry) : -1; }
static int sel_case_index(void) { return db() ? handpose_case_find_hash(db(), E.sel_case) : -1; }
static const HandPoseCase *sel_case(void) {
    int i = sel_case_index();
    return i >= 0 ? &db()->c[i] : NULL;
}

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

// ------------------------------------------------------------------ the edit target

// Item / reload cases edit the case item's grip; the scene cases (interact, vehicle, mantle,
// stub) their own anchor + the key at the timeline time.
static bool case_edits_grip(const HandPoseCase *c) { return c && (c->kind == HCK_ITEM || c->kind == HCK_RELOAD); }

static int grip_of_item(const char *id) { return id && id[0] ? handpose_find(db(), id) : -1; }

static int key_at_time(const HandPoseCase *c, float t) {
    for (int k = 0; c && k < c->key_count; k++) if (fabsf(c->keys[k].t - t) < 1e-3f) return k;
    return -1;
}

static HpTarget target(void) {
    HpTarget none = { -1, -1, -1 };
    if (!db()) return none;
    if (E.case_mode) {
        int ci = sel_case_index();
        if (ci < 0) return none;
        const HandPoseCase *c = &db()->c[ci];
        if (case_edits_grip(c)) {
            int e = grip_of_item(c->item);
            return e >= 0 ? hp_target_grip(e) : none;
        }
        return hp_target_case(ci, key_at_time(c, hp_case_state()->t));
    }
    int e = sel_entry_index();
    return e >= 0 ? hp_target_grip(e) : none;
}

static u32 target_hash(HpTarget t) {
    if (t.entry >= 0) return db()->e[t.entry].hash;
    if (t.ci >= 0) return db()->c[t.ci].hash;
    return 0;
}

// A hand edit on a case at a time without a key adds one there (seeded from the blended pose).
static HpTarget target_for_hands(void) {
    HpTarget t = target();
    if (t.ci >= 0 && t.key < 0) {
        int k = handpose_case_key_at(db(), t.ci, hp_case_state()->t);
        if (k >= 0) {
            t.key = k;
            status("keyframe added at %.3f s", hp_case_state()->t);
        } else status("cannot add a key (%d max)", HANDPOSE_CASE_KEYS);
    }
    return t;
}

// ------------------------------------------------------------------ edits + undo

static void begin_edit(HpTarget t, u32 key) { hp_undo_push(&E.undo, db(), t, key); }
static void end_edit(HpTarget t) {
    HandPoseDB *d = db();
    if (E.mirror_live && hp_target_has_layers(d, t)) {
        const HpCtrl *c = hp_rig_ctrl(E.active);
        if (c && c->hand >= 0) hp_edit_mirror(d, t, c->hand);
    }
    hp_edit_commit(d);
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    u32 h = target_hash(t);
    if (h) set_dirty(h, true);
}

static void undo_redo(bool redo) {
    HandPoseDB *d = db();
    HpTarget t = redo ? hp_redo(&E.undo, d) : hp_undo(&E.undo, d);
    if (t.entry < 0 && t.ci < 0) { status("nothing to %s", redo ? "redo" : "undo"); return; }
    hp_edit_commit(d);
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    set_dirty(target_hash(t), true);
    status("%s (%s)", redo ? "redo" : "undo", t.entry >= 0 ? d->e[t.entry].id : d->c[t.ci].id);
}

static void snap_take(HpUndoSnap *s, HpTarget t) {
    HandPoseDB *d = db();
    memset(s, 0, sizeof(*s));
    s->is_case = t.entry < 0;
    if (t.entry >= 0) { s->e = d->e[t.entry]; s->hash = s->e.hash; }
    else if (t.ci >= 0) { s->c = d->c[t.ci]; s->hash = s->c.hash; }
}
static void snap_restore(const HpUndoSnap *s) {
    HandPoseDB *d = db();
    if (s->is_case) {
        int i = handpose_case_find_hash(d, s->hash);
        if (i >= 0) { i16 p = d->c[i].parent; d->c[i] = s->c; d->c[i].parent = p; }
    } else {
        int i = handpose_find_hash(d, s->hash);
        if (i >= 0) { memcpy(d->e[i].grip, s->e.grip, sizeof(s->e.grip)); d->e[i].root = s->e.root; d->e[i].has_root = s->e.has_root; }
    }
    hp_edit_commit(d);
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

// The item (and attachments) that shows a grip: a weapon / item itself, an attachment mounted on
// the first weapon that takes it, a hold style on its first weapon / item.
static u16 preview_item_for(const char *id, int category, u8 att[ATT_SLOTS], char *why, int why_size) {
    Game *g = game();
    const ItemDB *items = &g->items;
    memset(att, 0, ATT_SLOTS);
    const WeaponDef *wd = weapon_by_id(items, id);
    if (wd && category != HPC_ATTACHMENT && category != HPC_STYLE) { memcpy(att, wd->att_default, ATT_SLOTS); return item_for_weapon(items, wd); }
    if (category == HPC_ITEM) return item_find(items, id);
    if (category == HPC_ATTACHMENT) {
        u8 aid = attach_find(items, id + 4);   // "att_<id>"
        const AttachmentDef *a = attach_def(items, aid);
        for (int i = 0; a && i < items->weapon_count; i++) {
            const WeaponDef *w = &items->weapons[i];
            if (!attach_allowed(w, a)) continue;
            memcpy(att, w->att_default, ATT_SLOTS);
            att[a->slot] = aid;
            return item_for_weapon(items, w);
        }
    }
    if (category == HPC_STYLE) {
        const char *name = id + 6;
        if (!strncmp(name, "grip_", 5)) { snprintf(why, (size_t)why_size, "grip style: shows on the free hand in the interaction cases"); return 0; }
        for (int i = 0; i < items->weapon_count; i++)
            if (!strcmp(items->weapons[i].hold_style, name)) { memcpy(att, items->weapons[i].att_default, ATT_SLOTS); return item_for_weapon(items, &items->weapons[i]); }
        for (int i = 1; i < items->item_count; i++)
            if (items->items[i].weapon < 0 && items->items[i].has_hold && !strcmp(items->items[i].hold_style, name)) return (u16)i;
    }
    if (!why[0]) snprintf(why, (size_t)why_size, "no item found for %s", id);
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

static void hold_item(u16 item, const u8 *att) {
    Game *g = game();
    Player *p = game_local_player();
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
    s->count = item ? 1 : 0;
    const WeaponDef *wd = item ? item_weapon(&g->items, item) : NULL;
    s->ammo = wd ? (i16)(wd->mag_size > 1 ? wd->mag_size - 1 : wd->mag_size) : 0;   // one short: a reload can run
    if (att) memcpy(s->att, att, ATT_SLOTS);
    p->inv.revision++;
    g->hotbar_slot = 0;
    if (item) snprintf(E.preview_text, sizeof(E.preview_text), "holding %s", item_def(&g->items, item)->id);
}

static void update_preview(void) {
    HandPoseDB *d = db();
    if (!d) return;
    u32 key = E.case_mode ? E.sel_case ^ 0x5bd1e995u : E.sel_entry;
    if (E.preview_for == key) return;
    E.preview_for = key;
    E.preview_text[0] = 0;
    u8 att[ATT_SLOTS];
    if (E.case_mode) {
        const HandPoseCase *c = sel_case();
        if (!c) return;
        if (case_edits_grip(c)) {
            int e = grip_of_item(c->item);
            u16 it = preview_item_for(c->item, e >= 0 ? d->e[e].category : HPC_ITEM, att, E.preview_text, sizeof(E.preview_text));
            if (it) hold_item(it, att);
        } else {
            // Empty hands: the interaction takes the right hand (holding an item, the free left
            // one does it at runtime), both on the controls / the ledge.
            hold_item(0, NULL);
            snprintf(E.preview_text, sizeof(E.preview_text), "hands empty (%s)", handpose_case_kind_name(c->kind));
        }
        return;
    }
    int i = sel_entry_index();
    if (i < 0) return;
    const HandPoseEntry *e = &d->e[i];
    u16 item = preview_item_for(e->id, e->category, att, E.preview_text, sizeof(E.preview_text));
    if (item) hold_item(item, att);
}

// ------------------------------------------------------------------ the local body + rig context

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

static const ItemSockets *held_sockets(const HandPoseEntry **owner) {
    Game *g = game();
    Player *p = game_local_player();
    *owner = NULL;
    if (!g || !p) return NULL;
    const WeaponDef *wd = g->world ? weapon_equipped_def(g->world, g->local_slot) : NULL;
    const ItemDef *id = item_def(&g->items, p->inv.slots[g->hotbar_slot < INV_SLOTS ? g->hotbar_slot : 0].item);
    const ItemSockets *k = wd ? &wd->sockets : (id && id->has_hold ? &id->sockets : NULL);
    if (k) {
        int e = handpose_find_hash(db(), k->pose_id[1]);
        if (e >= 0) *owner = &db()->e[e];
    }
    return k;
}

// What unset sockets / poles resolve to at runtime for the edited grip: the item's grip (in the
// grip's space) and the hold style's elbow hints.
static void edit_defaults(const HandPoseEntry *e, HpEditDefaults *d) {
    for (int h = 0; h < 2; h++) { d->socket[h] = xf_identity(); d->pole[h] = v3(h ? 0.7f : -0.7f, -1, 0.2f); }
    Game *g = game();
    if (!g || !g->world) return;
    const HandPoseEntry *owner;
    const ItemSockets *it = held_sockets(&owner);
    const WeaponDef *wd = weapon_equipped_def(g->world, g->local_slot);
    Player *p = game_local_player();
    const ItemDef *id = p ? item_def(&g->items, p->inv.slots[g->hotbar_slot < INV_SLOTS ? g->hotbar_slot : 0].item) : NULL;
    const char *style = wd ? wd->hold_style : (id && id->has_hold ? id->hold_style : NULL);
    if (it && e) {
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

static bool build_ctx(HpRigCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    CharacterInst *c = local_char();
    Game *g = game();
    if (!c || !g) return false;
    const AnimLibrary *lib = g_engine.anim;
    const Skeleton *s = &lib->skel;
    Xform m2w = model_to_world(c);
    m2w.s = v3(1, 1, 1);
    const Xform *mdl = c->rt.final_model;
    static const BoneRole arm[2][3] = { { BR_L_ARM, BR_L_FOREARM, BR_L_HAND }, { BR_R_ARM, BR_R_FOREARM, BR_R_HAND } };
    for (int h = 0; h < 2; h++) {
        int a = s->role[arm[h][0]], b = s->role[arm[h][1]], hb = s->role[arm[h][2]];
        if (a < 0 || b < 0 || hb < 0) return false;
        ctx->shoulder[h] = xf_point(m2w, mdl[a].t);
        ctx->elbow[h] = xf_point(m2w, mdl[b].t);
        ctx->wrist[h] = xf_point(m2w, mdl[hb].t);
        ctx->palm_frame[h] = xf_mul(m2w, xf_mul(mdl[hb], lib->hands.palm[h]));
        ctx->curl_axis[h] = lib->hands.curl_axis[h];
        ctx->thumb_side[h] = lib->hands.thumb_side[h];
        for (int f = 0; f < HAND_FINGERS; f++) {
            for (int j = 0; j < HAND_FINGER_JOINTS; j++) {
                int fb = handpose_finger_bone(s, h, f, j);
                ctx->finger_bone[h][f][j] = fb >= 0 ? xf_mul(m2w, mdl[fb]) : xf_identity();
                ctx->finger_bone[h][f][j].s = v3(1, 1, 1);
            }
            ctx->finger_tip[h][f] = xf_point(m2w, handpose_fingertip(s, mdl, h, f));
        }
        ctx->grip_space[h] = xf_identity();
    }
    ctx->pole_len = 0.25f;
    // Pole hints: the hold's are relative to the aim yaw (model space), the ledge / seat anchors'
    // to the body.
    float rel_yaw = wrap_angle(c->rt.in.aim_yaw - c->rt.proc.render_body_yaw);
    ctx->cam_rot = quat_mul(m2w.r, quat_yaw(rel_yaw));
    if (E.case_mode) {
        const HandPoseCase *cs = sel_case();
        if (cs && !case_edits_grip(cs)) {
            if (cs->kind == HCK_VEHICLE || cs->kind == HCK_MANTLE) ctx->cam_rot = m2w.r;
            else {
                HandsFpTargets t;
                if (hands_fp_targets(&t)) ctx->cam_rot = t.cam_rot;
            }
            return hp_case_rig_frames(g->world, ctx);
        }
    }
    // Grips: the hold frame = the finalized item without its root.
    if (!c->rt.proc.has_item) return false;
    HpTarget t = target();
    const HandPoseEntry *owner;
    const ItemSockets *k = held_sockets(&owner);
    Xform root = owner ? handpose_root(db(), (int)(owner - db()->e)) : xf_identity();
    float rs = root.s.x > 0 ? root.s.x : 1.0f;
    root.s = v3(1, 1, 1);
    Xform item_w = xf_mul(m2w, c->rt.proc.item_model);
    item_w.s = v3(1, 1, 1);
    ctx->root_parent = xf_mul(item_w, xf_inverse(root));
    if (t.entry >= 0 && db()->e[t.entry].category == HPC_ATTACHMENT && k) {
        // An attachment's sub-grip: its frame on the weapon, the weapon's root.
        for (int h = 0; h < 2; h++) if (k->has_pose_space[h]) ctx->grip_space[h] = k->pose_space[h];
        ctx->root_parent = item_w;
        ctx->grip_scale = rs;
    }
    return true;
}

// ------------------------------------------------------------------ projection / picking

static bool project(void *ctx, vec3 p, vec2 *px) {
    (void)ctx;
    return render_world_to_screen(p, px);
}

static float seg_dist(vec2 p, vec2 a, vec2 b) {
    vec2 ab = v2(b.x - a.x, b.y - a.y);
    float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 1e-6f ? clampf(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2, 0, 1) : 0;
    float dx = a.x + ab.x * t - p.x, dy = a.y + ab.y * t - p.y;
    return sqrtf(dx * dx + dy * dy);
}

static vec3 ctrl_end(int i) {
    const HpCtrl *c = hp_rig_ctrl(i);
    if (c->kind == HPK_FINGER) return c->joint + 1 < HAND_FINGER_JOINTS ? E.world[HP_RIG_FINGER(c->hand, c->finger, c->joint + 1)].t : E.ctx.finger_tip[c->hand][c->finger];
    return E.world[i].t;
}

static bool ctrl_usable(int i) {
    const HpCtrl *c = hp_rig_ctrl(i);
    HpTarget t = target();
    if (!c || !hp_target_valid(db(), t)) return false;
    if (c->kind == HPK_ROOT) {
        if (t.entry >= 0 && db()->e[t.entry].category == HPC_ATTACHMENT) return false;   // the weapon's root
        return true;
    }
    if (E.case_mode && sel_case() && sel_case()->kind == HCK_RELOAD) return false;   // view only
    return true;
}

static int pick(vec2 m) {
    if (!E.ctx_valid) return -1;
    float best = S(12.0f);
    int bi = -1;
    for (int i = 0; i < HP_RIG_CTRLS; i++) {
        if (!ctrl_usable(i)) continue;
        vec2 a, b;
        if (!render_world_to_screen(E.world[i].t, &a)) continue;
        const HpCtrl *c = hp_rig_ctrl(i);
        float d;
        if (c->kind == HPK_FINGER && render_world_to_screen(ctrl_end(i), &b)) d = seg_dist(m, a, b) + S(2.0f);   // points beat bones
        else d = sqrtf((m.x - a.x) * (m.x - a.x) + (m.y - a.y) * (m.y - a.y));
        if (d < best) { best = d; bi = i; }
    }
    return bi;
}

// ------------------------------------------------------------------ gizmo

static float gizmo_size(vec3 p) {
    vec3 cam = E.cam_valid ? E.cam.pos : p;
    return 0.12f * fmaxf(0.25f, v3_dist(p, cam)) * 0.9f;
}

// Rotation whose columns are the orthonormal basis x, y, z.
static quat quat_basis(vec3 x, vec3 y, vec3 z) {
    float tr = x.x + y.y + z.z;
    quat q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s; q.x = (y.z - z.y) / s; q.y = (z.x - x.z) / s; q.z = (x.y - y.x) / s;
    } else if (x.x > y.y && x.x > z.z) {
        float s = sqrtf(1.0f + x.x - y.y - z.z) * 2.0f;
        q.w = (y.z - z.y) / s; q.x = 0.25f * s; q.y = (y.x + x.y) / s; q.z = (z.x + x.z) / s;
    } else if (y.y > z.z) {
        float s = sqrtf(1.0f + y.y - x.x - z.z) * 2.0f;
        q.w = (z.x - x.z) / s; q.x = (y.x + x.y) / s; q.y = 0.25f * s; q.z = (z.y + y.z) / s;
    } else {
        float s = sqrtf(1.0f + z.z - x.x - y.y) * 2.0f;
        q.w = (x.y - y.x) / s; q.x = (z.x + x.z) / s; q.y = (z.y + y.z) / s; q.z = 0.25f * s;
    }
    return quat_norm(q);
}

static quat orient_basis(int ctrl, int orient) {
    if (ctrl < 0) return quat_identity();
    const HpCtrl *c = hp_rig_ctrl(ctrl);
    switch (orient) {
    case HPX_LOCAL: return E.world[ctrl].r;
    case HPX_PARENT: return c->parent >= 0 ? E.world[c->parent].r : E.ctx.root_parent.r;
    case HPX_NORMAL: {
        if (c->kind == HPK_FINGER) {
            vec3 ax[3];
            hp_rig_finger_axes(&E.ctx, c->hand, c->finger, c->joint, ax);
            // x = curl, y = spread, z = twist (orthonormalized).
            vec3 x = v3_norm(ax[0]), z = v3_norm(v3_sub(ax[2], v3_scale(x, v3_dot(ax[2], x))));
            return quat_basis(x, v3_cross(z, x), z);
        }
        if (c->kind == HPK_IK) return quat_mul(E.world[ctrl].r, quat_axis_angle(v3(1, 0, 0), -DG_PI * 0.5f));   // Z = the palm normal
        return E.world[ctrl].r;
    }
    default: return quat_identity();
    }
}

static vec3 pivot_point(void) {
    vec3 sum = v3(0, 0, 0);
    int n = 0;
    for (int i = 0; i < HP_RIG_CTRLS; i++) if (E.sel[i]) { sum = v3_add(sum, E.world[i].t); n++; }
    if (E.pivot == HPX_PIVOT_ACTIVE && E.active >= 0) return E.world[E.active].t;
    return n ? v3_scale(sum, 1.0f / (float)n) : (E.active >= 0 ? E.world[E.active].t : v3(0, 0, 0));
}

static int gizmo_mode(void) { return E.tool == TOOL_MOVE ? HPX_GRAB : E.tool == TOOL_ROTATE ? HPX_ROTATE : E.tool == TOOL_SCALE ? HPX_SCALE : HPX_NONE; }

// The gizmo handle under the mouse (axis 0..2, -1 none).
static int gizmo_pick(vec2 m) {
    int mode = gizmo_mode();
    if (mode == HPX_NONE || E.active < 0 || !E.sel[E.active]) return -1;
    vec3 p = pivot_point();
    float sz = gizmo_size(p);
    quat b = orient_basis(E.active, E.orient);
    static const vec3 e[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    vec2 ps;
    if (!render_world_to_screen(p, &ps)) return -1;
    float best = S(9.0f);
    int bi = -1;
    for (int a = 0; a < 3; a++) {
        vec3 ax = quat_rotate(b, e[a]);
        if (mode == HPX_ROTATE) {
            vec3 u = quat_rotate(b, e[(a + 1) % 3]), v = quat_rotate(b, e[(a + 2) % 3]);
            vec2 prev;
            bool hp = false;
            for (int k = 0; k <= 32; k++) {
                float an = (float)k / 32.0f * DG_PI * 2.0f;
                vec3 q = v3_add(p, v3_add(v3_scale(u, cosf(an) * sz * 0.8f), v3_scale(v, sinf(an) * sz * 0.8f)));
                vec2 s2;
                if (!render_world_to_screen(q, &s2)) { hp = false; continue; }
                if (hp) {
                    float d = seg_dist(m, prev, s2);
                    if (d < best) { best = d; bi = a; }
                }
                prev = s2;
                hp = true;
            }
        } else {
            vec2 s2;
            if (!render_world_to_screen(v3_madd(p, ax, sz), &s2)) continue;
            float d = seg_dist(m, ps, s2);
            if (d < best) { best = d; bi = a; }
        }
    }
    return bi;
}

static void draw_gizmo(void) {
    int mode = hpx_active(&E.xf) ? E.xf.mode : gizmo_mode();
    if (mode == HPX_NONE || E.active < 0 || !E.sel[E.active]) return;
    vec3 p = hpx_active(&E.xf) ? E.xf.pivot : pivot_point();
    float sz = gizmo_size(p);
    int orient = hpx_active(&E.xf) ? E.xf.orient : E.orient;
    quat b = hpx_active(&E.xf) ? E.xf.basis[orient] : orient_basis(E.active, orient);
    static const vec3 e[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    static const u32 col[3] = { DBG_RED, DBG_GREEN, DBG_BLUE };
    for (int a = 0; a < 3; a++) {
        bool hot = hpx_active(&E.xf) && E.xf.axis == a;
        u32 c = hot ? DBG_YELLOW : col[a];
        vec3 ax = quat_rotate(b, e[a]);
        if (mode == HPX_ROTATE) {
            vec3 u = quat_rotate(b, e[(a + 1) % 3]), v = quat_rotate(b, e[(a + 2) % 3]);
            vec3 prev = v3_add(p, v3_scale(u, sz * 0.8f));
            for (int k = 1; k <= 40; k++) {
                float an = (float)k / 40.0f * DG_PI * 2.0f;
                vec3 q = v3_add(p, v3_add(v3_scale(u, cosf(an) * sz * 0.8f), v3_scale(v, sinf(an) * sz * 0.8f)));
                dbg_line(prev, q, c, 0, false);
                prev = q;
            }
        } else {
            vec3 tip = v3_madd(p, ax, sz);
            dbg_line(p, tip, c, 0, false);
            if (mode == HPX_SCALE) dbg_box(tip, v3s(sz * 0.05f), b, c, 0);
            else dbg_cross(tip, sz * 0.06f, c, 0);
        }
    }
    // The constraint line through the pivot while transforming.
    if (hpx_active(&E.xf) && E.xf.axis >= 0 && !E.xf.plane) {
        vec3 ax = hpx_axis_world(&E.xf, E.xf.axis);
        dbg_line(v3_madd(p, ax, -sz * 4), v3_madd(p, ax, sz * 4), col[E.xf.axis], 0, false);
    }
}

// ------------------------------------------------------------------ rig drawing

static u32 ctrl_color(int i) {
    const HpCtrl *c = hp_rig_ctrl(i);
    if (E.sel[i]) return i == E.active ? DBG_WHITE : 0xFFE0E0E0u;
    for (int p = c->parent, n = 0; p >= 0 && n < 8; p = hp_rig_parent(p), n++) if (E.sel[p]) return 0xFFB0B0B0u;   // children of the selection: dimmed highlight
    if (i == E.hover) return DBG_YELLOW;
    if (!ctrl_usable(i)) return DBG_GREY;
    switch (c->kind) {
    case HPK_ROOT: return DBG_ORANGE;
    case HPK_IK: return c->hand ? 0xFF4040FFu : 0xFFFFA040u;
    case HPK_POLE: return DBG_MAGENTA;
    default: return c->hand ? 0xFF60C0FFu : 0xFFFFC060u;
    }
}

static void draw_palm(Xform x, float half_w, u32 c) {
    // A palm plate (fingers along -Z, back of the hand +Y).
    dbg_box(v3_add(x.t, quat_rotate(x.r, v3(0, 0, -0.03f))), v3(half_w, 0.008f, 0.045f), x.r, c, 0);
    dbg_line(x.t, v3_add(x.t, quat_rotate(x.r, v3(0, 0.03f, 0))), c, 0, false);
}

static void draw_rig(void) {
    if (!E.ctx_valid) return;
    dbg_set_shape_depth(false);
    // Arms (display only: they follow the IK).
    for (int h = 0; h < 2; h++) {
        dbg_line(E.ctx.shoulder[h], E.ctx.elbow[h], 0xFF808080u, 0, false);
        dbg_line(E.ctx.elbow[h], E.ctx.wrist[h], 0xFF808080u, 0, false);
    }
    // Root.
    Xform r = E.world[HP_RIG_ROOT];
    u32 rc = ctrl_color(HP_RIG_ROOT);
    float rs = 0.05f;
    dbg_line(v3_madd(r.t, quat_rotate(r.r, v3(1, 0, 0)), -rs), v3_madd(r.t, quat_rotate(r.r, v3(1, 0, 0)), rs), rc, 0, false);
    dbg_line(v3_madd(r.t, quat_rotate(r.r, v3(0, 0, 1)), -rs), v3_madd(r.t, quat_rotate(r.r, v3(0, 0, 1)), rs * 1.6f), rc, 0, false);
    dbg_line(r.t, v3_madd(r.t, quat_rotate(r.r, v3(0, 1, 0)), rs), rc, 0, false);
    dbg_box(r.t, v3s(0.012f), r.r, rc, 0);
    for (int h = 0; h < 2; h++) {
        // IK control: the palm target + a line from the root (hierarchy) and from the solved palm.
        int ik = HP_RIG_IK(h);
        u32 c = ctrl_color(ik);
        draw_palm(E.world[ik], 0.04f, c);
        dbg_line(r.t, E.world[ik].t, 0x60FFFFFFu & c, 0, false);
        float glue = v3_dist(E.ctx.palm_frame[h].t, E.world[ik].t);
        if (glue > GLUE_REFUSE_M) dbg_line(E.ctx.palm_frame[h].t, E.world[ik].t, DBG_RED, 0, false);
        // Pole: elbow -> pole point.
        int pl = HP_RIG_POLE(h);
        u32 pc = ctrl_color(pl);
        dbg_line(E.ctx.elbow[h], E.world[pl].t, pc, 0, false);
        dbg_cross(E.world[pl].t, 0.018f, pc, 0);
        // Fingers: bones joint -> next joint, a cross on each joint.
        for (int f = 0; f < HAND_FINGERS; f++)
            for (int j = 0; j < HAND_FINGER_JOINTS; j++) {
                int fi = HP_RIG_FINGER(h, f, j);
                u32 fc = ctrl_color(fi);
                dbg_line(E.world[fi].t, ctrl_end(fi), fc, 0, false);
                dbg_cross(E.world[fi].t, E.sel[fi] || fi == E.hover ? 0.007f : 0.004f, fc, 0);
            }
    }
    if (E.active >= 0) dbg_text(v3_add(E.world[E.active].t, v3(0, 0.035f, 0)), hp_rig_ctrl(E.active)->name, DBG_WHITE, 0.008f, 0);
    dbg_set_shape_depth(true);
}

// Onion skin: the hands at the neighbouring keys (ghosts: the arm re-solved onto the key's IK
// targets with its fingers, drawn as bone lines).
static void draw_onion(void) {
    const HandPoseCase *c = E.case_mode ? sel_case() : NULL;
    CharacterInst *ch = local_char();
    if (!E.onion || !c || case_edits_grip(c) || !c->key_count || !ch || !E.ctx_valid) return;
    float t = hp_case_state()->t;
    int prev = -1, next = -1;
    for (int k = 0; k < c->key_count; k++) {
        if (c->keys[k].t < t - 1e-3f) prev = k;
        if (c->keys[k].t > t + 1e-3f && next < 0) next = k;
    }
    const AnimLibrary *lib = g_engine.anim;
    const Skeleton *s = &lib->skel;
    Xform m2w = model_to_world(ch);
    m2w.s = v3(1, 1, 1);
    Xform w2m = xf_inverse(m2w);
    static Pose pose;
    static Xform model[ANIM_MAX_BONES];
    int ks[2] = { prev, next };
    u32 cols[2] = { 0xFF60A0FFu, 0xFFFF9060u };
    HpTarget base = hp_target_case(sel_case_index(), -1);
    for (int q = 0; q < 2; q++) {
        if (ks[q] < 0) continue;
        pose_copy(&pose, &ch->rt.final);
        memcpy(model, ch->rt.final_model, sizeof(Xform) * (size_t)s->count);
        HpTarget kt = base;
        kt.key = ks[q];
        static const BoneRole arm[2][3] = { { BR_L_ARM, BR_L_FOREARM, BR_L_HAND }, { BR_R_ARM, BR_R_FOREARM, BR_R_HAND } };
        Xform kw[HP_RIG_CTRLS];
        hp_rig_world(db(), kt, &E.ctx, &E.defs, kw);
        for (int h = 0; h < 2; h++) {
            if (!(c->hands & (1 << h))) continue;
            int a = s->role[arm[h][0]], b = s->role[arm[h][1]], hb = s->role[arm[h][2]];
            Xform palm_m = xf_mul(w2m, kw[HP_RIG_IK(h)]);
            Xform hand = proc_hand_for_socket(palm_m, lib->hands.palm[h]);
            vec3 pole = quat_rotate(quat_conj(m2w.r), v3_sub(kw[HP_RIG_POLE(h)].t, E.ctx.elbow[h]));
            anim_ik_two_bone_pose(s, &pose, model, a, b, hb, hand.t, &hand.r, pole, 1.0f);
            HandPose hp;
            handpose_case_key_pose(db(), kt.ci, -1, kt.key, h, &hp);
            handpose_apply_fingers(s, &pose, model, h, hp.j, &lib->hands, 1.0f);
            u32 col = cols[q];
            dbg_line(xf_point(m2w, model[a].t), xf_point(m2w, model[b].t), col, 0, false);
            dbg_line(xf_point(m2w, model[b].t), xf_point(m2w, model[hb].t), col, 0, false);
            for (int f = 0; f < HAND_FINGERS; f++) {
                vec3 last = xf_point(m2w, model[hb].t);
                for (int j = 0; j < HAND_FINGER_JOINTS; j++) {
                    int fb = handpose_finger_bone(s, h, f, j);
                    if (fb < 0) break;
                    vec3 p = xf_point(m2w, model[fb].t);
                    dbg_line(last, p, col, 0, false);
                    last = p;
                }
                dbg_line(last, xf_point(m2w, handpose_fingertip(s, model, h, f)), col, 0, false);
            }
        }
    }
}

// ------------------------------------------------------------------ transforms

static void apply_xform_result(void) {
    HandPoseDB *d = db();
    HpTarget t = E.xf_target;
    // From the start state every update (absolute, no drift).
    snap_restore(E.xf_snap);
    for (int i = 0; i < E.xf.count; i++) {
        int ctrl = E.xf.ids[i];
        hp_rig_apply(d, t, &E.xf_ctx, &E.defs, ctrl, E.xf.start[i], E.xf.out[i], &E.xf_j0[ctrl], E.locks[ctrl]);
    }
    if (E.mirror_live && hp_target_has_layers(d, t)) {
        const HpCtrl *c = hp_rig_ctrl(E.active);
        if (c && c->hand >= 0) hp_edit_mirror(d, t, c->hand);
    }
    hp_edit_commit(d);
}

// Which selected controls take a mode (fingers rotate only, poles translate only, ...).
static int xform_ctrls(int mode, int *out) {
    int top[HP_RIG_CTRLS];
    int n = hp_rig_top_level(E.sel, top, HP_RIG_CTRLS), k = 0;
    u8 need = mode == HPX_GRAB ? HPA_T : mode == HPX_ROTATE ? HPA_R : HPA_S;
    for (int i = 0; i < n; i++) {
        const HpCtrl *c = hp_rig_ctrl(top[i]);
        if ((c->allow & need) && ctrl_usable(top[i])) {
            if (mode == HPX_SCALE && E.case_mode && !case_edits_grip(sel_case())) continue;   // case anchors don't scale
            out[k++] = top[i];
        }
    }
    return k;
}

static bool xform_begin(int mode, vec2 mouse, bool drag) {
    if (!E.ctx_valid || hpx_active(&E.xf)) return false;
    int ids[HP_RIG_CTRLS];
    int n = xform_ctrls(mode, ids);
    if (!n) {
        status("%s: nothing selected takes it (fingers rotate only, poles move only, the root scales on grips only)", hpx_mode_name(mode));
        return false;
    }
    bool hands = false;
    for (int i = 0; i < n; i++) hands |= hp_rig_ctrl(ids[i])->kind != HPK_ROOT;
    HpTarget t = hands ? target_for_hands() : target();
    if (!hp_target_valid(db(), t)) return false;
    // Hand controls on a case need its key (target_for_hands added it): recompute the frames.
    hp_rig_world(db(), t, &E.ctx, &E.defs, E.world);
    Xform start[HP_RIG_CTRLS];
    int act = 0;
    for (int i = 0; i < n; i++) {
        start[i] = E.world[ids[i]];
        if (ids[i] == E.active) act = i;
    }
    quat basis[HPX_ORIENT_COUNT];
    for (int o = 0; o < HPX_ORIENT_COUNT; o++) basis[o] = orient_basis(E.active >= 0 ? E.active : ids[0], o);
    hpx_begin(&E.xf, mode, mouse, n, ids, start, act, basis, E.orient, E.pivot);
    E.xf_drag = drag;
    E.xf_ctx = E.ctx;
    E.xf_target = t;
    for (int i = 0; i < HP_RIG_CTRLS; i++) {
        const HpCtrl *c = hp_rig_ctrl(i);
        HpLocal l = c->kind == HPK_FINGER ? hp_rig_local(db(), t, i, &E.defs) : (HpLocal){ { 0 } };
        E.xf_j0[i] = (HandJoint){ l.rot_deg.x, l.rot_deg.y, l.rot_deg.z };
    }
    if (!E.xf_snap) E.xf_snap = (HpUndoSnap *)malloc(sizeof(HpUndoSnap));
    snap_take(E.xf_snap, t);
    begin_edit(t, 0);
    hp_undo_break(&E.undo);
    return true;
}

static void xform_confirm(void) {
    if (!hpx_active(&E.xf)) return;
    apply_xform_result();
    hpx_status(&E.xf, E.xf_status, sizeof(E.xf_status));
    hpx_end(&E.xf);
    end_edit(E.xf_target);
    status("%s", E.xf_status);
}

static void xform_cancel(void) {
    if (!hpx_active(&E.xf)) return;
    hpx_cancel(&E.xf);
    snap_restore(E.xf_snap);
    hp_undo_pop(&E.undo);
    status("transform cancelled");
}

static void xform_update(vec2 mouse, bool snap) {
    if (!hpx_active(&E.xf)) return;
    HpxView v = { project, NULL, E.cam_valid ? E.cam.pos : v3(0, 0, 0) };
    hpx_update(&E.xf, mouse, &v, snap);
    apply_xform_result();
    hpx_status(&E.xf, E.xf_status, sizeof(E.xf_status));
}

// ------------------------------------------------------------------ selection

static void select_only(int i) {
    memset(E.sel, 0, sizeof(E.sel));
    if (i >= 0) { E.sel[i] = true; E.active = i; }
}

static void select_click(int i, bool extend) {
    if (i < 0) { if (!extend) memset(E.sel, 0, sizeof(E.sel)); return; }
    if (extend) {
        if (E.sel[i] && E.active == i) E.sel[i] = false;
        else { E.sel[i] = true; E.active = i; }
    } else select_only(i);
}

static void select_rel(int which, bool extend) {
    if (E.active < 0) return;
    int out[HP_RIG_CTRLS], n = 0;
    if (which == 0) { int p = hp_rig_parent(E.active); if (p >= 0) out[n++] = p; }
    else if (which == 1) n = hp_rig_children(E.active, out, HP_RIG_CTRLS);
    else if (which == 2) n = hp_rig_siblings(E.active, out, HP_RIG_CTRLS);
    else { int m = hp_rig_mirror(E.active); if (m >= 0) out[n++] = m; }
    if (!n) return;
    if (!extend) memset(E.sel, 0, sizeof(E.sel));
    for (int k = 0; k < n; k++) E.sel[out[k]] = true;
    if (which != 2) E.active = out[0];
}

// ------------------------------------------------------------------ per frame (camera)

static void freeze_world(bool on) {
    Game *g = game();
    if (g && g->world) game_world_freeze(g->world, on, E.neutral_light);
}

static bool scene_cam(World *w, RenderCamera *cam) {
    double now = platform_time();
    float dt = E.last_time > 0 ? (float)fminf(0.1f, (float)(now - E.last_time)) : 0.0f;
    E.last_time = now;
    update_preview();
    // The grip is the same in every state: the state only places the item (preview choice).
    int fs = E.preview_state;
    const HandPoseCase *c = E.case_mode ? sel_case() : NULL;
    if (c && hp_case_hold_state() >= 0) fs = hp_case_hold_state();
    if (c && !case_edits_grip(c)) fs = -1;
    cvar_set_int(cvar_find("holdpoint_force_state"), fs);
    hp_case_frame(w, dt);
    const HandPoseEntry *te = NULL;
    HpTarget t = target();
    if (t.entry >= 0) te = &db()->e[t.entry];
    edit_defaults(te, &E.defs);
    E.ctx_valid = build_ctx(&E.ctx);
    if (E.ctx_valid) {
        if (hpx_active(&E.xf)) hp_rig_world(db(), E.xf_target, &E.ctx, &E.defs, E.world);
        else hp_rig_world(db(), t, &E.ctx, &E.defs, E.world);
        draw_rig();
        draw_onion();
        draw_gizmo();
    }
    CharacterInst *ch = local_char();
    if (E.fp_view || !ch) { E.cam_valid = false; return false; }
    vec3 focus = E.ctx_valid ? (E.focus == 1 ? E.world[HP_RIG_ROOT].t : (E.active >= 0 ? E.world[E.active].t : E.world[HP_RIG_IK(1)].t)) : ch->root.t;
    Player *p = game_local_player();
    float yaw = p->sim.move.yaw + E.yaw;
    vec3 fwd = dir_from_angles(yaw, E.pitch);
    cam->pos = v3_madd(focus, fwd, -E.dist);
    cam->yaw = yaw;
    cam->pitch = E.pitch;
    cam->roll = 0;
    cam->fov_deg = E.fov;
    cam->vm_fov_deg = E.fov;
    cam->ads_t = 0;
    cam->znear = fminf(cam->znear > 0 ? cam->znear : 0.05f, 0.02f);
    E.cam = *cam;
    E.cam_valid = true;
    return true;
}

// ------------------------------------------------------------------ input

static bool key_down(int sc) { const bool *k = SDL_GetKeyboardState(NULL); return k && sc >= 0 && sc < SDL_SCANCODE_COUNT && k[sc]; }

static bool key_pressed(int sc) {
    for (int i = 0; i < E.key_inject_n; i++) if (E.key_inject[i] == sc) return true;
    return key_down(sc) && !E.keys_prev[sc];
}

static void keys_end_frame(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    for (int i = 0; k && i < SDL_SCANCODE_COUNT; i++) E.keys_prev[i] = k[i] ? 1 : 0;
    E.key_inject_n = 0;
}

static void save_target(void);
static void store_key(void);

static void handle_keys(struct nk_context *ctx) {
    if (nk_item_is_any_active(ctx)) return;   // typing in a field
    bool shift = key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT);
    bool ctrl = key_down(SDL_SCANCODE_LCTRL) || key_down(SDL_SCANCODE_RCTRL);
    bool alt = key_down(SDL_SCANCODE_LALT) || key_down(SDL_SCANCODE_RALT);
    if (hpx_active(&E.xf)) {
        if (key_pressed(SDL_SCANCODE_X)) hpx_axis(&E.xf, 0, shift);
        if (key_pressed(SDL_SCANCODE_Y)) hpx_axis(&E.xf, 1, shift);
        if (key_pressed(SDL_SCANCODE_Z)) hpx_axis(&E.xf, 2, shift);
        static const struct { int sc; char c; } num[] = {
            { SDL_SCANCODE_0, '0' }, { SDL_SCANCODE_1, '1' }, { SDL_SCANCODE_2, '2' }, { SDL_SCANCODE_3, '3' }, { SDL_SCANCODE_4, '4' },
            { SDL_SCANCODE_5, '5' }, { SDL_SCANCODE_6, '6' }, { SDL_SCANCODE_7, '7' }, { SDL_SCANCODE_8, '8' }, { SDL_SCANCODE_9, '9' },
            { SDL_SCANCODE_KP_0, '0' }, { SDL_SCANCODE_KP_1, '1' }, { SDL_SCANCODE_KP_2, '2' }, { SDL_SCANCODE_KP_3, '3' }, { SDL_SCANCODE_KP_4, '4' },
            { SDL_SCANCODE_KP_5, '5' }, { SDL_SCANCODE_KP_6, '6' }, { SDL_SCANCODE_KP_7, '7' }, { SDL_SCANCODE_KP_8, '8' }, { SDL_SCANCODE_KP_9, '9' },
            { SDL_SCANCODE_PERIOD, '.' }, { SDL_SCANCODE_KP_PERIOD, '.' }, { SDL_SCANCODE_MINUS, '-' }, { SDL_SCANCODE_KP_MINUS, '-' }, { SDL_SCANCODE_BACKSPACE, '\b' },
        };
        for (int i = 0; i < (int)ARRAY_COUNT(num); i++) if (key_pressed(num[i].sc)) hpx_char(&E.xf, num[i].c);
        if (key_pressed(SDL_SCANCODE_RETURN) || key_pressed(SDL_SCANCODE_KP_ENTER)) { xform_update(E.mouse, ctrl); xform_confirm(); }
        else if (key_pressed(SDL_SCANCODE_ESCAPE)) xform_cancel();
        else xform_update(E.mouse, ctrl);
        return;
    }
    if (ctrl && key_pressed(SDL_SCANCODE_Z)) { undo_redo(shift); return; }
    if (ctrl && key_pressed(SDL_SCANCODE_Y)) { undo_redo(true); return; }
    if (ctrl && key_pressed(SDL_SCANCODE_S)) { save_target(); return; }
    if (ctrl && key_pressed(SDL_SCANCODE_C) && E.active >= 0) {
        E.clip = hp_rig_local(db(), target(), E.active, &E.defs);
        E.clip_kind = hp_rig_ctrl(E.active)->kind;
        E.clip_valid = true;
        status("copied the transform of %s", hp_rig_ctrl(E.active)->name);
        return;
    }
    if (ctrl && key_pressed(SDL_SCANCODE_V) && E.active >= 0 && E.clip_valid) {
        if (hp_rig_ctrl(E.active)->kind != E.clip_kind) { status("paste: the copied transform is from another kind of control"); return; }
        HpTarget t = hp_rig_ctrl(E.active)->kind == HPK_ROOT ? target() : target_for_hands();
        begin_edit(t, 0);
        hp_rig_set_local(db(), t, E.active, &E.defs, E.clip);
        end_edit(t);
        status("pasted onto %s", hp_rig_ctrl(E.active)->name);
        return;
    }
    if (alt && (key_pressed(SDL_SCANCODE_G) || key_pressed(SDL_SCANCODE_R) || key_pressed(SDL_SCANCODE_S))) {
        int ch = key_pressed(SDL_SCANCODE_G) ? HPA_T : key_pressed(SDL_SCANCODE_R) ? HPA_R : HPA_S;
        HpTarget t = target();
        begin_edit(t, 0);
        int n = 0;
        for (int i = 0; i < HP_RIG_CTRLS; i++) if (E.sel[i]) n += hp_rig_clear(db(), t, i, ch) ? 1 : 0;
        end_edit(t);
        status("cleared %s on %d control(s)", ch == HPA_T ? "location" : ch == HPA_R ? "rotation" : "scale", n);
        return;
    }
    if (key_pressed(SDL_SCANCODE_G)) xform_begin(HPX_GRAB, E.mouse, false);
    else if (key_pressed(SDL_SCANCODE_R)) xform_begin(HPX_ROTATE, E.mouse, false);
    else if (key_pressed(SDL_SCANCODE_S)) xform_begin(HPX_SCALE, E.mouse, false);
    else if (key_pressed(SDL_SCANCODE_A)) {
        bool any = false;
        for (int i = 0; i < HP_RIG_CTRLS; i++) any |= E.sel[i];
        for (int i = 0; i < HP_RIG_CTRLS; i++) E.sel[i] = !any && !alt && ctrl_usable(i);
    }
    else if (key_pressed(SDL_SCANCODE_LEFTBRACKET)) select_rel(0, shift);
    else if (key_pressed(SDL_SCANCODE_RIGHTBRACKET)) select_rel(1, shift);
    else if (key_pressed(SDL_SCANCODE_SEMICOLON)) select_rel(2, true);
    else if (key_pressed(SDL_SCANCODE_M)) select_rel(3, shift);
    else if (key_pressed(SDL_SCANCODE_I)) store_key();
    else if (key_pressed(SDL_SCANCODE_F)) E.focus = 0;
    else if (key_pressed(SDL_SCANCODE_SPACE) && E.case_mode) hp_case_play(!hp_case_state()->playing);
    else if (key_pressed(SDL_SCANCODE_LEFT) && E.case_mode) hp_case_set_time(hp_case_state()->t - 1.0f / 60.0f);
    else if (key_pressed(SDL_SCANCODE_RIGHT) && E.case_mode) hp_case_set_time(hp_case_state()->t + 1.0f / 60.0f);
    else if (key_pressed(SDL_SCANCODE_1)) E.tool = TOOL_SELECT;
    else if (key_pressed(SDL_SCANCODE_2) || key_pressed(SDL_SCANCODE_W)) E.tool = TOOL_MOVE;
    else if (key_pressed(SDL_SCANCODE_3) || key_pressed(SDL_SCANCODE_E)) E.tool = TOOL_ROTATE;
    else if (key_pressed(SDL_SCANCODE_4)) E.tool = TOOL_SCALE;
}

static void handle_mouse(struct nk_context *ctx) {
    const struct nk_input *in = &ctx->input;
    if (!E.mouse_inject) E.mouse = v2(in->mouse.pos.x, in->mouse.pos.y);
    bool over_ui = ui_mouse_over_window();
    bool ctrl = key_down(SDL_SCANCODE_LCTRL) || key_down(SDL_SCANCODE_RCTRL);
    bool shift = key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT);
    bool alt = key_down(SDL_SCANCODE_LALT) || key_down(SDL_SCANCODE_RALT);
    if (hpx_active(&E.xf)) {
        if (E.xf_drag) {
            xform_update(E.mouse, ctrl);
            if (!nk_input_is_mouse_down(in, NK_BUTTON_LEFT)) xform_confirm();
            else if (nk_input_is_mouse_pressed(in, NK_BUTTON_RIGHT)) xform_cancel();
            return;
        }
        if (nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) { xform_update(E.mouse, ctrl); xform_confirm(); }
        else if (nk_input_is_mouse_pressed(in, NK_BUTTON_RIGHT)) xform_cancel();
        return;
    }
    // Orbit: middle / right drag or Alt + left drag; wheel zooms.
    bool orbit_btn = nk_input_is_mouse_down(in, NK_BUTTON_MIDDLE) || nk_input_is_mouse_down(in, NK_BUTTON_RIGHT) || (alt && nk_input_is_mouse_down(in, NK_BUTTON_LEFT));
    if (!orbit_btn) E.orbiting = false;
    else if (!E.orbiting && !over_ui &&
             (nk_input_is_mouse_pressed(in, NK_BUTTON_MIDDLE) || nk_input_is_mouse_pressed(in, NK_BUTTON_RIGHT) || (alt && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT))))
        E.orbiting = true;
    if (E.orbiting) {
        E.yaw -= in->mouse.delta.x * 0.006f;
        E.pitch = clampf(E.pitch - in->mouse.delta.y * 0.006f, -1.5f, 1.5f);
        return;
    }
    if (!over_ui && in->mouse.scroll_delta.y != 0) E.dist = clampf(E.dist * powf(0.9f, in->mouse.scroll_delta.y), 0.12f, 6.0f);
    if (over_ui) { E.hover = -1; return; }
    E.hover = pick(E.mouse);
    if (nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) {
        int ax = gizmo_pick(E.mouse);
        if (ax >= 0 && xform_begin(gizmo_mode(), E.mouse, true)) { hpx_axis(&E.xf, ax, false); return; }
        select_click(E.hover, shift);
    }
}

// ------------------------------------------------------------------ save / keys

static float glue_error(int hand) {
    if (!E.ctx_valid) return 0;
    return v3_dist(E.ctx.palm_frame[hand].t, E.world[HP_RIG_IK(hand)].t);
}

static bool hand_used(int h) {
    HpTarget t = target();
    if (t.entry >= 0) {
        if (!((db()->e[t.entry].hands >> h) & 1)) return false;
        if (h == 1) return true;
        // The support hand only where the hold style in use puts it on the item (the phone's
        // portrait style is one-handed, its landscape one two-handed).
        CharacterInst *ch = local_char();
        const HandPoseEntry *owner;
        const ItemSockets *k = held_sockets(&owner);
        if (!ch || !k || !k->has_left) return false;
        const HoldStyle *hs = proc_hold_style(&g_engine.anim->proc, ch->rt.proc.style);
        return hs && hs->left_hand;
    }
    const HandPoseCase *c = sel_case();
    if (!c || !((c->hands >> h) & 1)) return false;
    if (c->kind == HCK_INTERACT) {
        // The interaction uses one hand (or both): the ones the hand system reaches with.
        HandsFpTargets ft;
        return hands_fp_targets(&ft) && ft.ov_w[h] > 0.5f;
    }
    return c->kind != HCK_STUB;
}

static void save_target(void) {
    HandPoseDB *d = db();
    HpTarget t = target();
    if (!hp_target_valid(d, t)) { status("nothing to save"); return; }
    // Hand glue: refuse a grip / key whose IK targets the arms can't reach.
    const HandPoseCase *c = sel_case();
    // Grips and the cases whose hands the runtime solves onto the anchor (interactions, seats);
    // the ledge depends on the hang clip, the stubs have no runtime yet: warned only.
    bool check = E.ctx_valid && (t.entry >= 0 || (c && (c->kind == HCK_INTERACT || c->kind == HCK_VEHICLE) && t.key >= 0));
    for (int h = 0; h < 2 && check; h++) {
        if (!hand_used(h)) continue;
        float g = glue_error(h);
        if (g > GLUE_REFUSE_M) {
            status("not saved: the %s hand ends %.1f mm from its IK target (unreachable socket) - move the root / IK closer", h ? "right" : "left", g * 1000.0f);
            return;
        }
    }
    bool ok;
    char where[700], rel[200];
    if (t.entry >= 0) {
        HandPoseEntry *e = &d->e[t.entry];
        bool was = e->reviewed;
        e->reviewed = true;
        if (E.save_dir && E.save_dir->str[0]) { ok = hp_write_entry(e, E.save_dir->str, NULL); handpose_system_note_own_write(); snprintf(where, sizeof(where), "%s", E.save_dir->str); }
        else { ok = handpose_system_save(t.entry); snprintf(where, sizeof(where), "%s", handpose_system_src_dir() ? handpose_system_src_dir() : handpose_system_rt_dir()); }
        if (!ok) e->reviewed = was;
        else set_dirty(e->hash, false);
        handpose_entry_rel_path(e, "", rel, sizeof(rel));
    } else {
        HandPoseCase *cs = &d->c[t.ci];
        bool was = cs->reviewed;
        cs->reviewed = true;
        if (E.save_dir && E.save_dir->str[0]) { ok = hp_write_case(cs, E.save_dir->str, NULL); handpose_system_note_own_write(); snprintf(where, sizeof(where), "%s", E.save_dir->str); }
        else { ok = handpose_system_save_case(t.ci); snprintf(where, sizeof(where), "%s", handpose_system_src_dir() ? handpose_system_src_dir() : handpose_system_rt_dir()); }
        if (!ok) cs->reviewed = was;
        else set_dirty(cs->hash, false);
        handpose_case_rel_path(cs, "", rel, sizeof(rel));
    }
    hp_edit_commit(d);
    status("%s %s%s", ok ? "saved" : "cannot write", where, rel);
}

static void store_key(void) {
    int ci = sel_case_index();
    const HandPoseCase *c = sel_case();
    if (!E.case_mode || !c || case_edits_grip(c)) { status("keyframes: open a case (interaction, seat, ledge)"); return; }
    begin_edit(hp_target_case(ci, -1), 0);
    int k = handpose_case_key_at(db(), ci, hp_case_state()->t);
    end_edit(hp_target_case(ci, -1));
    if (k >= 0) status("keyframe at %.3f s (%d keys)", db()->c[ci].keys[k].t, db()->c[ci].key_count);
    else status("no room for another key (%d max)", HANDPOSE_CASE_KEYS);
}

static void select_case(int ci) {
    HandPoseDB *d = db();
    Game *g = game();
    if (ci < 0 || !g || !g->world) return;
    if (hpx_active(&E.xf)) xform_cancel();
    char why[160] = "";
    E.sel_case = d->c[ci].hash;
    E.case_mode = true;
    E.preview_for = 0;
    if (!hp_case_enter(g->world, ci, why, sizeof(why))) { status("case %s: %s", d->c[ci].id, why); return; }
    hp_undo_break(&E.undo);
    select_only(d->c[ci].kind == HCK_ITEM ? HP_RIG_IK(1) : HP_RIG_ROOT);
    E.focus = 1;
    E.dist = d->c[ci].kind == HCK_VEHICLE ? 1.4f : 1.1f;
    status("case %s (%s): timeline at the contact frame", d->c[ci].id, handpose_case_kind_name(d->c[ci].kind));
}

static void select_grip(int e) {
    Game *g = game();
    if (e < 0) return;
    if (hpx_active(&E.xf)) xform_cancel();
    if (E.case_mode && g) hp_case_leave(g->world);
    E.case_mode = false;
    E.sel_entry = db()->e[e].hash;
    E.preview_for = 0;
    hp_undo_break(&E.undo);
    if (E.active < 0) select_only(HP_RIG_IK(1));
}

// ------------------------------------------------------------------ panels

static void draw_list(struct nk_context *ctx) {
    HandPoseDB *d = db();
    float h = ui_height() * 0.52f;
    if (nk_begin(ctx, "HAND POSES", nk_rect(S(8), S(8), S(270), h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        nk_layout_row_dynamic(ctx, S(22), 2);
        if (nk_option_label(ctx, "Grips", E.list_tab == 0)) E.list_tab = 0;
        if (nk_option_label(ctx, "Cases", E.list_tab == 1)) E.list_tab = 1;
        nk_layout_row_dynamic(ctx, S(22), 2);
        if (E.list_tab == 0) {
            static const char *cats[HPC_COUNT + 1] = { "all" };
            for (int c = 0; c < HPC_ATTACHMENT + 1; c++) cats[c + 1] = handpose_category_name(c);
            E.cat = nk_combo(ctx, cats, HPC_ATTACHMENT + 2, E.cat + 1, (int)S(20), nk_vec2(S(150), S(160))) - 1;
        } else nk_label(ctx, "", NK_TEXT_LEFT);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, E.filter, sizeof(E.filter), nk_filter_default);
        nk_layout_row_dynamic(ctx, S(18), 2);
        E.f_unreviewed = nk_check_label(ctx, "unreviewed", E.f_unreviewed);
        E.f_edited = nk_check_label(ctx, "edited", E.f_edited);
        HandPoseScanReport r;
        hp_count(d, &r);
        char b[200];
        nk_layout_row_dynamic(ctx, S(16), 1);
        snprintf(b, sizeof(b), "%d grips, %d cases; %d unreviewed (*)", d->count, d->case_count, r.unreviewed);
        nk_label(ctx, b, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, h - S(150), 1);
        if (nk_group_begin(ctx, "entries", NK_WINDOW_BORDER)) {
            nk_layout_row_dynamic(ctx, S(17), 1);
            if (E.list_tab == 0) {
                for (int i = 0; i < d->count; i++) {
                    const HandPoseEntry *e = &d->e[i];
                    if (E.cat >= 0 && e->category != E.cat) continue;
                    if (e->category > HPC_ATTACHMENT) continue;
                    if (E.f_unreviewed && e->reviewed) continue;
                    if (E.f_edited && !is_dirty(e->hash)) continue;
                    if (E.filter[0] && !strstr(e->id, E.filter) && !strstr(e->label, E.filter)) continue;
                    snprintf(b, sizeof(b), "%c%c%c %s", e->reviewed ? ' ' : '*', handpose_entry_has_overrides(e) ? '+' : ' ', is_dirty(e->hash) ? '!' : ' ', e->id);
                    nk_bool on = !E.case_mode && e->hash == E.sel_entry;
                    if (nk_selectable_label(ctx, b, NK_TEXT_LEFT, &on) && on) select_grip(i);
                }
            } else {
                for (int i = 0; i < d->case_count; i++) {
                    const HandPoseCase *c = &d->c[i];
                    if (E.f_unreviewed && c->reviewed) continue;
                    if (E.f_edited && !is_dirty(c->hash)) continue;
                    if (E.filter[0] && !strstr(c->id, E.filter) && !strstr(c->label, E.filter)) continue;
                    snprintf(b, sizeof(b), "%c%c%c %-20s %s", c->reviewed ? ' ' : '*', c->key_count ? '+' : ' ', is_dirty(c->hash) ? '!' : ' ', c->id, handpose_case_kind_name(c->kind));
                    nk_bool on = E.case_mode && c->hash == E.sel_case;
                    if (nk_selectable_label(ctx, b, NK_TEXT_LEFT, &on) && on) select_case(i);
                }
            }
            nk_group_end(ctx);
        }
    }
    nk_end(ctx);
}

static void outliner_row(struct nk_context *ctx, int i, int depth) {
    const HpCtrl *c = hp_rig_ctrl(i);
    char b[64];
    snprintf(b, sizeof(b), "%*s%s%s", depth * 3, "", c->name, ctrl_usable(i) ? "" : " (-)");
    nk_bool on = E.sel[i];
    if (nk_selectable_label(ctx, b, NK_TEXT_LEFT, &on)) {
        bool extend = key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT);
        if (ctrl_usable(i)) select_click(i, extend);
    }
}

static void draw_outliner(struct nk_context *ctx) {
    float y = ui_height() * 0.52f + S(16);
    float h = ui_height() - y - S(8);
    if (nk_begin(ctx, "OUTLINER", nk_rect(S(8), y, S(270), h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        nk_layout_row_dynamic(ctx, S(16), 1);
        HpTarget t = target();
        char b[160];
        if (t.entry >= 0) snprintf(b, sizeof(b), "Root(%s)", db()->e[t.entry].id);
        else if (t.ci >= 0) snprintf(b, sizeof(b), "Root(anchor of %s)%s", db()->c[t.ci].id, t.key >= 0 ? "" : "  no key here");
        else snprintf(b, sizeof(b), "(nothing selected)");
        nk_label_colored(ctx, b, NK_TEXT_LEFT, nk_rgb(255, 190, 90));
        nk_layout_row_dynamic(ctx, S(17), 1);
        outliner_row(ctx, HP_RIG_ROOT, 0);
        static const char *hn[2] = { "left hand", "right hand" };
        for (int hd = 0; hd < 2; hd++) {
            outliner_row(ctx, HP_RIG_IK(hd), 1);
            if (nk_tree_push_id(ctx, NK_TREE_NODE, hn[hd], NK_MINIMIZED, hd)) {
                nk_layout_row_dynamic(ctx, S(17), 1);
                for (int f = 0; f < HAND_FINGERS; f++) {
                    // A whole finger at once.
                    snprintf(b, sizeof(b), "      %s (finger)", handpose_finger_name(f));
                    bool all = true;
                    for (int j = 0; j < 3; j++) all &= E.sel[HP_RIG_FINGER(hd, f, j)];
                    nk_bool on = all;
                    if (nk_selectable_label(ctx, b, NK_TEXT_LEFT, &on)) {
                        if (!(key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT))) memset(E.sel, 0, sizeof(E.sel));
                        hp_rig_select_finger(E.sel, hd, f, on);
                        E.active = HP_RIG_FINGER(hd, f, 0);
                    }
                    for (int j = 0; j < HAND_FINGER_JOINTS; j++) outliner_row(ctx, HP_RIG_FINGER(hd, f, j), 3);
                }
                nk_tree_pop(ctx);
            }
        }
        nk_layout_row_dynamic(ctx, S(17), 1);
        outliner_row(ctx, HP_RIG_POLE(0), 1);
        outliner_row(ctx, HP_RIG_POLE(1), 1);
        nk_layout_row_dynamic(ctx, S(22), 2);
        if (nk_button_label(ctx, "whole hand L")) { memset(E.sel, 0, sizeof(E.sel)); hp_rig_select_hand(E.sel, 0, true); E.active = HP_RIG_IK(0); }
        if (nk_button_label(ctx, "whole hand R")) { memset(E.sel, 0, sizeof(E.sel)); hp_rig_select_hand(E.sel, 1, true); E.active = HP_RIG_IK(1); }
        nk_layout_row_dynamic(ctx, S(14), 1);
        nk_label(ctx, "[ parent  ] child  ; siblings  M mirror  A all", NK_TEXT_LEFT);
    }
    nk_end(ctx);
}

static void draw_toolbar(struct nk_context *ctx) {
    float x = S(286), w = ui_width() - S(286) - S(348);
    if (nk_begin(ctx, "hp_toolbar", nk_rect(x, S(8), w, S(64)), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, S(24), 8);
        for (int t = 0; t < 4; t++) if (nk_option_label(ctx, k_tool_names[t], E.tool == t)) E.tool = t;
        static const char *ori[HPX_ORIENT_COUNT];
        for (int o = 0; o < HPX_ORIENT_COUNT; o++) ori[o] = hpx_orient_name(o);
        E.orient = nk_combo(ctx, ori, HPX_ORIENT_COUNT, E.orient, (int)S(20), nk_vec2(S(120), S(120)));
        static const char *piv[HPX_PIVOT_COUNT];
        for (int p = 0; p < HPX_PIVOT_COUNT; p++) piv[p] = hpx_pivot_name(p);
        E.pivot = nk_combo(ctx, piv, HPX_PIVOT_COUNT, E.pivot, (int)S(20), nk_vec2(S(160), S(100)));
        E.mirror_live = nk_check_label(ctx, "mirror", E.mirror_live);
        bool nl = nk_check_label(ctx, "neutral light", E.neutral_light);
        if (nl != E.neutral_light) {
            E.neutral_light = nl;
            Game *g = game();
            if (g && g->world) game_world_freeze_lighting(g->world, nl);
        }
        nk_layout_row_dynamic(ctx, S(18), 1);
        char b[300];
        if (hpx_active(&E.xf)) snprintf(b, sizeof(b), "%s   (X/Y/Z axis, Shift+axis plane, Ctrl snap, type a value, Enter / click: confirm, Esc / right click: cancel)", E.xf_status);
        else snprintf(b, sizeof(b), "%s", E.status[0] ? E.status : "G move  R rotate  S scale  Alt+G/R/S clear  I keyframe  Ctrl+Z / Ctrl+Shift+Z  Ctrl+S save  MMB / RMB orbit");
        nk_label(ctx, b, NK_TEXT_LEFT);
    }
    nk_end(ctx);
}

static bool prop3(struct nk_context *ctx, const char *names[3], vec3 *v, float lo, float hi, float step, const bool lock[3]) {
    vec3 o = *v;
    float *f[3] = { &v->x, &v->y, &v->z };
    for (int i = 0; i < 3; i++) {
        if (lock && lock[i]) {
            char b[48];
            snprintf(b, sizeof(b), "%s %.4f (locked)", names[i] + 1, *f[i]);
            nk_label(ctx, b, NK_TEXT_LEFT);
        } else nk_property_float(ctx, names[i], lo, f[i], hi, step, step * 0.25f);
    }
    return memcmp(&o, v, sizeof(o)) != 0;
}

static void draw_npanel(struct nk_context *ctx) {
    HandPoseDB *d = db();
    float w = S(340);
    if (nk_begin(ctx, "ITEM / TRANSFORM", nk_rect(ui_width() - w - S(8), S(8), w, ui_height() - S(16)),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE)) {
        char b[260];
        HpTarget t = target();
        nk_layout_row_dynamic(ctx, S(17), 1);
        if (t.entry >= 0) {
            const HandPoseEntry *e = &d->e[t.entry];
            snprintf(b, sizeof(b), "%s  (%s)%s", e->id, handpose_category_name(e->category), e->reviewed ? "" : "  unreviewed");
            nk_label(ctx, b, NK_TEXT_LEFT);
            snprintf(b, sizeof(b), "inherits %s; universal grip (every state)", e->inherits[0] ? e->inherits : "-");
            nk_label(ctx, b, NK_TEXT_LEFT);
        } else if (t.ci >= 0) {
            const HandPoseCase *c = &d->c[t.ci];
            snprintf(b, sizeof(b), "case %s  (%s)%s", c->id, handpose_case_kind_name(c->kind), c->reviewed ? "" : "  unreviewed");
            nk_label(ctx, b, NK_TEXT_LEFT);
            snprintf(b, sizeof(b), "bind %s", c->bind[0] ? c->bind : "-");
            nk_label(ctx, b, NK_TEXT_LEFT);
        } else {
            nk_label(ctx, "select a grip or a case", NK_TEXT_LEFT);
            nk_end(ctx);
            return;
        }
        if (E.preview_text[0]) nk_label_colored(ctx, E.preview_text, NK_TEXT_LEFT, nk_rgb(140, 200, 255));
        // Hand glue (refused on save beyond 5 mm).
        for (int h = 0; h < 2 && E.ctx_valid; h++) {
            if (!hand_used(h)) continue;
            float g = glue_error(h);
            snprintf(b, sizeof(b), "glue %s: %.2f mm%s", h ? "R" : "L", g * 1000.0f, g > GLUE_REFUSE_M ? "  UNREACHABLE (save refused)" : "");
            nk_label_colored(ctx, b, NK_TEXT_LEFT, g > GLUE_REFUSE_M ? nk_rgb(255, 90, 90) : nk_rgb(150, 220, 150));
        }
        nk_layout_row_dynamic(ctx, S(24), 4);
        if (nk_button_label(ctx, "Save")) save_target();
        if (nk_button_label(ctx, "Undo")) undo_redo(false);
        if (nk_button_label(ctx, "Redo")) undo_redo(true);
        if (nk_button_label(ctx, "Leave")) { scene_leave(); nk_end(ctx); return; }
        if (!E.case_mode) {
            nk_layout_row_dynamic(ctx, S(22), 2);
            nk_label(ctx, "preview state", NK_TEXT_LEFT);
            static const char *st[HP_STATE_COUNT];
            for (int s = 0; s < HP_STATE_COUNT; s++) st[s] = holdpoint_state_name(s);
            E.preview_state = nk_combo(ctx, st, HP_STATE_COUNT, E.preview_state, (int)S(20), nk_vec2(S(180), S(260)));
        }
        // ---- the active control
        int a = E.active;
        if (a >= 0 && nk_tree_push(ctx, NK_TREE_TAB, "Transform", NK_MAXIMIZED)) {
            const HpCtrl *c = hp_rig_ctrl(a);
            nk_layout_row_dynamic(ctx, S(18), 1);
            snprintf(b, sizeof(b), "%s%s", c->name, ctrl_usable(a) ? "" : "  (not editable here)");
            nk_label_colored(ctx, b, NK_TEXT_LEFT, nk_rgb(255, 255, 255));
            HpTarget et = c->kind == HPK_ROOT ? t : t;
            HpLocal l = hp_rig_local(d, et, a, &E.defs);
            HpLocal o = l;
            bool lock_loc[3], lock_rot[3];
            for (int i = 0; i < 3; i++) { lock_loc[i] = (E.locks[a] >> i) & 1; lock_rot[i] = (E.locks[a] >> (3 + i)) & 1; }
            bool changed = false;
            u32 key = 0x9000u + (u32)a;
            if (c->allow & HPA_T) {
                static const char *ln[3] = { "#Location X", "#Location Y", "#Location Z" };
                nk_layout_row_dynamic(ctx, S(20), 1);
                changed |= prop3(ctx, ln, &l.loc, -2.0f, 2.0f, c->kind == HPK_POLE ? 0.01f : 0.001f, lock_loc);
            }
            if (c->allow & HPA_R) {
                if (c->kind == HPK_FINGER) {
                    static const char *rn[3] = { "#Rotation X (curl)", "#Rotation Y (spread)", "#Rotation Z (twist)" };
                    HandJoint lo, hi;
                    handpose_joint_limits(c->finger, c->joint, &lo, &hi);
                    nk_layout_row_dynamic(ctx, S(20), 1);
                    float *f[3] = { &l.rot_deg.x, &l.rot_deg.y, &l.rot_deg.z };
                    float los[3] = { lo.curl, lo.spread, lo.twist }, his[3] = { hi.curl, hi.spread, hi.twist };
                    for (int i = 0; i < 3; i++) nk_property_float(ctx, rn[i], los[i], f[i], his[i], 0.5f, 0.2f);
                    changed |= memcmp(&l.rot_deg, &o.rot_deg, sizeof(vec3)) != 0;
                    snprintf(b, sizeof(b), "limits: curl %.0f..%.0f, spread %.0f..%.0f, twist %.0f..%.0f", lo.curl, hi.curl, lo.spread, hi.spread, lo.twist, hi.twist);
                    nk_layout_row_dynamic(ctx, S(16), 1);
                    nk_label(ctx, b, NK_TEXT_LEFT);
                } else {
                    static const char *rn[3] = { "#Rotation X", "#Rotation Y", "#Rotation Z" };
                    nk_layout_row_dynamic(ctx, S(20), 1);
                    if (prop3(ctx, rn, &l.rot_deg, -180.0f, 180.0f, 0.25f, lock_rot)) { l.rot = quat_norm(handpose_euler_deg_to_quat(l.rot_deg)); changed = true; }
                    nk_layout_row_dynamic(ctx, S(16), 1);
                    snprintf(b, sizeof(b), "quaternion %.4f %.4f %.4f %.4f", l.rot.x, l.rot.y, l.rot.z, l.rot.w);
                    nk_label(ctx, b, NK_TEXT_LEFT);
                }
            }
            if ((c->allow & HPA_S) && t.entry >= 0) {
                nk_layout_row_dynamic(ctx, S(20), 1);
                nk_property_float(ctx, "#Scale (uniform)", HANDPOSE_ROOT_SCALE_MIN, &l.scale, HANDPOSE_ROOT_SCALE_MAX, 0.005f, 0.002f);
                changed |= l.scale != o.scale;
            }
            if (changed && ctrl_usable(a)) {
                HpTarget wt = c->kind == HPK_ROOT ? t : target_for_hands();
                begin_edit(wt, key);
                hp_rig_set_local(d, wt, a, &E.defs, l);
                end_edit(wt);
            }
            // Locks, reset, copy / paste.
            nk_layout_row_dynamic(ctx, S(20), 4);
            nk_label(ctx, "lock", NK_TEXT_LEFT);
            nk_bool lk = (E.locks[a] & HPL_LOCK_LOC) != 0;
            if (nk_checkbox_label(ctx, "loc", &lk)) E.locks[a] = (u8)((E.locks[a] & ~HPL_LOCK_LOC) | (lk ? HPL_LOCK_LOC : 0));
            nk_bool lr = (E.locks[a] & HPL_LOCK_ROT) != 0;
            if (nk_checkbox_label(ctx, "rot", &lr)) E.locks[a] = (u8)((E.locks[a] & ~HPL_LOCK_ROT) | (lr ? HPL_LOCK_ROT : 0));
            nk_bool ls = (E.locks[a] & HPL_LOCK_SCALE) != 0;
            if (nk_checkbox_label(ctx, "scale", &ls)) E.locks[a] = (u8)((E.locks[a] & ~HPL_LOCK_SCALE) | (ls ? HPL_LOCK_SCALE : 0));
            nk_layout_row_dynamic(ctx, S(22), 3);
            int clr = 0;
            if (nk_button_label(ctx, "reset loc")) clr = HPA_T;
            if (nk_button_label(ctx, "reset rot")) clr = HPA_R;
            if (nk_button_label(ctx, "reset scale")) clr = HPA_S;
            if (clr) {
                begin_edit(t, 0);
                hp_rig_clear(d, t, a, clr);
                end_edit(t);
            }
            nk_layout_row_dynamic(ctx, S(22), 2);
            if (nk_button_label(ctx, "copy transform")) { E.clip = hp_rig_local(d, t, a, &E.defs); E.clip_kind = c->kind; E.clip_valid = true; }
            if (nk_button_label(ctx, "paste transform") && E.clip_valid && E.clip_kind == c->kind) {
                HpTarget wt = c->kind == HPK_ROOT ? t : target_for_hands();
                begin_edit(wt, 0);
                hp_rig_set_local(d, wt, a, &E.defs, E.clip);
                end_edit(wt);
            }
            // Finger shortcut: the curl of the whole finger.
            if (c->kind == HPK_FINGER && hp_target_has_layers(d, t)) {
                HandPose p;
                hp_target_resolve(d, t, c->hand, &p);
                float curl = handpose_finger_curl(p.j, c->finger), c0 = curl;
                nk_layout_row_dynamic(ctx, S(20), 1);
                snprintf(b, sizeof(b), "#%s curl (3 joints)", handpose_finger_name(c->finger));
                nk_property_float(ctx, b, -20.0f, &curl, 120.0f, 1.0f, 0.4f);
                if (curl != c0) {
                    HpTarget wt = target_for_hands();
                    begin_edit(wt, 0xA000u + (u32)c->finger);
                    hp_edit_set_curl(d, wt, c->hand, c->finger, curl);
                    end_edit(wt);
                }
            }
            nk_tree_pop(ctx);
        }
        // ---- hand: presets, mirror, IK weight
        const HpCtrl *ac = a >= 0 ? hp_rig_ctrl(a) : NULL;
        int hand = ac && ac->hand >= 0 ? ac->hand : 1;
        if (hp_target_valid(d, t) && nk_tree_push(ctx, NK_TREE_TAB, "Hand", NK_MINIMIZED)) {
            static const char *presets[32];
            int pc = DG_MIN(handpose_preset_count(), 32);
            for (int k = 0; k < pc; k++) presets[k] = handpose_preset_name(k);
            nk_layout_row_dynamic(ctx, S(24), 2);
            E.preset = nk_combo(ctx, presets, pc, DG_CLAMP(E.preset, 0, pc - 1), (int)S(20), nk_vec2(S(180), S(260)));
            if (nk_button_label(ctx, hand ? "preset -> R" : "preset -> L")) {
                HpTarget wt = target_for_hands();
                begin_edit(wt, 0);
                hp_edit_preset(d, wt, hand, presets[E.preset]);
                end_edit(wt);
            }
            nk_layout_row_dynamic(ctx, S(24), 1);
            snprintf(b, sizeof(b), "mirror %s -> %s", hand ? "R" : "L", hand ? "L" : "R");
            if (nk_button_label(ctx, b)) {
                HpTarget wt = target_for_hands();
                begin_edit(wt, 0);
                hp_edit_mirror(d, wt, hand);
                end_edit(wt);
            }
            if (hp_target_has_layers(d, t)) {
                HandPose p;
                hp_target_resolve(d, t, hand, &p);
                float ikw = p.ik_weight;
                nk_layout_row_dynamic(ctx, S(20), 1);
                nk_property_float(ctx, "#IK weight", 0, &ikw, 1, 0.01f, 0.005f);
                if (ikw != p.ik_weight) {
                    begin_edit(t, 0x400);
                    hp_edit_touch(d, t, hand, HPL_IKW, NULL)->ik_weight = saturate(ikw);
                    end_edit(t);
                }
            }
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "View", NK_MINIMIZED)) {
            nk_layout_row_dynamic(ctx, S(22), 2);
            if (nk_option_label(ctx, "orbit", !E.fp_view)) E.fp_view = false;
            if (nk_option_label(ctx, "first person", E.fp_view)) E.fp_view = true;
            if (nk_option_label(ctx, "focus control", E.focus == 0)) E.focus = 0;
            if (nk_option_label(ctx, "focus root", E.focus == 1)) E.focus = 1;
            nk_layout_row_dynamic(ctx, S(20), 2);
            float yd = E.yaw * DG_RAD2DEG, pd = E.pitch * DG_RAD2DEG;
            nk_property_float(ctx, "#yaw", -360, &yd, 360, 1, 0.5f);
            nk_property_float(ctx, "#pitch", -85, &pd, 85, 1, 0.5f);
            nk_property_float(ctx, "#dist", 0.12f, &E.dist, 6, 0.01f, 0.005f);
            nk_property_float(ctx, "#fov", 15, &E.fov, 90, 1, 0.5f);
            E.yaw = yd * DG_DEG2RAD;
            E.pitch = pd * DG_DEG2RAD;
            nk_layout_row_dynamic(ctx, S(20), 1);
            E.onion = nk_check_label(ctx, "onion skin (neighbouring keys)", E.onion);
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

static void draw_timeline(struct nk_context *ctx) {
    HandPoseDB *d = db();
    int ci = sel_case_index();
    if (!E.case_mode || ci < 0) return;
    HandPoseCase *c = &d->c[ci];
    const HpCaseState *cs = hp_case_state();
    float x = S(286), w = ui_width() - S(286) - S(348), h = S(178);
    if (nk_begin(ctx, "TIMELINE", nk_rect(x, ui_height() - h - S(8), w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_MINIMIZABLE)) {
        char b[200];
        nk_layout_row_dynamic(ctx, S(24), 9);
        if (nk_button_label(ctx, "|<")) hp_case_set_time(0);
        if (nk_button_label(ctx, "<")) hp_case_set_time(cs->t - 1.0f / 60.0f);
        if (nk_button_label(ctx, cs->playing ? "pause" : "play")) hp_case_play(!cs->playing);
        if (nk_button_label(ctx, ">")) hp_case_set_time(cs->t + 1.0f / 60.0f);
        if (nk_button_label(ctx, ">|")) hp_case_set_time(c->duration);
        hp_case_set_loop(nk_check_label(ctx, "loop", cs->loop));
        float sp = cs->speed;
        nk_property_float(ctx, "#speed", 0.05f, &sp, 4.0f, 0.05f, 0.02f);
        if (sp != cs->speed) hp_case_set_speed(sp);
        if (nk_button_label(ctx, "key (I)")) store_key();
        int k = key_at_time(c, cs->t);
        if (nk_button_label(ctx, "del key") && k >= 0) {
            begin_edit(hp_target_case(ci, -1), 0);
            handpose_case_key_remove(c, k);
            end_edit(hp_target_case(ci, -1));
        }
        // Scrub (the reload's clip owns the time: view only).
        float t = cs->t;
        nk_layout_row_dynamic(ctx, S(22), 1);
        if (cs->view_only) {
            snprintf(b, sizeof(b), "reload clip %.2f / %.2f s (view only: play runs the real reload)", cs->t, c->duration);
            nk_label(ctx, b, NK_TEXT_LEFT);
        } else {
            nk_slider_float(ctx, 0.0f, &t, c->duration, 0.001f);
            if (t != cs->t) hp_case_set_time(t);
        }
        // Keys + markers on a strip.
        struct nk_rect r = nk_widget_bounds(ctx);
        nk_layout_row_dynamic(ctx, S(16), 1);
        nk_label(ctx, "", NK_TEXT_LEFT);
        struct nk_command_buffer *out = nk_window_get_canvas(ctx);
        float x0 = r.x, x1 = r.x + r.w;
        nk_fill_rect(out, nk_rect(x0, r.y, r.w, S(14)), 0, nk_rgb(40, 40, 46));
        float mk[3] = { c->contact, c->use, c->release };
        struct nk_color mc[3] = { nk_rgb(90, 220, 120), nk_rgb(230, 210, 90), nk_rgb(230, 110, 90) };
        for (int m = 0; m < 3; m++) {
            float px = x0 + (x1 - x0) * (mk[m] / fmaxf(c->duration, 1e-3f));
            nk_fill_rect(out, nk_rect(px - 1, r.y, 2, S(14)), 0, mc[m]);
        }
        for (int q = 0; q < c->key_count; q++) {
            float px = x0 + (x1 - x0) * (c->keys[q].t / fmaxf(c->duration, 1e-3f));
            nk_fill_rect(out, nk_rect(px - S(4), r.y + S(3), S(8), S(8)), S(2), q == k ? nk_rgb(255, 255, 255) : nk_rgb(200, 160, 255));
        }
        float pxc = x0 + (x1 - x0) * (cs->t / fmaxf(c->duration, 1e-3f));
        nk_fill_rect(out, nk_rect(pxc - 1, r.y - S(2), 2, S(18)), 0, nk_rgb(90, 160, 255));
        // Markers (editable) and the key list.
        nk_layout_row_dynamic(ctx, S(20), 4);
        float mc0 = c->contact, mu0 = c->use, mr0 = c->release, du0 = c->duration;
        nk_property_float(ctx, "#contact", 0.0f, &c->contact, c->duration, 0.01f, 0.004f);
        nk_property_float(ctx, "#use", c->contact, &c->use, c->release, 0.01f, 0.004f);
        nk_property_float(ctx, "#release", c->contact, &c->release, c->duration, 0.01f, 0.004f);
        nk_property_float(ctx, "#duration", fmaxf(0.05f, c->release), &c->duration, 10.0f, 0.01f, 0.004f);
        if (mc0 != c->contact || mu0 != c->use || mr0 != c->release || du0 != c->duration) {
            float nc = c->contact, nu = c->use, nr = c->release, nd = c->duration;
            c->contact = mc0; c->use = mu0; c->release = mr0; c->duration = du0;
            begin_edit(hp_target_case(ci, -1), 0x7000u);
            c->contact = nc; c->use = clampf(nu, nc, nr); c->release = clampf(nr, nc, nd); c->duration = nd;
            end_edit(hp_target_case(ci, -1));
        }
        nk_layout_row_dynamic(ctx, S(16), 1);
        int n = snprintf(b, sizeof(b), "t %.3f s  %s  keys:", cs->t, k >= 0 ? "(on a key)" : "");
        for (int q = 0; q < c->key_count && n < (int)sizeof(b) - 24; q++) n += snprintf(b + n, sizeof(b) - (size_t)n, " %s@%.2f", c->keys[q].name, c->keys[q].t);
        nk_label(ctx, b, NK_TEXT_LEFT);
    }
    nk_end(ctx);
}

static void scene_draw_ui(void) {
    if (!db() || !game() || !game()->world) return;
    struct nk_context *ctx = ui_nk();
    handle_mouse(ctx);
    handle_keys(ctx);
    if (!nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT) && !hpx_active(&E.xf)) hp_undo_break(&E.undo);   // one undo step per drag
    draw_list(ctx);
    draw_outliner(ctx);
    draw_toolbar(ctx);
    draw_npanel(ctx);
    draw_timeline(ctx);
    keys_end_frame();
    E.mouse_inject = false;
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
    E.case_mode = false;
    memset(E.keys_prev, 1, sizeof(E.keys_prev));   // keys held while opening are not presses
    if (sel_entry_index() < 0) {
        HandPoseDB *d = db();
        for (int i = 0; i < d->count && !E.sel_entry; i++) if (d->e[i].category == HPC_WEAPON && !d->e[i].reviewed) E.sel_entry = d->e[i].hash;
        if (!E.sel_entry && d->count) E.sel_entry = d->e[0].hash;
    }
    select_only(HP_RIG_IK(1));
    freeze_world(true);
    return true;
}

static void scene_leave_fn(void) {
    if (hpx_active(&E.xf)) xform_cancel();
    Game *g = game();
    if (g) hp_case_leave(g->world);
    restore_slot();
    freeze_world(false);
    cvar_set_int(cvar_find("holdpoint_force_state"), E.saved_force);
    cvar_set_int(cvar_find("holdpoint_static"), E.saved_static);
    E.case_mode = false;
    if (E.dirty_count) log_warn("scene handpose: %d edited grip(s) / case(s) not saved (they stay in memory until handpose_reload)", E.dirty_count);
}

static const GameScene k_scene = {
    .name = "handpose",
    .desc = "hand pose editor: universal grips and simulated cases on a control rig (docs/HANDPOSE.md)",
    .enter = scene_enter_fn,
    .leave = scene_leave_fn,
    .camera = scene_cam,
    .draw = scene_draw_ui,
    .free_cursor = true,
    .hide_hud = true,
};

// ------------------------------------------------------------------ hpe command (scripts / tests)

static void dump_target(void) {
    HpTarget t = target();
    HandPoseDB *d = db();
    if (!hp_target_valid(d, t)) { log_info("hpe: nothing selected"); return; }
    Xform r = hp_target_root(d, t);
    log_info("hpe: %s%s key %d, root pos %.4f %.4f %.4f rot %.4f %.4f %.4f %.4f scale %.3f", t.entry >= 0 ? "grip " : "case ", t.entry >= 0 ? d->e[t.entry].id : d->c[t.ci].id,
             t.key, r.t.x, r.t.y, r.t.z, r.r.x, r.r.y, r.r.z, r.r.w, r.s.x);
    HandsFpTargets ft;
    if (E.case_mode && hands_fp_targets(&ft))
        log_info("hpe:   interaction weights L %.2f R %.2f, case %08x / %08x at %.3f s", ft.ov_w[0], ft.ov_w[1], ft.ov_case[0], ft.ov_case[1], ft.ov_case_t[1]);
    for (int h = 0; h < 2; h++) {
        if (!E.ctx_valid) break;
        Xform ik = E.world[HP_RIG_IK(h)];
        log_info("hpe:   IK_%s world %.4f %.4f %.4f  glue %.2f mm", h ? "R" : "L", ik.t.x, ik.t.y, ik.t.z, glue_error(h) * 1000.0f);
    }
    if (hp_target_has_layers(d, t))
        for (int h = 0; h < 2; h++) {
            HandPose p;
            hp_target_resolve(d, t, h, &p);
            log_info("hpe:   %s: set %04x ik %.4f %.4f %.4f, index curl %.2f %.2f %.2f, thumb %.2f", h ? "right" : "left", hp_target_layer(d, t, h)->set, p.socket.t.x, p.socket.t.y, p.socket.t.z,
                     p.j[HF_INDEX][0].curl, p.j[HF_INDEX][1].curl, p.j[HF_INDEX][2].curl, p.j[HF_THUMB][0].curl);
        }
}

// World freeze check (tests): the time of day, the weather and every zombie / NPC / vehicle
// position after N ticks equal the snapshot.
static void freeze_snapshot(bool check) {
    Game *g = game();
    World *w = g ? g->world : NULL;
    if (!w) return;
    double days = tod_days(&w->tod, (double)w->tick);
    // The authoritative weather at this tick (+ the integrated surface state).
    WeatherParams wx;
    memset(&wx, 0, sizeof(wx));
    if (w->weather.version) weather_eval(&w->weather, w->tick, 1.0f / 60.0f, &wx);
    wx.v[0] += w->weather.wetness * 1000.0f + w->weather.puddles * 100.0f + w->weather.snow * 10.0f;
    vec3 pos[64];
    int idx[64];
    int n = 0;
    ENT_FOREACH(w, i) {
        if (n >= 64) break;
        EntityType ty = (EntityType)w->type[i];
        if (ty == ET_PLAYER) continue;
        if (ty == ET_ZOMBIE || ty == ET_VEHICLE || ty == ET_PROP || ty == ET_DUMMY) { idx[n] = i; pos[n++] = w->xf[i].pos; }
    }
    if (!check) {
        E.fz_valid = true;
        E.fz_days = days;
        E.fz_wx = wx;
        memcpy(E.fz_pos, pos, sizeof(pos));
        E.fz_n = n;
        E.fz_tick = w->tick;
        log_info("hpe freeze: snapshot at tick %u (%d moving entities, %.4f days)", w->tick, n, days);
        return;
    }
    if (!E.fz_valid) { log_warn("hpe freeze: no snapshot (hpe freeze begin)"); return; }
    float moved = 0;
    int worst = -1;
    for (int k = 0; k < n && k < E.fz_n; k++) {
        float m = v3_dist(pos[k], E.fz_pos[k]);
        if (m > moved) { moved = m; worst = idx[k]; }
    }
    if (worst >= 0 && moved > 1e-5f) log_info("hpe freeze: entity %d (type %d, '%s') moved %.4f m", worst, w->type[worst], w->names[worst], moved);
    float wd = 0;
    for (int p = 0; p < WP_COUNT; p++) wd = fmaxf(wd, fabsf(wx.v[p] - E.fz_wx.v[p]));
    bool ok = days == E.fz_days && wd == 0.0f && moved < 1e-5f && n == E.fz_n;
    log_info("hpe freeze: %s after %u ticks (time %+.6f h, weather %.6f, entities %d -> %d, moved %.5f m)", ok ? "PASS" : "FAIL", w->tick - E.fz_tick,
             (days - E.fz_days) * 24.0, wd, E.fz_n, n, moved);
}

static int scancode_of(const char *s) {
    if (!strcmp(s, "enter")) return SDL_SCANCODE_RETURN;
    if (!strcmp(s, "esc")) return SDL_SCANCODE_ESCAPE;
    if (!strcmp(s, "space")) return SDL_SCANCODE_SPACE;
    if (!strcmp(s, "minus")) return SDL_SCANCODE_MINUS;
    if (!strcmp(s, "period")) return SDL_SCANCODE_PERIOD;
    if (!strcmp(s, "[")) return SDL_SCANCODE_LEFTBRACKET;
    if (!strcmp(s, "]")) return SDL_SCANCODE_RIGHTBRACKET;
    SDL_Scancode sc = SDL_GetScancodeFromName(s);
    return sc == SDL_SCANCODE_UNKNOWN ? -1 : (int)sc;
}

static void cmd_hpe(int argc, const char **argv) {
    HandPoseDB *d = db();
    if (!d || argc < 2) {
        log_info("hpe select <grip> | case <id> | ctrl <name> [add] | key <name>... | xform move|rotate|scale [x|y|z|-x|...] <value> | local <loc|rot|scale> x [y z] | "
                 "clear g|r|s | time <s> | play | stop | keyframe | mirror on|off | tool <name> | orient <name> | pivot <name> | light neutral|world | save | undo | redo | "
                 "view orbit|fp|root|ctrl|<yaw> <pitch> <dist> | freeze begin|check | dump");
        return;
    }
    const char *c = argv[1];
    if (!strcmp(c, "select") && argc > 2) {
        int i = handpose_find(d, argv[2]);
        if (i < 0) { log_warn("hpe: no grip '%s'", argv[2]); return; }
        select_grip(i);
        return;
    }
    if (!strcmp(c, "case") && argc > 2) {
        int i = handpose_case_find(d, argv[2]);
        if (i < 0) { log_warn("hpe: no case '%s'", argv[2]); return; }
        select_case(i);
        return;
    }
    if (!strcmp(c, "ctrl") && argc > 2) {
        int i = hp_rig_find(argv[2]);
        if (i < 0) { log_warn("hpe: no control '%s'", argv[2]); return; }
        select_click(i, argc > 3 && !strcmp(argv[3], "add"));
        return;
    }
    if (!strcmp(c, "key")) {
        for (int k = 2; k < argc && E.key_inject_n < (int)sizeof(E.key_inject); k++) {
            int sc = scancode_of(argv[k]);
            if (sc >= 0) E.key_inject[E.key_inject_n++] = (u8)sc;
        }
        // Applied directly (the injected presses act now, before the next frame).
        struct nk_context *ctx = ui_nk();
        if (ctx) handle_keys(ctx);
        E.key_inject_n = 0;
        return;
    }
    if (!strcmp(c, "mouse") && argc > 3) {
        E.mouse = v2((float)atof(argv[2]) * ui_width(), (float)atof(argv[3]) * ui_height());
        E.mouse_inject = true;
        if (hpx_active(&E.xf)) xform_update(E.mouse, false);
        return;
    }
    if (!strcmp(c, "xform") && argc > 3) {
        int mode = !strcmp(argv[2], "move") ? HPX_GRAB : !strcmp(argv[2], "rotate") ? HPX_ROTATE : !strcmp(argv[2], "scale") ? HPX_SCALE : HPX_NONE;
        if (mode == HPX_NONE) { log_warn("hpe xform: move | rotate | scale"); return; }
        if (!xform_begin(mode, E.mouse, false)) return;
        int vi = 3;
        if (argc > 4) {
            const char *ax = argv[3];
            bool plane = ax[0] == '-';
            int a = ax[plane] == 'x' ? 0 : ax[plane] == 'y' ? 1 : ax[plane] == 'z' ? 2 : -1;
            if (a >= 0) hpx_axis(&E.xf, a, plane);
            vi = 4;
        }
        for (const char *p = argv[vi]; *p; p++) hpx_char(&E.xf, *p);
        xform_update(E.mouse, false);
        xform_confirm();
        dump_target();
        return;
    }
    if (!strcmp(c, "time") && argc > 2) { hp_case_set_time((float)atof(argv[2])); return; }
    if (!strcmp(c, "play")) { hp_case_play(true); return; }
    if (!strcmp(c, "stop")) { hp_case_play(false); return; }
    if (!strcmp(c, "keyframe")) { store_key(); return; }
    if (!strcmp(c, "undo") || !strcmp(c, "redo")) { undo_redo(c[0] == 'r'); return; }
    if (!strcmp(c, "save")) { save_target(); return; }
    if (!strcmp(c, "dump")) { dump_target(); return; }
    if (!strcmp(c, "mirror") && argc > 2) { E.mirror_live = !strcmp(argv[2], "on"); return; }
    if (!strcmp(c, "onion") && argc > 2) { E.onion = !strcmp(argv[2], "on"); return; }
    if (!strcmp(c, "tool") && argc > 2) {
        for (int t = 0; t < 4; t++) if (!SDL_strcasecmp(argv[2], k_tool_names[t])) E.tool = t;
        return;
    }
    if (!strcmp(c, "orient") && argc > 2) {
        for (int o = 0; o < HPX_ORIENT_COUNT; o++) if (!SDL_strcasecmp(argv[2], hpx_orient_name(o))) E.orient = o;
        return;
    }
    if (!strcmp(c, "pivot") && argc > 2) {
        if (!strcmp(argv[2], "individual")) E.pivot = HPX_PIVOT_INDIVIDUAL;
        else if (!strcmp(argv[2], "median")) E.pivot = HPX_PIVOT_MEDIAN;
        else if (!strcmp(argv[2], "active")) E.pivot = HPX_PIVOT_ACTIVE;
        return;
    }
    if (!strcmp(c, "light") && argc > 2) {
        E.neutral_light = !strcmp(argv[2], "neutral");
        Game *g = game();
        if (g && g->world) game_world_freeze_lighting(g->world, E.neutral_light);
        return;
    }
    if (!strcmp(c, "state") && argc > 2) {
        int s = holdpoint_state_from_name(argv[2]);
        if (s >= 0) E.preview_state = s;
        return;
    }
    if (!strcmp(c, "view") && argc > 2) {
        if (!strcmp(argv[2], "fp")) E.fp_view = true;
        else if (!strcmp(argv[2], "orbit")) E.fp_view = false;
        else if (!strcmp(argv[2], "root")) E.focus = 1;
        else if (!strcmp(argv[2], "ctrl")) E.focus = 0;
        else if (argc > 4) { E.yaw = (float)atof(argv[2]) * DG_DEG2RAD; E.pitch = (float)atof(argv[3]) * DG_DEG2RAD; E.dist = (float)atof(argv[4]); }
        return;
    }
    if (!strcmp(c, "freeze") && argc > 2) { freeze_snapshot(!strcmp(argv[2], "check")); return; }
    if (!strcmp(c, "clear") && argc > 2) {
        int ch = argv[2][0] == 'g' ? HPA_T : argv[2][0] == 'r' ? HPA_R : HPA_S;
        HpTarget t = target();
        begin_edit(t, 0);
        for (int i = 0; i < HP_RIG_CTRLS; i++) if (E.sel[i]) hp_rig_clear(d, t, i, ch);
        end_edit(t);
        return;
    }
    if (!strcmp(c, "local") && argc > 3 && E.active >= 0) {
        HpTarget t = hp_rig_ctrl(E.active)->kind == HPK_ROOT ? target() : target_for_hands();
        HpLocal l = hp_rig_local(d, t, E.active, &E.defs);
        float v[3] = { (float)atof(argv[3]), argc > 4 ? (float)atof(argv[4]) : 0, argc > 5 ? (float)atof(argv[5]) : 0 };
        if (!strcmp(argv[2], "loc")) l.loc = v3(v[0], v[1], v[2]);
        else if (!strcmp(argv[2], "rot")) { l.rot_deg = v3(v[0], v[1], v[2]); l.rot = quat_norm(handpose_euler_deg_to_quat(l.rot_deg)); }
        else if (!strcmp(argv[2], "scale")) l.scale = v[0];
        begin_edit(t, 0);
        hp_rig_set_local(d, t, E.active, &E.defs, l);
        end_edit(t);
        return;
    }
    log_warn("hpe: unknown command '%s'", c);
}

void hp_editor_register(void) {
    scene_register(&k_scene);
    E.save_dir = cvar_reg_string("handpose_editor_dir", "", 0, "hand pose editor: save into this directory only (tests); empty = assets/handposes in the source tree + the runtime copy");
    cmd_register("hpe", cmd_hpe, "hand pose editor (scene handpose) commands for scripts: hpe with no arguments lists them");
}
