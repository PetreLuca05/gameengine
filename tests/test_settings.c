// Settings audit tests (docs/SETTINGS_AUDIT.md), on the real assets and the real cvar system:
//   * release locks: a user write of a TIER / LOCKED cvar is rejected (and counted), Engine Fidelity
//     writes TIER cvars, the host writes everything, a LOCKED | RULE cvar takes user writes only where
//     this machine sets the rules (not a client);
//   * config.cfg: only player settings and the host's rules are saved; TIER / LOCKED entries of an
//     older config are dropped on load;
//   * consistency set: loads, every entry is CVAR_LOCKED in the audit, the hash ignores order /
//     formatting and changes with any value, a missing reason is refused, the join check rejects a
//     different set;
//   * every fidelity level (under the locks) resolves every locked parameter to the same value: the
//     resolved cvar sets of two levels differ only in TIER / EDITABLE fidelity features, and the
//     consistency set holds after each level;
//   * the audit file: every fidelity feature is TIER or EDITABLE, the Graphics tab places are only
//     Engine Fidelity / Display / Comfort (+ the dev page), every place shown to a player is EDITABLE.
#include "core/consistency.h"
#include "core/cvar.h"
#include "core/engine.h"
#include "core/platform.h"
#include "render/fidelity/fidelity.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;   // the core library logs through it
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

static cJSON *g_audit;   // assets/ui/settings_audit.json "cvars"
static cJSON *g_audit_root;

static const char *audit_class(const char *cvar) {
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(g_audit, cvar);
    const cJSON *c = e ? cJSON_GetObjectItemCaseSensitive(e, "class") : NULL;
    return cJSON_IsString(c) ? c->valuestring : NULL;
}

static u32 flags_of_class(const char *cls) {
    if (!cls) return 0;
    if (!strcmp(cls, "EDITABLE")) return CVAR_EDITABLE;
    if (!strcmp(cls, "TIER")) return CVAR_TIER;
    if (!strcmp(cls, "LOCKED")) return CVAR_LOCKED;
    if (!strcmp(cls, "LOCKED+RULE")) return CVAR_LOCKED | CVAR_RULE;
    return 0;
}

static bool g_client;   // the rule gate: this machine is a client of someone else's server
static bool rule_gate(void) { return !g_client; }

// ------------------------------------------------------------------ locks

static void test_locks(void) {
    Cvar *ed = cvar_reg_float("t_editable", 1, 0, 10, CVAR_ARCHIVE | CVAR_EDITABLE, "test editable");
    Cvar *ti = cvar_reg_int("t_tier", 1, 0, 3, CVAR_TIER, "test tier");
    Cvar *lo = cvar_reg_float("t_locked", 1, 0, 10, CVAR_LOCKED, "test locked");
    Cvar *ru = cvar_reg_float("t_rule", 1, 0, 10, CVAR_ARCHIVE | CVAR_LOCKED | CVAR_RULE, "test rule");
    cvar_set_rule_gate(rule_gate);
    CHECK(cvar_dev_build(), "unit tests are dev builds");
    // Dev build, locks off: everything writable.
    cvar_set_release_locks(false);
    cvar_set_float(ti, 2);
    cvar_set_float(lo, 2);
    CHECK(ti->i == 2 && lo->f == 2.0f, "dev build: tier / locked writable");
    // Release locks.
    cvar_set_release_locks(true);
    int rej = cvar_rejected_count();
    cvar_set_float(ed, 3);
    CHECK(ed->f == 3.0f, "release: editable writable");
    cvar_set_float(ti, 3);
    cvar_set("t_locked", "5");
    cmd_execute_line("t_locked 6");   // the console path
    CHECK(ti->i == 2 && lo->f == 2.0f, "release: user writes of tier / locked rejected (%d, %g)", ti->i, lo->f);
    CHECK(cvar_rejected_count() == rej + 3, "release: three rejections counted (%d)", cvar_rejected_count() - rej);
    CvarSource s = cvar_source_push(CVAR_SRC_FIDELITY);
    cvar_set_float(ti, 3);
    cvar_set_float(lo, 7);
    cvar_source_pop(s);
    CHECK(ti->i == 3 && lo->f == 2.0f, "release: Engine Fidelity writes tier cvars, not locked ones");
    s = cvar_source_push(CVAR_SRC_HOST);
    cvar_set_float(lo, 8);
    cvar_source_pop(s);
    CHECK(lo->f == 8.0f, "release: the host / consistency set writes locked cvars");
    cvar_reset(lo);
    CHECK(lo->f == 8.0f, "release: a user reset of a locked cvar is rejected too");
    // Server rules: the host's machine sets them, a client takes the host's.
    g_client = false;
    cvar_set_float(ru, 4);
    CHECK(ru->f == 4.0f, "rule: the host (or a solo game) sets it");
    g_client = true;
    cvar_set_float(ru, 5);
    CHECK(ru->f == 4.0f, "rule: a client cannot set it");
    s = cvar_source_push(CVAR_SRC_HOST);   // MSG_CVARS from the host
    cvar_set_float(ru, 6);
    cvar_source_pop(s);
    CHECK(ru->f == 6.0f, "rule: replicated from the host");
    g_client = false;
    CHECK(!strcmp(cvar_class_name(ed->flags), "EDITABLE") && !strcmp(cvar_class_name(ti->flags), "TIER") && !strcmp(cvar_class_name(lo->flags), "LOCKED") &&
              !strcmp(cvar_class_name(ru->flags), "LOCKED+RULE"),
          "class names");
}

// ------------------------------------------------------------------ config.cfg

static void test_config(void) {
    cvar_set_release_locks(true);
    Cvar *ed = cvar_find("t_editable"), *ru = cvar_find("t_rule");
    Cvar *ti_a = cvar_reg_int("t_tier_archived", 1, 0, 3, CVAR_ARCHIVE | CVAR_TIER, "an old archived tier cvar");
    Cvar *lo_a = cvar_reg_float("t_locked_archived", 1, 0, 10, CVAR_ARCHIVE | CVAR_LOCKED, "an old archived locked cvar");
    CHECK(cvar_saved(ed) && cvar_saved(ru) && !cvar_saved(ti_a) && !cvar_saved(lo_a), "saved: editable + rules only");
    const char *path = "test_settings_config.cfg";
    config_set_enabled(true);
    CHECK(config_save(path), "config written");
    char *text = platform_read_file(path, NULL);
    CHECK(text && strstr(text, "t_editable") && strstr(text, "t_rule") && !strstr(text, "t_tier_archived") && !strstr(text, "t_locked_archived"),
          "config.cfg holds no tier / locked values");
    free(text);
    // An older config with tier / locked entries: dropped on load, the player's values kept.
    const char *old = "set t_editable \"7\"\nset t_tier_archived \"3\"\nset t_locked_archived \"9\"\nset t_locked \"9\"\n";
    platform_write_file(path, old, strlen(old));
    int rej = cvar_rejected_count();
    CHECK(config_load_file(path), "old config loads");
    CHECK(ed->f == 7.0f && ti_a->i == 1 && lo_a->f == 1.0f && cvar_find("t_locked")->f == 8.0f, "old config: editable taken, tier / locked dropped");
    CHECK(config_load_dropped() == 3 && cvar_rejected_count() == rej, "three entries dropped quietly (%d), none rejected", config_load_dropped());
    remove(path);
    char bak[64];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    remove(bak);
    config_set_enabled(false);
}

// ------------------------------------------------------------------ consistency set

static void test_consistency(void) {
    char err[256] = "";
    CHECK(consistency_load_text("{\"values\": {\"b\": {\"value\": 2, \"why\": \"reason two\"}, \"a\": {\"value\": 1.0, \"why\": \"reason one\"}}}", err, sizeof(err)), "small set: %s", err);
    u32 h1 = consistency_hash();
    CHECK(consistency_load_text("{\n \"values\": { \"a\": { \"why\": \"other words\", \"value\": 1 },\n  \"b\": { \"value\": 2.0, \"why\": \"reason two\" } } }", err, sizeof(err)), "reordered: %s", err);
    CHECK(consistency_hash() == h1, "the hash ignores order, formatting and reasons (%08x %08x)", h1, consistency_hash());
    CHECK(consistency_load_text("{\"values\": {\"a\": {\"value\": 1, \"why\": \"reason one\"}, \"b\": {\"value\": 3, \"why\": \"reason two\"}}}", err, sizeof(err)), "changed");
    CHECK(consistency_hash() != h1, "the hash changes with any value");
    CHECK(!consistency_load_text("{\"values\": {\"a\": {\"value\": 1}}}", err, sizeof(err)), "an entry without a reason is refused");
    CHECK(!consistency_load_text("{\"values\": {\"a\": {\"why\": \"no value here\"}}}", err, sizeof(err)), "an entry without a value is refused");

    CHECK(consistency_load(CONSISTENCY_PATH), "%s loads", CONSISTENCY_PATH);
    CHECK(consistency_count() >= 40 && consistency_hash() != 0, "the shipped set: %d values, hash %08x", consistency_count(), consistency_hash());
    for (int i = 0; i < consistency_count(); i++) {
        const char *cls = audit_class(consistency_name(i));
        CHECK(cls && !strncmp(cls, "LOCKED", 6), "%s is in the set but %s in the audit", consistency_name(i), cls ? cls : "missing");
        CHECK(strlen(consistency_note(i)) >= 8, "%s has a reason", consistency_name(i));
    }
    // The join check (MSG_HELLO).
    char why[160] = "";
    CHECK(consistency_join_ok(consistency_hash(), why, sizeof(why)), "the same set joins");
    CHECK(!consistency_join_ok(consistency_hash() ^ 1u, why, sizeof(why)) && strstr(why, "consistency set mismatch"), "another set is rejected (%s)", why);
    // The required visibility values.
    static const struct { const char *n; float v; } want[] = { { "r_foliage_density", 1.0f }, { "r_shadow_max_distance", 150.0f }, { "r_fog_scale", 1.0f },
                                                             { "r_lens_drops", 1.0f }, { "r_gamma", 1.0f }, { "r_stretch", 0.0f } };
    for (int i = 0; i < ARRAY_COUNT(want); i++) {
        int k = consistency_find(want[i].n);
        CHECK(k >= 0 && fabsf((float)atof(consistency_value(k)) - want[i].v) < 1e-4f, "%s = %g in the set", want[i].n, want[i].v);
    }
}

// ------------------------------------------------------------------ levels

#define MAXC 512
typedef struct Snap { int n; char name[MAXC][CVAR_NAME_MAX]; char str[MAXC][CVAR_STR_MAX]; u32 flags[MAXC]; } Snap;
static Snap g_snap[FID_LEVELS];

static void snapshot(Snap *s) {
    s->n = 0;
    for (int i = 0; i < cvar_count() && s->n < MAXC; i++) {
        const Cvar *c = cvar_at(i);
        dg_strcpy(s->name[s->n], CVAR_NAME_MAX, c->name);
        dg_strcpy(s->str[s->n], CVAR_STR_MAX, c->str);
        s->flags[s->n] = c->flags;
        s->n++;
    }
}

static void test_levels(void) {
    char err[256] = "";
    CHECK(fid_load_dir("assets/render/fidelity", err, sizeof(err)), "fidelity tables: %s", err);
    // Every fidelity feature cvar with its audited class (and its full range / Modern default).
    for (int f = 0; f < fid_feature_count(); f++) {
        const FidFeatureDef *d = fid_feature(f);
        const char *cls = audit_class(d->cvar);
        CHECK(cls && (!strcmp(cls, "TIER") || !strcmp(cls, "EDITABLE")), "fidelity feature %s is %s (must be TIER / EDITABLE)", d->cvar, cls ? cls : "missing");
        u32 fl = flags_of_class(cls);
        FidBounds b = fid_bounds(FID_MODERN, FID_ERA_MODERN, f);
        if (d->kind == FID_BOOL) cvar_reg_bool(d->cvar, b.def != 0, fl, d->label);
        else if (d->kind == FID_INT) cvar_reg_int(d->cvar, (int)b.def, (int)d->lo, (int)d->hi, fl, d->label);
        else cvar_reg_float(d->cvar, b.def, d->lo, d->hi, fl, d->label);
    }
    // Every consistency value (the engine registers them with their own types; strings do here).
    for (int i = 0; i < consistency_count(); i++)
        if (!cvar_find(consistency_name(i))) cvar_reg_string(consistency_name(i), "", flags_of_class(audit_class(consistency_name(i))), "a locked value");
    cvar_reg_string("r_fidelity_custom", "", CVAR_ARCHIVE | CVAR_EDITABLE, "user choices");
    fid_attach_cvars();
    cvar_set_release_locks(true);
    CHECK(consistency_apply() == 0, "every consistency entry is a registered locked cvar");
    for (int l = 0; l < FID_LEVELS; l++) {
        fid_apply(l);
        char rep[1024];
        CHECK(fid_check_table(l, rep, sizeof(rep)) == 0, "%s applied under the release locks: %s", fid_level_id(l), rep);
        CHECK(consistency_diff(rep, sizeof(rep)) == 0, "%s keeps every locked value: %s", fid_level_id(l), rep);
        snapshot(&g_snap[l]);
    }
    // Resolved cvar set per level: only TIER / EDITABLE fidelity features may differ.
    for (int l = 1; l < FID_LEVELS; l++) {
        int diff = 0, bad = 0;
        for (int i = 0; i < g_snap[0].n && i < g_snap[l].n; i++) {
            if (!strcmp(g_snap[0].str[i], g_snap[l].str[i])) continue;
            diff++;
            bool feature = fid_feature_by_cvar(g_snap[0].name[i]) >= 0;
            if (!feature || (g_snap[0].flags[i] & CVAR_LOCKED)) {
                bad++;
                printf("  %s: %s = %s at off, %s here\n", fid_level_id(l), g_snap[0].name[i], g_snap[0].str[i], g_snap[l].str[i]);
            }
        }
        CHECK(bad == 0, "off vs %s: %d cvars differ, %d of them not tier / editable features", fid_level_id(l), diff, bad);
    }
    // A player can't move a tier feature under the locks, Engine Fidelity can.
    Cvar *ao = cvar_find("r_ao");
    int before = ao->i;
    cvar_set("r_ao", before ? "0" : "1");
    CHECK(ao->i == before, "release: r_ao (tier) not writable by the player");
    fid_apply(FID_MODERN);
    cvar_set_release_locks(false);
}

// ------------------------------------------------------------------ the audit file

static void test_audit_places(void) {
    int n = 0, gfx = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, g_audit) {
        n++;
        const char *cls = cJSON_GetObjectItemCaseSensitive(e, "class")->valuestring, *ui = cJSON_GetObjectItemCaseSensitive(e, "ui")->valuestring;
        bool shown = strcmp(ui, "\xE2\x80\x94") && !strstr(ui, "(dev)") && strcmp(ui, "Lobby rules");
        if (shown) CHECK(!strcmp(cls, "EDITABLE"), "%s is shown in %s but %s", e->string, ui, cls);
        if (!strcmp(ui, "Lobby rules")) CHECK(!strcmp(cls, "LOCKED+RULE"), "%s is a lobby rule but %s", e->string, cls);
        if (!strncmp(ui, "Graphics", 8)) {
            gfx++;
            CHECK(!strcmp(ui, "Graphics (top)") || !strcmp(ui, "Graphics > Display") || !strcmp(ui, "Graphics > Comfort") || !strcmp(ui, "Graphics (dev)"),
                  "%s: Graphics place '%s' is not Engine Fidelity / Display / Comfort", e->string, ui);
        }
    }
    CHECK(n > 400 && gfx > 20, "audit: %d cvars, %d in the Graphics tab", n, gfx);
}

int main(void) {
    cvar_system_init();
    char *text = platform_read_file("assets/ui/settings_audit.json", NULL);
    g_audit_root = text ? cJSON_Parse(text) : NULL;
    free(text);
    g_audit = g_audit_root ? cJSON_GetObjectItemCaseSensitive(g_audit_root, "cvars") : NULL;
    CHECK(g_audit != NULL, "assets/ui/settings_audit.json (run from the build's bin directory)");
    if (!g_audit) return 1;
    test_locks();
    test_config();
    test_consistency();
    test_levels();
    test_audit_places();
    cJSON_Delete(g_audit_root);
    printf("test_settings: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
