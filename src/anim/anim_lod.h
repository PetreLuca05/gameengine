// Character LOD selection (client rendering only; gameplay never depends on it).
//
// Screen size = projected height in pixels. LOD n is used while the size is below
// threshold[n-1]; switching requires crossing the threshold by `hysteresis` (fraction), so a
// size jittering around a threshold never oscillates.
#ifndef DG_ANIM_LOD_H
#define DG_ANIM_LOD_H

#include "core/base.h"

#define CHAR_LOD_COUNT 4

typedef struct LodConfig {
    float threshold[CHAR_LOD_COUNT - 1];  // pixel heights: LOD0 above t[0], LOD1 above t[1], LOD2 above t[2], else LOD3
    float hysteresis;                     // e.g. 0.15
    float bias;                           // r_lod_bias * quality scale (>1 = higher detail)
    int update_every[CHAR_LOD_COUNT];     // animation (render pose) update interval in ticks
    float cull_pixels;                    // below: not drawn (state machine still ticks)
    float shadow_pixels;                  // below: no shadow
} LodConfig;

DG_INLINE void lod_config_default(LodConfig *c) {
    c->threshold[0] = 260.0f;
    c->threshold[1] = 120.0f;
    c->threshold[2] = 50.0f;
    c->hysteresis = 0.15f;
    c->bias = 1.0f;
    c->update_every[0] = 1;
    c->update_every[1] = 2;
    c->update_every[2] = 4;
    c->update_every[3] = 8;
    c->cull_pixels = 3.0f;
    c->shadow_pixels = 40.0f;
}

DG_INLINE int lod_select(const LodConfig *c, int current, float pixels) {
    float p = pixels * (c->bias > 0 ? c->bias : 1.0f);
    int target = CHAR_LOD_COUNT - 1;
    for (int i = 0; i < CHAR_LOD_COUNT - 1; i++)
        if (p >= c->threshold[i]) { target = i; break; }
    if (current < 0 || current >= CHAR_LOD_COUNT) return target;
    // Step one boundary at a time; each crossing needs the hysteresis margin.
    while (current < target && p < c->threshold[current] * (1.0f - c->hysteresis)) current++;
    while (current > target && p > c->threshold[current - 1] * (1.0f + c->hysteresis)) current--;
    return current;
}

#endif
