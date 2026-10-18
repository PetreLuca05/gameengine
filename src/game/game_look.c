// Game side of the Looks (docs/LOOKS.md, src/render/post/look.h): the one saved setting (r_look),
// the `look <name>` command, the look's finish into the frame's PostSettings (bounded by Engine
// Fidelity and the user's effect strengths), the time-of-day / weather modulators, the Settings
// A/B (hold to compare with Realism) and, in dev builds only (DG_LOOK_TUNE), look_dump / look_tune.
#include "game_internal.h"
#include "world/weather.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "render/render.h"
#include "render/fidelity/fidelity.h"
#include "render/post/look.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

static struct {
    Cvar *look, *modulators;
    Cvar *bloom, *bloom_int, *vignette, *grain, *chromatic, *sharpen;
    bool compare, compare_next;   // A/B: Realism while the Settings button is held
    int last_level;
} GL;

int game_look_selected(void) {
    int id = look_find(GL.look ? GL.look->str : "");
    return id >= 0 ? id : LOOK_REALISM;
}

int game_look_current(void) { return GL.compare ? LOOK_REALISM : game_look_selected(); }

void game_look_set(int id) {
    if (id < 0 || id >= LOOK_COUNT) return;
    cvar_set_cv(GL.look, look_id_name(id));
}

void game_look_compare_hold(bool held) { GL.compare_next |= held; }
bool game_look_comparing(void) { return GL.compare; }

// The user's strength of a finish effect: its cvar over the cvar's default (Engine Fidelity forces
// the cvar to 0 where the feature is unavailable).
static float strength(const Cvar *c) {
    if (!c) return 1.0f;
    float d = (float)atof(c->default_str);
    return d > 1e-6f ? c->f / d : (c->f > 0 ? 1.0f : 0.0f);
}

static int fid_now(void) { return fid_loaded() ? fid_level() : FID_MODERN; }

void game_look_apply(PostSettings *s) {
    int id = game_look_current();
    LookUser u;
    u.bloom = GL.bloom && GL.bloom->i ? strength(GL.bloom_int) : 0.0f;
    u.vignette = strength(GL.vignette);
    u.grain = strength(GL.grain);
    u.chromatic = strength(GL.chromatic);
    u.sharpen = strength(GL.sharpen);
    look_apply_post(s, id, look_get(id), &u, fid_now());
}

void game_look_modulate(PostSettings *s) {
    if (!GL.modulators->i) return;
    World *w = G ? G->world : NULL;
    LookEnv e = { 45.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    if (w && daynight_active(w)) {
        e.sun_elev_deg = world_sun_elevation_deg(w);
        e.daylight = daynight_daylight();
    }
    WeatherParams wp;
    if (weather_current(w, &wp)) {
        e.cloud = wp.v[WP_CLOUD_COVER];
        e.precip = wp.v[WP_PRECIP];
        e.lightning = saturate(wp.v[WP_LIGHTNING] / 4.0f);   // strikes per minute: 4+ = a full storm
        e.wind = wp.v[WP_WIND];
    }
    int id = game_look_current();
    LookMod m;
    look_modulate(look_get(id), &e, &m);
    look_apply_mod(s, &m);
}

// Once per frame (game_post_frame): the A/B hold of the last UI frame, Engine Fidelity changes.
void game_look_frame(void) {
    GL.compare = GL.compare_next;
    GL.compare_next = false;
    int lvl = fid_now();
    if (GL.last_level >= 0 && lvl != GL.last_level) render_looks_invalidate();   // the LUTs follow the level (docs/LOOKS.md)
    GL.last_level = lvl;
}

// ------------------------------------------------------------------ commands

static void cmd_look(int argc, const char **argv) {
    if (argc < 2) {
        int cur = game_look_selected();
        for (int i = 0; i < LOOK_COUNT; i++) log_info("%s %-10s %-11s %s", i == cur ? "*" : " ", look_id_name(i), look_display_name(i), look_desc(i));
        log_info("usage: look <name>");
        return;
    }
    int id = look_find(argv[1]);
    if (id < 0) { log_info("look: unknown look '%s' (realism, pubg, cinematic, vibrant, gritty)", argv[1]); return; }
    game_look_set(id);
    log_info("look: %s", look_display_name(id));
}

#if DG_LOOK_TUNE
// Dev tools (compiled out of release builds): print a look as the C initializer of looks.c, and
// override its constants live. Nothing is written to disk: paste look_dump's output into looks.c.
static void dump(int id) {
    static char buf[16384];
    look_format(look_get(id), buf, sizeof(buf));
    log_info("// %s%s", look_display_name(id), look_tune_dirty(id) ? " (tuned, not in looks.c yet)" : "");
    for (char *line = buf, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        log_info("%s", line);
    }
}

static void cmd_look_dump(int argc, const char **argv) {
    if (argc > 1 && !strcmp(argv[1], "all")) {
        for (int i = 0; i < LOOK_COUNT; i++) dump(i);
        return;
    }
    int id = argc > 1 ? look_find(argv[1]) : game_look_selected();
    if (id < 0) { log_info("usage: look_dump [name|all]"); return; }
    dump(id);
}

static void cmd_look_tune(int argc, const char **argv) {
    int id = game_look_selected();
    int count;
    const LookField *f = look_fields(&count);
    if (argc < 2) {
        log_info("look_tune <field> <values...> | <field> <knot> <value> | reset [all] | print   (edits %s, live, never saved)", look_id_name(id));
        for (int i = 0; i < count; i++) {
            const float *v = (const float *)((const char *)look_get(id) + f[i].offset);
            char vals[160] = "";
            for (int k = 0; k < f[i].count; k++) {
                size_t n = strlen(vals);
                if (f[i].is_int) snprintf(vals + n, sizeof(vals) - n, "%d ", *(const int *)v);
                else snprintf(vals + n, sizeof(vals) - n, "%.4g ", (double)v[k]);
            }
            log_info("  %-18s %s", f[i].name, vals);
        }
        return;
    }
    if (!strcmp(argv[1], "reset")) {
        look_tune_reset(argc > 2 && !strcmp(argv[2], "all") ? -1 : id);
        render_looks_invalidate();
        log_info("look_tune: %s back to the compiled constants", argc > 2 ? "all looks" : look_id_name(id));
        return;
    }
    if (!strcmp(argv[1], "print")) { dump(id); return; }
    const LookField *fd = NULL;
    for (int i = 0; i < count; i++)
        if (!strcmp(f[i].name, argv[1])) fd = &f[i];
    if (!fd) { log_info("look_tune: no field '%s' (look_tune lists them)", argv[1]); return; }
    LookParams *p = look_tune_edit(id);
    float *v = (float *)((char *)p + fd->offset);
    int nv = argc - 2;
    if (fd->is_int && nv == 1) *(int *)v = atoi(argv[2]);
    else if (nv == fd->count) for (int k = 0; k < nv; k++) v[k] = (float)atof(argv[2 + k]);
    else if (fd->count > 1 && nv == 2 && atoi(argv[2]) >= 0 && atoi(argv[2]) < fd->count) v[atoi(argv[2])] = (float)atof(argv[3]);
    else { log_info("look_tune: %s takes %d value%s%s", fd->name, fd->count, fd->count == 1 ? "" : "s", fd->count > 1 ? " (or <index> <value>)" : ""); return; }
    render_looks_invalidate();
    log_info("look_tune: %s.%s set (look_dump prints the table for looks.c)", look_id_name(id), fd->name);
}
#endif

void game_look_init(void) {
    memset(&GL, 0, sizeof(GL));
    GL.last_level = -1;
    GL.look = cvar_reg_string("r_look", "realism", CVAR_LOCKED, "post-processing Look: realism (always at start; not in Settings or the config), pubg, cinematic, vibrant, gritty for dev (docs/LOOKS.md)");
    GL.modulators = cvar_reg_bool("r_look_modulators", true, CVAR_LOCKED, "the Look's time-of-day / weather modulation (0 = the static grade, tests)");
    GL.bloom = cvar_find("r_bloom");
    GL.bloom_int = cvar_find("r_bloom_intensity");
    GL.vignette = cvar_find("r_vignette");
    GL.grain = cvar_find("r_film_grain");
    GL.chromatic = cvar_find("r_chromatic");
    GL.sharpen = cvar_find("r_sharpen");
    cmd_register("look", cmd_look, "look <name>: the post-processing Look (realism, pubg, cinematic, vibrant, gritty); no argument lists them");
#if DG_LOOK_TUNE
    cmd_register("look_dump", cmd_look_dump, "look_dump [name|all]: print a Look as the C table of looks.c (dev builds)");
    cmd_register("look_tune", cmd_look_tune, "look_tune <field> <values>: override the current Look's constants live, never saved (dev builds)");
#endif
}
