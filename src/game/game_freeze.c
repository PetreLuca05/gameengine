// Frozen world (scene handpose, docs/HANDPOSE.md "Frozen world"): time of day, weather (and its
// transitions), wind gusts, VFX time, zombie / NPC simulation, vehicle physics, props and ambience
// stop; the editor runs on its own clock. Optional neutral lighting: 12:30, clear sky. Leaving
// restores everything as it was (the time of day, the weather transition where it stopped).
#include "game_internal.h"
#include "audio/audio.h"
#include "gameplay/characters.h"
#include "core/cvar.h"
#include "core/log.h"
#include "world/time_of_day.h"

static struct {
    bool on;
    World *w;
    float hours, day_length;
    float wind_ts, vfx_ts;
} FZ;

bool game_world_frozen(void) { return FZ.on; }

static void cvar_swap(const char *name, float *saved, float value) {
    Cvar *c = cvar_find(name);
    if (!c) return;
    if (saved) *saved = c->f;
    cvar_set_float(c, value);
}

void game_world_freeze(World *w, bool on, bool neutral_light) {
    if (!w || !w->authority || on == FZ.on) return;
    if (on) {
        FZ.w = w;
        FZ.hours = tod_hours(&w->tod, (double)w->tick);
        FZ.day_length = w->tod.day_length;
        tod_set_day_length(&w->tod, (double)w->tick, 0.0f);
        if (neutral_light) tod_set_time(&w->tod, (double)w->tick, 12.5f);
        weather_freeze(w, true, neutral_light);
        cvar_swap("wind_timescale", &FZ.wind_ts, 0.0f);
        cvar_swap("vfx_timescale", &FZ.vfx_ts, 0.0f);
        audio_set_ambience_gain(0.0f);
        g_chars_frozen = true;
        FZ.on = true;
        log_info("world frozen (time %.2f h%s)", FZ.hours, neutral_light ? ", neutral lighting" : "");
        return;
    }
    FZ.on = false;
    g_chars_frozen = false;
    if (w == FZ.w) {
        tod_set_time(&w->tod, (double)w->tick, FZ.hours);
        tod_set_day_length(&w->tod, (double)w->tick, FZ.day_length);
        weather_freeze(w, false, false);
    }
    cvar_swap("wind_timescale", NULL, FZ.wind_ts);
    cvar_swap("vfx_timescale", NULL, FZ.vfx_ts);
    audio_set_ambience_gain(1.0f);
    log_info("world running again");
}

void game_world_freeze_lighting(World *w, bool neutral) {
    if (!FZ.on || w != FZ.w) return;
    tod_set_time(&w->tod, (double)w->tick, neutral ? 12.5f : FZ.hours);
    weather_freeze(w, false, false);
    weather_freeze(w, true, neutral);
}
