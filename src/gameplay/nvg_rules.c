// Night vision goggles: gameplay rules (nvg_rules.h). Pure.
#include "gameplay/nvg_rules.h"
#include "render/post/nvg_model.h"

int nvg_rules_pick(const NvgItemView *items, int n) {
    for (int i = 0; i < n; i++)
        if (items[i].is_nvg && items[i].charge > 0) return i;
    return -1;
}

bool nvg_rules_toggle(NvgRuleState *st, const NvgItemView *items, int n, double now) {
    if (st->on) {
        st->on = false;
        st->illum = false;
        st->flip_t0 = now;
        st->flipped = true;
        return true;
    }
    int i = nvg_rules_pick(items, n);
    if (i < 0) return false;
    st->on = true;
    st->uid = items[i].uid;
    st->flip_t0 = now;
    st->flipped = true;
    st->drain_acc = 0.0f;
    return true;
}

bool nvg_rules_set_illum(NvgRuleState *st, bool on) {
    if (on && !st->on) return false;
    st->illum = on;
    return true;
}

bool nvg_rules_flipping(const NvgRuleState *st, double now, float flip_time) {
    return st->flipped && now - st->flip_t0 < flip_time && now >= st->flip_t0;
}

float nvg_rules_flip_pos(const NvgRuleState *st, double now, float flip_time) {
    float t = nvg_rules_flipping(st, now, flip_time) ? (float)((now - st->flip_t0) / flip_time) : 1.0f;
    return st->on ? t : 1.0f - t;
}

bool nvg_rules_ads_allowed(const NvgRuleState *st, double now, float flip_time) {
    return !st->on && !nvg_rules_flipping(st, now, flip_time);
}

bool nvg_rules_drain(NvgRuleState *st, u16 *charge, const NvgVariant *v, bool illum, float dt) {
    if (!st->on) return false;
    float after = nvg_battery_drain(v, 1.0f, illum, dt);   // fraction left of a full battery after dt
    st->drain_acc += (1.0f - after) * NVG_CHARGE_FULL;
    int whole = (int)st->drain_acc;
    if (whole > 0) {
        st->drain_acc -= (float)whole;
        *charge = (u16)DG_MAX((int)*charge - whole, 0);
    }
    if (*charge == 0) {
        st->on = false;
        st->illum = false;
        return true;
    }
    return false;
}

bool nvg_rules_swap_battery(u16 *charge, int *batteries) {
    if (*batteries <= 0) return false;
    (*batteries)--;
    *charge = NVG_CHARGE_FULL;
    return true;
}
