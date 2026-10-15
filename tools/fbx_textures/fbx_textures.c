// fbx_textures: writes the textures embedded in FBX files to a directory (under their original
// file names) and lists which material uses which diffuse texture.
//
//   fbx_textures [--out <dir>] <model.fbx>...      (default out: <fbx dir>/textures)
//
// Character models (Mixamo "nonPBR" exports) embed Diffuse / Normal / Specular / Glossiness PNGs;
// the game loads the extracted diffuse maps for skinned characters (assets/human/textures).
#include "ufbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static const char *base_name(const char *p) {
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}

static int extract(const char *path, const char *out_dir_opt) {
    char out_dir[1024];
    if (out_dir_opt) snprintf(out_dir, sizeof(out_dir), "%s", out_dir_opt);
    else {
        snprintf(out_dir, sizeof(out_dir), "%s", path);
        char *s = (char *)base_name(out_dir);
        snprintf(s, sizeof(out_dir) - (size_t)(s - out_dir), "textures");
    }
    MKDIR(out_dir);
    ufbx_load_opts opts = { 0 };
    ufbx_error err;
    ufbx_scene *sc = ufbx_load_file(path, &opts, &err);
    if (!sc) { fprintf(stderr, "%s: %s\n", path, err.description.data); return 1; }
    int written = 0;
    for (size_t i = 0; i < sc->videos.count; i++) {
        const ufbx_video *v = sc->videos.data[i];
        if (!v->content.size) continue;
        const char *name = base_name(v->relative_filename.length ? v->relative_filename.data : v->filename.data);
        if (!name[0]) continue;
        char out[1400];
        snprintf(out, sizeof(out), "%s/%s", out_dir, name);
        FILE *f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", out); continue; }
        fwrite(v->content.data, 1, v->content.size, f);
        fclose(f);
        printf("  %s (%zu KB)\n", out, v->content.size / 1024);
        written++;
    }
    for (size_t m = 0; m < sc->materials.count; m++) {
        const ufbx_material *mt = sc->materials.data[m];
        const ufbx_texture *t = mt->fbx.diffuse_color.texture ? mt->fbx.diffuse_color.texture : mt->pbr.base_color.texture;
        printf("  material '%s' -> %s\n", mt->name.data, t ? base_name(t->relative_filename.length ? t->relative_filename.data : t->filename.data) : "(no texture)");
    }
    printf("%s: %d texture(s)\n", path, written);
    ufbx_free_scene(sc);
    return 0;
}

int main(int argc, char **argv) {
    const char *out = NULL;
    int rc = 0, files = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) { out = argv[++i]; continue; }
        rc |= extract(argv[i], out);
        files++;
    }
    if (!files) { fprintf(stderr, "usage: fbx_textures [--out dir] model.fbx...\n"); return 2; }
    return rc;
}
