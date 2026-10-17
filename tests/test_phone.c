// In-game phone + companion page (docs/PHONE.md): QR round trip through a decoder, pairing token
// auth (wrong / expired / replayed tokens, sessions, rate limits), HTTP / WebSocket protocol
// pieces, orientation conversion on reference poses, filtering + reach clamp, quaternion packing,
// JPEG pipeline throughput on the job system, photo / video files (headers parsed back) and their
// sidecars, and a headless client round trip against the real server: pair -> 100 frames ->
// photo -> 2 s video -> unpair. Plus TLS: certificate generation and a handshake with mbedTLS.
// Phone OS: the pairing flow, the screen layout + hit tests, assets/phone/*.json + the icon atlas,
// the companion page injection, the HUD panel at 16:9 / 21:9 / 4:3, gallery delete.
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET tsock;
#define TSOCK_BAD INVALID_SOCKET
#define tclose closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int tsock;
#define TSOCK_BAD (-1)
#define tclose close
#endif
#include "core/engine.h"
#include "core/jobs.h"
#include "core/platform.h"
#include "gameplay/phone/phone_capture.h"
#include "gameplay/phone/phone_orient.h"
#include "gameplay/phone/phone_os.h"
#include "gameplay/phone/phone_proto.h"
#include "gameplay/phone/phone_qr.h"
#include "gameplay/phone/phone_stream.h"
#include "gameplay/phone/phone_video.h"
#include "net/http_ws/hws_auth.h"
#include "net/http_ws/hws_proto.h"
#include "net/http_ws/hws_server.h"
#include "net/http_ws/hws_tls.h"
#include "quirc/quirc.h"
#include "cJSON.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;
static int g_fail, g_checks;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static double now_s(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

// Angle between rotations from the vector part of a^-1 b (well conditioned near zero, unlike acos).
static float qangle_deg(quat a, quat b) {
    quat d = quat_mul(quat_conj(quat_norm(a)), quat_norm(b));
    float v = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0f * atan2f(v, fabsf(d.w)) * DG_RAD2DEG;
}

static float vang_deg(vec3 a, vec3 b) { return acosf(clampf(v3_dot(v3_norm(a), v3_norm(b)), -1, 1)) * DG_RAD2DEG; }

// ------------------------------------------------------------------ QR

static void test_qr(void) {
    const char *url = "https://192.168.1.42:47820/?t=Qm9vb2stZmluZS10b2tlbg";
    PhoneQr qr;
    CHECK(phone_qr_encode(url, &qr), "encode");
    CHECK(qr.size >= 21 && qr.size <= PHONE_QR_MAX && (qr.size - 17) % 4 == 0, "QR size %d is a valid version", qr.size);
    int w = 0;
    u8 *rgba = phone_qr_rgba(&qr, 4, 4, &w);
    CHECK(rgba && w == (qr.size + 8) * 4, "image size");
    // Decode it back with quirc (an independent decoder).
    struct quirc *q = quirc_new();
    CHECK(q && quirc_resize(q, w, w) == 0, "quirc init");
    int qw, qh;
    u8 *img = quirc_begin(q, &qw, &qh);
    for (int i = 0; i < w * w; i++) img[i] = rgba[i * 4];
    quirc_end(q);
    CHECK(quirc_count(q) == 1, "one code found (%d)", quirc_count(q));
    if (quirc_count(q) == 1) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, 0, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        CHECK(err == QUIRC_SUCCESS, "decode: %s", quirc_strerror(err));
        CHECK(err == QUIRC_SUCCESS && strcmp((const char *)data.payload, url) == 0, "round trip: '%s'", (const char *)data.payload);
    }
    quirc_destroy(q);
    free(rgba);
    // Finder pattern in the top-left corner (fixed by the standard): 7x7 dark ring.
    CHECK(qr.mod[0] == 1 && qr.mod[6] == 1 && qr.mod[6 * qr.size] == 1 && qr.mod[1 * qr.size + 1] == 0 && qr.mod[3 * qr.size + 3] == 1, "finder pattern");
}

// ------------------------------------------------------------------ auth

static void rnd_fill(u8 r[16], u8 seed) {
    for (int i = 0; i < 16; i++) r[i] = (u8)(seed * 31 + i * 7);
}

static void test_auth(void) {
    HwsAuth a;
    hws_auth_init(&a, 120000);
    u8 r[16];
    rnd_fill(r, 1);
    char tok[HWS_KEY_CHARS + 1];
    dg_strcpy(tok, sizeof(tok), hws_auth_issue(&a, 0, r, 1000));
    CHECK(strlen(tok) == HWS_KEY_CHARS, "token length %zu", strlen(tok));
    for (const char *p = tok; *p; p++) CHECK((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_', "url safe");
    int owner = -1;
    char sess[HWS_KEY_CHARS + 1];
    rnd_fill(r, 2);
    CHECK(hws_auth_pair(&a, "AAAAAAAAAAAAAAAAAAAAAA", 2000, r, &owner, sess) == HWS_AUTH_BAD, "wrong token rejected");
    CHECK(hws_auth_pair(&a, "", 2000, r, &owner, sess) == HWS_AUTH_BAD, "empty token rejected");
    char shorter[HWS_KEY_CHARS];
    memcpy(shorter, tok, HWS_KEY_CHARS - 1);
    shorter[HWS_KEY_CHARS - 1] = 0;
    CHECK(hws_auth_pair(&a, shorter, 2000, r, &owner, sess) == HWS_AUTH_BAD, "prefix of the token rejected");
    CHECK(fabsf(hws_auth_token_remaining(&a, 0, 61000) - 60.0f) < 0.01f, "remaining time");
    CHECK(hws_auth_pair(&a, tok, 2000, r, &owner, sess) == HWS_AUTH_OK && owner == 0, "right token accepted");
    CHECK(hws_auth_pair(&a, tok, 2500, r, &owner, sess) == HWS_AUTH_BAD, "token is one-time (replay rejected)");
    CHECK(hws_auth_session(&a, sess) == 0, "session valid");
    CHECK(hws_auth_session(&a, "nope") < 0, "bad session");
    // Expiry.
    rnd_fill(r, 3);
    dg_strcpy(tok, sizeof(tok), hws_auth_issue(&a, 1, r, 10000));
    CHECK(hws_auth_pair(&a, tok, 10000 + 120000, r, &owner, sess) == HWS_AUTH_EXPIRED, "expired token rejected at exactly 2 min");
    CHECK(hws_auth_pair(&a, tok, 10000 + 1000, r, &owner, sess) == HWS_AUTH_BAD, "an expired token is gone");
    // Re-pair: a new token for owner 0 keeps the old session until it is used, then replaces it.
    char old[HWS_KEY_CHARS + 1];
    memcpy(old, a.owner[0].session, sizeof(old));
    rnd_fill(r, 4);
    dg_strcpy(tok, sizeof(tok), hws_auth_issue(&a, 0, r, 20000));
    CHECK(hws_auth_session(&a, old) == 0, "old session still valid while re-pairing");
    rnd_fill(r, 5);
    CHECK(hws_auth_pair(&a, tok, 21000, r, &owner, sess) == HWS_AUTH_OK, "re-pair");
    CHECK(hws_auth_session(&a, old) < 0 && hws_auth_session(&a, sess) == 0, "the old phone's session is revoked");
    hws_auth_unpair(&a, 0);
    CHECK(hws_auth_session(&a, sess) < 0, "unpair revokes the session");
    // Rate limiting: burst of 20 then throttled; refills at 10 / s; 5 failures -> ban.
    u32 ip = 0xC0A80105u;
    int allowed = 0;
    for (int i = 0; i < 40; i++) allowed += hws_rate_allow(&a, ip, 50000);
    CHECK(allowed == 20, "burst of 20 (%d)", allowed);
    CHECK(hws_rate_allow(&a, ip, 50000 + 200), "refills (10 / s)");
    for (int i = 0; i < 5; i++) hws_rate_fail(&a, ip, 60000 + (u64)i);
    CHECK(hws_rate_banned(&a, ip, 60100) && !hws_rate_allow(&a, ip, 60100), "5 failures ban the address");
    CHECK(!hws_rate_banned(&a, ip, 60100 + 61000), "ban lifts after 60 s");
    CHECK(!hws_rate_banned(&a, 0xC0A80106u, 60100), "other addresses unaffected");
}

// ------------------------------------------------------------------ protocol

static void test_proto(void) {
    const char *req = "GET /ws?t=abc%2Dd&x=1 HTTP/1.1\r\nHost: 10.0.0.2:47820\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    HwsRequest rq;
    CHECK(hws_parse_request(req, (int)strlen(req) - 3, &rq) == 0, "incomplete request needs more");
    CHECK(hws_parse_request(req, (int)strlen(req), &rq) == 1, "parse");
    CHECK(hws_path_is(&rq, "/ws") && rq.header_count == 5, "path / headers");
    char v[32];
    CHECK(hws_query_param(rq.query, rq.query_len, "t", v, sizeof(v)) && strcmp(v, "abc-d") == 0, "query param percent-decoded");
    CHECK(!hws_query_param(rq.query, rq.query_len, "s", v, sizeof(v)), "absent param");
    CHECK(hws_is_ws_upgrade(&rq), "valid upgrade");
    char acc[29];
    int kl;
    const char *k = hws_header(&rq, "sec-websocket-key", &kl);
    hws_ws_accept_key(k, kl, acc);
    CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "RFC 6455 accept key example (%s)", acc);
    CHECK(hws_parse_request("GET  / HTTP/1.1\r\n\r\n", 19, &rq) < 0, "malformed request line");
    CHECK(hws_parse_request("GET /\x01 HTTP/1.1\r\n\r\n", 20, &rq) < 0, "control char in target");
    char big[HWS_MAX_REQUEST + 16];
    memset(big, 'a', sizeof(big));
    memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    CHECK(hws_parse_request(big, (int)sizeof(big), &rq) < 0, "oversized request rejected");
    // Frames: fragmented masked text + ping in between, decoded incrementally byte by byte.
    u8 wire[256];
    int n = 0;
    u8 f1[64];
    int l1 = hws_ws_client_frame(f1, sizeof(f1), HWS_OP_TEXT, "hel", 3, 0x11223344u);
    f1[0] &= 0x7f;   // not final
    memcpy(wire + n, f1, (size_t)l1); n += l1;
    n += hws_ws_client_frame(wire + n, (int)sizeof(wire) - n, HWS_OP_PING, "p", 1, 0x55667788u);
    int l3 = hws_ws_client_frame(wire + n, (int)sizeof(wire) - n, HWS_OP_CONT, "lo", 2, 0x99aabbccu);
    n += l3;
    u8 store[64];
    HwsWsDecoder d;
    hws_ws_decoder_init(&d, store, sizeof(store));
    int got_ping = 0, got_msg = 0;
    for (int i = 0; i < n; i++) {
        int used;
        HwsWsResult r = hws_ws_decode(&d, wire + i, 1, &used);
        if (r == HWS_WS_CONTROL) { got_ping += d.opcode == HWS_OP_PING && d.ctrl_len == 1 && d.ctrl[0] == 'p'; hws_ws_decoder_next(&d); }
        if (r == HWS_WS_MESSAGE) { got_msg += d.msg_len == 5 && memcmp(d.msg, "hello", 5) == 0 && d.msg_opcode == HWS_OP_TEXT; hws_ws_decoder_next(&d); }
        CHECK(r != HWS_WS_ERROR, "no decode error at byte %d", i);
    }
    CHECK(got_ping == 1 && got_msg == 1, "fragmented message + interleaved ping (%d %d)", got_ping, got_msg);
    // Unmasked client frames and oversized messages are protocol errors.
    u8 h[10];
    int hl = hws_ws_frame_header(h, HWS_OP_TEXT, true, 3);
    int used;
    hws_ws_decoder_init(&d, store, sizeof(store));
    CHECK(hws_ws_decode(&d, h, hl, &used) == HWS_WS_ERROR, "unmasked frame rejected");
    u8 bigf[200];
    u8 payload[100] = { 0 };
    int bl = hws_ws_client_frame(bigf, sizeof(bigf), HWS_OP_BINARY, payload, 100, 1);
    hws_ws_decoder_init(&d, store, 64);
    CHECK(hws_ws_decode(&d, bigf, bl, &used) == HWS_WS_ERROR, "message over the limit rejected");
    CHECK(hws_ws_frame_header(h, HWS_OP_BINARY, true, 70000) == 10 && h[1] == 127, "64-bit length header");
    // Addresses.
    u32 ip;
    CHECK(hws_ipv4_parse("192.168.0.12", &ip) && ip == 0xC0A8000Cu, "parse ip");
    CHECK(!hws_ipv4_parse("192.168.0", &ip) && !hws_ipv4_parse("1.2.3.256", &ip), "bad ips");
    CHECK(hws_ipv4_is_private(0x0A000001u) && hws_ipv4_is_private(0xAC1F0001u) && !hws_ipv4_is_private(0xAC200001u) && !hws_ipv4_is_private(0x08080808u), "private ranges");
    CHECK(hws_ipv4_lan_rank(0xC0A80001u) > hws_ipv4_lan_rank(0x0A000001u) && hws_ipv4_lan_rank(0x08080808u) == 0, "lan ranking");
    CHECK(strcmp(hws_mime_for("a.MP4"), "video/mp4") == 0 && strcmp(hws_mime_for("x.jpg"), "image/jpeg") == 0, "mime");
    // Orientation message round trip.
    PhoneOrientMsg m = { 1, 513, 123456, { 1, -2, 3, 32767 } }, m2;
    u8 ob[PHONE_ORIENT_BYTES];
    phone_write_orient(ob, &m);
    CHECK(phone_parse_orient(ob, PHONE_ORIENT_BYTES, &m2) && m2.seq == 513 && m2.page_ms == 123456 && m2.q[1] == -2 && m2.q[3] == 32767, "orient message");
}

// ------------------------------------------------------------------ orientation

static void test_orientation(void) {
    // Reference poses (W3C angles -> where the in-game lens / screen top point, uncalibrated).
    struct { float a, b, g; vec3 lens, top; const char *name; } ref[] = {
        { 0, 0, 0, { 0, -1, 0 }, { 0, 0, -1 }, "flat on a table, top north: lens down" },
        { 0, 90, 0, { 0, 0, -1 }, { 0, 1, 0 }, "upright portrait facing north" },
        { 90, 90, 0, { -1, 0, 0 }, { 0, 1, 0 }, "upright, turned 90 deg left (alpha = 90: west)" },
        { 0, 90, 90, { -1, 0, 0 }, { 0, 1, 0 }, "gamma 90 from upright (right edge back): lens swings west" },
        { 0, 135, 0, { 0, 0.7071f, -0.7071f }, { 0, 0.7071f, 0.7071f }, "tilted back 45 deg: lens up" },
        { 0, 45, 0, { 0, -0.7071f, -0.7071f }, { 0, 0.7071f, -0.7071f }, "tilted forward 45 deg: lens down" },
    };
    for (int i = 0; i < ARRAY_COUNT(ref); i++) {
        quat q = phone_orient_game(phone_device_quat(ref[i].a, ref[i].b, ref[i].g));
        vec3 lens = quat_rotate(q, v3(0, 0, -1)), top = quat_rotate(q, v3(0, 1, 0));
        CHECK(vang_deg(lens, ref[i].lens) < 0.05f && vang_deg(top, ref[i].top) < 0.05f, "%s: lens %.2f %.2f %.2f top %.2f %.2f %.2f", ref[i].name, lens.x, lens.y, lens.z,
              top.x, top.y, top.z);
    }
    // Portrait upright with the screen rotated to landscape (gamma -90 about the screen normal is
    // a roll of the item): top points left / right, the lens stays forward.
    {
        quat q = phone_orient_game(quat_mul(phone_device_quat(0, 90, 0), quat_axis_angle(v3(0, 0, 1), 90.0f * DG_DEG2RAD)));
        CHECK(vang_deg(quat_rotate(q, v3(0, 0, -1)), v3(0, 0, -1)) < 0.05f && vang_deg(quat_rotate(q, v3(0, 1, 0)), v3(-1, 0, 0)) < 0.05f, "landscape roll");
    }
    // Calibration: phone held upright facing an arbitrary heading -> identity relative pose.
    for (int deg = -170; deg <= 180; deg += 35) {
        quat q = phone_orient_game(phone_device_quat((float)deg, 90, 0));
        float h = phone_heading(q);
        quat rel = phone_apply_calibration(q, h);
        CHECK(qangle_deg(rel, quat_identity()) < 0.05f, "calibrated upright pose is identity (alpha %d: %.3f deg)", deg, qangle_deg(rel, quat_identity()));
        // Pitch survives a yaw-only calibration: tilt the same phone back 30 deg.
        quat t = phone_apply_calibration(phone_orient_game(phone_device_quat((float)deg, 120, 0)), h);
        vec3 ypr = phone_euler_ypr(t);
        CHECK(fabsf(ypr.y * DG_RAD2DEG - 30.0f) < 0.05f && fabsf(ypr.x) < 1e-3f, "pitch 30 deg kept after calibration (%.3f)", ypr.y * DG_RAD2DEG);
    }
    // Heading of a flat phone comes from the screen top (no jump while tilting up through 45 deg).
    {
        float prev = phone_heading(phone_orient_game(phone_device_quat(30, 0, 0)));
        float maxjump = 0;
        for (float b = 0; b <= 90; b += 1) {
            float h = phone_heading(phone_orient_game(phone_device_quat(30, b, 0)));
            maxjump = fmaxf(maxjump, fabsf(wrap_angle(h - prev)));
            prev = h;
        }
        CHECK(maxjump < 0.002f, "heading continuous from flat to upright (%.4f rad)", maxjump);
        CHECK(fabsf(prev - 30.0f * DG_DEG2RAD) < 1e-3f, "heading = alpha (%.3f)", prev);
    }
    // Euler round trip, sensitivity / inversion.
    for (int i = 0; i < 200; i++) {
        Rng rg = rng_make((u64)i + 9);
        vec3 a = v3(rng_range(&rg, -3.0f, 3.0f), rng_range(&rg, -1.5f, 1.5f), rng_range(&rg, -3.0f, 3.0f));
        quat q = phone_from_ypr(a);
        CHECK(qangle_deg(phone_from_ypr(phone_euler_ypr(q)), q) < 0.02f, "ypr round trip");
    }
    quat yaw20 = phone_from_ypr(v3(20 * DG_DEG2RAD, 0, 0));
    CHECK(fabsf(phone_euler_ypr(phone_shape(yaw20, 1.5f, false, false, false)).x * DG_RAD2DEG - 30.0f) < 0.01f, "sensitivity scales angles");
    CHECK(fabsf(phone_euler_ypr(phone_shape(yaw20, 1.0f, true, false, false)).x * DG_RAD2DEG + 20.0f) < 0.01f, "invert yaw");
    // Camera space: looking down 30 deg with the phone level -> the phone pitches +30 vs the camera.
    vec3 c = phone_euler_ypr(phone_to_camera(quat_identity(), -30.0f * DG_DEG2RAD));
    CHECK(fabsf(c.y * DG_RAD2DEG - 30.0f) < 0.01f, "camera space removes the view pitch (%.3f)", c.y * DG_RAD2DEG);
    // Packing.
    float maxerr = 0;
    for (int i = 0; i < 1000; i++) {
        Rng rg = rng_make((u64)i * 77 + 1);
        quat q = quat_norm((quat){ rng_range(&rg, -1, 1), rng_range(&rg, -1, 1), rng_range(&rg, -1, 1), rng_range(&rg, -1, 1) });
        maxerr = fmaxf(maxerr, qangle_deg(q, phone_quat_unpack(phone_quat_pack(q))));
        i16 s[4];
        phone_quat_to_i16(q, s);
        maxerr = fmaxf(maxerr, qangle_deg(q, phone_quat_from_i16(s)) * 0.1f);
    }
    CHECK(maxerr < 0.25f, "32-bit packed rotation error %.3f deg", maxerr);
}

static void test_filter_and_reach(void) {
    // 1-euro: a noisy static pose settles close to the truth; a fast turn is followed with little lag.
    PhoneOneEuro f;
    phone_one_euro_init(&f, 1.5f, 0.4f, 1.0f);
    Rng rg = rng_make(5);
    quat truth = phone_from_ypr(v3(0.3f, 0.2f, 0.1f));
    float jitter_out = 0;
    for (int i = 0; i < 240; i++) {
        quat noisy = quat_mul(truth, quat_axis_angle(v3(rng_range(&rg, -1, 1), rng_range(&rg, -1, 1), rng_range(&rg, -1, 1)), 0.6f * DG_DEG2RAD));
        quat o = phone_one_euro(&f, noisy, 1.0f / 60.0f);
        if (i > 120) jitter_out = fmaxf(jitter_out, qangle_deg(o, truth));
    }
    CHECK(jitter_out < 0.4f, "0.6 deg noise filtered to %.3f deg (peak)", jitter_out);
    float lag = 0;
    for (int i = 0; i < 60; i++) {
        quat t = quat_mul(truth, quat_yaw((float)i * 3.0f * DG_DEG2RAD));   // 180 deg/s
        quat o = phone_one_euro(&f, t, 1.0f / 60.0f);
        if (i > 20) lag = fmaxf(lag, qangle_deg(o, t));
    }
    CHECK(lag < 6.0f, "fast turn lag %.2f deg (< 2 frames at 180 deg/s)", lag);
    // Reach cone: random poses (incl. absurd ones) always end inside, poses inside the knee are unchanged.
    PhoneReach r = phone_reach_default();
    int outside = 0;
    for (int i = 0; i < 5000; i++) {
        Rng g = rng_make((u64)i + 1000);
        quat q = quat_norm((quat){ rng_range(&g, -1, 1), rng_range(&g, -1, 1), rng_range(&g, -1, 1), rng_range(&g, -1, 1) });
        if (!phone_in_reach(phone_clamp_reach(q, &r), &r, 0.01f)) outside++;
    }
    CHECK(outside == 0, "clamped poses inside the reach cone (%d outside)", outside);
    quat in = phone_from_ypr(v3(20 * DG_DEG2RAD, -15 * DG_DEG2RAD, 30 * DG_DEG2RAD));
    CHECK(qangle_deg(phone_clamp_reach(in, &r), in) < 0.01f, "inside the knee: unchanged");
    // Continuity: sweeping past the limit never jumps.
    float prev = 0, maxstep = 0;
    for (int i = 0; i <= 179; i++) {   // 180 itself wraps to -180 in the input
        float y = phone_euler_ypr(phone_clamp_reach(phone_from_ypr(v3((float)i * DG_DEG2RAD, 0, 0)), &r)).x * DG_RAD2DEG;
        if (i) maxstep = fmaxf(maxstep, fabsf(y - prev));
        prev = y;
    }
    CHECK(maxstep <= 1.0001f && prev <= r.yaw, "clamp is continuous (%.3f) and bounded (%.2f)", maxstep, prev);
}

// ------------------------------------------------------------------ JPEG / photos / video

static u8 *test_image(int w, int h, int seed) {
    u8 *p = (u8 *)malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 *q = p + ((size_t)y * (size_t)w + (size_t)x) * 4;
            q[0] = (u8)((x + seed * 3) & 255);
            q[1] = (u8)((y * 2 + seed) & 255);
            q[2] = (u8)(((x / 40 + y / 40 + seed) & 1) * 200);
            q[3] = 255;
        }
    return p;
}

typedef struct EncJob { const u8 *src; int w, h; PhoneBuf out; double t0, t1; } EncJob;
static void enc_job(void *u) {
    EncJob *e = (EncJob *)u;
    e->t0 = now_s();
    phone_jpeg_encode(e->src, e->w, e->h, e->w * 4, e->w, e->h, 70, &e->out);
    e->t1 = now_s();
}

static void test_jpeg_pipeline(void) {
    int w = 640, h = 1136;
    u8 *img = test_image(w, h, 1);
    PhoneBuf b = { 0 };
    CHECK(phone_jpeg_encode(img, w, h, w * 4, w, h, 70, &b), "encode");
    int jw = 0, jh = 0;
    CHECK(phone_jpeg_info(b.data, b.len, &jw, &jh) && jw == w && jh == h, "JPEG header %dx%d", jw, jh);
    CHECK(phone_jpeg_encode(img, w, h, w * 4, 320, 568, 60, &b) && phone_jpeg_info(b.data, b.len, &jw, &jh) && jw == 320 && jh == 568, "downscaled %dx%d", jw, jh);
    phone_buf_free(&b);
    // 30 fps on the job system for 2 s: the "render thread" (this loop) only memcpys + submits.
    enum { N = 60, INFLIGHT = 3 };
    EncJob jobs[INFLIGHT];
    memset(jobs, 0, sizeof(jobs));
    JobCounter jc[INFLIGHT];
    memset(jc, 0, sizeof(jc));
    u8 *frames[INFLIGHT];
    for (int i = 0; i < INFLIGHT; i++) frames[i] = (u8 *)malloc((size_t)w * (size_t)h * 4);
    double t_start = now_s(), worst_submit = 0, latency_max = 0;
    int skipped = 0;
    for (int f = 0; f < N; f++) {
        double due = t_start + f / 30.0;
        while (now_s() < due) SDL_Delay(1);
        int s = f % INFLIGHT;
        if (jc[s].pending) { skipped++; continue; }   // slot still encoding: the sink would skip this frame
        if (jobs[s].t1 > 0) latency_max = fmax(latency_max, (jobs[s].t1 - jobs[s].t0) * 1000.0);
        double t0 = now_s();
        memcpy(frames[s], img, (size_t)w * (size_t)h * 4);
        jobs[s].src = frames[s];
        jobs[s].w = w;
        jobs[s].h = h;
        jobs_submit(enc_job, NULL, &jobs[s], &jc[s]);
        worst_submit = fmax(worst_submit, (now_s() - t0) * 1000.0);
    }
    for (int i = 0; i < INFLIGHT; i++) jobs_wait(&jc[i]);
    double total = now_s() - t_start;
    printf("  jpeg 640x1136 @30: %.2f s for %d frames, encode <= %.1f ms, submit <= %.2f ms, skipped %d\n", total, N, latency_max, worst_submit, skipped);
    CHECK(skipped == 0, "30 fps sustained (%d frames skipped)", skipped);
    CHECK(worst_submit < 5.0, "render thread cost per frame %.2f ms (copy + submit only)", worst_submit);
    // INFLIGHT slots: an encode may take up to INFLIGHT frame times without a skip (wall clock on a
    // shared CPU jitters; the skip count above is the real requirement).
    CHECK(latency_max < INFLIGHT * 1000.0 / 30.0, "encode %.1f ms fits the %d-slot pipeline", latency_max, INFLIGHT);
    for (int i = 0; i < INFLIGHT; i++) {
        phone_buf_free(&jobs[i].out);
        free(frames[i]);
    }
    free(img);
}

static bool file_bytes(const char *path, u8 **out, size_t *n) {
    *out = (u8 *)platform_read_file(path, n);
    return *out != NULL;
}

// Walks MP4 top-level boxes; returns true if ftyp, moov and mdat are present. *dur from mvhd.
static bool mp4_check(const u8 *d, size_t n, double *dur, int *w, int *h) {
    bool ftyp = false, moov = false, mdat = false;
    size_t p = 0;
    while (p + 8 <= n) {
        u32 sz = (u32)d[p] << 24 | (u32)d[p + 1] << 16 | (u32)d[p + 2] << 8 | d[p + 3];
        if (sz < 8 || p + sz > n) return false;
        if (!memcmp(d + p + 4, "ftyp", 4)) ftyp = true;
        if (!memcmp(d + p + 4, "mdat", 4)) mdat = true;
        if (!memcmp(d + p + 4, "moov", 4)) {
            moov = true;
            for (size_t q = p + 8; q + 32 < p + sz; q++) {
                if (!memcmp(d + q, "mvhd", 4)) {
                    const u8 *m = d + q + 4;   // version + flags, then (v0) ctime, mtime, timescale, duration
                    u32 ts = (u32)m[12] << 24 | (u32)m[13] << 16 | (u32)m[14] << 8 | m[15];
                    u32 du = (u32)m[16] << 24 | (u32)m[17] << 16 | (u32)m[18] << 8 | m[19];
                    if (ts) *dur = (double)du / ts;
                }
                if (!memcmp(d + q, "avc1", 4) && q + 36 < n) {
                    *w = d[q + 28] << 8 | d[q + 29];
                    *h = d[q + 30] << 8 | d[q + 31];
                }
            }
        }
        p += sz;
    }
    return ftyp && moov && mdat;
}

static void test_photo_video_files(const char *dir) {
    // Photo with the camera look + sidecar.
    int w = 640, h = 1136;
    u8 *img = test_image(w, h, 2);
    u8 before = img[(size_t)(10 * w + 10) * 4];
    phone_photo_look(img, w, h, 0.5f, 0.6f, PHONE_FILTER_MONO);
    u8 *corner = img + (size_t)(2 * w + 2) * 4;
    CHECK(corner[0] == corner[1] && corner[1] == corner[2], "mono filter");
    CHECK(img[(size_t)(10 * w + 10) * 4] <= before || before == 0, "vignette darkens corners");
    char stem[64], path[640], side[640];
    phone_media_stem(dir, ".jpg", 1792000000, stem, sizeof(stem));
    snprintf(path, sizeof(path), "%s%s.jpg", dir, stem);
    PhoneBuf jb = { 0 };
    phone_jpeg_encode(img, w, h, w * 4, w, h, 92, &jb);
    CHECK(platform_write_file(path, jb.data, (size_t)jb.len), "write %s", path);
    PhoneShotMeta m;
    memset(&m, 0, sizeof(m));
    dg_strcpy(m.kind, sizeof(m.kind), "photo");
    snprintf(m.file, sizeof(m.file), "%s.jpg", stem);
    m.width = w;
    m.height = h;
    m.zoom = 2.5f;
    m.position = v3(1.5f, 2.0f, -3.25f);
    m.day = 3;
    m.tod_hours = 14.25f;
    dg_strcpy(m.game_time, sizeof(m.game_time), "Day 3  14:15");
    dg_strcpy(m.weather, sizeof(m.weather), "light_rain");
    dg_strcpy(m.filter, sizeof(m.filter), "mono");
    char *js = phone_meta_to_json(&m);
    snprintf(side, sizeof(side), "%s%s.json", dir, stem);
    platform_write_file(side, js, strlen(js));
    free(js);
    char stem2[64];
    phone_media_stem(dir, ".jpg", 1792000000, stem2, sizeof(stem2));
    CHECK(strcmp(stem, stem2) != 0 && strncmp(stem, stem2, 19) == 0 && stem2[19] == '_', "unique names in the same second (%s / %s)", stem, stem2);
    u8 *bytes;
    size_t n;
    int jw = 0, jh = 0;
    CHECK(file_bytes(path, &bytes, &n) && phone_jpeg_info(bytes, (int)n, &jw, &jh) && jw == w && jh == h, "photo file is a valid %dx%d JPEG", jw, jh);
    free(bytes);
    char *back = platform_read_file(side, NULL);
    PhoneShotMeta m2;
    CHECK(back && phone_meta_from_json(back, &m2), "sidecar parses");
    CHECK(m2.width == w && m2.day == 3 && fabsf(m2.tod_hours - 14.25f) < 1e-4f && fabsf(m2.position.z + 3.25f) < 1e-4f && !strcmp(m2.weather, "light_rain") &&
              !strcmp(m2.file, m.file) && fabsf(m2.zoom - 2.5f) < 1e-4f, "sidecar matches the photo");
    free(back);
    phone_buf_free(&jb);
    free(img);

    // Video: 2 s at 30 fps of moving content through the recorder (job system).
    snprintf(path, sizeof(path), "%sclip.mp4", dir);
    char err[128] = "";
    PhoneRecorder *r = phone_recorder_start(path, w, h, 4000, err, sizeof(err));
    CHECK(r != NULL, "recorder start: %s", err);
    double t0 = now_s();
    int dropped_push = 0;
    for (int f = 0; f < 60 && r; f++) {
        u8 *fr = test_image(w, h, f * 4);
        while (now_s() < t0 + f / 30.0) SDL_Delay(1);
        if (!phone_recorder_push(r, fr, w, h, (u32)(f * 1000 / 30))) dropped_push++;
        free(fr);
    }
    double secs = 0;
    int frames = 0, dropped = 0;
    CHECK(r && phone_recorder_stop(r, 2000, &secs, &frames, &dropped), "recorder stop");
    printf("  video: %d frames, %.2f s, dropped %d\n", frames, secs, dropped);
    CHECK(frames + dropped == 60 && frames >= 50, "frames recorded (%d, %d dropped)", frames, dropped);
    CHECK(fabs(secs - 2.0) < 0.1, "clip length %.2f s", secs);
    double dur = 0;
    int vw = 0, vh = 0;
    CHECK(file_bytes(path, &bytes, &n) && mp4_check(bytes, n, &dur, &vw, &vh), "MP4 has ftyp / moov / mdat");
    CHECK(fabs(dur - 2.0) < 0.15 && vw == 640 && vh == 1136, "MP4 duration %.2f s, %dx%d", dur, vw, vh);
    free(bytes);
    // Gallery scan finds the photo (sorted newest first).
    char base[512];
    snprintf(base, sizeof(base), "%s", dir);
}

// ------------------------------------------------------------------ headless client

typedef struct Client { tsock fd; u8 buf[1 << 20]; int len, last_len; } Client;

static tsock tcp_connect(u16 port) {
    tsock fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7F000001u);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { tclose(fd); return TSOCK_BAD; }
#if defined(_WIN32)
    DWORD tv = 3000;
#else
    struct timeval tv = { 3, 0 };
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    return fd;
}

static int cl_recv(Client *c) {
    int n = (int)recv(c->fd, (char *)c->buf + c->len, (int)sizeof(c->buf) - c->len, 0);
    if (n > 0) c->len += n;
    return n;
}

// HTTP GET; returns the status code, body copied into body (malloc).
static int http_get(u16 port, const char *path, u8 **body, int *body_len) {
    tsock fd = tcp_connect(port);
    if (fd == TSOCK_BAD) return -1;
    char req[512];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", path);
    send(fd, req, n, 0);
    Client *c = (Client *)calloc(1, sizeof(Client));
    c->fd = fd;
    while (cl_recv(c) > 0) {}
    tclose(fd);
    int code = -1;
    if (c->len > 12) code = atoi((const char *)c->buf + 9);
    const char *hdr_end = NULL;
    for (int i = 3; i < c->len; i++)
        if (!memcmp(c->buf + i - 3, "\r\n\r\n", 4)) { hdr_end = (const char *)c->buf + i + 1; break; }
    if (body && hdr_end) {
        *body_len = c->len - (int)(hdr_end - (const char *)c->buf);
        *body = (u8 *)malloc((size_t)*body_len + 1);
        memcpy(*body, hdr_end, (size_t)*body_len);
        (*body)[*body_len] = 0;
    }
    free(c);
    return code;
}

static int ws_open(Client *c, u16 port, const char *query) {
    c->fd = tcp_connect(port);
    c->len = c->last_len = 0;
    if (c->fd == TSOCK_BAD) return -1;
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET /ws?%s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     query);
    send(c->fd, req, n, 0);
    for (int tries = 0; tries < 50; tries++) {
        for (int i = 3; i < c->len; i++)
            if (!memcmp(c->buf + i - 3, "\r\n\r\n", 4)) {
                int code = atoi((const char *)c->buf + 9);
                memmove(c->buf, c->buf + i + 1, (size_t)(c->len - i - 1));
                c->len -= i + 1;
                return code;
            }
        if (cl_recv(c) <= 0) break;
    }
    return -1;
}

// Drops the frame returned by the previous ws_next.
static void ws_consume(Client *c) {
    if (!c->last_len) return;
    memmove(c->buf, c->buf + c->last_len, (size_t)(c->len - c->last_len));
    c->len -= c->last_len;
    c->last_len = 0;
}

// Next server frame (unmasked). Returns the opcode, payload in *p / *n (valid until the next call).
static int ws_next(Client *c, u8 **p, int *n) {
    ws_consume(c);
    for (int tries = 0; tries < 200; tries++) {
        if (c->len >= 2) {
            u64 len = c->buf[1] & 127;
            int hl = 2;
            if (len == 126) { hl = 4; len = c->len >= 4 ? (u64)c->buf[2] << 8 | c->buf[3] : 0; }
            else if (len == 127) {
                hl = 10;
                len = 0;
                if (c->len >= 10)
                    for (int i = 0; i < 8; i++) len = len << 8 | c->buf[2 + i];
            }
            if (c->len >= hl && (u64)c->len >= (u64)hl + len) {
                *p = c->buf + hl;
                *n = (int)len;
                c->last_len = hl + (int)len;
                return c->buf[0] & 15;
            }
        }
        if (cl_recv(c) <= 0) return -1;
    }
    return -1;
}

// Non-blocking variant: -2 when no complete frame is available within ~1 ms.
static int ws_try_next(Client *c, u8 **p, int *n) {
    ws_consume(c);
    for (int pass = 0; pass < 2; pass++) {
        if (c->len >= 2) {
            u64 len = c->buf[1] & 127;
            int hl = len == 126 ? 4 : (len == 127 ? 10 : 2);
            if (c->len >= hl) {
                if (hl == 4) len = (u64)c->buf[2] << 8 | c->buf[3];
                if (hl == 10) {
                    len = 0;
                    for (int i = 0; i < 8; i++) len = len << 8 | c->buf[2 + i];
                }
                if ((u64)c->len >= (u64)hl + len) return ws_next(c, p, n);
            }
        }
        if (pass) break;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(c->fd, &rd);
        struct timeval tv = { 0, 1000 };
        if (select((int)c->fd + 1, &rd, NULL, NULL, &tv) <= 0) return -2;
        if (cl_recv(c) <= 0) return -1;
    }
    return -2;
}

static void ws_send_text(Client *c, const char *s) {
    u8 f[1024];
    int n = hws_ws_client_frame(f, sizeof(f), HWS_OP_TEXT, s, (int)strlen(s), 0xA1B2C3D4u);
    send(c->fd, (const char *)f, n, 0);
}

static void ws_send_bin(Client *c, const u8 *d, int len) {
    u8 f[256];
    int n = hws_ws_client_frame(f, sizeof(f), HWS_OP_BINARY, d, len, 0x01020304u);
    send(c->fd, (const char *)f, n, 0);
}

// Waits for the next server event of a type (skipping others); returns true if seen.
static bool wait_event(HwsServer *s, int type, HwsEvent *ev, double timeout_s) {
    double t = now_s() + timeout_s;
    while (now_s() < t) {
        while (hws_poll(s, ev)) {
            if (ev->type == type) return true;
            hws_event_free(ev);
        }
        SDL_Delay(1);
    }
    return false;
}

static void test_server_roundtrip(const char *dir) {
    const char page[] = "<!doctype html><title>phone</title>";
    HwsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 47890;
    cfg.bind_ip = 0x7F000001u;
    cfg.lan_ip = 0x7F000001u;
    cfg.page = page;
    cfg.page_len = sizeof(page) - 1;
    char err[256] = "";
    HwsServer *s = hws_start(&cfg, err, sizeof(err));
    CHECK(s != NULL, "server start: %s", err);
    if (!s) return;
    u8 *body = NULL;
    int bl = 0;
    CHECK(http_get(cfg.port, "/", &body, &bl) == 200 && bl == (int)sizeof(page) - 1 && !memcmp(body, page, (size_t)bl), "page served");
    free(body);
    CHECK(http_get(cfg.port, "/../../etc/passwd", NULL, NULL) == 404, "no file system access");
    CHECK(http_get(cfg.port, "/media/1/x.jpg?s=bogus", NULL, NULL) == 403, "media needs a session");
    Client *c = (Client *)calloc(1, sizeof(Client));
    CHECK(ws_open(c, cfg.port, "t=wrongwrongwrongwrong12") == 403, "WebSocket with a wrong token rejected");
    tclose(c->fd);
    // Pair.
    char tok[HWS_KEY_CHARS + 1];
    CHECK(hws_issue_token(s, 0, tok), "issue token");
    char url[256];
    hws_url(s, tok, url, sizeof(url));
    CHECK(strstr(url, "http://127.0.0.1:47890/?t=") == url, "url %s", url);
    char q[64];
    snprintf(q, sizeof(q), "t=%s", tok);
    CHECK(ws_open(c, cfg.port, q) == 101, "pair with the token");
    u8 *p;
    int n;
    char session[64] = "";
    if (ws_next(c, &p, &n) == HWS_OP_TEXT) {
        cJSON *j = cJSON_ParseWithLength((const char *)p, (size_t)n);
        const cJSON *sv = cJSON_GetObjectItem(j, "s");
        if (cJSON_IsString(sv)) dg_strcpy(session, sizeof(session), sv->valuestring);
        cJSON_Delete(j);
    }
    CHECK(strlen(session) == HWS_KEY_CHARS, "session key received");
    HwsEvent ev;
    CHECK(wait_event(s, HWS_EV_CONNECT, &ev, 2.0) && ev.owner == 0 && !ev.resumed, "connect event");
    int conn = ev.conn;
    Client *c2 = (Client *)calloc(1, sizeof(Client));
    CHECK(ws_open(c2, cfg.port, q) == 403, "the token can't be used twice");
    tclose(c2->fd);
    // Orientation sample page -> game.
    PhoneOrientMsg om = { 0, 7, 1000, { 0, 0, 0, 32767 } };
    u8 ob[PHONE_ORIENT_BYTES];
    phone_write_orient(ob, &om);
    ws_send_bin(c, ob, PHONE_ORIENT_BYTES);
    PhoneOrientMsg om2;
    CHECK(wait_event(s, HWS_EV_BINARY, &ev, 2.0) && phone_parse_orient(ev.data, ev.len, &om2) && om2.seq == 7, "orientation sample delivered");
    hws_event_free(&ev);
    // Stream 100 frames (320 x 568 to keep the test quick) through the JPEG sink; the client acks.
    PhoneStreamSink *sink = phone_stream_ws_create(s, conn, 30.0f, 70);
    u8 *img = test_image(320, 568, 3);
    int received = 0, bad = 0;
    u32 last_id = 0;
    double t0 = now_s();
    for (int f = 1; received < 100 && now_s() - t0 < 20.0; f++) {
        PhoneFrameInfo fi = { (u32)f, (u32)SDL_GetTicks(), 1.0f, 0 };
        sink->push(sink, img, 320, 568, &fi);
        sink->update(sink, (u32)SDL_GetTicks());
        // Drain what arrived.
        for (;;) {
            int op = ws_try_next(c, &p, &n);
            if (op < 0) break;
            if (op == HWS_OP_PING) continue;
            if (op != HWS_OP_BINARY) continue;
            int jw, jh;
            if (n > PHONE_FRAME_HEADER && p[0] == PHONE_MSG_FRAME && phone_jpeg_info(p + PHONE_FRAME_HEADER, n - PHONE_FRAME_HEADER, &jw, &jh)) {
                received++;
                last_id = phone_get_u32(p + 8);
                char ack[96];
                snprintf(ack, sizeof(ack), "{\"c\":\"ack\",\"f\":%u}", last_id);
                ws_send_text(c, ack);
            } else bad++;
        }
        while (hws_poll(s, &ev)) {
            if (ev.type == HWS_EV_TEXT) {
                cJSON *j = cJSON_Parse((const char *)ev.data);
                const cJSON *fv = cJSON_GetObjectItem(j, "f");
                if (cJSON_IsNumber(fv)) sink->on_ack(sink, (u32)fv->valuedouble, (u32)SDL_GetTicks());
                cJSON_Delete(j);
            }
            hws_event_free(&ev);
        }
        SDL_Delay(5);
    }
    PhoneStreamStats st;
    sink->stats(sink, &st);
    printf("  stream: %d frames received in %.2f s, rtt %.1f ms, encode %.1f ms, quality %d\n", received, now_s() - t0, st.rtt_ms, st.encode_ms, st.quality);
    CHECK(received >= 100 && bad == 0, "100 frames streamed and decodable (%d, %d bad)", received, bad);
    sink->destroy(sink);
    // Photo: the game registers the file; the client downloads it with its session.
    char photo[640];
    snprintf(photo, sizeof(photo), "%sroundtrip.jpg", dir);
    PhoneBuf jb = { 0 };
    phone_jpeg_encode(img, 320, 568, 320 * 4, 320, 568, 90, &jb);
    platform_write_file(photo, jb.data, (size_t)jb.len);
    int mid = hws_media_add(s, 0, photo, "roundtrip.jpg");
    char path[256];
    snprintf(path, sizeof(path), "/media/%d/roundtrip.jpg?s=%s&dl=1", mid, session);
    int jw = 0, jh = 0;
    CHECK(http_get(cfg.port, path, &body, &bl) == 200 && bl == jb.len && phone_jpeg_info(body, bl, &jw, &jh) && jw == 320, "photo downloaded (%d bytes)", bl);
    free(body);
    snprintf(path, sizeof(path), "/media/%d/roundtrip.jpg?s=%s", mid + 1, session);
    CHECK(http_get(cfg.port, path, NULL, NULL) == 404, "unknown media id");
    phone_buf_free(&jb);
    // Video 2 s, then download.
    char clip[640];
    snprintf(clip, sizeof(clip), "%sroundtrip.mp4", dir);
    PhoneRecorder *r = phone_recorder_start(clip, 320, 568, 2000, err, sizeof(err));
    for (int f = 0; f < 60 && r; f++) {
        u8 *fr = test_image(320, 568, f);
        phone_recorder_push(r, fr, 320, 568, (u32)(f * 1000 / 30));
        free(fr);
        SDL_Delay(2);
    }
    double secs = 0;
    int frames = 0, dropped = 0;
    CHECK(r && phone_recorder_stop(r, 2000, &secs, &frames, &dropped) && fabs(secs - 2.0) < 0.1, "2 s video (%.2f s)", secs);
    mid = hws_media_add(s, 0, clip, "roundtrip.mp4");
    snprintf(path, sizeof(path), "/media/%d/roundtrip.mp4?s=%s", mid, session);
    double dur = 0;
    int vw = 0, vh = 0;
    CHECK(http_get(cfg.port, path, &body, &bl) == 200 && mp4_check(body, (size_t)bl, &dur, &vw, &vh) && vw == 320 && vh == 560, "video downloaded, valid MP4 (%dx%d)", vw,
          vh);
    free(body);
    // Resume with the session (page reload), then unpair.
    tclose(c->fd);
    CHECK(wait_event(s, HWS_EV_DISCONNECT, &ev, 3.0), "disconnect event on close");
    char sq[64];
    snprintf(sq, sizeof(sq), "s=%s", session);
    CHECK(ws_open(c, cfg.port, sq) == 101, "resume with the session key");
    CHECK(wait_event(s, HWS_EV_CONNECT, &ev, 2.0) && ev.resumed, "resumed connect event");
    hws_unpair(s, 0);
    bool saw_unpaired = false, closed = false;
    for (int i = 0; i < 6; i++) {
        int op = ws_next(c, &p, &n);
        if (op == HWS_OP_TEXT && strstr((const char *)p, "unpaired")) saw_unpaired = true;
        if (op == HWS_OP_CLOSE || op < 0) { closed = true; break; }
    }
    CHECK(saw_unpaired && closed, "unpair notifies and closes the page (%d %d)", saw_unpaired, closed);
    tclose(c->fd);
    CHECK(ws_open(c, cfg.port, sq) == 403, "the session is gone after unpair");
    tclose(c->fd);
    HwsStats hs = hws_stats(s);
    CHECK(hs.auth_failures >= 3, "auth failures counted (%u)", hs.auth_failures);
    free(img);
    free(c);
    free(c2);
    hws_stop(s);
}

// ------------------------------------------------------------------ TLS

typedef struct TlsSock { tsock fd; } TlsSock;
static int tls_send(void *ctx, const unsigned char *b, size_t n) { return (int)send(((TlsSock *)ctx)->fd, (const char *)b, (int)n, 0); }
static int tls_recv(void *ctx, unsigned char *b, size_t n) {
    int r = (int)recv(((TlsSock *)ctx)->fd, (char *)b, (int)n, 0);
    return r < 0 ? MBEDTLS_ERR_SSL_TIMEOUT : r;
}

static void test_tls(const char *dir) {
    char certs[512];
    snprintf(certs, sizeof(certs), "%scerts/", dir);
    const char page[] = "secure page";
    HwsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 47892;
    cfg.http_port = 47893;
    cfg.tls = true;
    cfg.bind_ip = 0x7F000001u;
    cfg.lan_ip = 0x7F000001u;
    cfg.cert_dir = certs;
    cfg.page = page;
    cfg.page_len = sizeof(page) - 1;
    char err[256] = "";
    double t0 = now_s();
    HwsServer *s = hws_start(&cfg, err, sizeof(err));
    CHECK(s != NULL, "TLS server start: %s", err);
    if (!s) return;
    printf("  tls: certificates ready in %.0f ms, CA %s\n", (now_s() - t0) * 1000.0, hws_ca_fingerprint(s));
    // Client: trusts only our CA, verifies the IP SAN.
    u8 *ca_der = NULL;
    int ca_len = 0;
    CHECK(http_get(cfg.http_port, "/ca.crt", &ca_der, &ca_len) == 200 && ca_len > 100, "CA served over the plain HTTP helper (%d bytes)", ca_len);
    CHECK(http_get(cfg.http_port, "/?t=x", NULL, NULL) == 302, "helper redirects to HTTPS");
    mbedtls_x509_crt ca;
    mbedtls_x509_crt_init(&ca);
    CHECK(ca_der && mbedtls_x509_crt_parse_der(&ca, ca_der, (size_t)ca_len) == 0 && ca.MBEDTLS_PRIVATE(ca_istrue), "CA parses and is a CA");
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, NULL, 0);
    mbedtls_ssl_config conf;
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_setup(&ssl, &conf);
    mbedtls_ssl_set_hostname(&ssl, "127.0.0.1");   // matched against the IP SAN
    TlsSock ts = { tcp_connect(cfg.port) };
    mbedtls_ssl_set_bio(&ssl, &ts, tls_send, tls_recv, NULL);
    int rc;
    while ((rc = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    CHECK(rc == 0, "TLS handshake with certificate verification (-0x%04x, verify flags %x)", (unsigned)-rc, mbedtls_ssl_get_verify_result(&ssl));
    if (rc == 0) {
        const char req[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        mbedtls_ssl_write(&ssl, (const u8 *)req, sizeof(req) - 1);
        char resp[2048];
        int got = 0;
        for (int i = 0; i < 20 && got < (int)sizeof(resp) - 1; i++) {
            int r2 = mbedtls_ssl_read(&ssl, (u8 *)resp + got, sizeof(resp) - 1 - (size_t)got);
            if (r2 <= 0) break;
            got += r2;
        }
        resp[got] = 0;
        CHECK(strstr(resp, "200 OK") && strstr(resp, "secure page"), "page over HTTPS");
    }
    mbedtls_ssl_free(&ssl);
    tclose(ts.fd);
    hws_stop(s);
    // Second start reuses the stored CA (trust survives restarts) and the server certificate.
    char fp1[100];
    {
        HwsServer *s2 = hws_start(&cfg, err, sizeof(err));
        CHECK(s2 != NULL, "restart");
        dg_strcpy(fp1, sizeof(fp1), hws_ca_fingerprint(s2));
        hws_stop(s2);
    }
    u8 *der2 = NULL;
    (void)der2;
    mbedtls_x509_crt_free(&ca);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    free(ca_der);
    // A new IP gets a new server certificate signed by the same CA.
    cfg.lan_ip = 0xC0A80163u;   // 192.168.1.99 (certificate only; still bound to loopback)
    HwsServer *s3 = hws_start(&cfg, err, sizeof(err));
    CHECK(s3 && !strcmp(fp1, hws_ca_fingerprint(s3)), "same CA for a new LAN IP");
    char leaf[600];
    snprintf(leaf, sizeof(leaf), "%sserver_192.168.1.99.crt", certs);
    CHECK(platform_file_exists(leaf), "server certificate issued for the new IP");
    hws_stop(s3);
}

// ------------------------------------------------------------------ phone OS (flow, layout, assets)

static char *read_asset(const char *rel) {
    char p[512];
    platform_data_path(rel, p, sizeof(p));
    size_t n;
    return platform_read_file(p, &n);
}

static void test_phone_os_flow(void) {
    PhoneOs os;
    phone_os_init(&os);
    CHECK(os.flow == PFLOW_IDLE && os.app == PAPP_OFF && !phone_os_panel(&os), "idle: no panel");
    // Equip: pairing at once (the caller starts the server + issues a code), the portal, the panel.
    CHECK(phone_os_equip(&os, true, false), "equip asks for a pairing code");
    CHECK(os.flow == PFLOW_PAIRING && os.app == PAPP_PORTAL && phone_os_panel(&os), "equip -> pairing, portal, panel");
    CHECK(!phone_os_ui_only(PAPP_CAMERA) && phone_os_ui_only(PAPP_PORTAL) && phone_os_ui_only(PAPP_HOME), "only the camera renders the lens");
    phone_os_open(&os, PAPP_CAMERA);
    CHECK(os.app == PAPP_PORTAL, "apps do not open over the portal");
    // Timeout -> the in-game phone (home).
    for (int i = 0; i < 59; i++) phone_os_tick(&os, 1.0f, 60.0f, 0.0f);
    CHECK(os.flow == PFLOW_PAIRING, "59 s: still pairing");
    phone_os_tick(&os, 1.0f, 60.0f, 0.0f);
    CHECK(os.flow == PFLOW_LOCAL && os.app == PAPP_HOME && phone_os_panel(&os) && os.reason[0], "60 s: in-game phone (%s)", os.reason);
    // Session memory: a declined / timed-out phone skips the portal on re-equip.
    phone_os_open(&os, PAPP_GALLERY);
    phone_os_unequip(&os);
    CHECK(os.app == PAPP_OFF && !phone_os_panel(&os), "unequipped: off, no panel");
    CHECK(!phone_os_equip(&os, true, false) && os.app == PAPP_GALLERY, "re-equip: no portal, the last app");
    // Back: viewer -> grid -> home -> nothing.
    os.viewer = 2;
    CHECK(phone_os_back(&os) && os.viewer == -1 && os.app == PAPP_GALLERY, "back: viewer -> grid");
    CHECK(phone_os_back(&os) && os.app == PAPP_HOME, "back: grid -> home");
    CHECK(!phone_os_back(&os), "back on home: nothing");
    // Connect portal from the home screen ("Connect your phone"), then a device.
    CHECK(phone_os_reconnect(&os) && os.flow == PFLOW_PAIRING && os.app == PAPP_PORTAL, "connect: the portal again");
    phone_os_connected(&os);
    CHECK(os.flow == PFLOW_PAIRED && os.app == PAPP_HOME && !phone_os_panel(&os), "device connected: paired, home, panel removed");
    phone_os_open(&os, PAPP_CAMERA);
    phone_os_disconnected(&os);
    CHECK(os.flow == PFLOW_LOST && os.app == PAPP_PORTAL && phone_os_panel(&os), "device lost: the portal (Reconnect), panel back");
    PhoneLayoutIn li = { os.app, os.flow, -1, 0, 2, 4, 0 };
    PhoneHit h[PHONE_MAX_HITS];
    int n = phone_os_layout(&li, h, PHONE_MAX_HITS);
    CHECK(phone_os_find(h, n, PB_RECONNECT, 0) && phone_os_find(h, n, PB_DECLINE, 0) && phone_os_find(h, n, PB_CANCEL, 0), "lost portal: Reconnect, decline, cancel");
    phone_os_connected(&os);
    CHECK(os.app == PAPP_CAMERA, "reconnected: back to the app it was on");
    // Paired + unequip + re-equip with the device still there: no portal.
    phone_os_unequip(&os);
    CHECK(!phone_os_equip(&os, true, true) && os.flow == PFLOW_PAIRED && os.app == PAPP_CAMERA, "re-equip paired: skips the portal");
    // The device left while the phone was down: the portal on the next equip.
    phone_os_unequip(&os);
    CHECK(!phone_os_equip(&os, true, false) && os.flow == PFLOW_LOST && os.app == PAPP_PORTAL, "re-equip after the device left: portal");
    // Decline from the portal; cancel; no server; auto-pair off.
    phone_os_decline(&os);
    CHECK(os.flow == PFLOW_LOCAL && os.app == PAPP_CAMERA && !os.reason[0], "decline: in-game phone, no reason");
    PhoneOs b;
    phone_os_init(&b);
    phone_os_equip(&b, true, false);
    phone_os_cancel(&b);
    CHECK(b.flow == PFLOW_IDLE, "cancel: pairing stops");
    phone_os_unequip(&b);
    CHECK(phone_os_equip(&b, true, false) && b.flow == PFLOW_PAIRING, "after cancel: equip pairs again");
    phone_os_server_failed(&b, "port in use");
    CHECK(b.flow == PFLOW_LOCAL && b.app == PAPP_HOME && !strcmp(b.reason, "port in use"), "no server: in-game phone at once");
    phone_os_init(&b);
    CHECK(!phone_os_equip(&b, false, false) && b.flow == PFLOW_LOCAL && b.app == PAPP_HOME, "auto-pair off: straight to the in-game phone");
    // Battery: cosmetic drain while equipped, never below 5 %.
    float b0 = b.battery;
    phone_os_tick(&b, 600.0f, 0.0f, 0.005f / 60.0f);
    CHECK(b.battery < b0 && b.battery >= 0.05f, "battery drains while equipped (%.3f -> %.3f)", b0, b.battery);
    CHECK(phone_app_from_name("gallery") == PAPP_GALLERY && !strcmp(phone_app_name(PAPP_PORTAL), "portal") && phone_app_from_name("x") < 0, "app names");
}

static void test_phone_os_layout(void) {
    // Every app's buttons stay on the 640 x 1136 screen, do not overlap (except the home bar strip
    // under full-height areas) and hit-test back to themselves.
    static const PhoneApp apps[] = { PAPP_PORTAL, PAPP_HOME, PAPP_CAMERA, PAPP_GALLERY, PAPP_GALLERY };
    for (int a = 0; a < 5; a++) {
        for (int f = PFLOW_PAIRING; f <= PFLOW_LOST; f++) {
            PhoneLayoutIn li = { apps[a], (PhoneFlow)f, a == 4 ? 1 : -1, 0, 2, 4, 14 };
            PhoneHit h[PHONE_MAX_HITS];
            int n = phone_os_layout(&li, h, PHONE_MAX_HITS);
            CHECK(n > 0, "%s: buttons", phone_app_name(apps[a]));
            for (int i = 0; i < n; i++) {
                const PhoneRect *r = &h[i].r;
                CHECK(r->x >= 0 && r->y >= 0 && r->x + r->w <= PHONE_DESIGN_W + 0.01f && r->y + r->h <= PHONE_DESIGN_H + 0.01f, "%s: button %d on screen", phone_app_name(apps[a]), h[i].id);
                int k = phone_os_hit(h, n, r->x + r->w * 0.5f, r->y + r->h * 0.5f);
                CHECK(k == i, "%s: button %d (%d) hit at its centre (got %d)", phone_app_name(apps[a]), h[i].id, h[i].index, k);
            }
        }
    }
    // Home grid: one cell per app in apps.json order; the gallery grid pages with the scroll.
    PhoneLayoutIn li = { PAPP_HOME, PFLOW_LOCAL, -1, 0, 2, 4, 0 };
    PhoneHit h[PHONE_MAX_HITS];
    int n = phone_os_layout(&li, h, PHONE_MAX_HITS);
    CHECK(phone_os_find(h, n, PB_APP, 0) && phone_os_find(h, n, PB_APP, 1) && !phone_os_find(h, n, PB_APP, 2), "home: exactly the two apps");
    CHECK(phone_os_find(h, n, PB_CONNECT, 0) != NULL, "home (in-game phone): Connect your phone");
    li.flow = PFLOW_PAIRED;
    n = phone_os_layout(&li, h, PHONE_MAX_HITS);
    CHECK(!phone_os_find(h, n, PB_CONNECT, 0), "home (paired): no Connect button");
    PhoneLayoutIn g = { PAPP_GALLERY, PFLOW_LOCAL, -1, 0, 2, 4, 40 };
    n = phone_os_layout(&g, h, PHONE_MAX_HITS);
    const PhoneHit *c0 = phone_os_find(h, n, PB_CELL, 0);
    g.scroll = 2;
    n = phone_os_layout(&g, h, PHONE_MAX_HITS);
    const PhoneHit *c6 = phone_os_find(h, n, PB_CELL, 6);
    CHECK(c0 && c6 && !phone_os_find(h, n, PB_CELL, 0), "gallery: scrolling by two rows starts at item 6");
    // Zoom slider: log scale, the knob maps back to the zoom.
    PhoneRect s = { 0, 100, 40, 500 };
    for (float z = 1.0f; z <= 8.0f; z *= 1.3f) {
        float y = phone_slider_from_zoom(z, s, 8.0f);
        CHECK(fabsf(phone_zoom_from_slider(y, s, 8.0f) - z) < 1e-3f, "slider round trip at %.2fx", z);
    }
    CHECK(fabsf(phone_zoom_from_slider(100, s, 8) - 8) < 1e-4f && fabsf(phone_zoom_from_slider(600, s, 8) - 1) < 1e-4f, "slider ends: 8x top, 1x bottom");
}

static void test_phone_os_assets(void) {
    char err[160] = "";
    char *js = read_asset("assets/phone/apps.json");
    CHECK(js != NULL, "assets/phone/apps.json present");
    PhoneApps a;
    CHECK(js && phone_apps_parse(js, &a, err, sizeof(err)), "apps.json parses (%s)", err);
    CHECK(a.count == 2 && !strcmp(a.apps[0].id, "camera") && a.apps[0].app == PAPP_CAMERA && !strcmp(a.apps[1].id, "gallery") && a.apps[1].app == PAPP_GALLERY,
          "apps.json: exactly Camera and Gallery");
    CHECK(a.apps[0].tile >= 0 && a.apps[1].tile >= 0 && a.tile_px == 128, "app icons are atlas tiles");
    CHECK(phone_apps_weather_tile(&a, "rain", false) == phone_apps_tile(&a, "w_rain") && phone_apps_weather_tile(&a, "clear", true) == phone_apps_tile(&a, "w_night") &&
          phone_apps_weather_tile(&a, "nope", false) == phone_apps_tile(&a, "w_partly"), "weather glyphs (clear at night = moon)");
    // Every weather preset has a glyph.
    static const char *wx[] = { "clear", "little_cloudy", "cloudy", "overcast", "fog", "light_rain", "rain", "storm", "light_snow", "snow", "blizzard" };
    for (size_t i = 0; i < sizeof(wx) / sizeof(wx[0]); i++) {
        bool found = false;
        for (int k = 0; k < a.wx_count; k++) found |= !strcmp(a.wx[k].weather, wx[i]);
        CHECK(found, "weather_icons: %s", wx[i]);
    }
    // An app with no built-in app (future: map, messages) parses as "coming soon".
    PhoneApps b;
    CHECK(phone_apps_parse("{\"apps\":[{\"id\":\"map\",\"label\":\"Map\",\"icon\":\"map\"}]}", &b, err, sizeof(err)) && b.count == 1 && b.apps[0].app < 0 && b.apps[0].tile < 0,
          "data-driven: an extra app without a built-in");
    CHECK(!phone_apps_parse("{\"apps\":[]}", &b, err, sizeof(err)), "no apps: an error");
    free(js);
    // Icon atlas: one row of 128 px tiles, as many as apps.json lists.
    char p[512];
    platform_data_path("assets/phone/icons.png", p, sizeof(p));
    size_t pn;
    u8 *png = (u8 *)platform_read_file(p, &pn);
    CHECK(png && pn > 24 && !memcmp(png + 1, "PNG", 3), "assets/phone/icons.png present");
    if (png) {
        u32 w = (u32)png[16] << 24 | (u32)png[17] << 16 | (u32)png[18] << 8 | png[19], h = (u32)png[20] << 24 | (u32)png[21] << 16 | (u32)png[22] << 8 | png[23];
        CHECK(h == 128 && w == 128u * (u32)a.tile_count, "atlas %ux%u for %d tiles", w, h, a.tile_count);
    }
    // Theme: light (white background, dark text), every key present.
    js = read_asset("assets/phone/theme.json");
    PhoneTheme t;
    CHECK(js && phone_theme_parse(js, &t, err, sizeof(err)), "theme.json parses");
    CHECK(t.bg == 0xFFFFFFFFu && (t.text & 0xFF) < 60, "light theme: white screen, dark text");
    CHECK(t.panel.readout_gap > 0 && t.panel.radius > 0 && t.panel.bezel > 0 && t.panel.shadow > 0 && (t.bezel_color >> 24) > 0, "panel tokens");
    // Page injection: the marker replaced, "</" escaped, the atlas as a data URI.
    const char *page = "<script>/*DG_PHONE_DATA*/</script><p>x</p>";
    size_t on = 0;
    char *o = phone_page_build(page, strlen(page), js, "{\"a\":\"</script>\"}", png, pn, &on);
    CHECK(o && strstr(o, "window.DG_THEME={") && strstr(o, "window.DG_APPS={\"a\":\"<\\/script>\"}") && strstr(o, "window.DG_ICONS=\"data:image/png;base64,iVBORw0KGgo") &&
          !strstr(o, "/*DG_PHONE_DATA*/") && on == strlen(o) && !strcmp(o + on - 17, "</script><p>x</p>"), "page: assets injected");
    free(o);
    o = phone_page_build(page, strlen(page), NULL, NULL, NULL, 0, &on);
    CHECK(o && strstr(o, "window.DG_THEME=null;window.DG_APPS=null;window.DG_ICONS=null;"), "page: missing assets -> null");
    free(o);
    free(js);
    free(png);
}

static bool overlaps(PhoneRect a, PhoneRect b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

static void test_phone_panel_layout(void) {
    // The HUD panel at 16:9, 21:9 and 4:3: from the theme tokens and the HUD safe area only,
    // portrait at the screen aspect, ~22 % of the height, inside the safe area, never over the
    // item / ammo readout (bottom right, readout_gap high, ~240 design px wide).
    PhoneTheme t;
    phone_theme_default(&t);
    char *js = read_asset("assets/phone/theme.json");
    char err[64];
    if (js) phone_theme_parse(js, &t, err, sizeof(err));
    free(js);
    static const int res[][2] = { { 1920, 1080 }, { 2560, 1080 }, { 1440, 1080 }, { 3440, 1440 }, { 1280, 720 }, { 1024, 768 } };
    float aspect = 640.0f / 1136.0f;
    for (size_t i = 0; i < sizeof(res) / sizeof(res[0]); i++) {
        float W = (float)res[i][0], H = (float)res[i][1], px = H / 1080.0f;
        float m = 0.035f * H;   // safe_frac (assets/ui/theme.json)
        PhoneRect safe = { m, m, W - 2 * m, H - 2 * m };
        PhoneRect readout = { safe.x + safe.w - 240 * px, safe.y + safe.h - t.panel.readout_gap * px, 240 * px, t.panel.readout_gap * px };
        for (int corner = 0; corner < 4; corner++) {
            PhoneRect r = phone_panel_rect(safe, px, H, 0.22f, corner, aspect, &t.panel);
            float bez = t.panel.bezel * px;
            PhoneRect outer = { r.x - bez, r.y - bez, r.w + 2 * bez, r.h + 2 * bez };
            CHECK(fabsf(r.h - 0.22f * H) < 1.0f && fabsf(r.w / r.h - aspect) < 1e-3f, "%dx%d corner %d: 22 %% high, phone aspect", res[i][0], res[i][1], corner);
            CHECK(outer.x >= safe.x - 0.5f && outer.y >= safe.y - 0.5f && outer.x + outer.w <= safe.x + safe.w + 0.5f && outer.y + outer.h <= safe.y + safe.h + 0.5f,
                  "%dx%d corner %d: inside the safe area", res[i][0], res[i][1], corner);
            CHECK(!overlaps(outer, readout) || corner == 1 || corner == 3, "%dx%d corner %d: clear of the item readout", res[i][0], res[i][1], corner);
            bool right = corner == 0 || corner == 2, bottom = corner <= 1;
            CHECK(right ? outer.x + outer.w > safe.x + safe.w - 1 : outer.x < safe.x + 1, "%dx%d corner %d: against its side", res[i][0], res[i][1], corner);
            CHECK(bottom ? outer.y + outer.h > safe.y + safe.h - t.panel.readout_gap * px - 1 : outer.y < safe.y + 1, "%dx%d corner %d: against its edge", res[i][0], res[i][1], corner);
        }
        // An oversized panel shrinks to the room left.
        PhoneRect big = phone_panel_rect(safe, px, H, 0.5f, 0, aspect, &t.panel);
        CHECK(big.y >= safe.y && big.h <= safe.h, "%dx%d: 50 %% panel fits", res[i][0], res[i][1]);
    }
}

static void test_gallery_delete(const char *dir) {
    char base[512], d[600], path[700];
    snprintf(base, sizeof(base), "%sdelete_test/", dir);
    SDL_CreateDirectory(base);
    snprintf(d, sizeof(d), "%sscreenshots", base);
    SDL_CreateDirectory(d);
    snprintf(d, sizeof(d), "%sscreenshots/phone", base);
    SDL_CreateDirectory(d);
    PhoneGallery *g = (PhoneGallery *)calloc(1, sizeof(PhoneGallery));
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/del_%d.jpg", d, i);
        platform_write_file(path, "\xff\xd8x", 3);
        char side[720];
        snprintf(side, sizeof(side), "%s/del_%d.json", d, i);
        platform_write_file(side, "{}", 2);
        PhoneGalleryItem *it = phone_gallery_add(g, path, false, NULL);
        it->server_id = 10 + i;
    }
    CHECK(g->count == 3 && phone_gallery_find_server_id(g, 11) == 1, "3 items, found by server id");
    snprintf(path, sizeof(path), "%s/del_1.jpg", d);
    CHECK(phone_gallery_delete(g, 1) && g->count == 2 && !platform_file_exists(path), "delete: entry + file gone");
    snprintf(path, sizeof(path), "%s/del_1.json", d);
    CHECK(!platform_file_exists(path), "delete: sidecar gone");
    CHECK(phone_gallery_find_server_id(g, 11) < 0 && phone_gallery_find_server_id(g, 12) == 0 && !phone_gallery_delete(g, 5), "delete: the others stay, out of range is refused");
    while (g->count) phone_gallery_delete(g, 0);
    free(g);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    SDL_Init(0);
    jobs_init(3);
    char dir[512];
    snprintf(dir, sizeof(dir), "%sphone_test/", platform_pref_dir());
    SDL_CreateDirectory(dir);
    test_qr();
    test_auth();
    test_proto();
    test_orientation();
    test_filter_and_reach();
    test_jpeg_pipeline();
    test_photo_video_files(dir);
    test_server_roundtrip(dir);
    test_tls(dir);
    test_phone_os_flow();
    test_phone_os_layout();
    test_phone_os_assets();
    test_phone_panel_layout();
    test_gallery_delete(dir);
    hws_random_shutdown();
    jobs_shutdown();
    printf("phone: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
