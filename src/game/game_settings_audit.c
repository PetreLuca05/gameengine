// settings_audit (docs/SETTINGS_AUDIT.md): the classification of every registered cvar. Class from
// the registration flags (CVAR_EDITABLE / CVAR_TIER / CVAR_LOCKED / CVAR_RULE), the reason and the
// settings-UI place from assets/ui/settings_audit.json, the category from command_categories.json,
// "locked" = writes a player makes are rejected right now (release locks). The same table is
// generated into docs/SETTINGS_AUDIT.md by tools/tests/lint_settings.py.
#include "game_internal.h"
#include "core/cmd_registry.h"
#include "core/consistency.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/lod_fade.h"
#include "render/fidelity/fidelity.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

#define AUDIT_PATH "assets/ui/settings_audit.json"

static void cmd_settings_audit(int argc, const char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    char *text = platform_read_file(AUDIT_PATH, NULL);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *audit = cJSON_GetObjectItemCaseSensitive(root, "cvars");
    if (!audit) log_warn("settings_audit: cannot read %s (reasons / UI places missing)", AUDIT_PATH);
    static CmdRegistryCfg cats;
    char *ct = platform_read_file("assets/ui/command_categories.json", NULL);
    char err[128];
    bool have_cats = ct && cmdreg_parse(&cats, ct, err, sizeof(err));
    free(ct);
    int n = 0, by_class[5] = { 0 }, unclassified = 0, mismatch = 0, missing = 0, shown = 0;
    log_info("settings_audit: name | category | class | UI | locked now | reason");
    for (int i = 0; i < cvar_count(); i++) {
        Cvar *c = cvar_at(i);
        n++;
        const char *cls = cvar_class_name(c->flags);
        u32 k = c->flags & CVAR_CLASS_MASK;
        int ci = k == CVAR_EDITABLE ? 0 : (k == CVAR_TIER ? 1 : (k == CVAR_LOCKED ? ((c->flags & CVAR_RULE) ? 3 : 2) : 4));
        by_class[ci]++;
        if (ci == 4) unclassified++;
        const cJSON *e = audit ? cJSON_GetObjectItemCaseSensitive(audit, c->name) : NULL;
        const cJSON *jc = cJSON_GetObjectItemCaseSensitive(e, "class"), *ju = cJSON_GetObjectItemCaseSensitive(e, "ui"), *jr = cJSON_GetObjectItemCaseSensitive(e, "reason");
        if (!e) missing++;
        else if (!cJSON_IsString(jc) || strcmp(jc->valuestring, cls)) {
            mismatch++;
            log_warn("settings_audit: %s is %s, %s says %s", c->name, cls, AUDIT_PATH, cJSON_IsString(jc) ? jc->valuestring : "?");
        }
        if (filter && !strstr(c->name, filter) && strcmp(filter, cls)) continue;
        int cat = have_cats ? cmdreg_category(&cats, c->name) : -1;
        bool locked = !cvar_writable(c, CVAR_SRC_USER);
        log_info("  %-28s | %-9s | %-11s | %-22s | %-3s | %s", c->name, cat >= 0 ? cats.cats[cat].id : "?", cls, cJSON_IsString(ju) ? ju->valuestring : "-", locked ? "yes" : "no",
                 cJSON_IsString(jr) ? jr->valuestring : "(no audit entry)");
        shown++;
    }
    char rep[512];
    int dev = consistency_check(rep, sizeof(rep));
    log_info("settings_audit: %d cvars (%d shown): %d EDITABLE, %d TIER, %d LOCKED, %d LOCKED+RULE, %d unclassified; %d without an audit entry, %d class mismatches; "
             "release locks %s; consistency set %d values, hash %08x (effective %08x)%s%s",
             n, shown, by_class[0], by_class[1], by_class[2], by_class[3], unclassified, missing, mismatch, cvar_release_locks() ? "on" : "off (dev build)",
             consistency_count(), consistency_hash(), consistency_hash_effective(), dev ? ", changed: " : "", dev ? rep : "");
    cJSON_Delete(root);
}

// settings_lock_test <cvar> <value>: try a player write and report the result (tools/tests/settings_locks.cfg).
static void cmd_settings_lock_test(int argc, const char **argv) {
    if (argc < 3) { log_info("usage: settings_lock_test <cvar> <value>"); return; }
    Cvar *c = cvar_find(argv[1]);
    if (!c) { log_warn("settings_lock_test: no cvar %s", argv[1]); return; }
    char before[CVAR_STR_MAX];
    dg_strcpy(before, sizeof(before), c->str);
    int rej = cvar_rejected_count();
    cvar_set_cv(c, argv[2]);
    bool rejected = cvar_rejected_count() > rej;
    log_info("settings_lock_test: %s (%s) %s -> %s: %s", c->name, cvar_class_name(c->flags), before, c->str, rejected ? "rejected" : "accepted");
}

// settings_visibility: what this frame shows, for the per-level comparison
// (tools/tests/run_settings_tests.py): foliage / instance counts, the fog density every path uses,
// shadow distance and types, LOD scales, precipitation, the locked density / impostor switch.
static float cvf(const char *n) {
    Cvar *c = cvar_find(n);
    return c ? c->f : -1.0f;
}

static void cmd_settings_visibility(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    u32 vis = 0, faded = 0, imp = 0;
    bool have = render_visibility_counts(&vis, &faded, &imp);
    // Two lines (LOG_LINE_MAX): the second, "settings_visibility+", continues the first.
    log_info("settings_visibility: level=%s era=%d vgeo=%d counts=%d visible=%u faded=%u impostors=%u fog=%.6f shadow_distance=%g shadows=%g", fid_level_id(fid_level()),
             (int)cvf("r_era"), (int)cvf("r_vgeo"), have ? 1 : 0, vis, faded, imp, (double)game_post_fog_density(), (double)cvf("r_shadow_max_distance"), (double)cvf("r_shadows"));
    log_info("settings_visibility+ light_shadows=%g cloud_shadows=%g lens_drops=%g auto_exposure=%g foliage_density=%g impostor_px=%g lod_bias=%g", (double)cvf("r_light_shadows"),
             (double)cvf("r_cloud_shadows"), (double)cvf("r_lens_drops"), (double)cvf("r_auto_exposure"), (double)cvf("r_foliage_density"), (double)cvf("r_vgeo_impostor_px"),
             (double)cvf("r_lod_bias"));
    log_info("settings_visibility+ lod_static=%g lod_char=%g lod_foliage=%g precip=%g water=%g vfx_decals=%g", (double)lod_scale(LODC_STATIC), (double)lod_scale(LODC_CHARACTER),
             (double)lod_scale(LODC_FOLIAGE), (double)cvf("r_precip_quality"), (double)cvf("r_water"), (double)cvf("vfx_decal_max"));
}

void settings_audit_register(void) {
    cmd_register("settings_visibility", cmd_settings_visibility, "settings_visibility: this frame's foliage counts, fog density, shadow distance, LOD scales (compared per Engine Fidelity level by tools/tests/run_settings_tests.py)");
    cmd_register("settings_audit", cmd_settings_audit, "settings_audit [filter|class]: the classification of every cvar (EDITABLE / TIER / LOCKED / LOCKED+RULE, reason, UI, locked now; docs/SETTINGS_AUDIT.md)");
    cmd_register("settings_lock_test", cmd_settings_lock_test, "settings_lock_test <cvar> <value>: a player write of a cvar and whether the settings locks rejected it (tests)");
}
