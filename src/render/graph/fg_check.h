// Frame graph validation (pure, no Vulkan): given the passes and resources of one frame, finds
//   * dead passes: a pass that writes something, but nothing it writes is consumed (an imported
//     image, a transient read / written by a later pass, a history read by this frame's graph under
//     its own or its ping-pong partner's name: "x_a" <-> "x_b"). Passes without tracked writes are
//     side-effect passes (buffers, readbacks) and count as live.
//   * unused resources: transients / histories declared but used by no scheduled pass.
//   * undefined reads: a transient read before any pass wrote it.
// fg.c fills it every frame (fg_last_check); tests/test_fidelity.c drives it directly.
#ifndef DG_FG_CHECK_H
#define DG_FG_CHECK_H

#include "core/base.h"

enum { FGC_TRANSIENT, FGC_HISTORY, FGC_IMPORT };

typedef struct FgcRes { const char *name; int kind; } FgcRes;
typedef struct FgcUse { int res; bool write; } FgcUse;
typedef struct FgcPass { const char *name; const FgcUse *uses; int use_count; } FgcPass;

#define FGC_MAX_NAMES 12

typedef struct FgcReport {
    int passes, dead_passes, unused_res, undefined_reads;
    char dead[FGC_MAX_NAMES][32];
    char unused[FGC_MAX_NAMES][48];
    char undefined[FGC_MAX_NAMES][48];
} FgcReport;

void fgc_analyze(const FgcPass *passes, int pass_count, const FgcRes *res, int res_count, FgcReport *out);
bool fgc_history_partner(const char *a, const char *b);   // same name, or "<x>_a" vs "<x>_b"

#endif
