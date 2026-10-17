// fbx_textures: writes the textures embedded in FBX files to a directory (under their original
// file names) and lists which material uses which diffuse texture.
//
//   fbx_textures [--out <dir>] <model.fbx>...      (default out: <fbx dir>/textures)
//
// Character models (Mixamo "nonPBR" exports) embed Diffuse / Normal / Specular / Glossiness PNGs.
// Also lists every material texture connection and writes <out>/<model>.maps.json (material -> FBX
// property -> file), from which tools/gen_skin_atlas.py builds the skins' albedo, normal and ORM
// atlases (docs/CHARACTERS.md "PBR maps").
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
        // Every texture connection (property <- file, embedded or not) and the scalar terms the
        // maps scale, so a model's real PBR content is visible (docs/CHARACTERS.md "PBR maps").
        for (size_t k = 0; k < mt->textures.count; k++) {
            const ufbx_material_texture *mtx = &mt->textures.data[k];
            const ufbx_texture *tx = mtx->texture;
            const char *fn = tx ? base_name(tx->relative_filename.length ? tx->relative_filename.data : tx->filename.data) : "?";
            printf("      %-22s <- %s%s\n", mtx->material_prop.data, fn, tx && tx->content.size ? " (embedded)" : "");
        }
        printf("      shininess %.2f  specular %.3f %.3f %.3f x %.2f  emission %.3f %.3f %.3f x %.2f  bump %.2f\n",
               mt->fbx.specular_exponent.value_real,
               mt->fbx.specular_color.value_vec3.x, mt->fbx.specular_color.value_vec3.y, mt->fbx.specular_color.value_vec3.z,
               mt->fbx.specular_factor.value_real,
               mt->fbx.emission_color.value_vec3.x, mt->fbx.emission_color.value_vec3.y, mt->fbx.emission_color.value_vec3.z,
               mt->fbx.emission_factor.value_real, mt->fbx.bump_factor.value_real);
    }
    // <out>/<model>.maps.json: material -> { FBX property: texture file } for tools/gen_skin_atlas.py
    // (which file is the normal map / glossiness / specular of which material, docs/CHARACTERS.md).
    char mpath[1400], model[256];
    snprintf(model, sizeof(model), "%s", base_name(path));
    char *dot = strrchr(model, '.');
    if (dot) *dot = 0;
    snprintf(mpath, sizeof(mpath), "%s/%s.maps.json", out_dir, model);
    FILE *mf = fopen(mpath, "wb");
    if (mf) {
        fprintf(mf, "{\n  \"model\": \"%s\",\n  \"materials\": {", model);
        for (size_t m = 0; m < sc->materials.count; m++) {
            const ufbx_material *mt = sc->materials.data[m];
            fprintf(mf, "%s\n    \"%s\": {", m ? "," : "", mt->name.data);
            int n = 0;
            for (size_t k = 0; k < mt->textures.count; k++) {
                const ufbx_texture *tx = mt->textures.data[k].texture;
                if (!tx) continue;
                fprintf(mf, "%s \"%s\": \"%s\"", n++ ? "," : "", mt->textures.data[k].material_prop.data,
                        base_name(tx->relative_filename.length ? tx->relative_filename.data : tx->filename.data));
            }
            fprintf(mf, " }");
        }
        fprintf(mf, "\n  }\n}\n");
        fclose(mf);
        printf("  %s\n", mpath);
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
