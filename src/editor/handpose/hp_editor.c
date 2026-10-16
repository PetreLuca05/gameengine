// Hand pose editor (scene handpose, docs/HANDPOSE.md).
//
// The studio (data/levels/handpose.json) is a normal level: the local player's character is posed
// by the gameplay pipeline (controller -> hold point -> hand IK -> hand poses), holding the selected
// item through the inventory, sitting in the vehicle or held on the interactable through the same
// anchors the game uses. The editor only changes the data (assets/handposes), forces the previewed
// state (the hold point's force cvars, g_handpose_preview) and owns the camera.
#include "hp_editor.h"
#include "hp_gizmo.h"
#include "hp_system.h"
#include "hp_undo.h"
#include "anim/anim_library.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "gameplay/attachments.h"
#include "gameplay/characters.h"
#include "gameplay/entities.h"
#include "gameplay/items.h"
#include "net/net.h"
#include "player/hands.h"
#include "render/debug_draw.h"
#include "ui/console.h"
#include "ui/ui.h"
#include "nuklear_config.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAM_HANDS, CAM_GRIP, CAM_FP, CAM_BODY, CAM_COUNT };
enum { FILTER_ALL, FILTER_NEW, FILTER_ORPHAN };
enum { EDIT_SOCKET, EDIT_WRIST };
#define MAX_BOXES 64
#define DRAFT_DIR "handpose_drafts"

static struct {
    bool open;
    char prev_map[64];
    int sel;                       // entry index (-1), revalidated by sel_hash every frame
    u32 sel_hash;
    int hand, state;
    int filter;
    char search[64];
    HpGizmo gizmo;
    int edit;                      // EDIT_SOCKET / EDIT_WRIST
    bool gizmo_on;
    int finger, joint;             // selected finger joint (-1 none)
    bool joint_drag;
    bool pick_contact;
    // view
    int cam;
    float yaw, pitch, dist;
    vec3 pan;
    bool hands_only, xray, wire, socket_axes, joint_axes, show_contacts, symmetry, procedural;
    // playback
    float walk;
    int clip;
    float clip_t;
    bool playing;
    // undo / clipboard
    HpUndo undo;
    HandPoseEntry committed;
    bool has_committed;
    HandPoseLayer clipboard;
    bool has_clipboard;
    // scene setup
    u32 setup_hash;
    // picking camera (the frame's render camera)
    HpCamera rc;
    bool rc_valid;
    // misc
    char status[200];
    double autosave_t;
    int drafts;
    bool drafts_checked;
    bool orphan_pick[512];
    bool confirm_delete;
    int preset;
    float master_curl;
    // this frame's geometry (world)
    HandPoseView view;
    bool view_ok;
    Xform item_world;
    int box_count;
    vec3 box_c[MAX_BOXES];
    quat box_r[MAX_BOXES];
    vec3 box_h[MAX_BOXES];
    Xform anchor[2];               // relative entries: the gameplay target the socket offsets
    bool anchor_ok[2];
    EntityId station;
} E = { .sel = -1, .hand = 1, .finger = -1, .joint = -1, .cam = CAM_HANDS, .yaw = 2.35f, .pitch = -0.3f, .dist = 0.75f, .socket_axes = true,
        .joint_axes = true, .show_contacts = true, .clip = -1, .preset = 0 };

static float S(float v) { return v * ui_scale(); }
static HandPoseDB *db(void) { return g_engine.anim ? &g_engine.anim->handposes : NULL; }
static World *world(void) { Game *g = game(); return g ? g->world : NULL; }

bool hp_editor_open(void) { return E.open; }

static HandPoseEntry *cur(void) {
    HandPoseDB *d = db();
    if (!d) return NULL;
    if (E.sel >= 0 && E.sel < d->count && d->e[E.sel].hash == E.sel_hash) return &d->e[E.sel];
    E.sel = handpose_find_hash(d, E.sel_hash);
    return E.sel >= 0 ? &d->e[E.sel] : NULL;
}

static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status, sizeof(E.status), fmt, ap);
    va_end(ap);
}

static bool state_used(const HandPoseEntry *e, int s) { return s == HPS_DEFAULT || (e->states & (1u << s)); }

static void select_entry(int i) {
    HandPoseDB *d = db();
    if (!d || i < 0 || i >= d->count) return;
    E.sel = i;
    E.sel_hash = d->e[i].hash;
    E.committed = d->e[i];
    E.has_committed = true;
    HandPoseEntry *e = &d->e[i];
    if (e->category == HPC_INTERACT || e->category == HPC_VEHICLE) E.hand = 1;   // the free hand (the one used with empty hands)
    if (!(e->hands & (1 << E.hand))) E.hand = (e->hands & 2) ? 1 : 0;
    if (!state_used(e, E.state)) E.state = HPS_DEFAULT;
    E.finger = E.joint = -1;
    E.pick_contact = false;
    E.pan = v3(0, 0, 0);
}

// ------------------------------------------------------------------ scene setup

static u16 item_of_weapon(const ItemDB *idb, int wi) {
    for (int i = 1; i < idb->item_count; i++) if (idb->items[i].weapon == wi) return (u16)i;
    return item_find(idb, idb->weapons[wi].id);
}

static void equip(u16 item, const u8 *att) {
    Player *p = game_local_player();
    Game *g = game();
    if (!p || !g) return;
    InvSlot *s = &p->inv.slots[0];
    memset(s, 0, sizeof(*s));
    s->item = item;
    s->count = item ? 1 : 0;
    const WeaponDef *wd = item ? item_weapon(&g->items, item) : NULL;
    s->ammo = wd ? (i16)wd->mag_size : 0;
    if (att) memcpy(s->att, att, sizeof(s->att));
    p->inv.revision++;
    g->hotbar_slot = 0;
}

static void stand_at(vec3 p, float yaw_deg) {
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "setpos %.3f %.3f %.3f %.1f 0", p.x, p.y, p.z, yaw_deg);
    cmd_execute_line(cmd);
}

static bool seated(void) {
    Player *p = game_local_player();
    return p && p->sim.move.mode == MOVE_SEATED;
}

// The station (interactable / vehicle entity) for an entry in the studio, nearest to the origin.
static EntityId find_station(const HandPoseEntry *e) {
    World *w = world();
    if (!w) return ENT_NONE;
    EntityId best = ENT_NONE;
    float bd = 1e9f;
    ENT_FOREACH(w, i) {
        EntityId id = ENT_MAKE(i, w->gen[i]);
        bool match = false;
        if (e->category == HPC_INTERACT && (w->comps[i] & C_INTERACT) && w->inter[i].def && w->inter[i].def->name) {
            char want[96];
            handpose_entry_id(want, sizeof(want), HPC_INTERACT, w->inter[i].def->name);
            match = !strcmp(want, e->id);
        } else if (e->category == HPC_VEHICLE && w->type[i] == ET_VEHICLE) {
            const char *n = vehicles_def_name_of(w, id);
            char want[96];
            handpose_entry_id(want, sizeof(want), HPC_VEHICLE, n ? n : "");
            match = n && !strcmp(want, e->id);
        }
        if (!match) continue;
        float d = v3_len(w->xf[i].pos);
        if (d < bd) { bd = d; best = id; }
    }
    return best;
}

// Puts the character in the situation the entry is used in: holding the item (weapons, items, an
// attachment on its first host weapon, a style's first item), in front of the interactable, or in
// the vehicle's driver seat.
static void setup_scene(HandPoseEntry *e) {
    Game *g = game();
    if (!g || !g->world) return;
    const ItemDB *idb = &g->items;
    E.setup_hash = e->hash;
    E.station = ENT_NONE;
    bool vehicle = e->category == HPC_VEHICLE;
    if (!vehicle && seated()) cmd_execute_line("vehicle_exit");
    u8 att[ATT_SLOTS] = { 0 };
    u16 item = 0;
    switch (e->category) {
    case HPC_WEAPON:
        for (int w = 0; w < idb->weapon_count; w++) if (!strcmp(idb->weapons[w].id, e->id)) item = item_of_weapon(idb, w);
        break;
    case HPC_ITEM: item = item_find(idb, e->id); break;
    case HPC_ATTACHMENT:
        for (int a = 1; a <= idb->attachment_count && !item; a++) {
            char id[96];
            handpose_entry_id(id, sizeof(id), HPC_ATTACHMENT, idb->attachments[a].id);
            if (strcmp(id, e->id)) continue;
            for (int w = 0; w < idb->weapon_count; w++) {
                if (!attach_allowed(&idb->weapons[w], &idb->attachments[a])) continue;
                item = item_of_weapon(idb, w);
                att[idb->attachments[a].slot] = (u8)a;
                break;
            }
        }
        break;
    case HPC_STYLE: {
        const char *style = e->id + 6;   // "style/<name>"
        if (!strncmp(style, "grip_", 5) || !strcmp(style, "none")) break;
        for (int w = 0; w < idb->weapon_count && !item; w++) if (!strcmp(idb->weapons[w].hold_style, style)) item = item_of_weapon(idb, w);
        for (int i = 1; i < idb->item_count && !item; i++) if (idb->items[i].weapon < 0 && !strcmp(idb->items[i].hold_style, style)) item = (u16)i;
        break;
    }
    default: break;
    }
    equip(item, att);
    if (e->category == HPC_INTERACT || vehicle) {
        EntityId st = find_station(e);
        E.station = st;
        if (!st) { status("no %s station in the studio (data/levels/handpose.json)", e->id); return; }
        vec3 p = g->world->xf[ENT_INDEX(st)].pos;
        if (vehicle) {
            stand_at(v3(p.x - 1.4f, 0.05f, p.z), 0);
            cmd_execute_line("vehicle_put 0 0");
        } else {
            stand_at(v3(p.x, 0.05f, p.z + 0.75f), 0);
        }
    } else {
        stand_at(v3(0, 0.05f, 0), 0);
    }
}

// ------------------------------------------------------------------ preview (every frame)

static int grip_of(const HandPoseEntry *e) { return !strcmp(e->inherits, "style/grip_point") ? 2 : 3; }   // GRIP_POINT / GRIP_GRAB

static void drive_preview(HandPoseEntry *e) {
    HandPosePreview *pv = &g_handpose_preview;
    pv->active = true;
    pv->hands_only = E.hands_only;
    pv->hide_item = E.xray;
    pv->walk_speed = E.walk;
    pv->clip = (i16)E.clip;
    pv->clip_time = E.clip_t;
    pv->vehicle_state = -1;
    pv->anchor_on[0] = pv->anchor_on[1] = false;
    E.anchor_ok[0] = E.anchor_ok[1] = false;
    // Hold point state: the hold states force it, the others leave the natural (hip) one.
    int hs = E.state >= HPS_HIP && E.state <= HPS_PHONE_LANDSCAPE ? E.state - 1 : -1;
    cvar_set_int(cvar_find("holdpoint_force_state"), hs);
    cvar_set_int(cvar_find("holdpoint_static"), E.procedural ? 0 : 1);
    World *w = world();
    Player *p = game_local_player();
    if (!e || !w || !p) return;
    if (e->category == HPC_VEHICLE) {
        pv->vehicle_state = (i8)(E.state == HPS_DRIVE_ONEHAND ? HPS_DRIVE_ONEHAND : HPS_DRIVE);
        vec3 gp[2];
        quat gr[2];
        bool use[2];
        int slot = game()->local_slot;
        if (g_vehicle_pose.seat_hands && g_vehicle_pose.seat_hands(w, slot, gp, gr, use))
            for (int h = 0; h < 2; h++) { E.anchor[h] = xf_make(gp[h], gr[h]); E.anchor_ok[h] = true; }
    }
    if (e->category == HPC_INTERACT && ent_valid(w, E.station)) {
        // The interaction held on its target: the edited hand (both for two-handed interactions).
        const InteractDef *def = w->inter[ENT_INDEX(E.station)].def;
        for (int h = 0; h < 2; h++) {
            bool use = h == E.hand || (def && def->hands == HANDS_BOTH);
            if (!use) continue;
            vec3 tp;
            quat tr;
            bool ok = def && def->hand_target ? def->hand_target(w, E.station, game()->local_slot, h, &tp, &tr) : false;
            if (!ok && def) {
                // Auto-placed (no hand target in code): the top of the bounds, as the game does once reviewed.
                int i = (int)ENT_INDEX(E.station);
                tp = v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, v3(0, w->vis[i].half.y, 0)));
                tr = quat_mul(quat_yaw(p->sim.move.yaw), quat_axis_angle(v3(1, 0, 0), -1.2f));
                ok = true;
            }
            if (!ok) continue;
            pv->anchor_on[h] = true;
            pv->anchor_pos[h] = tp;
            pv->anchor_rot[h] = tr;
            pv->anchor_pose[h] = e->hash;
            pv->anchor_base[h] = handpose_grip_style_hash(grip_of(e));
            pv->anchor_state[h] = (u8)(E.state == HPS_HOLD ? HPS_HOLD : E.state == HPS_USE ? HPS_USE : HPS_DEFAULT);
            E.anchor[h] = xf_make(tp, tr);
            E.anchor_ok[h] = true;
        }
    }
}

static void stop_preview(void) {
    memset(&g_handpose_preview, 0, sizeof(g_handpose_preview));
    g_handpose_preview.clip = -1;
    g_handpose_preview.vehicle_state = -1;
    Cvar *c = cvar_find("holdpoint_force_state");
    if (c) cvar_set_int(c, -1);
    c = cvar_find("holdpoint_static");
    if (c) cvar_set_int(c, 0);
}

// ------------------------------------------------------------------ geometry

static void gather(void) {
    World *w = world();
    Game *g = game();
    E.view_ok = w && g && chars_handpose_view(w, g->local_slot, &E.view);
    E.box_count = 0;
    if (!E.view_ok) return;
    E.item_world = xf_mul(E.view.root, E.view.item_model);
    E.item_world.s = v3(1, 1, 1);
    if (E.view.has_item) E.box_count = hands_held_boxes(&g->items, E.view.weapon, E.view.weapon ? NULL : E.view.item, E.view.att, E.box_c, E.box_r, E.box_h, MAX_BOXES);
}

static Xform palm_world(int h) {
    const Skeleton *s = E.view.skel;
    int b = s->role[h ? BR_R_HAND : BR_L_HAND];
    Xform r = E.view.root;
    r.s = v3(1, 1, 1);
    return b >= 0 ? xf_mul(r, xf_mul(E.view.model[b], E.view.hands->palm[h])) : r;
}

static Xform model_to_world(Xform m) {
    Xform r = E.view.root;
    r.s = v3(1, 1, 1);
    return xf_mul(r, m);
}

// The frame the entry's socket is expressed in (world): the item (absolute entries; an attachment
// grip's own space) or the gameplay target (relative entries).
static bool socket_parent(const HandPoseEntry *e, int h, Xform *out) {
    if (e->relative) {
        if (!E.anchor_ok[h]) return false;
        *out = E.anchor[h];
        return true;
    }
    if (!E.view_ok || !E.view.has_item || !E.view.sockets) return false;
    Xform p = E.item_world;
    if (E.view.sockets->has_pose_space[h]) p = xf_mul(p, E.view.sockets->pose_space[h]);
    *out = p;
    return true;
}

// The socket the entry inherits when it doesn't override it (entry space).
static Xform default_socket(const HandPoseEntry *e, int h) {
    if (e->relative || !E.view_ok || !E.view.sockets) return xf_identity();
    const ItemSockets *k = E.view.sockets;
    Xform s = h ? k->grip_r : k->grip_l;
    if (k->has_pose_space[h]) s = xf_mul(xf_inverse(k->pose_space[h]), s);
    return s;
}

static void resolved(const HandPoseEntry *e, HandPose *out) {
    const HandPoseDB *d = db();
    handpose_resolve(d, (int)(e - d->e), E.hand, E.state, out);
}

static Xform socket_value(const HandPoseEntry *e, int h, const HandPose *rp) { return rp->has_socket ? rp->socket : default_socket(e, h); }

// ------------------------------------------------------------------ edits

static HandPoseLayer *layer(HandPoseEntry *e) { return &e->layer[E.hand][E.state]; }

static void ensure_fingers(HandPoseEntry *e, int f) {
    HandPoseLayer *l = layer(e);
    if (l->set & HPL_FINGER(f)) return;
    HandPose rp;
    resolved(e, &rp);
    memcpy(l->j[f], rp.j[f], sizeof(l->j[f]));
    l->set |= (u16)HPL_FINGER(f);
}

static void set_socket(HandPoseEntry *e, Xform s) {
    HandPoseLayer *l = layer(e);
    s.s = v3(1, 1, 1);
    s.r = quat_norm(s.r);
    l->socket = s;
    l->set |= HPL_SOCKET;
}

static void set_wrist(HandPoseEntry *e, Xform x) {
    HandPoseLayer *l = layer(e);
    x.s = v3(1, 1, 1);
    x.r = quat_norm(x.r);
    l->wrist = x;
    l->set |= HPL_WRIST;
}

static void apply_preset(HandPoseEntry *e, const char *name) {
    HandPoseLayer *l = layer(e);
    if (handpose_preset(name, l->j)) l->set |= HPL_FINGERS_ALL;
    status("preset %s on %s %s", name, E.hand ? "right" : "left", handpose_state_name(E.state));
}

// Contact solver on the live pose: the fingers as the hold placed them minus the finger pose
// (the base clip's finger locals), the item's boxes in model space.
static void solve_contact(HandPoseEntry *e, int f) {
    if (!E.view_ok) return;
    const Skeleton *s = E.view.skel;
    static Pose pose;
    static Xform model[ANIM_MAX_BONES];
    pose_copy(&pose, E.view.final_pose);
    static const BoneRole first[2][5] = {
        { BR_L_THUMB1, BR_L_INDEX1, BR_L_MIDDLE1, BR_L_RING1, BR_L_PINKY1 },
        { BR_R_THUMB1, BR_R_INDEX1, BR_R_MIDDLE1, BR_R_RING1, BR_R_PINKY1 },
    };
    for (int q = 0; q < 5; q++) {
        int b = s->role[first[E.hand][q]];
        for (int k = 0; k < 4 && b >= 0; k++) {
            pose.local[b] = E.view.base_pose->local[b];
            int next = -1;
            for (int c = b + 1; c < s->count; c++) if (s->parent[c] == b) { next = c; break; }
            b = next;
        }
    }
    pose_to_model(s, &pose, model);
    HandBox boxes[MAX_BOXES + 1];
    int n = 0;
    Xform inv_root = xf_inverse(model_to_world(xf_identity()));
    for (int i = 0; i < E.box_count; i++) {
        Xform bw = xf_mul(E.item_world, xf_make(E.box_c[i], E.box_r[i]));
        Xform bm = xf_mul(inv_root, bw);
        boxes[n++] = (HandBox){ bm.t, E.box_h[i], bm.r };
    }
    World *w = world();
    if (e->relative && w && ent_valid(w, E.station)) {
        int i = (int)ENT_INDEX(E.station);
        Xform bm = xf_mul(inv_root, xf_make(v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, v3(0, w->vis[i].half.y * 0.0f, 0))), w->xf[i].rot));
        boxes[n++] = (HandBox){ bm.t, w->vis[i].half, bm.r };
    }
    if (!n) { status("contact: nothing to touch (no item / station geometry)"); return; }
    ensure_fingers(e, f);
    HandPoseLayer *l = layer(e);
    HandPose rp;
    resolved(e, &rp);
    vec3 target, *tp = NULL;
    Xform par;
    for (int c = 0; c < rp.contact_count; c++)
        if (rp.contacts[c].finger == f && socket_parent(e, E.hand, &par)) {
            target = xf_point(inv_root, xf_point(par, rp.contacts[c].point));
            tp = &target;
        }
    HandContactResult r = handpose_contact_solve(s, &pose, model, E.hand, f, l->j, E.view.hands, boxes, n, tp, f == HF_THUMB ? 0.009f : 0.008f, 0.0005f);
    if (r.blocked) status("contact %s: the open finger is already inside the geometry - move the hand socket out first", handpose_finger_name(f));
    else status("contact %s: %s, surface gap %.2f mm, curl %.1f deg", handpose_finger_name(f), r.touched ? "touching" : "no contact (limit / target)", r.tip_dist * 1000, r.curl_add);
}

// Click on the item / station: the contact point of the selected finger (entry space).
static bool pick_contact_point(HandPoseEntry *e, vec2 mouse) {
    if (!E.rc_valid || E.finger < 0) return false;
    vec3 ro, rd;
    hpg_ray(&E.rc, mouse, &ro, &rd);
    float best = 1e9f;
    for (int i = 0; i < E.box_count; i++) {
        Xform bw = xf_mul(E.item_world, xf_make(E.box_c[i], E.box_r[i]));
        float t;
        if (hpg_ray_box(ro, rd, bw.t, bw.r, E.box_h[i], &t) && t < best) best = t;
    }
    World *w = world();
    if (e->relative && w && ent_valid(w, E.station)) {
        int i = (int)ENT_INDEX(E.station);
        float t;
        if (hpg_ray_box(ro, rd, w->xf[i].pos, w->xf[i].rot, w->vis[i].half, &t) && t < best) best = t;
    }
    Xform par;
    if (best > 1e8f || !socket_parent(e, E.hand, &par)) return false;
    vec3 p = xf_point(xf_inverse(par), v3_madd(ro, rd, best));
    HandPoseLayer *l = layer(e);
    if (!(l->set & HPL_CONTACTS)) {
        HandPose rp;
        resolved(e, &rp);
        l->contact_count = rp.contact_count;
        memcpy(l->contacts, rp.contacts, sizeof(l->contacts));
        l->set |= HPL_CONTACTS;
    }
    int slot = -1;
    for (int c = 0; c < l->contact_count; c++) if (l->contacts[c].finger == E.finger) slot = c;
    if (slot < 0 && l->contact_count < HANDPOSE_CONTACTS) slot = l->contact_count++;
    if (slot < 0) return false;
    l->contacts[slot] = (HandContact){ (u8)E.finger, p };
    solve_contact(e, E.finger);
    return true;
}

// ------------------------------------------------------------------ undo / dirty / save

static bool xf_eq(Xform a, Xform b) { return !memcmp(&a.t, &b.t, sizeof(vec3)) && !memcmp(&a.r, &b.r, sizeof(quat)); }
static bool layer_eq(const HandPoseLayer *a, const HandPoseLayer *b) {
    if (a->set != b->set || a->contact_count != b->contact_count || !xf_eq(a->socket, b->socket) || !xf_eq(a->wrist, b->wrist)) return false;
    if (memcmp(&a->pole, &b->pole, sizeof(vec3)) || a->ik_weight != b->ik_weight || memcmp(a->j, b->j, sizeof(a->j))) return false;
    for (int c = 0; c < a->contact_count; c++)
        if (a->contacts[c].finger != b->contacts[c].finger || memcmp(&a->contacts[c].point, &b->contacts[c].point, sizeof(vec3))) return false;
    return true;
}
static bool entry_eq(const HandPoseEntry *a, const HandPoseEntry *b) {
    if (a->reviewed != b->reviewed || strcmp(a->inherits, b->inherits)) return false;
    for (int h = 0; h < 2; h++)
        for (int s = 0; s < HPS_COUNT; s++) if (!layer_eq(&a->layer[h][s], &b->layer[h][s])) return false;
    return true;
}

// An edit gesture ends when the mouse is released: one undo step per gesture.
static void commit(bool mouse_down) {
    HandPoseEntry *e = cur();
    HandPoseDB *d = db();
    if (!e || !d) return;
    if (!E.has_committed || E.committed.hash != e->hash) { E.committed = *e; E.has_committed = true; return; }
    if (entry_eq(e, &E.committed)) return;
    for (int h = 0; h < 2; h++)
        for (int s = 0; s < HPS_COUNT; s++) handpose_clamp_fingers(e->layer[h][s].j);
    e->dirty = true;
    handpose_rehash(d);
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    if (mouse_down) return;   // still dragging
    hpu_push(&E.undo, &E.committed);
    E.committed = *e;
}

static void undo_redo(bool redo) {
    HandPoseDB *d = db();
    if (!d) return;
    int i = redo ? hpu_redo(&E.undo, d) : hpu_undo(&E.undo, d);
    if (i < 0) { status("nothing to %s", redo ? "redo" : "undo"); return; }
    d->e[i].dirty = true;
    select_entry(i);
    g_engine.anim->manifest.hash[AC_HAND_POSES] = d->hash;
    status("%s %s", redo ? "redo" : "undo", d->e[i].id);
}

static void draft_path(const HandPoseEntry *e, char *out, int n) {
    char rel[200];
    handpose_entry_rel_path(e, "", rel, sizeof(rel));
    snprintf(out, (size_t)n, "%s%s%s", platform_pref_dir(), DRAFT_DIR, rel);
}

static bool save_entry(int i) {
    HandPoseDB *d = db();
    HandPoseEntry *e = &d->e[i];
    e->reviewed = true;
    bool ok = handpose_system_save(i);
    if (ok) {
        e->dirty = false;
        char p[700];
        draft_path(e, p, sizeof(p));
        remove(p);
    }
    if (E.has_committed && E.committed.hash == e->hash) E.committed = *e;
    return ok;
}

static void save_all(void) {
    HandPoseDB *d = db();
    int n = 0, fail = 0;
    for (int i = 0; d && i < d->count; i++)
        if (d->e[i].dirty) { if (save_entry(i)) n++; else fail++; }
    status("saved %d entries%s", n, fail ? " (some failed: see the log)" : "");
}

static void revert_entry(HandPoseEntry *e) {
    HandPoseDB *d = db();
    hpu_push(&E.undo, e);
    HandPoseDB tmp;
    handpose_db_init(&tmp);
    char rel[300], path[700], err[200] = "";
    handpose_entry_rel_path(e, HANDPOSE_DIR, rel, sizeof(rel));
    platform_data_path(rel, path, sizeof(path));
    bool loaded = e->on_disk && handpose_entry_load_file(&tmp, path, err, sizeof(err));
    int ti = loaded ? handpose_find(&tmp, e->id) : -1;
    if (ti >= 0) {
        memcpy(e->layer, tmp.e[ti].layer, sizeof(e->layer));
        e->reviewed = tmp.e[ti].reviewed;
        dg_strcpy(e->inherits, sizeof(e->inherits), tmp.e[ti].inherits);
    } else {
        handpose_entry_clear_layers(e);   // a stub: no overrides
    }
    handpose_db_free(&tmp);
    e->dirty = false;
    handpose_link(d);
    handpose_rehash(d);
    E.committed = *e;
    status("reverted %s", e->id);
}

// Autosave: unsaved entries go to the user dir every 20 s (restored on the next open).
static void autosave(void) {
    double now = platform_time();
    if (now < E.autosave_t) return;
    E.autosave_t = now + 20.0;
    HandPoseDB *d = db();
    int n = 0;
    for (int i = 0; d && i < d->count; i++) {
        if (!d->e[i].dirty) continue;
        char p[700], dir[640];
        draft_path(&d->e[i], p, sizeof(p));
        snprintf(dir, sizeof(dir), "%s%s/styles", platform_pref_dir(), DRAFT_DIR);
        SDL_CreateDirectory(dir);
        n += handpose_entry_save_file(&d->e[i], p) ? 1 : 0;
    }
    if (n) log_info("handpose editor: autosaved %d draft(s) to %s%s", n, platform_pref_dir(), DRAFT_DIR);
}

static int count_drafts(bool restore, bool discard) {
    char dir[640];
    snprintf(dir, sizeof(dir), "%s%s", platform_pref_dir(), DRAFT_DIR);
    HandPoseDB tmp;
    handpose_db_init(&tmp);
    int n = 0;
    const char *subs[2] = { "", "/styles" };
    for (int k = 0; k < 2; k++) {
        char d2[700];
        snprintf(d2, sizeof(d2), "%s%s", dir, subs[k]);
        int count = 0;
        char **files = SDL_GlobDirectory(d2, "*.json", 0, &count);
        for (int i = 0; files && i < count; i++) {
            char p[900], err[128];
            snprintf(p, sizeof(p), "%s/%s", d2, files[i]);
            n++;
            if (restore && handpose_entry_load_file(&tmp, p, err, sizeof(err))) {
                HandPoseEntry *src = &tmp.e[tmp.count - 1];
                int di = handpose_find(db(), src->id);
                if (di >= 0) {
                    HandPoseEntry *dst = &db()->e[di];
                    memcpy(dst->layer, src->layer, sizeof(dst->layer));
                    dg_strcpy(dst->inherits, sizeof(dst->inherits), src->inherits);
                    dst->dirty = true;
                }
            }
            if (discard) remove(p);
        }
        SDL_free(files);
    }
    handpose_db_free(&tmp);
    if (restore && db()) {
        handpose_link(db());
        handpose_rehash(db());
    }
    return n;
}

// ------------------------------------------------------------------ open / close

static void set_open(bool on) {
    if (on == E.open) return;
    Game *g = game();
    if (on) {
        if (!g || !g_engine.anim) { log_warn("handpose editor: needs the game and the animation library"); return; }
        E.open = true;
        hpg_init(&E.gizmo);
        handpose_system_scan(g->world, true);   // new entries show up at once
        E.drafts = count_drafts(false, false);
        if (E.sel < 0) {
            // Start on the first unreviewed entry (else the first entry).
            HandPoseDB *d = db();
            for (int i = 0; i < d->count && E.sel < 0; i++) if (!d->e[i].reviewed && d->e[i].category != HPC_STYLE) select_entry(i);
            if (E.sel < 0 && d->count) select_entry(0);
        }
        E.setup_hash = 0;
        status("hand pose editor: %d entries, %d unreviewed", db()->count, handpose_system_report()->unreviewed);
    } else {
        E.open = false;
        stop_preview();
        handpose_system_set_unsaved(false);
    }
}

static void cmd_scene(int argc, const char **argv) {
    Game *g = game();
    if (!g) return;
    if (argc < 2) { log_info("scene <handpose | map>: open the hand pose editor studio, or host a map (closes the editor)"); return; }
    if (!strcmp(argv[1], "handpose")) {
        if (net_role() != NET_OFF && net_host_client_count() > 0 && !(argc > 2 && !strcmp(argv[2], "force"))) {
            log_warn("scene handpose: players are connected (the studio hosts a new game): 'scene handpose force' to continue");
            return;
        }
        if (strcmp(g->map, "handpose")) {
            dg_strcpy(E.prev_map, sizeof(E.prev_map), g->world ? g->map : "devroom");
            if (!game_host("handpose")) { log_error("scene handpose: data/levels/handpose.json failed to load"); return; }
        }
        set_open(true);
        return;
    }
    set_open(false);
    game_host(argv[1]);
}

static void cmd_editor(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    set_open(!E.open);
}

// Scripted control (tests, screenshots): hpe select <id> | state <name> | hand <l|r> | cam <hands|grip|fp|body>
// | orbit <yaw deg> <pitch deg> <dist> | toggle <hands_only|xray|wire|gizmo|symmetry|sway> <0|1> | preset <name>
// | solve <finger|all> | save | next.
static void next_unreviewed(void);

static void cmd_hpe(int argc, const char **argv) {
    if (argc < 2) { log_info("hpe select|state|hand|cam|orbit|toggle|preset|solve|save|next ..."); return; }
    const char *v = argv[1], *a = argc > 2 ? argv[2] : "";
    if (!E.open) set_open(true);
    HandPoseEntry *e = cur();
    if (!strcmp(v, "select")) {
        int i = handpose_find(db(), a);
        if (i < 0) { log_warn("hpe: no entry %s", a); return; }
        select_entry(i);
    } else if (!strcmp(v, "state")) {
        int st = handpose_state_from_name(a);
        if (st >= 0) E.state = st;
    } else if (!strcmp(v, "hand")) {
        E.hand = a[0] == 'l' ? 0 : 1;
    } else if (!strcmp(v, "cam")) {
        E.cam = !strcmp(a, "grip") ? CAM_GRIP : !strcmp(a, "fp") ? CAM_FP : !strcmp(a, "body") ? CAM_BODY : CAM_HANDS;
    } else if (!strcmp(v, "orbit") && argc > 4) {
        E.yaw = (float)atof(argv[2]) * DG_DEG2RAD;
        E.pitch = (float)atof(argv[3]) * DG_DEG2RAD;
        E.dist = (float)atof(argv[4]);
    } else if (!strcmp(v, "toggle") && argc > 3) {
        bool on = atoi(argv[3]) != 0;
        if (!strcmp(a, "hands_only")) E.hands_only = on;
        else if (!strcmp(a, "xray")) E.xray = on;
        else if (!strcmp(a, "wire")) E.wire = on;
        else if (!strcmp(a, "gizmo")) E.gizmo_on = on;
        else if (!strcmp(a, "symmetry")) E.symmetry = on;
        else if (!strcmp(a, "sway")) E.procedural = on;
    } else if (!strcmp(v, "preset") && e) {
        apply_preset(e, a);
    } else if (!strcmp(v, "solve") && e) {
        gather();
        for (int f = 0; f < 5; f++) if (!strcmp(a, "all") || !strcmp(a, handpose_finger_name(f))) solve_contact(e, f);
        HandPose rp;
        resolved(e, &rp);
        const HandHold *hh = E.view_ok ? &E.view.proc->hand[E.hand] : NULL;
        for (int f = 0; f < 5; f++)
            log_info("hpe solve: %s data %.1f %.1f %.1f  runtime %.1f %.1f %.1f", handpose_finger_name(f), rp.j[f][0].curl, rp.j[f][1].curl, rp.j[f][2].curl,
                     hh ? hh->j[f][0].curl : -1, hh ? hh->j[f][1].curl : -1, hh ? hh->j[f][2].curl : -1);
    } else if (!strcmp(v, "save") && e) {
        save_entry(E.sel);
    } else if (!strcmp(v, "next")) {
        next_unreviewed();
    }
    if (E.status[0]) log_info("hpe: %s", E.status);
}

void hp_editor_init(void) {
    cmd_register("hpe", cmd_hpe, "hand pose editor control (scripts): hpe select|state|hand|cam|orbit|toggle|preset|solve|save|next");
    cmd_register("scene", cmd_scene, "scene handpose: the hand pose editor studio (docs/HANDPOSE.md); scene <map>: host a map");
    cmd_register("handpose_editor", cmd_editor, "toggle the hand pose editor in the current map (scene handpose: in its studio)");
}

// ------------------------------------------------------------------ camera

static vec3 frame_target(void) {
    if (!E.view_ok) return v3(0, 1.2f, 0);
    Xform r = E.view.root;
    switch (E.cam) {
    case CAM_GRIP: return palm_world(E.hand).t;
    case CAM_BODY: return v3_add(r.t, quat_rotate(r.r, v3(0, 1.0f, 0)));
    default: {
        HandPoseEntry *e = cur();
        if (e && (e->hands & 3) == 3) return v3_lerp(palm_world(0).t, palm_world(1).t, 0.5f);
        if (E.view.has_item) return E.item_world.t;
        return palm_world(E.hand).t;
    }
    }
}

bool hp_editor_camera(RenderCamera *cam) {
    if (!E.open || E.cam == CAM_FP || !E.view_ok) return false;
    vec3 t = v3_add(frame_target(), E.pan);
    float dist = E.dist * (E.cam == CAM_GRIP ? 0.45f : E.cam == CAM_BODY ? 3.2f : 1.0f);
    cam->pos = v3_madd(t, dir_from_angles(E.yaw, E.pitch), -dist);
    cam->yaw = E.yaw;
    cam->pitch = E.pitch;
    cam->roll = 0;
    cam->znear = fminf(cam->znear > 0 ? cam->znear : 0.05f, 0.02f);
    return true;
}

void hp_editor_rendered_camera(const RenderCamera *cam) {
    if (!E.open) return;
    int w = 0, h = 0;
    if (g_engine.window) SDL_GetWindowSizeInPixels(g_engine.window, &w, &h);
    E.rc = (HpCamera){ cam->pos, quat_euler(cam->yaw, cam->pitch, cam->roll), cam->fov_deg, (float)w, (float)h };
    E.rc_valid = w > 0 && h > 0;
}

// ------------------------------------------------------------------ overlays

static void axes(Xform x, float len, bool depth) {
    dbg_line(x.t, v3_add(x.t, quat_rotate(x.r, v3(len, 0, 0))), DBG_RED, 0, depth);
    dbg_line(x.t, v3_add(x.t, quat_rotate(x.r, v3(0, len, 0))), DBG_GREEN, 0, depth);
    dbg_line(x.t, v3_add(x.t, quat_rotate(x.r, v3(0, 0, len))), DBG_BLUE, 0, depth);
}

static int finger_joints(int h, vec3 *pts, int *fj) {
    const Skeleton *s = E.view.skel;
    static const BoneRole first[2][5] = {
        { BR_L_THUMB1, BR_L_INDEX1, BR_L_MIDDLE1, BR_L_RING1, BR_L_PINKY1 },
        { BR_R_THUMB1, BR_R_INDEX1, BR_R_MIDDLE1, BR_R_RING1, BR_R_PINKY1 },
    };
    int n = 0;
    for (int f = 0; f < 5; f++) {
        int b = s->role[first[h][f]];
        for (int k = 0; k < 3 && b >= 0; k++) {
            pts[n] = model_to_world(E.view.model[b]).t;
            fj[n++] = f * 3 + k;
            int next = -1;
            for (int c = b + 1; c < s->count; c++) if (s->parent[c] == b) { next = c; break; }
            b = next;
        }
    }
    return n;
}

static Xform edited_world(HandPoseEntry *e, bool *ok) {
    HandPose rp;
    resolved(e, &rp);
    Xform par;
    *ok = socket_parent(e, E.hand, &par);
    if (!*ok) return xf_identity();
    Xform sock = xf_mul(par, socket_value(e, E.hand, &rp));
    return E.edit == EDIT_WRIST ? xf_mul(sock, rp.wrist) : sock;
}

static void draw_overlays(HandPoseEntry *e) {
    if (!E.view_ok || !e) return;
    HandPose rp;
    // Item wireframe (x-ray draws it through everything instead of the solid item).
    if (E.wire || E.xray)
        for (int i = 0; i < E.box_count; i++) {
            Xform bw = xf_mul(E.item_world, xf_make(E.box_c[i], E.box_r[i]));
            dbg_set_shape_depth(!E.xray);
            dbg_box(bw.t, E.box_h[i], bw.r, E.xray ? DBG_CYAN : DBG_GREY, 0);
            dbg_set_shape_depth(true);
        }
    // Hand x-ray: the finger bones over everything.
    if (E.xray) {
        const Skeleton *s = E.view.skel;
        int hand_b[2] = { s->role[BR_L_HAND], s->role[BR_R_HAND] };
        for (int b = 0; b < s->count; b++) {
            int p = s->parent[b];
            if (p < 0) continue;
            for (int h = 0; h < 2; h++)
                if (hand_b[h] >= 0 && (b == hand_b[h] || skel_is_descendant(s, b, hand_b[h])))
                    dbg_line(model_to_world(E.view.model[p]).t, model_to_world(E.view.model[b]).t, h == E.hand ? DBG_ORANGE : DBG_WHITE, 0, false);
        }
    }
    for (int h = 0; h < 2; h++) {
        if (!(e->hands & (1 << h))) continue;
        int st = E.state;
        handpose_resolve(db(), (int)(e - db()->e), h, st, &rp);
        Xform par;
        if (!socket_parent(e, h, &par)) continue;
        Xform sock = xf_mul(par, socket_value(e, h, &rp));
        if (E.socket_axes) {
            axes(sock, 0.035f, false);
            axes(palm_world(h), 0.02f, false);
            dbg_cross(xf_mul(sock, rp.wrist).t, 0.008f, DBG_YELLOW, 0);
        }
        if (E.show_contacts)
            for (int c = 0; c < rp.contact_count; c++) {
                vec3 p = xf_point(par, rp.contacts[c].point);
                dbg_sphere(p, 0.004f, (rp.contacts[c].finger == E.finger && h == E.hand) ? DBG_ORANGE : DBG_MAGENTA, 0);
            }
        // Symmetry: the other hand's socket mirrored across the item's YZ plane.
        if (E.symmetry && !e->relative && h != E.hand) {
            Xform m = xf_mul(par, handpose_mirror_xform(socket_value(e, h, &rp)));
            dbg_line(m.t, v3_add(m.t, quat_rotate(m.r, v3(0.03f, 0, 0))), DBG_MAGENTA, 0, false);
            dbg_line(m.t, v3_add(m.t, quat_rotate(m.r, v3(0, 0.03f, 0))), DBG_MAGENTA, 0, false);
            dbg_line(m.t, v3_add(m.t, quat_rotate(m.r, v3(0, 0, 0.03f))), DBG_MAGENTA, 0, false);
        }
    }
    // Finger joints of the edited hand.
    if (E.joint_axes) {
        vec3 pts[16];
        int fj[16];
        int n = finger_joints(E.hand, pts, fj);
        for (int i = 0; i < n; i++) {
            bool sel = E.finger >= 0 && fj[i] == E.finger * 3 + E.joint;
            dbg_cross(pts[i], sel ? 0.007f : 0.004f, sel ? DBG_ORANGE : DBG_CYAN, 0);
        }
    }
    // Gizmo.
    bool ok;
    Xform ed = edited_world(e, &ok);
    if (E.gizmo_on && ok && E.rc_valid) {
        E.gizmo.size = fmaxf(0.03f, v3_dist(E.rc.pos, ed.t) * 0.12f);
        Xform fr = hpg_frame(&E.gizmo, ed);
        vec3 a[160], b[160];
        int ax[160];
        int n = hpg_lines(&E.gizmo, fr, a, b, ax, 160);
        static const u32 col[6] = { DBG_RED, DBG_GREEN, DBG_BLUE, DBG_RED, DBG_GREEN, DBG_BLUE };
        for (int i = 0; i < n; i++) dbg_line(a[i], b[i], (ax[i] == E.gizmo.hot || ax[i] == E.gizmo.active) ? DBG_YELLOW : col[ax[i]], 0, false);
    }
}

// ------------------------------------------------------------------ viewport input

static void viewport_input(struct nk_context *ctx, HandPoseEntry *e) {
    struct nk_input *in = &ctx->input;
    vec2 m = v2(in->mouse.pos.x, in->mouse.pos.y);
    bool over_ui = nk_window_is_any_hovered(ctx);
    bool ldown = in->mouse.buttons[NK_BUTTON_LEFT].down;
    bool lpress = ldown && in->mouse.buttons[NK_BUTTON_LEFT].clicked;
    // Orbit (right), pan (middle), zoom (wheel).
    if (!over_ui) {
        if (in->mouse.buttons[NK_BUTTON_RIGHT].down) {
            E.yaw -= in->mouse.delta.x * 0.006f;
            E.pitch = clampf(E.pitch - in->mouse.delta.y * 0.006f, -1.5f, 1.5f);
        }
        if (in->mouse.buttons[NK_BUTTON_MIDDLE].down) {
            float k = E.dist * 0.0015f;
            vec3 rgt = right_from_yaw(E.yaw), up = v3(0, 1, 0);
            E.pan = v3_add(E.pan, v3_add(v3_scale(rgt, -in->mouse.delta.x * k), v3_scale(up, in->mouse.delta.y * k)));
        }
        if (in->mouse.scroll_delta.y != 0) E.dist = clampf(E.dist * powf(0.88f, in->mouse.scroll_delta.y), 0.15f, 6.0f);
    }
    if (!e || !E.rc_valid || !E.view_ok) return;
    bool ok;
    Xform ed = edited_world(e, &ok);
    // Gizmo drag.
    if (E.gizmo.active != HPG_NONE) {
        if (!ldown) { hpg_end(&E.gizmo); return; }
        Xform nw = hpg_drag(&E.gizmo, &E.rc, m);
        Xform par;
        if (!socket_parent(e, E.hand, &par)) return;
        if (E.edit == EDIT_SOCKET) {
            set_socket(e, xf_mul(xf_inverse(par), nw));
        } else {
            HandPose rp;
            resolved(e, &rp);
            set_wrist(e, xf_mul(xf_inverse(xf_mul(par, socket_value(e, E.hand, &rp))), nw));
        }
        return;
    }
    // Joint drag: up / down curls, left / right spreads (the base joint).
    if (E.joint_drag) {
        if (!ldown) { E.joint_drag = false; return; }
        if (E.finger >= 0) {
            ensure_fingers(e, E.finger);
            HandJoint *j = &layer(e)->j[E.finger][E.joint];
            j->curl -= in->mouse.delta.y * 0.35f;
            if (E.joint == 0) j->spread += in->mouse.delta.x * 0.2f * (E.hand ? 1.0f : -1.0f);
            *j = handpose_clamp_joint(E.finger, E.joint, *j);
        }
        return;
    }
    if (over_ui) return;
    if (E.gizmo_on && ok) E.gizmo.hot = hpg_pick(&E.gizmo, &E.rc, hpg_frame(&E.gizmo, ed), m, 9.0f);
    else E.gizmo.hot = HPG_NONE;
    if (!lpress) return;
    if (E.pick_contact) {
        if (pick_contact_point(e, m)) E.pick_contact = false;
        return;
    }
    if (E.gizmo.hot != HPG_NONE && hpg_begin(&E.gizmo, &E.rc, ed, E.gizmo.hot, m)) return;
    vec3 pts[16];
    int fj[16];
    int n = finger_joints(E.hand, pts, fj);
    int k = hpg_pick_point(&E.rc, pts, n, m, 12.0f);
    if (k >= 0) {
        E.finger = fj[k] / 3;
        E.joint = fj[k] % 3;
        E.joint_drag = true;
    }
}

static void shortcuts(void) {
    const bool *ks = SDL_GetKeyboardState(NULL);
    static bool prev[6];
    bool ctrl = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL], shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    bool now[6] = { ks[SDL_SCANCODE_Z] && ctrl && !shift, (ks[SDL_SCANCODE_Y] && ctrl) || (ks[SDL_SCANCODE_Z] && ctrl && shift), ks[SDL_SCANCODE_F] && !ctrl,
                    ks[SDL_SCANCODE_W] && !ctrl, ks[SDL_SCANCODE_E] && !ctrl, ks[SDL_SCANCODE_S] && ctrl };
    bool edge[6];
    for (int i = 0; i < 6; i++) { edge[i] = now[i] && !prev[i]; prev[i] = now[i]; }
    if (console_is_open()) return;
    if (edge[0]) undo_redo(false);
    if (edge[1]) undo_redo(true);
    if (edge[2]) E.pan = v3(0, 0, 0);
    if (edge[3]) { E.gizmo.mode = HPG_TRANSLATE; E.gizmo_on = true; }
    if (edge[4]) { E.gizmo.mode = HPG_ROTATE; E.gizmo_on = true; }
    if (edge[5] && cur()) { int i = E.sel; if (save_entry(i)) status("saved %s", db()->e[i].id); }
}

// ------------------------------------------------------------------ panels

static bool matches(const HandPoseEntry *e) {
    if (E.filter == FILTER_NEW && (e->reviewed || e->category == HPC_STYLE)) return false;
    if (E.filter == FILTER_ORPHAN && !e->orphan) return false;
    if (E.search[0] && !SDL_strcasestr(e->id, E.search) && !SDL_strcasestr(e->label, E.search)) return false;
    return true;
}

static void next_unreviewed(void) {
    HandPoseDB *d = db();
    for (int k = 1; d && k <= d->count; k++) {
        int i = (E.sel + k) % d->count;
        if (!d->e[i].reviewed && d->e[i].category != HPC_STYLE && !d->e[i].orphan) { select_entry(i); return; }
    }
    status("every entry is reviewed");
}

static void panel_entries(struct nk_context *ctx, float x, float y, float w, float h) {
    HandPoseDB *d = db();
    if (!nk_begin(ctx, "HAND POSES", nk_rect(x, y, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE)) { nk_end(ctx); return; }
    HandPoseScanReport c;
    hp_count(d, &c);
    nk_layout_row_dynamic(ctx, S(24), 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, E.search, sizeof(E.search), nk_filter_default);
    char nb[48], ob[48];
    snprintf(nb, sizeof(nb), "New (%d)", c.unreviewed);
    snprintf(ob, sizeof(ob), "Orphans (%d)", c.orphans);
    nk_layout_row_dynamic(ctx, S(22), 3);
    if (nk_option_label(ctx, "All", E.filter == FILTER_ALL)) E.filter = FILTER_ALL;
    if (nk_option_label(ctx, nb, E.filter == FILTER_NEW)) E.filter = FILTER_NEW;
    if (nk_option_label(ctx, ob, E.filter == FILTER_ORPHAN)) E.filter = FILTER_ORPHAN;
    nk_layout_row_dynamic(ctx, S(24), 2);
    if (nk_button_label(ctx, "Next unreviewed")) next_unreviewed();
    if (nk_button_label(ctx, "Scan for new")) {
        handpose_system_scan(world(), true);
        const HandPoseScanReport *r = handpose_system_report();
        status("scan: %d sources, %d new entries, %d orphaned", r->sources, r->created, r->orphans);
    }
    static const char *cat_titles[HPC_COUNT] = { "Hold styles / empty hands", "Weapons", "Items", "Attachment grips", "Vehicle controls", "Interactables" };
    static const int order[HPC_COUNT] = { HPC_WEAPON, HPC_ITEM, HPC_ATTACHMENT, HPC_VEHICLE, HPC_INTERACT, HPC_STYLE };
    for (int oc = 0; oc < HPC_COUNT; oc++) {
        int cat = order[oc], n = 0, nn = 0;
        for (int i = 0; i < d->count; i++) if (d->e[i].category == cat && matches(&d->e[i])) { n++; nn += !d->e[i].reviewed; }
        if (!n) continue;
        char title[96];
        snprintf(title, sizeof(title), "%s (%d)%s", cat_titles[cat], n, nn && cat != HPC_STYLE ? "  *new" : "");
        if (nk_tree_push_id(ctx, NK_TREE_TAB, title, cat == HPC_STYLE ? NK_MINIMIZED : NK_MAXIMIZED, cat)) {
            for (int i = 0; i < d->count; i++) {
                HandPoseEntry *e = &d->e[i];
                if (e->category != cat || !matches(e)) continue;
                char row[160];
                snprintf(row, sizeof(row), "%s%s%s%s%s", e->dirty ? "* " : "", e->label[0] ? e->label : e->id, (!e->reviewed && cat != HPC_STYLE) ? "  [NEW]" : "",
                         e->auto_placed ? "  [AUTO]" : "", e->orphan ? "  [ORPHAN]" : "");
                nk_layout_row_dynamic(ctx, S(20), 1);
                nk_bool on = i == E.sel;
                if (nk_selectable_label(ctx, row, NK_TEXT_LEFT, &on) && i != E.sel) select_entry(i);
            }
            nk_tree_pop(ctx);
        }
    }
    // Orphans: the source is gone. Deleting needs a selection and a confirmation.
    if (c.orphans && nk_tree_push(ctx, NK_TREE_TAB, "Remove orphans", NK_MINIMIZED)) {
        int picked = 0;
        for (int i = 0; i < d->count && i < (int)ARRAY_COUNT(E.orphan_pick); i++) {
            if (!d->e[i].orphan) continue;
            nk_layout_row_dynamic(ctx, S(20), 1);
            nk_bool v = E.orphan_pick[i];
            nk_checkbox_label(ctx, d->e[i].id, &v);
            E.orphan_pick[i] = v;
            picked += v;
        }
        nk_layout_row_dynamic(ctx, S(24), 1);
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "Delete %d file(s)...", picked);
        if (picked && nk_button_label(ctx, lbl)) E.confirm_delete = true;
        if (E.confirm_delete) {
            nk_layout_row_dynamic(ctx, S(36), 1);
            nk_label_wrap(ctx, "Deletes the selected assets/handposes files (source and runtime copy). Cannot be undone.");
            nk_layout_row_dynamic(ctx, S(24), 2);
            if (nk_button_label(ctx, "Delete")) {
                int removed = 0;
                for (int i = d->count - 1; i >= 0; i--) {
                    if (i < (int)ARRAY_COUNT(E.orphan_pick) && E.orphan_pick[i] && d->e[i].orphan &&
                        hp_remove_orphan(d, i, handpose_system_src_dir() ? handpose_system_src_dir() : handpose_system_rt_dir(), handpose_system_rt_dir()))
                        removed++;
                }
                memset(E.orphan_pick, 0, sizeof(E.orphan_pick));
                E.confirm_delete = false;
                handpose_system_note_own_write();
                status("removed %d orphaned entries", removed);
            }
            if (nk_button_label(ctx, "Cancel")) E.confirm_delete = false;
        }
        nk_tree_pop(ctx);
    }
    nk_end(ctx);
}

static bool vec_row(struct nk_context *ctx, const char *label, float *v, float lo, float hi, float step) {
    float o[3] = { v[0], v[1], v[2] };
    nk_layout_row_dynamic(ctx, S(20), 4);
    nk_label(ctx, label, NK_TEXT_LEFT);
    for (int i = 0; i < 3; i++) nk_property_float(ctx, "#", lo, &v[i], hi, step, step * 0.2f);
    return memcmp(o, v, sizeof(o)) != 0;
}

// Position (mm) + rotation (deg) of a transform, nudge keypad. Returns true when edited.
static bool xform_fields(struct nk_context *ctx, Xform *x, float step_mm) {
    bool ch = false;
    float mm[3] = { x->t.x * 1000, x->t.y * 1000, x->t.z * 1000 };
    if (vec_row(ctx, "pos mm", mm, -1000, 1000, step_mm)) { x->t = v3(mm[0] / 1000, mm[1] / 1000, mm[2] / 1000); ch = true; }
    vec3 eu = handpose_quat_to_euler_deg(x->r);
    float e[3] = { eu.x, eu.y, eu.z };
    if (vec_row(ctx, "rot p/y/r", e, -180, 180, 0.5f)) { x->r = handpose_euler_deg_to_quat(v3(e[0], e[1], e[2])); ch = true; }
    // Nudge keypad: +/- 1 mm along, +/- 1 deg about the frame's own axes.
    static const char *ax[3] = { "X", "Y", "Z" };
    nk_layout_row_dynamic(ctx, S(20), 6);
    for (int i = 0; i < 3; i++) {
        vec3 d = v3(i == 0, i == 1, i == 2);
        char a[8], b[8];
        snprintf(a, sizeof(a), "-%s", ax[i]);
        snprintf(b, sizeof(b), "+%s", ax[i]);
        if (nk_button_label(ctx, a)) { x->t = v3_madd(x->t, quat_rotate(x->r, d), -0.001f); ch = true; }
        if (nk_button_label(ctx, b)) { x->t = v3_madd(x->t, quat_rotate(x->r, d), 0.001f); ch = true; }
    }
    nk_layout_row_dynamic(ctx, S(20), 6);
    for (int i = 0; i < 3; i++) {
        vec3 d = v3(i == 0, i == 1, i == 2);
        char a[8], b[8];
        snprintf(a, sizeof(a), "-r%s", ax[i]);
        snprintf(b, sizeof(b), "+r%s", ax[i]);
        if (nk_button_label(ctx, a)) { x->r = quat_norm(quat_mul(x->r, quat_axis_angle(d, -DG_DEG2RAD))); ch = true; }
        if (nk_button_label(ctx, b)) { x->r = quat_norm(quat_mul(x->r, quat_axis_angle(d, DG_DEG2RAD))); ch = true; }
    }
    return ch;
}

// The hand diagram: 5 fingers x 3 joints; click selects a joint. Orange = selected, blue = this
// layer overrides the finger, grey = inherited.
static void hand_diagram(struct nk_context *ctx, HandPoseEntry *e) {
    nk_layout_row_dynamic(ctx, S(118), 1);
    struct nk_rect r;
    enum nk_widget_layout_states ws = nk_widget(&r, ctx);
    if (ws == NK_WIDGET_INVALID) return;
    struct nk_command_buffer *cb = nk_window_get_canvas(ctx);
    nk_fill_rect(cb, r, 4, nk_rgb(30, 32, 36));
    const HandPoseLayer *l = layer(e);
    // Right hand: thumb on the left of the diagram (palm towards the viewer); left hand mirrored.
    static const float fx[5] = { 0.14f, 0.34f, 0.5f, 0.66f, 0.82f };
    static const float fy0[5] = { 0.82f, 0.62f, 0.58f, 0.62f, 0.68f };
    struct nk_input *in = &ctx->input;
    nk_fill_rect(cb, nk_rect(r.x + r.w * 0.26f, r.y + r.h * 0.66f, r.w * 0.64f, r.h * 0.3f), 6, nk_rgb(60, 62, 70));
    for (int f = 0; f < 5; f++) {
        float x = r.x + r.w * (E.hand ? fx[f] : 1.0f - fx[f]);
        for (int k = 0; k < 3; k++) {
            float y = r.y + r.h * (fy0[f] - (float)k * (f == 0 ? 0.17f : 0.19f));
            float rad = S(7);
            struct nk_rect jr = nk_rect(x - rad, y - rad, rad * 2, rad * 2);
            bool sel = E.finger == f && E.joint == k;
            bool over = (l->set & HPL_FINGER(f)) != 0;
            struct nk_color col = sel ? nk_rgb(255, 150, 40) : over ? nk_rgb(80, 150, 255) : nk_rgb(140, 140, 140);
            nk_fill_circle(cb, jr, col);
            if (nk_input_is_mouse_click_in_rect(in, NK_BUTTON_LEFT, jr)) { E.finger = f; E.joint = k; }
        }
    }
}

static void panel_properties(struct nk_context *ctx, float x, float y, float w, float h, HandPoseEntry *e) {
    if (!nk_begin(ctx, "PROPERTIES", nk_rect(x, y, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE)) { nk_end(ctx); return; }
    if (!e) { nk_layout_row_dynamic(ctx, S(20), 1); nk_label(ctx, "No entry selected.", NK_TEXT_LEFT); nk_end(ctx); return; }
    char line[256];
    nk_layout_row_dynamic(ctx, S(18), 1);
    snprintf(line, sizeof(line), "%s  (%s)%s", e->id, handpose_category_name(e->category), e->dirty ? "  - unsaved" : "");
    nk_label(ctx, line, NK_TEXT_LEFT);
    snprintf(line, sizeof(line), "source: %s", e->source[0] ? e->source : "-");
    nk_label(ctx, line, NK_TEXT_LEFT);
    snprintf(line, sizeof(line), "inherits: %s%s", e->inherits[0] ? e->inherits : "(the hold style in use)", e->relative ? "   socket: offset from the target" : "");
    nk_label(ctx, line, NK_TEXT_LEFT);
    if (e->auto_placed) nk_label(ctx, "AUTO-PLACED sockets: check them, then save (acts in game once reviewed)", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, S(22), 2);
    nk_bool rv = e->reviewed;
    nk_checkbox_label(ctx, "reviewed", &rv);
    e->reviewed = rv;
    HandPose rp;
    resolved(e, &rp);
    HandPoseLayer *l = layer(e);

    // Socket / wrist.
    if (nk_tree_push(ctx, NK_TREE_TAB, "Hand socket", NK_MAXIMIZED)) {
        nk_layout_row_dynamic(ctx, S(22), 2);
        nk_bool ov = (l->set & HPL_SOCKET) != 0;
        nk_checkbox_label(ctx, "override here", &ov);
        if (!ov && (l->set & HPL_SOCKET)) l->set &= (u16)~HPL_SOCKET;
        if (nk_button_label(ctx, "gizmo")) { E.edit = EDIT_SOCKET; E.gizmo_on = true; }
        Xform s = socket_value(e, E.hand, &rp);
        if (xform_fields(ctx, &s, 0.5f) || (ov && !(l->set & HPL_SOCKET))) set_socket(e, s);
        nk_tree_pop(ctx);
    }
    if (nk_tree_push(ctx, NK_TREE_TAB, "Wrist offset (palm frame)", NK_MINIMIZED)) {
        nk_layout_row_dynamic(ctx, S(22), 2);
        nk_bool ov = (l->set & HPL_WRIST) != 0;
        nk_checkbox_label(ctx, "override here", &ov);
        if (!ov && (l->set & HPL_WRIST)) l->set &= (u16)~HPL_WRIST;
        if (nk_button_label(ctx, "gizmo")) { E.edit = EDIT_WRIST; E.gizmo_on = true; }
        Xform x = rp.wrist;
        if (xform_fields(ctx, &x, 0.25f) || (ov && !(l->set & HPL_WRIST))) set_wrist(e, x);
        nk_tree_pop(ctx);
    }
    if (nk_tree_push(ctx, NK_TREE_TAB, "Elbow pole / IK weight", NK_MINIMIZED)) {
        nk_layout_row_dynamic(ctx, S(22), 1);
        nk_bool ov = (l->set & HPL_POLE) != 0;
        nk_checkbox_label(ctx, "override pole (aim space direction)", &ov);
        if (!ov) l->set &= (u16)~HPL_POLE;
        vec3 pole = rp.has_pole ? rp.pole : v3(E.hand ? 0.7f : -0.6f, -1.0f, 0.2f);
        float pv[3] = { pole.x, pole.y, pole.z };
        if (vec_row(ctx, "pole", pv, -2, 2, 0.02f) || (ov && !(l->set & HPL_POLE))) { l->pole = v3(pv[0], pv[1], pv[2]); l->set |= HPL_POLE; }
        nk_layout_row_dynamic(ctx, S(22), 2);
        nk_label(ctx, "IK weight", NK_TEXT_LEFT);
        float wv = rp.ik_weight;
        if (nk_slider_float(ctx, 0, &wv, 1, 0.01f) && wv != rp.ik_weight) { l->ik_weight = wv; l->set |= HPL_IKW; }
        nk_tree_pop(ctx);
    }

    // Fingers.
    if (nk_tree_push(ctx, NK_TREE_TAB, "Fingers", NK_MAXIMIZED)) {
        hand_diagram(ctx, e);
        nk_layout_row_dynamic(ctx, S(22), 2);
        static const char *names[16];
        for (int i = 0; i < handpose_preset_count() && i < 16; i++) names[i] = handpose_preset_name(i);
        E.preset = nk_combo(ctx, names, handpose_preset_count(), E.preset, S(20), nk_vec2(S(200), S(260)));
        if (nk_button_label(ctx, "Apply preset")) apply_preset(e, handpose_preset_name(E.preset));
        // Master curl: adds to every joint's curl (split like the presets).
        nk_layout_row_dynamic(ctx, S(22), 2);
        nk_label(ctx, "master curl (jog)", NK_TEXT_LEFT);
        float mc = 0;
        if (nk_slider_float(ctx, -30, &mc, 30, 0.5f) && mc != 0) {
            for (int f = 0; f < 5; f++) {
                ensure_fingers(e, f);
                for (int k = 0; k < 3; k++) l->j[f][k] = handpose_clamp_joint(f, k, (HandJoint){ l->j[f][k].curl + mc * 0.05f, l->j[f][k].spread, l->j[f][k].twist });
            }
        }
        for (int f = 0; f < 5; f++) {
            bool on = (l->set & HPL_FINGER(f)) != 0;
            snprintf(line, sizeof(line), "%s%s", handpose_finger_name(f), on ? "  (override)" : "");
            if (nk_tree_push_id(ctx, NK_TREE_NODE, line, f == E.finger ? NK_MAXIMIZED : NK_MINIMIZED, 100 + f)) {
                nk_layout_row_dynamic(ctx, S(22), 3);
                nk_bool ov = on;
                nk_checkbox_label(ctx, "override", &ov);
                if (ov && !on) ensure_fingers(e, f);
                if (!ov && on) l->set &= (u16)~HPL_FINGER(f);
                if (nk_button_label(ctx, "Solve contact")) solve_contact(e, f);
                if (nk_button_label(ctx, E.pick_contact && E.finger == f ? "click item..." : "Pick point")) { E.finger = f; E.joint = 2; E.pick_contact = true; }
                HandFingers *src = (l->set & HPL_FINGER(f)) ? &l->j : &rp.j;
                for (int k = 0; k < 3; k++) {
                    HandJoint lo, hi, v = (*src)[f][k];
                    handpose_joint_limits(f, k, &lo, &hi);
                    float c = v.curl, s = v.spread, t = v.twist;
                    nk_layout_row_dynamic(ctx, S(20), 4);
                    static const char *jn[3] = { "base", "mid", "tip" };
                    nk_label(ctx, jn[k], NK_TEXT_LEFT);
                    nk_property_float(ctx, "#curl", lo.curl, &c, hi.curl, 1, 0.25f);
                    nk_property_float(ctx, "#spr", lo.spread, &s, hi.spread, 1, 0.25f);
                    nk_property_float(ctx, "#tw", lo.twist, &t, hi.twist, 1, 0.25f);
                    if (c != v.curl || s != v.spread || t != v.twist) {
                        ensure_fingers(e, f);
                        l->j[f][k] = handpose_clamp_joint(f, k, (HandJoint){ c, s, t });
                        E.finger = f;
                        E.joint = k;
                    }
                }
                nk_tree_pop(ctx);
            }
        }
        nk_layout_row_dynamic(ctx, S(24), 2);
        if (nk_button_label(ctx, "Solve all contacts")) for (int f = 0; f < 5; f++) solve_contact(e, f);
        if (nk_button_label(ctx, "Clear contacts")) { l->contact_count = 0; l->set |= HPL_CONTACTS; }
        nk_tree_pop(ctx);
    }
    nk_end(ctx);
}

static void panel_bottom(struct nk_context *ctx, float x, float y, float w, float h, HandPoseEntry *e) {
    if (!nk_begin(ctx, "STATE / VIEW / ACTIONS", nk_rect(x, y, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE)) { nk_end(ctx); return; }
    // States the entry uses.
    if (e) {
        int used[HPS_COUNT], n = 0;
        for (int s = 0; s < HPS_COUNT; s++) if (state_used(e, s)) used[n++] = s;
        nk_layout_row_dynamic(ctx, S(22), n + 2);
        for (int i = 0; i < n; i++) {
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "%s%s", handpose_state_name(used[i]), e->layer[E.hand][used[i]].set ? "*" : "");
            if (nk_option_label(ctx, lbl, E.state == used[i])) E.state = used[i];
        }
        if ((e->hands & 1) && nk_option_label(ctx, "LEFT", E.hand == 0)) E.hand = 0;
        if ((e->hands & 2) && nk_option_label(ctx, "RIGHT", E.hand == 1)) E.hand = 1;
    }
    // Actions.
    nk_layout_row_dynamic(ctx, S(24), 10);
    if (nk_button_label(ctx, "Save") && e) { int i = E.sel; status(save_entry(i) ? "saved %s" : "save FAILED: %s", db()->e[i].id); }
    if (nk_button_label(ctx, "Save all")) save_all();
    if (nk_button_label(ctx, "Revert") && e) revert_entry(e);
    if (nk_button_label(ctx, "Reset layer") && e) { handpose_layer_clear(layer(e)); status("%s %s: overrides cleared", E.hand ? "right" : "left", handpose_state_name(E.state)); }
    if (nk_button_label(ctx, "Copy") && e) { E.clipboard = *layer(e); E.has_clipboard = true; status("copied %s %s", E.hand ? "right" : "left", handpose_state_name(E.state)); }
    if (nk_button_label(ctx, "Paste") && e && E.has_clipboard) *layer(e) = E.clipboard;
    if (nk_button_label(ctx, "Mirror to other") && e) {
        handpose_mirror_layer(layer(e), &e->layer[E.hand ? 0 : 1][E.state]);
        status("mirrored %s -> %s (%s)", E.hand ? "right" : "left", E.hand ? "left" : "right", handpose_state_name(E.state));
    }
    if (nk_button_label(ctx, "To all states") && e) {
        int n = 0;
        for (int s = 0; s < HPS_COUNT; s++) if (s != E.state && state_used(e, s)) { e->layer[E.hand][s] = *layer(e); n++; }
        status("copied to %d states", n);
    }
    if (nk_button_label(ctx, "Fingers to style") && e) {
        // The resolved fingers become the style's: every item of this hold style without its own
        // finger overrides gets them.
        HandPoseDB *d = db();
        int si = e->parent;
        if (si >= 0 && d->e[si].category == HPC_STYLE) {
            HandPose rp;
            resolved(e, &rp);
            hpu_push(&E.undo, &d->e[si]);
            HandPoseLayer *sl = &d->e[si].layer[E.hand][HPS_DEFAULT];
            memcpy(sl->j, rp.j, sizeof(sl->j));
            sl->set |= HPL_FINGERS_ALL;
            d->e[si].dirty = true;
            int users = 0;
            for (int i = 0; i < d->count; i++) users += d->e[i].parent == si;
            handpose_rehash(d);
            status("fingers -> %s (%d entries inherit it; save the style to keep it)", d->e[si].id, users);
        } else {
            status("%s inherits no style", e->id);
        }
    }
    if (nk_button_label(ctx, "Next unreviewed")) next_unreviewed();
    nk_layout_row_dynamic(ctx, S(24), 10);
    char ub[32], rb[32];
    snprintf(ub, sizeof(ub), "Undo (%d)", hpu_undo_count(&E.undo));
    snprintf(rb, sizeof(rb), "Redo (%d)", hpu_redo_count(&E.undo));
    if (nk_button_label(ctx, ub)) undo_redo(false);
    if (nk_button_label(ctx, rb)) undo_redo(true);
    static const char *cams[CAM_COUNT] = { "Both hands", "Grip close-up", "First person", "Full body" };
    for (int i = 0; i < CAM_COUNT; i++) if (nk_option_label(ctx, cams[i], E.cam == i)) E.cam = i;
    if (nk_button_label(ctx, "Reframe (F)")) E.pan = v3(0, 0, 0);
    if (nk_button_label(ctx, E.gizmo.mode == HPG_TRANSLATE ? "Move (W)" : "Rotate (E)")) E.gizmo.mode = !E.gizmo.mode;
    nk_bool loc = E.gizmo.local;
    nk_checkbox_label(ctx, "local", &loc);
    E.gizmo.local = loc;
    nk_bool sn = E.gizmo.snap;
    nk_checkbox_label(ctx, "snap", &sn);
    E.gizmo.snap = sn;
    // View toggles.
    nk_layout_row_dynamic(ctx, S(22), 9);
    nk_bool v;
#define TOG(label, field) do { v = E.field; nk_checkbox_label(ctx, label, &v); E.field = v; } while (0)
    TOG("gizmo", gizmo_on);
    TOG("hands only", hands_only);
    TOG("x-ray", xray);
    TOG("wireframe", wire);
    TOG("socket axes", socket_axes);
    TOG("joint axes", joint_axes);
    TOG("contacts", show_contacts);
    TOG("symmetry", symmetry);
    TOG("sway / bob", procedural);
#undef TOG
    // Playback: the controller (or one clip, scrubbed) and a walk preview.
    nk_layout_row_dynamic(ctx, S(22), 6);
    const AnimLibrary *lib = g_engine.anim;
    static const char *clips[LIB_MAX_CLIPS + 1];
    clips[0] = "(controller)";
    for (int i = 0; i < lib->clip_count; i++) clips[i + 1] = lib->clips[i].name;
    E.clip = nk_combo(ctx, clips, lib->clip_count + 1, E.clip + 1, S(20), nk_vec2(S(260), S(300))) - 1;
    float dur = E.clip >= 0 ? lib->clips[E.clip].duration : 1.0f;
    if (nk_button_label(ctx, E.playing ? "Pause" : "Play")) E.playing = !E.playing;
    if (E.playing && E.clip >= 0) E.clip_t = fmodf(E.clip_t + (float)g_engine.frame_dt, fmaxf(dur, 0.01f));
    nk_slider_float(ctx, 0, &E.clip_t, fmaxf(dur, 0.01f), 0.01f);
    nk_label(ctx, "walk m/s", NK_TEXT_RIGHT);
    nk_slider_float(ctx, 0, &E.walk, 6, 0.1f);
    snprintf(ub, sizeof(ub), "%.1f", E.walk);
    nk_label(ctx, ub, NK_TEXT_LEFT);
    if (E.status[0]) {
        nk_layout_row_dynamic(ctx, S(18), 1);
        nk_label(ctx, E.status, NK_TEXT_LEFT);
    }
    nk_end(ctx);
}

// Glue readout + banners (drafts, external edits) at the top centre.
static void panel_hud(struct nk_context *ctx, float sw) {
    float w = S(520), x = (sw - w) * 0.5f;
    bool banner = E.drafts > 0 || handpose_system_external_pending();
    if (!nk_begin(ctx, "##hp_hud", nk_rect(x, S(8), w, banner ? S(96) : S(46)), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) { nk_end(ctx); return; }
    char line[200];
    nk_layout_row_dynamic(ctx, S(18), 1);
    if (E.view_ok && E.view.proc) {
        const ProcState *ps = E.view.proc;
        snprintf(line, sizeof(line), "glue  L %.2f mm / %.2f deg   R %.2f mm   %s%s", ps->glue_err_pos * 1000, ps->glue_err_ang, ps->right_err_pos * 1000,
                 holdpoint_state_name(ps->hp_out.state), ps->reach_shifted ? "   (reach pull)" : "");
    } else {
        snprintf(line, sizeof(line), "no character pose yet");
    }
    nk_label(ctx, line, NK_TEXT_CENTERED);
    if (E.drafts > 0) {
        snprintf(line, sizeof(line), "%d autosaved draft(s) from an earlier session", E.drafts);
        nk_layout_row_dynamic(ctx, S(18), 1);
        nk_label(ctx, line, NK_TEXT_CENTERED);
        nk_layout_row_dynamic(ctx, S(22), 2);
        if (nk_button_label(ctx, "Restore drafts")) { count_drafts(true, true); E.drafts = 0; status("drafts restored (unsaved)"); }
        if (nk_button_label(ctx, "Discard drafts")) { count_drafts(false, true); E.drafts = 0; }
    } else if (handpose_system_external_pending()) {
        nk_layout_row_dynamic(ctx, S(18), 1);
        nk_label(ctx, "assets/handposes changed on disk while you have unsaved edits", NK_TEXT_CENTERED);
        nk_layout_row_dynamic(ctx, S(22), 2);
        if (nk_button_label(ctx, "Reload (discard my edits)")) { handpose_system_external_resolve(true); E.has_committed = false; }
        if (nk_button_label(ctx, "Keep my edits")) handpose_system_external_resolve(false);
    }
    nk_end(ctx);
}

// ------------------------------------------------------------------ frame

void hp_editor_draw(void) {
    if (!E.open) return;
    HandPoseDB *d = db();
    Game *g = game();
    if (!d || !g || !g->world) return;
    struct nk_context *ctx = ui_nk();
    HandPoseEntry *e = cur();
    if (e && e->hash != E.setup_hash) setup_scene(e);
    e = cur();
    drive_preview(e);
    gather();
    int sw = 0, sh = 0;
    if (g_engine.window) SDL_GetWindowSizeInPixels(g_engine.window, &sw, &sh);
    float W = (float)sw, H = (float)sh;
    float left = S(320), right = S(380), bottom = S(176);
    panel_entries(ctx, S(8), S(8), left, H - bottom - S(24));
    e = cur();
    panel_properties(ctx, W - right - S(8), S(8), right, H - bottom - S(24), e);
    panel_bottom(ctx, S(8), H - bottom - S(8), W - S(16), bottom, e);
    panel_hud(ctx, W);
    e = cur();
    bool ldown = ctx->input.mouse.buttons[NK_BUTTON_LEFT].down;
    viewport_input(ctx, e);
    shortcuts();
    e = cur();
    draw_overlays(e);
    commit(ldown);
    bool unsaved = false;
    for (int i = 0; i < d->count; i++) unsaved |= d->e[i].dirty;
    handpose_system_set_unsaved(unsaved);
    autosave();
}
