# Night optics: night vision goggles + thermal scope

Status: **plan, not implemented yet** (awaiting approval). This file becomes the
system doc once the code lands; sections marked *Plan* describe intent.

Two devices, two physical models, never a colour filter:

| | NVG (`nvg`, key 8) | Thermal scope (`scope_thermal`, optic) |
|---|---|---|
| Physics | image intensifier: photons 0.5..0.9 µm (visible red + near-IR) amplified ~30 000x | microbolometer: emitted 8..14 µm (LWIR) radiance, no light needed |
| Where | full screen post profile, replaces exposure / bloom / tonemap / look | scope PiP only (own thermal render pass); the rest of the view stays normal |
| Sees | moon / stars / airglow, lamps, IR illuminators, IR-reflective foliage | temperature x emissivity; glass is opaque, polished metal mirrors the cold sky |
| Cannot | see in total darkness without the illuminator, see through smoke well, see colour | see through glass, read texture / print detail |

## Gameplay rules

- **8 toggles NVG** (own rebindable action `ACT_NVG`, default 8) when the
  inventory holds an `nvg` with charge left (the first one found in grid order).
  Tap = goggles on / off; hold 0.4 s = IR illuminator on / off (only while on).
- **No ADS at all while the goggles are down or flipping** (any sight: iron,
  red dot, scope, thermal). The ADS input is cleared client-side before the
  UserCmd is built and the host's weapon sim refuses ADS while `EF_NVG` is set,
  so prediction agrees. First blocked press shows the hint "NVG down: no aim".
- Flipping takes 0.6 s (hand-pose case `nvg_flip`); weapon stays in the right
  hand, the left hand goes to the brow. Flip up re-allows ADS when it finishes.
- Battery: charge lives in the item instance's `dur` (per-mille, already on the
  wire and drawn as a bar in the grid). Drains while on, faster with the
  illuminator. Empty -> goggles go dark (image fades over 1 s) and switch off.
  Grid context action "Replace battery" consumes one `battery_aa`.
- Tube protection: extreme brightness for > 3 s (daylight, a flashlight in the
  face) shuts the goggles off with a HUD warning; a whiteout precedes it.
- Thermal: zombies run cold (default 24 °C core, configurable 20..28) and read
  dimmer than people (34..36 °C skin). Glass blocks thermal. Foliage is cold at
  night but blocks what is behind it. Heat persists: parked engines, corpses,
  where a car stood, footprints for a minute.
- HUD while NVG is on: opacity x0.35, compass / kill feed / hints hidden,
  vitals + NVG battery chip kept (a `hud_rules` block in assets/ui/hud.json).
- Accessibility: one cvar `nvg_brightness_floor` (0..0.15, `CVAR_EDITABLE`).
  No other settings: the look is fixed.

## 1. NVG image-intensifier model (*Plan*)

Runs in `src/render/post/post_nvg.c` + `shaders/post/nvg_*.comp`, inserted in
`post_resolve` after `post_lens_build` (linear HDR, pre-exposure, scope already
composited). When active it replaces exposure / bloom / flare / tonemap / look
and writes `ldr` directly; final.frag still runs (its grain is disabled).
The old debug `nightvision` command (a green tint in tonemap.comp) is removed.

### 1.1 Photocathode input (flux Φ)

`Φ = k_vis · dot(hdr_linear, w_GaAs) + k_nir · NIR`

- `w_GaAs = (0.55, 0.35, 0.10)` (R, G, B): a Gen-3 GaAs photocathode is most
  sensitive in red / near-IR, so red lamps and sodium light read bright and
  blue sky light weak.
- `NIR` is a separate near-IR irradiance image (1.2). Moonlight is ~50 % near-IR
  and starlight + airglow is mostly near-IR: that is why NVG works on a moonless
  night when the visible image is black.

### 1.2 Near-IR layer

- **G-buffer:** the prepass gets an extra **RG8 `nir` target**, allocated and
  written only while the local NVG is on (pipeline variant, no cost otherwise):
  `R = ir_reflectance`, `G = ir self-emission` (lamp bulbs, embers, fire,
  exhaust, IR strobes).
- **`ir_reflectance`** per material: explicit value from the material /
  thermal class table (`assets/thermal/materials.json`) when known, else
  derived from albedo: `clamp(luma(albedo)·1.2, 0.05, 0.7)`, with a **foliage
  boost** (chlorophyll "red edge": leaves and grass ~0.5..0.6 regardless of
  how dark green they are, so foliage glows on NVG).
- **Lights:** `PointLight` / `SpotLight` get `ir_emission` (ratio of near-IR to
  visible output) and a flag `LIGHT_IR_ONLY`. Defaults by light type:
  incandescent / fire 3.0, sodium 0.6, fluorescent 0.2, LED 0.05, IR
  illuminator only IR. IR-only lights contribute nothing to the visible HDR.
- **NIR pass** (`nvg_nir.comp`, deferred): from depth + normal + `nir`,
  `NIR = refl · (E_sky_nir · AO + Σ IR lights (N·L, falloff, spot cone,
  shadow if the light has a shadow tile)) + self_emission`.
  `E_sky_nir` comes from `TodSky` / `TodConfig`: moon illuminance x phase x
  elevation x 1.0 + `night_sky_floor` x 3.0 (airglow is IR-rich), x cloud loss.
- **Illuminators:** the goggles' own IR spot (co-located with the eye, 30°,
  range 40 m) and every other player's (from `EF_NVG_IR`) are `LIGHT_IR_ONLY`
  spots. Other illuminators / IR lasers are also drawn as **beams** in the NIR
  image (cone particles scattered by fog / smoke / rain: bright haze in fog)
  and as a bright dot at the source when looking at it. Bodycam does not exist
  in the code yet; it would reuse `LIGHT_IR_ONLY`.

### 1.3 Gain: automatic brightness control (ABC)

- Mean Φ over the tube circle (log-average, reduced like the exposure
  histogram into an SSBO, read on the GPU: no CPU stall).
- Target gain `G* = clamp(L_target / Φ_mean, G_min, G_max)` (G_max ~ 3·10⁴).
- Dynamics: a **second-order spring** on log G, ζ = 0.55, ω so the settle time
  is ~0.35 s brightening and ~0.9 s darkening: it overshoots and settles, and is
  slower than the eye's adaptation (current exposure adapt).
- Output per pixel saturates like a real screen:
  `Y = Y_sat · (1 − exp(−G·Φ / Y_sat))` + `nvg_brightness_floor`.

### 1.4 Bright sources: halo, whiteout, autogating

- **Halo:** a Gen-3 tube shows a fixed-size soft disc around every bright
  point (electrons bouncing at the MCP input), not a bloom that grows with
  exposure. Pixels above a threshold are convolved with a disc of **fixed
  angular radius** (0.9°, `halo_deg` per item) at 1/4 res (32-tap Poisson
  disc + soft core). Radius in pixels = angle / (pixel angular size), so it is
  the same apparent size at every resolution (tested). Halo brightness ∝ source
  flux.
- **Whiteout:** when `G·Φ_mean` exceeds the tube's linear range (muzzle flash,
  headlights, flashlight into the lens) a whiteout level `W` attacks in 30 ms
  and releases with `whiteout_recover` (0.5..1.0 s, per item; default 0.7 s):
  floor raised, contrast compressed, image washes to the phosphor colour.
  Muzzle flash couples through a per-frame peak flux (the flash light term +
  an explicit `nvg_flash()` kick from `bal_fx_muzzle` for the owner).
- **Autogating:** during fast gain drops the power supply gates the
  photocathode: random 1-frame brightness dips with a probability ∝ |d log G /
  dt| (visible flicker at the transition only).
- **Day failure:** if `G_min` cannot bring the mean into range for 3 s (sun,
  lamp in the face) -> full whiteout + "BRIGHT SOURCE: NVG OFF" and the goggles
  switch off (tube protection).

### 1.5 Noise

- **Scintillation (shot noise):** expected photoelectrons per tube pixel per
  frame `N = Φ · QE · A_px · dt` (QE 0.3). Output = Poisson(N) x MCP gain
  sample (exponential distribution, noise factor ~1.6), approximated with a
  hashed per-frame Gaussian for N > 20 and a direct Poisson draw below. Relative
  noise ∝ 1/√N: **variance increases monotonically as light decreases**, and at
  starlight levels the image becomes sparkling speckle (tested).
- **EBI:** equivalent background illumination: a constant dark-current flux so
  total darkness still shows faint sparkle, not flat black.
- **Fixed pattern:** fiber-optic **honeycomb** (hex multi-boundaries) and faint
  **chicken-wire** lines, strength rising at low output; per-pixel gain
  variation (static, seeded).
- **Blemishes:** 3..8 small black spots, static per item instance (seed = item
  uid), inside the circle.

### 1.6 Optics and display

- **Tube resolution:** the photocathode image is resampled to a tube grid of
  ~64 lp/mm equivalent: 1024 px across the 40° circle at Modern, 640 at Low;
  then the phosphor MTF (Gaussian, σ 0.7 tube px) and upscaled to the screen.
- **Edge:** softness and slight pincushion growing outside 70 % of the radius;
  eyepiece vignette.
- **Phosphor persistence:** a history buffer at tube res, exponential decay
  `τ = 60 ms` P43 / `40 ms` P45 (white), brightness-dependent: bright things
  leave short trails.
- **Monochrome output:** `display = phosphor_rgb · Y` with
  P43 green ≈ (0.40, 1.00, 0.32), white phosphor P45 ≈ (0.86, 0.90, 1.00).
- **Field:** the world camera FOV does not change. Variants:
  - `nvg` (PVS-14-like monocular-pair look, P43): one **40° circle**.
  - `nvg_white` (P45): same, white phosphor.
  - `nvg_dual` (binocular, P45): **figure-8**: two 40° circles offset ±8° so
    the union is ~56° wide.
  Outside the mask is black. Mask size is computed in angles from the current
  horizontal FOV, so it is correct for every FOV and aspect.

### 1.7 Interplay

- Flashlights: bright white blobs + halo; pointed at you: whiteout.
- Fire / embers / hot exhaust: bright (high IR self-emission).
- Zombies look like people (NVG is not thermal); skin and cloth brightness by
  `ir_reflectance`.
- Smoke / fog: scatter the illuminator -> bright haze in the cone (NIR beam
  term), and attenuate distance like the visible image.
- Rain: streaks scintillate (rain drops in the NIR image at x2 gain).
- Audio: unchanged (the real devices are silent except a faint whine on
  power-up; a quiet 0.3 s power-up whine patch is the only sound).

### 1.8 Fidelity tiers (`FF_NVG`, `r_nvg_quality`)

| Tier | Tube res | Halo | Persistence | Honeycomb / blemishes | NIR pass |
|---|---|---|---|---|---|
| Off | — direct output: same model, half res, no halo / persistence | | | | |
| Low | 640 | no | no | blemishes only | no shadows |
| Medium | 768 | yes (16 taps) | yes | yes | no shadows |
| High | 1024 | yes | yes | yes | shadowed IR spots |
| Modern | 1024 | yes (32 taps) | yes | yes + chicken wire | shadowed + beams |

## 2. Thermal scope model (*Plan*)

### 2.1 Attachment

`assets/attachments/scope_thermal.json`: slot optic, `"sight": "scope"`,
`"sensor": "thermal"`, optical 1x lens with **digital zoom 1/2/3/4x** (mouse
wheel while ADS), PiP, battery (the attachment item's `dur`), palettes
cycled by `ACT_OPTIC_MODE` (proposed default **N**; B is fire mode) while ADS.
Mesh from a Blender script (`assets/src/items/scope_thermal.py`, item
pipeline). Ids are `u8` sorted by file name: adding the file shifts later
attachment ids, all machines get the same set (protocol bump anyway).

### 2.2 Thermal render pass

`src/render/thermal.c` + `shaders/thermal/*.{vert,frag,comp}`. When
`RenderSight.sensor == SENSOR_THERMAL`, the PiP passes (`exec_scope_pip` and
friends, render.c) bind **thermal pipelines** instead of `PIPE_LIT` and render
directly at **core resolution** into an R16F radiance target (cheap: 640×512
or 384×288). No lighting is evaluated:

`L = ε · B(T_surf) + (1 − ε) · B(T_reflected)`

- `B(T)` = Planck radiance integrated over 8..14 µm, a 1D LUT over
  −50..1500 °C (`thermal_lut`, pure, tested against numeric integration).
- `T_reflected` for a surface = apparent sky temperature in the mirror
  direction when it points to the sky (2.4), else the local ambient. So
  **polished metal (ε 0.1) shows the cold sky**, a mirror-like dark patch in
  White Hot.
- **Glass is opaque:** `MAT_GLASS` geometry draws in the opaque thermal pass
  with its own temperature (ε 0.88), plus 12 % reflection. Nothing behind it.
- **Water:** ε 0.96 at nadir falling with Fresnel at grazing angles, so lakes
  show their own (lagging) temperature looking down and the cold sky at a
  distance.
- **Sky:** apparent sky temperature (2.4); clear night sky ≈ −30..−45 °C at
  zenith -> black in White Hot.
- **Transparents / VFX:** particles carry a `temperature` (muzzle flash 900,
  fire 700, explosion 1200 decaying, breath 30, exhaust plume 120) and an
  LWIR extinction `k_lwir = k_vis · lwir_ratio` (smoke 0.2: thin smoke
  passes, dense smoke still blocks; fog: weather fog x 0.6; rain lowers
  contrast and pulls wet surfaces to the wet-bulb temperature).

### 2.3 Sensor (microbolometer core) — `post_thermal.c` / `thermal_sensor.comp`

1. **Core resolution:** Low/Medium 384×288, High/Modern 640×512 (12 µm). The
   square PiP takes the centre 288² / 512².
2. **Thermal smear:** bolometer time constant: per-pixel exponential response
   `τ = 12 ms` (+ display lag), so fast hot objects drag a trail.
3. **NETD noise:** temporal Gaussian 45 mK equivalent.
4. **Fixed-pattern noise:** per-pixel offset + gain map and column offsets
   (seeded per scope instance) that **drift** with time since the last NUC
   and with ambient change.
5. **NUC:** shutter calibration every `nuc_interval` (30 s after power-on,
   growing to 180 s) and on large ambient jumps: image **freezes 0.3 s**,
   click sound (`thermal_nuc` patch), FPN reset.
6. **AGC:** per-frame histogram of the core image (GPU reduce). Linear mode:
   low/high = 0.5 % / 99.9 % points with fast attack / slow release
   (`τ_up` 0.05 s, `τ_down` 1.2 s), so a muzzle flash or fire expands the
   range and **compresses everything else**, then recovers. Optional plateau
   equalization mode and a fixed range (`thermal_debug range a b`).
7. **Optics:** germanium lens blur (λ ≈ 10 µm: larger Airy disc), **halo
   around hot sources**, slight vignette.
8. **Edge enhancement (DDE):** unsharp mask 1.5 px, gain 0.6: the typical
   bright/dark edge halo of thermal cores.
9. **Digital zoom:** crop of the core + nearest-neighbour upscale: true
   pixelation at 3x/4x.
10. **Palette:** 256-entry LUTs (`assets/thermal/palettes.json`): White Hot,
    Black Hot, Red Hot (white hot + top 8 % of the range in red), Ironbow,
    Rainbow.
11. **Display + OSD:** upscaled to the microdisplay (the PiP target at its
    usual size), then a digital OSD drawn in the same compute pass with a
    bitmap font: palette name, zoom, battery, crosshair reticle (sharp, display
    res), and a **centre spot temperature** (3×3 core pixels, inverted through
    the LUT assuming ε 0.95, like real devices: polished metal reads wrong on
    purpose). Composited by the existing scope.frag (eye box, vignette).

### 2.4 Fidelity (`FF_THERMAL`)

Low/Medium: 384×288, no smear, DDE simplified; High: 640×512; Modern: + VFX
heat particles with LWIR extinction, residual heat decals.

## 3. Temperature state layer (*Plan*) — `src/world/thermal*.c`

Pure core (`thermal_model.c`, unit-tested) + world glue (`game_thermal.c`).
Ambient reference: `WP_TEMPERATURE` from the weather (already replicated, with
the season offset); water bodies' own `temperature`.

### 3.1 Surfaces: thermal classes with inertia

Each class integrates `dT/dt = (T_eq − T) / τ_class`, with
`T_eq = T_air + a·S_sun·(1 − cloud) − r·(1 − cloud)·night − w·wetness`
(`S_sun` from the sun elevation, `night` = sky radiative cooling, `w` =
evaporative cooling). Integrated on the CPU per class (a dozen scalars) at
world time; a `time_set` jump re-solves the periodic 24 h curve (two days of
the ODE) instead of stepping. Per pixel: sun-facing and sunlit surfaces
(N·sun, the shadow term during the day) get `+ a_dir · S_sun · max(N·L, 0)`
scaled by the class's *surface response* (thin metal high, concrete low), so
the sunny side of a wall is warm in the afternoon and stays warmer into the
night only by the class's inertia.

Classes (`assets/thermal/materials.json`, values to tune):

| class | ε | τ (h) | solar gain | night loss | ir_reflectance | maps from |
|---|---|---|---|---|---|---|
| soil | 0.93 | 2.5 | 8 | 4 | 0.30 | SURF_DIRT, terrain soil |
| grass | 0.98 | 0.5 | 3 | 5 | 0.50 | terrain grass, cover |
| foliage | 0.97 | 0.2 | 1.5 | 3 | 0.55 | vgeo foliage / trees |
| asphalt | 0.95 | 3.0 | 14 | 3 | 0.10 | SURF_ASPHALT, roads |
| concrete | 0.92 | 5.0 | 9 | 2 | 0.40 | SURF_CONCRETE, facades |
| brick | 0.93 | 4.0 | 9 | 2 | 0.35 | SURF_BRICK |
| wood | 0.90 | 1.5 | 6 | 3 | 0.45 | SURF_WOOD |
| metal_painted | 0.90 | 0.4 | 12 | 5 | albedo | SURF_METAL_* painted, vehicle body |
| metal_bare | 0.12 | 0.3 | 10 | 5 | albedo | item metal (materials.json "metal" > 0.8) |
| glass | 0.88 | 1.0 | 4 | 4 | transmits | MAT_GLASS, "glass" |
| water | 0.96 | 24 | 1 | 1 | 0.02 | water bodies (own temp) |
| cloth | 0.90 | 0.3 | 5 | 3 | 0.60 | SURF_CLOTH |
| skin | 0.98 | — (entity) | — | — | 0.50 | characters |
| snow | 0.98 | 2.0 | 0 (≤ 0 °C) | 6 | 0.70 | snow cover |
| plastic | 0.92 | 0.5 | 8 | 4 | 0.40 | item polymer |

Material plumbing: render materials have no surface id today. Plan: a `u8
thermal` (class) in `Material` / `VgMaterial` / the push constants, set from
the same `material` string that drives `SURF_*` (brushes, kits, mapgen), from
`MAT_GLASS`, from vgeo foliage flags, from terrain layers, and from item
materials.json (metal / polymer / fabric). Unknown -> concrete-like default.
Emissivity and `ir_reflectance` come from the class table (a 16-entry UBO).

### 3.2 Entities (`assets/thermal/entities.json`)

- **Humans:** core 37, skin 34 (face 35.5, hands 33 − cold wind chill),
  clothing by insulation `T = T_air + (T_skin − T_air) / (1 + clo·k)`; per
  modular part role (head, hands, torso, legs, headwear, armor, backpack).
  Breath plumes (30 °C) when `T_air < 10`.
- **Zombies:** core `zombie_core_c` 24 (configurable 20..28), skin offset −2:
  read dimmer, same clothing model. Derived from replicated state
  (`char_id` / `CharacterInst.horde`), identical on every machine.
- **Corpses / ragdolls:** Newtonian cooling from the death tick
  (`death_tick`, replicated): `T = T_air + (T0 − T_air)·e^(−t/τ)`,
  τ = 600 s (gameplay-scaled). Blood: decals start 34 °C, cool τ 90 s.
- **Vehicles (local cosmetic, from replicated rpm / engine / brake flags /
  velocity):** lumped parts engine (to 90 °C, τ heat 120 s, cool 900 s),
  exhaust (to 300 °C at load), brakes per wheel (spikes on braking, τ 60 s),
  tires (friction + speed, τ 300 s). Drawn as **hot spots** (position, radius,
  temperature) applied in the thermal shader on the body / wheels; positions
  from `VehicleDef` sockets (new `engine` socket, default front of the body
  box; exhaust; wheels). A parked car leaves a **residual ground patch**
  (heat decal, decays 20 min).
- **Weapon barrels:** from the existing `BalMuzzleHeat` (per player, every
  machine, from `EV_SHOT`): `T_barrel = T_air + k · heat`, so heat accumulates
  with fire rate and cools with τ 8 s (already modelled); applied to the
  weapon item's barrel / suppressor part roles.
- **Lights:** bulb 120 °C, LED 45 °C, fire 700 °C.
- **Footprints:** +2 °C ground patches, decay 60 s (local, from replicated
  movement).
- `temp_set <entity> <°C>` overrides any entity's core temperature (dev).

### 3.3 Hot-spot / heat-decal buffer

One SSBO of up to 256 thermal hot spots + 128 ground heat decals per frame,
built on the CPU by `game_thermal.c`, read by the thermal shader (falloff by
distance on surfaces). Keeps vehicles, corpses' ground, footprints and parked
heat cheap without per-vertex temperature data.

## 4. Networking (*Plan*) — `DG_PROTOCOL_VERSION` 36 -> 37

- Toggle requests: `MSG_INV_INTENT` new intents `II_NVG` (on / off) and
  `II_NVG_IR` (illuminator), reliable (button bits are full: `UserCmd.buttons`
  uses all 16). Host validates (nvg present, charge > 0), drains `dur`, owns
  the state.
- Replicated to everyone in the entity flags (bits 24.. are free, no extra
  bytes): `EF_NVG` (down), `EF_NVG_IR` (illuminator). The flip animation and
  the goggles' up/down pose are derived from the flag edge on every machine.
- Goggles mesh variant: new worn slot `CWORN_FACE` in the anim state's
  `ANS_PARTS` block (item id of the active nvg, room in the mask), so a
  helmet and NVG can be worn together.
- Thermal: entirely local to the scope owner. Gameplay-relevant temperatures
  (zombie vs human, corpses) derive from replicated state; vehicles / barrels
  from replicated usage (rpm, flags, `EV_SHOT`).
- New attachment file shifts attachment ids: covered by the bump.

## 5. Tools and tests (*Plan*)

- Dev room **night optics range** (footprint area, e.g. x −120..−80,
  z 40..80): a dark corridor (closed brushes, one 0.2 lm bulb, an
  incandescent and an LED lamp), an open yard with lights 0.5..500 cd, a
  flashlight on a stand aimed down the range, an IR-reflective foliage patch,
  a glass pane with a warm dummy behind it, a polished metal plate, a parked
  warm car (`"thermal": {"engine": 85}`), a fire (new `fire` entity: light +
  VFX + 700 °C hot spot), zombie and human dummies at set temperatures, a
  pool; `nvg_range` warps there and sets night.
- Commands: `nvg_debug` (gain, ABC state, Φ mean, whiteout, illuminator,
  battery), `thermal_debug` (raw temperature view, AGC range, NUC timer,
  per-material ε under the crosshair, `range a b`), `temp_set <entity> <°C>`,
  `time_set` (exists). All with descriptions + categories.
- Unit tests (`test_night_optics`, pure): key 8 toggles only with an `nvg`
  with charge; battery drain + swap; ADS refused while `EF_NVG`; ABC overshoot
  then settle within bounds; whiteout recovery within `whiteout_recover`;
  scintillation variance monotonic in Φ; halo radius constant in angle across
  720p..4K; circle / figure-8 masks per variant; Planck LUT vs numeric
  integral; zombie vs human readouts match config; glass shows its own
  temperature; bare metal reflects the sky temperature; vehicle parts heat
  and cool per rates; barrel heat accumulates with fire rate; AGC expands on
  a hot source and recovers; NUC cadence; palettes map ends correctly.
- Script tests: `tools/tests/night_optics.cfg` + `night_optics_check.py`
  (both optics at night and day, screenshots, zero validation errors),
  `nvg_net.sh` (flags + goggles replicated to a second client).

## 6. Implementation order

1. Data + pure models: thermal classes / entities / Planck LUT / ABC /
   noise stats / masks, tests.
2. Items + input + net: `nvg` (+ `nvg_white`, `nvg_dual`), `battery_aa`,
   `ACT_NVG`, intents, `EF_NVG*`, battery, ADS block, HUD chip, protocol 37.
3. NVG post profile (no NIR yet), fidelity rows.
4. Near-IR: `nir` G-buffer target, `ir_reflectance` / `ir_emission`,
   `LIGHT_IR_ONLY`, illuminators, beams.
5. Thermal: material class plumbing, thermal PiP pipelines, sensor, palettes,
   OSD, `scope_thermal` + mesh.
6. Flip case, goggles mesh (item pipeline), `CWORN_FACE`.
7. Night optics range, debug commands, script tests, this doc finalized.

Note: this cloud container is Linux without the Windows toolchain / GPU, so
the pure modules and unit tests can be built and run here; the render passes,
screenshots, validation and two-machine tests have to be run on the Windows
machine.
