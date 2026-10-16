// Local CA + per-IP server certificate, TLS connections, CSPRNG (see hws_tls.h).
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   // POSIX / BSD socket and time declarations under -std=c11
#endif
#include "hws_tls.h"
#include "core/log.h"
#include "core/platform.h"
#include "hws_proto.h"
#include <SDL3/SDL.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CA_DAYS (365 * 10)
#define LEAF_DAYS 397            // browsers / iOS reject longer-lived server certificates (398 days max)
#define LEAF_RENEW_DAYS 30       // re-issue when this close to expiry

struct HwsTls {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context ca_key, leaf_key;
    mbedtls_x509_crt ca, chain;   // chain = leaf + CA
    mbedtls_ssl_config conf;
    char *ca_pem;
    char fingerprint[100];
};

struct HwsTlsConn {
    mbedtls_ssl_context ssl;
    HwsIoFn send_fn, recv_fn;
    void *ctx;
};

static const char *ALPN[] = { "http/1.1", NULL };

static void fmt_err(char *err, size_t n, const char *what, int rc) {
    char e[128];
    mbedtls_strerror(rc, e, sizeof(e));
    snprintf(err, n, "%s: %s (-0x%04x)", what, e, (unsigned)-rc);
}

// ------------------------------------------------------------------ CSPRNG

static struct { bool init; SDL_Mutex *mu; mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg; } g_rng;

bool hws_random(u8 *out, size_t n) {
    if (!g_rng.mu) g_rng.mu = SDL_CreateMutex();
    SDL_LockMutex(g_rng.mu);
    bool ok = true;
    if (!g_rng.init) {
        mbedtls_entropy_init(&g_rng.ent);
        mbedtls_ctr_drbg_init(&g_rng.drbg);
        static const char pers[] = "deadground-phone-rng";
        ok = mbedtls_ctr_drbg_seed(&g_rng.drbg, mbedtls_entropy_func, &g_rng.ent, (const u8 *)pers, sizeof(pers) - 1) == 0;
        g_rng.init = ok;
    }
    if (ok) ok = mbedtls_ctr_drbg_random(&g_rng.drbg, out, n) == 0;
    SDL_UnlockMutex(g_rng.mu);
    return ok;
}

void hws_random_shutdown(void) {
    if (g_rng.init) {
        mbedtls_ctr_drbg_free(&g_rng.drbg);
        mbedtls_entropy_free(&g_rng.ent);
    }
    if (g_rng.mu) SDL_DestroyMutex(g_rng.mu);
    memset(&g_rng, 0, sizeof(g_rng));
}

// ------------------------------------------------------------------ certificates

static void validity(int days_from, int days_to, char nb[16], char na[16]) {
    time_t now = time(NULL);
    time_t a = now + (time_t)days_from * 86400, b = now + (time_t)days_to * 86400;
    struct tm ta, tb;
#if defined(_WIN32)
    gmtime_s(&ta, &a);
    gmtime_s(&tb, &b);
#else
    gmtime_r(&a, &ta);
    gmtime_r(&b, &tb);
#endif
    strftime(nb, 16, "%Y%m%d%H%M%S", &ta);
    strftime(na, 16, "%Y%m%d%H%M%S", &tb);
}

static int gen_key(HwsTls *t, mbedtls_pk_context *k) {
    int rc = mbedtls_pk_setup(k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc == 0) rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*k), mbedtls_ctr_drbg_random, &t->drbg);
    return rc;
}

static int load_key(HwsTls *t, mbedtls_pk_context *k, const char *path) {
    size_t n;
    char *pem = platform_read_file(path, &n);
    if (!pem) return -1;
    int rc = mbedtls_pk_parse_key(k, (const u8 *)pem, n + 1, NULL, 0, mbedtls_ctr_drbg_random, &t->drbg);
    mbedtls_platform_zeroize(pem, n);
    free(pem);
    return rc;
}

static int load_crt(mbedtls_x509_crt *c, const char *path) {
    size_t n;
    char *pem = platform_read_file(path, &n);
    if (!pem) return -1;
    int rc = mbedtls_x509_crt_parse(c, (const u8 *)pem, n + 1);
    free(pem);
    return rc;
}

static bool write_pem_key(const mbedtls_pk_context *k, const char *path) {
    u8 buf[2048];
    if (mbedtls_pk_write_key_pem((mbedtls_pk_context *)k, buf, sizeof(buf)) != 0) return false;
    bool ok = platform_write_file(path, buf, strlen((char *)buf));
    mbedtls_platform_zeroize(buf, sizeof(buf));
    return ok;
}

// Issues a certificate for subject_key, signed by issuer_key. ip == 0: the CA itself.
static int issue(HwsTls *t, mbedtls_pk_context *subject_key, mbedtls_pk_context *issuer_key, const char *subject, const char *issuer, u32 ip, int days,
                 const char *path) {
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, subject_key);
    mbedtls_x509write_crt_set_issuer_key(&w, issuer_key);
    int rc = mbedtls_x509write_crt_set_subject_name(&w, subject);
    if (rc == 0) rc = mbedtls_x509write_crt_set_issuer_name(&w, issuer);
    u8 serial[16];
    mbedtls_ctr_drbg_random(&t->drbg, serial, sizeof(serial));
    serial[0] &= 0x7f;   // positive
    serial[0] |= 0x01;
    if (rc == 0) rc = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof(serial));
    char nb[16], na[16];
    validity(-1, days, nb, na);
    if (rc == 0) rc = mbedtls_x509write_crt_set_validity(&w, nb, na);
    if (rc == 0) rc = mbedtls_x509write_crt_set_subject_key_identifier(&w);
    if (rc == 0) rc = mbedtls_x509write_crt_set_authority_key_identifier(&w);
    if (ip == 0) {
        if (rc == 0) rc = mbedtls_x509write_crt_set_basic_constraints(&w, 1, 0);
        if (rc == 0) rc = mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN);
    } else {
        if (rc == 0) rc = mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1);
        if (rc == 0) rc = mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        mbedtls_asn1_sequence eku = { 0 };
        eku.buf.tag = MBEDTLS_ASN1_OID;
        eku.buf.p = (u8 *)MBEDTLS_OID_SERVER_AUTH;
        eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
        if (rc == 0) rc = mbedtls_x509write_crt_set_ext_key_usage(&w, &eku);
        u8 ipb[4] = { (u8)(ip >> 24), (u8)(ip >> 16), (u8)(ip >> 8), (u8)ip };
        mbedtls_x509_san_list san = { 0 };
        san.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        san.node.san.unstructured_name.p = ipb;
        san.node.san.unstructured_name.len = 4;
        if (rc == 0) rc = mbedtls_x509write_crt_set_subject_alternative_name(&w, &san);
    }
    u8 pem[4096];
    if (rc == 0) rc = mbedtls_x509write_crt_pem(&w, pem, sizeof(pem), mbedtls_ctr_drbg_random, &t->drbg);
    if (rc == 0 && !platform_write_file(path, pem, strlen((char *)pem))) rc = -1;
    mbedtls_x509write_crt_free(&w);
    return rc;
}

// The leaf is reused when it names this IP, verifies against the CA and has > 30 days left.
static bool leaf_usable(HwsTls *t, const char *crt_path, u32 ip) {
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    bool ok = load_crt(&c, crt_path) == 0;
    if (ok) {
        u32 flags = 0;
        ok = mbedtls_x509_crt_verify(&c, &t->ca, NULL, NULL, &flags, NULL, NULL) == 0;
        mbedtls_x509_time soon;
        time_t when = time(NULL) + (time_t)LEAF_RENEW_DAYS * 86400;
        struct tm tm;
#if defined(_WIN32)
        gmtime_s(&tm, &when);
#else
        gmtime_r(&when, &tm);
#endif
        soon.year = tm.tm_year + 1900;
        soon.mon = tm.tm_mon + 1;
        soon.day = tm.tm_mday;
        soon.hour = tm.tm_hour;
        soon.min = tm.tm_min;
        soon.sec = tm.tm_sec;
        if (ok && mbedtls_x509_time_cmp(&c.valid_to, &soon) < 0) ok = false;
        // The IP SAN must match.
        bool ip_ok = false;
        const mbedtls_x509_sequence *s = &c.subject_alt_names;
        while (ok && s && s->buf.p) {
            mbedtls_x509_subject_alternative_name san;
            if (mbedtls_x509_parse_subject_alt_name(&s->buf, &san) == 0) {
                if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS && san.san.unstructured_name.len == 4) {
                    const u8 *p = san.san.unstructured_name.p;
                    ip_ok |= ((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]) == ip;
                }
                mbedtls_x509_free_subject_alt_name(&san);
            }
            s = s->next;
        }
        ok = ok && ip_ok;
    }
    mbedtls_x509_crt_free(&c);
    return ok;
}

HwsTls *hws_tls_create(const char *dir, u32 lan_ip, char *err, size_t err_size) {
    HwsTls *t = (HwsTls *)calloc(1, sizeof(HwsTls));
    if (!t) return NULL;
    psa_crypto_init();   // TLS 1.3 / PSA-backed key exchange
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_pk_init(&t->ca_key);
    mbedtls_pk_init(&t->leaf_key);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_x509_crt_init(&t->chain);
    mbedtls_ssl_config_init(&t->conf);
    static const char pers[] = "deadground-phone-tls";
    int rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, (const u8 *)pers, sizeof(pers) - 1);
    if (rc) { fmt_err(err, err_size, "rng", rc); goto fail; }
    SDL_CreateDirectory(dir);
    char ca_key[512], ca_crt[512], lk[512], lc[512], ipstr[20];
    hws_ipv4_format(lan_ip, ipstr, sizeof(ipstr));
    snprintf(ca_key, sizeof(ca_key), "%sca.key", dir);
    snprintf(ca_crt, sizeof(ca_crt), "%sca.crt", dir);
    snprintf(lk, sizeof(lk), "%sserver_%s.key", dir, ipstr);
    snprintf(lc, sizeof(lc), "%sserver_%s.crt", dir, ipstr);

    // Root CA: generated once.
    if (load_key(t, &t->ca_key, ca_key) != 0 || load_crt(&t->ca, ca_crt) != 0) {
        mbedtls_pk_free(&t->ca_key);
        mbedtls_pk_init(&t->ca_key);
        mbedtls_x509_crt_free(&t->ca);
        mbedtls_x509_crt_init(&t->ca);
        log_info("phone: generating the local root certificate (%s)", ca_crt);
        rc = gen_key(t, &t->ca_key);
        if (rc == 0 && !write_pem_key(&t->ca_key, ca_key)) rc = -1;
        if (rc == 0) rc = issue(t, &t->ca_key, &t->ca_key, "CN=Deadground Local Phone CA,O=Deadground", "CN=Deadground Local Phone CA,O=Deadground", 0,
                                CA_DAYS, ca_crt);
        if (rc == 0) rc = load_crt(&t->ca, ca_crt);
        if (rc) { fmt_err(err, err_size, "root certificate", rc); goto fail; }
    }
    // Server certificate for this IP.
    if (!leaf_usable(t, lc, lan_ip) || load_key(t, &t->leaf_key, lk) != 0) {
        mbedtls_pk_free(&t->leaf_key);
        mbedtls_pk_init(&t->leaf_key);
        log_info("phone: issuing a server certificate for %s", ipstr);
        rc = gen_key(t, &t->leaf_key);
        if (rc == 0 && !write_pem_key(&t->leaf_key, lk)) rc = -1;
        char subj[96];
        snprintf(subj, sizeof(subj), "CN=%s,O=Deadground", ipstr);
        if (rc == 0) rc = issue(t, &t->leaf_key, &t->ca_key, subj, "CN=Deadground Local Phone CA,O=Deadground", lan_ip, LEAF_DAYS, lc);
        if (rc) { fmt_err(err, err_size, "server certificate", rc); goto fail; }
    }
    rc = load_crt(&t->chain, lc);
    if (rc == 0) rc = load_crt(&t->chain, ca_crt);
    if (rc) { fmt_err(err, err_size, "certificate chain", rc); goto fail; }

    t->ca_pem = platform_read_file(ca_crt, NULL);
    u8 dig[32];
    mbedtls_sha256(t->ca.raw.p, t->ca.raw.len, dig, 0);
    for (int i = 0; i < 32; i++) snprintf(t->fingerprint + i * 3, 4, i < 31 ? "%02X:" : "%02X", dig[i]);

    rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) {
        mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
        mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_alpn_protocols(&t->conf, ALPN);
        rc = mbedtls_ssl_conf_own_cert(&t->conf, &t->chain, &t->leaf_key);
    }
    if (rc) { fmt_err(err, err_size, "tls config", rc); goto fail; }
    return t;
fail:
    hws_tls_destroy(t);
    return NULL;
}

void hws_tls_destroy(HwsTls *t) {
    if (!t) return;
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->chain);
    mbedtls_x509_crt_free(&t->ca);
    mbedtls_pk_free(&t->leaf_key);
    mbedtls_pk_free(&t->ca_key);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
    free(t->ca_pem);
    free(t);
}

const char *hws_tls_ca_pem(const HwsTls *t) { return t && t->ca_pem ? t->ca_pem : ""; }
const u8 *hws_tls_ca_der(const HwsTls *t, size_t *len) {
    if (len) *len = t ? t->ca.raw.len : 0;
    return t ? t->ca.raw.p : NULL;
}
const char *hws_tls_fingerprint(const HwsTls *t) { return t ? t->fingerprint : ""; }

// ------------------------------------------------------------------ connections

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    HwsTlsConn *c = (HwsTlsConn *)ctx;
    int n = c->send_fn(c->ctx, (u8 *)buf, len);
    return n > 0 ? n : (n == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_CONN_RESET);
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    HwsTlsConn *c = (HwsTlsConn *)ctx;
    int n = c->recv_fn(c->ctx, buf, len);
    return n > 0 ? n : (n == 0 ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_CONN_RESET);
}

HwsTlsConn *hws_tls_conn_new(HwsTls *t, HwsIoFn send_fn, HwsIoFn recv_fn, void *ctx) {
    HwsTlsConn *c = (HwsTlsConn *)calloc(1, sizeof(HwsTlsConn));
    if (!c) return NULL;
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &t->conf) != 0) {
        mbedtls_ssl_free(&c->ssl);
        free(c);
        return NULL;
    }
    c->send_fn = send_fn;
    c->recv_fn = recv_fn;
    c->ctx = ctx;
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    return c;
}

void hws_tls_conn_free(HwsTlsConn *c) {
    if (!c) return;
    mbedtls_ssl_free(&c->ssl);
    free(c);
}

int hws_tls_handshake(HwsTlsConn *c) {
    int rc = mbedtls_ssl_handshake(&c->ssl);
    if (rc == 0) return 1;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return 0;
    char e[128];
    mbedtls_strerror(rc, e, sizeof(e));
    log_debug("phone: TLS handshake failed: %s (-0x%04x)%s", e, (unsigned)-rc,
              rc == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE ? " - the browser rejected the certificate (expected until it is trusted)" : "");
    return -1;
}

int hws_tls_read(HwsTlsConn *c, u8 *buf, size_t len) {
    int rc = mbedtls_ssl_read(&c->ssl, buf, len);
    if (rc > 0) return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return 0;
    return -1;   // 0 (EOF), close notify, errors
}

int hws_tls_write(HwsTlsConn *c, const u8 *buf, size_t len) {
    int rc = mbedtls_ssl_write(&c->ssl, buf, len);
    if (rc > 0) return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    return -1;
}

size_t hws_tls_pending(HwsTlsConn *c) { return mbedtls_ssl_get_bytes_avail(&c->ssl); }
