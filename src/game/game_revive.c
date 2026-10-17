// Fabricator chamber presentation (docs/REVIVE.md): glass, liquid, interior.
#include "game_revive.h"
#include "game_internal.h"
#include "core/engine.h"
#include "render/render.h"
#include <math.h>

static void draw_cyl(vec3 pos, quat rot, vec3 local, float r, float half_h, Material m) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.mesh = game()->meshes.cylinder;
    d.model = mat4_trs(v3_add(pos, quat_rotate(rot, local)), rot, v3(r * 2.0f, half_h * 2.0f, r * 2.0f));
    d.mat = m;
    render_draw(&d);
}

void game_revive_draw_chamber(World *w, int i, vec3 pos, quat rot, const FabView *fv, float pulse) {
    (void)w;
    (void)i;
    bool busy = fv->m.state == FAB_RECONSTRUCTING || fv->m.state == FAB_OPENING;
    // Liquid: fills during the reconstruction, drains while the chamber opens.
    float fill = fv->m.state == FAB_RECONSTRUCTING ? fminf(1.0f, fv->progress * 4.0f) : (fv->m.state == FAB_OPENING ? fmaxf(0.0f, 1.0f - fv->remaining) : 0.0f);
    if (busy && fill > 0.01f) {
        float h = 2.0f * fill;
        Material liq = material_color(v4(0.25f, 0.8f, 0.75f, 0.22f));
        liq.roughness = 0.05f;
        liq.emissive = 0.4f + 0.8f * pulse;
        liq.flags = MAT_NO_SHADOW;
        draw_cyl(pos, rot, v3(0, 0.14f + h * 0.5f, 0), 0.52f, h * 0.5f, liq);
    }
    // Glass (open: slid up into the cap).
    float lift = fv->m.state == FAB_OPENING ? fminf(1.0f, 1.5f - fv->remaining) * 1.9f : 0.0f;
    Material glass = material_color(v4(0.7f, 0.88f, 0.95f, 0.14f));
    glass.roughness = 0.02f;
    glass.metallic = 0.0f;
    glass.flags = MAT_NO_SHADOW;
    draw_cyl(pos, rot, v3(0, 1.19f + lift, 0), 0.56f, 1.04f - lift * 0.5f, glass);
}
