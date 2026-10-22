# Procedural audio

Sounds are synthesized at runtime from parameters, blended with generated or recorded samples
where those work better. Every sound, synthesized or sampled, is then shaped by a per-voice
environment chain built from the world. That chain models where the sound is, how far away, what
is in between, the room or the open air around it, the weather, and the listener's state.

The same gunshot recipe fired in a corridor, a valley and a field gives three different tails, and
each shot varies from the replicated shot seed. A new weapon or surface needs parameters, not new
samples.

```
 game thread                                  audio thread (64-frame blocks, flush-to-zero)
 audio_emit(recipe, pos, params, seed) ──┐    ┌─ voice pool (128 slots, preallocated arenas)
 audio_play / _fx / loops (legacy names) ├─►  │    patch layers (≤ 4) ─► env chain ─► bus
 env tick: rays, rooms, echoes, weather ─┘    │    sends ─► 3 shared FDN reverbs ─► sfx bus
         lock-free command ring               └─ buses ─► listener filter ─► death low-pass ─► master
```

## Files

| Area | Code |
|---|---|
| Synth graph: nodes, patch compiler, voices | `src/audio/synth/syn_graph.{c,h}`, `syn_dsp.{c,h}` (filters, smoothers, noise, oscillators) |
| Patch JSON, writer | `src/audio/synth/syn_patch.{c,h}` |
| Recipes, binding expressions, library | `src/audio/synth/syn_recipe.{c,h}`, `syn_expr.{c,h}` |
| Materials (synthesis + environment) | `src/audio/synth/syn_material.{c,h}`, `assets/audio/materials.json` |
| Mixer (voices, buses, reverbs, degradation) | `src/audio/mixer.{c,h}` |
| Environment chain DSP, FDN | `src/audio/env/env_chain.{c,h}` |
| Environment model (physics, pure) | `src/audio/env/env_model.{c,h}` |
| Environment, world side (rays, caches, per voice) | `src/audio/env/env_world.{c,h}` |
| Device, samples, library load, emit API, legacy API | `src/audio/audio.{c,h}` |
| Game glue: rays, foliage, listener state, footsteps, zombie calls, `audio_debug` | `src/game/game_audio.c` |
| Recipe editor, environment preview | `src/ui/audio_editor.c` |
| Data | `assets/audio/patches/*.json`, `assets/audio/recipes/*.json`, `assets/audio/events.json`, `assets/audio/materials.json` |
| Recorded CC0 takes: manifest, importer, credits | `assets/audio/sample_sources.json`, `tools/audio_import/import_samples.py`, `assets/sounds/<name>_<n>.ogg`, `assets/sounds/CREDITS.md` |

## Synth graph

A patch is a directed acyclic graph of nodes processing 64-frame mono buffers.
`syn_patch_compile` produces the execution order (Kahn's algorithm, dependencies first) and rejects
cycles: feedback only lives inside nodes (comb, allpass, delay, Karplus-Strong, waveguide). It also
lays out every node's state in one arena (≤ 64 KB per layer, ≤ 96 KB per voice). A voice renders
with no allocation.

Patches are compiled at the mixer rate (48 kHz; miniaudio resamples to the device). They are
immutable once compiled, so the audio thread reads them freely. The editor replaces a patch with an
edited copy and keeps the old copy alive until shutdown.

Node parameters are one of:
- a constant;
- `"$param"`, `"$param*k"` or `"$param*k+o"`: a named patch parameter, set per voice from the
  recipe, smoothed with a one-pole of `tau` (15 ms) and ramped across each block;
- `{"mod": "node", "scale": k, "offset": o}`: block-rate modulation by another node's output.

Each node sees its parameters at the block's start and end. Gains ramp between them, and filters
redesign when the value moves.

### Node reference

| Node | Parameters (defaults) | Options | Notes |
|---|---|---|---|
| `osc` | hz 440, amp 1, fm 0, pw 0.5, phase 0 | wave `sine` / `saw` / `square` / `tri` | polyBLEP saw / square; `fm` = Hz per unit of the input (sweeps, vibrato) |
| `noise` | amp 1, density 2000 | color `white` / `pink` / `brown` / `velvet` | pink: Kellet filter; velvet: sparse ±1 impulses |
| `impulse` | amp 1, width_ms 0, delay_ms 0, repeat_ms 0, count 1 | | raised cosine of unit area: a wider contact is darker, not louder |
| `friedlander` | amp 1, dur_ms 2, b 1.5, delay_ms 0 | | blast pressure p(t) = P (1 − t/T) e^(−bt/T) |
| `nwave` | amp 1, dur_ms 0.3, rise_us 20, delay_ms 0 | | supersonic N-wave with finite rise |
| `pulses` | hz 30, amp 1, jitter 0, width_ms 0.5, decay_ms 3, count 4, spread 0.2 | | engine firing pulses, per-cylinder variation |
| `glottal` | hz 110, amp 1, open 0.6, jitter, shimmer, fry, fry_hz, breath | | Rosenberg pulse (flow derivative), vocal fry, aspiration |
| `grains` | rate, amp, hz, spread, dur_ms, decay, q | src `noise` / `sine` / `click` / `sample` | Poisson grain cloud (≤ 16 live); `decay` thins it over time |
| `env` | amp 1, time 1, gate 1 | `stages` [[ms, level, curve]], `sustain`, or `adsr` [a, d, s, r] | with an input: a VCA; curve > 0 = exponential-like |
| `biquad` | hz 1000, q 0.707, gain_db 0 | mode `lp` `hp` `bp` `notch` `peak` `lshelf` `hshelf` `ap` | RBJ cookbook, TDF-II |
| `svf` | hz, q | mode `lp` `hp` `bp` `notch` | Simper / Cytomic: stable under fast modulation |
| `onepole` | hz | mode `lp` `hp` | |
| `comb`, `allpass`, `delay` | ms, fb, damp / g, mix | `max_ms` | fractional delay, linear interpolation |
| `tilt` | db, pivot | | spectral tilt around the pivot |
| `shaper` | drive, mix, amp, bias | mode `tanh` `fold` `clip` `asym` | |
| `ring`, `am` | amp / hz, depth, amp | | ring: in0 × in1; am: by an LFO or in1 |
| `gain`, `mix` | amp, db / amp, `gains` | | ramped |
| `modal` | pitch, damp, amp, material, spread, bright | `modes` [[Hz, T60, gain]] | 2-pole resonator bank; `material` ≥ 0 takes the bank from `materials.json` |
| `karplus` | hz, t60, bright, amp | `max_ms` | plucked string; without input, a seeded noise burst |
| `waveguide` | length (m), reflect, loss, amp, c | `max_ms` | tube: round trip 2L/c, end reflection, loss low-pass |
| `formant` | vowel 0..5 (a e i o u schwa), shift, amp, bw | | five band-passes; fractional vowels morph |
| `conv` | len_ms, decay_ms, bright, amp, mix | | short seeded body IR, ≤ 256 taps |
| `sample` | pitch, amp, start_ms, jitter_ms | play `once` / `loop` | samples are decoded mono at 48 kHz |

A voice ends when all its terminators (envelopes, impulses, blasts, N-waves, one-shot samples) are
done and its output has stayed below -90 dB for 8 blocks. `max_dur` caps it. Loops (`"loop": true`)
end only when the mixer stops them: a stop sets every patch's `gate` parameter to 0 and fades.

Gain conventions:
- `modal` scales each mode so a one-sample impulse peaks at the mode's gain.
- Driven continuously at resonance, a mode gains about 1/(1−r). Engine bodies therefore use an amp
  near 0.002.

### Patch format

```json
{ "name": "gun_blast", "loop": false, "max_dur": 0, "tau": 0.015,
  "params": { "energy": 1500, "tilt": [0, -12, 12] },
  "nodes": [ { "id": "n", "type": "noise", "color": "pink" },
             { "id": "e", "type": "env", "in": "n", "stages": [[0.15, 1], [40, 0, 3.5]], "time": "$burst_scale" },
             { "id": "lp", "type": "biquad", "mode": "lp", "in": "e", "hz": "$lp_hz" } ],
  "out": "lp" }
```

A parameter is a default, or `[default, lo, hi]`; the bounds also clamp the recipe's bindings and
set the editor's ranges.

## Recipes

A recipe is a list of layers, each a patch or a sample. Bindings map the event's physical values
onto patch parameters; random ranges vary them per seed. Optional layers are dropped first under
load.

```json
{ "name": "gunshot", "bus": "sfx", "priority": 0.95, "echo": true, "max_instances": 24,
  "distance": { "min": 6, "max": 2500, "rolloff": 0.6 },
  "directivity": { "front_db": 3, "rear_db": -6, "rear_lp": 4000 },
  "inputs": { "energy": 2000, "caliber": 7.62, "barrel": 0.4, "device": 0 },
  "layers": [
    { "patch": "gun_blast", "gain_db": -6,
      "bind": { "amp": "sqrt(max(energy, 50) / 2000)", "tilt": "sel(device, 0, -1.5, 2.5, -6)" },
      "random": { "burst_amp": ["db", -1.2, 1.2], "thump_ms": ["mul", 0.95, 1.05] } },
    { "sample": "@", "gain_db": "lerp(distance, 100, 0, 250, -30) - 3", "pitch": [0.98, 1.02] },
    { "sample": "@_distant", "when": "distance > 80", "gain_db": "lerp(distance, 100, -30, 250, -3)" } ] }
```

Recipe fields:
- `bus`: `sfx`, `voice`, `ui` or `ambience`.
- `spatial` / `env`: whether the sound is positional and runs the environment chain (UI recipes are
  neither).
- `loop`, `doppler` (the voice runs through a propagation delay line), `echo` (outdoor echo
  children).
- `stack_s`: the same recipe again within this time and 2 m folds into the louder one.
- `reverb`: send scale.

Layer fields:
- `"sample": "name"`: a fixed sample.
- `"sample": ["a", "b", …]` + `"sample_sel": "expr"`: one name per value of the expression (rounded,
  clamped; up to 24; `""` plays nothing), e.g. the footstep's recording per ground material.
- Any sample name, fixed or `@`, with recorded takes `<name>_1 .. <name>_N` (≤ 16) plays one take
  per seed (see "Recorded samples").
- `"sample": "@"` or `"@_suffix"`: the event's sample name, plus the suffix (a weapon's
  `sound_fire`, its `_distant` variant).
- `when`: an expression; the layer plays when it is > 0.5 at emit.
- `gain_db`: a number or an expression.
- `bind`: patch parameter → expression.
- `random`: `["db" | "mul" | "add", lo, hi]`. One draw per (seed, layer, parameter), so
  re-evaluating a playing voice keeps its offsets.
- `pitch: [lo, hi]`: shorthand for a `mul` random on a sample's pitch.

Every recipe also gets the inputs `distance` (to the listener at emit) and `seed01`.

### Bindings

Expressions: `+ - * / % ^`, comparisons, `&& || !`, `c ? a : b`, and these functions:
- `lerp(x, x0, y0, x1, y1)` (clamped), `clamp`, `min`, `max`, `abs`, `sqrt`, `cbrt`, `pow`, `exp`,
  `log`, `log10`;
- `db(dB)` (to linear), `todb(g)`, `step`, `floor`;
- `sel(i, a, b, …)` (up to 23 choices; equal constants share a slot, so a table may repeat values);
- `mat_contact(m)`, `mat_noise(m)`, `mat_noise_hz(m)`, `mat_modes(m)`, `mat_hard(m)`: material table
  lookups.

Expressions compile once to a stack program and run on the game thread, at emit and whenever a
loop's values change (`audio_voice_params`, `audio_loop_recipe`).

### Events

`assets/audio/events.json` maps the legacy names (`audio_play("impact_hard")`, `fx_sound`,
`EV_SOUND`) to a recipe with fixed inputs. Values the caller passes win. A name not listed plays its
sample through the same chain, so every sound gets distance, air, occlusion and rooms.
`snd_procedural 0` plays only the samples.

### Recorded samples

The recipes' sample layers play real recordings: CC0 takes listed in
`assets/audio/sample_sources.json`, imported by `tools/audio_import/import_samples.py` into
`assets/sounds/<name>_<n>.ogg`, credited in `assets/sounds/CREDITS.md`.

Source: the [CC0-Public-Domain-Sounds](https://github.com/lavenderdotpet/CC0-Public-Domain-Sounds)
mirror at a pinned commit. It includes Kenney's Impact Sounds, OpenGameArt CC0 packs (breaking /
falling / hits, wood and metal, water, mud, creatures) and Warfork's CC0 assets.

| Takes | Played by |
|---|---|
| `footstep_{hard,wood,grass,snow,carpet,metal,mud}` | `footstep` (`sample_sel: material`; dirt, sand, gravel and wading stay synthesized) |
| `impact_{hard,metal,wood,glass,water,flesh,plate_ring}`, `melee_hit(_wall)`, `shard_glass`, `debris_{hard,soft}` | `impact` and the VFX effects' `@` sample |
| `zombie_{groan,growl,scream,attack}` | `zombie_voice` (`sample_sel: call`, pitched by the zombie's voice) |
| `rain_loop`, `hurt`, `door_{open,close,slam}` | their recipes / legacy names |

- **Import** (per file): mono 48 kHz, high-pass, the leading silence trimmed, the tail cut at
  −55 dB or `max_s`, duplicates dropped. Per set, every take is scaled to the same loudness (the
  loudest 50 ms at −14 dBFS) and kept under −1 dBFS. Loops get an equal-power crossfade.
- **Re-import:** `import_samples.py --fetch DIR` (clone) or `--src DIR`, then rebuild.
  `--check` (ctest `audio_takes`) verifies that every set has its takes and credits.
- **Hybrid balance:** each recording sits about level with its synthesized layers, measured with
  `audio_render` against `--no-samples`. The zombie takes sit ~3 dB under. Impact takes follow
  the energy (`todb(sqrt(energy / 400))`) and a per-material offset.
- **Loading:** `audio.c` decodes every take named in the manifest at library load, so none
  decodes mid-game. A name with takes ignores its old single file (the `gen_assets.py`
  placeholder).
- **Still placeholders:** the gunshots (`shot_*`). No CC0 firearm recordings were reachable from
  the build machine; *The Free Firearm Sound Library* (CC0, OpenGameArt) is the candidate. Add it
  to the manifest when it can be fetched.

## Sound families

| Recipe | Model | Fed by |
|---|---|---|
| `gunshot` | Friedlander thump (positive phase ≈ 0.6 ∛(E/100) ms) + pink burst through the device's tilt / low-pass; shorter barrel = louder, longer, brighter, more crackle; suppressor −28 dB (volume), LP 2.6 kHz, burst × 0.4, first-round pop. Mechanism (impulse → metallic modes, a bolt-carrier return) for the shooter only. The weapon's sample and its distant sample are layers. **The tail comes from the environment.** | `bal_world.c` play_report / bal_shooter_report: energy of all pellets, calibre, barrel, device, replicated seed |
| `bullet_crack` | N-wave, duration ∝ calibre · miss^¼, rise from miss | `listen_step` at `bal_crack_segment`'s arrival |
| `bullet_whizz` | band-passed noise sweeping down over the pass | subsonic `bal_whizz_segment` |
| `impact` | material modal bank excited by an impulse of the contact width (√E, head-on rings, grazing scrapes), noise burst, thud for soft ground / flesh; debris grains above ~200 J; splash + Minnaert bubbles on water; the effect's sample layer | `vfx_game_impact`: material, energy that arrived, cos(incidence) |
| `footstep` | heel + toe into the ground's modes, weight thud, granular crunch (gravel / snow / leaves), grass swish, splash when wading; snow / mud from the weather | `chars_take_steps` (anim foot plants; horde zombies by speed), ground ray |
| `vehicle_engine` | firing pulses at rpm · cyl / 120 → asymmetric drive by throttle → exhaust waveguide + body modes, intake roar; `engine_loop` sample pitched by rpm; doppler, exhaust radiates back | `game_vehicles.c` vehicle_audio |
| `vehicle_tires`, `vehicle_wind`, `vehicle_thump` | surface-filtered rolling noise + gravel grains + skid band / squeal; body wind by speed (louder inside); suspension bottoming and a flat tire once per turn | the same |
| `wind`, `rain`, `leaves`, `thunder` | three wind bands with gusts from the wind field; rain hiss + drop grains + drumming on the roof above (metal rings); leaf grains × density × wind; thunder crackle (near) + brown-noise rumble, darker and longer with distance, placed at the strike so it arrives d / c later | `game_weather.c` weather_audio, strikes |
| `water_lap`, `fire` | low-passed swell + bubbles; crackle + hiss + roar | the listener over water; a burning molotov |
| `zombie_voice` | glottal source (jitter / shimmer / fry / breath) → 5 formants whose vowel glides with the call's contour → rasp. Calls: 0 groan, 1 growl, 2 scream, 3 attack bark. Pitch, tract length and roughness come from the zombie's entity id, so it sounds the same on every machine | `game_audio.c` zombie_voices: the nearest 6 |
| `zombie_crowd` | formant-filtered murmur, density by count | the rest, one loop per direction sector |
| `ui_click`, `ui_confirm`, `death_ring` | impulse → band-pass; two sine blips; 3.6 kHz ring with warble and a quiet 2nd harmonic, a 52 Hz lub-dub at the heart rate | `click`; `audio_set_death_mix` |

Networking is unchanged. Every sound derives from replicated events (shots carry their seed) or
from local world state and queries.

## Environment chain

These are the per-voice DSP stages, in order (`env_chain.c`):
1. **Propagation.** A non-doppler voice starts after d / c (or a fixed delay: the crack's arrival,
   the network head start). A doppler voice reads a 1.36 s delay line at the current d / c, which
   gives the arrival time and the doppler shift.
2. **Top cut.** Two one-poles at the minimum of the air absorption cut, the directivity's off-axis
   cut and any cap.
3. **Three bands** (< 250 Hz, 250 Hz..2 kHz, > 2 kHz, complementary one-pole split): air
   absorption at the band centres, occlusion / transmission / diffraction, ground effect.
4. **Gain.** Distance law × directivity × weather masking × volume.
5. **Early reflections.** Up to 6 taps from a 170 ms ring, each a delay, gain and pan; one
   gain-weighted low-pass per ear. The tap loops vectorize.
6. **Direct path.** A spherical-head model per ear (Brown–Duda): a one-pole/one-zero shelf with
   α = 1.05 + 0.95 cos(θ · 180/150) (+6 dB facing the ear, about −20 dB at 8 kHz on the far side,
   less with `spread`, −30 % highs from behind), the Woodworth ITD a/c (|ψ| + sin|ψ|) (0..0.66 ms)
   and a mild broadband ILD of ±22 %.
7. **Sends** to three shared 8-line FDN reverbs (Householder matrix, two-band decay, pre-delay,
   allpass diffusion):
   - zone 0: the listener's room;
   - zone 1: another room (the loudest remote source's);
   - zone 2: the outdoor diffuse field (forest, urban, reflectors).

   The FDN's output is normalized so that a unit impulse sent in leaves RT60 / 13.8 of energy, the
   energy of a diffuse field with that decay. The sends can then be set from physics (below)
   instead of by ear.

Every parameter is a target smoothed at block rate and ramped inside the block. Tap delays glide;
new taps fade in.

## Environment model

| Term | Model (`env_model.c`) |
|---|---|
| Speed of sound | 331.3 √(1 + T/273.15) (ballistics sets it from the weather) |
| Air absorption | ISO 9613-1 α(f, T, RH): band losses at 125 Hz / 1 kHz / 4 kHz; the top cut where the loss exceeds the 4 kHz loss by 6 dB. Humidity comes from rain / wetness / fog. |
| Distance | miniaudio's inverse law, per recipe: min / (min + rolloff (d − min)) |
| Directivity | front_db on axis, rear_db straight behind, linear in cos θ; rear low-pass |
| Ground effect | −2.5 dB per doubling past 20 m on the mid band, outdoors |
| Transmission | per band: tl_base + tl_per_cm × thickness, capped at tl_max per wall, summed over walls (≤ 3) |
| Diffraction | Maekawa: 10 log10(3 + 20 N) − 4.77, N = 2 δ f / c (capped at 25 dB). Power-summed with transmission. |
| Masking | noise floor from rain × exposure and wind; gain × r / √(r² + 1), r = level / floor (not for ambience) |
| Room | box fitted to the 6 axis rays, narrowed by the diagonals; indoor = roof + ≥ 6 of 8 horizontal hits; absorption = mean of the hit materials (open = 1) + 0.05 clutter; Eyring RT60 per band (≤ 4 s) |
| Ground reflection | outdoors, the image source below the ground as an extra tap: delay = (\|S′L\| − \|SL\|) / c; hard ground (concrete, asphalt, water) reflects 0.95 with the full band, soft ground (grass, dirt, snow, sand) 0.8 under a 1.6 kHz low-pass. It gives the comb and the low boost of a real open field. |
| Room send | Sabine critical distance r_c = 0.057 √(Q V / RT60); the send gives D/R = 20 log10(r_c / r) at r m (Q: directivity factor) |
| Outdoor field | `EnvField` (forest, urban, reflector hits, mean distance, snow) → scatter s; RT60 0.3..2.3 s; D/R at 15 m = 26 − 22 s dB (field ~+25, forest ~+6, street ~+2), falling like a room's with distance |
| Early reflections | first-order image sources of the box (closed walls only): delay = (\|S′L\| − \|SL\|) / c, gain = √(1 − α) × \|SL\| / \|S′L\| |
| Outdoor echoes | 24 rays (3° up, 450 m) around the source, clustered by azimuth and distance into reflectors; delay = (\|SR\| + \|RL\| − \|SL\|) / c; gain = reflectivity × size(√rays) × spreading × air, less with snow; ≥ 25 ms after the direct sound. Each echo is a **child voice**: the same recipe and seed, its optional layers dropped, started at the echo's delay, arriving from the reflector. |

The world side (`env_world.c`) works through callbacks in `game_audio.c`:
- **Rays:** static and moving physics bodies, never characters, never the listener's own vehicle.
- **Occlusion:** a ray listener → source, a back-cast for each wall's thickness, three offset rays
  (left, right, over) for a diffraction path. Geometry within `max(0.25 m, self_radius)` of the
  source is what the source sits on, not a wall; a vehicle's own body uses 2.8 m.
- **Occlusion budget:** `snd_occlusion_voices` voices per frame (round robin), and every voice once
  at emit.
- **Room probes:** 12 rays, cached per 2 m cell for 1.5 s. The listener's room is re-probed four
  times a second and immediately after a jump.
- **Echo scans:** cached per 8 m cell for 4 s.
- **Forest and urban:** forest density comes from foliage placements, or from wooden hits close
  around (trunks); urban comes from hard hits within 80 m.

The listener state (`game_audio_frame`) feeds the world-bus filter:

| State | Effect |
|---|---|
| Underwater | LP 450 Hz, +3 dB lows |
| In a vehicle | LP 2.4 kHz, +4 dB lows |
| Low health | LP falls to 3.5 kHz |
| ADS | −1 dB highs |
| Helmet (`snd_helmet`) | −4 dB highs |
| Death / downed mix | its own low-pass and gain |

## Mixer

- **Voices:** 128 slots, each with up to 4 layers. Slots are allocated on the game thread; when the
  pool is full, the voice with the lowest score (priority × audibility, loops +0.2) is stolen.
- **Command ring:** 2048 entries, game → audio. Each voice publishes the generation that finished
  and its peak level.
- **Virtual voices:** a voice whose audibility falls below 3·10⁻⁵ keeps its time but renders
  nothing. A virtual one-shot older than 8 s is stopped.
- **No allocation:** `mixer_render` never allocates; the tests count through an allocator hook.
- **Degradation** (`snd_degrade -1`, automatic): above 18 % of the buffer, optional layers are
  dropped; above 30 %, early reflections are capped at 2; under 10 %, both return.
- **Without a device** (`--test` / `--nosound`), the mixer still runs offline, paced by the frame
  time (`snd_offline`). Script tests therefore play voices for real, and `audio_record` captures
  them.

## Voice chat

Push to talk (`Talk`, **Y** by default; Controls tab). Close by you hear a player's voice in the
world; far away it comes over the radio.

- **Capture** (`src/audio/voice_capture.c`): the default microphone through miniaudio at 16 kHz mono
  16-bit, into a lock-free ring. It opens on the first push to talk and stays open.
- **Codec** (`src/audio/voice_codec.c`, pure): IMA ADPCM, 20 ms frames of 163 bytes (65 kbit/s, about
  33 dB SNR on a voice-like signal). Every frame carries its own decoder state, so a lost packet costs
  only its own 20 ms. Two frames make one packet (40 ms, 25 packets a second).
- **Network** (`MSG_VOICE`, unreliable, protocol 37): a client sends its packets to the host. The host
  plays them and relays them, with the slot, to every other welcomed client. The host's own voice
  goes straight to every client.
- **Playback** (`mixer_stream_*`): one stream per player slot. Packets are decoded, upsampled to
  48 kHz and pushed into the stream's ring. The mixer starts it after an 80 ms jitter buffer and skips
  ahead when more than 0.4 s is queued. The voice is split by `radio` (0..1) between two paths:
  - **proximity:** the stream's environment chain, from the talker's head (`chars_head_world`), the
    same model as every other sound: distance, walls, panning, room reverb sends.
  - **radio:** band-passed 350 Hz .. 3.2 kHz (two biquads each side), driven (tanh), a low hiss while it
    plays, a squelch burst at the start (50 ms) and at the end (120 ms), centred, at a steady level.
- **Distance** (`src/game/game_voice.c`): `radio` is a smoothstep from `voice_radio_dist` (30 m) over
  `voice_radio_fade` (10 m). A player this machine no longer replicates (far away) is on the radio.
- **Settings:** Audio tab: Voice chat on / off (`voice_chat`), Microphone gain (`voice_mic_gain`),
  Voice volume (`snd_voice`, the voice bus).
- **HUD:** the talkers on the left, a green dot (in the world) or amber with RADIO; "You" while the
  key is held.
- **Tests and tools:** `test_voice` (the codec, the upsampler, the stream's jitter buffer, the radio
  band). `voice_info` logs the microphone, the packets sent / received and each talker's distance,
  level and radio amount. `voice_test_tone 1` replaces the microphone with a test voice, and `2` also
  talks without the key. `voice_loopback 1` plays your own voice `voice_loopback_dist` metres in front
  of you (past 30 m: the radio).

## Tools

| Command | What it does |
|---|---|
| `audio_synth_play <recipe> [key=value …] [@sample] [2d] [at=x,y,z] [seed=n] [volume=v] [self=m]` | Play anything, by recipe, events name or sample. |
| `audio_editor` | Recipe picker, event input sliders, seed, 3D / 2D, A/B against a sample, offline render (waveform + log spectrogram), live edits of the layers' patch values, Save patch / Revert. |
| `audio_env_preview` | Distance, azimuth, source facing, walls (count, material, thickness), diffraction detour, a box room (size, material), a reflector behind you, humidity / temperature / rain / wind. Shows gain / top cut / bands / delay / taps / RT60 / echo and plays the recipe through exactly those parameters (`AudioEmit.env`; the room's reverb is pinned for 6 s). |
| `audio_debug 1` / `2` / `3` | 1: voice readout (distance, delay, gain, top cut, bands, walls, zone, peak), the listener's room and the forest / urban field. 2: adds the direct paths (green clear, red through walls, yellow around an edge, cyan echoes). 3: adds the probe rays, room box and echo scan. |
| `audio_voices` | Logs every voice's chain readout and the mixer stats. |
| `audio_env_probe` | Logs the room estimate and the discrete echoes at the listener. |
| `audio_record <file.wav> <s>` | Captures the last ≤ 4 s of output. |
| `snd_trace 1` | Logs each voice's start: delay, gain, top cut, occlusion, zone. |
| `audio_render` (host tool, `tools/audio_render`) | Renders any recipe offline through the real chain and mixer to a stereo WAV, in a mock scene: `--scene field\|valley\|forest\|urban\|corridor\|room\|wall\|indoor_out\|anechoic`, `--dist`, `--az`, `--hs`, `--dur`, `--seed`, `--repeat n --every s`, `--ramp key=a:b` (loops: rpm, wind), `--chain off`, `--normalize dB`, `--no-samples`. |
| `analyze.py` | Objective measures of a WAV: peak / RMS / crest, onset and rise, time above −20 / −40 dB, octave-band shares and T60 (causal filters + Schroeder), centroid, ILD, D/R. |
| `render_pack.py <dir>` | The listening pack: ~60 renders (every family, distances, scenes, A/B) as MP3 + a README of what to listen for. |
| A/B switches | `snd_env 0` (chain off: distance law and pan only), `snd_echoes`, `snd_echo_max`, `snd_procedural 0` (samples only). |

## Dev room

**Sound test bay**, in the hall at x 4..36, z −17..−7:
- floor strips: concrete, wood, thin / thick metal, gravel, grass, sand, dirt, tile, asphalt;
- impact panels 6.5 m north of them: concrete, brick, plaster, metal, wood, glass, tile, cloth,
  asphalt (stand behind one for occlusion);
- a tiled reverb room, 6 × 3 × 5 m inside, with its door off-axis;
- a metal-roofed rain shelter (`weather_set rain 5`);
- a car at (18, 0.3, −3).

**Audio range**, north-west, pad at (−230, 0, −230):
- a 40 m cliff face 145 m west (echo at +0.85 s);
- a closed 24 × 3 × 3 m concrete corridor north;
- a brick alley south (slaps);
- 64 trunks south-west (scatter);
- material panels north-east;
- a car east;
- an open field at (−700, 0, 300), nothing within 450 m.

## Tests

`test_voice`: see "Voice chat".

`test_synth`:
- the compiler orders out-of-order nodes and rejects cycles, self-feeds, modulation cycles, bad
  types / parameters / inputs and oversized state; writes and re-parses a patch;
- biquads match the RBJ reference coefficients (< 1e-6) and the low-pass is −3 dB at fc; SVF and
  one-pole responses;
- envelope timing (10 ms / 110 ms), the gate holding the sustain and releasing in 50 ms;
- modal T60 and frequency, material rings;
- pink and brown slopes;
- expressions;
- every shipped patch and recipe loads and renders finite, bounded audio;
- the gunshot is bit-identical per seed and varies across seeds; its level and low end grow with
  energy; the suppressor is quieter and darker, a brake brighter, a hider darker; the `@` sample
  layer plays;
- recorded takes: counted, the same take per seed, every take picked; a name without takes plays
  itself; `sample_sel` picks a list entry (`""` and past the end play nothing); a list with no
  sample at all is dropped;
- the mixer: start delay = d / c to the sample; finished voices free their slots; stealing by
  score; stop fades; **no allocation in `mixer_render`**;
- hostile parameters stay finite, and there are no denormals after 20 s of decay;
- a smoothed gain jump puts > 70 dB less energy above 3 kHz than the tone, and ≥ 20 dB less than an
  unsmoothed jump;
- the `death_ring` patch (pitch, pop-free attack and stop, heartbeat);
- 64 voices with chains within 1.96 ms per 10 ms buffer, and the chain alone within 15 µs per voice.
  Both budgets are scaled by the machine's measured speed against a desktop.

`test_audio_env`:
- ISO 9613-1 table within 8 %; humidity; top cut; distance law; directivity; ground; masking;
- transmission matches the material table, walls add, glass vs drywall vs concrete; diffraction;
- box room dims, RT60, the floor reflection's tap delay, hard vs padded rooms, the open field;
- the dev room corridor rebuilt from `devroom.json`, within 20 % of its real size;
- a cliff at 150 m echoes at 2d / c ± 3 %, snow damps it, an open field has no echo;
- the audio range pad's echo at 2 × 145 m / c;
- the delay = d / c of a world voice; occlusion through a wide wall matches the table, a narrow
  wall diffracts; same room gives taps and the room send;
- the chain's band gains, pan, doppler pitch, a click-free gain step; the FDN's −30 dB per 0.5 s at
  RT60 1 s;
- the head model: far-ear highs shadowed, ITD sign and size, left / right symmetry;
- the ground tap's delay is the image path difference, grass darker and weaker than concrete;
- the room send grows with distance and falls with volume; outdoors the field is driest, then a
  forest, then a street; the outdoor send grows with distance.

`tools/tests/audio_range.cfg` + `audio_range_check.py`:
- in the game: corridor and tiled-room estimates within 20 %;
- the cliff echo at the pad;
- every family plays;
- a footstep on a floor strip is not occluded;
- the recorded takes load (> 100), and footsteps, impacts and zombies play them (`snd_trace`
  lists each voice's samples).

## Tuning guide

- **Level:** each patch's `amp` / `gain` and the recipe's `gain_db` per layer. Aim for patch peaks
  around 0.5-1. The mixer soft-clips above 0.8 on the master.
- **Hybrid balance:** put the mechanical character in the sample layer (`"@"`) and the physics
  (energy, distance, device) in the synthesized layers. Push the sample down with distance
  (`lerp(distance, …)`) and let the distant sample and the environment take over.
- **Materials:** edit `assets/audio/materials.json`, then run `audio_reload`. `absorb` drives rooms;
  `reflect` drives outdoor echoes; `tl_*` drives walls; `modes`, `contact_ms` and `noise` drive
  impacts and footsteps.
- **Rooms that ring too long or too short:** the 0.05 clutter term and the 4 s cap in
  `env_room_estimate`, plus the room send scale (0.3) in `env_world_voice_eval`.
- **Echo density:** `snd_echo_max`, the 25 ms merge window, the cluster tolerance (20 % + 3 m) in
  `env_outdoor_echoes`.
- **CPU:** see `audio_voices` (mixer µs per buffer) and `audio_debug 1`. Mark expensive detail
  layers `optional`, raise `max_instances` carefully, and keep loops few. One-shots far beyond
  their recipe's `max` distance are not started.
