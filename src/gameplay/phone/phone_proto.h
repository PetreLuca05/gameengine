// Phone companion protocol (WebSocket, little-endian). Mirrored in web/phone/index.html; any
// change here needs the page updated too (PHONE_PROTO_VERSION is checked in the hello).
//
// Binary, page -> game
//   PHONE_MSG_ORIENT (16 bytes)  u8 type, u8 flags (bit0 = screen landscape), u16 seq,
//                                u32 page time (ms), i16 qx qy qz qw (/ 32767): device -> earth
//                                quaternion, W3C Rz(alpha) Rx(beta) Ry(gamma).
// Binary, game -> page
//   PHONE_MSG_FRAME              u8 type, u8 flags (bit0 recording), u16 w, u16 h, u16 quality,
//                                u32 frame id, u32 capture time (game ms), f32 zoom, then the JPEG.
//   PHONE_MSG_PHOTO              u8 type, u8 pad[3], u32 media id, then the JPEG (save-to-phone blob).
// Text (JSON), page -> game: {"c":"hello","v":N,"ua":"..."}, {"c":"shutter"}, {"c":"record"},
//   {"c":"zoom","v":2.5}, {"c":"calibrate"}, {"c":"recenter"}, {"c":"unpair"},
//   {"c":"ack","f":frame_id,"pt":page_ms}, {"c":"ping","pt":page_ms}, {"c":"gallery"},
//   {"c":"perm","orientation":true|false}
// Text (JSON), game -> page: {"t":"session",...} (server), {"t":"status",...}, {"t":"pong",...},
//   {"t":"media",...}, {"t":"gallery","items":[...]}, {"t":"toast","msg":"..."}, {"t":"unpaired"}
#ifndef DG_PHONE_PROTO_H
#define DG_PHONE_PROTO_H

#include "core/base.h"

#define PHONE_PROTO_VERSION 1

enum { PHONE_MSG_ORIENT = 1, PHONE_MSG_FRAME = 16, PHONE_MSG_PHOTO = 17 };

#define PHONE_ORIENT_BYTES 16
#define PHONE_FRAME_HEADER 24

typedef struct PhoneOrientMsg { u8 flags; u16 seq; u32 page_ms; i16 q[4]; } PhoneOrientMsg;

DG_INLINE void phone_put_u16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
DG_INLINE void phone_put_u32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
DG_INLINE u16 phone_get_u16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
DG_INLINE u32 phone_get_u32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

DG_INLINE bool phone_parse_orient(const u8 *b, int len, PhoneOrientMsg *m) {
    if (len != PHONE_ORIENT_BYTES || b[0] != PHONE_MSG_ORIENT) return false;
    m->flags = b[1];
    m->seq = phone_get_u16(b + 2);
    m->page_ms = phone_get_u32(b + 4);
    for (int i = 0; i < 4; i++) m->q[i] = (i16)phone_get_u16(b + 8 + i * 2);
    return true;
}

DG_INLINE int phone_write_orient(u8 out[PHONE_ORIENT_BYTES], const PhoneOrientMsg *m) {
    out[0] = PHONE_MSG_ORIENT;
    out[1] = m->flags;
    phone_put_u16(out + 2, m->seq);
    phone_put_u32(out + 4, m->page_ms);
    for (int i = 0; i < 4; i++) phone_put_u16(out + 8 + i * 2, (u16)m->q[i]);
    return PHONE_ORIENT_BYTES;
}

DG_INLINE void phone_write_frame_header(u8 out[PHONE_FRAME_HEADER], u8 flags, u16 w, u16 h, u16 quality, u32 frame, u32 capture_ms, float zoom) {
    out[0] = PHONE_MSG_FRAME;
    out[1] = flags;
    phone_put_u16(out + 2, w);
    phone_put_u16(out + 4, h);
    phone_put_u16(out + 6, quality);
    phone_put_u32(out + 8, frame);
    phone_put_u32(out + 12, capture_ms);
    u32 z;
    memcpy(&z, &zoom, 4);
    phone_put_u32(out + 16, z);
    phone_put_u32(out + 20, 0);
}

#endif
