// QR code generation (see phone_qr.h).
#include "phone_qr.h"
#include "qrcodegen/qrcodegen.h"
#include <stdlib.h>

bool phone_qr_encode(const char *text, PhoneQr *out) {
    static DG_THREAD_LOCAL u8 tmp[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
    memset(out, 0, sizeof(*out));
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) return false;
    out->size = qrcodegen_getSize(qr);
    for (int y = 0; y < out->size; y++)
        for (int x = 0; x < out->size; x++) out->mod[y * out->size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
    return true;
}

u8 *phone_qr_rgba(const PhoneQr *q, int scale, int border, int *w) {
    int n = (q->size + 2 * border) * scale;
    u8 *px = (u8 *)malloc((size_t)n * (size_t)n * 4);
    if (!px) return NULL;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int mx = x / scale - border, my = y / scale - border;
            bool dark = mx >= 0 && my >= 0 && mx < q->size && my < q->size && q->mod[my * q->size + mx];
            u8 *p = px + ((size_t)y * (size_t)n + (size_t)x) * 4;
            p[0] = p[1] = p[2] = dark ? 0 : 255;
            p[3] = 255;
        }
    *w = n;
    return px;
}
