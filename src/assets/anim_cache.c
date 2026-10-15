#include "anim_cache.h"
#include "fbx_loader.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_MAGIC 0x43414744u   // "DGAC"
enum { KIND_SKEL = 1, KIND_MESH = 2, KIND_CLIPS = 3 };

static struct {
    char dir[512];
    bool rebuild;
    AnimCacheStats stats;
} AC;

void anim_cache_set_dir(const char *dir) { dg_strcpy(AC.dir, sizeof(AC.dir), dir); }
void anim_cache_set_rebuild(bool rebuild) { AC.rebuild = rebuild; }
const char *anim_cache_dir(void) { return AC.dir; }
AnimCacheStats anim_cache_stats(void) { return AC.stats; }

// ------------------------------------------------------------------ byte buffer

typedef struct Buf { u8 *p; size_t n, cap; size_t rd; bool bad; } Buf;

static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 1024;
        b->p = (u8 *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void get(Buf *b, void *d, size_t n) {
    if (b->bad || b->rd + n > b->n) { b->bad = true; memset(d, 0, n); return; }
    memcpy(d, b->p + b->rd, n);
    b->rd += n;
}
static void *get_alloc(Buf *b, size_t n) {
    if (b->bad || b->rd + n > b->n) { b->bad = true; return NULL; }
    void *d = malloc(n ? n : 1);
    memcpy(d, b->p + b->rd, n);
    b->rd += n;
    return d;
}

static void cache_path(const char *fbx, int kind, u32 key, char *out, size_t n) {
    char stem[64];
    fbx_file_stem(fbx, stem, sizeof(stem));
    for (char *c = stem; *c; c++) if (*c == ' ') *c = '_';
    static const char *k[] = { "", "skel", "mesh", "clips" };
    snprintf(out, n, "%s%s.%s.%08x.dgc", AC.dir, stem, k[kind], key);
}

static bool read_entry(const char *fbx, int kind, u32 key, Buf *b) {
    if (AC.rebuild || !AC.dir[0]) return false;
    char path[700];
    cache_path(fbx, kind, key, path, sizeof(path));
    size_t size = 0;
    char *data = platform_read_file(path, &size);
    if (!data) return false;
    b->p = (u8 *)data;
    b->n = size;
    b->rd = 0;
    u32 hdr[4];
    get(b, hdr, sizeof(hdr));
    if (b->bad || hdr[0] != CACHE_MAGIC || hdr[1] != ANIM_BAKE_VERSION || hdr[2] != (u32)kind || hdr[3] != key) {
        free(b->p);
        memset(b, 0, sizeof(*b));
        return false;
    }
    return true;
}

static void write_entry(const char *fbx, int kind, u32 key, Buf *body) {
    if (!AC.dir[0]) return;
    SDL_CreateDirectory(AC.dir);
    char path[700];
    cache_path(fbx, kind, key, path, sizeof(path));
    Buf b = { 0 };
    u32 hdr[4] = { CACHE_MAGIC, ANIM_BAKE_VERSION, (u32)kind, key };
    put(&b, hdr, sizeof(hdr));
    put(&b, body->p, body->n);
    if (platform_write_file(path, b.p, b.n)) AC.stats.writes++;
    else log_warn("anim cache: cannot write %s", path);
    free(b.p);
}

static u32 entry_key(const char *fbx, u32 extra) {
    u32 h = fbx_file_hash(fbx);
    if (!h) return 0;
    u32 v = ANIM_BAKE_VERSION;
    h = anim_hash(&v, sizeof(v), h);
    return anim_hash(&extra, sizeof(extra), h);
}

// ------------------------------------------------------------------ skeleton

bool anim_cache_skeleton(const char *fbx, Skeleton *out, char *err, int err_size) {
    u32 key = entry_key(fbx, 0);
    if (!key) { if (err) snprintf(err, (size_t)err_size, "cannot read %s", fbx); return false; }
    Buf b = { 0 };
    if (read_entry(fbx, KIND_SKEL, key, &b)) {
        get(&b, out, sizeof(*out));
        bool ok = !b.bad;
        free(b.p);
        if (ok) { AC.stats.hits++; return true; }
    }
    AC.stats.misses++;
    if (!fbx_load_skeleton(fbx, out, err, err_size)) return false;
    Buf w = { 0 };
    put(&w, out, sizeof(*out));
    write_entry(fbx, KIND_SKEL, key, &w);
    free(w.p);
    return true;
}

// ------------------------------------------------------------------ mesh

bool anim_cache_mesh(const char *fbx, const Skeleton *canon, SkinMeshData *out, char *err, int err_size) {
    u32 key = entry_key(fbx, canon->hash);
    if (!key) { if (err) snprintf(err, (size_t)err_size, "cannot read %s", fbx); return false; }
    Buf b = { 0 };
    if (read_entry(fbx, KIND_MESH, key, &b)) {
        get(&b, out, sizeof(*out));
        for (int l = 0; l < out->lod_count && l < SKIN_MAX_LODS; l++) {
            SkinLod *L = &out->lods[l];
            L->verts = (SkinVertex *)get_alloc(&b, sizeof(SkinVertex) * L->vcount);
            L->vmat = (u8 *)get_alloc(&b, L->vcount);
            L->indices = (u32 *)get_alloc(&b, sizeof(u32) * L->icount);
        }
        bool ok = !b.bad;
        free(b.p);
        if (ok) { AC.stats.hits++; return true; }
        memset(out, 0, sizeof(*out));
    }
    AC.stats.misses++;
    if (!fbx_load_skinned_mesh(fbx, canon, out, err, err_size)) return false;
    skin_generate_lods(out);
    Buf w = { 0 };
    SkinMeshData hdr = *out;
    for (int l = 0; l < SKIN_MAX_LODS; l++) { hdr.lods[l].verts = NULL; hdr.lods[l].vmat = NULL; hdr.lods[l].indices = NULL; }
    put(&w, &hdr, sizeof(hdr));
    for (int l = 0; l < out->lod_count; l++) {
        put(&w, out->lods[l].verts, sizeof(SkinVertex) * out->lods[l].vcount);
        put(&w, out->lods[l].vmat, out->lods[l].vcount);
        put(&w, out->lods[l].indices, sizeof(u32) * out->lods[l].icount);
    }
    write_entry(fbx, KIND_MESH, key, &w);
    free(w.p);
    return true;
}

// ------------------------------------------------------------------ clips

int anim_cache_clips(const char *fbx, const Skeleton *canon, float rate, Clip *out, int max, char *err, int err_size) {
    u32 rbits;
    memcpy(&rbits, &rate, sizeof(rbits));
    u32 key = entry_key(fbx, canon->hash ^ (rbits * 2654435761u));
    if (!key) { if (err) snprintf(err, (size_t)err_size, "cannot read %s", fbx); return 0; }
    Buf b = { 0 };
    if (read_entry(fbx, KIND_CLIPS, key, &b)) {
        int n = 0;
        get(&b, &n, sizeof(n));
        int got = 0;
        for (int i = 0; i < n && i < max && !b.bad; i++) {
            Clip *c = &out[i];
            get(&b, c, sizeof(*c));
            size_t fb = (size_t)c->frames;
            c->data = (Xform *)get_alloc(&b, sizeof(Xform) * fb * (size_t)c->bones);
            c->root_t = (vec3 *)get_alloc(&b, sizeof(vec3) * fb);
            c->root_yaw = (float *)get_alloc(&b, sizeof(float) * fb);
            got++;
        }
        bool ok = !b.bad && got == n;
        free(b.p);
        if (ok) { AC.stats.hits++; return got; }
        for (int i = 0; i < got; i++) clip_free(&out[i]);
    }
    AC.stats.misses++;
    FbxBakeOptions o = { rate, false, -1 };
    int n = fbx_bake_clips(fbx, canon, &o, out, max, err, err_size);
    if (n <= 0) return n;
    Buf w = { 0 };
    put(&w, &n, sizeof(n));
    for (int i = 0; i < n; i++) {
        Clip hdr = out[i];
        hdr.data = NULL;
        hdr.root_t = NULL;
        hdr.root_yaw = NULL;
        put(&w, &hdr, sizeof(hdr));
        put(&w, out[i].data, sizeof(Xform) * (size_t)out[i].frames * (size_t)out[i].bones);
        put(&w, out[i].root_t, sizeof(vec3) * (size_t)out[i].frames);
        put(&w, out[i].root_yaw, sizeof(float) * (size_t)out[i].frames);
    }
    write_entry(fbx, KIND_CLIPS, key, &w);
    free(w.p);
    return n;
}
