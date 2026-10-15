#include "horde_grid.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline u32 cell_hash(int cx, int cz) {
    u32 h = (u32)cx * 73856093u ^ (u32)cz * 19349663u;
    return h & (HGRID_BUCKETS - 1);
}

bool hgrid_init(HordeGrid *g, int capacity, float cell) {
    memset(g, 0, sizeof(*g));
    g->cell = cell > 0.1f ? cell : 2.0f;
    g->inv_cell = 1.0f / g->cell;
    g->capacity = capacity;
    g->bucket_of = (u32 *)calloc((size_t)capacity, sizeof(u32));
    g->start = (int *)calloc(HGRID_BUCKETS + 1, sizeof(int));
    g->items = (int *)calloc((size_t)capacity, sizeof(int));
    return g->bucket_of && g->start && g->items;
}

void hgrid_free(HordeGrid *g) {
    free(g->bucket_of);
    free(g->start);
    free(g->items);
    memset(g, 0, sizeof(*g));
}

void hgrid_build(HordeGrid *g, const vec3 *pos, const u8 *active, int count) {
    if (count > g->capacity) count = g->capacity;
    memset(g->start, 0, sizeof(int) * (HGRID_BUCKETS + 1));
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (active && !active[i]) { g->bucket_of[i] = 0xFFFFFFFFu; continue; }
        u32 b = cell_hash((int)floorf(pos[i].x * g->inv_cell), (int)floorf(pos[i].z * g->inv_cell));
        g->bucket_of[i] = b;
        g->start[b + 1]++;
        n++;
    }
    for (int b = 0; b < HGRID_BUCKETS; b++) g->start[b + 1] += g->start[b];
    // Counting sort, stable (ascending item index inside a bucket).
    static int cursor[HGRID_BUCKETS];
    memcpy(cursor, g->start, sizeof(int) * HGRID_BUCKETS);
    for (int i = 0; i < count; i++) {
        u32 b = g->bucket_of[i];
        if (b == 0xFFFFFFFFu) continue;
        g->items[cursor[b]++] = i;
    }
    g->count = n;
}

static void insert_sorted(int *out, int *n, int v, int max) {
    // Small insertion sort keeps the output ascending (deterministic neighbour order).
    int k = *n;
    if (k >= max) {
        if (v >= out[max - 1]) return;
        k = max - 1;
    } else {
        (*n)++;
    }
    while (k > 0 && out[k - 1] > v) { out[k] = out[k - 1]; k--; }
    out[k] = v;
}

int hgrid_query(const HordeGrid *g, const vec3 *pos, vec3 p, float radius, int self, int *out, int max) {
    if (max <= 0) return 0;
    int n = 0;
    float r2 = radius * radius;
    int x0 = (int)floorf((p.x - radius) * g->inv_cell), x1 = (int)floorf((p.x + radius) * g->inv_cell);
    int z0 = (int)floorf((p.z - radius) * g->inv_cell), z1 = (int)floorf((p.z + radius) * g->inv_cell);
    // Distinct buckets only (hash collisions between the visited cells would double count).
    u32 seen[64];
    int seen_n = 0;
    for (int cz = z0; cz <= z1; cz++)
        for (int cx = x0; cx <= x1; cx++) {
            u32 b = cell_hash(cx, cz);
            bool dup = false;
            for (int s = 0; s < seen_n; s++) if (seen[s] == b) { dup = true; break; }
            if (dup) continue;
            if (seen_n < 64) seen[seen_n++] = b;
            for (int k = g->start[b]; k < g->start[b + 1]; k++) {
                int j = g->items[k];
                if (j == self) continue;
                float dx = pos[j].x - p.x, dz = pos[j].z - p.z;
                if (dx * dx + dz * dz > r2) continue;
                insert_sorted(out, &n, j, max);
            }
        }
    return n;
}
