// Detailed item meshes (docs/ITEM_PIPELINE.md): the C loader on every shipped .dgim, material layering
// for every condition (vertex colours / (roughness, metallic) in range), draw groups and posing (slide,
// fold, hide), and the meshes' sockets against the gameplay data as the game itself derives it
// (items.c: muzzle from the box model, sight from "iron_sights", support hand; attachments.c: mount,
// sight point, lens), so ADS alignment and the hand glue still hold with the meshes swapped in.
#include "assets/item_mesh.h"
#include "gameplay/items.h"
#include "gameplay/attachments.h"
#include "core/log.h"
#include "core/cvar.h"
#include "core/engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_checks, g_fails;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(c)) {                                                      \
            g_fails++;                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
        }                                                                \
    } while (0)

// ------------------------------------------------------------------ renderer / console stubs
static int s_meshes, s_draws, s_bad_verts;
MeshId render_mesh_create(const Vertex *v, u32 vn, const u32 *idx, u32 in) {
    for (u32 i = 0; i < vn; i++) {
        float r = v[i].uv.x, m = v[i].uv.y, nl = v3_len(v[i].normal);
        if (!(r >= 0 && r <= 1 && m >= 0 && m <= 1) || fabsf(nl - 1.0f) > 0.02f || !isfinite(v[i].pos.x)) s_bad_verts++;
    }
    for (u32 i = 0; i < in; i++)
        if (idx[i] >= vn) s_bad_verts++;
    return (MeshId)++s_meshes;
}
void render_mesh_destroy(MeshId id) { (void)id; }
void render_draw(const DrawItem *d) { (void)d; s_draws++; }
void render_draw_viewmodel(const DrawItem *d) { (void)d; s_draws++; }
void render_camera_info(vec3 *pos, float *fov) { *pos = v3(0, 0, 0); *fov = 75.0f; }
void cmd_register(const char *name, CmdFn fn, const char *desc) { (void)name; (void)fn; (void)desc; }

static float dist(vec3 a, vec3 b) { return v3_len(v3_sub(a, b)); }

static void test_meshes(const ItemDB *db) {
    int n = 0;
    for (int i = 1; i < db->item_count; i++) {
        const ItemDef *it = &db->items[i];
        const WeaponDef *wd = item_weapon(db, (u16)i);
        const ItemModelRef *mr = wd ? &wd->model : &it->model;
        if (it->kind == ITEM_ATTACHMENT) continue;
        CHECK(mr->mesh[0], "%s has no \"mesh\" (every item ships a detailed model)", it->id);
        if (!mr->mesh[0]) continue;
        CHECK(mr->has_bounds, "%s: mesh %s header unreadable", it->id, mr->mesh);
        ItemMesh *m = item_mesh_get(mr->mesh);
        CHECK(m != NULL, "%s: %s does not load", it->id, mr->mesh);
        if (!m) continue;
        n++;
        CHECK(m->lod_count == ITEM_MESH_LODS && m->tris[0] > m->tris[1] && m->tris[1] > m->tris[2], "%s: LODs %u / %u / %u", it->id, m->tris[0], m->tris[1], m->tris[2]);
        Xform g;
        CHECK(item_mesh_socket(m, "grip_r", &g) && v3_len(g.t) < 1e-4f, "%s: grip_r is not the item origin", it->id);
        // Every condition composes (the material library resolves every slot, values in range).
        s_bad_verts = 0;
        for (int c = 0; c < ITEM_COND_COUNT; c++) item_mesh_draw(m, c, v3(0, 0, 0), quat_identity(), 1.0f, NULL, 0, true);
        CHECK(s_bad_verts == 0, "%s: %d composed vertices out of range", it->id, s_bad_verts);
    }
    CHECK(n >= 40, "only %d item meshes", n);
}

// Weapons: the mesh sockets where the game puts them (items.c), the mech parts the fire animation drives.
static void test_weapon_sockets(const ItemDB *db) {
    for (int w = 0; w < db->weapon_count; w++) {
        const WeaponDef *wd = &db->weapons[w];
        ItemMesh *m = item_mesh_get(wd->model.mesh);
        CHECK(m != NULL, "weapon %s: no mesh", wd->id);
        if (!m) continue;
        Xform s;
        if (wd->kind != WPN_MELEE) {
            CHECK(item_mesh_socket(m, "muzzle", &s) && dist(s.t, wd->muzzle_offset) < 0.002f, "%s: muzzle socket %.4f %.4f %.4f vs the game's %.4f %.4f %.4f", wd->id, s.t.x,
                  s.t.y, s.t.z, wd->muzzle_offset.x, wd->muzzle_offset.y, wd->muzzle_offset.z);
            CHECK(item_mesh_socket(m, "sight", &s) && dist(s.t, wd->iron_sight) < 0.002f, "%s: sight socket off the iron sight line (ADS alignment)", wd->id);
            for (int k = 0; k < ATT_SLOTS; k++) {
                static const char *names[ATT_SLOTS] = { "optic", "muzzle", "underbarrel", "magazine", "stock" };
                if (!wd->att_slots[k].present || k == ATT_MUZZLE) continue;
                CHECK(item_mesh_socket(m, names[k], &s) && dist(s.t, wd->att_slots[k].socket.t) < 0.002f, "%s: %s slot socket", wd->id, names[k]);
            }
        }
        if (wd->sockets.has_left)
            CHECK(item_mesh_socket(m, "grip_l", &s) && dist(s.t, wd->sockets.grip_l.t) < 0.002f, "%s: grip_l %.4f %.4f %.4f vs %.4f %.4f %.4f", wd->id, s.t.x, s.t.y, s.t.z,
                  wd->sockets.grip_l.t.x, wd->sockets.grip_l.t.y, wd->sockets.grip_l.t.z);
        // Every mech role of the box model exists as a moving mesh part.
        for (int i = 0; i < wd->part_count; i++) {
            int mech = wd->parts[i].mech;
            if (!mech) continue;
            bool found = false;
            for (int p = 0; p < m->part_count; p++) found |= m->parts[p].mech == mech;
            CHECK(found, "%s: no mesh part for mech %d", wd->id, mech);
        }
    }
}

// Attachments: mount / sight / lens / grip_l where attachments.c puts them; every allowed attachment of
// every weapon has a mesh for it ("mesh_for" variants included).
static void test_attachments(const ItemDB *db) {
    for (int a = 1; a <= db->attachment_count; a++) {
        const AttachmentDef *ad = &db->attachments[a];
        ItemMesh *m = item_mesh_get(ad->model.mesh);
        CHECK(m != NULL, "attachment %s: no mesh", ad->id);
        if (!m) continue;
        Xform s;
        CHECK(item_mesh_socket(m, "mount", &s) && dist(s.t, ad->mount.t) < 0.002f, "%s: mount socket", ad->id);
        if (ad->slot == ATT_OPTIC) {
            CHECK(item_mesh_socket(m, "sight", &s) && dist(s.t, ad->sight_pt) < 0.002f, "%s: sight socket vs sight_socket", ad->id);
            CHECK(item_mesh_socket(m, "lens", &s) && dist(s.t, ad->lens_pos) < 0.002f, "%s: lens socket vs lens.pos", ad->id);
        }
        if (ad->has_grip_l) CHECK(item_mesh_socket(m, "grip_l", &s) && dist(s.t, ad->grip_l.t) < 0.002f, "%s: grip_l", ad->id);
        if (ad->emitter) CHECK(item_mesh_socket(m, "emitter", &s), "%s: emitter attachment without an emitter socket", ad->id);
        for (int k = 0; k < ad->mesh_for_count; k++) CHECK(item_mesh_get(ad->mesh_for[k].mesh) != NULL, "%s: mesh_for %s does not load", ad->id, ad->mesh_for_weapon[k]);
    }
    for (int w = 0; w < db->weapon_count; w++) {
        const WeaponDef *wd = &db->weapons[w];
        for (int a = 1; a <= db->attachment_count; a++) {
            const AttachmentDef *ad = &db->attachments[a];
            if (!attach_allowed(wd, ad)) continue;
            CHECK(item_mesh_get(attach_model_for(ad, wd)->mesh) != NULL, "%s on %s: no mesh", ad->id, wd->id);
        }
    }
    // The per-well variants resolve.
    const WeaponDef *smg = NULL;
    for (int w = 0; w < db->weapon_count; w++)
        if (!strcmp(db->weapons[w].id, "smg")) smg = &db->weapons[w];
    for (int a = 1; a <= db->attachment_count && smg; a++)
        if (!strcmp(db->attachments[a].id, "extended_mag"))
            CHECK(strstr(attach_model_for(&db->attachments[a], smg)->mesh, "_smg") != NULL, "extended_mag on the smg: %s", attach_model_for(&db->attachments[a], smg)->mesh);
}

// Posing: the bolt rides the fire animation's slide, the front sight folds under an optic, the magazine
// hides with the extended one (and its follower with it).
static void test_pose(const ItemDB *db) {
    const WeaponDef *rifle = NULL;
    for (int w = 0; w < db->weapon_count; w++)
        if (!strcmp(db->weapons[w].id, "rifle")) rifle = &db->weapons[w];
    CHECK(rifle != NULL, "no rifle");
    if (!rifle) return;
    ItemMesh *m = item_mesh_get(rifle->model.mesh);
    if (!m) return;
    int bolt = item_mesh_find_part(m, "bolt_carrier"), fs = item_mesh_find_part(m, "front_sight"), mag = item_mesh_find_part(m, "magazine");
    CHECK(bolt >= 0 && fs >= 0 && mag >= 0, "rifle parts missing");
    if (bolt < 0 || fs < 0 || mag < 0) return;
    ItemMeshPose p = { 0 };
    p.wobble = quat_identity();
    Xform r0 = item_mesh_part_xform(m, bolt, &p);
    p.slide = 0.03f;
    Xform r1 = item_mesh_part_xform(m, bolt, &p);
    CHECK(fabsf(r1.t.z - r0.t.z - 0.03f) < 1e-5f, "bolt travel %.4f", r1.t.z - r0.t.z);
    p.slide = 0;
    Xform f0 = item_mesh_part_xform(m, fs, &p);
    p.fold_slots = 1u << ATT_OPTIC;
    Xform f1 = item_mesh_part_xform(m, fs, &p);
    vec3 up0 = quat_rotate(f0.r, v3(0, 1, 0)), up1 = quat_rotate(f1.r, v3(0, 1, 0));
    CHECK(up0.y > 0.99f && fabsf(up1.y) < 0.1f, "front sight fold: up %.2f -> %.2f", up0.y, up1.y);
    // Hidden groups draw nothing: count draws with and without the magazine slot filled.
    s_draws = 0;
    item_mesh_draw(m, ITEM_COND_WORN, v3(0, 0, 0), quat_identity(), 1.0f, NULL, 0, true);
    int all = s_draws;
    ItemMeshPose h = { 0 };
    h.wobble = quat_identity();
    h.hide_slots = 1u << ATT_MAGAZINE;
    s_draws = 0;
    item_mesh_draw(m, ITEM_COND_WORN, v3(0, 0, 0), quat_identity(), 1.0f, &h, 0, true);
    int follower = item_mesh_find_part(m, "mag_follower");
    CHECK(s_draws < all && follower >= 0 && m->parts[follower].group == m->parts[mag].group, "magazine hide: %d -> %d draws", all, s_draws);
    CHECK(all <= 12, "rifle draws %d groups (static parts should merge)", all);
}

int main(void) {
    static ItemDB db;
    log_init(NULL);
    CHECK(items_load(&db, "data/items.json", "data/weapons"), "items_load");
    test_meshes(&db);
    test_weapon_sockets(&db);
    test_attachments(&db);
    test_pose(&db);
    printf("item_mesh: %d checks, %d failed (%d meshes composed)\n", g_checks, g_fails, s_meshes);
    item_mesh_shutdown();
    return g_fails ? 1 : 0;
}
