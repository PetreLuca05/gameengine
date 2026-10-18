// Inventory screen (docs/INVENTORY.md "Screen"): the pure parts. Layout (left: the player's grids,
// centre: the player with the equipment slots around it, right: the hotbar, the loot grid below
// it when a world container is open) and the drag-and-drop state machine that turns pointer /
// pad input into intents (gameplay/inventory/inv_intent.h). No renderer, no SDL
// (tests/test_inventory_ui.c).
#ifndef DG_INV_UI_H
#define DG_INV_UI_H

#include "ui/gui/gui_layout.h"
#include "core/dgmath.h"
#include "gameplay/inventory.h"

// ------------------------------------------------------------------ layout

#define INV_UI_MAX_PANELS 8

typedef struct InvLayoutIn {
    GuiRect safe;              // the safe area (pixels)
    float scale;               // DPI scale (design px -> px)
    bool loot;                 // a world container is open
    int loot_cols, loot_rows;  // its root grid's bounding box (cells)
    int panel_n;               // the player's grids, in the left column (top to bottom)
    struct { int cols, rows; } panel[INV_UI_MAX_PANELS];
} InvLayoutIn;

typedef struct InvLayout {
    float cell;                // grid cell size (px), the same in every grid
    float pad, gap, title_h;   // panel padding, gaps, panel title height
    GuiRect header, footer;
    GuiRect left, center, right;
    GuiRect panel[INV_UI_MAX_PANELS];   // left column panels, unscrolled (y from left.y)
    float left_content_h;      // > left.h: the column scrolls
    GuiRect player;            // the live player (no backdrop)
    GuiRect equip[EQ_COUNT];
    GuiRect hot[INV_HOTBAR];   // hotbar tiles
    bool hot_compact;          // a row of squares (loot open) instead of large tiles
    GuiRect loot;              // the loot panel (title + grid), loot open only
    GuiRect discard;           // drop zone (bottom of the centre)
} InvLayout;

void inv_layout(const InvLayoutIn *in, InvLayout *out);
// The bounding box (cells) of a container def's compartments (their "at" placement).
void inv_container_extent(const InvContainerDef *cd, int *cols, int *rows);

// Pad / keyboard cursor: the nearest rect in a direction (0 left, 1 right, 2 up, 3 down) from
// rects[cur], scored by distance along the direction plus twice the sideways offset; -1: none.
int inv_nav_step(const GuiRect *rects, int n, int cur, int dir);

// ------------------------------------------------------------------ drag and drop

typedef enum InvTgtKind {
    IT_NONE, IT_CELL, IT_EQUIP, IT_HOT, IT_ATTACH, IT_DISCARD, IT_OUTSIDE, IT_PANEL, IT_COUNT
} InvTgtKind;

// What the pointer (or the pad cursor) is over. CELL: side / parent / loc / x / y and the item
// covering that cell (uid, 0 = empty); EQUIP: index = slot, uid = occupant; HOT: index = hotbar
// slot, uid = the bound item; ATTACH: uid = the weapon (side), index = attachment slot.
typedef struct InvTarget {
    u8 kind, side, loc;
    i8 index;
    u16 parent, uid;
    i16 x, y;
} InvTarget;

typedef enum InvDDState { IDD_IDLE, IDD_PRESSED, IDD_DRAGGING } InvDDState;

typedef struct InvDrag {
    int state;            // InvDDState
    InvTarget src;        // where it was picked up
    u16 uid;              // the carried item (ATTACH: the weapon)
    u8 side;
    i16 grab_x, grab_y;   // the grabbed cell relative to the item's top-left (unrotated frame)
    u8 rot;               // the carried orientation (R toggles)
    bool split;           // Shift at pick-up: a split dialog on drop
    bool carry;           // pad / keyboard carry (no pointer)
    vec2 press_pos;
} InvDrag;

typedef struct InvDDIn {
    vec2 mouse;
    bool pressed, released, right_pressed, double_clicked;
    bool shift, ctrl, alt;
    bool rotate;          // R / pad X while carrying
    bool cancel;          // Esc / B while dragging
    InvTarget hover;
    float threshold;      // drag start distance (px)
} InvDDIn;

typedef enum InvDDAction {
    IDA_NONE,
    IDA_INTENT,           // send out.in
    IDA_SELECT,           // out.at selected (inspect panel / tooltip)
    IDA_CONTEXT,          // right click on out.at
    IDA_OPEN,             // double click: open the container in a window, or inspect
    IDA_SPLIT,            // ask how many (out.in: the move, count = half)
    IDA_CONFIRM,          // ask before discarding (out.in)
    IDA_PICKED,           // carry started (sound)
    IDA_CANCEL,
    IDA_REJECT,           // out.reason (InvResult or IDR_*), out.at shakes
} InvDDAction;

#define IDR_TAKE_FIRST 100   // a loot item can't be bound to the hotbar: take it first

typedef struct InvDDOut {
    int action;
    InvIntent in;
    InvTarget at;
    int reason;
} InvDDOut;

// The trees the state machine reads (sizes, stacks, what a discard needs to confirm).
typedef struct InvView {
    const ItemDB *db;
    const InvTree *tree[INV_SIDES];
    const u16 *quick;
} InvView;

void idd_reset(InvDrag *d);
InvDDOut idd_update(InvDrag *d, const InvDDIn *in, const InvView *v);
// The intent for releasing what d carries on t (shared by the mouse and the carry mode).
InvDDOut idd_drop_on(const InvDrag *d, InvTarget t, const InvView *v);
// Pad / keyboard: pick up what the cursor is on, or place the carried item there (A / Enter).
InvDDOut idd_carry_accept(InvDrag *d, InvTarget focus, const InvView *v, bool split);
// The carried item's footprint at the hovered cell: top-left cell + size (false: not over a cell).
bool idd_footprint(const InvDrag *d, InvTarget t, const InvView *v, int *x, int *y, int *w, int *h);
const char *idd_reason_text(int reason);

#endif
