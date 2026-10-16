#include "icons.h"
#include "item_view.h"
#include "ui/gui/gui.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "stb_image.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_W 2048

typedef enum IconSrc { ICON_NONE, ICON_FILE, ICON_LIVE_PENDING, ICON_LIVE } IconSrc;

static struct {
    bool ready;
    int tile;                       // 128 or 256
    int cols, rows;
    TextureId atlas;
    u8 src[MAX_ITEM_DEFS];
    int item_count;
    TextureId live_target;          // 2x tile, SSAA for live bakes
    // bake_icons command
    bool baking;
    int bake_next;
    char bake_dir[400];
    bool bake_quit;
    TextureId bake_t256, bake_t128;
    int bake_wait;
} I;

static const ItemDB *db(void) {
    Game *g = game();
    return g && g->items.item_count > 1 ? &g->items : NULL;
}

static bool load_png(const char *rel, int want, u8 *dst, int stride) {
    char full[512];
    snprintf(full, sizeof(full), "%s%s", platform_base_dir(), rel);
    size_t size = 0;
    char *data = platform_read_file(full, &size);
    if (!data) return false;
    int w, h, c;
    u8 *px = stbi_load_from_memory((const u8 *)data, (int)size, &w, &h, &c, 4);
    free(data);
    if (!px) return false;
    // Nearest / box resample into the tile when the file has another size.
    for (int y = 0; y < want; y++)
        for (int x = 0; x < want; x++) {
            int sx = x * w / want, sy = y * h / want;
            memcpy(dst + (size_t)y * stride + (size_t)x * 4, px + ((size_t)sy * w + sx) * 4, 4);
        }
    stbi_image_free(px);
    return true;
}

static void setup(void) {
    const ItemDB *d = db();
    if (!d) return;
    if (I.atlas) render_texture_destroy(I.atlas);
    I.tile = gui_scale() > 1.4f ? 256 : 128;
    I.cols = ATLAS_W / I.tile;
    I.item_count = d->item_count;
    I.rows = (I.item_count + I.cols - 1) / I.cols;
    int h = I.rows * I.tile;
    u8 *px = (u8 *)calloc((size_t)ATLAS_W * h, 4);
    int files = 0;
    for (int i = 1; i < d->item_count; i++) {
        const ItemDef *it = &d->items[i];
        u8 *dst = px + ((size_t)(i / I.cols) * I.tile * ATLAS_W + (size_t)(i % I.cols) * I.tile) * 4;
        char rel[256];
        bool ok = false;
        if (it->icon[0]) ok = load_png(it->icon, I.tile, dst, ATLAS_W * 4);
        if (!ok) {
            snprintf(rel, sizeof(rel), "assets/ui/icons/%d/%s.png", I.tile, it->id);
            ok = load_png(rel, I.tile, dst, ATLAS_W * 4);
        }
        if (!ok) {
            // The other size, resampled.
            snprintf(rel, sizeof(rel), "assets/ui/icons/%d/%s.png", I.tile == 128 ? 256 : 128, it->id);
            ok = load_png(rel, I.tile, dst, ATLAS_W * 4);
        }
        I.src[i] = ok ? ICON_FILE : ICON_LIVE_PENDING;
        files += ok;
    }
    I.atlas = render_texture_create(ATLAS_W, h, px, false, false);
    free(px);
    if (!I.live_target) I.live_target = render_ui3d_target_create(512, 512);
    I.ready = true;
    log_info("ui: icon atlas %dx%d (%d px tiles): %d baked, %d rendered live", ATLAS_W, h, I.tile, files, d->item_count - 1 - files);
}

static void cmd_bake_icons(int argc, const char **argv);

void icons_init(void) {
    memset(&I, 0, sizeof(I));
    cmd_register("bake_icons", cmd_bake_icons, "bake_icons [dir] [quit]: render every item icon to <dir>/{128,256}/<id>.png (default: the source assets/ui/icons)");
}

void icons_reload(void) {
    I.ready = false;
}

static void view_for(u16 item, TextureId target, int size, RenderUi3dView *v, RenderUi3dPart *parts, int *n) {
    const ItemDB *d = db();
    vec3 mn, mx;
    const u8 *att = NULL;
    u8 def[ATT_SLOTS];
    const WeaponDef *wd = item_weapon(d, item);
    if (wd) { memset(def, 0, sizeof(def)); att = def; }   // icons show the bare weapon
    *n = item_view_parts(d, item, att, -1, parts, ITEM_VIEW_MAX_PARTS, &mn, &mx);
    ItemViewCam cam;
    item_view_default_cam(d, item, &cam);
    cam.ortho = true;
    memset(v, 0, sizeof(*v));
    v->target = target;
    item_view_matrices(&cam, mn, mx, 1.0f, &v->view, &v->proj);
    v->clear = v4(0, 0, 0, 0);
    v->parts = parts;
    v->part_count = *n;
    (void)size;
}

void icons_tick(void) {
    const ItemDB *d = db();
    if (!d) return;
    if (!I.ready || d->item_count != I.item_count) setup();
    if (!I.ready) return;
    static RenderUi3dPart parts[ITEM_VIEW_MAX_PARTS];
    // Live bake: one missing icon per frame, rendered at 2x and blitted (filtered) into its tile.
    for (int i = 1; i < I.item_count && !I.baking; i++) {
        if (I.src[i] != ICON_LIVE_PENDING) continue;
        RenderUi3dView v;
        int n;
        view_for((u16)i, I.live_target, I.tile, &v, parts, &n);
        v.copy_to = I.atlas;
        v.copy_x = (i % I.cols) * I.tile;
        v.copy_y = (i / I.cols) * I.tile;
        v.copy_w = v.copy_h = I.tile;
        render_ui3d(&v);
        I.src[i] = ICON_LIVE;
        break;
    }
    // bake_icons: two sizes per frame, written as PNGs after the frame.
    if (I.baking) {
        if (I.bake_wait > 0) { I.bake_wait--; return; }
        if (I.bake_next >= d->item_count) {
            I.baking = false;
            log_info("bake_icons: %d items written to %s/{128,256}", d->item_count - 1, I.bake_dir);
            icons_reload();
            if (I.bake_quit) g_engine.quit = true;
            return;
        }
        int i = I.bake_next++;
        if (i == 0) return;
        char path[512];
        RenderUi3dView v;
        int n;
        view_for((u16)i, I.bake_t256, 256, &v, parts, &n);
        snprintf(path, sizeof(path), "%s/256/%s.png", I.bake_dir, d->items[i].id);
        v.png_path = path;
        v.png_size = 256;
        render_ui3d(&v);
        static RenderUi3dPart parts2[ITEM_VIEW_MAX_PARTS];
        RenderUi3dView v2;
        view_for((u16)i, I.bake_t128, 128, &v2, parts2, &n);
        char path2[512];
        snprintf(path2, sizeof(path2), "%s/128/%s.png", I.bake_dir, d->items[i].id);
        v2.png_path = path2;
        v2.png_size = 128;
        render_ui3d(&v2);
    }
}

bool icons_get(u16 item, TextureId *tex, vec4 *uv) {
    if (!I.ready || !item || item >= I.item_count || I.src[item] == ICON_NONE || I.src[item] == ICON_LIVE_PENDING) return false;
    float aw = (float)ATLAS_W, ah = (float)(I.rows * I.tile);
    float x = (float)((item % I.cols) * I.tile), y = (float)((item / I.cols) * I.tile);
    *tex = I.atlas;
    *uv = v4((x + 0.5f) / aw, (y + 0.5f) / ah, (x + I.tile - 0.5f) / aw, (y + I.tile - 0.5f) / ah);
    return true;
}

int icons_missing(void) {
    int n = 0;
    for (int i = 1; i < I.item_count; i++) n += I.src[i] != ICON_FILE;
    return n;
}

// bake_icons [dir] [quit]: renders every item (bare weapons, orthographic 3/4 view, transparent
// background) into <dir>/256/<id>.png and <dir>/128/<id>.png. Default dir: the source tree's
// assets/ui/icons (development builds) so the PNGs can be committed.
static void cmd_bake_icons(int argc, const char **argv) {
    if (!db()) { log_warn("bake_icons: no item database yet"); return; }
    const char *dir = NULL;
    I.bake_quit = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "quit")) I.bake_quit = true;
        else dir = argv[i];
    }
#ifdef DG_SOURCE_DIR
    if (!dir) {
        snprintf(I.bake_dir, sizeof(I.bake_dir), "%s/assets/ui/icons", DG_SOURCE_DIR);
        dir = I.bake_dir;
    }
#endif
    if (!dir) dir = "assets/ui/icons";
    if (dir != I.bake_dir) dg_strcpy(I.bake_dir, sizeof(I.bake_dir), dir);
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/256", I.bake_dir);
    SDL_CreateDirectory(sub);
    snprintf(sub, sizeof(sub), "%s/128", I.bake_dir);
    SDL_CreateDirectory(sub);
    if (!I.bake_t256) I.bake_t256 = render_ui3d_target_create(512, 512);
    if (!I.bake_t128) I.bake_t128 = render_ui3d_target_create(256, 256);
    I.baking = true;
    I.bake_next = 0;
    I.bake_wait = 2;
    log_info("bake_icons: baking %d items into %s", db()->item_count - 1, I.bake_dir);
}

