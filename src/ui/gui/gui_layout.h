// Retained layout layer (docs/UI.md "Layout"): a flat tree of nodes (parent before child) solved
// into pixel rects for a viewport. Pure: no renderer, no SDL (tests/test_ui.c).
//
//   * sizes are design pixels at 1080p, multiplied by the DPI scale (gui_layout_scale);
//   * a node is placed by its parent: anchors (9-point, + offset) in an overlay parent, or in
//     sequence in a row / column stack (gap, padding, cross-axis alignment, flex shares);
//   * size: fixed, fraction of the parent's content (w_pct / h_pct), fit to the children (stacks),
//     clamped by min / max (max_w_pct of the parent), optional aspect ratio (w / h);
//   * "safe" roots sit inside the safe area: a margin, and on very wide screens a centred region of
//     at most max_aspect (21:9 and wider keep menus and HUD near the centre of view);
//   * "avoid": after placement, a node that overlaps the named node is moved / shrunk beside it
//     (the centred settings modal next to the pause button stack on 4:3).
// Screens describe their nodes in assets/ui/screens/*.json (gui_layout_parse).
#ifndef DG_GUI_LAYOUT_H
#define DG_GUI_LAYOUT_H

#include "core/base.h"

typedef struct GuiRect { float x, y, w, h; } GuiRect;

typedef enum GuiAnchor { GA_TL, GA_T, GA_TR, GA_L, GA_C, GA_R, GA_BL, GA_B, GA_BR } GuiAnchor;
typedef enum GuiDir { GD_OVERLAY, GD_ROW, GD_COLUMN } GuiDir;
typedef enum GuiAlign { GAL_START, GAL_CENTER, GAL_END, GAL_STRETCH } GuiAlign;

#define GUI_LAYOUT_MAX_NODES 96

typedef struct LayoutNode {
    char id[32];
    char parent_id[32];
    char avoid[32];
    char label[48];           // widget nodes: text (buttons, titles)
    char action[32];          // widget nodes: what the screen does with it
    char cond[24];            // widget nodes: visibility condition evaluated by the screen
    int parent;               // index, -1 = root (the viewport)
    int dir;                  // GuiDir: how the children are placed
    int anchor;               // GuiAnchor in an overlay parent
    int align;                // GuiAlign: children on the cross axis (stacks)
    float x, y;               // offset from the anchor (design px)
    float w, h;               // design px, 0 = auto (fit children / fill in a stretch stack)
    float w_pct, h_pct;       // of the parent's content box (0 = unused)
    float min_w, min_h, max_w, max_h;   // design px (0 = none)
    float max_w_pct, max_h_pct;
    float aspect;             // w / h (0 = free)
    float flex;               // share of the remaining main-axis space in a stack
    float pad[4];             // left, top, right, bottom
    float gap;
    bool safe;                // roots: inside the safe area
    bool hidden;              // skipped (no space taken) - set by the screen before solving
    GuiRect rect;             // solved (pixels)
} LayoutNode;

typedef struct LayoutParams {
    float w, h;               // viewport (pixels)
    float scale;              // DPI scale (design px -> pixels)
    float safe_frac;          // margin as a fraction of the smaller side
    float max_aspect;         // safe area aspect limit (0 = none)
} LayoutParams;

float gui_layout_scale(float w, float h, float user_scale);   // user_scale > 0 overrides
GuiRect gui_layout_safe_rect(const LayoutParams *p);
void gui_layout_solve(LayoutNode *n, int count, const LayoutParams *p);
int gui_layout_find(const LayoutNode *n, int count, const char *id);
// JSON: { "nodes": [ { "id": ..., "parent": ..., "dir": "column", "anchor": "bottom_left", ... } ] }.
// Unknown parents are an error (returns false, *count = nodes parsed so far).
bool gui_layout_parse(const char *json, LayoutNode *out, int max, int *count, char *err, int err_size);

DG_INLINE bool gui_rect_contains(GuiRect r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
DG_INLINE bool gui_rect_overlap(GuiRect a, GuiRect b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
DG_INLINE GuiRect gui_rect(float x, float y, float w, float h) { GuiRect r = { x, y, w, h }; return r; }
DG_INLINE GuiRect gui_rect_inset(GuiRect r, float d) { GuiRect o = { r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d }; return o; }

#endif
