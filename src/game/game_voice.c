// Voice chat (docs/AUDIO_PROCEDURAL.md "Voice chat"), see game_voice.h.
#include "game_voice.h"
#include "game_internal.h"
#include "audio/audio.h"
#include "audio/mixer.h"
#include "audio/voice_capture.h"
#include "audio/env/env_world.h"
#include "net/net.h"
#include "input/input.h"
#include "render/render.h"
#include "ui/console.h"
#include "ui/gui/gui.h"
#include "ui/gui/gui_draw.h"
#include "ui/gui/gui_layout.h"
#include "ui/gui/gui_theme.h"
#include "gameplay/characters.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define G (g_engine.game)

static struct {
    Cvar *on, *mic_gain, *radio_dist, *radio_fade, *loopback, *loopback_dist, *test_tone;
    VoiceEncoder enc;
    i16 pend[VOICE_FRAME * VOICE_PACKET_FRAMES];
    int npend;
    u16 seq;
    bool ptt;
    double heard[MAX_PLAYERS];
    VoiceUpsample up[MAX_PLAYERS];
    EnvVoiceState env[MAX_PLAYERS];
    bool env_live[MAX_PLAYERS];
    float radio[MAX_PLAYERS];
    double tone_t, tone_carry;
    u32 ray_turn;
    u32 sent, received;
    float dist[MAX_PLAYERS];
} V;

// A test voice in place of the microphone (voice_test_tone): a buzzy 150 Hz voice with two formant-ish
// partials and four syllables a second.
static int test_tone(i16 *out, int max, float dt) {
    V.tone_carry += (double)dt * VOICE_RATE;
    int n = DG_MIN((int)V.tone_carry, max);
    V.tone_carry -= n;
    for (int i = 0; i < n; i++) {
        double t = V.tone_t + (double)i / VOICE_RATE;
        double syl = 0.5 + 0.5 * sin(2.0 * DG_PI * 4.0 * t);
        double x = 0.5 * sin(2.0 * DG_PI * 150.0 * t) + 0.25 * sin(2.0 * DG_PI * 750.0 * t) + 0.15 * sin(2.0 * DG_PI * 1650.0 * t);
        out[i] = (i16)(x * syl * 9000.0);
    }
    V.tone_t += (double)n / VOICE_RATE;
    return n;
}

static void send_packet(int me) {
    u8 pkt[VOICE_PACKET_MAX];
    pkt[0] = (u8)(V.seq & 0xFF);
    pkt[1] = (u8)(V.seq >> 8);
    V.seq++;
    for (int f = 0; f < VOICE_PACKET_FRAMES; f++) voice_encode(&V.enc, V.pend + f * VOICE_FRAME, pkt + 2 + f * VOICE_FRAME_BYTES);
    if (net_role() == NET_CLIENT) net_send_voice(pkt, VOICE_PACKET_MAX);
    else if (net_role() == NET_HOST) net_host_send_voice(me, pkt, VOICE_PACKET_MAX);
    if (V.loopback->i) game_voice_receive(me, pkt, VOICE_PACKET_MAX);
    V.sent++;
}

void game_voice_receive(int slot, const u8 *pkt, int len) {
    Mixer *m = audio_mixer();
    if (!m || slot < 0 || slot >= MAX_PLAYERS || len < 2 + VOICE_FRAME_BYTES || !V.on || !V.on->i) return;
    if (G && slot == G->local_slot && !V.loopback->i) return;
    int frames = DG_MIN((len - 2) / VOICE_FRAME_BYTES, VOICE_PACKET_FRAMES);
    for (int f = 0; f < frames; f++) {
        i16 pcm[VOICE_FRAME];
        float up[VOICE_FRAME * 3];
        voice_decode(pkt + 2 + f * VOICE_FRAME_BYTES, pcm);
        voice_upsample3(&V.up[slot], pcm, VOICE_FRAME, up);
        mixer_stream_push(m, slot, up, VOICE_FRAME * 3);
    }
    V.heard[slot] = platform_time();
    V.received++;
}

bool game_voice_talking(int slot) {
    if (slot < 0 || slot >= MAX_PLAYERS) return false;
    if (G && slot == G->local_slot) return V.ptt;
    return platform_time() - V.heard[slot] < 0.3;
}

// Where a player's voice comes from: the head of their pose (else the eye); false when this machine
// doesn't know (not replicated here: far away).
static bool talker_pos(World *w, int s, const RenderCamera *cam, vec3 *out) {
    if (s == G->local_slot) {   // loopback: in front of the camera
        *out = v3_madd(cam->pos, forward_from_yaw(cam->yaw), V.loopback_dist->f);
        return true;
    }
    Player *p = &w->players[s];
    if (!p->active || !ent_valid(w, p->ent)) return false;
    if (!chars_head_world(w, p->ent, G->alpha, out)) *out = player_eye_pos(&p->sim.move);
    return true;
}

void game_voice_tick(World *w) {
    if (!w || !G || !V.on) return;
    float dt = (float)g_engine.frame_dt;
    int me = G->local_slot;
    bool can = V.on->i && me >= 0 && me < MAX_PLAYERS && w->players[me].active;
    bool ptt = can && ((input_down(ACT_TALK) && !console_is_open()) || V.test_tone->i == 2);
    if (ptt && !V.ptt) { V.npend = 0; memset(&V.enc, 0, sizeof(V.enc)); }   // a new talk: a fresh encoder
    V.ptt = ptt;
    // The microphone (or the test voice): always drained, sent while the key is held.
    if (ptt && !V.test_tone->i) voice_capture_open();
    i16 buf[4096];
    int n = V.test_tone->i ? test_tone(buf, 4096, dt) : voice_capture_read(buf, 4096);
    if (ptt) {
        float g = V.mic_gain->f;
        for (int i = 0; i < n; i++) {
            V.pend[V.npend++] = (i16)DG_CLAMP((int)lrintf((float)buf[i] * g), -32768, 32767);
            if (V.npend == VOICE_FRAME * VOICE_PACKET_FRAMES) { send_packet(me); V.npend = 0; }
        }
    }
}

void game_voice_frame(World *w, const RenderCamera *cam) {
    Mixer *m = audio_mixer();
    if (!w || !m || !cam || !G || !V.on) return;
    // Every talker: the proximity voice from their head, the radio past voice_radio_dist.
    double now = platform_time();
    float rd = V.radio_dist->f, rf = fmaxf(V.radio_fade->f, 0.1f);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        bool active = now - V.heard[s] < 1.0 || mixer_stream_level(m, s) > 1e-4f;
        if (!active) { V.env_live[s] = false; continue; }
        vec3 pos;
        bool known = talker_pos(w, s, cam, &pos);
        float dist = known ? v3_dist(pos, cam->pos) : 1e9f;
        V.dist[s] = dist;
        float k = saturate((dist - rd) / rf);
        V.radio[s] = k * k * (3.0f - 2.0f * k);
        if (!known) pos = cam->pos;
        EnvSource src;
        memset(&src, 0, sizeof(src));
        src.pos = pos;
        src.volume = 1.0f;
        src.min_dist = 1.0f;
        src.max_dist = rd + rf + 10.0f;
        src.rolloff = 1.0f;
        src.reverb = 0.6f;
        src.spatial = true;
        src.max_taps = 4;
        src.self_radius = 0.5f;
        if (!V.env_live[s]) { env_world_voice_start(&V.env[s], &src); V.env_live[s] = true; }
        else V.env[s].src.pos = pos;
        EnvParams ep;
        env_world_voice_eval(&V.env[s], (V.ray_turn++ & 3) == 0, &ep);
        mixer_stream_env(m, s, &ep, V.radio[s]);
    }
}

// The talking indicator: the talkers (you while the key is held) on the left, a dot, RADIO past the range.
void game_voice_draw(void) {
    World *w = game_world();
    if (!w || !G || !V.on || !V.on->i) return;
    const GuiTheme *th = gui_theme();
    LayoutParams lp = gui_layout_params();
    GuiRect safe = gui_layout_safe_rect(&lp);
    float x = safe.x, y = safe.y + safe.h * 0.42f, row = gui_s(th->t_small + 10);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        if (!game_voice_talking(s)) continue;
        bool mine = s == G->local_slot;
        const char *name = w->players[s].active && w->players[s].name[0] ? w->players[s].name : "Player";
        bool radio = !mine && V.radio[s] > 0.5f;
        float r = gui_s(4);
        gd_circle(x + r, y + row * 0.5f, r, radio ? th->warning : th->ok);
        char line[64];
        snprintf(line, sizeof(line), "%s%s", mine ? "You" : name, radio ? "  ·  RADIO" : "");
        GuiText t = { gui_s(th->t_small + 1), GW_SEMIBOLD, th->hud_text, GUI_LEFT, 0, false, true, 0 };
        gd_text(&t, x + r * 3, y + (row - gd_line_height(t.size)) * 0.5f, line);
        y += row;
    }
}

static void cmd_voice_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    Mixer *m = audio_mixer();
    char line[256];
    int k = snprintf(line, sizeof(line), "voice: %s, mic %s, talking %d, sent %u, received %u packets;", V.on->i ? "on" : "off",
                     voice_capture_is_open() ? "open" : (V.test_tone->i ? "test tone" : "closed"), V.ptt ? 1 : 0, V.sent, V.received);
    for (int s = 0; s < MAX_PLAYERS && k < (int)sizeof(line) - 40; s++)
        if (platform_time() - V.heard[s] < 1.0)
            k += snprintf(line + k, sizeof(line) - (size_t)k, " slot %d %.0f m level %.3f radio %.2f;", s, V.dist[s], m ? mixer_stream_level(m, s) : 0.0f, V.radio[s]);
    log_info("%s", line);
}

void game_voice_register(void) {
    V.on = cvar_reg_bool("voice_chat", true, CVAR_ARCHIVE | CVAR_EDITABLE, "Voice chat: push to talk (Talk, Y) and hearing the others");
    V.mic_gain = cvar_reg_float("voice_mic_gain", 1.0f, 0.0f, 4.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "Voice chat: microphone gain");
    V.radio_dist = cvar_reg_float("voice_radio_dist", 30.0f, 1.0f, 500.0f, CVAR_LOCKED, "Voice chat: past this distance (m) a voice goes to the radio");
    V.radio_fade = cvar_reg_float("voice_radio_fade", 10.0f, 0.1f, 100.0f, CVAR_LOCKED, "Voice chat: metres over which the voice crossfades into the radio");
    V.loopback = cvar_reg_bool("voice_loopback", false, CVAR_LOCKED, "Voice chat test: hear your own voice, voice_loopback_dist in front of you");
    V.loopback_dist = cvar_reg_float("voice_loopback_dist", 3.0f, 0.0f, 1000.0f, CVAR_LOCKED, "Voice chat test: the loopback voice's distance (m; past voice_radio_dist: the radio)");
    V.test_tone = cvar_reg_int("voice_test_tone", 0, 0, 2, CVAR_LOCKED, "Voice chat test: 1 a test voice replaces the microphone, 2 also talks without the key");
    cmd_register("voice_info", cmd_voice_info, "Voice chat: microphone, packets sent / received, each talker's level and radio amount");
}
