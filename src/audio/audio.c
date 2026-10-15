#include "audio.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "miniaudio.h"
#include <stdlib.h>
#include <stdio.h>

#define MAX_VOICES 48
#define MAX_KNOWN 64

typedef struct Voice { ma_sound sound; bool used; } Voice;
typedef struct KnownSound { u32 hash; bool exists; char path[256]; } KnownSound;

typedef struct AudioSystem {
    ma_engine engine;
    ma_sound_group groups[4];
    bool ok;
    Voice voices[MAX_VOICES];
    int next_voice;
    KnownSound known[MAX_KNOWN];
    int known_count;
    ma_sound music;
    bool music_loaded;
    Cvar *master, *music_vol, *sfx, *voice;
} AudioSystem;

#define A (g_engine.audio)

bool audio_init(void) {
    A = (AudioSystem *)calloc(1, sizeof(AudioSystem));
    A->master = cvar_reg_float("snd_master", 0.8f, 0, 1, CVAR_ARCHIVE, "master volume");
    A->music_vol = cvar_reg_float("snd_music", 0.4f, 0, 1, CVAR_ARCHIVE, "music volume");
    A->sfx = cvar_reg_float("snd_sfx", 0.9f, 0, 1, CVAR_ARCHIVE, "effects volume");
    A->voice = cvar_reg_float("snd_voice", 1.0f, 0, 1, CVAR_ARCHIVE, "voice chat volume (reserved)");
    for (int i = 1; i < g_engine.argc; i++)
        if (!strcmp(g_engine.argv[i], "--nosound") || !strcmp(g_engine.argv[i], "--test")) { log_info("audio: disabled by --nosound"); return false; }
    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, &A->engine) != MA_SUCCESS) {
        log_warn("audio: no output device, running silent");
        return false;
    }
    for (int i = 0; i < 4; i++) ma_sound_group_init(&A->engine, 0, NULL, &A->groups[i]);
    A->ok = true;
    log_info("audio: miniaudio engine at %u Hz", ma_engine_get_sample_rate(&A->engine));
    return true;
}

void audio_shutdown(void) {
    if (!A) return;
    if (A->ok) {
        for (int i = 0; i < MAX_VOICES; i++) if (A->voices[i].used) ma_sound_uninit(&A->voices[i].sound);
        if (A->music_loaded) ma_sound_uninit(&A->music);
        for (int i = 0; i < 4; i++) ma_sound_group_uninit(&A->groups[i]);
        ma_engine_uninit(&A->engine);
    }
    free(A);
    A = NULL;
}

bool audio_available(void) { return A && A->ok; }

static const char *resolve(const char *name) {
    u32 h = dg_hash_str(name);
    for (int i = 0; i < A->known_count; i++)
        if (A->known[i].hash == h) return A->known[i].exists ? A->known[i].path : NULL;
    KnownSound *k = A->known_count < MAX_KNOWN ? &A->known[A->known_count++] : &A->known[0];
    k->hash = h;
    k->exists = false;
    const char *exts[] = { ".wav", ".ogg" };
    for (int e = 0; e < 2; e++) {
        char rel[128], buf[256];
        snprintf(rel, sizeof(rel), "assets/sounds/%s%s", name, exts[e]);
        const char *p = platform_data_path(rel, buf, sizeof(buf));
        if (platform_file_exists(p)) {
            dg_strcpy(k->path, sizeof(k->path), p);
            k->exists = true;
            break;
        }
    }
    if (!k->exists) log_debug("audio: no sound file for '%s'", name);
    return k->exists ? k->path : NULL;
}

static ma_sound_group *group_for(SoundCategory c) {
    switch (c) {
    case SND_MUSIC: return &A->groups[1];
    case SND_VOICE: return &A->groups[2];
    case SND_UI: return &A->groups[3];
    default: return &A->groups[0];
    }
}

static void play(const char *name, vec3 pos, float volume, SoundCategory cat, bool spatial) {
    if (!audio_available()) return;
    const char *path = resolve(name);
    if (!path) return;
    Voice *v = &A->voices[A->next_voice];
    A->next_voice = (A->next_voice + 1) % MAX_VOICES;
    if (v->used) ma_sound_uninit(&v->sound);
    v->used = false;
    // MA_SOUND_FLAG_DECODE caches decoded data in the resource manager, so repeats are cheap.
    if (ma_sound_init_from_file(&A->engine, path, MA_SOUND_FLAG_DECODE, group_for(cat), NULL, &v->sound) != MA_SUCCESS) return;
    v->used = true;
    ma_sound_set_volume(&v->sound, volume);
    if (spatial) {
        ma_sound_set_position(&v->sound, pos.x, pos.y, pos.z);
        ma_sound_set_min_distance(&v->sound, 1.5f);
        ma_sound_set_max_distance(&v->sound, 60.0f);
        ma_sound_set_attenuation_model(&v->sound, ma_attenuation_model_inverse);
    } else {
        ma_sound_set_spatialization_enabled(&v->sound, MA_FALSE);
    }
    ma_sound_start(&v->sound);
}

void audio_play(const char *name, vec3 pos, float volume, SoundCategory cat) { play(name, pos, volume, cat, true); }
void audio_play_2d(const char *name, float volume, SoundCategory cat) { play(name, v3(0, 0, 0), volume, cat, false); }

void audio_play_music(const char *name) {
    if (!audio_available()) return;
    const char *path = resolve(name);
    if (!path) return;
    if (A->music_loaded) ma_sound_uninit(&A->music);
    A->music_loaded = ma_sound_init_from_file(&A->engine, path, MA_SOUND_FLAG_STREAM, group_for(SND_MUSIC), NULL, &A->music) == MA_SUCCESS;
    if (A->music_loaded) {
        ma_sound_set_looping(&A->music, MA_TRUE);
        ma_sound_set_spatialization_enabled(&A->music, MA_FALSE);
        ma_sound_start(&A->music);
    }
}

void audio_update(vec3 pos, vec3 fwd, vec3 up) {
    if (!audio_available()) return;
    ma_engine_listener_set_position(&A->engine, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(&A->engine, 0, fwd.x, fwd.y, fwd.z);
    ma_engine_listener_set_world_up(&A->engine, 0, up.x, up.y, up.z);
    ma_engine_set_volume(&A->engine, A->master->f);
    ma_sound_group_set_volume(&A->groups[0], A->sfx->f);
    ma_sound_group_set_volume(&A->groups[1], A->music_vol->f);
    ma_sound_group_set_volume(&A->groups[2], A->voice->f);
    ma_sound_group_set_volume(&A->groups[3], A->sfx->f);
    // Recycle finished voices.
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &A->voices[i];
        if (v->used && ma_sound_at_end(&v->sound)) { ma_sound_uninit(&v->sound); v->used = false; }
    }
}
