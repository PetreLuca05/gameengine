// anim_probe <clip> [bone suffix] [steps]: a bone's model-space position over the clip
// (normalized time), e.g. to find where a motion really starts (anim_sets/*.json sub-ranges)
// or whether a clip moves the head in steps.
#include "anim/anim_library.h"
#include "core/engine.h"
#include "core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

int main(int argc, char **argv) {
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    AnimLibrary *L = (AnimLibrary *)calloc(1, sizeof(AnimLibrary));
    if (!anim_lib_load(L, 30.0f)) return 1;
    const char *name = argc > 1 ? argv[1] : "jump";
    int ci = anim_lib_find_clip(L, name);
    if (ci < 0) { printf("no clip %s\n", name); return 1; }
    const Clip *c = &L->clips[ci];
    int bone = skel_find_suffix(&L->skel, argc > 2 ? argv[2] : "Hips");
    if (bone < 0) { printf("no bone\n"); return 1; }
    int steps = argc > 3 ? atoi(argv[3]) : 50;
    printf("clip %s duration %.3f s\n", name, c->duration);
    static Pose p;
    static Xform m[ANIM_MAX_BONES];
    for (int i = 0; i <= steps; i++) {
        float ph = (float)i / (float)steps;
        clip_sample_phase(c, ph, &p);
        pose_to_model(&L->skel, &p, m);
        printf("%.4f  t %.3f  %.4f %.4f %.4f\n", ph, ph * c->duration, m[bone].t.x, m[bone].t.y, m[bone].t.z);
    }
    return 0;
}
