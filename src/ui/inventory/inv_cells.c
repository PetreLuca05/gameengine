// Cell images (docs/INVENTORY.md "Cell images"): every item drawn at its grid aspect (a 4x2 rifle
// is a 4:2 picture), packed on shelves in one atlas at CELL_PX per cell. Each image comes from, in
// order: the item's "cell_image" override, the baked assets/ui/cells/<id>.png (bake_cells), or a
// live bake through the UI item view renderer (one per frame). Baked PNGs are square and
// anamorphic: the item framed at its grid aspect, stretched to the square (the loader squeezes
// it back), plus <id>_r.png, the same item turned a quarter (rotated items draw the image turned
// with gd_image_rot90, so the variant is for tools and the reference sheet).
#include "inv_screen.h"
#include "ui/item_view.h"
#include "ui/gui/gui.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "stb_image.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_W 2048
#define ATLAS_H 1024
#define CELL_PX 64
#define BAKE_PX 512

typedef enum CellSrc { CS_NONE, CS_FILE, CS_LIVE_PENDING, CS_LIVE } CellSrc;

static struct {
    bool ready;
    int item_count;
    TextureId atlas;
    u8 src[MAX_ITEM_DEFS];
    u16 x[MAX_ITEM_DEFS], y[MAX_ITEM_DEFS], w[MAX_ITEM_DEFS], h[MAX_ITEM_DEFS];
    TextureId live;            // square render target (anamorphic), blitted into the item's rect
    // bake_cells
    bool baking, bake_quit;
    int bake_next, bake_wait;
    char dir[400];
    TextureId bake_t;
} C;

static const ItemDB *db(void) {
    Game *g = game();
    return g && g->items.item_count > 1 ? &g->items : NULL;
}

// The PNG resampled (box filter, premultiplied) into a w x h rect of the atlas pixels.
static bool load_png(const char *rel, int w, int h, u8 *dst, int stride) {
    char full[512];
    snprintf(full, sizeof(full), "%s%s", platform_base_dir(), rel);
    size_t size = 0;
    char *data = platform_read_file(full, &size);
    if (!data) return false;
    int iw, ih, c;
    u8 *px = stbi_load_from_memory((const u8 *)data, (int)size, &iw, &ih, &c, 4);
    free(data);
    if (!px) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int x0 = x * iw / w, x1 = DG_MAX(x0 + 1, (x + 1) * iw / w);
            int y0 = y * ih / h, y1 = DG_MAX(y0 + 1, (y + 1) * ih / h);
            float r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++, n++) {
                    const u8 *p = px + ((size_t)sy * iw + sx) * 4;
                    float pa = p[3] / 255.0f;
                    r += p[0] * pa; g += p[1] * pa; b += p[2] * pa; a += pa;
                }
            u8 *o = dst + (size_t)y * stride + (size_t)x * 4;
            float inv = a > 0 ? 1.0f / a : 0.0f;
            o[0] = (u8)DG_CLAMP((int)(r * inv + 0.5f), 0, 255);
            o[1] = (u8)DG_CLAMP((int)(g * inv + 0.5f), 0, 255);
            o[2] = (u8)DG_CLAMP((int)(b * inv + 0.5f), 0, 255);
            o[3] = (u8)DG_CLAMP((int)(a / (float)n * 255.0f + 0.5f), 0, 255);
        }
    stbi_image_free(px);
    return true;
}

static void setup(void) {
    const ItemDB *d = db();
    if (!d) return;
    if (C.atlas) render_texture_destroy(C.atlas);
    C.item_count = d->item_count;
    u8 *px = (u8 *)calloc((size_t)ATLAS_W * ATLAS_H, 4);
    // Shelf packing, tallest first is not needed at this fill (~40 %): file order keeps it stable.
    int sx = 0, sy = 0, shelf = 0, files = 0;
    for (int i = 1; i < d->item_count; i++) {
        const ItemDef *it = &d->items[i];
        int w = DG_MAX(1, (int)it->grid_w) * CELL_PX, h = DG_MAX(1, (int)it->grid_h) * CELL_PX;
        if (sx + w > ATLAS_W) { sx = 0; sy += shelf; shelf = 0; }
        if (sy + h > ATLAS_H) { C.src[i] = CS_NONE; continue; }
        C.x[i] = (u16)sx; C.y[i] = (u16)sy; C.w[i] = (u16)w; C.h[i] = (u16)h;
        sx += w;
        shelf = DG_MAX(shelf, h);
        u8 *dst = px + ((size_t)C.y[i] * ATLAS_W + C.x[i]) * 4;
        char rel[256];
        bool ok = it->cell_image[0] && load_png(it->cell_image, w, h, dst, ATLAS_W * 4);
        if (!ok) {
            snprintf(rel, sizeof(rel), "assets/ui/cells/%s.png", it->id);
            ok = load_png(rel, w, h, dst, ATLAS_W * 4);
        }
        C.src[i] = ok ? CS_FILE : CS_LIVE_PENDING;
        files += ok;
    }
    C.atlas = render_texture_create(ATLAS_W, ATLAS_H, px, false, true);
    free(px);
    if (!C.live) C.live = render_ui3d_target_create(BAKE_PX, BAKE_PX);
    C.ready = true;
    log_info("ui: cell images %dx%d: %d baked, %d rendered live", ATLAS_W, ATLAS_H, files, d->item_count - 1 - files);
}

// An item framed at its grid aspect (rot: turned a quarter, the aspect inverted) into target.
static void view_for(u16 item, TextureId target, bool rot, RenderUi3dView *v, RenderUi3dPart *parts) {
    const ItemDB *d = db();
    const ItemDef *it = item_def(d, item);
    vec3 mn, mx;
    u8 none[ATT_SLOTS];
    memset(none, 0, sizeof(none));
    int n = item_view_parts(d, item, item_weapon(d, item) ? none : NULL, -1, parts, ITEM_VIEW_MAX_PARTS, &mn, &mx);
    ItemViewCam cam;
    item_view_default_cam(d, item, &cam);
    cam.ortho = true;
    float aspect = (float)DG_MAX(1, it->grid_w) / (float)DG_MAX(1, it->grid_h);
    memset(v, 0, sizeof(*v));
    v->target = target;
    item_view_matrices(&cam, mn, mx, rot ? 1.0f / aspect : aspect, &v->view, &v->proj);
    if (rot) v->view = mat4_mul(mat4_from_quat(quat_axis_angle(v3(0, 0, 1), -1.5707963f)), v->view);
    v->clear = v4(0, 0, 0, 0);
    v->parts = parts;
    v->part_count = n;
}

void inv_cells_tick(void) {
    const ItemDB *d = db();
    if (!d) return;
    if (!C.ready || d->item_count != C.item_count) setup();
    if (!C.ready) return;
    static RenderUi3dPart parts[ITEM_VIEW_MAX_PARTS];
    for (int i = 1; i < C.item_count && !C.baking; i++) {
        if (C.src[i] != CS_LIVE_PENDING) continue;
        RenderUi3dView v;
        view_for((u16)i, C.live, false, &v, parts);
        v.copy_to = C.atlas;
        v.copy_x = C.x[i];
        v.copy_y = C.y[i];
        v.copy_w = C.w[i];
        v.copy_h = C.h[i];
        render_ui3d(&v);
        C.src[i] = CS_LIVE;
        break;
    }
    if (!C.baking) return;
    if (C.bake_wait > 0) { C.bake_wait--; return; }
    if (C.bake_next >= d->item_count) {
        C.baking = false;
        C.ready = false;   // reload from the new PNGs
        log_info("bake_cells: %d items written to %s", d->item_count - 1, C.dir);
        if (C.bake_quit) g_engine.quit = true;
        return;
    }
    int i = C.bake_next++;
    if (i == 0) return;
    static char path[2][512];
    static RenderUi3dPart parts2[ITEM_VIEW_MAX_PARTS];
    RenderUi3dView v;
    view_for((u16)i, C.bake_t, false, &v, parts);
    snprintf(path[0], sizeof(path[0]), "%s/%s.png", C.dir, d->items[i].id);
    v.png_path = path[0];
    v.png_size = BAKE_PX / 2;
    render_ui3d(&v);
    if (d->items[i].rotatable) {
        RenderUi3dView r;
        view_for((u16)i, C.bake_t, true, &r, parts2);
        snprintf(path[1], sizeof(path[1]), "%s/%s_r.png", C.dir, d->items[i].id);
        r.png_path = path[1];
        r.png_size = BAKE_PX / 2;
        render_ui3d(&r);
    }
}

bool inv_cells_get(u16 item, TextureId *tex, vec4 *uv) {
    if (!C.ready || !item || item >= C.item_count || C.src[item] == CS_NONE || C.src[item] == CS_LIVE_PENDING) return false;
    float x = C.x[item], y = C.y[item];
    *tex = C.atlas;
    *uv = v4((x + 0.5f) / ATLAS_W, (y + 0.5f) / ATLAS_H, (x + C.w[item] - 0.5f) / ATLAS_W, (y + C.h[item] - 0.5f) / ATLAS_H);
    return true;
}

// bake_cells [dir] [quit]: every item at its grid aspect (+ the rotated variant) into
// <dir>/<id>.png (default: the source tree's assets/ui/cells, so the PNGs can be committed).
static void cmd_bake_cells(int argc, const char **argv) {
    if (!db()) { log_warn("bake_cells: no item database yet"); return; }
    const char *dir = NULL;
    C.bake_quit = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "quit")) C.bake_quit = true;
        else dir = argv[i];
    }
#ifdef DG_SOURCE_DIR
    if (!dir) {
        snprintf(C.dir, sizeof(C.dir), "%s/assets/ui/cells", DG_SOURCE_DIR);
        dir = C.dir;
    }
#endif
    if (!dir) dir = "assets/ui/cells";
    if (dir != C.dir) dg_strcpy(C.dir, sizeof(C.dir), dir);
    SDL_CreateDirectory(C.dir);
    if (!C.bake_t) C.bake_t = render_ui3d_target_create(BAKE_PX, BAKE_PX);   // 2x supersampled (box-filtered on readback)
    C.baking = true;
    C.bake_next = 0;
    C.bake_wait = 2;
    log_info("bake_cells: baking %d items into %s", db()->item_count - 1, C.dir);
}

void inv_cells_init(void) {
    memset(&C, 0, sizeof(C));
    cmd_register("bake_cells", cmd_bake_cells, "bake_cells [dir] [quit]: render every item at its inventory grid aspect (+ _r rotated) into <dir>/<id>.png (default assets/ui/cells)");
}
