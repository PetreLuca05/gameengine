// handpose_migrate: schema 1 hand poses -> universal grips + cases (docs/HANDPOSE.md "Versioning").
//   handpose_migrate [--dir assets/handposes] [--dry-run] [--keep]
// Run from the repository root. For every schema 1 file directly in the directory (and every
// schema 1 style in styles/):
//   * weapons / items / attachments / styles -> items/<id>.json (styles/<name>.json): the grip is
//     the "default" layer with "hip" on top (the hip version is kept); every other state layer that
//     differs from it is reported and dropped;
//   * interactables (use_*) / vehicles (veh_*) -> cases/<id>.json: one key at the contact marker
//     from "default" + "use" / "hold" ("drive"). When an authored case in cases/ is already bound
//     to the same thing (door_knob binds interactable:door), the old entry is superseded: its data
//     is reported (and merged into the authored case when that has no keys yet).
// The old files are removed afterwards (--keep leaves them). Exit 1 on a parse error.
#include "anim/handpose.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static char **list_json(const char *dir, int *count) {
    char **files = SDL_GlobDirectory(dir, "*.json", 0, count);
    if (files) qsort(files, (size_t)*count, sizeof(char *), cmp_str);
    return files;
}

static bool write_text(const char *path, char *txt, bool dry) {
    if (!txt) return false;
    bool ok = dry || platform_write_file(path, txt, strlen(txt));
    free(txt);
    return ok;
}

static int schema_of(const cJSON *root) {
    const cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema");
    return cJSON_IsNumber(sv) ? sv->valueint : 0;
}

int main(int argc, char **argv) {
    const char *dir = "assets/handposes";
    bool dry = false, keep = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--dry-run")) dry = true;
        else if (!strcmp(argv[i], "--keep")) keep = true;
        else { printf("usage: handpose_migrate [--dir assets/handposes] [--dry-run] [--keep]\n"); return 2; }
    }
    log_init(NULL);
    log_set_min_level(LOG_WARN);
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/items", dir);
    if (!dry) SDL_CreateDirectory(sub);
    snprintf(sub, sizeof(sub), "%s/cases", dir);
    if (!dry) SDL_CreateDirectory(sub);
    // Authored cases already in cases/ (their binds supersede the old interactable / vehicle entries).
    HandPoseDB cases;
    handpose_db_init(&cases);
    int nc = 0;
    char **cf = list_json(sub, &nc);
    for (int i = 0; cf && i < nc; i++) {
        char path[900], err[200];
        snprintf(path, sizeof(path), "%s/%s", sub, cf[i]);
        if (!handpose_load_file(&cases, path, err, sizeof(err))) printf("  ! %s\n", err);
    }
    SDL_free(cf);
    int files = 0, grips = 0, ncase = 0, superseded = 0, diffs = 0, errors = 0;
    printf("handpose_migrate: %s%s\n", dir, dry ? " (dry run)" : "");
    for (int pass = 0; pass < 2; pass++) {
        char d[600];
        snprintf(d, sizeof(d), pass ? "%s/styles" : "%s", dir);
        int n = 0;
        char **list = list_json(d, &n);
        for (int i = 0; list && i < n; i++) {
            char path[900];
            snprintf(path, sizeof(path), "%s/%s", d, list[i]);
            char *text = platform_read_file(path, NULL);
            if (!text) continue;
            cJSON *root = cJSON_Parse(text);
            free(text);
            if (!root) { printf("  ! %s: JSON parse error\n", path); errors++; continue; }
            if (schema_of(root) >= 2) { cJSON_Delete(root); continue; }
            files++;
            char id[64];
            snprintf(id, sizeof(id), "%s", list[i]);
            char *dot = strrchr(id, '.');
            if (dot) *dot = 0;
            const char *cat = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "category")) ? cJSON_GetObjectItemCaseSensitive(root, "category")->valuestring : "";
            HandPoseMigrateNote note;
            bool ok = true;
            if (!pass && (!strcmp(cat, "vehicle") || !strcmp(cat, "interactable"))) {
                HandPoseCase c;
                handpose_case_from_v1(&c, root, id, &note);
                int owner = handpose_case_find_bind(&cases, c.bind);
                if (owner >= 0 && strcmp(cases.c[owner].id, c.id)) {
                    HandPoseCase *a = &cases.c[owner];
                    superseded++;
                    printf("  %-22s -> superseded by cases/%s.json (%s)%s\n", id, a->id, c.bind, c.key_count ? "" : ": no pose data");
                    if (c.key_count && !a->key_count) {
                        a->keys[0] = c.keys[0];
                        a->key_count = 1;
                        snprintf(path, sizeof(path), "%s/cases/%s.json", dir, a->id);
                        ok = write_text(path, handpose_case_write(a), dry);
                        printf("  %-22s    its key moved into %s\n", "", a->id);
                    }
                } else if (owner >= 0) {
                    printf("  %-22s -> cases/%s.json exists (authored): kept, old data %s\n", id, c.id, c.key_count ? "dropped" : "empty");
                    superseded++;
                } else {
                    snprintf(path, sizeof(path), "%s/cases/%s.json", dir, c.id);
                    ok = write_text(path, handpose_case_write(&c), dry);
                    printf("  %-22s -> cases/%s.json (%s, %d key%s)\n", id, c.id, c.bind, c.key_count, c.key_count == 1 ? "" : "s");
                    ncase++;
                }
            } else {
                HandPoseEntry *e = (HandPoseEntry *)calloc(1, sizeof(HandPoseEntry));
                snprintf(e->id, sizeof(e->id), "%s%s", pass ? "style/" : "", id);
                cJSON_DeleteItemFromObjectCaseSensitive(root, "id");
                char err[200];
                if (!handpose_entry_parse(e, root, err, sizeof(err), &note)) { printf("  ! %s: %s\n", path, err); errors++; free(e); cJSON_Delete(root); continue; }
                if (pass) e->category = HPC_STYLE;
                char out[900];
                handpose_entry_rel_path(e, dir, out, sizeof(out));
                ok = write_text(out, handpose_entry_write(e), dry);
                printf("  %-22s -> %s%s\n", e->id, out + strlen(dir) + 1, handpose_entry_has_overrides(e) ? "" : " (stub)");
                grips++;
                free(e);
            }
            if (note.text[0]) { printf("  %-22s    differences: %s\n", "", note.text); diffs += note.dropped; }
            if (!ok) { printf("  ! cannot write the migrated %s\n", id); errors++; }
            cJSON_Delete(root);
        }
        SDL_free(list);
    }
    // The old flat files go (styles were rewritten in place).
    if (!keep && !dry && !errors) {
        int n = 0;
        char **list = list_json(dir, &n);
        for (int i = 0; list && i < n; i++) {
            char path[900];
            snprintf(path, sizeof(path), "%s/%s", dir, list[i]);
            char *text = platform_read_file(path, NULL);
            cJSON *root = text ? cJSON_Parse(text) : NULL;
            free(text);
            if (root && schema_of(root) < 2) remove(path);
            cJSON_Delete(root);
        }
        SDL_free(list);
    }
    printf("handpose_migrate: %d schema 1 files: %d grips / styles, %d cases, %d superseded by authored cases, %d differing state layers dropped, %d errors\n",
           files, grips, ncase, superseded, diffs, errors);
    handpose_db_free(&cases);
    return errors ? 1 : 0;
}
