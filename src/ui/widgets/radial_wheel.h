// Radial wheel (docs/UI_WHEELS_HUD.md "Radial wheel"): the shared hold-to-open, release-to-select
// wheel behind the healing, throwables, ping and door wheels. 4..12 slots round a centre disc;
// a direction (mouse delta, right stick) picks the slot under it past a dead zone, with angular
// hysteresis at the edges; number keys and the scroll wheel pick directly. The selected slot is
// white (theme "select"); empty slots are dimmed, disabled ones greyed with their reason shown in
// the centre. The centre shows the selected slot's name, count and description, or the timed
// ring while an action runs. Opens and closes in 120 ms.
#ifndef DG_RADIAL_WHEEL_H
#define DG_RADIAL_WHEEL_H

#include "radial_math.h"
#include "wheel_def.h"
#include "timed_ring.h"

typedef struct WheelSlot {
    char id[32];
    char label[32];
    char desc[96];
    char reason[48];       // disabled: why ("Full health", "Not here")
    char icon[24];         // vector icon (hud_icons.c), or empty for the item's icon
    char sub[24];          // sub-wheel to open on select
    u16 item;              // inventory wheels: the item (icon + count)
    int count;             // > 0: shown as a stack count
    u32 color;             // 0 = theme text; pings: the type colour
    bool empty, disabled;
} WheelSlot;

typedef struct RadialWheel {
    const WheelDef *def;
    WheelSlot slots[RADIAL_MAX_SLOTS];
    int count;
    RadialSel sel;
    bool open;
    float t;               // 0..1 open animation
    float hl[RADIAL_MAX_SLOTS];   // per-slot highlight (presentation)
    int last_slot;         // hover sound on change
} RadialWheel;

// Selection parameters from the theme tokens (wheel_dead_zone, wheel_cursor, wheel_hysteresis_deg).
RadialParams radial_wheel_params(const RadialWheel *w);
void radial_wheel_open(RadialWheel *w, const WheelDef *def, const WheelSlot *slots, int n, int initial);
void radial_wheel_feed_mouse(RadialWheel *w, vec2 delta_px);   // relative mouse (window pixels)
void radial_wheel_feed_stick(RadialWheel *w, vec2 stick);      // right stick -1..1
void radial_wheel_pick(RadialWheel *w, int slot);              // number keys
void radial_wheel_step(RadialWheel *w, int dir);               // scroll
// Release: closes and returns the selected slot, or -1 (none, empty or disabled).
int radial_wheel_release(RadialWheel *w);
void radial_wheel_cancel(RadialWheel *w);
void radial_wheel_tick(RadialWheel *w, float dt);
bool radial_wheel_visible(const RadialWheel *w);
// Draws at the screen centre; ring (optional) replaces the centre text while active.
void radial_wheel_draw(const RadialWheel *w, const TimedRingState *ring);
// Selectable: not empty, not disabled.
DG_INLINE bool wheel_slot_selectable(const WheelSlot *s) { return s && !s->empty && !s->disabled; }

#endif
