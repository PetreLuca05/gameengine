// Night vision goggles: the gameplay rules (docs/NIGHT_OPTICS.md "Gameplay rules"). Pure: the host's
// game_nvg.c and the tests run them on a view of the inventory.
//
//   toggle     key 8 turns the goggles on when an `nvg` with charge is in the inventory (the first
//              one in grid order) and off again; the flip (hand case nvg_flip) takes flip_time
//   ADS        refused while the goggles are down and while flipping either way (any sight)
//   battery    the item's charge (InvItem.dur, 0..NVG_CHARGE_FULL) drains while on (faster with the
//              illuminator); empty switches the goggles off; "Replace battery" uses one battery_aa
#ifndef DG_NVG_RULES_H
#define DG_NVG_RULES_H

#include "core/base.h"

#define NVG_CHARGE_FULL 1000

struct NvgVariant;

typedef struct NvgItemView {
    u16 uid;
    bool is_nvg;
    u16 charge;                      // 0..NVG_CHARGE_FULL
} NvgItemView;

typedef struct NvgRuleState {
    bool on;                         // goggles down
    bool illum;                      // IR illuminator on (only while on)
    u16 uid;                         // the item in use
    double flip_t0;                  // s: when the last flip started
    bool flipped;                    // flip_t0 is set
    float drain_acc;                 // fractional charge drained, not yet taken from the item
} NvgRuleState;

// Index of the nvg to use (first with charge), -1 if none.
int nvg_rules_pick(const NvgItemView *items, int n);
// Toggles at time now; false when it cannot turn on (no nvg with charge).
bool nvg_rules_toggle(NvgRuleState *st, const NvgItemView *items, int n, double now);
bool nvg_rules_set_illum(NvgRuleState *st, bool on);
bool nvg_rules_flipping(const NvgRuleState *st, double now, float flip_time);
// 0 (up) .. 1 (down): the flip's progress for the hand case / the view transition.
float nvg_rules_flip_pos(const NvgRuleState *st, double now, float flip_time);
bool nvg_rules_ads_allowed(const NvgRuleState *st, double now, float flip_time);
// Drains dt seconds; true when the battery ran out this call (the goggles switch off).
bool nvg_rules_drain(NvgRuleState *st, u16 *charge, const struct NvgVariant *v, bool illum, float dt);
// Uses one battery: false (nothing changes) when there is none.
bool nvg_rules_swap_battery(u16 *charge, int *batteries);

#endif
