// Building kit validator (docs/BUILDING_KIT.md "Conventions"): every assets/kits/building/*.glb (or the
// pieces named on the command line) against the kit's rules (kit.json categories) and material sets
// (materials/*.json). Prints "<piece>: <problem> - <fix>" per problem; exit 1 if any.
//
//   validate_kit [--kit <dir>] [piece.glb ...]
#include "assets/kit_piece.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

typedef struct Files { char (*names)[128]; int n, cap; } Files;

static void add_file(Files *f, const char *name) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 256;
        f->names = realloc(f->names, sizeof(*f->names) * (size_t)f->cap);
    }
    snprintf(f->names[f->n++], 128, "%s", name);
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

// Files in dir ending in ext (names only), sorted.
static void list_dir(const char *dir, const char *ext, Files *out) {
    size_t el = strlen(ext);
#if defined(_WIN32)
    char pat[512];
    snprintf(pat, sizeof(pat), "%s\\*%s", dir, ext);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do add_file(out, fd.cFileName); while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > el && !strcmp(e->d_name + l - el, ext)) add_file(out, e->d_name);
    }
    if (d) closedir(d);
#endif
    qsort(out->names, (size_t)out->n, sizeof(*out->names), cmp_names);
}

static void rules_for(const cJSON *kit, const char *id, KitRules *r) {
    kit_rules_default(r);
    const cJSON *pe = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(kit, "pieces"), id);
    const char *cat = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pe, "category"));
    if (!cat) return;
    snprintf(r->category, sizeof(r->category), "%s", cat);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(kit, "categories"), cat);
    const cJSON *v;
    r->collision_required = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "collision"));
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(c, "max_tris"))) r->max_tris = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(c, "grid"))) r->grid = (float)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(c, "grid_height"))) r->grid_height = (float)v->valuedouble;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(c, "sockets"))
        if (cJSON_IsString(v) && r->required_socket_n < 8) snprintf(r->required_sockets[r->required_socket_n++], 24, "%s", v->valuestring);
}

int main(int argc, char **argv) {
    const char *kit_dir = "assets/kits/building";
    Files pieces = { 0 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--kit") && i + 1 < argc) kit_dir = argv[++i];
        else add_file(&pieces, argv[i]);
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/kit.json", kit_dir);
    char *kt = read_text(path);
    cJSON *kit = kt ? cJSON_Parse(kt) : NULL;
    free(kt);
    if (!kit) printf("validate_kit: %s missing or not JSON (category rules skipped)\n", path);
    // Known material slots from the material sets.
    char slots[4096] = "";
    Files mats = { 0 };
    snprintf(path, sizeof(path), "%s/materials", kit_dir);
    list_dir(path, ".json", &mats);
    for (int i = 0; i < mats.n; i++) {
        char nm[128];
        snprintf(nm, sizeof(nm), "M_%s", mats.names[i]);
        nm[strlen(nm) - 5] = 0;   // ".json"
        strncat(slots, nm, sizeof(slots) - strlen(slots) - 2);
        strncat(slots, "\n", sizeof(slots) - strlen(slots) - 1);
    }
    if (!pieces.n) list_dir(kit_dir, ".glb", &pieces);
    int bad_pieces = 0, problems = 0;
    static char out[16384];
    for (int i = 0; i < pieces.n; i++) {
        const char *arg = pieces.names[i];
        const char *base = strrchr(arg, '/');
        const char *bs = strrchr(arg, '\\');
        if (bs && (!base || bs > base)) base = bs;
        base = base ? base + 1 : arg;
        char id[KIT_ID];
        snprintf(id, sizeof(id), "%s", base);
        char *dot = strrchr(id, '.');
        if (dot) *dot = 0;
        if (strchr(arg, '/') || strchr(arg, '\\')) snprintf(path, sizeof(path), "%s", arg);
        else snprintf(path, sizeof(path), "%s/%s", kit_dir, arg);
        KitPiece *p = calloc(1, sizeof(KitPiece));
        char err[256];
        if (!kit_piece_load(p, path, id, err, sizeof(err))) {
            printf("%s\n", err);
            problems++;
            bad_pieces++;
            free(p);
            continue;
        }
        KitRules r;
        rules_for(kit, id, &r);
        if (kit && !cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(kit, "pieces"), id)) {
            printf("%s: not in kit.json - add it (category, snap) so its rules apply\n", id);
            problems++;
        }
        int n = kit_piece_validate(p, &r, mats.n ? slots : NULL, out, sizeof(out));
        if (n) { fputs(out, stdout); problems += n; bad_pieces++; }
        kit_piece_free(p);
        free(p);
    }
    printf("validate_kit: %d piece(s), %d with problems, %d problem(s)\n", pieces.n, bad_pieces, problems);
    cJSON_Delete(kit);
    free(pieces.names);
    free(mats.names);
    return problems ? 1 : 0;
}
