// Uniform spatial hash for the horde (pure, no world): rebuilt every tick from the zombie
// positions with a counting sort (no allocation after init), queried for neighbours within a
// radius (separation, attack range, player proximity). Cells are `cell` metres on XZ; the hash
// table has a fixed power-of-two size, collisions just add candidates (the radius test filters).
#ifndef DG_HORDE_GRID_H
#define DG_HORDE_GRID_H

#include "core/base.h"
#include "core/dgmath.h"

#define HGRID_BUCKETS 4096   // power of two

typedef struct HordeGrid {
    float cell, inv_cell;
    int capacity;
    int count;
    u32 *bucket_of;          // [capacity] bucket of each item
    int *start;              // [HGRID_BUCKETS + 1] prefix sums
    int *items;              // [capacity] item indices sorted by bucket
} HordeGrid;

bool hgrid_init(HordeGrid *g, int capacity, float cell);
void hgrid_free(HordeGrid *g);
// pos[i] for i < count; active[i] == 0 items are left out (NULL = all active).
void hgrid_build(HordeGrid *g, const vec3 *pos, const u8 *active, int count);
// Items within radius of p (XZ distance), excluding `self` (-1 = none), up to max, ascending
// index order (deterministic). Returns how many were written.
int hgrid_query(const HordeGrid *g, const vec3 *pos, vec3 p, float radius, int self, int *out, int max);

#endif
