// Procedural audio tools (docs/AUDIO_PROCEDURAL.md "Recipe editor", "Environment preview"):
//   audio_editor       pick a recipe, set its event inputs with sliders, trigger it (3D / 2D, seed),
//                      edit the layers' patch values live, see the waveform and spectrogram of an
//                      offline render, A/B against a sample, save the patch
//   audio_env_preview  drive the environment chain with sliders (distance, direction, walls,
//                      diffraction, room, an outdoor reflector, weather) and play a recipe through it
#ifndef DG_AUDIO_EDITOR_H
#define DG_AUDIO_EDITOR_H

#include "core/base.h"

void audio_editor_init(void);
void audio_editor_draw(void);
bool audio_editor_open(void);   // either panel

#endif
