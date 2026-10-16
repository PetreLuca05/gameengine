// Shelf atlas packer for VFX textures (pure). Textures are packed into a few square pages of a
// 2D array texture; each texture keeps its grid (flipbook / variants) inside its rectangle.
#ifndef DG_VFX_ATLAS_H
#define DG_VFX_ATLAS_H

#include "core/base.h"

#define VFX_ATLAS_MAX_PAGES 16

typedef struct VfxAtlasRect { int w, h; int x, y, page; bool placed; } VfxAtlasRect;

typedef struct VfxAtlasPacker {
    int page_size;
    int pages;
    int padding;
    // shelves per page
    int shelf_y[VFX_ATLAS_MAX_PAGES], shelf_h[VFX_ATLAS_MAX_PAGES], shelf_x[VFX_ATLAS_MAX_PAGES];
} VfxAtlasPacker;

// Packs rects (sorted internally by height, results written back in input order). Returns the
// number of pages used, or -1 if something didn't fit in VFX_ATLAS_MAX_PAGES pages.
int vfx_atlas_pack(VfxAtlasRect *rects, int count, int page_size, int padding);

#endif
