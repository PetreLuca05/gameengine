// Settings audit tools (docs/SETTINGS_AUDIT.md):
//   settings_audit [filter] [locked|tier|editable|rule|ui]: the classification table of every
//     registered cvar (name, category, class from the registration flags, place in the settings UI,
//     locked in release / now, reason from assets/ui/settings_audit.json) and a summary; flags a cvar
//     whose flags disagree with the audit file or that has no entry.
//   sv_rule [cvar value]: the host's server rules (CVAR_LOCKED | CVAR_RULE): lists them, or sets one
//     for the session (replicated to every client; refused on a client).
//   consistency_check: the consistency set (assets/render/consistency.json) against the cvars now.
//   visibility_probe [tag]: one line of what decides visibility this frame (foliage density and the
//     visible / faded / impostor instance counts, fog extinction, shadow distance, LOD / impostor /
//     grass ranges, droplets, decals): tools/tests/run_settings_tests.py compares it per level.
#include "game_internal.h"
#include "core/consistency.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/lod_fade.h"
#include "render/fidelity/fidelity.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIT_PATH "assets/ui/settings_audit.json"

static bool match_kind(const Cvar *c, const char *kind) {
    if (!kind) return true;
    if (!strcmp(kind, "locked")) return (c->flags & CVAR_LOCKED) && !(c->flags & CVAR_RULE);
    if (!strcmp(kind, "rule")) return (c->flags & CVAR_RULE) != 0;
    if (!strcmp(kind, "tier")) return (c->flags & CVAR_TIER) != 0;
    if (!strcmp(kind, "editable")) return (c->flags & CVAR_EDITABLE) != 0;
    return true;
}

static void cmd_settings_audit(int argc, const char **argv) {
    char *text = platform_read_file(AUDIT_PATH, NULL);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *all = root ? cJSON_GetObjectItemCaseSensitive(root, "cvars") : NULL;
    if (!all) log_warn("settings_audit: %s missing or unreadable (reasons / UI places unknown)", AUDIT_PATH);
    const char *filter = NULL, *kind = NULL;
    bool ui_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "locked") || !strcmp(argv[i], "tier") || !strcmp(argv[i], "editable") || !strcmp(argv[i], "rule")) kind = argv[i];
        else if (!strcmp(argv[i], "ui")) ui_only = true;
        else filter = argv[i];
    }
    int count[5] = { 0 }, bad = 0, shown = 0;
    log_info("settings_audit: %-30s %-10s %-11s %-24s %-8s %s", "cvar", "category", "class", "settings UI", "locked", "reason");
    for (int i = 0; i < cvar_count(); i++) {
        const Cvar *c = cvar_at(i);
        const char *cls = cvar_class_name(c->flags);
        int k = !strcmp(cls, "EDITABLE") ? 0 : !strcmp(cls, "TIER") ? 1 : !strcmp(cls, "LOCKED") ? 2 : !strcmp(cls, "LOCKED+RULE") ? 3 : 4;
        count[k]++;
        const cJSON *e = all ? cJSON_GetObjectItemCaseSensitive(all, c->name) : NULL;
        const cJSON *jc = e ? cJSON_GetObjectItemCaseSensitive(e, "class") : NULL, *ju = e ? cJSON_GetObjectItemCaseSensitive(e, "ui") : NULL;
        const cJSON *jr = e ? cJSON_GetObjectItemCaseSensitive(e, "reason") : NULL, *jg = e ? cJSON_GetObjectItemCaseSensitive(e, "category") : NULL;
        const char *ui = cJSON_IsString(ju) ? ju->valuestring : "?";
        bool in_ui = ui[0] && strcmp(ui, "\xE2\x80\x94") && strcmp(ui, "?");
        char note[48] = "";
        if (k == 4) snprintf(note, sizeof(note), " [NO CLASS FLAG]");
        else if (!e) snprintf(note, sizeof(note), " [NOT IN THE AUDIT]");
        else if (!cJSON_IsString(jc) || strcmp(jc->valuestring, cls)) snprintf(note, sizeof(note), " [AUDIT SAYS %s]", cJSON_IsString(jc) ? jc->valuestring : "?");
        if (note[0]) bad++;
        if (filter && !strstr(c->name, filter)) continue;
        if (!match_kind(c, kind) || (ui_only && !in_ui)) continue;
        const char *locked = (c->flags & CVAR_TIER) ? "fidelity" : (c->flags & CVAR_RULE) ? "host" : (c->flags & CVAR_LOCKED) ? "yes" : "no";
        log_info("  %-30s %-10s %-11s %-24s %-8s %s%s", c->name, cJSON_IsString(jg) ? jg->valuestring : "?", cls, ui, locked, cJSON_IsString(jr) ? jr->valuestring : "", note);
        shown++;
    }
    log_info("settings_audit: %d cvars (%d shown): %d EDITABLE, %d TIER, %d LOCKED, %d LOCKED+RULE, %d without a class; %d disagree with %s; release locks %s, %d writes rejected",
             cvar_count(), shown, count[0], count[1], count[2], count[3], count[4], bad, AUDIT_PATH, cvar_release_locks() ? "ON" : "off (dev build)", cvar_rejected_count());
    cJSON_Delete(root);
}

static void cmd_sv_rule(int argc, const char **argv) {
    if (argc < 3) {
        log_info("sv_rule <cvar> <value>: the host's server rules (replicated to every player):");
        for (int i = 0; i < cvar_count(); i++) {
            const Cvar *c = cvar_at(i);
            if (c->flags & CVAR_RULE) log_info("  %-28s = %-8s (default %s) %s", c->name, c->str, c->default_str, c->desc);
        }
        return;
    }
    Cvar *c = cvar_find(argv[1]);
    if (!c || !(c->flags & CVAR_RULE)) { log_warn("sv_rule: %s is not a server rule (settings_audit rule lists them)", argv[1]); return; }
    if (!cvar_writable(c, CVAR_SRC_USER)) { log_warn("sv_rule: only the host sets the server rules"); return; }
    cvar_set_cv(c, argv[2]);
    log_info("sv_rule: %s = %s (replicated to every player)", c->name, c->str);
}

static void cmd_consistency_check(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    char rep[1024];
    int d = consistency_diff(rep, sizeof(rep));
    log_info("consistency_check: %d shared values, hash %08x: %s%s", consistency_count(), consistency_hash(), d ? "differ: " : "every value as listed", d ? rep : "");
}

static float cvf(const char *n) {
    Cvar *c = cvar_find(n);
    return c ? c->f : -1.0f;
}

static void cmd_visibility_probe(int argc, const char **argv) {
    u32 vis = 0, faded = 0, imp = 0;
    bool have = render_vgeo_counts(&vis, &faded, &imp);
    float gs = 0, ge = 0;
    int lvl = fid_loaded() ? fid_level() : FID_MODERN;
    render_grass_fade(lvl, &gs, &ge);
    log_info("visibility_probe %s: level=%s foliage_density=%g instances=%u faded=%u impostors=%u counted=%d fog=%.6f shadow_distance=%g impostor_px=%g classic_impostor_px=%g "
             "lod_px=%g error_px=%g",
             argc > 1 ? argv[1] : "-", fid_level_id(lvl), cvf("r_foliage_density"), vis, faded, imp, have ? 1 : 0, render_fog_extinction(), cvf("r_shadow_max_distance"),
             cvf("r_vgeo_impostor_px"), cvf("r_classic_impostor_px"), cvf("r_vgeo_lod_px"), cvf("r_vgeo_error_px"));
    // (two lines: the log truncates long ones)
    log_info("visibility_probe %s+ lod_static=%g lod_foliage=%g lod_impostor=%g grass_end=%g lens_drops=%g decal_max=%g chunk_radius=%g consistency=%08x", argc > 1 ? argv[1] : "-",
             lod_scale(LODC_STATIC), lod_scale(LODC_FOLIAGE), lod_scale(LODC_IMPOSTOR), ge, cvf("r_lens_drops"), cvf("vfx_decal_max"), cvf("world_chunk_radius"), consistency_hash());
}

void game_settings_audit_init(void) {
    cmd_register("visibility_probe", cmd_visibility_probe, "visibility_probe [tag]: one line of the visibility outcomes this frame (foliage, fog, shadow / LOD ranges; run_settings_tests.py)");
    cmd_register("settings_audit", cmd_settings_audit, "settings_audit [filter] [locked|tier|editable|rule|ui]: classification of every cvar (docs/SETTINGS_AUDIT.md)");
    cmd_register("sv_rule", cmd_sv_rule, "sv_rule [cvar value]: list / set the host's server rules (foliage density, draw distance ...; replicated)");
    cmd_register("consistency_check", cmd_consistency_check, "consistency_check: the shared values (assets/render/consistency.json) against the cvars now");
}
