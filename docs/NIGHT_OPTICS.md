# Night optics: night vision goggles + thermal scope

Two devices, two physical models, never a colour filter:

| | NVG (`nvg`, `nvg_white`, `nvg_dual`; key 8) | Thermal scope (`scope_thermal`, optic) |
|---|---|---|
| Physics | image intensifier: photons of the visible red + near-IR, amplified ~10^3..10^4x | microbolometer: emitted 8..14 µm (LWIR) radiance, no light needed |
| Where | full screen post profile: replaces exposure / bloom / flare / tonemap / grade | the scope PiP only (thermal shading of the PiP + a sensor pass); the rest of the view stays normal |
| Sees | moon / stars / airglow, lamps by their near-IR, IR illuminators, IR-bright foliage | temperature x emissivity; glass is opaque, polished metal mirrors the cold sky |
| Cannot | see colour, see in total darkness without an illuminator | see through glass, read print / texture detail |

| What | Where |
|---|---|
| Intensifier model (pure: ABC, whiteout, autogating, protection, noise, halo, field, battery) | `src/render/post/nvg_model.c` |
| NVG gameplay rules (pure: toggle, ADS, drain, battery swap) | `src/gameplay/nvg_rules.c` |
| NVG game side (key 8, `UserCmd.gear`, host state, flags, HUD hooks, debug) | `src/game/game_nvg.c` |
| NVG post profile | `src/render/post/post_nvg.c`, `shaders/post/nvg_{common.glsl,nir,flux,reduce,halo,tube,display}.comp` |
| Temperature layer (pure: classes, inertia, entities, vehicles, sky, Planck LUT) | `src/world/thermal.c` |
| Thermal sensor model (pure: AGC, NUC, palettes, cores, zoom) | `src/render/thermal_sensor.c` |
| Thermal game side (temperature state, heat volumes, sensor control, OSD, battery, debug) | `src/game/game_thermal.c` |
| Thermal shading of the PiP | `shaders/thermal.glsl` (lit body, particles, water), `FrameData.thermal*` |
| Thermal sensor passes | `src/render/post/post_thermal.c`, `shaders/post/thermal_{common.glsl,core,reduce,display}.comp` |
| Data | `assets/nvg/nvg.json`, `assets/thermal/{materials,entities,palettes}.json`, `data/items.json` (`nvg*`, `battery_aa`), `assets/attachments/scope_thermal.json` |
| Models | `assets/src/items/{_nvg,nvg,nvg_white,nvg_dual,battery_aa,att_scope_thermal}.py` -> `assets/items/meshes/*.dgim` |
| Hand pose | `assets/handposes/cases/nvg_flip.json` (`item:nvg:flip`) |
| Tests | `test_night_optics` (ctest `night_optics`), `tools/tests/nvg_toggle.cfg` + `nvg_check.py`, `night_optics.cfg` + `night_optics_check.py`, `nvg_net.sh` + `nvg_net_check.py` |

## Gameplay rules

- **Key 8** (`ACT_NVG`, rebindable; it shares 8 with slot 8, which open wheels and the spectator
  controls keep): a **tap** flips the goggles down / up when the inventory holds an `nvg*` item with
  charge (the first in reading order: equipment, then the grid by row). A **0.4 s hold** while they are
  down switches the **IR illuminator**. Without goggles / with an empty battery a tap only hints.
- **No ADS while the goggles are down or flipping**, through any sight (iron, red dot, scope,
  thermal). The client strips `BTN_AIM` from its command, the host's sim refuses it by its own state
  (`nvg_blocks_ads`): prediction agrees. Flip up to aim; the flip takes 0.6 s (`flip_time`).
- **Flip**: the left hand reaches to the brow mount and back (`item:nvg:flip`, the first-person arms
  in `hands.c`, the body others see in `characters.c` through the goggles hook); the view passes
  through dark as the housing swings in front of the eye. The goggles show on the head while down or
  flipping (the item mesh about its `flip_pivot` socket, 100 deg up), for everyone (`EF_NVG`,
  `EF_NVG_DUAL` picks the binocular model).
- **Battery**: one AA (`runtime_h` per variant, 1 h of real time; the illuminator x1.4). The charge is
  the item's `InvItem.dur` (0..1000, `"durability": 1000`, `"battery_h"`), drained by the host, sent
  to the owner with the inventory. Empty: the host turns them off ("NVG battery empty") and the local
  request follows. **Replace battery** (inventory context menu, `II_RECHARGE`, console `nvg_recharge`)
  uses a `battery_aa` (`"battery_cell": true`) from the inventory.
- **Tube protection**: the mean flux pinned above the range at the minimum gain for `protect_s` (3 s:
  daylight, a light held into the lens) shuts the goggles off ("Bright source: NVG tube protection").
- **HUD rules** while the viewed image is the goggles' (`nvg_hud_amount`): dimmed (x0.35) and minimal:
  vitals, ammo, weapons, hints stay; compass, pings and the kill feed hide. An **NVG chip** right of
  the vitals while you carry goggles: lit while down ("NVG IR" with the illuminator), the battery pill
  and percent, the key.
- **Thermal scope**: drains its battery while it is on the drawn weapon (host; the weapon's
  `InvItem.dur` counts the charge *used*, so a fresh weapon reads full; `battery_h` 3 h). Empty: a black
  display with "NO BATTERY". Palettes cycle on **N** (`ACT_OPTIC_MODE`) while aiming; the **wheel**
  zooms 1..4x digitally while aiming through it (weapons don't cycle then).
- **Zombies read colder** than people: core `thermal_zombie_core` (24 C, a `CVAR_LOCKED | CVAR_RULE`
  host rule, 15..37), skin 22 C, vs human skin 34 C, face 35.5 C.
- Accessibility: `nvg_brightness_floor` (Graphics > Comfort, 0..0.15). The look is otherwise fixed.

## 1. Image intensifier (NVG)

`post_nvg.c`, in `post_resolve` after the sights / lens droplets: the input is the **linear HDR before
exposure**; when fully down the intensifier is the whole image (no bloom / tonemap / grade); while
flipping it blends from the normal output. Engine Fidelity Off (direct output) runs it too (it is
gameplay) and the final pass then encodes its output.

1. **Near-IR image** (`nvg_nir.comp`, half render resolution, Low a quarter): near-IR irradiance x
   reflectance, deferred from the G-buffer.
   - Moon / sun: its near-IR share (`ir_moon` 1.0) x N.L x the sun shadow map.
   - Sky: airglow and starlight are mostly near-IR (`ir_sky` 3 at night, 0.5 at noon); the sky glows.
   - Lamps: every point light x its **IR emission** (`PointLight.ir`; 0 = from the colour: warm
     incandescent / fire ~2..3, neutral ~0.6, white LED ~0.1; fires set 3).
   - **IR illuminators** (`LIGHT_IR_ONLY` in effect: never in the visible lights): the own one and
     every other player's with `EF_NVG_IR`, a 30 deg cone, weak (`illum_intensity` 6, range 25 m).
     Their **beams** scatter in haze / fog / rain (single scattering along the view ray, white-noise
     jittered: the tube's noise hides it), and another player's emitter seen from in front is a
     **bright point**.
   - Reflectance: foliage (the G-buffer's thin flag) 0.55 (the chlorophyll red edge), metal 0.25,
     the rest 0.40; texture detail comes from the visible image.
2. **Photocathode flux** (`nvg_flux.comp`, the tube's internal resolution): `k_vis dot(hdr, GaAs) +
   k_nir NIR`, GaAs weights (0.55, 0.35, 0.10): red-heavy.
3. **ABC metering** (`nvg_reduce.comp`): the log-average flux inside the field and the linear mean
   (the tube's photocurrent), read back two frames late.
4. **ABC / whiteout** (CPU, `nvg_step`):
   - Gain = a damped spring on log2 gain (damping 0.55) towards `target / mean`, settling in
     0.35 s when the scene brightens and 0.9 s when it darkens: it overshoots and settles, slower than
     the eye. Range 20..4000.
   - **Whiteout** from the mean over saturation or the photocurrent over `overload` (4) x saturation:
     30 ms attack, under 5 % within `whiteout_recover` (0.7 s P43, 0.6 s P45) after it ends. A few small
     lamps barely move the photocurrent; a light filling the tube does. The own muzzle flash kicks it
     (`EF_FIRING`).
   - **Autogating**: 1-frame dips while the gain is pulled down fast.
   - **Tube protection** (above).
5. **Halo** (`nvg_halo.comp`, quarter resolution, Medium+): the amplified flux above 0.6 x
   saturation gathered over a disc of **fixed angular radius** (`halo_deg` 0.9; the radius in pixels
   from the FOV, the same on every screen), turned per pixel (grain, not blocks), brighter towards the
   source.
6. **Tube** (`nvg_tube.comp`): photoelectrons `N = (flux + EBI) x photons x dt x 60` per tube pixel
   per frame, a Poisson draw (normal above 20) times the MCP gain spread (excess noise factor 1.6):
   the **scintillation** sparkles as the light falls (relative noise sqrt(F / N)); EBI keeps a dark tube
   sparkling. Screen `Y = Ysat (1 - exp(-G flux / Ysat))`, the whiteout raises the floor and collapses
   the contrast, a fixed per-pixel gain pattern (per goggles), the **phosphor persistence**
   `max(Y, history x exp(-dt / tau))` (60 ms P43 / 40 ms P45, ping-pong history, Medium+).
7. **Display** (`nvg_display.comp`, output resolution): the phosphor MTF (~1 tube pixel), softness
   and slight pincushion towards the rim, the fiber plate (a faint honeycomb at low output, chicken
   wire fainter still), blemishes (3..7 black spots seeded per goggles), the brightness floor,
   **monochrome phosphor** (P43 green (0.40, 1.00, 0.32), P45 white (0.86, 0.90, 1.00)), the field
   and the flip transition.
   - **The field**: no hard-edged circle. An **extreme eyepiece vignette** (full brightness inside
     35 % of the tube's radius, falling as (1 - t^2)^3 to black at 115 %), **stretched horizontally
     to the screen's width** (`nvg_stretch`: black reached at the side edges); the vertical keeps the
     tube's 40 deg. The dual tube is the union of two (figure 8). The world FOV does not change.
   - The tube's internal resolution: `tube_px` across the circle (1024 Modern / High, 768 Medium,
     512 Low), never above the output.

Fidelity (`r_night_optics`, the Effects group; Off / Low 0, Medium 1, High 2, Modern 3): Low = half the
tube resolution, no halo, no persistence, a quarter-resolution NIR image; Medium = 16 halo taps; Modern
= 32 taps. Other interplay falls out of the model: flashlights are white blobs with a halo, fires and
incandescent lamps bright (IR-rich), LEDs dim, zombies look like people, foliage bright, smoke / fog
light up in an illuminator's cone, rain sparkles; audio is unchanged.

`assets/nvg/nvg.json`: the photocathode, `ir_moon` / `ir_sky`, `y_sat`, `overload`, `flip_time`; per
variant (`p43`, `p45`, `dual`): phosphor, tubes, `circle_deg`, `tube_sep_deg`, `halo_deg`,
`whiteout_recover`, `persistence`, gain range, ABC target / settle / damping, `tube_px`, photons, noise
factor, EBI, `protect_s`, `runtime_h`, illuminator drain / range / angle / intensity.

## 2. Thermal scope

### Thermal render pass

With `RenderThermal.on` (the viewed optic is `"sensor": "thermal"` and aiming) the scope PiP renders
at the **core's resolution** (384 x 288 Low / Medium, 640 x 512 High / Modern: the centre square) and
its frame UBO copy carries `thermal.x = 1`: the shared lit body (`lit_body.glsl`), the particles and
the water return **8..14 µm radiance** instead of light (`thermal.glsl`):

    L = e B(T) + (1 - e) B(T_reflected)       T_reflected: the sky along the mirror direction, else the ground

- **Class**: every material flag bit is taken, so it is inferred: glass / city windows -> glass,
  ground cover -> grass, foliage cards (thin AO byte) / alpha-masked -> foliage, emissive or bright
  unlit (lamps) -> hot (a bulb, 120 C), terrain by its colour -> grass / soil, metallic > 0.5 ->
  bare metal (rough < 0.35) or painted metal, brownish -> wood, else concrete. Skinned characters are
  bodies.
- **Surface temperature**: the class's temperature now (`ThermalSurfaces`) + facing the sun while
  sunlit (the shadow map) - facing the clear night sky (roofs colder).
- **Heat volumes** (`FrameData.thermal_vol`, 64, nearest the scope first, `game_thermal.c`): bodies set
  a character's surface (the weighted regions: face / chest / legs / hands; worn gear on a body is warmed
  through), hot spots pull any surface towards their temperature (engine, exhaust, brake discs, tyres,
  barrels, fires), ground patches warm up-facing surfaces (a car parked there, the ground around a fire).
- **Glass is opaque**: it shows its own temperature (+12 % reflection); nothing behind it.
- **Polished metal** (emissivity 0.12) mirrors the sky: cold, dark in White Hot.
- **Water**: emissivity 0.96 looking down, falling with Fresnel at grazing angles: its own lagging
  temperature near, the cold sky far.
- **Sky**: `air - depression x cos(z)^0.35` (depression 45 C clear, 15 % of that under full cloud):
  the zenith is far below the air.
- **Particles**: additive (flashes, sparks, flames) are hot gas (900 C), emissive alpha particles
  (fire) 650 C, the rest (smoke, dust, mist) at the air temperature and only 30 % opaque: thin smoke
  passes, a dense cloud still hides.
- **Fog / haze** attenuates at 0.6x the visible extinction towards the air's radiance.
- Decals are not drawn (blood / bullet holes have no heat of their own).

### Sensor (`post_thermal.c`, `thermal_sensor.c`)

1. `thermal_core.comp` (core resolution, ping-pong history): empty pixels get the sky along their
   ray; germanium lens blur (3 x 3); **DDE** edge enhancement (`b3 + 0.6 (b3 - b5)`: the bright / dark
   edge halo); **NETD** 45 mK; **fixed pattern** per pixel and per column, drifting with the time since
   the last NUC; **smear** (12 ms bolometer time constant); while the NUC shutter is closed the last
   image **holds**.
2. `thermal_reduce.comp`: a log histogram of what the display shows -> the 0.5 % / 99.9 % radiance
   points and the centre 3 x 3 (spot meter), read back two frames late.
3. CPU: **AGC** (`thermal_agc_step`): the range widens fast (50 ms: a muzzle flash or a fire stretches
   it and compresses everything else) and narrows slowly (1.2 s), never narrower than 2 C;
   `thermal_debug range a b` fixes it. **NUC** (`thermal_nuc_step`): a calibration at power-on, then
   after 30 s, growing x1.5 to 180 s, and on a 4 C ambient change: 0.3 s freeze + a click.
4. `thermal_display.comp` (2x the core, up to 1024): the **digital zoom** crop (nearest: true
   pixelation), AGC -> **palette** (White Hot, Black Hot, Red Hot, Ironbow, Rainbow; 256 entries,
   `assets/thermal/palettes.json`), lens vignette, and a **digital OSD** at display resolution (a 5 x 6
   bitmap font): palette + zoom, battery (+ "MAN" with a fixed range), the **spot temperature** (assumed
   emissivity 0.95, as real devices: polished metal reads wrong on purpose), "NUC" while calibrating,
   a sharp crosshair with a dark outline; "NO BATTERY" when empty. The optical reticle is `"none"`
   (`RET_NONE`). The display image replaces the PiP for the scope composite (eye box, eyepiece).

Fidelity: the core is 384 x 288 below High (`r_night_optics`).

## 3. Temperature state layer (`world/thermal.c`, `game_thermal.c`)

**Ambient**: the replicated weather: `WP_TEMPERATURE` (with the season offset), cloud cover, wetness,
wind; the sun from the clock. Everything below derives from replicated state or is local cosmetic, so
every machine computes the same thermal picture without new traffic.

**Surfaces**: per class `dT/dt = (T_eq - T) / tau`, on world time:

    T_eq = air + solar_gain S (1 - 0.75 cloud) - night_loss (0.2 + 0.8 clear) night - wet_cool wetness

`S` = sin(elevation)^0.8; `night` fades in below ~4 deg. The directional term uses the class-smoothed
sun (it fades with the class's inertia, not at sunset). A clock jump > 1 h (join, `time_set`) re-settles
the 48 h before it (10 min steps, today's weather, a half-sine sun). Classes and defaults
(`assets/thermal/materials.json`, all tunable):

| class | e | tau h | solar | direct | night | sky-facing | wet | near-IR |
|---|---|---|---|---|---|---|---|---|
| concrete | 0.92 | 5.0 | 9 | 6 | 2 | 2 | 3 | 0.40 |
| brick | 0.93 | 4.0 | 9 | 6 | 2 | 2 | 3 | 0.35 |
| soil | 0.93 | 2.5 | 8 | 5 | 4 | 2 | 4 | 0.30 |
| grass | 0.98 | 0.5 | 3 | 2 | 5 | 1.5 | 2 | 0.50 |
| foliage | 0.97 | 0.2 | 1.5 | 1.5 | 3 | 1 | 1.5 | 0.55 |
| asphalt | 0.95 | 3.0 | 14 | 8 | 3 | 2 | 3 | 0.10 |
| wood | 0.90 | 1.5 | 6 | 5 | 3 | 2 | 3 | 0.45 |
| metal_painted | 0.90 | 0.4 | 12 | 10 | 5 | 3 | 2 | albedo |
| metal_bare | 0.12 | 0.3 | 10 | 8 | 5 | 3 | 1 | albedo |
| glass | 0.88 | 1.0 | 4 | 3 | 4 | 2 | 2 | 0.08 |
| water | 0.96 | 24 | 1 | 0.5 | 1 | 0.5 | 0 | 0.02 |
| cloth | 0.90 | 0.3 | 5 | 4 | 3 | 1 | 3 | 0.60 |
| skin | 0.98 | (entity) | | | | | | 0.50 |
| snow | 0.98 | 2.0 | 0 (<= 0 C) | | 6 | 2 | | 0.70 |
| plastic | 0.92 | 0.5 | 8 | 6 | 4 | 2 | 2 | 0.40 |
| hot | 0.95 | (its own) | | | | | | 0.60 |

`"surfaces"` maps material / surface names (level `"material"`, `SurfaceMaterial`, item materials) to
classes (`thermal_class_of`).

**Entities** (`assets/thermal/entities.json`):
- Humans: core 37, skin 34 (face +1.5, hands -1), minus 0.12 C per C of air below 20 and 0.15 per m/s
  of wind; clothing `air + (skin - air) / (1 + clo k)` per role (torso 1.0, legs 0.8, headwear 2.0,
  armor 3.0, backpack 4.0). Zombies: core 24 (the rule cvar), skin 22.
- Corpses / ragdolls: Newtonian cooling from the replicated death tick, tau 600 s (gameplay-scaled).
- Vehicles (local, from the replicated engine flag, rpm, brake flag, velocity): engine to air + 55 + 30
  x load (heat 120 s, cool 900 s), exhaust air + 120 + 200 x load (40 / 300 s), brakes +0.6 C/s per m/s
  while braking (cool 60 s), tyres + 0.6 C per m/s (300 / 400 s). The engine sits at the end opposite
  the exhaust socket. A warm engine standing still warms the ground under it; the patch cools over
  20 min after the car leaves.
- Barrels: air + 2.5 C per gram of the shooter's muzzle heat (`bal_fx_heat`: tau 8 s, every machine
  from `EV_SHOT`), capped at 450 C: it accumulates with the fire rate.
- Fires (level `"fire"`: a flickering IR-rich light + flame / smoke VFX, `LIGHT_GROUP_FIRE`): 700 C,
  the ground around them +25 C. Bulbs 120 C, LEDs 45 C, muzzle flashes 900 C.

**Band radiance**: `B(T)` integrated over 8..14 µm (Simpson, 600 steps) into a 256-entry LUT from -60 C
in 6.5 C steps (both the shaders and the spot meter's inverse use it).

## 4. Networking (`DG_PROTOCOL_VERSION` 37)

- `UserCmd.gear`: `GEAR_NVG` (down), `GEAR_NVG_IR` (illuminator), every tick as state (the button bits
  are full). The host validates against the inventory, owns the state and the battery.
- `EF_NVG`, `EF_NVG_IR`, `EF_NVG_DUAL` (entity flags 24..26: no extra bytes): every machine shows the
  goggles on the head, times the flip from the flag's edge, adds other players' illuminators to its own
  NVG.
- `InvIntentType II_RECHARGE`; the `scope_thermal` attachment file shifts the ids of later attachment
  files.
- Thermal is local to the scope owner; every temperature derives from replicated state (weather,
  clock, characters' kind and death tick, vehicle state, shots).

## 5. Tools

- `nvg [0|1] [ir]`, `nvg_report` (request, flip, ADS block, item, battery, the replicated flags),
  `nvg_debug` (flux mean / photocurrent, gain, whiteout, flicker, protection, quality, tube scale, halo
  px, NIR), `nvg_battery [0..1]` (dev), `nvg_recharge`, `nightvision` (= `nvg`).
- `thermal_debug [range lo hi | palette n | zoom z]` (air, classes with emissivity and temperature,
  sensor state, AGC range, spot, NUC timer, `thermal_debug_volumes 1` lists the heat volumes),
  `thermal_probe <entity>` (a character's face / chest / legs, a vehicle's parts), `temp_set <entity>
  <C | off>` (a character's core / a vehicle's engine).
- `nvg_range [HH:MM]`: the dev room's **night optics range** (x 140..192, z 48..80, stand at
  (166, 0, 78) looking north): a dark corridor (a dim bulb, an LED), lamps of rising intensity on poles,
  a bright LED on a stand, IR-reflective foliage, a glass pane with a person behind it, a polished metal
  plate lying flat (it mirrors the sky), a fire, a warm parked car (85 C), a person and a zombie, a
  shallow pool.

## 6. Tests

- `test_night_optics` (pure): data parses; the Planck LUT vs the integral, its inverse, monotonic;
  inertia (concrete warm after a sunny day, metal follows the air, water lags, foliage below the air at
  night, periodic settle, roofs colder at night, sunlit faces warmer, snow <= 0, wet cooler); zombie vs
  human readouts, clothing, corpse cooling, vehicle parts heat with use and cool per rates, barrel heat
  with fire rate; glass reads its own temperature, bare metal the sky; AGC widens on a hot source and
  recovers, minimum span; NUC cadence and freeze; palettes; zoom crop; ABC overshoot / settle;
  whiteout after a flash recovered within the variant's time; scintillation variance monotonic in the
  flux; halo radius constant in angle across resolutions; the vignette field (monotonic, no step, the
  horizontal stretch, resolution independent, figure 8); daylight protection; battery; the rules
  (toggle, ADS, swap, empty).
- `tools/tests/nvg_toggle.cfg` + `nvg_check.py`: 8 without goggles, with them (ADS refused), off (ADS
  works), empty battery refuses, the swap, running empty.
- `tools/tests/night_optics.cfg` + `night_optics_check.py`: both optics on the range at night and noon
  (screenshots), the flash whiteout and its recovery, person / zombie / glass / metal plate readings,
  probes, the noon shutoff.
- `tools/tests/nvg_net.sh` + `nvg_net_check.py`: the flags reach another machine and clear.
