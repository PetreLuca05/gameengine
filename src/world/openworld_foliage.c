#include "openworld_internal.h"
#include "core/log.h"
#include <stdlib.h>

// Trees and ground cover of open-world maps (docs/ZOMBIE_MAP.md "Vegetation"). Stage 1: none.

void ow_foliage_load(World *w, OpenWorld *o) {
    (void)w;
    (void)o;
}

void ow_foliage_free(World *w, OpenWorld *o) {
    (void)w;
    (void)o;
}

void ow_foliage_frame(World *w, OpenWorld *o, vec3 cam) {
    (void)w;
    (void)o;
    (void)cam;
}

void *ow_foliage_chunk(void *user, int cx, int cz, size_t *size) {
    (void)user;
    (void)cx;
    (void)cz;
    *size = 0;
    return NULL;
}
