// Hold point dev commands: holdpoint_reload / holdpoint_save / holdpoint_test.
//
// holdpoint_test equips every weapon in turn (hotbar slot 1) and cycles it through every hold
// point state, recording the first-person hand <-> socket error every frame (blends included) and
// the ADS sight alignment once the ADS blend has settled. Report: per weapon / state maxima and a
// PASS / FAIL line (hands < 2 mm / 1 deg, sight < 0.5 mm / 0.1 deg).
#include "game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/characters.h"
#include "anim/anim_library.h"
#include "ui/holdpoint_editor.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

static struct {
    bool active;
    int weapon, state, frame;
    float glue_pos[MAX_WEAPON_DEFS][HP_STATE_COUNT], glue_ang[MAX_WEAPON_DEFS][HP_STATE_COUNT];
    float ads_pos[MAX_WEAPON_DEFS], ads_ang[MAX_WEAPON_DEFS];
    InvSlot saved;
} T;

static const int k_settle = 45, k_sample = 30;

static void cmd_holdpoint_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    AnimLibrary *lib = g_engine.anim;
    if (!lib) return;
    char rel[300], path[600];
    snprintf(rel, sizeof(rel), "%s/holdpoint.json", lib->human_dir);
    size_t size = 0;
    char *txt = platform_read_file(platform_data_path(rel, path, sizeof(path)), &size);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    if (!root) { log_warn("holdpoint_reload: cannot parse %s", path); free(txt); return; }
    proc_holdpoint_load(&lib->proc, root);
    cJSON_Delete(root);
    free(txt);
    lib->manifest.hash[AC_HOLD_STYLES] = lib->proc.hash;
    log_info("holdpoint_reload: %s (styles hash %08x; peers must reload too)", path, lib->proc.hash);
}

bool holdpoint_save_all(const char *path_override) {
    AnimLibrary *lib = g_engine.anim;
    if (!lib) return false;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "_comment",
                            "Hold points: camera space (pos = right, up, forward m; rot = pitch, yaw, roll deg). ADS is computed from each item's sight "
                            "socket and eye_relief. Edited with holdpoint_editor (F8); see docs/CAMERA_HOLDPOINT.md.");
    cJSON *st = cJSON_AddObjectToObject(root, "styles");
    for (int i = 0; i < lib->proc.style_count; i++) {
        char *txt = holdpoint_style_to_json(&lib->proc.styles[i].hp);
        cJSON_AddItemToObject(st, lib->proc.styles[i].name, cJSON_Parse(txt));
        free(txt);
    }
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    char rel[300], path[600];
    snprintf(rel, sizeof(rel), "%s/holdpoint.json", lib->human_dir);
#ifdef DG_SOURCE_DIR
    if (!path_override) snprintf(path, sizeof(path), "%s/%s", DG_SOURCE_DIR, rel);
#else
    if (!path_override) platform_data_path(rel, path, sizeof(path));
#endif
    if (path_override) snprintf(path, sizeof(path), "%s", path_override);
    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) { fputs(out, f); fputc('\n', f); fclose(f); }
    // Runtime copy too (so a reload keeps the edit when running from the build).
    char rt[600];
    snprintf(rt, sizeof(rt), "%s%s", platform_base_dir(), rel);
    if (ok && strcmp(rt, path)) {
        FILE *g = fopen(rt, "wb");
        if (g) { fputs(out, g); fputc('\n', g); fclose(g); }
    }
    free(out);
    log_info("holdpoint_save: %s %s", ok ? "wrote" : "cannot write", path);
    return ok;
}

static void cmd_holdpoint_save(int argc, const char **argv) { holdpoint_save_all(argc > 1 ? argv[1] : NULL); }

static void equip(int wi) {
    Player *p = game_local_player();
    if (!p) return;
    const WeaponDef *wd = &G->items.weapons[wi];
    u16 item = 0;
    for (int i = 1; i < G->items.item_count; i++) if (G->items.items[i].weapon == wi) item = (u16)i;
    if (!item) item = item_find(&G->items, wd->id);
    InvSlot *s = inv_dev_hot(&p->inv, &G->items, 0);
    memset(s, 0, sizeof(*s));
    s->item = item;
    s->count = 1;
    s->ammo = (i16)wd->mag_size;
    p->inv.revision++;
    G->hotbar_slot = 0;
}

static void cmd_holdpoint_test(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Player *p = game_local_player();
    if (!p || G->mode != GM_HOST) { log_warn("holdpoint_test: needs a local host player"); return; }
    memset(&T, 0, sizeof(T));
    T.active = true;
    T.saved = *inv_hot(&p->inv, 0);
    equip(0);
    cvar_set_int(cvar_find("holdpoint_force_state"), 0);
    cvar_set_int(cvar_find("holdpoint_static"), 1);
    log_info("holdpoint_test: %d weapons x %d states", G->items.weapon_count, HP_STATE_COUNT);
}

static void report(void) {
    bool pass = true;
    for (int wi = 0; wi < G->items.weapon_count; wi++) {
        const WeaponDef *wd = &G->items.weapons[wi];
        char line[512];
        int n = snprintf(line, sizeof(line), "  %-9s", wd->id);
        float worst_p = 0, worst_a = 0;
        for (int s = 0; s < HP_STATE_COUNT; s++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %.2f/%.2f", holdpoint_state_name(s), T.glue_pos[wi][s] * 1000.0f, T.glue_ang[wi][s]);
            worst_p = fmaxf(worst_p, T.glue_pos[wi][s]);
            worst_a = fmaxf(worst_a, T.glue_ang[wi][s]);
        }
        bool gun = wd->kind != WPN_MELEE;
        if (gun) snprintf(line + n, sizeof(line) - (size_t)n, " | sight %.3f mm %.3f deg", T.ads_pos[wi] * 1000.0f, T.ads_ang[wi]);
        bool ok = worst_p < 0.002f && worst_a < 1.0f && (!gun || (T.ads_pos[wi] < 0.0005f && T.ads_ang[wi] < 0.1f));
        if (!ok) pass = false;
        log_info("%s%s", line, ok ? "" : "  <-- FAIL");
    }
    log_info("holdpoint_test: %s (hands < 2 mm / 1 deg every frame, sight < 0.5 mm / 0.1 deg)", pass ? "PASS" : "FAIL");
}

void game_holdpoint_test_frame(void) {
    if (!T.active) return;
    Player *p = game_local_player();
    if (!p) { T.active = false; return; }
    T.frame++;
    float gp[2], ga[2], ap, aa;
    chars_fp_diag(gp, ga, &ap, &aa);
    // Every frame after the equip settled (blends between states included).
    if (T.frame > 20 || T.state > 0) {
        float pe = fmaxf(gp[0], gp[1]), ae = fmaxf(ga[0], ga[1]);
        T.glue_pos[T.weapon][T.state] = fmaxf(T.glue_pos[T.weapon][T.state], pe);
        T.glue_ang[T.weapon][T.state] = fmaxf(T.glue_ang[T.weapon][T.state], ae);
    }
    if (T.state == HP_ADS && T.frame > k_settle) {
        T.ads_pos[T.weapon] = fmaxf(T.ads_pos[T.weapon], ap);
        T.ads_ang[T.weapon] = fmaxf(T.ads_ang[T.weapon], aa);
    }
    if (T.frame < k_settle + k_sample) return;
    T.frame = 0;
    if (++T.state >= HP_STATE_COUNT) {
        T.state = 0;
        if (++T.weapon >= G->items.weapon_count) {
            T.active = false;
            cvar_set_int(cvar_find("holdpoint_force_state"), -1);
            cvar_set_int(cvar_find("holdpoint_static"), 0);
            *inv_dev_hot(&p->inv, &G->items, 0) = T.saved;
            p->inv.revision++;
            report();
            return;
        }
        equip(T.weapon);
    }
    cvar_set_int(cvar_find("holdpoint_force_state"), T.state);
}

bool game_holdpoint_test_active(void) { return T.active; }

void game_holdpoint_register(void) {
    holdpoint_editor_init();
    cmd_register("holdpoint_reload", cmd_holdpoint_reload, "reload assets/human/holdpoint.json");
    cmd_register("holdpoint_save", cmd_holdpoint_save, "holdpoint_save [path]: write the hold point styles to assets/human/holdpoint.json");
    cmd_register("holdpoint_test", cmd_holdpoint_test, "cycle every weapon through every hold point state and report hand glue / sight alignment");
}
