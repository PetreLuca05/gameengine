// Voice chat (docs/AUDIO_PROCEDURAL.md "Voice chat"): push to talk (ACT_TALK, Y) captures the
// microphone (audio/voice_capture.c), encodes 40 ms packets (audio/voice_codec.h) and sends them to the
// host, which plays them and relays them to everyone else. Each machine plays a player's voice from
// their head through the environment chain (distance, walls, panning, room); past voice_radio_dist it
// crossfades over voice_radio_fade metres into the radio (band-passed, driven, hiss, squelch, level).
#ifndef DG_GAME_VOICE_H
#define DG_GAME_VOICE_H

#include "core/base.h"
#include "audio/voice_codec.h"

#define VOICE_PACKET_MAX (2 + VOICE_PACKET_FRAMES * VOICE_FRAME_BYTES)   // u16 sequence + the frames

struct World;
struct RenderCamera;

void game_voice_register(void);
// Every game frame (game_frame): push to talk, the microphone, the packets out.
void game_voice_tick(struct World *w);
// Every render frame, after the audio listener update: each talker's position, proximity / radio.
void game_voice_frame(struct World *w, const struct RenderCamera *cam);
// A packet from the network (the host's relay, or a client's on the host).
void game_voice_receive(int slot, const u8 *pkt, int len);
bool game_voice_talking(int slot);   // heard in the last 0.3 s (the local slot: push to talk held)
void game_voice_draw(void);          // HUD: the talking indicator

#endif
