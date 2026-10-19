// Inventory character preview (characters.h chars_preview, docs/CHARACTERS.md "Character preview").
//
// A second, client-only CharacterInst: never in w->chars, never simulated, replicated or hit-tested. It
// takes the shown player's character definition and parts (only read from the live CharacterInst) and
// the requested gear, and is posed by its own CharRuntime: the controller's idle clip (the user's clip
// set; a missing clip plays what the controller falls back to, never a procedural stand-in), no foot IK
// / turn / lean / damage, the hold system when an item is in its hands. Its parts are drawn into the
// preview view only (SkinnedDraw.view), its gear and held item into the preview's own static list
// (render_char_preview_capture_*), and render_char_preview renders them alone with the studio rig of
// assets/ui/char_preview.json (char_preview_rig.c): front view, transparent background.
//
// The instance stands at the world origin facing -Z (its draws are in no other pass, so where it
// stands is invisible); the camera looks at it level from -Z at chest height.
#include "characters.h"
#include "characters_internal.h"
#include "char_preview_rig.h"
#include "gameplay/attachments.h"
#include "player/hands.h"
#include "render/char_preview_view.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIB (g_engine.anim)
#define CHAR_PREVIEW_RIG_PATH "assets/ui/char_preview.json"

static struct {
    CharacterInst *c;          // the preview instance (its def, parts, part cache, gear, skeleton instance)
    CharRuntime *rt;           // its own animation runtime
    const CharacterDef *def;
    u32 lib_rev;
    u16 held;                  // the held item the runtime runs with (0xFFFF: none yet)
    bool framed;
    float head_y, bottom_y;    // framing heights of its idle pose (m above the feet)
    float hip_y, knee_y;
    float face_fix;            // turn squaring the idle's stance to the camera (rad)
    u64 frame;                 // engine frame of the last request (one evaluation per frame)
    TextureId tex;
    CharPreviewRig rig;
    bool rig_loaded;
    bool frozen;               // tests: the pose holds still (char_preview_show 3)
} PV;

void char_preview_set_frozen(bool on) { PV.frozen = on; }

void char_preview_rig_reload(void) {
    char buf[512];
    char *text = platform_read_file(platform_data_path(CHAR_PREVIEW_RIG_PATH, buf, sizeof(buf)), NULL);
    char_preview_rig_default(&PV.rig);
    PV.rig_loaded = true;
    if (!text) { log_warn("char preview: %s missing, built-in look", CHAR_PREVIEW_RIG_PATH); return; }
    char err[160];
    if (!char_preview_rig_parse(&PV.rig, text, err, sizeof(err))) {
        log_error("%s (built-in look)", err);
        char_preview_rig_default(&PV.rig);
    }
    free(text);
    PV.framed = false;
    PV.face_fix = 0.0f;
}

const CharPreviewRig *char_preview_rig(void) {
    if (!PV.rig_loaded) char_preview_rig_reload();
    return &PV.rig;
}

static void pv_free(void) {
    if (PV.c) free(PV.c->si);
    free(PV.c);
    free(PV.rt);
    PV.c = NULL;
    PV.rt = NULL;
    PV.def = NULL;
}

void char_preview_shutdown(void) { pv_free(); }

// A fresh instance of `def` (a reload or another character drops the old one: its def is gone).
static bool pv_setup(const CharacterDef *def) {
    if (PV.c && PV.def == def && PV.lib_rev == LIB->revision) return true;
    pv_free();
    CharacterInst *c = (CharacterInst *)calloc(1, sizeof(CharacterInst));
    PV.rt = (CharRuntime *)calloc(1, sizeof(CharRuntime));
    if (!c || !PV.rt) { free(c); pv_free(); return false; }
    c->def = def;
    c->def_index = (int)(def - LIB->chars);
    c->is_player = true;
    c->slot = -1;
    c->si = (SkeletonInstance *)malloc(sizeof(SkeletonInstance));
    skel_inst_init(c->si, &LIB->skeleton);
    chars_preview_pick_parts(c);
    charrt_init(PV.rt, LIB, def);
    PV.c = c;
    PV.def = def;
    PV.lib_rev = LIB->revision;
    PV.held = 0xFFFF;
    PV.framed = false;
    PV.face_fix = 0.0f;
    return true;
}

static float bone_y(const Xform *pose, int role) {
    int b = LIB->skel.role[role];
    return b >= 0 ? pose[b].t.y : 0.0f;
}

// The yaw a left -> right bone line faces square to (right_from_yaw convention), 0 without the bones.
static float line_yaw(const Xform *pose, int left, int right) {
    int l = LIB->skel.role[left], r = LIB->skel.role[right];
    if (l < 0 || r < 0) return 0.0f;
    vec3 d = v3_sub(pose[r].t, pose[l].t);
    return atan2f(-d.z, d.x);
}

// The pose of this frame: the idle clip (or the held item's idle + the hold), the instance at the
// origin turned by `turn` (+ the stance fix); the world pose goes into c->si. Returns the root.
static Xform pv_pose(World *w, const CharPreviewIn *in, float turn, const ItemSockets **sock_out) {
    float yaw = turn + PV.face_fix;
    CharacterInst *c = PV.c;
    CharRuntime *rt = PV.rt;
    u16 item = in->held_item;
    const ItemDef *it = item && w->items ? item_def(w->items, item) : NULL;
    const WeaponDef *wd = it ? item_weapon(w->items, item) : NULL;
    if (!it) item = 0;
    if (item != PV.held) {   // another item: the idle of its style at once, no blend from the last one
        charrt_init(rt, LIB, c->def);
        PV.held = item;
    }
    int style = HOLD_NONE;
    if (item) {
        style = proc_hold_style_index(&LIB->proc, wd ? wd->hold_style : it->hold_style);
        if (style < 0) style = HOLD_ITEM_SMALL;
    }
    float dt = PV.frozen ? 0.0f : clampf((float)g_engine.frame_dt, 0.0f, 0.1f);
    CharInputs ci;
    memset(&ci, 0, sizeof(ci));
    ci.grounded = true;
    ci.move_local = v2(0, 1);
    ci.health = c->def->hp;
    ci.body_yaw = ci.aim_yaw = yaw;
    ci.weapon_type = style;
    ci.weapon_id = item;
    charrt_set_inputs(rt, &ci);
    charrt_tick(rt, dt);
    charrt_eval_base(rt);
    Xform root = xf_make(v3(0, 0, 0), quat_yaw(yaw));
    ProcInput pi;
    charrt_proc_input(rt, &pi, root, dt);
    pi.foot_ik = pi.turn = pi.lean = pi.damage = pi.spine = false;   // the clip as authored
    pi.hold = item != 0;
    pi.hp_static = true;                                             // no hold point sway / breathing
    static ItemSockets sock;
    *sock_out = NULL;
    if (item) {
        if (wd) {
            static WeaponDef eff;
            attach_weapon_effective(w->items, wd, in->held_att, &eff);
            sock = eff.sockets;
        } else {
            sock = it->sockets;
        }
        pi.item = &sock;
        pi.sprinting = PV.rig.held_low;   // the low ready: not aimed at the camera
        *sock_out = &sock;
    }
    charrt_post(rt, &pi);
    static Xform world[ANIM_MAX_BONES];
    charrt_to_world(rt, rt->final_model, root, c->def->scale, world);
    skel_inst_set_pose(c->si, world);
    if (!PV.framed) {
        // From the first idle pose: the framing heights (the head-top bone + the crown allowance, the
        // image bottom between the hip joints and the knees) and the stance's turn (an idle often stands
        // a little sideways): the shoulders, and some of the hips, squared to the camera by `square`.
        float hip = 0.5f * (bone_y(world, BR_L_UPLEG) + bone_y(world, BR_R_UPLEG)), knee = 0.5f * (bone_y(world, BR_L_LEG) + bone_y(world, BR_R_LEG));
        PV.head_y = bone_y(world, BR_HEAD_END) + PV.rig.crown_m * c->def->scale;
        PV.bottom_y = char_preview_bottom_y(&PV.rig, hip, knee);
        PV.hip_y = hip;
        PV.knee_y = knee;
        PV.framed = PV.head_y > PV.bottom_y + 0.1f;
        float sh = wrap_angle(line_yaw(world, BR_L_ARM, BR_R_ARM) - yaw), hp = wrap_angle(line_yaw(world, BR_L_UPLEG, BR_R_UPLEG) - yaw);
        PV.face_fix = -PV.rig.square * (0.7f * sh + 0.3f * hp);
        root = xf_make(v3(0, 0, 0), quat_yaw(turn + PV.face_fix));
        charrt_to_world(rt, rt->final_model, root, c->def->scale, world);
        skel_inst_set_pose(c->si, world);
    }
    return root;
}

// Camera space (x right, y up, z towards the camera) -> world.
static vec3 cam_to_world(vec3 right, vec3 up, vec3 back, vec3 v) { return v3_add(v3_add(v3_scale(right, v.x), v3_scale(up, v.y)), v3_scale(back, v.z)); }

TextureId chars_preview(World *w, int player_slot, const CharPreviewIn *in) {
    if (!w || !in || !LIB || !LIB->loaded || in->w < 8 || in->h < 8) return 0;
    if (PV.frame == g_engine.frame_index && PV.c) return PV.tex;   // several callers in a frame: the first request
    PV.frame = g_engine.frame_index;
    PV.tex = 0;
    const CharPreviewRig *rig = char_preview_rig();
    // The shown player's character (read only): its definition and parts; none (menus, between lives):
    // the player character with the default skin.
    const Player *p = player_slot >= 0 && player_slot < MAX_PLAYERS && w->players[player_slot].active ? &w->players[player_slot] : NULL;
    const CharacterInst *pc = p && w->chars ? char_get(w, p->ent) : NULL;
    const CharacterDef *def = pc ? pc->def : anim_lib_char_by_id(LIB, "player");
    if (!def || !pv_setup(def)) return 0;
    CharacterInst *c = PV.c;
    c->slot = player_slot;
    if (pc) memcpy(c->parts, pc->parts, sizeof(c->parts));
    memcpy(c->worn, in->worn, sizeof(c->worn));
    float yaw = rig->turn_deg * DG_DEG2RAD + in->yaw;
    const ItemSockets *sock = NULL;
    Xform root = pv_pose(w, in, yaw, &sock);
    if (!PV.framed) return 0;
    // The camera: level at chest height, straight in front (-Z), the image spanning the framing heights.
    CharPreviewRig fr = *rig;
    if (in->head_top > 0.0f) fr.head_top = in->head_top;   // the caller's framing (the inventory: the head by the primary slot)
    CharPreviewShot shot = char_preview_frame(&fr, PV.head_y, PV.bottom_y);
    vec3 look = v3(0, shot.cam_y, 0), cam = v3(0, shot.cam_y, -shot.dist);
    vec3 back = v3(0, 0, -1), right = v3(-1, 0, 0), up = v3(0, 1, 0);   // camera axes (it looks down +Z)
    // Draws: the parts (the preview view), the gear and the held item (captured: the preview's own list).
    chars_preview_draw(c);
    render_char_preview_capture_begin(cam, shot.fov_rad * DG_RAD2DEG);
    chars_preview_draw_worn(w, c);
    if (sock && PV.rt->proc.has_item) {
        const ItemDef *it = item_def(w->items, in->held_item);
        const WeaponDef *wd = item_weapon(w->items, in->held_item);
        Xform im = PV.rt->proc.item_model;
        Xform iw = xf_mul(xf_make(root.t, root.r), xf_make(v3_scale(im.t, c->def->scale), im.r));
        hands_set_draw_scale(PV.rt->proc.item_scale);
        hands_set_fire_mech(false, 0.0f, v3(0, 0, 0));
        hands_draw_held_att(wd, wd ? NULL : it, in->held_att, iw.t, iw.r, 0);
        hands_set_draw_scale(1.0f);
    }
    render_char_preview_capture_end();
    RenderCharPreview rq;
    memset(&rq, 0, sizeof(rq));
    rq.width = in->w;
    rq.height = in->h;
    rq.supersample = rig->supersample;
    rq.cam_pos = cam;
    rq.view = mat4_look_at(cam, look, up);
    rq.proj = mat4_perspective(shot.fov_rad, (float)in->w / (float)in->h, 0.05f, 30.0f);
    rq.key_dir = cam_to_world(right, up, back, rig->key_dir);
    rq.key_color = v3_scale(rig->key_color, rig->key);
    rq.fill_dir = cam_to_world(right, up, back, rig->fill_dir);
    rq.fill_color = v3_scale(rig->fill_color, rig->fill);
    for (int k = 0; k < 2; k++) {
        rq.rim_pos[k] = v3_add(look, cam_to_world(right, up, back, rig->rim_pos[k]));
        rq.rim_color[k] = v3_scale(rig->rim_color, rig->rim);
    }
    rq.rim_radius = rig->rim_radius;
    rq.sky_ambient = v3_scale(rig->sky, rig->ambient);
    rq.ground_ambient = v3_scale(rig->ground, rig->ambient);
    rq.exposure = rig->exposure;
    rq.tonemap = rig->tonemap;
    rq.saturation = rig->saturation;
    render_char_preview(&rq);
    PV.tex = render_char_preview_texture();
    return PV.tex;
}

// Diagnostics (char_preview_check): the skin drawn, the framing.
void char_preview_info(char *out, size_t n) {
    if (!PV.c) { snprintf(out, n, "no preview yet"); return; }
    const CharPartOption *o = character_part(PV.c->def, PV.c->part_gpu_sel, CSLOT_BODY);
    CharPreviewShot s = char_preview_frame(&PV.rig, PV.head_y, PV.bottom_y);
    snprintf(out, n, "character %s, body %s, head top %.3f m, hips %.3f m, knees %.3f m, image bottom %.3f m, camera %.3f m high %.3f m away, fov %.1f deg, stance turn %.1f deg",
             PV.c->def->id, o && o->id[0] ? o->id : "?", PV.head_y, PV.hip_y, PV.knee_y, PV.bottom_y, s.cam_y, s.dist, s.fov_rad * DG_RAD2DEG, PV.face_fix * DG_RAD2DEG);
}
