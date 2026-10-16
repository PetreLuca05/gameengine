#include "fg_check.h"
#include <stdio.h>
#include <string.h>

bool fgc_history_partner(const char *a, const char *b) {
    if (!strcmp(a, b)) return true;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb || la < 2) return false;
    if (strncmp(a, b, la - 1) || a[la - 2] != '_') return false;
    char x = a[la - 1], y = b[la - 1];
    return (x == 'a' && y == 'b') || (x == 'b' && y == 'a');
}

static void note(char (*list)[48], int *count, const char *name) {
    if (*count < FGC_MAX_NAMES) snprintf(list[*count], 48, "%s", name);
    (*count)++;
}

void fgc_analyze(const FgcPass *P, int np, const FgcRes *R, int nr, FgcReport *out) {
    memset(out, 0, sizeof(*out));
    out->passes = np;
    // Resource usage: first pass, any use, history reads.
    for (int r = 0; r < nr; r++) {
        if (R[r].kind == FGC_IMPORT) continue;
        int first = -1;
        bool first_writes = false;
        for (int p = 0; p < np && first < 0; p++)
            for (int u = 0; u < P[p].use_count; u++)
                if (P[p].uses[u].res == r) {
                    first = p;
                    for (int v = 0; v < P[p].use_count; v++) if (P[p].uses[v].res == r && P[p].uses[v].write) first_writes = true;
                    break;
                }
        if (first < 0) note(out->unused, &out->unused_res, R[r].name);
        else if (R[r].kind == FGC_TRANSIENT && !first_writes) note(out->undefined, &out->undefined_reads, R[r].name);
    }
    for (int p = 0; p < np; p++) {
        int writes = 0;
        bool live = false;
        for (int u = 0; u < P[p].use_count && !live; u++) {
            if (!P[p].uses[u].write) continue;
            int r = P[p].uses[u].res;
            if (r < 0 || r >= nr) continue;
            writes++;
            if (R[r].kind == FGC_IMPORT) { live = true; break; }
            if (R[r].kind == FGC_TRANSIENT) {
                for (int q = p + 1; q < np && !live; q++)
                    for (int v = 0; v < P[q].use_count; v++)
                        if (P[q].uses[v].res == r) { live = true; break; }
            } else {
                // History: consumed if this frame's graph reads it (or its ping-pong partner).
                for (int q = 0; q < np && !live; q++)
                    for (int v = 0; v < P[q].use_count; v++) {
                        int s = P[q].uses[v].res;
                        if (q != p && !P[q].uses[v].write && s >= 0 && s < nr && R[s].kind == FGC_HISTORY && fgc_history_partner(R[s].name, R[r].name)) { live = true; break; }
                    }
            }
        }
        if (writes > 0 && !live) {
            if (out->dead_passes < FGC_MAX_NAMES) snprintf(out->dead[out->dead_passes], 32, "%s", P[p].name);
            out->dead_passes++;
        }
    }
}
