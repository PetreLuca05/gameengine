// QR codes for phone pairing (Project Nayuki qrcodegen, MIT). Byte mode, error correction M
// (boosted when it fits), version chosen automatically.
#ifndef DG_PHONE_QR_H
#define DG_PHONE_QR_H

#include "core/base.h"

#define PHONE_QR_MAX 177   // modules per side (version 40)

typedef struct PhoneQr { int size; u8 mod[PHONE_QR_MAX * PHONE_QR_MAX]; } PhoneQr;   // 1 = dark

bool phone_qr_encode(const char *text, PhoneQr *out);
// Greyscale-free RGBA image: dark modules black, light white, a quiet zone of `border` modules.
// Returns malloc'd w * w * 4 bytes.
u8 *phone_qr_rgba(const PhoneQr *q, int scale, int border, int *w);

#endif
