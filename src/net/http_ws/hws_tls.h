// TLS for the phone companion server (mbedTLS 3.6): a local root CA generated once per install
// and a server certificate for the current LAN IP signed by it (re-issued when the IP changes or
// it nears expiry), so a trust decision about the CA survives IP changes. Also the CSPRNG used
// for pairing tokens. See docs/PHONE.md "HTTPS and permissions".
#ifndef DG_HWS_TLS_H
#define DG_HWS_TLS_H

#include "core/base.h"

typedef struct HwsTls HwsTls;
typedef struct HwsTlsConn HwsTlsConn;

// dir: where ca.key / ca.crt / server_<ip>.key / .crt live (created if missing). lan_ip in host
// order (the certificate's IP subjectAltName). NULL on failure (err filled).
HwsTls *hws_tls_create(const char *dir, u32 lan_ip, char *err, size_t err_size);
void hws_tls_destroy(HwsTls *t);
const char *hws_tls_ca_pem(const HwsTls *t);           // the root certificate (PEM), for /ca.crt
const u8 *hws_tls_ca_der(const HwsTls *t, size_t *len); // DER (iOS profile install / Android)
const char *hws_tls_fingerprint(const HwsTls *t);      // SHA-256 of the CA (hex, colon separated), shown in game

// Per connection, over caller-supplied non-blocking I/O. send / recv return bytes, 0 when they
// would block, < 0 on error / close.
typedef int (*HwsIoFn)(void *ctx, u8 *buf, size_t len);
HwsTlsConn *hws_tls_conn_new(HwsTls *t, HwsIoFn send_fn, HwsIoFn recv_fn, void *ctx);
void hws_tls_conn_free(HwsTlsConn *c);
int hws_tls_handshake(HwsTlsConn *c);                  // 1 done, 0 in progress, -1 failed
int hws_tls_read(HwsTlsConn *c, u8 *buf, size_t len);  // > 0 bytes, 0 would block, -1 closed / error
int hws_tls_write(HwsTlsConn *c, const u8 *buf, size_t len);   // > 0 bytes, 0 would block, -1 error
size_t hws_tls_pending(HwsTlsConn *c);                 // decrypted bytes buffered inside mbedTLS

// Cryptographically secure random bytes (mbedTLS CTR-DRBG seeded from the OS). Thread safe.
bool hws_random(u8 *out, size_t n);
void hws_random_shutdown(void);

#endif
