// Settings audit unit tests (docs/SETTINGS_AUDIT.md), on the real cvar system, the real consistency
// set and the real fidelity tables:
//   * release locks: a player's write of a TIER / LOCKED cvar is rejected (value kept, counted),
//     EDITABLE always works, Engine Fidelity writes TIER, the host writes LOCKED, a host rule follows
//     the rule gate (only the host / an offline player sets it); dev builds write everything;
//   * config.cfg: config_save leaves TIER / LOCKED cvars out, config_load_file drops them;
//   * the consistency set: loads, applies (value + default), normalizes, hashes stably; an edited
//     value changes the effective hash (rules don't), the self-check reports it, an entry naming a
//     non-locked cvar is an error; the join check rejects a mismatch or a missing hash;
//   * every Engine Fidelity level x era, with the locks on, resolves the same value for every locked
//     cvar (the resolved set diffed per tier) and the visibility floors hold.
#include "core/cvar.h"
#include "core/consistency.h"
#include "core/engine.h"
#include "core/platform.h"
#include "render/fidelity/fidelity.h"
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

static bool g_client;   // the rule gate: connected to someone else's server
static bool rule_gate(void) { return !g_client; }

static bool wrote(Cvar *c, const char *v, CvarSource src) {
    CvarSource prev = cvar_source_push(src);
    int rej = cvar_rejected_count();
    cvar_set_cv(c, v);
    cvar_source_pop(prev);
    return cvar_rejected_count() == rej && !strcmp(c->str, v);
}

static void test_locks(void) {
    Cvar *e = cvar_reg_float("t_editable", 1.0f, 0, 10, CVAR_ARCHIVE | CVAR_EDITABLE, "a player setting");
    Cvar *t = cvar_reg_int("t_tier", 2, 0, 3, CVAR_TIER, "a fidelity tier");
    Cvar *l = cvar_reg_float("t_locked", 1.0f, 0, 10, CVAR_LOCKED, "a locked value");
    Cvar *r = cvar_reg_float("t_rule", 1.0f, 0, 10, CVAR_ARCHIVE | CVAR_LOCKED | CVAR_RULE, "a host rule");
    cvar_set_rule_gate(rule_gate);
    CHECK(!strcmp(cvar_class_name(e->flags), "EDITABLE") && !strcmp(cvar_class_name(t->flags), "TIER") && !strcmp(cvar_class_name(l->flags), "LOCKED") &&
              !strcmp(cvar_class_name(r->flags), "LOCKED+RULE"),
          "class names");
    // Dev build (the test binary has DG_DEV_BUILD 1): every write goes through.
    CHECK(cvar_dev_build() && !cvar_release_locks(), "dev build, locks off by default");
    CHECK(wrote(t, "1", CVAR_SRC_USER) && wrote(l, "2", CVAR_SRC_USER), "dev build: a player may write TIER / LOCKED cvars");
    // Release locks.
    cvar_set_release_locks(true);
    CHECK(cvar_release_locks(), "locks on");
    CHECK(wrote(e, "3", CVAR_SRC_USER), "EDITABLE: a player write works");
    CHECK(!wrote(t, "3", CVAR_SRC_USER) && t->i == 1, "TIER: a player write is rejected (kept %d)", t->i);
    CHECK(wrote(t, "3", CVAR_SRC_FIDELITY), "TIER: Engine Fidelity writes it");
    CHECK(!wrote(l, "5", CVAR_SRC_USER) && fabsf(l->f - 2.0f) < 1e-6f, "LOCKED: a player write is rejected (kept %g)", l->f);
    CHECK(!wrote(l, "5", CVAR_SRC_FIDELITY), "LOCKED: Engine Fidelity can't write it either");
    CHECK(wrote(l, "1", CVAR_SRC_HOST), "LOCKED: the host / consistency set writes it");
    CHECK(!cvar_writable(l, CVAR_SRC_USER) && cvar_writable(l, CVAR_SRC_HOST) && cvar_writable(e, CVAR_SRC_USER), "cvar_writable agrees");
    g_client = false;
    CHECK(wrote(r, "0.5", CVAR_SRC_USER), "RULE: the host (or offline) sets its rules");
    g_client = true;
    CHECK(!wrote(r, "0.8", CVAR_SRC_USER), "RULE: a client can't");
    CHECK(wrote(r, "0.9", CVAR_SRC_HOST), "RULE: the host's replicated value lands on the client");
    g_client = false;
    // The console path (cmd_execute_line -> cvar_set_cv) is a player write.
    cmd_execute_line("t_locked 7");
    CHECK(fabsf(l->f - 1.0f) < 1e-6f, "console: 't_locked 7' rejected");
    cmd_execute_line("set t_tier 0");
    CHECK(t->i == 3, "console: 'set t_tier 0' rejected");
    cmd_execute_line("reset t_locked");
    CHECK(fabsf(l->f - 1.0f) < 1e-6f, "console: reset of a locked cvar is a write too (rejected, value unchanged)");

    // config.cfg.
    config_set_enabled(true);
    const char *path = "test_settings_config.cfg";
    CHECK(config_save(path), "config_save");
    char *txt = platform_read_file(path, NULL);
    CHECK(txt && strstr(txt, "t_editable") && strstr(txt, "t_rule") && !strstr(txt, "t_tier") && !strstr(txt, "t_locked"), "config.cfg: player settings + host rules only");
    free(txt);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("set t_editable \"4\"\nset t_tier \"0\"\nset t_locked \"9\"\nset t_rule \"0.7\"\n", f);
        fclose(f);
    }
    CHECK(config_load_file(path) && config_load_dropped() == 2, "config_load_file drops the TIER / LOCKED entries (%d)", config_load_dropped());
    CHECK(fabsf(e->f - 4.0f) < 1e-6f && t->i == 3 && fabsf(l->f - 1.0f) < 1e-6f && fabsf(r->f - 0.7f) < 1e-6f, "config: editable %g tier %d locked %g rule %g", e->f, t->i, l->f, r->f);
    cvar_set_release_locks(false);
    CHECK(config_load_file(path) && config_load_dropped() == 2 && t->i == 3, "dev builds drop them from the config too (they're never saved)");
    remove(path);
    remove("test_settings_config.cfg.bak");
    config_set_enabled(false);
}

static void test_consistency(void) {
    cvar_set_release_locks(true);
    Cvar *a = cvar_reg_bool("t_c_bool", false, CVAR_LOCKED, "a locked switch");
    Cvar *b = cvar_reg_float("t_c_float", 2.0f, 0, 100, CVAR_LOCKED, "a locked distance");
    Cvar *r = cvar_reg_float("t_c_rule", 1.0f, 0, 1, CVAR_LOCKED | CVAR_RULE, "a locked rule");
    const char *json = "{\"values\": {\"t_c_float\": {\"value\": 150, \"why\": \"distance\"}, \"t_c_bool\": {\"value\": true, \"why\": \"on\"},"
                       " \"t_c_rule\": {\"value\": 1.0, \"why\": \"rule\"}}}";
    char err[200] = "";
    CHECK(consistency_load_text(json, err, sizeof(err)) && consistency_count() == 3, "loads: %s", err);
    CHECK(consistency_apply(err, sizeof(err)) == 0, "applies: %s", err);
    CHECK(a->i == 1 && fabsf(b->f - 100.0f) < 1e-6f && !strcmp(b->default_str, b->str), "applied (clamped to the range, %g) and the default follows", b->f);
    CHECK(!strcmp(consistency_find("t_c_float"), "100"), "the stored value is normalized by the cvar (%s)", consistency_find("t_c_float"));
    u32 h = consistency_hash(), eff = consistency_hash_effective();
    CHECK(h == consistency_hash() && eff == consistency_hash_effective(), "hashes are stable");
    // Order-independent (sorted).
    const char *json2 = "{\"values\": {\"t_c_rule\": {\"value\": 1.0, \"why\": \"rule\"}, \"t_c_bool\": {\"value\": 1, \"why\": \"on\"},"
                        " \"t_c_float\": {\"value\": 150, \"why\": \"distance\"}}}";
    CHECK(consistency_load_text(json2, err, sizeof(err)) && consistency_apply(err, sizeof(err)) == 0 && consistency_hash() == h, "the hash ignores the file's order");
    // A rule changes nothing; a locked edit (dev console) changes the effective hash and the check.
    CvarSource prev = cvar_source_push(CVAR_SRC_HOST);
    cvar_set_cv(r, "0.5");
    CHECK(consistency_hash_effective() == eff, "a host rule is not part of the effective hash");
    cvar_set_cv(b, "50");
    cvar_source_pop(prev);
    char rep[256];
    CHECK(consistency_hash_effective() != eff && consistency_hash() == h, "an edited locked value changes the effective hash, not the file's");
    CHECK(consistency_check(rep, sizeof(rep)) == 1 && strstr(rep, "t_c_float"), "self-check: %s", rep);
    char why[200];
    CHECK(consistency_join_ok(eff, eff, false, why, sizeof(why)), "join: equal hashes");
    CHECK(!consistency_join_ok(eff, consistency_hash_effective(), false, why, sizeof(why)) && strstr(why, "consistency set mismatch"), "join: a different set is rejected (%s)", why);
    CHECK(!consistency_join_ok(eff, eff, true, why, sizeof(why)), "join: no hash (an older client) is rejected");
    // Errors.
    Cvar *ed = cvar_reg_float("t_c_editable", 1.0f, 0, 2, CVAR_EDITABLE, "not locked");
    (void)ed;
    CHECK(consistency_load_text("{\"values\": {\"t_c_editable\": {\"value\": 1, \"why\": \"x\"}}}", err, sizeof(err)) && consistency_apply(err, sizeof(err)) == 1 && strstr(err, "not CVAR_LOCKED"),
          "an entry naming an editable cvar: %s", err);
    CHECK(!consistency_load_text("{\"values\": {\"t_c_bool\": {\"value\": 1}}}", err, sizeof(err)), "an entry without a why: %s", err);
    CHECK(!consistency_load_text("{\"values\": [1]}", err, sizeof(err)) || consistency_count() == 0, "malformed");
    // The real set.
    CHECK(consistency_load(CONSISTENCY_PATH) && consistency_count() >= 40, "%s: %d values", CONSISTENCY_PATH, consistency_count());
    cvar_set_release_locks(false);
}

// Every fidelity level x era with the locks on: fidelity writes its tiers, never a locked value.
static void test_tiers(void) {
    char err[256] = "";
    CHECK(consistency_load(CONSISTENCY_PATH), "consistency set");
    CHECK(fid_load_dir("assets/render/fidelity", err, sizeof(err)), "fidelity tables: %s", err);
    for (int f = 0; f < fid_feature_count(); f++) {
        const FidFeatureDef *d = fid_feature(f);
        FidBounds b = fid_bounds(FID_MODERN, FID_ERA_MODERN, f);
        u32 flags = CVAR_TIER;
        if (fid_feature_locked(f)) flags = CVAR_LOCKED;
        if (d->kind == FID_BOOL) cvar_reg_bool(d->cvar, b.def != 0, flags, d->label);
        else if (d->kind == FID_INT) cvar_reg_int(d->cvar, (int)b.def, (int)d->lo, (int)d->hi, flags, d->label);
        else cvar_reg_float(d->cvar, b.def, d->lo, d->hi, flags, d->label);
    }
    Cvar *era = cvar_reg_int("r_era", 0, 0, 4, CVAR_EDITABLE, "era");
    cvar_reg_string("r_fidelity_custom", "", CVAR_EDITABLE, "user choices");
    // Every locked cvar of the consistency set that exists here: the set's value.
    int applied_errors = consistency_apply(err, sizeof(err));
    (void)applied_errors;   // the set names cvars this test doesn't register
    fid_attach_cvars();
    cvar_set_release_locks(true);
    int rej0 = cvar_rejected_count();
    char ref[64][CVAR_STR_MAX], names[64][CVAR_NAME_MAX];
    int nref = 0;
    for (int e = 0; e < FID_ERAS; e++) {
        cvar_set_int(era, e);
        for (int l = 0; l < FID_LEVELS; l++) {
            fid_apply((l + 3) % FID_LEVELS);
            fid_apply(l);
            int k = 0;
            for (int i = 0; i < cvar_count(); i++) {
                Cvar *c = cvar_at(i);
                if (!(c->flags & CVAR_LOCKED) || strncmp(c->name, "r_", 2) && strncmp(c->name, "vfx_", 4) && strncmp(c->name, "ads_", 4)) continue;
                if (k >= 64) break;
                if (e == 0 && l == 0) {
                    dg_strcpy(names[k], CVAR_NAME_MAX, c->name);
                    dg_strcpy(ref[k], CVAR_STR_MAX, c->str);
                    nref = k + 1;
                } else {
                    CHECK(!strcmp(names[k], c->name) && !strcmp(ref[k], c->str), "era %s level %s: %s = %s, %s at modern / off", fid_era_id(e), fid_level_id(l), c->name, c->str, ref[k]);
                }
                const char *cs = consistency_find(c->name);
                if (cs) CHECK(!strcmp(cs, c->str), "%s/%s: %s = %s, the consistency set says %s", fid_era_id(e), fid_level_id(l), c->name, c->str, cs);
                k++;
            }
            const char *floors[] = { "r_shadows", "r_water", "r_precip_quality" };
            for (int i = 0; i < 3; i++) {
                Cvar *c = cvar_find(floors[i]);
                CHECK(c && c->i >= 1, "%s/%s: %s = %d (>= 1)", fid_era_id(e), fid_level_id(l), floors[i], c ? c->i : -1);
            }
        }
    }
    CHECK(nref >= 15, "%d locked render cvars diffed per tier", nref);
    CHECK(cvar_rejected_count() == rej0, "Engine Fidelity never wrote a locked cvar (%d rejections)", cvar_rejected_count() - rej0);
    cvar_set_release_locks(false);
}

int main(void) {
    cvar_system_init();
    test_locks();
    test_consistency();
    test_tiers();
    printf("test_settings: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
