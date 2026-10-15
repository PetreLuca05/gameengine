#include "level.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/mesh_gen.h"
#include "ui/font.h"
#include "gameplay/entities.h"
#include "cJSON.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define MAX_BUCKETS 4

typedef struct LevelBuild {
    World *w;
    MeshBuilder buckets[MAX_BUCKETS];
    u32 bucket_flags[MAX_BUCKETS];
    int bucket_count;
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
    for (int i = 0; i < b->bucket_count; i++) if (b->bucket_flags[i] == flags) return &b->buckets[i];
    if (b->bucket_count >= MAX_BUCKETS) return &b->buckets[0];
    int i = b->bucket_count++;
    mb_init(&b->buckets[i]);
    b->bucket_flags[i] = flags;
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
    mb_box_transformed(bucket(b, flags), mat4_trs(center, rot, v3s(1)), half, color);
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
    add_static_body(b, &bd);
}

static void build_brush(LevelBuild *b, const cJSON *br) {
    const char *type = js(br, "type", "box");
    vec3 pos = jv3(br, "pos", v3(0, 0, 0));
    float yaw = jf(br, "yaw", 0) * DG_DEG2RAD;
    u32 color = jcol(br, "color", rgba8(0.6f, 0.6f, 0.62f, 1));
    u32 flags = jb(br, "grid", true) ? MAT_GRID : 0;
    if (!jb(br, "shadow", true)) flags |= MAT_NO_SHADOW;
    World *w = b->w;
    u32 label_col = rgba8(0.08f, 0.08f, 0.1f, 1);
    const char *label = js(br, "label", NULL);

    if (!strcmp(type, "box")) {
        vec3 size = jv3(br, "size", v3(1, 1, 1));
        vec3 half = v3_scale(size, 0.5f);
        vec3 center = v3_add(pos, v3(0, half.y, 0));
        add_box(b, center, half, yaw, color, flags, jb(br, "collide", true));
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
    add_box(b, v3(0, -t * 0.5f, 0), v3(hx + t, t * 0.5f, hz + t), 0, floor_col, MAT_GRID, true);
    add_box(b, v3(0, h + t * 0.5f, 0), v3(hx + t, t * 0.5f, hz + t), 0, wall_col, MAT_GRID | MAT_NO_SHADOW, true);
    add_box(b, v3(-hx - t * 0.5f, h * 0.5f, 0), v3(t * 0.5f, h * 0.5f, hz), 0, wall_col, MAT_GRID, true);
    add_box(b, v3(hx + t * 0.5f, h * 0.5f, 0), v3(t * 0.5f, h * 0.5f, hz), 0, wall_col, MAT_GRID, true);
    add_box(b, v3(0, h * 0.5f, -hz - t * 0.5f), v3(hx, h * 0.5f, t * 0.5f), 0, wall_col, MAT_GRID, true);
    add_box(b, v3(0, h * 0.5f, hz + t * 0.5f), v3(hx, h * 0.5f, t * 0.5f), 0, wall_col, MAT_GRID, true);

    World *w = b->w;
    u32 lc = rgba8(0.15f, 0.15f, 0.18f, 1);
    // Floor meter labels along both axes (every meter) and a coarse 5 m grid of coordinates.
    int ext = (int)fminf(hx, hz) - 1;
    char buf[32];
    for (int m = -ext; m <= ext; m++) {
        if (m == 0) continue;
        snprintf(buf, sizeof(buf), "%d", m);
        add_label(w, v3((float)m + 0.05f, 0.005f, 0.42f), v3(1, 0, 0), v3(0, 0, -1), 0.22f, rgba8(0.6f, 0.1f, 0.1f, 1), buf);
        add_label(w, v3(0.08f, 0.005f, (float)m + 0.12f), v3(1, 0, 0), v3(0, 0, -1), 0.22f, rgba8(0.1f, 0.15f, 0.6f, 1), buf);
    }
    for (int x = -ext / 5 * 5; x <= ext; x += 5)
        for (int z = -ext / 5 * 5; z <= ext; z += 5) {
            if (x == 0 || z == 0) continue;
            snprintf(buf, sizeof(buf), "%d,%d", x, z);
            add_label(w, v3((float)x + 0.06f, 0.005f, (float)z + 0.26f), v3(1, 0, 0), v3(0, 0, -1), 0.2f, lc, buf);
        }
    add_label(w, v3(0.1f, 0.005f, 0.3f), v3(1, 0, 0), v3(0, 0, -1), 0.24f, lc, "0,0");
    // Wall height marks every 0.5 m on each wall, at several positions.
    for (int wall = 0; wall < 4; wall++) {
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

static void spawn_entity(World *w, const cJSON *e) {
    const char *type = js(e, "type", "");
    vec3 pos = jv3(e, "pos", v3(0, 0, 0));
    float yaw = jf(e, "yaw", 0) * DG_DEG2RAD;
    EntityId id = ENT_NONE;
    if (!strcmp(type, "gate")) {
        id = spawn_gate(w, pos, yaw, jf(e, "width", 3), jf(e, "height", 3), jf(e, "lift", 2.9f), jcol(e, "color", rgba8(0.35f, 0.4f, 0.45f, 1)));
    } else if (!strcmp(type, "lever")) {
        id = spawn_lever(w, pos, yaw, js(e, "target", ""));
    } else if (!strcmp(type, "switch")) {
        id = spawn_switch(w, pos, yaw, (u32)jf(e, "group", 1));
    } else if (!strcmp(type, "light")) {
        id = spawn_light(w, pos, jv3(e, "color", v3(1, 0.95f, 0.85f)), jf(e, "radius", 12), jf(e, "intensity", 20), (u32)jf(e, "group", 0), jb(e, "on", true));
    } else if (!strcmp(type, "container")) {
        id = spawn_container(w, pos, yaw);
        const cJSON *loot = cJSON_GetObjectItemCaseSensitive(e, "loot");
        const cJSON *it;
        int slot = 0;
        cJSON_ArrayForEach(it, loot) {
            if (slot >= CONTAINER_SLOTS || !cJSON_IsArray(it)) break;
            u16 item = item_find(w->items, cJSON_GetArrayItem(it, 0)->valuestring);
            int count = cJSON_GetArraySize(it) > 1 ? cJSON_GetArrayItem(it, 1)->valueint : 1;
            if (!item) continue;
            const WeaponDef *wd = item_weapon(w->items, item);
            InvSlot *s = &w->cont[ENT_INDEX(id)].slots[slot++];
            s->item = item;
            s->count = (u16)count;
            s->ammo = wd ? (i16)wd->mag_size : 0;
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
    } else if (!strcmp(type, "dummy")) {
        id = spawn_dummy(w, pos, yaw, jf(e, "hp", 200));
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

bool level_load(World *w, const char *path) {
    char buf[512];
    char *text = platform_read_file(platform_data_path(path, buf, sizeof(buf)), NULL);
    if (!text) { log_error("level: cannot read %s", path); return false; }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) { log_error("level: JSON parse error in %s near '%.40s'", path, cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?"); return false; }

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

    LevelBuild b;
    memset(&b, 0, sizeof(b));
    b.w = w;
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
        d->mat.flags = b.bucket_flags[i];
        d->mat.roughness = 0.85f;
        d->model = mat4_identity();
        mb_free(&b.buckets[i]);
    }
    phys_optimize(w->phys);
    cJSON_Delete(root);
    log_info("level '%s': %d static bodies, %d labels, %d draws, %d spawns", ls->name, ls->body_count, ls->label_count, ls->draw_count, ls->spawn_count);
    return true;
}

void level_draw_static(const World *w, vec3 cam_pos, vec3 cam_fwd) {
    const LevelStatic *ls = &w->level;
    for (int i = 0; i < ls->draw_count; i++) {
        DrawItem d;
        d.mesh = ls->draws[i].mesh;
        d.model = ls->draws[i].model;
        d.mat = ls->draws[i].mat;
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
