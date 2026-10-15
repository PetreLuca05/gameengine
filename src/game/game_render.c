// Presentation: builds draw lists for the world, local view, hands, effects, HUD and menus.
#include "game_internal.h"
#include "ui/recoil_debug.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "world/level.h"
#include "world/chunks.h"
#include "gameplay/weapons.h"
#include "gameplay/characters.h"
#include "ui/anim_debug.h"
#include "gameplay/effects.h"
#include "player/hands.h"
#include "ui/ui.h"
#include "ui/hud.h"
#include "ui/menus.h"
#include "ui/console.h"
#include "audio/audio.h"
#include "assets/assets.h"
#include "net/net.h"
#include "input/input.h"
#include <math.h>

#define G (g_engine.game)
#define MAX_CAPSULE_MESHES 16

typedef struct CapsuleMesh { float r, hh; MeshId mesh; } CapsuleMesh;
typedef struct GameRender { CapsuleMesh capsules[MAX_CAPSULE_MESHES]; int capsule_count; } GameRender;
#define GR (g_engine.game_render)

void game_render_init(void) { GR = ARENA_PUSH(&g_engine.perm_arena, GameRender); }

MeshId game_capsule_mesh(float r, float hh) {
    for (int i = 0; i < GR->capsule_count; i++)
        if (fabsf(GR->capsules[i].r - r) < 1e-3f && fabsf(GR->capsules[i].hh - hh) < 1e-3f) return GR->capsules[i].mesh;
    MeshBuilder mb;
    mb_init(&mb);
    mb_capsule(&mb, v3(0, 0, 0), r, hh, 12, 20, 0xFFFFFFFFu);
    MeshId id = mb_upload(&mb);
    mb_free(&mb);
    if (GR->capsule_count < MAX_CAPSULE_MESHES) {
        GR->capsules[GR->capsule_count].r = r;
        GR->capsules[GR->capsule_count].hh = hh;
        GR->capsules[GR->capsule_count].mesh = id;
        GR->capsule_count++;
    }
    return id;
}

static vec4 unpack(u32 c) {
    return v4((float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f, (float)((c >> 16) & 0xFF) / 255.0f, (float)(c >> 24) / 255.0f);
}

static void draw_shape(VisShape shape, vec3 half, vec3 pos, quat rot, Material mat) {
    DrawItem d;
    d.mat = mat;
    switch (shape) {
    case VIS_CYLINDER: d.mesh = G->meshes.cylinder; d.model = mat4_trs(pos, rot, v3(half.x * 2, half.y * 2, half.z * 2)); break;
    case VIS_SPHERE: d.mesh = G->meshes.sphere; d.model = mat4_trs(pos, rot, v3s(half.x * 2)); break;
    case VIS_CAPSULE: d.mesh = game_capsule_mesh(half.x, half.y); d.model = mat4_trs(pos, rot, v3s(1)); break;
    case VIS_NONE: return;
    default: d.mesh = G->meshes.cube; d.model = mat4_trs(pos, rot, v3_scale(half, 2)); break;
    }
    render_draw(&d);
}

static Material mat_from(u32 color, float rough, u32 flags) {
    Material m = material_color(unpack(color));
    m.roughness = rough;
    m.flags = flags;
    return m;
}

static void draw_box_local(vec3 pos, quat rot, vec3 offset, vec3 half, Material m) {
    draw_shape(VIS_BOX, half, v3_add(pos, quat_rotate(rot, offset)), rot, m);
}

static void render_entity(World *w, int i, float alpha, bool focused) {
    EntityType t = (EntityType)w->type[i];
    Transform *xf = &w->xf[i];
    vec3 pos = v3_lerp(xf->prev_pos, xf->pos, alpha);
    quat rot = quat_nlerp(xf->prev_rot, xf->rot, alpha);
    float anim = lerpf(w->anim_prev[i], w->anim[i], alpha);
    Visual *v = &w->vis[i];
    u32 hl = focused ? MAT_HIGHLIGHT : 0;
    Material m = mat_from(v->color, 0.75f, hl);
    if (v->emissive) m.emissive = v->emissive / 255.0f;

    if (v->model) {
        const Model *mdl = assets_model(v->model);
        if (mdl) {
            // Fit the model's bounds to the entity's physical extents.
            vec3 ext = v3_scale(v3_sub(mdl->bounds.max, mdl->bounds.min), 0.5f);
            vec3 mid = v3_scale(v3_add(mdl->bounds.max, mdl->bounds.min), 0.5f);
            vec3 sc = v3(v->half.x / fmaxf(ext.x, 1e-3f), v->half.y / fmaxf(ext.y, 1e-3f), v->half.z / fmaxf(ext.z, 1e-3f));
            mat4 base = mat4_mul(mat4_trs(pos, rot, sc), mat4_translate(v3_neg(mid)));
            for (int k = 0; k < mdl->mesh_count; k++) {
                DrawItem d;
                d.mesh = mdl->meshes[k].mesh;
                d.model = mat4_mul(base, mdl->meshes[k].local);
                d.mat = mdl->meshes[k].mat;
                d.mat.color = v4_lerp(d.mat.color, unpack(v->color), 0.35f);
                d.mat.flags |= hl;
                render_draw(&d);
            }
            return;
        }
    }
    switch (t) {
    case ET_DOOR: {
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        // Knobs on both sides.
        Material km = mat_from(rgba8(0.75f, 0.68f, 0.3f, 1), 0.3f, hl);
        km.metallic = 0.9f;
        vec3 knob = w->door[i].knob_local;
        draw_box_local(pos, rot, v3(knob.x, knob.y, 0.05f), v3(0.025f, 0.025f, 0.03f), km);
        draw_box_local(pos, rot, v3(knob.x, knob.y, -0.05f), v3(0.025f, 0.025f, 0.03f), km);
        if (w->flags[i] & EF_LOCKED) {
            Material lm = mat_from(rgba8(0.9f, 0.1f, 0.1f, 1), 0.4f, MAT_UNLIT);
            draw_box_local(pos, rot, v3(knob.x, knob.y + 0.1f, 0.03f), v3(0.02f, 0.03f, 0.01f), lm);
            draw_box_local(pos, rot, v3(knob.x, knob.y + 0.1f, -0.03f), v3(0.02f, 0.03f, 0.01f), lm);
        }
        break;
    }
    case ET_LEVER: {
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        float ang = lerpf(-0.75f, 0.75f, smoothstepf(0, 1, anim));
        quat hr = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), -ang));
        vec3 pivot = v3_add(pos, quat_rotate(rot, v3(0, 0.15f, 0)));
        Material hm = mat_from(rgba8(0.15f, 0.15f, 0.15f, 1), 0.5f, hl);
        draw_shape(VIS_BOX, v3(0.02f, 0.22f, 0.02f), v3_add(pivot, quat_rotate(hr, v3(0, 0.22f, 0))), hr, hm);
        Material gm = mat_from((w->flags[i] & EF_ON) ? rgba8(0.2f, 0.8f, 0.2f, 1) : rgba8(0.8f, 0.2f, 0.15f, 1), 0.5f, hl);
        draw_shape(VIS_BOX, v3(0.05f, 0.035f, 0.035f), v3_add(pivot, quat_rotate(hr, v3(0, 0.45f, 0))), hr, gm);
        break;
    }
    case ET_SWITCH: {
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        float tilt = (w->flags[i] & EF_ON) ? -0.3f : 0.3f;
        quat rr = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), tilt));
        Material rm = mat_from((w->flags[i] & EF_ON) ? rgba8(0.95f, 0.85f, 0.4f, 1) : rgba8(0.4f, 0.4f, 0.4f, 1), 0.5f, hl);
        if (w->flags[i] & EF_ON) rm.emissive = 0.4f;
        draw_shape(VIS_BOX, v3(0.025f, 0.045f, 0.012f), v3_add(pos, quat_rotate(rot, v3(0, 0, 0.018f))), rr, rm);
        break;
    }
    case ET_LIGHT: {
        bool on = (w->flags[i] & EF_ON) != 0;
        Material lm = mat_from(v->color, 0.5f, MAT_NO_SHADOW | (on ? MAT_UNLIT : 0));
        if (on) lm.color = v4(lm.color.x * 4, lm.color.y * 4, lm.color.z * 4, 1);
        else lm.color = v4(0.15f, 0.15f, 0.15f, 1);
        draw_shape(VIS_BOX, v->half, pos, rot, lm);
        if (on) {
            PointLight l = { v3_add(pos, v3(0, -0.15f, 0)), w->light[i].radius, w->light[i].color, w->light[i].intensity };
            render_add_light(&l);
        }
        break;
    }
    case ET_CONTAINER: {
        vec3 h = v->half;
        m.texture = assets_texture("crate");
        if (m.texture) { m.flags |= MAT_TEXTURED | MAT_TRIPLANAR; m.tex_scale = 1.6f; m.color = v4(1, 1, 1, 1); }
        float lid_h = 0.06f;
        draw_box_local(pos, rot, v3(0, -lid_h * 0.5f, 0), v3(h.x, h.y - lid_h * 0.5f, h.z), m);
        // Lid hinged at the back top edge.
        float ang = anim * 1.6f;
        quat lr = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), ang));
        vec3 hinge = v3_add(pos, quat_rotate(rot, v3(0, h.y - lid_h, -h.z)));
        Material lm = mat_from(rgba8(0.38f, 0.27f, 0.15f, 1), 0.8f, hl);
        draw_shape(VIS_BOX, v3(h.x + 0.01f, lid_h * 0.5f, h.z + 0.01f), v3_add(hinge, quat_rotate(lr, v3(0, lid_h * 0.5f, h.z))), lr, lm);
        Material bm = mat_from(rgba8(0.2f, 0.2f, 0.22f, 1), 0.4f, hl);
        draw_box_local(pos, rot, v3(-h.x + 0.08f, 0, h.z + 0.005f), v3(0.03f, h.y * 0.9f, 0.01f), bm);
        draw_box_local(pos, rot, v3(h.x - 0.08f, 0, h.z + 0.005f), v3(0.03f, h.y * 0.9f, 0.01f), bm);
        break;
    }
    case ET_PICKUP: {
        if (w->flags[i] & EF_HIDDEN) break;
        float t = (float)g_engine.real_time;
        float bob = sinf(t * 2.0f + (float)i) * 0.025f;
        quat spin = quat_yaw(t * 0.8f + (float)i);
        m.emissive = 0.12f + (focused ? 0.2f : 0.0f);
        draw_shape((VisShape)v->shape, v->half, v3_add(pos, v3(0, bob, 0)), spin, m);
        break;
    }
    case ET_DUMMY: {
        if (char_get_index(w, i)) {
            // Skinned dummy (characters.c draws it): just the stand.
            Material post = mat_from(rgba8(0.3f, 0.3f, 0.3f, 1), 0.6f, 0);
            draw_shape(VIS_BOX, v3(0.25f, 0.02f, 0.25f), v3_add(v3_sub(pos, v3(0, 0.9f, 0)), v3(0, 0.02f, 0)), rot, post);
            break;
        }
        // Toppled backwards when dead.
        quat tilt = quat_mul(rot, quat_axis_angle(v3(1, 0, 0), -anim * 1.45f));
        vec3 base = v3_sub(pos, v3(0, 0.9f, 0));
        vec3 center = v3_add(base, quat_rotate(tilt, v3(0, 0.9f, 0)));
        draw_shape(VIS_CAPSULE, v->half, center, tilt, m);
        Material hm = mat_from(rgba8(0.85f, 0.75f, 0.5f, 1), 0.8f, 0);
        draw_shape(VIS_SPHERE, v3s(0.16f), v3_add(base, quat_rotate(tilt, v3(0, 1.6f, 0))), tilt, hm);
        Material ring = mat_from(rgba8(0.85f, 0.15f, 0.1f, 1), 0.6f, 0);
        draw_shape(VIS_CYLINDER, v3(0.31f, 0.03f, 0.31f), v3_add(base, quat_rotate(tilt, v3(0, 1.2f, 0))), tilt, ring);
        draw_shape(VIS_CYLINDER, v3(0.31f, 0.03f, 0.31f), v3_add(base, quat_rotate(tilt, v3(0, 0.8f, 0))), tilt, ring);
        Material post = mat_from(rgba8(0.3f, 0.3f, 0.3f, 1), 0.6f, 0);
        draw_shape(VIS_BOX, v3(0.25f, 0.02f, 0.25f), v3_add(base, v3(0, 0.02f, 0)), rot, post);
        break;
    }
    case ET_ZOMBIE:
        break;   // characters.c
    case ET_PLAYER: {
        int slot = w->player_slot[i];
        if (slot == G->local_slot) break; // first person: drawn as hands
        if (char_get_index(w, i)) break;  // skinned character (characters.c)
        bool dead = (w->flags[i] & EF_DEAD) != 0;
        bool crouch = (w->flags[i] & EF_CROUCHED) != 0;
        float height = crouch ? PLAYER_CROUCH_HEIGHT : PLAYER_HEIGHT;
        float yaw = quat_get_yaw(rot);
        quat body_rot = quat_yaw(yaw);
        if (dead) body_rot = quat_mul(body_rot, quat_axis_angle(v3(1, 0, 0), -1.4f));
        float hh = height * 0.5f - PLAYER_RADIUS - 0.1f;
        vec3 center = v3_add(pos, quat_rotate(body_rot, v3(0, height * 0.5f - 0.1f, 0)));
        draw_shape(VIS_CAPSULE, v3(PLAYER_RADIUS, hh, PLAYER_RADIUS), center, body_rot, m);
        Material head = mat_from(rgba8(0.85f, 0.66f, 0.52f, 1), 0.7f, 0);
        vec3 head_pos = v3_add(pos, quat_rotate(body_rot, v3(0, height - 0.15f, 0)));
        draw_shape(VIS_BOX, v3(0.12f, 0.13f, 0.12f), head_pos, body_rot, head);
        Material visor = mat_from(rgba8(0.1f, 0.1f, 0.12f, 1), 0.2f, 0);
        draw_box_local(head_pos, body_rot, v3(0, 0.02f, -0.12f), v3(0.09f, 0.03f, 0.01f), visor);
        if (!dead) {
            float pitch = w->anim[i];
            vec3 eye = v3_add(pos, v3(0, crouch ? PLAYER_EYE_CROUCH : PLAYER_EYE_STAND, 0));
            hands_render_remote(w, slot, eye, yaw, pitch);
        }
        break;
    }
    case ET_PROJECTILE:
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        break;
    case ET_GATE:
        m.texture = assets_texture("hazard");
        if (m.texture) { m.flags |= MAT_TEXTURED | MAT_TRIPLANAR; m.tex_scale = 0.5f; m.color = v4(1, 1, 1, 1); m.metallic = 0.6f; m.roughness = 0.5f; }
        draw_shape(VIS_BOX, v->half, pos, rot, m);
        break;
    case ET_PROP:
        if (v->shape == VIS_BOX) {
            m.texture = assets_texture("crate");
            if (m.texture) { m.flags |= MAT_TEXTURED | MAT_TRIPLANAR; m.tex_scale = 1.0f / fmaxf(0.2f, v->half.x * 2.0f); m.color = v4_lerp(v4(1, 1, 1, 1), unpack(v->color), 0.3f); }
        }
        draw_shape((VisShape)v->shape, v->half, pos, rot, m);
        break;
    default:
        draw_shape((VisShape)v->shape, v->half, pos, rot, m);
        break;
    }
}

void game_render_world(World *w, float alpha, vec3 cam_pos, vec3 cam_fwd) {
    if (w->chunks) chunks_render(w, cam_pos);
    else level_draw_static(w, cam_pos, cam_fwd);
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_VISUAL) && w->type[i] != ET_ZOMBIE) continue;
        if ((w->flags[i] & EF_HIDDEN) && w->type[i] != ET_PICKUP) continue;
        render_entity(w, i, alpha, ENT_MAKE(i, w->gen[i]) == G->focus);
    }
    chars_render(w, G->alpha, cam_pos, G->local_slot);
}

static void draw_debug(World *w, vec3 cam) {
    if (G->r_drawcollision->i) {
        phys_debug_draw(w->phys, cam, 25.0f);
        for (int s = 0; s < MAX_PLAYERS; s++) {
            Player *p = &w->players[s];
            if (!p->active || !p->character) continue;
            float h = p->sim.move.crouched ? PLAYER_CROUCH_HEIGHT : PLAYER_HEIGHT;
            dbg_capsule(phys_character_pos(p->character), PLAYER_RADIUS, h, DBG_MAGENTA, 0);
        }
    }
    chars_debug_draw(w, cam);
    if (G->r_drawhitboxes->i) {
        ENT_FOREACH(w, i) {
            if (!(w->comps[i] & C_HITBOX)) continue;
            vec3 feet = w->type[i] == ET_DUMMY ? v3_sub(w->xf[i].pos, v3(0, 0.9f, 0)) : w->xf[i].pos;
            dbg_capsule(feet, w->hitbox[i].radius, w->hitbox[i].height, DBG_RED, 0);
            float hy = w->type[i] == ET_PLAYER ? w->hitbox[i].height - 0.18f : w->hitbox[i].head_height;
            dbg_sphere(v3_add(feet, v3(0, hy, 0)), w->hitbox[i].head_radius, DBG_YELLOW, 0);
        }
    }
}

void game_render(void) {
    World *w = G->world;
    if (!w) {
        ui_text_shadow(ui_width() * 0.5f, ui_height() * 0.45f, FONT_LARGE, UI_RGBA(230, 230, 230, 255), UI_ALIGN_CENTER,
                       net_role() == NET_CLIENT ? "Connecting..." : "No world loaded");
        menus_draw();
        console_draw();
        return;
    }
    Player *p = game_local_player();
    PlayerMove interp;
    if (p) {
        interp = p->sim.move;
        PlayerMove prev = (G->mode == GM_CLIENT) ? G->predicted_prev : p->sim.move;
        if (G->mode == GM_HOST) {
            int i = (int)ENT_INDEX(p->ent);
            prev.pos = w->xf[i].prev_pos;
        }
        interp.pos = v3_add(v3_lerp(prev.pos, p->sim.move.pos, G->alpha), G->error_offset);
        interp.crouch_amount = lerpf(prev.crouch_amount, p->sim.move.crouch_amount, G->alpha);
    } else {
        memset(&interp, 0, sizeof(interp));
        interp.pos = v3(0, 2, 10);
    }
    bool ads = p && (G->last_cmd.buttons & BTN_AIM) && p->sim.move.mode == MOVE_WALK;
    const WeaponDef *wd = p ? weapon_equipped_def(w, G->local_slot) : NULL;
    if (wd && wd->kind == WPN_MELEE) ads = false;
    input_set_ads(ads);
    bool dead = p && p->sim.move.mode == MOVE_DEAD;
    RenderCamera cam = camera_update(&G->view, &interp, (float)g_engine.frame_dt, ads, wd ? wd->ads_fov : 0, dead);
    // Weapon feel: camera kick spring (visual only, aim unaffected) + aim sway (bullets use it too).
    if (p && !dead) {
        vec3 kr = spring_vec3_sample(&G->recoil_vis.cam_rot, G->alpha);
        float kb = spring_f32_sample(&G->recoil_vis.cam_back, G->alpha);
        vec2 sway = weapon_sway_deg(wd, &p->sim.wpn, weapon_sway_time(G->last_cmd.seq) + G->alpha * SIM_DT);
        cam.pitch += (kr.x + sway.y) * DG_DEG2RAD;
        cam.yaw -= (kr.y + sway.x) * DG_DEG2RAD;
        cam.roll += kr.z * DG_DEG2RAD;
        cam.pos = v3_madd(cam.pos, dir_from_angles(cam.yaw, cam.pitch), -kb);
    }
    // First-person camera on the body's head socket (Unreal-style): position from the animated
    // head (spine bent to this frame's view), rotation = the view (pawn control rotation). Stair
    // smoothing and the landing dip are kept; the animation replaces the procedural head bob.
    {
        Xform sock;
        Player *lp = game_local_player();
        if (!dead && lp && cvar_find("cl_camera_socket")->i && chars_fp_camera(w, G->alpha, G->local_slot, cam.yaw, cam.pitch, &sock)) {
            float raw_eye = interp.pos.y + player_eye_height(&interp);
            float smooth = G->view.smooth_eye_y - raw_eye;   // stair smoothing (0 on flat ground)
            cam.pos = v3_add(sock.t, v3(0, smooth + G->view.land_offset, 0));
            cam.roll = G->view.roll;
            float hr = cvar_find("cl_camera_socket_roll")->f;
            if (hr > 0) {
                vec3 rgt = quat_rotate(sock.r, v3(1, 0, 0));
                cam.roll += asinf(clampf(rgt.y, -1.0f, 1.0f)) * hr;   // a little of the head's own roll
            }
            G->view.eye = cam.pos;
        } else if (!dead) {
            cam.pos = v3_add(cam.pos, player_eye_offset(cam.yaw, cam.pitch));
        }
    }
    render_set_camera(&cam);

    LevelStatic *ls = &w->level;
    RenderEnvironment env;
    memset(&env, 0, sizeof(env));
    env.sun_dir = v3_len2(ls->sun_dir) > 0 ? ls->sun_dir : v3_norm(v3(0.3f, 1, 0.2f));
    env.sun_color = v3_len2(ls->sun_color) > 0 ? ls->sun_color : v3(1, 1, 1);
    env.sun_intensity = ls->sun_intensity > 0 ? ls->sun_intensity : 2.0f;
    env.sky_ambient = ls->ambient_sky;
    env.ground_ambient = ls->ambient_ground;
    env.ambient_intensity = 0.55f;
    env.fog_color = ls->fog_color;
    env.fog_density = ls->fog_density;
    env.shadow_center = v3_madd(cam.pos, dir_from_angles(cam.yaw, 0), 12.0f);
    env.shadow_extent = 32.0f;
    render_set_environment(&env);

    vec3 fwd = dir_from_angles(cam.yaw, cam.pitch);
    game_render_world(w, G->mode == GM_HOST ? G->alpha : 1.0f, cam.pos, fwd);

    if (p && !dead) {
        HandsInput hi;
        memset(&hi, 0, sizeof(hi));
        hi.world = w;
        hi.player = G->local_slot;
        hi.eye = cam.pos;
        hi.yaw = cam.yaw;
        hi.pitch = cam.pitch;
        hi.roll = cam.roll;
        hi.look_delta = G->frame_look;
        hi.dt = (float)g_engine.frame_dt;
        hi.focus = G->use_pressed_frame && G->use_focus ? G->use_focus : G->focus;
        hi.use_target_valid = G->use_pressed_frame && G->use_focus_hands >= 0;
        hi.use_target = G->use_focus_point;
        hi.use_target_hands = G->use_focus_hands;
        hi.use_pressed = G->use_pressed_frame;
        hi.use_held = (G->last_cmd.buttons & BTN_USE) != 0;
        hi.ads = ads;
        hi.vm_kick_pos = spring_vec3_sample(&G->recoil_vis.vm_pos, G->alpha);
        hi.vm_kick_rot = spring_vec3_sample(&G->recoil_vis.vm_rot, G->alpha);
        hands_update(&hi);
        chars_render_local_fp(w, G->alpha, G->local_slot);
        hands_render();
    }
    fx_render();
    draw_debug(w, cam.pos);
    recoil_debug_draw(cam.pos);
    anim_debug_draw();
    vec3 right = right_from_yaw(cam.yaw);
    vec3 up = v3_cross(right, fwd);
    dbg_submit((float)g_engine.frame_dt, cam.pos, right, up);
    audio_update(cam.pos, fwd, up);

    hud_draw();
    menus_draw();
    console_draw();
}
