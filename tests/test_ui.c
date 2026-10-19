// Game UI (docs/UI.md): the layout engine at 16:9 / 21:9 / 4:3 (+ 720p, 4K) and an icon for every
// item. The inventory (rules, intents, drag and drop, reconcile) has its own tests:
// test_inventory.c, test_inventory_ui.c (docs/INVENTORY.md). Run from the build
// output directory (data/, assets/).
#include "ui/gui/gui_layout.h"
#include "gameplay/items.h"
#include "gameplay/attachments.h"
#include "core/engine.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static bool inside(GuiRect in, GuiRect out) {
    return in.x >= out.x - 0.5f && in.y >= out.y - 0.5f && in.x + in.w <= out.x + out.w + 0.5f && in.y + in.h <= out.y + out.h + 0.5f;
}

// ------------------------------------------------------------------ layout

static void test_layout_basics(void) {
    LayoutNode n[6];
    memset(n, 0, sizeof(n));
    // root (overlay) -> row (bottom centre, gap 10) with a fixed child, a flex child and an aspect child
    for (int i = 0; i < 6; i++) n[i].parent = -1;
    dg_strcpy(n[0].id, sizeof(n[0].id), "root");
    dg_strcpy(n[1].id, sizeof(n[1].id), "row");
    n[1].parent = 0; n[1].dir = GD_ROW; n[1].anchor = GA_B; n[1].w = 500; n[1].h = 40; n[1].gap = 10; n[1].align = GAL_STRETCH;
    n[2].parent = 1; n[2].w = 100;
    n[3].parent = 1; n[3].flex = 1;
    n[4].parent = 1; n[4].aspect = 2.0f; n[4].h = 40;
    LayoutParams p = { 1920, 1080, 1.0f, 0.0f, 0.0f };
    gui_layout_solve(n, 5, &p);
    CHECK(fabsf(n[1].rect.x - 710) < 0.5f && fabsf(n[1].rect.y - 1040) < 0.5f, "bottom-centre anchor (%.1f %.1f)", n[1].rect.x, n[1].rect.y);
    CHECK(fabsf(n[4].rect.w - 80) < 0.5f, "aspect child 80 wide (%.1f)", n[4].rect.w);
    CHECK(fabsf(n[3].rect.w - (500 - 100 - 80 - 20)) < 0.5f, "flex child takes the rest (%.1f)", n[3].rect.w);
    CHECK(fabsf(n[2].rect.h - 40) < 0.5f, "stretch on the cross axis");
    CHECK(fabsf(n[3].rect.x - (n[2].rect.x + 110)) < 0.5f, "gap between stacked children");
    // DPI scale: everything doubles at 4K.
    LayoutParams p2 = { 3840, 2160, 2.0f, 0.0f, 0.0f };
    gui_layout_solve(n, 5, &p2);
    CHECK(fabsf(n[1].rect.w - 1000) < 0.5f && fabsf(n[3].rect.w - 600) < 0.5f, "scale 2: sizes double");
    CHECK(fabsf(gui_layout_scale(1920, 1080, 0) - 1.0f) < 1e-4f && fabsf(gui_layout_scale(3840, 2160, 0) - 2.0f) < 1e-4f, "auto scale 1080p / 4K");
    CHECK(gui_layout_scale(1024, 768, 0) * 1280.0f <= 1024.5f, "4:3 keeps 1280 design px across");
    CHECK(fabsf(gui_layout_scale(1920, 1080, 1.5f) - 1.5f) < 1e-4f, "ui_scale overrides");
}

static void test_layout_pause(void) {
    size_t size = 0;
    char *json = platform_read_file("assets/ui/screens/pause.json", &size);
    CHECK(json != NULL, "assets/ui/screens/pause.json readable");
    if (!json) return;
    static LayoutNode base[GUI_LAYOUT_MAX_NODES];
    int count = 0;
    char err[160];
    bool ok = gui_layout_parse(json, base, GUI_LAYOUT_MAX_NODES, &count, err, sizeof(err));
    free(json);
    CHECK(ok && count > 5, "pause.json parses (%s)", err);
    static const struct { float w, h; const char *name; } res[] = {
        { 1920, 1080, "16:9 1080p" }, { 2560, 1080, "21:9" }, { 1024, 768, "4:3" }, { 1280, 720, "16:9 720p" }, { 3840, 2160, "16:9 4K" }, { 3440, 1440, "21:9 1440p" },
    };
    float stack_h_1080 = 0;
    for (int r = 0; r < (int)ARRAY_COUNT(res); r++) {
        static LayoutNode n[GUI_LAYOUT_MAX_NODES];
        memcpy(n, base, sizeof(LayoutNode) * (size_t)count);
        LayoutParams p = { res[r].w, res[r].h, gui_layout_scale(res[r].w, res[r].h, 0), 0.035f, 2.39f };
        gui_layout_solve(n, count, &p);
        GuiRect safe = gui_layout_safe_rect(&p), view = { 0, 0, res[r].w, res[r].h };
        int st = gui_layout_find(n, count, "stack"), md = gui_layout_find(n, count, "modal"), br = gui_layout_find(n, count, "brand"),
            dl = gui_layout_find(n, count, "dialog");
        CHECK(st >= 0 && md >= 0 && br >= 0 && dl >= 0, "%s: nodes present", res[r].name);
        if (st < 0 || md < 0 || br < 0 || dl < 0) continue;
        CHECK(inside(n[st].rect, safe), "%s: button stack inside the safe area", res[r].name);
        CHECK(fabsf(n[st].rect.x - safe.x) < 0.5f && fabsf(n[st].rect.y + n[st].rect.h - (safe.y + safe.h)) < 0.5f, "%s: stack anchored bottom left", res[r].name);
        CHECK(inside(n[br].rect, safe) && fabsf(n[br].rect.x + n[br].rect.w - (safe.x + safe.w)) < 0.5f, "%s: logo bottom right", res[r].name);
        CHECK(inside(n[md].rect, view), "%s: modal on screen", res[r].name);
        CHECK(!gui_rect_overlap(n[md].rect, n[st].rect), "%s: modal beside the stack (%.0f..%.0f vs stack ..%.0f)", res[r].name, n[md].rect.x,
              n[md].rect.x + n[md].rect.w, n[st].rect.x + n[st].rect.w);
        CHECK(n[md].rect.w <= res[r].w * 0.6f + 0.5f || n[md].rect.w <= n[md].min_w * p.scale + 0.5f, "%s: modal at most 60%% wide (%.0f)", res[r].name, n[md].rect.w);
        CHECK(n[md].rect.w >= 300 * p.scale, "%s: modal usable width (%.0f)", res[r].name, n[md].rect.w);
        CHECK(fabsf(n[dl].rect.x + n[dl].rect.w * 0.5f - res[r].w * 0.5f) < 1.0f, "%s: dialog centred", res[r].name);
        // Buttons: every stack child inside the stack, ordered top to bottom.
        float prev = -1;
        for (int i = 0; i < count; i++) {
            if (n[i].parent != st) continue;
            CHECK(inside(n[i].rect, n[st].rect), "%s: %s inside the stack", res[r].name, n[i].id);
            CHECK(n[i].rect.y >= prev, "%s: %s in order", res[r].name, n[i].id);
            prev = n[i].rect.y;
        }
        if (res[r].w == 1920) stack_h_1080 = n[st].rect.h;
        if (res[r].w == 3840) CHECK(fabsf(n[st].rect.h - stack_h_1080 * 2.0f) < 1.0f, "4K: the stack is exactly twice the 1080p size");
        if (res[r].w / res[r].h > 2.3f) CHECK(safe.w / safe.h <= 2.39f + 1e-3f, "%s: safe area aspect limited", res[r].name);
    }
    // Hidden nodes take no space.
    static LayoutNode n[GUI_LAYOUT_MAX_NODES];
    memcpy(n, base, sizeof(LayoutNode) * (size_t)count);
    LayoutParams p = { 1920, 1080, 1.0f, 0.035f, 2.39f };
    gui_layout_solve(n, count, &p);
    float h_all = n[gui_layout_find(n, count, "stack")].rect.h;
    n[gui_layout_find(n, count, "invite")].hidden = true;
    gui_layout_solve(n, count, &p);
    CHECK(n[gui_layout_find(n, count, "stack")].rect.h < h_all - 30, "a hidden button shrinks the stack");
    // Errors: unknown parent.
    int c2;
    CHECK(!gui_layout_parse("{\"nodes\":[{\"id\":\"a\",\"parent\":\"nope\"}]}", n, 8, &c2, err, sizeof(err)) && strstr(err, "nope"), "unknown parent reported");
}

static ItemDB g_db;

// ------------------------------------------------------------------ icons

static void test_icons(void) {
    int missing = 0;
    for (int i = 1; i < g_db.item_count; i++) {
        const ItemDef *d = &g_db.items[i];
        for (int s = 0; s < 2; s++) {
            char path[256];
            if (d->icon[0]) snprintf(path, sizeof(path), "%s", d->icon);
            else snprintf(path, sizeof(path), "assets/ui/icons/%d/%s.png", s ? 256 : 128, d->id);
            if (!platform_file_exists(path)) { missing++; printf("  no icon: %s\n", path); }
        }
    }
    CHECK(missing == 0, "every item has a 128 and a 256 px icon (%d missing; tools/bake_icons/bake_icons.sh)", missing);
    // Inventory cell images at the grid aspect (docs/INVENTORY.md; tools/bake_icons/bake_cells.sh).
    missing = 0;
    for (int i = 1; i < g_db.item_count; i++) {
        const ItemDef *d = &g_db.items[i];
        char path[256];
        if (d->cell_image[0]) snprintf(path, sizeof(path), "%s", d->cell_image);
        else snprintf(path, sizeof(path), "assets/ui/cells/%s.png", d->id);
        if (!platform_file_exists(path)) { missing++; printf("  no cell image: %s\n", path); }
    }
    CHECK(missing == 0, "every item has a cell image (%d missing; tools/bake_icons/bake_cells.sh)", missing);
}

int main(void) {
    printf("UI tests\n");
    test_layout_basics();
    test_layout_pause();
    CHECK(items_load(&g_db, "data/items.json", "data/weapons"), "items load");
    test_icons();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
