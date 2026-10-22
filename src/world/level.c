#include "gameplay/attachments.h"
#include "level.h"
#include "render/vgeo/vgeo_import.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/mesh_gen.h"
#include "ui/font.h"
#include "gameplay/entities.h"
#include "gameplay/characters.h"
#include "gameplay/revive/revive.h"
#include "anim/anim_library.h"
#include "vfx/vfx_library.h"
#include "cJSON.h"
#include "assets/assets.h"
#include "openworld.h"
#include "kit_world.h"
#include "level_include.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define MAX_BUCKETS 32

typedef struct LevelBuild {
    World *w;
    int material;   // SurfaceMaterial applied to the bodies being added
    MeshBuilder buckets[MAX_BUCKETS];
    u32 bucket_flags[MAX_BUCKETS];
    vec3 bucket_surf[MAX_BUCKETS];   // roughness, metallic, emissive
    float bucket_spacing[MAX_BUCKETS];   // MAT_GRID_MOA grid spacing (m)
    int bucket_count;
    vec3 surf;                       // surface of the brush being built (PBR test geometry)
    float spacing;                   // grid spacing of the brush being built (sighting targets)
    bool open_world;                 // the level has generated terrain (a "room" is then just its floor)
    bool no_render;                  // the brush being built is collision only ("render": false)
} LevelBuild;

// ------------------------------------------------------------------ json helpers

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static const char *js(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}
static bool jb(const cJSON *o, const char *k, bool def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}
static vec3 jv3(const cJSON *o, const char *k, vec3 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return v3((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble);
}
static u32 jcol(const cJSON *o, const char *k, u32 def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) < 3) return def;
    return rgba8((float)cJSON_GetArrayItem(v, 0)->valuedouble, (float)cJSON_GetArrayItem(v, 1)->valuedouble, (float)cJSON_GetArrayItem(v, 2)->valuedouble, 1);
}

// ------------------------------------------------------------------ geometry emitters

static MeshBuilder *bucket(LevelBuild *b, u32 flags) {
    for (int i = 0; i < b->bucket_count; i++)
        if (b->bucket_flags[i] == flags && v3_len2(v3_sub(b->bucket_surf[i], b->surf)) < 1e-6f && b->bucket_spacing[i] == b->spacing) return &b->buckets[i];
    if (b->bucket_count >= MAX_BUCKETS) return &b->buckets[0];
    int i = b->bucket_count++;
    mb_init(&b->buckets[i]);
    b->bucket_flags[i] = flags;
    b->bucket_surf[i] = b->surf;
    b->bucket_spacing[i] = b->spacing;
    return &b->buckets[i];
}

static void add_static_body(LevelBuild *b, const BodyDesc *d) {
    LevelStatic *ls = &b->w->level;
    if (ls->body_count >= MAX_STATIC_BODIES) { log_warn("level: too many static bodies"); return; }
    BodyId id = phys_add_body(b->w->phys, d);
    if (id != PHYS_INVALID_BODY) ls->bodies[ls->body_count++] = id;
}

static void add_box(LevelBuild *b, vec3 center, vec3 half, float yaw, u32 color, u32 flags, bool collide) {
    quat rot = quat_yaw(yaw);
    if (!b->no_render) mb_box_transformed(bucket(b, flags), mat4_trs(center, rot, v3s(1)), half, color);
    if (collide) {
        BodyDesc d;
        memset(&d, 0, sizeof(d));
        d.shape.kind = SHAPE_BOX;
        d.shape.half = half;
        d.pos = center;
        d.rot = rot;
        d.motion = PM_STATIC;
        d.layer = PL_STATIC;
        d.friction = 0.8f;
        d.material = (u8)b->material;
        add_static_body(b, &d);
    }
    LevelStatic *ls = &b->w->level;
    ls->bounds_min = v3_min(ls->bounds_min, v3_sub(center, half));
    ls->bounds_max = v3_max(ls->bounds_max, v3_add(center, half));
}

static void add_label(World *w, vec3 pos, vec3 right, vec3 up, float size, u32 color, const char *text) {
    LevelStatic *ls = &w->level;
    if (ls->label_count >= MAX_LABELS) return;
    WorldLabel *l = &ls->labels[ls->label_count++];
    l->pos = pos;
    l->right = right;
    l->up = up;
    l->size = size;
    l->color = color;
    dg_strcpy(l->text, sizeof(l->text), text);
}

// Label on the vertical face of something, facing direction yaw (the reader looks along -facing).
// yaw = orientation of the surface: its outward normal is quat_yaw(yaw) * +Z.
static void add_wall_label(World *w, vec3 pos, float yaw, float size, u32 color, const char *text) {
    vec3 facing = v3_neg(forward_from_yaw(yaw));   // normal pointing at the reader
    vec3 right = v3_neg(v3_cross(facing, v3(0, 1, 0)));
    add_label(w, v3_madd(pos, facing, 0.01f), right, v3(0, 1, 0), size, color, text);
    w->level.labels[w->level.label_count - 1].centered = true;
}

// Ramp rising along local -Z (the walking direction for yaw) with width along X.
static void add_ramp(LevelBuild *b, vec3 base, float yaw, float width, float length, float angle_deg, u32 color, u32 flags) {
    float h = tanf(angle_deg * DG_DEG2RAD) * length;
    quat rot = quat_yaw(yaw);
    float hw = width * 0.5f, hl = length * 0.5f;
    vec3 lp[6] = {
        { -hw, 0, hl }, { hw, 0, hl },      // low edge (front)
        { -hw, 0, -hl }, { hw, 0, -hl },    // back bottom
        { -hw, h, -hl }, { hw, h, -hl },    // high edge
    };
    vec3 p[6];
    for (int i = 0; i < 6; i++) p[i] = v3_add(base, quat_rotate(rot, lp[i]));
    MeshBuilder *mb = bucket(b, flags);
    vec3 slope_n = v3_norm(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[4], p[0])));
    if (slope_n.y < 0) slope_n = v3_neg(slope_n);
    mb_quad(mb, p[0], p[1], p[5], p[4], slope_n, color);
    vec3 back_n = quat_rotate(rot, v3(0, 0, -1));
    mb_quad(mb, p[3], p[2], p[4], p[5], back_n, color);
    mb_quad(mb, p[2], p[3], p[1], p[0], v3(0, -1, 0), color);
    vec3 nx = quat_rotate(rot, v3(1, 0, 0));
    u32 a = mb_vertex(mb, p[1], nx, v2(0, 0), color), c = mb_vertex(mb, p[3], nx, v2(0, 0), color), d = mb_vertex(mb, p[5], nx, v2(0, 0), color);
    mb_tri(mb, a, c, d);
    vec3 nnx = v3_neg(nx);
    a = mb_vertex(mb, p[0], nnx, v2(0, 0), color);
    c = mb_vertex(mb, p[4], nnx, v2(0, 0), color);
    d = mb_vertex(mb, p[2], nnx, v2(0, 0), color);
    mb_tri(mb, a, c, d);
    // Fix UVs to world-scale for the new verts (grid uses world pos anyway).

    // Collision: closed triangle mesh kept alive in the level arena for debug drawing.
    static const u32 tris[24] = { 0, 1, 5, 0, 5, 4, 3, 2, 4, 3, 4, 5, 2, 3, 1, 2, 1, 0, 1, 3, 5, 0, 4, 2 };
    vec3 *verts = ARENA_PUSH_N(&g_engine.level_arena, vec3, 6);
    u32 *idx = ARENA_PUSH_N(&g_engine.level_arena, u32, 24);
    for (int i = 0; i < 6; i++) verts[i] = lp[i];
    memcpy(idx, tris, sizeof(tris));
    BodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.shape.kind = SHAPE_MESH;
    bd.shape.verts = verts;
    bd.shape.vert_count = 6;
    bd.shape.indices = idx;
    bd.shape.index_count = 24;
    bd.pos = base;
    bd.rot = rot;
    bd.motion = PM_STATIC;
    bd.layer = PL_STATIC;
    bd.friction = 0.8f;
    bd.material = (u8)b->material;
    add_static_body(b, &bd);
}

// Unity-style renderer shadow flags of a brush / placement (docs/SHADOWS_UNITY_MODEL.md):
// "cast_shadows": "off" | "on" | "two_sided" | "shadows_only", "receive_shadows": bool, "layer": 0..7
// (light culling masks). The legacy "shadow": false = "cast_shadows": "off".
#define LEVEL_LAYER_SHIFT 28   // the brush layer rides in the bucket flags above every MAT_* bit
static u32 level_shadow_flags(const cJSON *e) {
    u32 f = 0;
    if (!jb(e, "shadow", true)) f |= MAT_NO_SHADOW;
    const char *cs = js(e, "cast_shadows", NULL);
    int c = cs ? shs_cast_from_name(cs) : -1;
    if (cs && c < 0) log_warn("level: cast_shadows '%s' (off | on | two_sided | shadows_only)", cs);
    if (c == CAST_SHADOWS_OFF) f |= MAT_NO_SHADOW;
    else if (c == CAST_SHADOWS_TWO_SIDED) f |= MAT_SHADOW_TWO_SIDED;
    else if (c == CAST_SHADOWS_ONLY) f |= MAT_SHADOW_ONLY;
    if (!jb(e, "receive_shadows", true)) f |= MAT_NO_RECEIVE;
    return f;
}
static u32 level_layer(const cJSON *e) { return (u32)DG_CLAMP((int)jf(e, "layer", 0), 0, SHADOW_LAYERS - 1); }

static void build_brush(LevelBuild *b, const cJSON *br) {
    const char *type = js(br, "type", "box");
    vec3 pos = jv3(br, "pos", v3(0, 0, 0));
    float yaw = jf(br, "yaw", 0) * DG_DEG2RAD;
    u32 color = jcol(br, "color", rgba8(0.6f, 0.6f, 0.62f, 1));
    u32 flags = jb(br, "grid", true) ? MAT_GRID : 0;
    if (jb(br, "fine_grid", false)) flags = MAT_GRID_FINE;
    flags |= level_shadow_flags(br) | level_layer(br) << LEVEL_LAYER_SHIFT;
    World *w = b->w;
    u32 label_col = rgba8(0.08f, 0.08f, 0.1f, 1);
    const char *label = js(br, "label", NULL);
    // Surface material (impact VFX / sounds): explicit, or a sensible default per brush type.
    const char *def_mat = !strcmp(type, "doorway") ? "plaster" : (!strcmp(type, "ramp") ? "wood" : "concrete");
    const char *mat_name = js(br, "material", def_mat);
    // Optional PBR surface (glossy floors, metal props, emissive signs); batched per surface.
    b->surf = v3(jf(br, "roughness", 0.85f), jf(br, "metallic", 0.0f), jf(br, "emissive", 0.0f));
    if (jb(br, "unlit", false)) flags |= MAT_UNLIT;
    // Greybox glass (story levels before the building kit, docs/STORY.md): a transparent bucket with the
    // Fresnel glass look; no shadow (sun reaches through), still collides.
    if (jb(br, "glass", false)) flags = (flags & ~(u32)(MAT_GRID | MAT_GRID_FINE)) | MAT_GLASS | MAT_NO_SHADOW;
    // City boxes (docs/STORY.md "Vista"): a procedural window grid lit at night ("facade"), or window panes ("windows").
    if (jb(br, "facade", false)) flags = (flags & ~(u32)(MAT_GRID | MAT_GRID_FINE)) | MAT_FACADE;
    if (jb(br, "windows", false)) flags = (flags & ~(u32)(MAT_GRID | MAT_GRID_FINE)) | MAT_WINDOWS;
    b->material = surface_from_name(mat_name);
    if (b->material < 0) { log_warn("level: unknown material '%s'", mat_name); b->material = SURF_DEFAULT; }

    if (!strcmp(type, "moa_target")) {
        // Sighting-in target at `distance` m from the firing point: a square board with a 1 MOA
        // grid (MAT_GRID_MOA), its centre snapped onto a 10 MOA line crossing, a 2 MOA aim square,
        // a post and a distance label. Faces local +Z (yaw like boxes) towards the shooter.
        float dist = jf(br, "distance", 10);
        float s = dist * tanf(DG_DEG2RAD / 60.0f);
        float side = jf(br, "size", 1);
        float cy = jf(br, "center_height", 1.6f);
        quat rot = quat_yaw(yaw);
        vec3 rt = quat_rotate(rot, v3(1, 0, 0));
        float snap = s * 10.0f;
        // Snap the centre along the board's own axes (world grid coordinates of the face).
        float u = v3_dot(pos, rt);
        float du = roundf(u / snap) * snap - u;
        pos = v3_madd(pos, rt, du);
        cy = fmaxf(roundf(cy / snap) * snap, side * 0.5f + 0.1f);
        b->spacing = s;
        add_box(b, v3(pos.x, cy, pos.z), v3(side * 0.5f, side * 0.5f, 0.04f), yaw, color, MAT_GRID_MOA, jb(br, "collide", true));
        b->spacing = 0;
        vec3 face = v3_add(v3(pos.x, cy, pos.z), quat_rotate(rot, v3(0, 0, 0.0405f)));
        add_box(b, face, v3(s, s, 0.0005f), yaw, rgba8(0.02f, 0.02f, 0.02f, 1), MAT_NO_SHADOW, false);
        float post_h = cy - side * 0.5f;
        if (post_h > 0.02f) add_box(b, v3(pos.x, post_h * 0.5f, pos.z), v3(0.03f, post_h * 0.5f, 0.03f), yaw, rgba8(0.3f, 0.3f, 0.32f, 1), 0, true);
        char text[40];
        snprintf(text, sizeof(text), "%.0f m  (1 square = 1 MOA = %.1f mm)", dist, s * 1000.0f);
        add_wall_label(w, v3_add(v3(pos.x, cy + side * 0.5f + 0.04f + side * 0.06f, pos.z), quat_rotate(rot, v3(0, 0, 0.041f))), yaw,
                       jf(br, "label_size", side * 0.05f), label_col, text);
    } else if (!strcmp(type, "box")) {
        vec3 size = jv3(br, "size", v3(1, 1, 1));
        vec3 half = v3_scale(size, 0.5f);
        vec3 center = v3_add(pos, v3(0, half.y, 0));
        // "render": false = collision only (story levels: wrecks drawn by the vista's day layer, game_vista.c).
        b->no_render = !jb(br, "render", true);
        add_box(b, center, half, yaw, color, flags, jb(br, "collide", true));
        b->no_render = false;
        if (label) {
            // Label on the face pointing towards the reader (local +Z), near the top.
            vec3 face = v3_add(center, quat_rotate(quat_yaw(yaw), v3(0, half.y - 0.18f, half.z)));
            add_wall_label(w, face, yaw, jf(br, "label_size", 0.16f), label_col, label);
        }
    } else if (!strcmp(type, "ramp")) {
        float width = jf(br, "width", 2), angle = jf(br, "angle", 30), length = jf(br, "length", 4);
        add_ramp(b, pos, yaw, width, length, angle, color, flags);
        float h = tanf(angle * DG_DEG2RAD) * length;
        float plat = jf(br, "platform", 0);
        if (plat > 0) {
            vec3 pc = v3_add(pos, quat_rotate(quat_yaw(yaw), v3(0, h * 0.5f, -length * 0.5f - plat * 0.5f)));
            add_box(b, pc, v3(width * 0.5f, h * 0.5f, plat * 0.5f), yaw, color, flags, true);
        }
        if (label) {
            vec3 lp = v3_add(pos, quat_rotate(quat_yaw(yaw), v3(0, 0.02f, length * 0.5f + 0.4f)));
            vec3 r = quat_rotate(quat_yaw(yaw), v3(1, 0, 0)), u = quat_rotate(quat_yaw(yaw), v3(0, 0, -1));
            add_label(w, lp, r, u, 0.3f, label_col, label);
        }
    } else if (!strcmp(type, "stairs")) {
        float rise = jf(br, "rise", 0.2f), run = jf(br, "run", 0.3f), width = jf(br, "width", 2);
        int count = (int)jf(br, "count", 8);
        quat rot = quat_yaw(yaw);
        for (int s = 0; s < count; s++) {
            float top = rise * (float)(s + 1);
            // Solid stack per step (no gaps underneath).
            vec3 c = v3_add(pos, quat_rotate(rot, v3(0, top * 0.5f, -(run * (float)s + run * 0.5f))));
            add_box(b, c, v3(width * 0.5f, top * 0.5f, run * 0.5f), yaw, color, flags, true);
        }
        float plat = jf(br, "platform", 0);
        if (plat > 0) {
            float h = rise * (float)count;
            vec3 c = v3_add(pos, quat_rotate(rot, v3(0, h * 0.5f, -(run * (float)count + plat * 0.5f))));
            add_box(b, c, v3(width * 0.5f, h * 0.5f, plat * 0.5f), yaw, color, flags, true);
        }
        if (label) {
            vec3 lp = v3_add(pos, quat_rotate(rot, v3(0, 0.02f, 0.45f)));
            vec3 r = quat_rotate(rot, v3(1, 0, 0)), u = quat_rotate(rot, v3(0, 0, -1));
            add_label(w, lp, r, u, 0.3f, label_col, label);
        }
    } else if (!strcmp(type, "doorway")) {
        // Wall segment along local X with a centered opening; optional door entity in the opening.
        float len = jf(br, "length", 6), height = jf(br, "height", 3.5f), thick = jf(br, "thickness", 0.25f);
        float dw = jf(br, "door_width", 1.1f), dh = jf(br, "door_height", 2.1f);
        quat rot = quat_yaw(yaw);
        float side = (len - dw) * 0.5f;
        add_box(b, v3_add(pos, quat_rotate(rot, v3(-(dw * 0.5f + side * 0.5f), height * 0.5f, 0))), v3(side * 0.5f, height * 0.5f, thick * 0.5f), yaw, color, flags, true);
        add_box(b, v3_add(pos, quat_rotate(rot, v3(dw * 0.5f + side * 0.5f, height * 0.5f, 0))), v3(side * 0.5f, height * 0.5f, thick * 0.5f), yaw, color, flags, true);
        add_box(b, v3_add(pos, quat_rotate(rot, v3(0, dh + (height - dh) * 0.5f, 0))), v3(dw * 0.5f, (height - dh) * 0.5f, thick * 0.5f), yaw, color, flags, true);
        // Frame trim (visual only, slightly darker)
        u32 trim = rgba8(0.3f, 0.3f, 0.32f, 1);
        add_box(b, v3_add(pos, quat_rotate(rot, v3(-dw * 0.5f - 0.03f, dh * 0.5f, 0))), v3(0.03f, dh * 0.5f, thick * 0.5f + 0.02f), yaw, trim, 0, false);
        add_box(b, v3_add(pos, quat_rotate(rot, v3(dw * 0.5f + 0.03f, dh * 0.5f, 0))), v3(0.03f, dh * 0.5f, thick * 0.5f + 0.02f), yaw, trim, 0, false);
        add_box(b, v3_add(pos, quat_rotate(rot, v3(0, dh + 0.03f, 0))), v3(dw * 0.5f + 0.06f, 0.03f, thick * 0.5f + 0.02f), yaw, trim, 0, false);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s", label ? label : "door 2.1 m");
        add_wall_label(w, v3_add(pos, quat_rotate(rot, v3(0, dh + 0.25f, thick * 0.5f))), yaw, 0.14f, label_col, buf);
        const cJSON *door = cJSON_GetObjectItemCaseSensitive(br, "door");
        if (door && w->authority) {
            u16 key = item_find(w->items, js(door, "key", ""));
            EntityId e = spawn_door(w, pos, yaw, dw - 0.04f, dh - 0.03f, jb(door, "locked", false), key, jcol(door, "color", rgba8(0.55f, 0.4f, 0.25f, 1)));
            dg_strcpy(w->names[ENT_INDEX(e)], sizeof(w->names[0]), js(door, "name", ""));
        }
    } else if (!strcmp(type, "tunnel")) {
        float len = jf(br, "length", 6), ih = jf(br, "inner_height", 1.25f), iw = jf(br, "inner_width", 1.4f), t = 0.25f;
        quat rot = quat_yaw(yaw);
        float outer_h = ih + t;
        add_box(b, v3_add(pos, quat_rotate(rot, v3(-(iw * 0.5f + t * 0.5f), outer_h * 0.5f, 0))), v3(t * 0.5f, outer_h * 0.5f, len * 0.5f), yaw, color, flags, true);
        add_box(b, v3_add(pos, quat_rotate(rot, v3(iw * 0.5f + t * 0.5f, outer_h * 0.5f, 0))), v3(t * 0.5f, outer_h * 0.5f, len * 0.5f), yaw, color, flags, true);
        add_box(b, v3_add(pos, quat_rotate(rot, v3(0, ih + t * 0.5f, 0))), v3(iw * 0.5f + t, t * 0.5f, len * 0.5f), yaw, color, flags, true);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s", label ? label : "crouch tunnel");
        add_wall_label(w, v3_add(pos, quat_rotate(rot, v3(0, ih + t * 0.5f, len * 0.5f))), yaw, 0.14f, label_col, buf);
    } else {
        log_warn("level: unknown brush type '%s'", type);
    }
}

static void build_room(LevelBuild *b, const cJSON *room) {
    vec3 size = jv3(room, "size", v3(60, 10, 60));
    float t = jf(room, "wall", 0.5f);
    u32 floor_col = jcol(room, "floor_color", rgba8(0.62f, 0.62f, 0.6f, 1));
    u32 wall_col = jcol(room, "wall_color", rgba8(0.7f, 0.7f, 0.68f, 1));
    float hx = size.x * 0.5f, hz = size.z * 0.5f, h = size.y;
    int fm = surface_from_name(js(room, "floor_material", "concrete")), wm = surface_from_name(js(room, "wall_material", "plaster"));
    // Open rooms: "walls" / "ceiling" false (outdoor test area under the sky); the floor can be
    // larger than the room ("floor_size") and invisible boundary walls keep players on it.
    bool walls = jb(room, "walls", true), ceiling = jb(room, "ceiling", true);
    // On an open world (a room brought in by an include) the terrain is the ground beyond the room:
    // the floor stays the room's size and the world's edge walls bound it.
    vec3 fsz = b->open_world ? size : jv3(room, "floor_size", v3(size.x, 0, size.z));
    float fx = fmaxf(fsz.x * 0.5f, hx), fz = fmaxf(fsz.z * 0.5f, hz);
    b->material = fm >= 0 ? fm : SURF_CONCRETE;
    // "floor": false = no floor slab (an included room on an open world stands on the terrain itself:
    // devroom_vegetation's nature ground); "floor_grid": false = a plain floor.
    if (jb(room, "floor", true)) add_box(b, v3(0, -t * 0.5f, 0), v3(fx + t, t * 0.5f, fz + t), 0, floor_col, jb(room, "floor_grid", true) ? MAT_GRID : 0u, true);
    b->material = wm >= 0 ? wm : SURF_PLASTER;
    if (ceiling) add_box(b, v3(0, h + t * 0.5f, 0), v3(hx + t, t * 0.5f, hz + t), 0, wall_col, MAT_GRID | MAT_NO_SHADOW, true);
    if (walls) {
        add_box(b, v3(-hx - t * 0.5f, h * 0.5f, 0), v3(t * 0.5f, h * 0.5f, hz), 0, wall_col, MAT_GRID, true);
        add_box(b, v3(hx + t * 0.5f, h * 0.5f, 0), v3(t * 0.5f, h * 0.5f, hz), 0, wall_col, MAT_GRID, true);
        add_box(b, v3(0, h * 0.5f, -hz - t * 0.5f), v3(hx, h * 0.5f, t * 0.5f), 0, wall_col, MAT_GRID, true);
        add_box(b, v3(0, h * 0.5f, hz + t * 0.5f), v3(hx, h * 0.5f, t * 0.5f), 0, wall_col, MAT_GRID, true);
    } else if (!b->open_world) {
        // Invisible boundary at the floor edge (collision only, 6 m tall).
        const float bh = 3.0f, bt = 0.5f;
        vec3 c[4] = { v3(-fx - bt, bh, 0), v3(fx + bt, bh, 0), v3(0, bh, -fz - bt), v3(0, bh, fz + bt) };
        vec3 hh[4] = { v3(bt, bh, fz + 2 * bt), v3(bt, bh, fz + 2 * bt), v3(fx + 2 * bt, bh, bt), v3(fx + 2 * bt, bh, bt) };
        for (int i = 0; i < 4; i++) {
            BodyDesc d;
            memset(&d, 0, sizeof(d));
            d.shape.kind = SHAPE_BOX;
            d.shape.half = hh[i];
            d.pos = c[i];
            d.rot = quat_identity();
            d.motion = PM_STATIC;
            d.layer = PL_STATIC;
            d.friction = 0.2f;
            d.material = (u8)b->material;
            add_static_body(b, &d);
        }
    }

    World *w = b->w;
    u32 lc = rgba8(0.15f, 0.15f, 0.15f, 1);
    // Floor meter labels along both axes (every meter) and a coarse 5 m grid of coordinates.
    int ext = (int)fminf(hx, hz) - 1;
    char buf[32];
    for (int m = -ext; m <= ext; m++) {
        if (m == 0) continue;
        snprintf(buf, sizeof(buf), "%d", m);
        add_label(w, v3((float)m + 0.05f, 0.005f, 0.42f), v3(1, 0, 0), v3(0, 0, -1), 0.22f, rgba8(0.08f, 0.08f, 0.08f, 1), buf);   // X axis: black (greys only)
        add_label(w, v3(0.08f, 0.005f, (float)m + 0.12f), v3(1, 0, 0), v3(0, 0, -1), 0.22f, rgba8(0.3f, 0.3f, 0.3f, 1), buf);    // Z axis: dark grey
    }
    for (int x = -ext / 5 * 5; x <= ext; x += 5)
        for (int z = -ext / 5 * 5; z <= ext; z += 5) {
            if (x == 0 || z == 0) continue;
            snprintf(buf, sizeof(buf), "%d,%d", x, z);
            add_label(w, v3((float)x + 0.06f, 0.005f, (float)z + 0.26f), v3(1, 0, 0), v3(0, 0, -1), 0.2f, lc, buf);
        }
    add_label(w, v3(0.1f, 0.005f, 0.3f), v3(1, 0, 0), v3(0, 0, -1), 0.24f, lc, "0,0");
    // Wall height marks every 0.5 m on each wall, at several positions.
    for (int wall = 0; wall < 4 && walls; wall++) {
        for (float along = -20; along <= 20; along += 10) {
            for (int k = 1; k <= (int)(fminf(h, 6.0f) * 2); k++) {
                float y = k * 0.5f;
                snprintf(buf, sizeof(buf), (k % 2) ? "%.1f m" : "%.0f m", y);
                vec3 p;
                float yaw;
                switch (wall) {
                case 0: p = v3(along, y, -hz); yaw = 0; break;              // north wall, inner face +Z
                case 1: p = v3(along, y, hz); yaw = DG_PI; break;           // south wall, inner face -Z
                case 2: p = v3(-hx, y, along); yaw = DG_PI * 0.5f; break;   // west wall, inner face +X
                default: p = v3(hx, y, along); yaw = -DG_PI * 0.5f; break;  // east wall, inner face -X
                }
                p.y += 0.08f;
                add_wall_label(w, p, yaw, (k % 2) ? 0.1f : 0.14f, lc, buf);
            }
        }
    }
}

// Static collision of a vgeo placement (.vgeo files only): its coarsest discrete LOD as a triangle
// mesh per instance, or the mesh's bounding sphere (cheap, for big grids).
static void add_vgeo_collision(World *w, const LevelVgeo *v, const char *mode) {
    size_t l = strlen(v->mesh);
    if (!strcmp(mode, "none") || l < 6 || strcmp(v->mesh + l - 5, ".vgeo") || !w->phys) return;
    char path[512];
    const VgBuilt *b = vgeo_file_get(platform_data_path(v->mesh, path, sizeof(path)));
    if (!b || !b->hdr.lod_count) return;
    u32 cap = v->grid[0] * v->grid[1];
    VgPlacementXf *xf = (VgPlacementXf *)malloc(sizeof(VgPlacementXf) * (cap ? cap : 1));
    float pp[3] = { v->pos.x, v->pos.y, v->pos.z };
    u32 n = vgeo_placement_expand(pp, v->yaw, v->scale, v->scale_jitter, v->spacing, v->grid[0], v->grid[1], v->seed, xf, cap);
    const VgDiscreteLod *lod = &b->lods[b->hdr.lod_count - 1];
    vec3 *verts = NULL;
    if (!strcmp(mode, "mesh")) verts = (vec3 *)malloc(sizeof(vec3) * (lod->vertex_count + 1));
    LevelStatic *ls = &w->level;
    for (u32 k = 0; k < n && ls->body_count < MAX_STATIC_BODIES; k++) {
        BodyDesc d;
        memset(&d, 0, sizeof(d));
        d.motion = PM_STATIC;
        d.layer = PL_STATIC;
        d.friction = 0.8f;
        d.rot = (quat){ xf[k].rot[0], xf[k].rot[1], xf[k].rot[2], xf[k].rot[3] };
        d.pos = v3(xf[k].pos[0], xf[k].pos[1], xf[k].pos[2]);
        if (verts) {
            for (u32 i = 0; i < lod->vertex_count; i++) {
                const float *p = b->lod_vertices[lod->first_vertex + i].pos;
                verts[i] = v3(p[0] * xf[k].scale, p[1] * xf[k].scale, p[2] * xf[k].scale);
            }
            d.shape.kind = SHAPE_MESH;
            d.shape.verts = verts;
            d.shape.vert_count = lod->vertex_count;
            d.shape.indices = b->lod_indices + lod->first_index;
            d.shape.index_count = lod->index_count;
        } else {
            d.shape.kind = SHAPE_SPHERE;
            d.shape.radius = b->hdr.sphere[3] * xf[k].scale * 0.8f;
            d.pos = v3_add(d.pos, quat_rotate(d.rot, v3(b->hdr.sphere[0] * xf[k].scale, b->hdr.sphere[1] * xf[k].scale, b->hdr.sphere[2] * xf[k].scale)));
        }
        BodyId id = phys_add_body(w->phys, &d);
        if (id != PHYS_INVALID_BODY) ls->bodies[ls->body_count++] = id;
    }
    free(verts);
    free(xf);
}

static void spawn_entity(World *w, const cJSON *e) {
    const char *type = js(e, "type", "");
    vec3 pos = jv3(e, "pos", v3(0, 0, 0));
    float yaw = jf(e, "yaw", 0) * DG_DEG2RAD;
    EntityId id = ENT_NONE;
    if (!strcmp(type, "door")) {
        // A door leaf on its own (story levels: entrances in brush walls); "glass": a glass leaf.
        u32 col = jcol(e, "color", rgba8(0.55f, 0.4f, 0.25f, 1));
        if (jb(e, "glass", false)) col = (col & 0x00FFFFFFu) | (DOOR_ALPHA_GLASS << 24);
        id = spawn_door(w, pos, yaw, jf(e, "width", 0.88f), jf(e, "height", 2.08f), jb(e, "locked", false), item_find(w->items, js(e, "key", "")), col);
    } else if (!strcmp(type, "gate")) {
        id = spawn_gate(w, pos, yaw, jf(e, "width", 3), jf(e, "height", 3), jf(e, "lift", 2.9f), jcol(e, "color", rgba8(0.35f, 0.4f, 0.45f, 1)));
    } else if (!strcmp(type, "lever")) {
        id = spawn_lever(w, pos, yaw, js(e, "target", ""));
    } else if (!strcmp(type, "switch")) {
        id = spawn_switch(w, pos, yaw, (u32)jf(e, "group", 1));
        // "preset": a shadow reference condition (shadow_preset) instead of a light group
        const char *preset = js(e, "preset", "");
        if (id && preset[0]) dg_strcpy(w->lever[ENT_INDEX(id)].target_name, sizeof(w->lever[ENT_INDEX(id)].target_name), preset);
    } else if (!strcmp(type, "fire")) {
        // A fire (docs/NIGHT_OPTICS.md): a flickering warm light that draws flames and smoke (game_render.c)
        // and is a 700 C heat source on the thermal scope (game_thermal.c); "radius" = the light's.
        id = spawn_light(w, pos, jv3(e, "color", v3(1.0f, 0.5f, 0.16f)), jf(e, "radius", 9), jf(e, "intensity", 18),
                         LIGHT_GROUP_FIRE | LIGHT_GROUP_NO_SHADOW, jb(e, "on", true));
        if (id) w->light[ENT_INDEX(id)].flicker = jf(e, "flicker", 0.35f);
    } else if (!strcmp(type, "light")) {
        id = spawn_light(w, pos, jv3(e, "color", v3(1, 0.95f, 0.85f)), jf(e, "radius", 12), jf(e, "intensity", 20), (u32)jf(e, "group", 0), jb(e, "on", true));
        // "shadow": {"type": "hard", "resolution": "high", "strength": 0.8, "bias": 1, "normal_bias": 1,
        // "near_plane": 0.2, "culling_mask": 255, "distance": 0} (docs/SHADOWS_UNITY_MODEL.md)
        const cJSON *sh = cJSON_GetObjectItemCaseSensitive(e, "shadow");
        if (id && cJSON_IsObject(sh)) {
            LightComp *lc = &w->light[ENT_INDEX(id)];
            shs_light_default(&lc->shadow, SHADOW_LIGHT_POINT);
            lc->shadow_given = true;
            for (const cJSON *k = sh->child; k; k = k->next) {
                char v[32], err[128];
                if (cJSON_IsString(k)) dg_strcpy(v, sizeof(v), k->valuestring);
                else if (cJSON_IsNumber(k)) snprintf(v, sizeof(v), "%g", k->valuedouble);
                else continue;
                if (!shs_light_set(&lc->shadow, k->string, v, err, sizeof(err))) log_warn("level: light shadow: %s", err);
            }
        }
        if (id && jb(e, "schedule", false)) {
            // light_schedule: on at dusk / off at dawn (assets/world/daynight.json), off until then.
            LightComp *lc = &w->light[ENT_INDEX(id)];
            lc->scheduled = true;
            lc->flicker = jf(e, "flicker", 0.0f);
            lc->sched_state = false;
            w->flags[ENT_INDEX(id)] &= ~(u32)EF_ON;
        }
    } else if (!strcmp(type, "container")) {
        id = spawn_container(w, pos, yaw);
        // "grid": the root container def (data/containers.json, default "crate"); "loot": [[item, count], ...].
        if (js(e, "grid", "")[0]) container_set_root(w, id, js(e, "grid", ""));
        const cJSON *loot = cJSON_GetObjectItemCaseSensitive(e, "loot");
        const cJSON *it;
        cJSON_ArrayForEach(it, loot) {
            if (!cJSON_IsArray(it)) continue;
            u16 item = item_find(w->items, cJSON_GetStringValue(cJSON_GetArrayItem(it, 0)));
            int count = cJSON_GetArraySize(it) > 1 ? cJSON_GetArrayItem(it, 1)->valueint : 1;
            if (item && container_add(w, id, item, count) > 0) log_warn("level: container loot doesn't fit (%s)", item_def(w->items, item)->id);
        }
    } else if (!strcmp(type, "pickup")) {
        u16 item = item_find(w->items, js(e, "item", ""));
        if (!item) { log_warn("level: unknown pickup item '%s'", js(e, "item", "")); return; }
        const WeaponDef *wd = item_weapon(w->items, item);
        id = spawn_pickup(w, pos, item, (int)jf(e, "count", 1), wd ? (i16)wd->mag_size : 0, jf(e, "respawn", 0));
    } else if (!strcmp(type, "prop")) {
        const char *shape = js(e, "shape", "box");
        VisShape vs = !strcmp(shape, "cylinder") ? VIS_CYLINDER : (!strcmp(shape, "sphere") ? VIS_SPHERE : VIS_BOX);
        vec3 half = v3_scale(jv3(e, "size", v3(0.8f, 0.8f, 0.8f)), 0.5f);
        if (vs == VIS_CYLINDER) half = v3(half.x, half.y, half.x);
        id = spawn_prop(w, v3_add(pos, v3(0, half.y, 0)), quat_yaw(yaw), vs, half, jcol(e, "color", rgba8(0.6f, 0.45f, 0.25f, 1)), jf(e, "mass", 20));
        w->body[ENT_INDEX(id)].density = jf(e, "density", 0);   // buoyancy (docs/WATER.md)
        w->vis[ENT_INDEX(id)].model = assets_model_id(js(e, "model", ""));
    } else if (!strcmp(type, "vgeo")) {
        if (w->level.vgeo_count >= MAX_LEVEL_VGEO) { log_warn("level: more than %d vgeo placements", MAX_LEVEL_VGEO); return; }
        LevelVgeo *v = &w->level.vgeo[w->level.vgeo_count++];
        memset(v, 0, sizeof(*v));
        dg_strcpy(v->mesh, sizeof(v->mesh), js(e, "mesh", ""));
        v->pos = pos;
        v->yaw = yaw;
        v->scale = jf(e, "scale", 1.0f);
        v->scale_jitter = jf(e, "scale_jitter", 0.0f);
        v->spacing = jf(e, "spacing", 4.0f);
        v->color = jv3(e, "color", v3(0.6f, 0.6f, 0.6f));
        v->roughness = jf(e, "roughness", 0.8f);
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(e, "grid");
        v->grid[0] = cJSON_IsArray(g) && cJSON_GetArraySize(g) > 0 ? (u32)cJSON_GetArrayItem(g, 0)->valueint : 1;
        v->grid[1] = cJSON_IsArray(g) && cJSON_GetArraySize(g) > 1 ? (u32)cJSON_GetArrayItem(g, 1)->valueint : 1;
        v->seed = (u32)jf(e, "seed", 1);
        {
            // Cast / Receive / layer as vgeo instance flags (vgeo_gpu.h VGI_*)
            u32 mf = level_shadow_flags(e);
            v->shadow_flags = ((mf & MAT_NO_SHADOW) ? 8u : 0u) | ((mf & MAT_SHADOW_ONLY) ? 16u : 0u) | ((mf & MAT_NO_RECEIVE) ? 32u : 0u) | level_layer(e) << 6;
        }
        add_vgeo_collision(w, v, js(e, "collision", v->grid[0] * v->grid[1] > 1 ? "sphere" : "mesh"));
        return;
    } else if (!strcmp(type, "dummy")) {
        id = spawn_dummy(w, pos, yaw, jf(e, "hp", 200));
    } else if (!strcmp(type, "character")) {
        // A dummy driven by a character definition (e.g. the player model holding an item).
        if (!w->authority) return;
        id = spawn_dummy(w, pos, yaw, jf(e, "hp", 300));
        const CharacterDef *cd = g_engine.anim ? anim_lib_char_by_id(g_engine.anim, js(e, "character", "player")) : NULL;
        if (id && cd) {
            char_detach(w, id);
            w->char_id[ENT_INDEX(id)] = cd->net_id;
            char_attach(w, id, cd->net_id);
        }
        const char *item = js(e, "item", NULL);
        if (id && item) w->item[ENT_INDEX(id)] = item_find(w->items, item);
    } else if (!strcmp(type, "zombie")) {
        if (!w->authority) return;
        const CharacterDef *cd = g_engine.anim ? anim_lib_char_by_id(g_engine.anim, js(e, "character", "zombie_placeholder")) : NULL;
        id = spawn_zombie(w, pos, yaw, cd ? cd->net_id : 2);
    } else if (!strcmp(type, "zombie_area")) {
        // rows x cols zombies on a grid, mixed character ids, each wandering around its spot.
        if (!w->authority || !g_engine.anim) return;
        int rows = (int)jf(e, "rows", 4), cols = (int)jf(e, "cols", 5);
        vec3 sp = jv3(e, "spacing", v3(2, 0, 3));
        float radius = jf(e, "radius", 1.8f);
        static const char *kinds[3] = { "zombie_placeholder", "zombie_brute", "zombie_runner" };
        int n = 0;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                const CharacterDef *cd = anim_lib_char_by_id(g_engine.anim, kinds[n % 3]);
                vec3 p = v3_add(pos, v3((float)c * sp.x - (float)(cols - 1) * sp.x * 0.5f, 0, (float)r * sp.z - (float)(rows - 1) * sp.z * 0.5f));
                EntityId z = spawn_zombie(w, p, yaw + (float)n * 0.7f, cd ? cd->net_id : 2);
                CharacterInst *ci = char_get(w, z);
                if (ci) ci->ai.wander_radius = radius;
                n++;
            }
        return;
    } else if (!strcmp(type, "bench")) {
        id = spawn_bench(w, pos, yaw);
    } else if (!strcmp(type, "fabricator")) {
        id = revive_spawn_fabricator(w, pos, yaw, jb(e, "powered", true));   // docs/REVIVE.md
    } else if (!strcmp(type, "marker")) {
        const char *shape = js(e, "shape", "capsule");
        VisShape vs = !strcmp(shape, "box") ? VIS_BOX : (!strcmp(shape, "cylinder") ? VIS_CYLINDER : VIS_CAPSULE);
        vec3 half = jv3(e, "half", v3(0.3f, 0.6f, 0.3f));
        float h = vs == VIS_CAPSULE ? half.y + half.x : half.y;
        id = spawn_marker(w, v3_add(pos, v3(0, h, 0)), quat_yaw(yaw), vs, half, jcol(e, "color", rgba8(0.2f, 0.6f, 0.9f, 1)), jb(e, "solid", true));
    } else {
        log_warn("level: unknown entity type '%s'", type);
        return;
    }
    if (id) dg_strcpy(w->names[ENT_INDEX(id)], sizeof(w->names[0]), js(e, "name", ""));
}

// ------------------------------------------------------------------ level files

static cJSON *read_json(const char *path, bool quiet) {
    char buf[512];
    char *text = platform_read_file(platform_data_path(path, buf, sizeof(buf)), NULL);
    if (!text) {
        if (!quiet) log_error("level: cannot read %s", path);
        return NULL;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) log_error("level: JSON parse error in %s near '%.40s'", path, cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
    return root;
}

static cJSON *read_level(const char *level, void *user) {
    (void)user;
    char rel[160];
    snprintf(rel, sizeof(rel), "data/levels/%s.json", level);
    return read_json(rel, false);
}

cJSON *level_json_load_file(const char *path, bool quiet) {
    cJSON *root = read_json(path, quiet);
    char err[256];
    if (root && !level_include_resolve(root, read_level, NULL, err, sizeof(err))) log_error("level: %s: %s", path, err);
    return root;
}

cJSON *level_json_load(const char *map) {
    if (!map || !map[0]) return NULL;
    char rel[160];
    snprintf(rel, sizeof(rel), "data/levels/%s.json", map);
    return level_json_load_file(rel, true);
}

bool level_load(World *w, const char *path) {
    cJSON *root = level_json_load_file(path, false);
    if (!root) return false;

    LevelStatic *ls = &w->level;
    memset(ls, 0, sizeof(*ls));
    dg_strcpy(ls->name, sizeof(ls->name), js(root, "name", "level"));
    ls->bounds_min = v3s(1e9f);
    ls->bounds_max = v3s(-1e9f);
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(root, "env");
    ls->sun_dir = v3_norm(jv3(env, "sun_dir", v3(0.35f, 1.0f, 0.25f)));
    ls->sun_color = jv3(env, "sun_color", v3(1, 0.96f, 0.9f));
    ls->sun_intensity = jf(env, "sun_intensity", 2.0f);
    ls->ambient_sky = jv3(env, "sky", v3(0.55f, 0.6f, 0.7f));
    ls->ambient_ground = jv3(env, "ground", v3(0.3f, 0.28f, 0.25f));
    ls->fog_color = jv3(env, "fog", v3(0.45f, 0.48f, 0.52f));
    ls->fog_density = jf(env, "fog_density", 0.0f);
    ls->sky = cJSON_GetObjectItemCaseSensitive(env, "sky_zenith") != NULL;
    ls->sky_zenith = jv3(env, "sky_zenith", v3(0.22f, 0.42f, 0.78f));
    ls->sky_horizon = jv3(env, "sky_horizon", v3(0.72f, 0.8f, 0.9f));
    ls->clouds = jf(env, "clouds", 0.4f);

    // Open-world levels: the generated terrain, POIs, water, trees and entities (docs/ZOMBIE_MAP.md).
    if (cJSON_GetObjectItemCaseSensitive(root, "world") && !openworld_load(w, root)) {
        cJSON_Delete(root);
        return false;
    }

    LevelBuild b;
    memset(&b, 0, sizeof(b));
    b.w = w;
    b.surf = v3(0.85f, 0.0f, 0.0f);
    b.open_world = w->openworld != NULL;
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(root, "room");
    if (room) build_room(&b, room);
    const cJSON *br;
    cJSON_ArrayForEach(br, cJSON_GetObjectItemCaseSensitive(root, "brushes")) build_brush(&b, br);

    const cJSON *lab;
    cJSON_ArrayForEach(lab, cJSON_GetObjectItemCaseSensitive(root, "labels")) {
        vec3 p = jv3(lab, "pos", v3(0, 0, 0));
        float size = jf(lab, "size", 0.2f);
        u32 c = jcol(lab, "color", rgba8(0.1f, 0.1f, 0.12f, 1));
        if (jb(lab, "floor", false)) {
            float yaw = jf(lab, "yaw", 0) * DG_DEG2RAD;
            add_label(w, v3_add(p, v3(0, 0.006f, 0)), right_from_yaw(yaw), forward_from_yaw(yaw), size, c, js(lab, "text", ""));
        } else {
            add_wall_label(w, p, jf(lab, "yaw", 0) * DG_DEG2RAD, size, c, js(lab, "text", ""));
        }
    }

    const cJSON *sp;
    cJSON_ArrayForEach(sp, cJSON_GetObjectItemCaseSensitive(root, "spawns")) {
        if (ls->spawn_count >= MAX_SPAWNS) break;
        ls->spawns[ls->spawn_count].pos = jv3(sp, "pos", v3(0, 0, 0));
        ls->spawns[ls->spawn_count].yaw = jf(sp, "yaw", 0) * DG_DEG2RAD;
        ls->spawn_count++;
    }
    if (!ls->spawn_count) { ls->spawns[0].pos = v3(0, 0.1f, 0); ls->spawn_count = 1; }

    if (w->authority) {
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "entities")) spawn_entity(w, e);
        entities_link(w);
    }

    for (int i = 0; i < b.bucket_count; i++) {
        if (ls->draw_count >= MAX_STATIC_DRAWS) break;
        StaticDraw *d = &ls->draws[ls->draw_count++];
        d->mesh = mb_upload(&b.buckets[i]);
        d->mat = material_color(v4(1, 1, 1, 1));
        d->mat.flags = b.bucket_flags[i] & ~(7u << LEVEL_LAYER_SHIFT);
        d->layer = (u8)((b.bucket_flags[i] >> LEVEL_LAYER_SHIFT) & 7u);
        d->mat.roughness = b.bucket_surf[i].x;
        d->mat.metallic = b.bucket_surf[i].y;
        d->mat.emissive = b.bucket_surf[i].z;
        if (d->mat.flags & MAT_GLASS) d->mat.color.w = 0.12f;   // forward blended pass (render.c exec_transparent)
        if (b.bucket_spacing[i] > 0) d->mat.tex_scale = b.bucket_spacing[i];
        d->model = mat4_identity();
        mb_free(&b.buckets[i]);
    }
    kit_world_level(w, root);   // building kit instances + prefabs (docs/BUILDING_KIT.md): the draw / body tail
    // Interior volumes (render_set_interiors, docs/STORY.md "Rendering"): every level sets (or clears) them.
    {
        RenderInterior iv[RENDER_MAX_INTERIORS];
        int ni = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "interiors")) {
            if (ni >= RENDER_MAX_INTERIORS) { log_warn("level: more than %d interiors", RENDER_MAX_INTERIORS); break; }
            RenderInterior *r = &iv[ni++];
            r->mn = jv3(it, "min", v3(0, 0, 0));
            r->mx = jv3(it, "max", v3(0, 0, 0));
            r->sky = jf(it, "sky", 0.2f);
            r->falloff = jf(it, "falloff", 2.5f);
            r->blend = jf(it, "blend", 0.4f);
            r->open_faces = 0;
            static const char *const faces[6] = { "-x", "+x", "-y", "+y", "-z", "+z" };
            const cJSON *g;
            cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(it, "glass"))
                for (int f = 0; f < 6; f++)
                    if (cJSON_IsString(g) && !strcmp(g->valuestring, faces[f])) r->open_faces |= 1u << f;
        }
        render_set_interiors(iv, ni);
        // City windows ("city": {"base", "intensity", "center": [x, z]}); levels without one turn them off.
        const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
        const cJSON *cc = cJSON_GetObjectItemCaseSensitive(city, "center");
        vec2 center = cJSON_IsArray(cc) && cJSON_GetArraySize(cc) >= 2 ? v2((float)cJSON_GetArrayItem(cc, 0)->valuedouble, (float)cJSON_GetArrayItem(cc, 1)->valuedouble) : v2(0, 0);
        render_set_city(jf(city, "base", 0.45f), city ? jf(city, "intensity", 2.5f) : 0.0f, center, NULL);
    }
    phys_optimize(w->phys);
    cJSON_Delete(root);
    log_info("level '%s': %d static bodies, %d labels, %d draws, %d spawns", ls->name, ls->body_count, ls->label_count, ls->draw_count, ls->spawn_count);
    return true;
}

void level_draw_static(const World *w, vec3 cam_pos, vec3 cam_fwd) {
    const LevelStatic *ls = &w->level;
    for (int i = 0; i < ls->draw_count; i++) {
        if (ls->draws[i].cull_dist > 0 && v3_len2(v3_flat(v3_sub(ls->draws[i].cull_center, cam_pos))) > ls->draws[i].cull_dist * ls->draws[i].cull_dist) continue;
        DrawItem d;
        memset(&d, 0, sizeof(d));
        d.mesh = ls->draws[i].mesh;
        d.model = ls->draws[i].model;
        d.mat = ls->draws[i].mat;
        d.layer = ls->draws[i].layer;
        render_draw(&d);
    }
    for (int i = 0; i < ls->label_count; i++) {
        const WorldLabel *l = &ls->labels[i];
        vec3 to = v3_sub(l->pos, cam_pos);
        float d2 = v3_len2(to);
        if (d2 > 22.0f * 22.0f) continue;
        if (d2 > 4.0f && v3_dot(to, cam_fwd) < 0) continue;
        font_text3d(l->pos, l->right, l->up, l->size, l->color, l->text, l->centered);
    }
}
