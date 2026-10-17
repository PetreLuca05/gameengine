// HUD (docs/UI_WHEELS_HUD.md): radial wheel selection (angles, dead zone, hysteresis, the virtual
// cursor, keys / scroll), the wheel JSON files, the timed ring (fill, countdown text), pings (cap,
// eviction, expiry, rate limit, wire packing, the ping types file), kill stats (streaks, the feed),
// the compass math, the console registry categories / lists / search, and the theme tokens the HUD
// code asks for. Run from the build output directory (assets/ next to it).
#include "ui/widgets/radial_math.h"
#include "ui/widgets/wheel_def.h"
#include "ui/widgets/timed_ring.h"
#include "ui/hud/compass_math.h"
#include "gameplay/ping.h"
#include "gameplay/kill_stats.h"
#include "core/cmd_registry.h"
#include "core/engine.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DG_SOURCE_DIR
#define DG_SOURCE_DIR "."
#endif

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

#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (eps))

// ------------------------------------------------------------------ radial wheel

static vec2 dir_deg(float deg, float len) {
    float a = deg * DG_DEG2RAD;
    return v2(sinf(a) * len, -cosf(a) * len);   // screen: up = -y, clockwise
}

static void test_radial(void) {
    // Angles: up 0, right 90, down 180, left 270.
    CHECK(NEAR(radial_angle_of(v2(0, -1)) * DG_RAD2DEG, 0, 0.01f), "up");
    CHECK(NEAR(radial_angle_of(v2(1, 0)) * DG_RAD2DEG, 90, 0.01f), "right");
    CHECK(NEAR(radial_angle_of(v2(0, 1)) * DG_RAD2DEG, 180, 0.01f), "down");
    CHECK(NEAR(radial_angle_of(v2(-1, 0)) * DG_RAD2DEG, 270, 0.01f), "left");
    // Every slot count 2..12: each slot's centre direction picks that slot.
    for (int n = RADIAL_MIN_SLOTS; n <= RADIAL_MAX_SLOTS; n++) {
        for (int k = 0; k < n; k++) {
            float a = radial_slot_angle(k, n);
            CHECK(radial_slot_for_angle(a, n) == k, "n %d slot %d centre", n, k);
            float half = DG_PI / (float)n;
            CHECK(radial_slot_for_angle(a + half * 0.9f, n) == k, "n %d slot %d inside +", n, k);
            CHECK(radial_slot_for_angle(a - half * 0.9f, n) == k, "n %d slot %d inside -", n, k);
            CHECK(radial_slot_for_angle(a + half * 1.1f, n) == (k + 1) % n, "n %d slot %d next", n, k);
        }
    }
    CHECK(NEAR(radial_angle_dist(0.1f, 2 * DG_PI - 0.1f), 0.2f, 1e-4f), "wraps");
    RadialParams p = { 8, 20.0f, 120.0f, 6.0f * DG_DEG2RAD };
    // Dead zone: no selection yet, and an existing one is kept.
    RadialSel s = { -1, { 0, 0 } };
    radial_feed_delta(&s, &p, v2(5, -5));
    CHECK(s.slot == -1, "dead zone keeps none (%d)", s.slot);
    radial_feed_delta(&s, &p, v2(30, 0));
    CHECK(s.slot == 2, "right = slot 2 of 8 (%d)", s.slot);
    s.cursor = v2(3, 3);
    radial_update(&s, &p);
    CHECK(s.slot == 2, "back in the dead zone keeps the slot (%d)", s.slot);
    // Clamp to the radius.
    s.cursor = v2(0, 0);
    radial_feed_delta(&s, &p, v2(1000, 0));
    CHECK(NEAR(sqrtf(s.cursor.x * s.cursor.x + s.cursor.y * s.cursor.y), 120, 0.01f), "cursor clamp");
    // Hysteresis: just past the edge keeps the slot, past edge + margin switches.
    float edge = 90.0f + 22.5f;
    s.slot = 2;
    s.cursor = dir_deg(edge + 3.0f, 100);
    radial_update(&s, &p);
    CHECK(s.slot == 2, "inside the hysteresis band keeps 2 (%d)", s.slot);
    s.cursor = dir_deg(edge + 8.0f, 100);
    radial_update(&s, &p);
    CHECK(s.slot == 3, "past the band switches to 3 (%d)", s.slot);
    s.cursor = dir_deg(edge - 3.0f, 100);
    radial_update(&s, &p);
    CHECK(s.slot == 3, "and back inside the band stays 3 (%d)", s.slot);
    // Hysteresis is capped so small slots still switch (12 slots: half = 15 deg).
    RadialParams p12 = { 12, 20.0f, 120.0f, 40.0f * DG_DEG2RAD };
    RadialSel s12 = { 0, { 0, 0 } };
    s12.cursor = dir_deg(30.0f, 100);
    radial_update(&s12, &p12);
    CHECK(s12.slot == 1, "capped hysteresis lets the next slot win (%d)", s12.slot);
    // Stick: absolute.
    RadialSel st = { -1, { 0, 0 } };
    radial_feed_stick(&st, &p, v2(0, 1));
    CHECK(st.slot == 4, "stick down = slot 4 (%d)", st.slot);
    radial_feed_stick(&st, &p, v2(0.05f, 0.05f));
    CHECK(st.slot == 4, "centred stick keeps (%d)", st.slot);
    // Keys / scroll.
    radial_select(&st, &p, 6);
    CHECK(st.slot == 6 && st.cursor.x == 0 && st.cursor.y == 0, "number key");
    radial_select(&st, &p, 9);
    CHECK(st.slot == 6, "out of range ignored");
    radial_step(&st, &p, 1);
    radial_step(&st, &p, 1);
    CHECK(st.slot == 0, "scroll wraps forward (%d)", st.slot);
    radial_step(&st, &p, -1);
    CHECK(st.slot == 7, "scroll wraps back (%d)", st.slot);
    st.slot = -1;
    radial_step(&st, &p, -1);
    CHECK(st.slot == 7, "scroll from none (%d)", st.slot);
}

static char *read_asset(const char *path) {
    char *s = platform_read_file(path, NULL);
    CHECK(s != NULL, "missing %s", path);
    return s;
}

static void test_wheel_files(void) {
    static const char *const files[] = { "healing", "throwables", "ping", "door" };
    WheelLib lib;
    memset(&lib, 0, sizeof(lib));
    for (int i = 0; i < 4; i++) {
        char path[96], err[160];
        snprintf(path, sizeof(path), "assets/ui/wheels/%s.json", files[i]);
        char *json = read_asset(path);
        if (!json) continue;
        WheelDef d;
        bool ok = wheel_def_parse(&d, json, err, sizeof(err));
        CHECK(ok, "%s: %s", path, err);
        if (ok) {
            CHECK(!strcmp(d.id, files[i]), "%s id %s", path, d.id);
            CHECK(d.min_slots >= 2 && d.max_slots <= 12, "%s slot bounds", path);
            wheel_lib_add(&lib, &d);
        }
        free(json);
    }
    CHECK(lib.count == 4, "4 wheels (%d)", lib.count);
    const WheelDef *door = wheel_lib_find(&lib, "door");
    CHECK(door && door->source == WHEEL_STATIC && door->slot_count == 3, "door wheel: 3 static slots");
    CHECK(door && door->default_slot == wheel_slot_index(door, "open"), "door default = open");
    CHECK(door && wheel_slot_index(door, "slam") >= 0 && wheel_slot_index(door, "peek") >= 0, "door slam / peek");
    const WheelDef *ping = wheel_lib_find(&lib, "ping");
    CHECK(ping && ping->slot_count == PING_TYPES, "ping wheel: 8 types");
    for (int i = 0; ping && i < ping->slot_count; i++) CHECK(ping_type_from_id(ping->slots[i].id) == i, "ping slot %d = type order", i);
    const WheelDef *heal = wheel_lib_find(&lib, "healing");
    CHECK(heal && heal->source == WHEEL_INVENTORY && !strcmp(heal->filter_kind, "consumable") && !strcmp(heal->filter_tag, "heal"), "healing filter");
    CHECK(heal && wheel_def_order(heal, "medkit") == 0 && wheel_def_order(heal, "nothing") == -1, "healing order");
    const WheelDef *thr = wheel_lib_find(&lib, "throwables");
    CHECK(thr && !strcmp(thr->filter_kind, "throwable"), "throwables filter");
    // Bad files.
    WheelDef d;
    char err[160];
    CHECK(!wheel_def_parse(&d, "{ \"id\": \"x\", \"source\": \"static\", \"slots\": [{ \"id\": \"a\" }] }", err, sizeof(err)), "1 static slot rejected");
    CHECK(!wheel_def_parse(&d, "{ \"id\": \"x\", \"source\": \"nope\" }", err, sizeof(err)), "bad source rejected");
    CHECK(!wheel_def_parse(&d, "{ \"id\": \"x\", \"source\": \"inventory\", \"min_slots\": 1 }", err, sizeof(err)), "min_slots < 2 rejected");
    CHECK(!wheel_def_parse(&d, "{ \"id\": \"x\", \"source\": \"static\", \"default\": \"zz\", \"slots\": [\"a\", \"b\"] }", err, sizeof(err)), "unknown default rejected");
    CHECK(!wheel_def_parse(&d, "{ broken", err, sizeof(err)), "syntax error");
    char big[2048] = "{ \"id\": \"x\", \"source\": \"static\", \"slots\": [";
    for (int i = 0; i < 13; i++) { char t[24]; snprintf(t, sizeof(t), "%s\"s%d\"", i ? "," : "", i); strcat(big, t); }
    strcat(big, "] }");
    CHECK(!wheel_def_parse(&d, big, err, sizeof(err)), "13 slots rejected");
}

// ------------------------------------------------------------------ timed ring

static void test_timed_ring(void) {
    CHECK(NEAR(timed_ring_fill(10, 10, TR_DRAIN), 1, 1e-5f), "drain full at start");
    CHECK(NEAR(timed_ring_fill(10, 2.5f, TR_DRAIN), 0.25f, 1e-5f), "drain quarter");
    CHECK(NEAR(timed_ring_fill(10, 2.5f, TR_CHARGE), 0.75f, 1e-5f), "charge");
    CHECK(timed_ring_fill(0, 1, TR_DRAIN) == 0, "zero duration");
    CHECK(timed_ring_fill(4, 9, TR_DRAIN) == 1 && timed_ring_fill(4, -1, TR_DRAIN) == 0, "clamped");
    char b[16];
    timed_ring_text(7.42f, b, sizeof(b));
    CHECK(!strcmp(b, "7.5"), "7.42 -> 7.5 (%s)", b);   // rounded up to the tenth
    timed_ring_text(9.99f, b, sizeof(b));
    CHECK(!strcmp(b, "10.0"), "9.99 -> 10.0 (%s)", b);
    timed_ring_text(12.2f, b, sizeof(b));
    CHECK(!strcmp(b, "13"), "12.2 -> 13 (%s)", b);
    timed_ring_text(0.01f, b, sizeof(b));
    CHECK(!strcmp(b, "0.1"), "never 0.0 before the end (%s)", b);
    timed_ring_text(0, b, sizeof(b));
    CHECK(!strcmp(b, "0.0"), "end (%s)", b);
    timed_ring_text(-3, b, sizeof(b));
    CHECK(!strcmp(b, "0.0"), "negative (%s)", b);
}

// ------------------------------------------------------------------ pings

static void test_pings(void) {
    PingList l;
    memset(&l, 0, sizeof(l));
    for (int i = 0; i < PING_PER_PLAYER; i++) ping_add(&l, (u32)(i + 1), 0, PING_GENERIC, v3((float)i, 0, 0), (float)i, 20);
    CHECK(ping_count_owner(&l, 0) == 3, "3 pings");
    ping_add(&l, 4, 0, PING_ENEMY, v3(9, 0, 0), 5, 10);
    CHECK(ping_count_owner(&l, 0) == 3, "still 3 after a 4th");
    bool has1 = false, has4 = false;
    for (int i = 0; i < PING_MAX; i++) if (l.p[i].active) { has1 |= l.p[i].uid == 1; has4 |= l.p[i].uid == 4; }
    CHECK(!has1 && has4, "the oldest went");
    ping_add(&l, 10, 1, PING_LOOT, v3(0, 0, 9), 5, 45);
    CHECK(ping_count(&l) == 4 && ping_count_owner(&l, 1) == 1, "other owners unaffected");
    ping_add(&l, 4, 0, PING_DANGER, v3(9, 0, 0), 6, 20);   // same uid replaces
    CHECK(ping_count_owner(&l, 0) == 3, "same uid replaces");
    const Ping *near = ping_find_near(&l, 0, v3(9.5f, 0, 0), 2.5f);
    CHECK(near && near->uid == 4 && near->type == PING_DANGER, "find near own");
    CHECK(!ping_find_near(&l, 1, v3(9.5f, 0, 0), 2.5f), "not another owner's");
    CHECK(ping_remove(&l, 4) && !ping_remove(&l, 4), "remove once");
    ping_expire(&l, 23.0f);   // uid 2 (born 1, 20 s) and 3 (born 2) expire at 21 / 22; 10 lives to 50
    CHECK(ping_count(&l) == 1, "expired (%d left)", ping_count(&l));
    Ping fp = { true, 1, 0, 0, { 0, 0, 0 }, 10, 20 };
    CHECK(NEAR(ping_alpha(&fp, 10.1f), 0.5f, 0.01f) && NEAR(ping_alpha(&fp, 15), 1, 1e-4f) && NEAR(ping_alpha(&fp, 19.25f), 0.5f, 0.01f), "alpha fade");
    // Rate limit: min gap, then the window.
    PingLimiter lim;
    memset(&lim, 0, sizeof(lim));
    CHECK(ping_rate_ok(&lim, 0, 1.0f), "first");
    CHECK(!ping_rate_ok(&lim, 0, 1.1f), "too soon");
    CHECK(ping_rate_ok(&lim, 0, 1.3f) && ping_rate_ok(&lim, 0, 1.6f) && ping_rate_ok(&lim, 0, 1.9f), "4 in the window");
    CHECK(!ping_rate_ok(&lim, 0, 2.2f), "5th within 3 s refused");
    CHECK(ping_rate_ok(&lim, 0, 4.05f), "after the window");
    CHECK(ping_rate_ok(&lim, 1, 2.2f), "per player");
    CHECK(!ping_rate_ok(&lim, PING_MAX_PLAYERS, 1), "bad slot");
    // Wire packing (cm).
    int a, b, c, t;
    vec3 pos;
    ping_pack(PING_VEHICLE, v3(-1234.56f, 87.65f, 4321.09f), &a, &b, &c);
    ping_unpack(a, b, c, &t, &pos);
    CHECK(t == PING_VEHICLE && NEAR(pos.x, -1234.56f, 0.006f) && NEAR(pos.y, 87.65f, 0.006f) && NEAR(pos.z, 4321.09f, 0.006f), "pack round trip");
    ping_pack(PING_GO, v3(0, -50.5f, 0), &a, &b, &c);
    ping_unpack(a, b, c, &t, &pos);
    CHECK(t == PING_GO && NEAR(pos.y, -50.5f, 0.006f), "negative height");
    CHECK(ping_contextual_type(1) == PING_ENEMY && ping_contextual_type(2) == PING_LOOT && ping_contextual_type(3) == PING_VEHICLE && ping_contextual_type(0) == PING_GENERIC, "contextual");
    // The ping types file.
    PingTypes pt;
    ping_types_defaults(&pt);
    char *json = read_asset("assets/ui/wheels/ping.json");
    char err[128];
    if (json) CHECK(ping_types_parse(&pt, json, err, sizeof(err)), "ping.json: %s", err);
    free(json);
    for (int i = 0; i < PING_TYPES; i++) {
        CHECK(pt.t[i].lifetime >= 1 && pt.t[i].label[0] && pt.t[i].chat[0], "type %d complete", i);
        CHECK(pt.t[i].color >= 0 && pt.t[i].color < 8, "type %d colour", i);
    }
    CHECK(pt.t[PING_ENEMY].lifetime < pt.t[PING_VEHICLE].lifetime, "enemy pings are short-lived");
    CHECK(!ping_types_parse(&pt, "{ \"slots\": [{ \"id\": \"nope\" }] }", err, sizeof(err)), "unknown type rejected");
    CHECK(!ping_types_parse(&pt, "{ \"slots\": [{ \"id\": \"enemy\", \"lifetime\": 0 }] }", err, sizeof(err)), "bad lifetime rejected");
}

// ------------------------------------------------------------------ kills

static void test_kills(void) {
    KillStats k;
    kill_stats_reset(&k);
    kill_stats_add(&k, KILLF_VICTIM_ZOMBIE, 1.0f);
    kill_stats_add(&k, KILLF_VICTIM_ZOMBIE | KILLF_HEADSHOT, 2.0f);
    kill_stats_add(&k, KILLF_VICTIM_PLAYER, 5.5f);
    CHECK(k.zombies == 2 && k.players == 1 && kill_stats_total(&k) == 3 && k.headshots == 1, "counts");
    CHECK(k.streak == 3 && k.best_streak == 3, "streak 3 (%d)", k.streak);
    kill_stats_add(&k, KILLF_VICTIM_ZOMBIE, 10.0f);
    CHECK(k.streak == 1 && k.best_streak == 3, "streak broken after 4 s (%d)", k.streak);
    kill_stats_tick(&k, 15.0f, 0.1f);
    CHECK(k.streak == 0, "streak times out");
    CHECK(k.pulse < 1.0f && k.pulse > 0.0f, "pulse decays");
    for (int i = 0; i < 20; i++) kill_stats_tick(&k, 15.0f, 0.1f);
    CHECK(k.pulse == 0 && k.head_tick == 0, "pulse / tick done");
    KillFeed f;
    memset(&f, 0, sizeof(f));
    for (int i = 0; i < 7; i++) {
        char v[16];
        snprintf(v, sizeof(v), "z%d", i);
        killfeed_push(&f, "Ana", v, 3, 0, i == 6, false);
    }
    CHECK(killfeed_count(&f) == KILLFEED_MAX, "5 entries max");
    CHECK(!strcmp(f.e[0].victim, "z6") && f.e[0].mine && !strcmp(f.e[4].victim, "z2"), "newest first");
    killfeed_tick(&f, 5.5f);
    CHECK(NEAR(killfeed_alpha(&f.e[0]), 0.5f, 1e-4f), "fades over the last second");
    killfeed_tick(&f, 0.6f);
    CHECK(killfeed_count(&f) == 0, "gone after 6 s");
}

// ------------------------------------------------------------------ compass

static void test_compass(void) {
    CHECK(NEAR(compass_heading_deg(0), 0, 1e-4f), "yaw 0 = north");
    CHECK(NEAR(compass_heading_deg(-DG_PI * 0.5f), 90, 1e-3f), "turning right = east");
    CHECK(NEAR(compass_heading_deg(DG_PI * 0.5f), 270, 1e-3f), "left = west");
    CHECK(NEAR(compass_bearing_deg(v3(0, 0, 0), v3(0, 0, -10)), 0, 1e-3f), "north = -Z");
    CHECK(NEAR(compass_bearing_deg(v3(0, 0, 0), v3(10, 0, 0)), 90, 1e-3f), "east = +X");
    CHECK(NEAR(compass_bearing_deg(v3(0, 0, 0), v3(0, 0, 10)), 180, 1e-3f), "south");
    CHECK(NEAR(compass_offset_deg(10, 350), 20, 1e-3f) && NEAR(compass_offset_deg(350, 10), -20, 1e-3f), "offset wraps");
    CHECK(NEAR(compass_wrap360(-30), 330, 1e-3f) && NEAR(compass_wrap180(270), -90, 1e-3f), "wrap");
    char b[16];
    compass_label(0, b, sizeof(b));
    CHECK(!strcmp(b, "N"), "N");
    compass_label(135, b, sizeof(b));
    CHECK(!strcmp(b, "SE"), "SE");
    compass_label(-90, b, sizeof(b));
    CHECK(!strcmp(b, "W"), "W");
    compass_label(105, b, sizeof(b));
    CHECK(!strcmp(b, "105"), "105");
    char c[64];
    compass_coords_text(v3(12.34f, -1.5f, -45.66f), c, sizeof(c));
    CHECK(!strcmp(c, "X 12.3   Z -45.7   H -1.5"), "coords (%s)", c);
    char *json = read_asset("assets/ui/hud_compass.json");
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    CHECK(root != NULL, "hud_compass.json parses");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "tick_every"), *l = cJSON_GetObjectItemCaseSensitive(root, "label_every");
    CHECK(cJSON_IsNumber(t) && t->valuedouble == 5 && cJSON_IsNumber(l) && l->valuedouble == 15, "ticks every 5, labels every 15");
    const cJSON *hz = cJSON_GetObjectItemCaseSensitive(root, "coords_hz");
    CHECK(cJSON_IsNumber(hz) && hz->valuedouble == 10, "coordinates at 10 Hz");
    cJSON_Delete(root);
    free(json);
}

// ------------------------------------------------------------------ registry

static void test_registry(void) {
    CmdRegistryCfg cfg;
    char err[128];
    char *json = read_asset("assets/ui/command_categories.json");
    bool ok = json && cmdreg_parse(&cfg, json, err, sizeof(err));
    CHECK(ok, "command_categories.json: %s", err);
    free(json);
    if (ok) {
        int r = cmdreg_category_by_id(&cfg, "rendering"), s = cmdreg_category_by_id(&cfg, "system"), n = cmdreg_category_by_id(&cfg, "network");
        CHECK(r >= 0 && s >= 0 && n >= 0, "categories");
        CHECK(cmdreg_category(&cfg, "r_bloom") == r, "prefix r_");
        CHECK(cmdreg_category(&cfg, "sv_cheats") == s, "exact name beats prefix");
        CHECK(cmdreg_category(&cfg, "sv_hostname") == n, "sv_ prefix");
        CHECK(cmdreg_category(&cfg, "zz_unknown") == -1, "unknown");
        CHECK(cmdreg_destructive(&cfg, "quit") && !cmdreg_destructive(&cfg, "noclip"), "destructive");
        CHECK(cfg.favorites.count > 0, "default favorites");
    }
    CmdRegistryCfg bad;
    CHECK(!cmdreg_parse(&bad, "{ \"categories\": [] }", err, sizeof(err)), "no categories rejected");
    CHECK(!cmdreg_parse(&bad, "{ \"categories\": [{ \"id\": \"a\" }], \"names\": { \"x\": \"b\" } }", err, sizeof(err)), "unknown name category rejected");
    // Longest prefix wins.
    CmdRegistryCfg t;
    CHECK(cmdreg_parse(&t, "{ \"categories\": [{ \"id\": \"a\", \"prefixes\": [\"r_\"] }, { \"id\": \"b\", \"prefixes\": [\"r_vol\"] }] }", err, sizeof(err)), "parse");
    CHECK(cmdreg_category(&t, "r_volumetrics") == 1 && cmdreg_category(&t, "r_bloom") == 0, "longest prefix");
    // Search.
    CHECK(cmdreg_match("", "anything", NULL), "empty query");
    CHECK(cmdreg_match("BLOOM", "r_bloom", "bloom strength"), "case-insensitive");
    CHECK(cmdreg_match("r_ strength", "r_bloom", "bloom strength"), "every word, name or description");
    CHECK(!cmdreg_match("bloom fog", "r_bloom", "bloom strength"), "all words must match");
    // Lists.
    CmdList l;
    cmdlist_parse(&l, "  noclip give  noclip map ");
    CHECK(l.count == 3 && !strcmp(l.items[2], "map"), "parse dedupes (%d)", l.count);
    char b[128];
    cmdlist_format(&l, b, sizeof(b));
    CHECK(!strcmp(b, "noclip give map"), "format (%s)", b);
    CHECK(!cmdlist_toggle(&l, "give") && !cmdlist_has(&l, "give") && l.count == 2, "toggle off");
    CHECK(cmdlist_toggle(&l, "give") && cmdlist_has(&l, "give"), "toggle on");
    CmdList r;
    memset(&r, 0, sizeof(r));
    cmdlist_touch(&r, "a", 3);
    cmdlist_touch(&r, "b", 3);
    cmdlist_touch(&r, "c", 3);
    cmdlist_touch(&r, "a", 3);
    CHECK(r.count == 3 && !strcmp(r.items[0], "a") && !strcmp(r.items[1], "c") && !strcmp(r.items[2], "b"), "recent: moves to front");
    cmdlist_touch(&r, "d", 3);
    CHECK(r.count == 3 && !strcmp(r.items[0], "d") && !strcmp(r.items[2], "c"), "recent: oldest drops");
    cmdlist_format(&l, b, 8);
    CHECK(strlen(b) < 8 && (b[0] == 0 || b[strlen(b) - 1] != ' '), "format truncates at whole names (%s)", b);
}

// ------------------------------------------------------------------ theme tokens

// Every gui_tok("...") the HUD / wheels / Commands code asks for exists in theme.json "layout".
static void test_tokens(void) {
    char *json = read_asset("assets/ui/theme.json");
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    const cJSON *layout = cJSON_GetObjectItemCaseSensitive(root, "layout");
    CHECK(cJSON_IsObject(layout), "theme layout");
    static const char *const files[] = { "src/ui/widgets/radial_wheel.c", "src/ui/widgets/timed_ring.c", "src/ui/hud/hud_kills.c", "src/ui/hud/hud_compass.c",
                                         "src/ui/hud/hud_pings.c", "src/ui/pause/commands_tab.c", "src/game/game_wheels.c", "src/game/game_peek.c" };
    int found = 0;
    for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, files[i]);
        char *src = platform_read_file(path, NULL);
        CHECK(src != NULL, "source %s", path);
        if (!src) continue;
        for (const char *p = strstr(src, "gui_tok(\""); p; p = strstr(p + 1, "gui_tok(\"")) {
            char name[48];
            const char *q = p + 9;
            int n = 0;
            while (q[n] && q[n] != '"' && n < 47) { name[n] = q[n]; n++; }
            name[n] = 0;
            CHECK(cJSON_GetObjectItemCaseSensitive(layout, name) != NULL, "%s: token %s missing from theme.json", files[i], name);
            found++;
        }
        free(src);
    }
    CHECK(found > 10, "token references scanned (%d)", found);
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(root, "colors");
    const cJSON *pt = cJSON_GetObjectItemCaseSensitive(colors, "ping_types");
    CHECK(cJSON_IsArray(pt) && cJSON_GetArraySize(pt) == 8, "8 ping colours");
    const cJSON *sel = cJSON_GetObjectItemCaseSensitive(colors, "select");
    CHECK(cJSON_IsArray(sel) && cJSON_GetArrayItem(sel, 0)->valueint == 255 && cJSON_GetArrayItem(sel, 1)->valueint == 255 && cJSON_GetArrayItem(sel, 2)->valueint == 255, "selection is white");
    cJSON_Delete(root);
    free(json);
}

int main(void) {
    test_radial();
    test_wheel_files();
    test_timed_ring();
    test_pings();
    test_kills();
    test_compass();
    test_registry();
    test_tokens();
    printf("test_hud: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
