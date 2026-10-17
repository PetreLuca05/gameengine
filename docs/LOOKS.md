# Looks (post-processing presets)

A **Look** decides how the image is graded and finished. It never decides what runs: that is
Engine Fidelity. It is also independent of the era look (geometry, foliage). There are five Looks,
**Realism, PUBG-style, Cinematic, Vibrant and Gritty**, all compiled into the game as constants.
The player picks one from a dropdown. Nobody edits a Look's parameters, creates a Look, or loads
one from a file. There is no "Custom" state.

| File | Role |
|---|---|
| `src/render/post/look.c/.h` | Pure: `LookParams`, the analytic grading stack, the LUT bake / shaper, finish + fidelity caps, modulators, dev reflection (`look_format`) |
| `src/render/post/looks.c` | The five `static const` tables (the only place a Look is defined) |
| `src/render/post/post_look.c` | GPU: one baked LUT per look (rebaked when invalidated), the Settings preview strip pass |
| `shaders/post/look.glsl` | The LUT fetch (shaper + texel centres), shared by the passes below |
| `shaders/post/tonemap.comp` | The look's LUT replaces the tonemapper; bloom threshold / tint, halation, letterbox, feedback colours |
| `shaders/post/final_direct.frag` | Engine Fidelity Off: the same LUT replaces the plain Reinhard (one fetch) |
| `shaders/post/look_preview.comp` | The preview strip (the scene under every look) |
| `src/game/game_look.c` | `r_look`, `look`, finish into `PostSettings`, modulators, A/B, dev `look_dump` / `look_tune` |
| `src/ui/screens/scr_settings.c` `look_section` | Settings > Graphics > Post-processing & Effects > Look |
| `tests/test_looks.c` | ctest `looks` (dev build) and `looks_release` (tuning compiled out) |
| `tools/tests/lint_looks.py` | ctest `lint_looks`: no disk I/O, five tables, dev tools gated, `r_look` ownership |
| `tools/tests/run_looks_tests.py`, `tools/tests/looks.cfg` | In-game: every look x 4 times of day x 3 weathers x every fidelity level under validation, frames, the grading-pass budget |

Cvars and commands:
- `r_look` (saved): `realism` (default), `pubg`, `cinematic`, `vibrant`, `gritty`. It is the only persisted Look setting.
- `look <name>` sets the Look. With no argument it lists the five.
- `r_look_modulators` (not saved, 1): set it to 0 for the static grade (tests).
- Dev builds only (`DG_LOOK_TUNE`, every configuration except CMake `Release`):
  - `look_dump [name|all]` prints a Look.
  - `look_tune ...` overrides its constants live.
  - See "Tune, then commit".

## The grading stack

All five Looks share one ordered pipeline. It turns exposed scene-linear HDR (Rec.709) into
display-linear output. `look_eval` (look.c) is the reference implementation.

1. **Exposure and white balance.** Exposure compensation (`exposure_ev`). White balance
   (`wb_temp` / `wb_tint`, −1..1) uses the same LMS model as the shader's `white_balance`:
   temperature > 0 is warm, tint > 0 is magenta.
2. **Tonal**, in log2 stops around mid grey 0.18:
   - Pivot contrast (`contrast`, `pivot_ev`), applied per channel.
   - Lift / gamma / gain as per-channel offsets in EV on soft luminance masks. Shadows fade out by
     `shadow_end`, highlights start at `highlight_start`, both transitions `range_soft` EV wide,
     and midtones are the rest.
   - Highlight roll-off: a hue-preserving shoulder on luminance above `rolloff_start` EV.
3. **Colour**:
   - Saturation around luminance.
   - Vibrance: weighted by (1 − saturation), or by saturation when negative. Skin protection (a
     Gaussian around the 30° orange hue) spares skin.
   - Hue-vs-hue: a rotation about the grey axis.
   - Hue-vs-sat and sat-vs-sat. The hue curves have 8 periodic Catmull-Rom knots every 45° of
     opponent hue (0 red, 45 orange, 90 yellow-green, 135 green, 180 cyan, 225 sky blue,
     270 blue-violet, 315 magenta). Sat-vs-sat has 5 linear knots.
   - Colour balance per range: a luma-neutral chroma push on the shadow / mid / highlight masks.
   - Split-toning: a hue and an amount for shadows and for highlights, around `split_balance` EV.
   - Chroma boosts approach the gamut edge smoothly (tanh) instead of clipping. That leaves no
     kink for the LUT to miss.
4. **Tonemapper**: ACES fit, AgX, Khronos PBR Neutral, or a Hable filmic curve with `tm_toe` /
   `tm_shoulder`. The output is display-linear 0..1.
5. **Creative LUT** (`lut`, `lut_strength`): a procedural LUT shipped with the preset, defined in C
   and applied on sRGB-encoded values.
   - *Print film*: a soft S-curve, warm highlights / cool shadows, slightly desaturated highlights.
   - *Bleach bypass*: luminance overlaid on the colour.
6. **Output levels**: `black_point` (with `black_tint`; > 0 gives milky shadows) and `white_point`.

Stages 1-6 are **baked once into a 32³ LUT per look**. The LUT input is a log2 shaper from
−11 to +7 EV around 0.18 (`look_lut_coord`, `look.glsl`), sampled at texel centres. The tonemap pass
replaces `tonemap(c)` with one trilinear fetch. Nothing is evaluated per pixel.

**When the LUTs are baked:**
- At startup (all five, about 60 ms).
- When `render_looks_invalidate` is called: on a `look_tune` edit, and when the Engine Fidelity
  level changes (`game_look_frame`).
- The bake has no level-dependent inputs today, so a fidelity change rebakes identical LUTs.

All five LUTs stay resident (5 × 256 KB), so switching Looks is instant.

The rest of the frame keeps working around the Look:
- The live grade uniforms (white balance, contrast, saturation) still run before the LUT: post
  volumes, the day / night cycle, photo mode and the Look modulators drive them.
- Lift / gamma / gain from volumes, the user `.cube` LUT (`r_lut`), the colour-blind filter and
  brightness / gamma run after it, as before.
- A photo-mode tonemapper override switches the Look's LUT off for the photo.
- The `r_post_compare` split still shows the raw scene on its left half.

**Finish.** Each finish effect is a `PostSettings` field, set by `look_apply_post` (look.c):
- Bloom intensity, threshold (exposed scene units, soft knee) and tint.
- Vignette, film grain (final.frag), chromatic aberration, sharpening (CAS, final.frag).
- Halation: a red-orange glow where the wide bloom is bright.
- Letterbox: 2.39:1 bars on the 3D image only, the HUD stays.
- The colours of the gameplay feedback: the damage edge tint and the low-health vignette colour.
  The behaviour (timing, strength, `r_post_feedback`) is unchanged.

The Graphics sliders scale the Look's finish. Each strength is the cvar divided by its default:
`r_bloom_intensity`, `r_vignette`, `r_film_grain`, `r_chromatic`, `r_sharpen`. At the defaults the
Look's own values apply. At 0 the effect is off, which is also what Engine Fidelity forces when the
feature is unavailable at the level. The Tonemapper control left the menu: the tonemapper is part of
the Look. `r_tonemapper` remains for `r_post_compare` and photo mode.

**Modulators** (`look_modulate`) scale the live grade from the sky and the weather. Each Look has
four condition sets, applied with these weights:
- **golden hour**: sun elevation −3° to 18°, clear-ish sky.
- **overcast**: cloud cover above 0.45, by day.
- **storm**: lightning, or heavy precipitation with wind.
- **night**: 1 − daylight.

Each set lists temperature / tint / exposure deltas, saturation / contrast / bloom / grain scales
and a vignette delta (`look_apply_mod`). Realism has none: it is faithful to the sky, which the
day / night cycle already lights. Damage and low health keep their own behaviour.

## Fidelity

Engine Fidelity bounds the Look (`look_caps`):

| level | grade | finish |
|---|---|---|
| Off | the baked LUT in `final_direct.frag` (one fetch; nearly free) | none: no bloom, vignette, grain, CA, halation, sharpening or letterbox |
| Low | LUT | bloom ≤ 0.04, grain ≤ 0.03, no CA, no halation |
| Medium | LUT | bloom ≤ 0.06, grain ≤ 0.08, no CA, halation ≤ 0.2 |
| High / Modern | LUT | the Look's values (CA ≤ 2) |

On top of these caps, the fidelity table still owns each feature's cvar. For example, chromatic
aberration is unavailable below High, so its strength is 0 there. A Look never writes a fidelity
or era cvar. It only fills its own `PostSettings` fields (`tests/test_looks.c` "fields").

## The five Looks

Every Look is a departure from **Realism**, the reference. Descriptions are the one-liners shown in
the menu.

- **Realism**: "Neutral and photographic: true sky white balance, natural colour, no lens effects."
  - AgX with a gentle roll-off above +4 EV and no extra contrast.
  - True white balance (the sky's, from the day / night cycle) and natural saturation, vibrance +5.
  - Minimal bloom, no vignette, grain or CA, mild sharpening, no modulation.
  - With modulators off it is the plain AgX tonemapper within 0.007 on a grey ramp.
- **PUBG-style** (inspired by PlayerUnknown's Battlegrounds; our own implementation): "Muted and
  slightly washed: milky shadows, cool green-grey mids, soft highlights. Made for open terrain."
  - Desaturated (0.84, vibrance −5, high saturations compressed).
  - Lifted, slightly cool black (0.008 linear ≈ 0.08 encoded) with the shadows raised 0.4 EV, and
    low-medium contrast (0.78 on ACES). Overall about as bright as Realism (+0.6 EV against ACES's
    darker mids), just washed.
  - A green-grey cast in the mids (green tint, a cool / green mid balance) with slightly warm
    highlights, and a soft highlight roll-off.
  - Foliage pulled towards olive (hue −4..−6° on yellow-greens / greens) and muted.
  - Restrained bloom (threshold 1), faint vignette, very light grain, slightly soft.
  - Overcast flattens it further (saturation −15 %, contrast −10 %).
- **Cinematic**: "Filmic: rich contrast, teal shadows and warm highlights, soft halation,
  letterboxed."
  - Hable filmic (toe 0.28, shoulder 0.22) with contrast 1.1.
  - Crushed-but-not-black shadows: a teal-tinted black point of about 0.06 encoded.
  - Split-toning: shadows teal (190°, 0.14), highlights orange (30°, 0.11).
  - Greens pushed towards teal and muted. Skin protected (0.9), print-film LUT at 0.35.
  - Warm, stronger bloom (0.07), halation, vignette, fine grain, letterbox.
  - Golden hour: warmer (+0.12), bloom +60 %, more saturation.
- **Vibrant**: "Punchy: saturated greens and sky blues, bright mids, clean blacks, crisp detail."
  - ACES with bright mids (+0.7 EV, mids +0.35 EV, shadows +0.3 EV), so it is brighter than
    Realism, with ACES's steeper mid slope and clean blacks.
  - Saturation 1.12 and vibrance +20.
  - Vivid greens and sky blues (hue-vs-sat up to 1.2), with very high saturations eased off.
  - Strong sharpening, minimal grain, clean bloom.
  - Overcast is compensated (saturation +8 %).
- **Gritty**: "Harsh and drained: only fire and blood keep their colour, dark shadows, heavy grain.
  Storms make it worse."
  - Saturation 0.45, but hue-vs-sat ×2.1 on reds and ×1.8 on oranges: fire, blood and muzzle flash
    keep their colour while greens and blues lose half or more.
  - Contrast 1.2 around −0.3 EV, with shadows pulled down 0.1-0.15 EV.
  - Cool-grey balance and bleached highlights (bleach bypass 0.35, white point 0.97).
  - Heavy grain, strong vignette, slight CA.
  - Storm: saturation −25 %, contrast +12 %, grain +60 %, vignette +0.1, cooler.

### Tables (`src/render/post/looks.c`)

`-` = zero / off. Hue knots run 0, 45, ... 315°, sat knots 0, .25, .5, .75, 1. Modulator sets list
temp, tint, exposure, saturation, contrast, bloom, grain, vignette.

| parameter | Realism | PUBG-style | Cinematic | Vibrant | Gritty |
|---|---|---|---|---|---|
| exposure (EV) | 0 | 0.6 | 0.35 | 0.7 | 0.1 |
| wb temp / tint | - | -0.03 -0.03 | - | - | -0.08 0 |
| contrast / pivot EV | 1 0 | 0.78 0 | 1.1 0 | 1 0 | 1.2 -0.3 |
| lift (EV, rgb) | - | 0.4 0.4 0.4 | - | 0.3 0.3 0.3 | -0.15 -0.12 -0.1 |
| gamma (EV, rgb) | - | 0.15 0.15 0.15 | - | 0.35 0.35 0.35 | - |
| gain (EV, rgb) | - | - | - | - | - |
| shadows end / highlights start / soft (EV) | -2.5 2 2 | -2 1.5 2 | -2 1.5 2 | -2.5 1.5 2 | -1.5 1.5 2 |
| highlight roll-off / start EV | 0.1 4 | 0.35 1.5 | 0.25 2 | 0.15 2.5 | 0.2 1.5 |
| saturation / vibrance / skin | 1 0.05 0.8 | 0.84 -0.05 0.8 | 1 0.08 0.9 | 1.12 0.2 0.7 | 0.45 -0.1 0 |
| hue-vs-hue (deg) | - | 0 0 -4 -6 0 0 0 0 | 0 0 4 8 0 0 0 0 | - | - |
| hue-vs-sat | 1 1 1 1 1 1 1 1 | 1 1 0.9 0.88 0.92 0.9 0.95 0.95 | 1 1.05 0.92 0.85 1.05 1 0.95 0.9 | 1 1 1.1 1.15 1.1 1.2 1.05 1 | 2.1 1.8 0.8 0.7 0.6 0.55 0.6 1.4 |
| sat-vs-sat | 1 1 1 1 1 | 1 1 0.97 0.92 0.85 | 1 1 1 0.96 0.9 | 1 1.05 1.05 1 0.95 | 1 1 1 1 1 |
| balance shadows | - | -0.01 0 0.012 | -0.02 0.005 0.015 | - | -0.015 0 0.02 |
| balance mids | - | -0.025 0.015 0 | - | - | -0.01 0 0.01 |
| balance highs | - | 0.025 0.008 -0.03 | 0.01 0 -0.01 | - | - |
| split shadow hue / amt | - | - | 190 0.14 | - | - |
| split highlight hue / amt | - | - | 30 0.11 | - | - |
| tonemapper | AgX | ACES | Filmic | ACES | ACES |
| filmic toe / shoulder | - | - | 0.28 0.22 | - | - |
| creative LUT | - | - | print film | - | bleach bypass |
| LUT strength | 0 | 0 | 0.35 | 0 | 0.35 |
| black / white point | 0 1 | 0.008 0.98 | 0.005 1 | 0 1 | 0 0.97 |
| black tint | 1 1 1 | 0.95 1 1.03 | 0.85 1 1.05 | 1 1 1 | 1 1 1 |
| bloom / threshold | 0.03 0 | 0.035 1 | 0.07 0.8 | 0.05 1 | 0.035 1.2 |
| bloom tint | 1 1 1 | 1 0.98 0.94 | 1 0.92 0.82 | 1 1 1 | 1 0.95 0.9 |
| vignette | 0 | 0.12 | 0.3 | 0.05 | 0.45 |
| grain | 0 | 0.015 | 0.035 | 0.005 | 0.12 |
| chromatic aberration | 0 | 0 | 0.05 | 0 | 0.25 |
| sharpen (CAS) | 0.3 | 0.1 | 0.15 | 0.55 | 0.45 |
| halation | 0 | 0 | 0.35 | 0 | 0 |
| letterbox 2.39:1 | - | - | on | - | - |
| damage tint | 1.3 0.25 0.2 | 1.25 0.3 0.22 | 1.2 0.2 0.18 | 1.35 0.25 0.2 | 1.4 0.15 0.12 |
| low-health vignette colour | - | 0.05 0 0 | 0.08 0.01 0 | - | 0.12 0 0 |
| golden hour {temp tint EV sat con bloom grain vig} | - | 0.04 0 0 0.03 0 0 0 0 | 0.12 0 0.1 0.08 0.05 0.6 0 0 | 0.06 0 0 0.05 0 0 0 0 | 0.03 0 0 0 0 0 0 0 |
| overcast | - | -0.03 0 0 -0.15 -0.1 0 0 0 | 0 0 0 0 -0.05 0 0 0 | 0 0 0 0.08 0.03 0 0 0 | 0 0 0 -0.1 0.05 0 0 0 |
| storm | - | 0 0 0 -0.1 -0.05 0 0 0 | - | - | -0.06 0 -0.15 -0.25 0.12 0 0.6 0.1 |
| night | - | -0.03 0 0 0 0 0 0 0 | -0.08 0 0 -0.1 0 0 0 0 | - | -0.04 0 0 0 0 0 0 0 |

What `tests/test_looks.c` measures:
- **Reference frame** (sky, grass, canopy, dirt, skin, deep shadow, clouds, concrete; mean HSV
  saturation, 1st-percentile and black-input luma, display-encoded):

  | look | mean saturation | 1 % black | black | mean luma |
  |---|---|---|---|---|
  | Realism | 0.268 | 0.003 | 0.000 | 0.461 |
  | PUBG-style | 0.178 | 0.091 | 0.086 | 0.453 |
  | Cinematic | 0.276 | 0.064 | 0.061 | 0.453 |
  | Vibrant | 0.392 | 0.000 | 0.000 | 0.497 |
  | Gritty | 0.168 | 0.000 | 0.000 | 0.378 |

- **Cinematic split**: shadow hue 202° (teal), highlight hue 33° (orange).
- **Gritty chroma vs Realism**: red 0.95, orange 0.97, green 0.32, blue 0.50.

## LUT accuracy

`test_looks` compares the baked LUT against `look_eval`. The LUT is read with the GPU sampler's math
(trilinear on the shaper coordinates, texel centres), and differences are measured on
display-encoded values:

| look | grey ramp max | colours p50 | colours p99 |
|---|---|---|---|
| Realism | 0.004 | 0.002 | 0.004 |
| PUBG-style | 0.005 | 0.001 | 0.005 |
| Cinematic | 0.006 | 0.002 | 0.013 |
| Vibrant | 0.008 | 0.002 | 0.016 |
| Gritty | 0.018 | 0.001 | 0.019 |

The test's tolerances are a grey ramp under 0.02 (5/255), a median under 0.005 and p99 under
0.03. The rare larger differences (max ≈ 0.09) are on extreme saturated colours, where the
tonemapper clips a channel at the display gamut edge. The 0.58 EV grid rounds off that corner, so
the LUT is the smoother of the two. A 48³ or 64³ LUT barely moves them (kinks, not resolution) and
triples the bake time. Gritty's grey-ramp figure is plain curvature: contrast, ACES and the bleach
overlay make the steepest mids.

## UI

Settings > Graphics > Post-processing & Effects > **Look**:
- A dropdown with the five Looks and the selected one's description.
- The **preview strip**: the current scene graded by each Look, in five low-res tiles (about 108 px
  high, the output aspect). The selected tile is outlined, and a click picks it. The strip comes
  from `render_look_preview`, a `look_preview` compute pass that runs only while the menu shows it.
  It reads the exposed pre-tonemap scene, or the lit HDR with the fixed exposure at Fidelity Off.
- **Hold to compare with Realism**: Realism renders while the button is held (`game_look_compare_hold`).

There are no sliders, no parameter panel and no import / export.

## Tune, then commit

The constants live in C. Tuning happens in a dev build and the values go back into the source:

1. Pick the look: `look cinematic`.
2. Override fields live:
   - `look_tune split_shadow_amt 0.16`
   - `look_tune lift -0.05 0 0.02` (a vector)
   - `look_tune hue_sat 3 0.8` (knot 3)
   - `look_tune` alone lists every field and its current value.
   - The LUTs rebake on the next frame (about 60 ms).
3. `look_dump` (or `look_tune print`) prints the look as the C initializer of `looks.c`, marked
   "tuned, not in looks.c yet". Paste it over the table in `src/render/post/looks.c`, rebuild,
   and run `ctest -R looks`. The tests pin each Look's character (PUBG-style lifted / muted,
   Cinematic teal / orange, Gritty keeps reds, and so on) and the LUT accuracy.
4. `look_tune reset [all]` returns to the compiled constants. Nothing is ever written to disk.

Release builds (CMake `Release`) compile with `DG_LOOK_TUNE` off:
- `look_tune`, `look_dump`, the override table and the field reflection do not exist.
- `look_get` is the constant table.
- `test_looks_release` builds the module that way and checks it, and `lint_looks.py` fails if any
  of that code leaks outside its `#if DG_LOOK_TUNE` blocks.

## In-game tests

`python tools/tests/run_looks_tests.py [build_dir] [--skip-validation] [--res WxH]`, after a build,
runs four checks:
1. **unit**: `test_looks`, `test_looks_release`, `lint_looks.py`.
2. **matrix**: every look × 4 times of day (06:30, 12:00, 19:30, 00:30) × 3 weathers (clear,
   overcast, storm) × every Engine Fidelity level (Off .. Modern), under the Vulkan validation
   layers. The log must show no validation message, every combination must reach its frame, and
   `r_fidelity` / `r_era` must keep their values across look switches.
3. **frames**: a screenshot per look at noon, clear sky, Modern. PUBG-style must be less saturated
   with lifted blacks compared with Realism, Cinematic letterboxed, Gritty less saturated.
4. **bench**: `post_bench` per look at 1080p. The `tonemap` pass (the whole grade + finish) must
   stay ≤ 0.15 ms (`--no-budget` skips the check on slow / software GPUs).

`tools/tests/looks.cfg` is the hand-run showcase: every look in the dev room and the yard at a few
times of day, with screenshots.
