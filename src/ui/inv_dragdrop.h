// Inventory drag and drop (docs/UI.md "Inventory"): a pure state machine fed with what the
// pointer is over each frame; it returns intents (gameplay/inv_ops.h) for the screen to send.
//
//   IDLE --press on an item--> PRESSED --moved > threshold--> DRAGGING --release--> IDLE + intent
//   PRESSED --release--> IDLE + select (shift: quick move)       DRAGGING --Esc / B--> IDLE
//   double click: equip; right click: context menu; gamepad / keyboard: pick up (A / Enter) moves
//   the item with the focus, A / Enter again places it (same rules as a mouse release).
//
// Release rules: onto an inventory slot = move (merge, remainder stays / swap / split with Ctrl);
// an attachment onto a weapon slot or the preview's attachment slot = mount (incompatible: reject +
// reason); an attachment dragged out of the preview = unmount; onto the hotbar = assign (a move);
// outside the panel = drop into the world (non-stackable items ask first); a crate slot = put.
#ifndef DG_INV_DRAGDROP_H
#define DG_INV_DRAGDROP_H

#include "gameplay/inv_ops.h"
#include "core/dgmath.h"

typedef enum DDState { DD_IDLE, DD_PRESSED, DD_DRAGGING } DDState;
typedef enum DDTargetKind { DT_NONE, DT_SLOT, DT_ATTACH, DT_PREVIEW, DT_OUTSIDE, DT_CONTAINER, DT_PANEL } DDTargetKind;
typedef enum DDAction {
    DDA_NONE, DDA_OP, DDA_SELECT, DDA_CONTEXT, DDA_CONFIRM_DROP, DDA_REJECT, DDA_CONTAINER_PUT, DDA_CONTAINER_TAKE, DDA_CANCEL, DDA_PICKED
} DDAction;

typedef struct DDTarget { u8 kind; i16 index; } DDTarget;   // index: inventory / container slot, attachment slot

typedef struct DragDrop {
    int state;                // DDState
    DDTarget src;             // what is carried (DT_SLOT, DT_ATTACH: the preview's attachment slot, DT_CONTAINER)
    vec2 press_pos;
    bool split;               // Ctrl at pick up: carry half the stack
    bool carry;               // keyboard / gamepad carry (no pointer)
} DragDrop;

typedef struct DDInput {
    vec2 mouse;
    bool pressed, released, right_pressed, double_clicked, shift, ctrl;
    bool cancel;              // Esc / B while dragging
    DDTarget hover;           // what the pointer (or the focus, when carrying) is over
    int weapon_slot;          // the inventory slot of the weapon shown in the preview (-1 none)
    float threshold;          // drag start distance (px)
} DDInput;

typedef struct DDResult {
    int action;               // DDAction
    InvOp op;                 // DDA_OP / DDA_CONFIRM_DROP (the drop to confirm)
    int slot;                 // DDA_SELECT / DDA_CONTEXT / DDA_CONTAINER_*: the slot; DDA_REJECT: the target slot
    int reject_reason;        // InvOpResult
    DDTarget reject_target;
} DDResult;

void dd_reset(DragDrop *d);
DDResult dd_update(DragDrop *d, const DDInput *in, const Inventory *inv, const ItemDB *db);
// Keyboard / gamepad: pick up what the focus is on, or place it there (A / Enter).
DDResult dd_carry_accept(DragDrop *d, DDTarget focus, const Inventory *inv, const ItemDB *db, int weapon_slot, bool ctrl);
// The intent for releasing the carried item on target (shared by the mouse and the carry mode).
DDResult dd_drop_on(const DragDrop *d, DDTarget target, const Inventory *inv, const ItemDB *db, int weapon_slot);

#endif
