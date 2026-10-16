#include "vfx_atlas.h"
#include <stdlib.h>
#include <string.h>

typedef struct Order { int index, h, w; } Order;

static int cmp_order(const void *a, const void *b) {
    const Order *x = (const Order *)a, *y = (const Order *)b;
    if (x->h != y->h) return y->h - x->h;
    if (x->w != y->w) return y->w - x->w;
    return x->index - y->index;   // stable
}

int vfx_atlas_pack(VfxAtlasRect *rects, int count, int page_size, int padding) {
    if (count <= 0) return 0;
    Order *ord = (Order *)malloc(sizeof(Order) * (size_t)count);
    for (int i = 0; i < count; i++) { ord[i].index = i; ord[i].h = rects[i].h; ord[i].w = rects[i].w; rects[i].placed = false; }
    qsort(ord, (size_t)count, sizeof(Order), cmp_order);
    int shelf_x[VFX_ATLAS_MAX_PAGES], shelf_y[VFX_ATLAS_MAX_PAGES], shelf_h[VFX_ATLAS_MAX_PAGES];
    memset(shelf_x, 0, sizeof(shelf_x));
    memset(shelf_y, 0, sizeof(shelf_y));
    memset(shelf_h, 0, sizeof(shelf_h));
    int pages = 1;
    bool ok = true;
    for (int k = 0; k < count; k++) {
        VfxAtlasRect *r = &rects[ord[k].index];
        int w = r->w + padding * 2, h = r->h + padding * 2;
        if (w > page_size || h > page_size) { ok = false; continue; }
        bool placed = false;
        for (int p = 0; p < pages && !placed; p++) {
            // Same shelf if it fits, else a new shelf below.
            if (shelf_x[p] + w <= page_size && h <= (shelf_h[p] ? shelf_h[p] : h) && shelf_y[p] + h <= page_size) {
            } else if (shelf_y[p] + shelf_h[p] + h <= page_size && w <= page_size) {
                shelf_y[p] += shelf_h[p];
                shelf_x[p] = 0;
                shelf_h[p] = 0;
            } else {
                continue;
            }
            if (shelf_x[p] + w > page_size) continue;
            r->x = shelf_x[p] + padding;
            r->y = shelf_y[p] + padding;
            r->page = p;
            r->placed = true;
            shelf_x[p] += w;
            if (h > shelf_h[p]) shelf_h[p] = h;
            placed = true;
        }
        if (!placed) {
            if (pages >= VFX_ATLAS_MAX_PAGES) { ok = false; continue; }
            int p = pages++;
            r->x = padding;
            r->y = padding;
            r->page = p;
            r->placed = true;
            shelf_x[p] = w;
            shelf_h[p] = h;
            shelf_y[p] = 0;
        }
    }
    free(ord);
    return ok ? pages : -1;
}
