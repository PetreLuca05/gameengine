// img_diff: compares two screenshots (PNG): the share of pixels whose max channel difference exceeds a
// threshold. Used by the vgeo vs legacy check (docs/VGEO.md "Tests").
//   img_diff a.png b.png [threshold=24] [max_percent=1.0]   -> exit 0 when within max_percent
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: img_diff a.png b.png [threshold] [max_percent]\n"); return 2; }
    int thr = argc > 3 ? atoi(argv[3]) : 24;
    double maxp = argc > 4 ? atof(argv[4]) : 1.0;
    int aw, ah, bw, bh, c;
    unsigned char *a = stbi_load(argv[1], &aw, &ah, &c, 3), *b = stbi_load(argv[2], &bw, &bh, &c, 3);
    if (!a || !b || aw != bw || ah != bh) { fprintf(stderr, "img_diff: cannot load / size mismatch\n"); return 2; }
    long over = 0, n = (long)aw * ah;
    double sum = 0;
    for (long i = 0; i < n; i++) {
        int d = 0;
        for (int k = 0; k < 3; k++) {
            int e = abs((int)a[i * 3 + k] - (int)b[i * 3 + k]);
            if (e > d) d = e;
        }
        sum += d;
        if (d > thr) over++;
    }
    double pct = 100.0 * (double)over / (double)n;
    printf("img_diff: %ld of %ld pixels over %d (%.3f %%), mean diff %.2f\n", over, n, thr, pct, sum / (double)n);
    return pct <= maxp ? 0 : 1;
}
