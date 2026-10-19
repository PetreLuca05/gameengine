// Inventory screen (docs/INVENTORY.md "Screen"): the pure parts. Layout (header: the title centred, the
// category bar above the divider; left: the weapon slots (live item previews) with their attachment
// chips; centre: the player character, rendered alone; right: the grid panel down to the bottom of the
// screen (an open world container's grid above the player's, scrolling when they don't fit) and,
// right of it, the helmet / plate carrier / backpack slots stacked) and the drag-and-drop state
// machine that turns pointer / pad input into intents (gameplay/inventory/inv_intent.h). No
// renderer, no SDL (tests/test_inventory_ui.c).
#ifndef DG_INV_UI_H
#define DG_INV_UI_H

#include "ui/gui/gui_layout.h"
#include "core/dgmath.h"
#include "gameplay/inventory.h"

// ------------------------------------------------------------------ layout

#define INV_UI_GRID_COLS 10   // the column is sized for this many cells (every backpack is 10 wide)
#define INV_UI_WEAPON_ROWS 2  // primary and secondary show their attachment chips

typedef struct InvLayoutIn {
    GuiRect safe;              // the safe area (pixels)
    float scale;               // DPI scale (design px -> px)
    int grid_cols, grid_rows;  // the player's grid (the worn backpack's)
    bool loot;                 // a world container is open
    bool loot_body;            // it is a body (a corpse): its equipment row above its grid
    int loot_cols, loot_rows;  // its grid (cells; 0 x 0: none yet)
} InvLayoutIn;

typedef struct InvLayout {
    float cell;                // grid cell size (px), the same in every grid
    float pad, gap, title_h;   // panel padding, gaps, panel title height
    float chip, chip_gap;      // attachment chip size / spacing
    GuiRect header;            // the title (centred) and the category bar, above the divider (its bottom edge)
    GuiRect cats;              // in the header: the category bar and Compact (right of the title, or a second row)
    GuiRect footer;            // under the left and centre columns: hints, the last rejection
    GuiRect left, center;      // the weapon slots; the character
    GuiRect right;             // the grid panel: from under the header to the bottom of the safe area
    GuiRect gear;              // right of the grid panel: helmet, body armor, backpack stacked
    GuiRect equip[EQ_COUNT];   // primary, secondary, holster in the left column; the gear in the gear column
    GuiRect att_row[INV_UI_WEAPON_ROWS];   // under primary / secondary: ATT_SLOTS square chips (chip, chip_gap) across the slot's width
    GuiRect player;            // centre: the character render (no backdrop of its own: the screen's blur)
    GuiRect discard;           // drop zone (bottom of the centre)
    GuiRect view;              // inside the grid panel: the scrolled area
    GuiRect loot;              // in view (unscrolled): the world container (title, equipment row, grid, Take all); loot open only
    GuiRect grid;              // in view (unscrolled): the player's grid (title + grid)
    float content_h;           // the scrolled content's height (> view.h: it scrolls)
} InvLayout;

void inv_layout(const InvLayoutIn *in, InvLayout *out);
// The bounding box (cells) of a container def's compartments (their "at" placement).
void inv_container_extent(const InvContainerDef *cd, int *cols, int *rows);

// Pad / keyboard cursor: the nearest rect in a direction (0 left, 1 right, 2 up, 3 down) from
// rects[cur], scored by distance along the direction plus twice the sideways offset; -1: none.
int inv_nav_step(const GuiRect *rects, int n, int cur, int dir);

// ------------------------------------------------------------------ drag and drop

typedef enum InvTgtKind {
    IT_NONE, IT_CELL, IT_EQUIP, IT_ATTACH, IT_CHAR, IT_DISCARD, IT_OUTSIDE, IT_PANEL, IT_COUNT
} InvTgtKind;

// What the pointer (or the pad cursor) is over. CELL: side / parent / loc / x / y and the item
// covering that cell (uid, 0 = empty); EQUIP: index = slot, uid = occupant; ATTACH: uid = the weapon
// (side), index = attachment slot; CHAR: the player character (a drop equips into its own slot).
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
    IDA_OPEN,             // double click: inspect
    IDA_SPLIT,            // ask how many (out.in: the move, count = half)
    IDA_CONFIRM,          // ask before discarding (out.in)
    IDA_PICKED,           // carry started (sound)
    IDA_CANCEL,
    IDA_REJECT,           // out.reason (InvResult), out.at shakes
} InvDDAction;

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
// The category bar (docs/INVENTORY.md "Categories"): 0 all, else ICAT_* + 1. Items outside it dim.
bool inv_cat_dims(int cat, const ItemDef *d);

#endif
