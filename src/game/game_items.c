// Detailed item models in game (docs/ITEM_PIPELINE.md): hot reload, the inspector and the dev room
// item showcase.
//   item_reload                                  re-read changed .dgim files + assets/items/materials.json, re-bake icons
//   item_inspect <item id | att_<id> | off> [view] orbit any item in front of the camera (world lighting)
//   item_inspect_view <shaded|wire|matid|wear|ao|dirt|sockets>
//   item_inspect_orbit <yaw> <pitch> [zoom]      degrees; zoom 1 = the item fills ~2/3 of the view height
//   item_inspect_spin <deg/s>                    turntable speed (0 = still)
//   item_inspect_lod <0..2|auto>                 force a LOD (the wireframe shows its triangles)
//   item_showcase                                teleport to the dev room showcase (turntables + lighting rig)
#include "game_internal.h"
#include "assets/item_mesh.h"
#include "gameplay/attachments.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "player/hands.h"
#include "ui/icons.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

typedef enum InspectView { IV_SHADED, IV_WIRE, IV_MATID, IV_WEAR, IV_AO, IV_DIRT, IV_SOCKETS, IV_COUNT } InspectView;
static const char *k_views[IV_COUNT] = { "shaded", "wire", "matid", "wear", "ao", "dirt", "sockets" };

static struct {
    bool on;
    char id[40];
    ItemMesh *mesh;
    int condition;
    int view;
    float yaw, pitch, zoom, spin;
    int lod;                 // -1 = auto (0: the inspector is close)
} IN = { .zoom = 1.0f, .spin = 20.0f, .lod = -1, .yaw = 30.0f, .pitch = -15.0f };

// The showcase: turntables in rows, west of the horde yard (data/levels/devroom.json "item showcase").
static const vec3 k_showcase = { -62.0f, 0.0f, 52.0f };
#define SHOWCASE_COLS 8
#define SHOWCASE_SPACING 2.2f

static ItemMesh *mesh_for_id(const char *id, int *cond) {
    const ItemDB *db = &G->items;
    if (!strncmp(id, "att_", 4)) {
        for (int a = 1; a <= db->attachment_count; a++)
            if (!strcmp(db->attachments[a].id, id + 4)) { *cond = db->attachments[a].model.condition; return item_mesh_get(db->attachments[a].model.mesh); }
    }
    u16 it = item_find(db, id);
    if (!it) {   // a mesh with no item of its own (a "mesh_for" variant): by file name
        char path[96];
        snprintf(path, sizeof(path), "assets/items/meshes/%s.dgim", id);
        *cond = ITEM_COND_WORN;
        return item_mesh_get(path);
    }
    const WeaponDef *wd = item_weapon(db, it);
    const ItemModelRef *mr = wd ? &wd->model : &db->items[it].model;
    if (db->items[it].kind == ITEM_ATTACHMENT) {
        const AttachmentDef *ad = attach_def(db, db->items[it].attachment);
        if (ad) mr = &ad->model;
    }
    *cond = mr->condition;
    return item_mesh_get(mr->mesh);
}

static void cmd_item_reload(int argc, const char **argv) {
    int n = item_mesh_reload_all(argc > 1 && !strcmp(argv[1], "all"));
    icons_rerender();
    log_info("item_reload: %d mesh(es) re-read, materials re-layered, icons re-rendering (bake_icons writes the PNGs)", n);
    game_add_hint("Item meshes reloaded");
}

static void cmd_item_inspect(int argc, const char **argv) {
    if (argc < 2 || !strcmp(argv[1], "off")) {
        IN.on = false;
        item_mesh_set_debug_view(0);
        return;
    }
    int cond = ITEM_COND_WORN;
    ItemMesh *m = mesh_for_id(argv[1], &cond);
    if (!m) { log_info("item_inspect: '%s' has no detailed mesh (item id, or att_<attachment id>)", argv[1]); return; }
    IN.on = true;
    IN.mesh = m;
    IN.condition = cond;
    dg_strcpy(IN.id, sizeof(IN.id), argv[1]);
    if (argc > 2)
        for (int v = 0; v < IV_COUNT; v++)
            if (!strcmp(argv[2], k_views[v])) IN.view = v;
    item_mesh_set_debug_view(IN.view == IV_MATID ? 1 : IN.view == IV_WEAR ? 2 : IN.view == IV_AO ? 3 : IN.view == IV_DIRT ? 4 : 0);
    log_info("item_inspect %s: %d parts, %d sockets, %u / %u / %u tris, bounds %.3f x %.3f x %.3f m", argv[1], m->part_count, m->socket_count, m->tris[0], m->tris[1],
             m->tris[2], m->bmax.x - m->bmin.x, m->bmax.y - m->bmin.y, m->bmax.z - m->bmin.z);
}

static void cmd_item_inspect_view(int argc, const char **argv) {
    if (argc < 2) { log_info("item_inspect_view: %s (shaded wire matid wear ao dirt sockets)", k_views[IN.view]); return; }
    for (int v = 0; v < IV_COUNT; v++)
        if (!strcmp(argv[1], k_views[v])) IN.view = v;
    item_mesh_set_debug_view(IN.view == IV_MATID ? 1 : IN.view == IV_WEAR ? 2 : IN.view == IV_AO ? 3 : IN.view == IV_DIRT ? 4 : 0);
}

static void cmd_item_inspect_orbit(int argc, const char **argv) {
    if (argc < 3) { log_info("item_inspect_orbit %.0f %.0f %.2f (yaw pitch [zoom])", IN.yaw, IN.pitch, IN.zoom); return; }
    IN.yaw = (float)atof(argv[1]);
    IN.pitch = (float)atof(argv[2]);
    if (argc > 3) IN.zoom = fmaxf(0.1f, (float)atof(argv[3]));
    IN.spin = 0;
}

static void cmd_item_inspect_spin(int argc, const char **argv) { IN.spin = argc > 1 ? (float)atof(argv[1]) : 20.0f; }
static void cmd_item_inspect_lod(int argc, const char **argv) { IN.lod = argc > 1 && strcmp(argv[1], "auto") ? DG_CLAMP(atoi(argv[1]), 0, ITEM_MESH_LODS - 1) : -1; }

static void cmd_item_showcase(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (strcmp(G->map, "devroom") && strcmp(G->map, "devroom_vegetation")) { log_info("item_showcase: the showcase is in the dev room"); return; }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "setpos %g 0 %g 180 -8\n", k_showcase.x + SHOWCASE_COLS * SHOWCASE_SPACING * 0.5f, k_showcase.z - 5.0f);
    cbuf_add(cmd);
}

// Wireframe of one LOD from the CPU copy (edges drawn once per triangle; capped).
static void draw_wire(const ItemMesh *m, int lod, vec3 pos, quat rot) {
    int budget = 150000;
    for (int p = 0; p < m->part_count && budget > 0; p++) {
        const DgimPart *dp = &m->dparts[p];
        int l = DG_MIN(lod, (int)dp->lod_count - 1);
        if (l < 0) continue;
        Xform x = item_mesh_part_xform(m, p, NULL);
        for (u32 t = 0; t + 2 < dp->lod_in[l] && budget > 0; t += 3, budget -= 3) {
            vec3 v[3];
            for (int k = 0; k < 3; k++) {
                const DgimVertex *s = &m->verts[dp->lod_v0[l] + m->indices[dp->lod_i0[l] + t + k]];
                v[k] = v3_add(pos, quat_rotate(rot, xf_point(x, item_mesh_vertex_pos(dp, s))));
            }
            dbg_line(v[0], v[1], 0xFF40E0FFu, 0, true);
            dbg_line(v[1], v[2], 0xFF40E0FFu, 0, true);
            dbg_line(v[2], v[0], 0xFF40E0FFu, 0, true);
        }
    }
}

static void draw_sockets(const ItemMesh *m, vec3 pos, quat rot, float size) {
    for (int i = 0; i < m->socket_count; i++) {
        Xform s;
        if (!item_mesh_socket(m, m->sockets[i].name, &s)) continue;
        vec3 p = v3_add(pos, quat_rotate(rot, s.t));
        quat r = quat_mul(rot, s.r);
        dbg_line(p, v3_add(p, quat_rotate(r, v3(size, 0, 0))), DBG_RED, 0, false);
        dbg_line(p, v3_add(p, quat_rotate(r, v3(0, size, 0))), DBG_GREEN, 0, false);
        dbg_line(p, v3_add(p, quat_rotate(r, v3(0, 0, -size))), DBG_BLUE, 0, false);
        dbg_text(v3_add(p, v3(0, size * 0.3f, 0)), m->sockets[i].name, DBG_YELLOW, 0.6f, 0);
    }
}

static void draw_inspector(const RenderCamera *cam) {
    ItemMesh *m = IN.mesh;
    if (!m || !m->loaded) return;
    IN.yaw += IN.spin * (float)g_engine.frame_dt;
    quat cam_rot = quat_euler(cam->yaw, cam->pitch, 0);
    vec3 fwd = dir_from_angles(cam->yaw, cam->pitch);
    vec3 ext = v3_sub(m->bmax, m->bmin), ctr = v3_scale(v3_add(m->bmax, m->bmin), 0.5f);
    float radius = fmaxf(0.02f, v3_len(ext) * 0.5f);
    float dist = radius / tanf(fmaxf(cam->fov_deg, 10.0f) * 0.5f * DG_DEG2RAD) * 1.35f / IN.zoom;
    dist = fmaxf(dist, cam->znear * 2.0f + radius);
    // Item frame: the camera's, turned by yaw (about the camera up) and pitch (about its right).
    quat rot = quat_mul(cam_rot, quat_mul(quat_axis_angle(v3(1, 0, 0), IN.pitch * DG_DEG2RAD), quat_axis_angle(v3(0, 1, 0), IN.yaw * DG_DEG2RAD)));
    vec3 target = v3_add(cam->pos, v3_scale(fwd, dist));
    vec3 pos = v3_sub(target, quat_rotate(rot, ctr));
    // A key light over the camera's shoulder so the inspector reads in any room or time of day.
    PointLight key = { 0 };
    key.pos = v3_add(cam->pos, quat_rotate(cam_rot, v3(-0.6f, 0.7f, 0.2f)));
    key.radius = dist * 6.0f + 2.0f;
    key.color = v3(1.0f, 0.95f, 0.88f);
    key.intensity = 3.0f;
    key.flags = LIGHT_NO_SHADOW;
    render_add_light(&key);
    ItemMeshPose pose = { 0 };
    pose.wobble = quat_identity();
    if (IN.view != IV_WIRE || IN.lod < 0) item_mesh_draw(m, item_mesh_condition(IN.condition), pos, rot, 1.0f, &pose, MAT_NO_SHADOW, false);
    if (IN.view == IV_WIRE) {
        if (IN.lod >= 0) item_mesh_draw(m, item_mesh_condition(IN.condition), pos, rot, 1.0f, &pose, MAT_NO_SHADOW, false);
        draw_wire(m, IN.lod < 0 ? 0 : IN.lod, pos, rot);
    }
    if (IN.view == IV_SOCKETS) draw_sockets(m, pos, rot, radius * 0.12f);
}

// Every item / attachment with a detailed mesh, once, in a stable order (weapons, attachments, items).
static int showcase_list(ItemMesh **out, const char **names, int *conds, int max) {
    const ItemDB *db = &G->items;
    int n = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 1; i < db->item_count && n < max; i++) {
            const ItemDef *it = &db->items[i];
            const WeaponDef *wd = item_weapon(db, (u16)i);
            if ((pass == 0) != (wd != NULL) || it->kind == ITEM_ATTACHMENT) continue;
            if (pass == 1) continue;
            const ItemModelRef *mr = wd ? &wd->model : &it->model;
            ItemMesh *m = item_mesh_get(mr->mesh);
            if (!m) continue;
            out[n] = m, names[n] = it->id, conds[n] = mr->condition, n++;
        }
        if (pass == 1)
            for (int a = 1; a <= db->attachment_count && n < max; a++) {
                ItemMesh *m = item_mesh_get(db->attachments[a].model.mesh);
                if (!m) continue;
                out[n] = m, names[n] = db->attachments[a].id, conds[n] = db->attachments[a].model.condition, n++;
            }
    }
    return n;
}

static void draw_box(vec3 c, vec3 half, vec4 col, float rough, float metal) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.mesh = G->meshes.cube;
    d.model = mat4_trs(c, quat_identity(), v3_scale(half, 2.0f));
    d.mat = material_color(col);
    d.mat.roughness = rough;
    d.mat.metallic = metal;
    render_draw(&d);
}

static void draw_showcase(const RenderCamera *cam) {
    if (strcmp(G->map, "devroom") && strcmp(G->map, "devroom_vegetation")) return;
    if (v3_len(v3_sub(cam->pos, k_showcase)) > 70.0f) return;
    ItemMesh *meshes[128];
    const char *names[128];
    int conds[128];
    int n = showcase_list(meshes, names, conds, 128);
    float spin = (float)fmod(g_engine.real_time * 0.5, 2.0 * DG_PI);
    for (int i = 0; i < n; i++) {
        int row = i / SHOWCASE_COLS, col = i % SHOWCASE_COLS;
        vec3 base = v3(k_showcase.x + col * SHOWCASE_SPACING, 0, k_showcase.z + row * SHOWCASE_SPACING * 1.4f);
        // Turntable: a plinth, a dark top disc stand-in (box), the item centred 25 cm above it.
        draw_box(v3(base.x, 0.45f, base.z), v3(0.45f, 0.45f, 0.45f), v4(0.16f, 0.16f, 0.17f, 1), 0.8f, 0.0f);
        draw_box(v3(base.x, 0.915f, base.z), v3(0.4f, 0.015f, 0.4f), v4(0.05f, 0.05f, 0.055f, 1), 0.35f, 0.6f);
        ItemMesh *m = meshes[i];
        vec3 ctr = v3_scale(v3_add(m->bmax, m->bmin), 0.5f);
        float r = v3_len(v3_scale(v3_sub(m->bmax, m->bmin), 0.5f));
        float scale = clampf(0.3f / fmaxf(r, 1e-3f), 0.5f, 6.0f);   // one display size: long guns shrink, a pill bottle grows (the label says by how much)
        quat rot = quat_axis_angle(v3(0, 1, 0), spin + i * 0.7f);
        vec3 c = v3(base.x, 0.93f + 0.06f + r * scale, base.z);
        vec3 pos = v3_sub(c, quat_rotate(rot, v3_scale(ctr, scale)));
        ItemMeshPose pose = { 0 };
        pose.wobble = quat_identity();
        item_mesh_draw(m, item_mesh_condition(conds[i]), pos, rot, scale, &pose, 0, false);
        char label[64];
        snprintf(label, sizeof(label), "%s  %uk tris  x%.1f", names[i], (m->tris[0] + 500) / 1000, scale);
        dbg_text(v3(base.x, 0.86f, base.z - 0.47f), label, DBG_WHITE, 0.07f, 0);
    }
    // Lighting rig: warm key / cool fill / rim per row.
    int rows = (n + SHOWCASE_COLS - 1) / SHOWCASE_COLS;
    for (int r = 0; r < rows; r++) {
        float z = k_showcase.z + r * SHOWCASE_SPACING * 1.4f;
        float x0 = k_showcase.x, x1 = k_showcase.x + (SHOWCASE_COLS - 1) * SHOWCASE_SPACING;
        PointLight l = { 0 };
        l.radius = 9.0f;
        l.flags = LIGHT_NO_SHADOW;
        l.pos = v3(x0 + (x1 - x0) * 0.3f, 2.6f, z - 1.6f), l.color = v3(1.0f, 0.93f, 0.82f), l.intensity = 6.0f;
        render_add_light(&l);
        l.pos = v3(x0 + (x1 - x0) * 0.75f, 1.8f, z - 1.8f), l.color = v3(0.7f, 0.8f, 1.0f), l.intensity = 2.5f;
        render_add_light(&l);
        l.pos = v3((x0 + x1) * 0.5f, 2.2f, z + 1.2f), l.color = v3(1.0f, 0.85f, 0.7f), l.intensity = 3.0f;
        render_add_light(&l);
    }
}

// Near-plane check (tools/tests/item_nearplane.cfg): the LOD 0 triangles of the first-person weapon and its
// attachments, posed as drawn, in view space. A visible clip is an edge crossing the camera near plane
// inside the view's near rectangle (a stock behind the eye, outside the frustum, is never seen).
typedef struct NearStats { int cuts, behind, total; float min_depth; int worst_cuts; char worst[40]; } NearStats;

static void nearplane_mesh(ItemMesh *m, Xform root, const ItemMeshPose *pose, mat4 view, float zn, float tx, float ty, NearStats *st) {
    for (int p = 0; p < m->part_count; p++) {
        const DgimPart *dp = &m->dparts[p];
        if (dp->lod_count == 0) continue;
        bool hidden = false;
        for (int q = p; q >= 0; q = m->parts[q].parent)
            if (pose && m->parts[q].hide_slot >= 0 && (pose->hide_slots >> m->parts[q].hide_slot & 1u)) hidden = true;
        if (hidden) continue;
        Xform x = xf_mul(root, item_mesh_part_xform(m, p, pose));
        vec3 *vs = malloc(sizeof(vec3) * dp->lod_vn[0]);
        for (u32 i = 0; i < dp->lod_vn[0]; i++) {
            vec3 w = render_viewmodel_to_world(xf_point(x, item_mesh_vertex_pos(dp, &m->verts[dp->lod_v0[0] + i])));
            vs[i] = mat4_mul_point(view, w);
            float depth = -vs[i].z;
            st->min_depth = fminf(st->min_depth, depth);
            st->total++;
            if (depth < zn) st->behind++;
        }
        const u16 *ix = m->indices + dp->lod_i0[0];
        int before = st->cuts;
        for (u32 t = 0; t + 2 < dp->lod_in[0]; t += 3)
            for (int e = 0; e < 3; e++) {
                vec3 a = vs[ix[t + e]], b = vs[ix[t + (e + 1) % 3]];
                float da = -a.z - zn, db = -b.z - zn;
                if ((da < 0) == (db < 0)) continue;
                vec3 c = v3_lerp(a, b, da / (da - db));
                if (fabsf(c.x) <= zn * tx && fabsf(c.y) <= zn * ty) st->cuts++;
            }
        if (st->cuts - before > st->worst_cuts) {
            st->worst_cuts = st->cuts - before;
            snprintf(st->worst, sizeof(st->worst), "%s/%s", m->id, m->parts[p].name);
        }
        free(vs);
    }
}

static void cmd_item_nearplane(int argc, const char **argv) {
    vec3 pos;
    quat rot;
    const WeaponDef *wd;
    const char *state = argc > 1 ? argv[1] : "now";
    if (!hands_fp_item(&pos, &rot, &wd) || !wd) { log_info("item_nearplane %s: no first-person weapon", state); return; }
    ItemMesh *m = item_mesh_get(wd->model.mesh);
    if (!m) { log_info("item_nearplane %s %s: no mesh", wd->id, state); return; }
    mat4 view = render_view_matrix();
    vec3 cp;
    float fov;
    render_camera_info(&cp, &fov);
    float zn = render_camera_znear(), ty = tanf(fov * 0.5f * DG_DEG2RAD), tx = ty * (float)render_width() / (float)DG_MAX(render_height(), 1);
    NearStats st = { 0, 0, 0, 1e9f, 0, "" };
    ItemMeshPose pose = { 0 };
    pose.wobble = quat_identity();
    for (int s = 0; s < ATT_SLOTS; s++)
        if (wd->att_applied[s]) pose.hide_slots |= (u8)(1u << s);
    pose.fold_slots = pose.hide_slots;
    nearplane_mesh(m, xf_make(pos, rot), &pose, view, zn, tx, ty, &st);
    for (int s = 0; s < ATT_SLOTS; s++) {
        const AttachmentDef *a = attach_def(&G->items, wd->att_applied[s]);
        ItemMesh *am = a ? item_mesh_get(attach_model_for(a, wd)->mesh) : NULL;
        if (am) nearplane_mesh(am, xf_mul(xf_make(pos, rot), attach_placement(wd, a)), NULL, view, zn, tx, ty, &st);
    }
    log_info("item_nearplane %s %s: %d visible near-plane cuts (%d / %d vertices nearer than %.3f m, outside the view), nearest %.3f m -> %s%s%s", wd->id, state, st.cuts,
             st.behind, st.total, zn, st.min_depth, st.cuts ? "CLIPS" : "ok", st.cuts ? ", most in " : "", st.worst);
}

void items_game_register(void) {
    item_mesh_register();
    cmd_register("item_reload", cmd_item_reload, "item_reload [all]: re-read changed item meshes (.dgim) and assets/items/materials.json, re-bake icons");
    cmd_register("item_inspect", cmd_item_inspect, "item_inspect <item id | att_<id> | off> [shaded|wire|matid|wear|ao|dirt|sockets]: orbit an item in front of the camera");
    cmd_register("item_inspect_view", cmd_item_inspect_view, "item_inspect_view <shaded|wire|matid|wear|ao|dirt|sockets>: inspector view");
    cmd_register("item_inspect_orbit", cmd_item_inspect_orbit, "item_inspect_orbit <yaw> <pitch> [zoom]: inspector orbit (degrees), stops the spin");
    cmd_register("item_inspect_spin", cmd_item_inspect_spin, "item_inspect_spin [deg/s]: inspector turntable speed (0 = still)");
    cmd_register("item_inspect_lod", cmd_item_inspect_lod, "item_inspect_lod <0..2|auto>: inspector LOD (the wireframe shows its triangles)");
    cmd_register("item_nearplane", cmd_item_nearplane, "item_nearplane [label]: count the first-person weapon's vertices inside the camera near plane (tools/tests/item_nearplane.cfg)");
    cmd_register("item_showcase", cmd_item_showcase, "item_showcase: go to the dev room item showcase (every detailed item on a turntable)");
}

void items_game_draw(const RenderCamera *cam) {
    if (!G || !G->world) return;
    draw_showcase(cam);
    if (IN.on) draw_inspector(cam);
}

void items_game_shutdown(void) { item_mesh_shutdown(); }
