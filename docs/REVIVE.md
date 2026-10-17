# Revive loop: downed, revive, death, DNA, fabricator

At 0 HP a player is knocked down instead of dying. From there:

- Teammates can revive the downed player.
- Bleeding out, or a lethal hit while downed, kills the player.
- On death the player watches a first-person death sequence, then spectates.
- The body stays as a DNA source. A teammate draws a blood sample from it with a syringe.
- The sample goes into a Fabricator, which rebuilds the player inside its chamber over 10 s. The
  rebuild uses a dedicated reconstruction effect.

Status:

- **Implemented.**
- `test_revive` passes (259 checks).
- The `lint_commands` lint passes.
- `tools/tests/revive_loop.cfg` runs the whole loop on one machine with validation on, with zero
  validation errors.
- `tools/tests/revive_net.sh` runs it over LAN: host, client and a late joiner.
- Screenshots of every stage are in `docs/revive_screenshots/`.

| Part | Files |
|---|---|
| Pure state machines | `src/gameplay/revive/revive_core.{c,h}`: rules, downed / bleed-out, timed actions, corpse, DNA registry, fabricator |
| Death sequence timeline | `src/gameplay/revive/death_seq.{c,h}`: stages, curves, impact detector |
| Ring / heartbeat synth | `src/audio/death_synth.{c,h}` (pure DSP), miniaudio nodes in `src/audio/audio.c` |
| Game integration | `src/gameplay/revive/revive.{c,h}`: host effects, interactables, replication fields, commands |
| Death sequence (local) | `src/game/game_death.c`: post, mix, HUD, impact probe, `die` / `death_preview` |
| Chamber + reconstruction | `src/game/game_revive.c`, `draw_fabricator` in `src/game/game_render.c`, `shaders/recon.glsl` |
| Kneel | `proc_kneel` in `src/anim/procedural.c`, state `downed` in `assets/human/controller.json` |
| HUD | `src/ui/hud.c` (downed panel, recovery status, fabricator panels), `src/ui/hud/hud_compass.c`, `src/game/game_wheels.c` (rings) |
| Data | `assets/gameplay/revive_rules.json`, `assets/vfx/death_sequence.json`, `assets/audio/death_ring.json`, `assets/vfx/effects/reconstruction.json` (+ `recon_*.json`), `data/items.json` (`syringe`, `dna_sample`) |
| Tests | `tests/test_revive.c`, `tools/tests/revive_loop.cfg`, `tools/tests/revive_net*.{sh,cfg}` |

## State machine

```
             0 hp (normal damage, downs allowed)                  revive done (host)
 ALIVE ─────────────────────────────────────▶ DOWNED ─────────────────────────────▶ ALIVE (revive hp)
   ▲  ╲  0 hp: kill-type / downs off / max downs / overkill  │ bleed-out 0 │ lethal hit while downed
   │   ╲─────────────────────────────────────────────────────┴─────────────┴─▶ DEAD ──▶ ET_CORPSE (DNA)
   │                                                                          │
   │ fabricator respawn (spawn_point, respawn hp, rule items)                 │ sv_respawn timer (rule)
   └──────────── RECONSTRUCTING ◀── INSERTED (queue) ◀── EXTRACTED ◀──────────┘──────▶ ALIVE (spawn point)
```

**Downed.**

- The player stays in `MOVE_WALK`, with `PlayerSim.down.downed` set. Movement is a forced crouch,
  scaled to `downed.move_speed`, with no jump, sprint, use or vehicles.
- The held item drops, unless it is a pistol and `keep_sidearm` is set. A kept sidearm can fire if
  `sidearm_can_shoot` is set.
- The bleed-out runs on the host clock. A stalled client still bleeds out.
- Each later down is shorter: `bleed_time × bleed_scale^downs`, never below `bleed_min`. With the
  defaults that is 60 s, then 36 s, ..., down to 15 s.
- Zombies keep targeting downed players: they only skip `MOVE_DEAD`.

**Timed actions.**

- These live in `PlayerSim.act` and are predicted. The client runs the same `act_step` in
  `game_sim_player`, so the ring starts on the press. The host applies the effect.
- Progress counts in ticks, so 3 s, 5 s and 6 s are exact at 60 Hz.
- Each action needs a fresh use press. It is cancelled by releasing use, by taking damage (host),
  by moving more than `move_tolerance` (horizontal only) or by the target no longer qualifying
  (range, state).
- The target is locked at the start, so the focus may drift during the action.

| Action | Target | Time | Gate | Hand-pose case |
|---|---|---|---|---|
| `RACT_REVIVE` | downed teammate (`EF_DOWNED`) | 6 s | feet within `revive.range` (1.6 m) | `revive_assist` (both palms on the chest, kneel) |
| `RACT_EXTRACT` | `ET_CORPSE` with `EF_DNA`, not your own | 3 s | feet within `corpse.range` (0.6 m) of the pelvis → head segment, auto-crouch | `extract_blood` (syringe in hand: free hand steadies the chest, needle hand on the forearm) |
| `RACT_INSERT` | `ET_FABRICATOR` accepting samples | 5 s | a `dna_sample` in the inventory, feet within `insert_range` of `sample_slot` | `fabricator_insert` (vial into the slot, other hand on the panel) |

While someone revives a downed player, the host writes `down.reviver`, `revive_ticks` and
`revive_total` into the downed player's `PlayerSim`. Their HUD shows the same ring ("Revived by
Alex"), and the bleed-out pauses (`pauses_bleed`).

**Corpse.**

- At death the host spawns an `ET_CORPSE` entity. It holds the player's slot, name and character,
  `EF_DNA`, and `samples_left` (`corpse.samples`, default 1).
- It despawns after `corpse.lifetime` (600 s), or `despawn_after_extract` (20 s) after the last
  sample is taken.
- While the player is dead, the body is the player's own ragdoll. When the player comes back, the
  corpse character adopts that ragdoll or lying pose (`corpse_adopt` in `characters.c`). Late
  joiners get a seeded lying pose.
- The host follows the body: the entity position is the pelvis, the yaw points at the head.

**DNA samples.**

- A sample is an inventory item `dna_sample`. `InvSlot.ammo` holds the registry id (1..32).
- Registry entries hold a unique id, the player slot, the name, the extractor, the carrier and a
  status: carried, dropped, in fabricator, reconstructing.
- The UI names the item "DNA: Alex #3" (`revive_item_name`).
- The host refreshes carriers every 15 ticks from inventories, pickups and containers. A sample
  nobody holds is freed.
- When the carrier dies, `sample.on_carrier_death` decides: `drop` (a pickup at the body), `lost`
  or `keep`.

**Fabricator.**

- States: `IDLE → RECONSTRUCTING (10 s) → OPENING (open_time, forced idle) → COOLDOWN → IDLE`.
- The queue holds up to 5 samples and runs oldest first.
- A queued or running sample whose player came back another way (timed respawn) or left is dropped.
  A running reconstruction then aborts to the cooldown.
- Power off mid-cycle ejects the current and queued samples as pickups (`eject_on_power_loss`).
  With `require_power`, an unpowered machine takes nothing.
- At the end of the reconstruction:
  1. The player respawns at `spawn_point` with `respawn_hp`.
  2. The old inventory stays on the body (the host-side corpse stash; not lootable yet).
  3. The player gets `respawn_items`.
  4. The player stays frozen for `open_time`.

**Respawn mode** (`respawn_mode`):

- `timer`: the existing `sv_respawn_*` only.
- `fabricator`: the timer respawn is disabled.
- `both`: the default.

**Kill-type damage.** `apply_damage_ex(..., DMGF_KILL)` skips the down. The `kill` command, `die`
and the bleed-out use it.

## Networking (host-authoritative)

| State | Where | Who sees it |
|---|---|---|
| Downed, bleed-out, revive progress, timed action, forced idle | `PlayerSim.down` / `.act` / `.frozen_ticks` (raw in the snapshot local data) | The owner, predicted and reconciled, plus its spectators |
| Downed (others) | `EF_DOWNED` entity flag → `CharInputs.downed` → controller state `downed` + kneel | Everyone |
| Corpse | `ET_CORPSE`: `item` = player slot, `count` = samples left, `extra` = (death tick, extract tick, extractor), `name`, `EF_DNA`, pos / rot = body axis | Everyone (always relevant) |
| Fabricator | `ET_FABRICATOR`: `count` = state \| queue << 8, `item` = current sample, `attach[5]` = queue, `extra` = (state start tick, powered, current player), `name` = current player | Everyone (always relevant) |
| DNA registry | `EV_DNA` reliable events on every change, resent to a late joiner (`revive_sv_player_joined`) | Everyone |
| One-shot feedback | `EV_REVIVE` (downed, revived, bled out, extracted, inserted, reconstruction start / done, ejected, cancelled) | Hints, sounds |

- Progress is never streamed. Every machine computes `fab_progress` from the replicated start tick,
  so late joiners see the right progress at once.
- Corpse and fabricator data uses the existing delta bits (`NF_ITEM`, `NF_EXTRA`, `NF_ATTACH`,
  `NF_NAME`) through the new world arrays `aux_count` / `aux_extra`.
- Protocol version 19.
- The death sequence is local only. Spectating starts on its own timer (`spec_death_cam_time`),
  which usually falls under the black.

## Rules (`assets/gameplay/revive_rules.json`)

`downed`:

- `enabled`, `bleed_time`, `bleed_scale`, `bleed_min`, `max_downs` (0 = unlimited)
- `overkill`: a hit dealing more than this past 0 HP kills (0 = off)
- `move_speed`, `hp` (the health pool while downed)
- `keep_sidearm`, `sidearm_can_shoot`, `auto_ping`
- `self_revive`: the hook (`RACT_SELF_REVIVE`) exists; no item ships yet

`revive`: `time`, `range`, `hp`, `move_tolerance`, `pauses_bleed`, `resets_downs`.

`corpse`: `lifetime`, `despawn_after_extract`, `range`, `extract_time`, `samples`, `crouch`,
`auto_ping`.

`sample`: `on_carrier_death` (`drop` | `lost` | `keep`), `weight`.

`fabricator`: `insert_time`, `insert_range`, `reconstruct_time`, `open_time`, `cooldown`,
`queue_max` (≤ 5), `respawn_hp`, `respawn_items` (`[["bandage", 1]]`), `eject_on_power_loss`,
`require_power`.

Top level: `respawn_mode` (`timer` | `fabricator` | `both`), `death_skippable`.

`revive_rules_reload` re-reads the file.

## Death sequence

**Stage 1, the fall.** At death the camera stays on the ragdoll head (the existing death camera).
In `s1_time` (0.4 s) the view eases out to:

- desaturate 0.95;
- contrast −0.15;
- vignette +0.35;
- edge blur 0.35;
- exposure drifting to −0.4 EV (slower).

The HUD is hidden. Audio at the same moment:

- The world buses (effects, music, voice; not the UI) go through a low-pass whose cutoff sweeps
  20 kHz → 400 Hz in 0.5 s (log space).
- They are ducked to 0.45.
- A 3.6 kHz tinnitus sine with a 5.5 Hz, 0.4 % warble rises in 0.25 s and holds.
- A slowing heartbeat plays under it (92 → 46 bpm).

**Stage 2, the impact.** The ragdoll's head or chest touching the ground cuts to black in one
frame. The trigger is any of:

- **Contact:** either body's clearance above the world is under `contact_dist`, after `min_time`,
  and the body was moving.
- **Velocity drop:** the speed falls by `speed_drop` within one step, from at least `min_speed`.
- **Timeout:** `timeout` (2.5 s).

At the cut:

- `render_camera_cut()` resets the TAA, temporal effects and auto-exposure.
- The ring and the low-passed world fade out over `fade` (2 s) with a zero-slope curve, so nothing
  pops.
- The heartbeat stops.

After the silence, the black holds for `hold_time` (1 s), then fades into spectating
(`fade_in`, 0.8 s). The fade-in frame is another camera cut, because the view may have switched
under the black. An attack press skips ahead (`death_skippable`, always with `sv_cheats`).

**Downed variant.** A light persistent version: desaturate 0.6, vignette 0.32, blur 0.1, low-pass
2.5 kHz, a faint ring (0.06), world gain 0.8. It rises over `attack` and recovers on revive over
`release` (the reverse curve). It never goes black.

**Post inputs.** `PostSettings.fb_black` / `fb_blur` (UBO `s_fb3`) plus the existing desaturate,
contrast, vignette and exposure. `tonemap.comp` mixes the bloom chain in as the blur and applies
the black, and `final.frag` applies the black after the grain and dither. The two tiers without
the full stack:

- **Off** (`final_direct.frag`): still desaturates and cuts.
- **Low** (no bloom): has no blur.

**Files.**

- `assets/vfx/death_sequence.json`: `stage1`, `impact`, `stage2`, `downed`.
- `assets/audio/death_ring.json`: `ring` (pitch, warble, level, attack, fade), `mix` (lpf_from,
  lpf_to, sweep, world_duck), `heartbeat`, `downed`.
- `death_reload` re-reads both files.

**Debug.**

| Command / cvar | Effect |
|---|---|
| `die [fall_height]` | Kills the local player with the full sequence. A height lifts the player first, so the ragdoll falls from there. |
| `down [slot]` | Knocks a player down. |
| `revive [slot]` | Revives a downed player, or respawns a dead one; post and audio go back to neutral. |
| `death_preview <1 fall \| 2 black \| 3 downed> <0..1>` | Scrubs the sequence without dying (`death_preview 0` = off). |
| `death_impact_debug 1` | Logs the contact that triggered the cut (speeds, clearances) and the time since death. |
| `death_dump` | Logs the stage, the post values and the mix (tests). |
| `death_sequence 0` | Disables the sequence. |

## Reconstruction shader (`shaders/recon.glsl`)

Each machine draws a client-local display body: the reconstructed player's own character, with
its definition and modular parts. It is an `ET_MARKER` local entity with `CharacterInst.phantom`
set: evaluated idle, no foot IK, floating `float_height` above the floor with a small bob. Its
skinned draws carry `MAT_RECON` (+ `MAT_RECON_FX`) and `SkinnedDraw.recon` = (feet position,
progress). They never go through vgeo (`vkg_route_skinned` skips `MAT_RECON`), so `skin.frag` /
`gbuffer_skin.frag` run the dissolve.

- **Build order.** For every point:
  `t = mix(0.55 · height / 1.85 m + 0.45 · spine radius / 0.32 m, noise, 0.3)`, where `noise` is 3D
  value noise at 7 and 19 per metre. The order is inside-out: the spine first, then outward,
  bottom to top. It depends only on world position: no time, no per-frame randomness, so TAA
  stays stable.
- **Three fronts.**
  - `fs = 1.75 · progress`, `fm = fs − 0.35`, `fk = fs − 0.7`.
  - A point is discarded until `fs ≥ t`.
  - It then shows a dark skeleton tint, a red muscle tint once `fm ≥ t`, and the final material
    once `fk ≥ t`.
  - At progress 1 everything is built and the effect is off.
- **Construction edge** (`MAT_RECON_FX`):
  - an emissive band of width 0.06 behind each front (cyan / white on the leading and skin fronts,
    red on the muscle front), which blooms;
  - vertical scan lines over the freshly built band;
  - a world-space lattice "wire flash" right behind the leading front.

  A barycentric wireframe would need `VK_KHR_fragment_shader_barycentric`. The lattice is a
  portable stand-in.
- **Shadows.** `skin_shadow.vert` drops the body until progress reaches 0.85.
- **Particles** (`game_revive.c`; rates in `assets/vfx/effects/reconstruction.json`):
  - motes from the glass, each aimed at a point on the current front height and life-timed to
    arrive there (no GPU attractor needed);
  - bubbles through the liquid;
  - floor mist and a heat shimmer (`distort`);
  - a completion burst plus a camera bump within `complete_shake_radius`.
- **Chamber.**
  - Glass is a blended cylinder with `MAT_GLASS`: Fresnel opacity plus a rim. It is not true
    screen-space refraction; the heat shimmer particles refract.
  - The liquid fills with the progress and carries `MAT_CAUSTICS` (animated caustic light, also on
    the floor ring while busy).
  - The interior point light pulses faster as the progress grows.
  - The panel shows the progress bar.
  - At completion the glass slides up into the cap while the chamber opens.
- **Fidelity.** `vfx_quality` 0 (Low) gives the dissolve only: no edge, scan, wire or particles.
  Higher levels add everything, with mote rates per level.
- `reconstruction_preview <0..1>` scrubs the effect on the dev room fabricator with your own
  character (no argument = off).

## UI

- **Downed panel:** the bleed-out ring ("Bleeding out" / "Paused"), "Downed", "Crawl to cover or
  wait for help" or "Alex is reviving you".
- **Rings:** the HUD centre ring shows the predicted action ("Reviving Alex", "Extracting DNA",
  "Inserting DNA") or "Revived by …"; an interruption shows "Cancelled".
- **Prompts:**
  - "Revive Alex (hold)", or "… is down: get closer";
  - "Extract DNA: Alex (hold)", "Extract DNA: get closer" or "(DNA taken)";
  - "Insert DNA (hold 5 s)", or the fabricator state.
- **Fabricator panels:** a world-space plate over each machine within 14 m: state, player and
  seconds, progress bar, queue names.
- **Spectator bar (dead):**
  - "Your body is waiting: DNA can be extracted"
  - "DNA extracted by Alex"
  - "In fabricator: queue #2"
  - "Reconstructing... 7", with a bar
- **Compass:** downed teammates as red help markers; bodies with DNA left as "DNA 12m".
- **Auto pings:** "Need help" on a down and at the body on death (`game_sv_auto_ping`).
- **Inventory:** DNA samples are named after their player.

## Dev room and commands

The dev room has a fabricator at (8.5, 0, 23) facing the spawn and a syringe on the loadout table.

| Command | Effect |
|---|---|
| `spawn_teammate [name] [distance]` | A client-less teammate to run the whole loop alone. |
| `spawn_corpse [slot] [distance]` | A DNA body. |
| `spawn_fabricator [distance] [powered]` | Spawns a fabricator. |
| `fab_power <0\|1> [entity]` | Powers fabricators on or off. |
| `revive_dump` | Logs players, corpses, fabricators and samples. |
| `revive_approach corpse\|fab\|downed` | Stands in reach of a timed action and looks at it. |
| `revive_wait players <n> \| fab <progress> \| <slot> alive\|dead\|downed\|reconstructing\|acted [timeout]` | Script sync. `acted`: the held timed action finished or was cancelled; `fab`: a reconstruction at that progress. Scripts wait on these, never on frame counts (a frame is one tick on a GPU, ~30 on lavapipe). |

All commands are in the "Revive & Death" category (`assets/ui/command_categories.json`).

## Tests

**`tests/test_revive.c` (CTest `revive`).**

- Rules parse, including the shipped JSON files.
- Knock / kill decisions: kill-type, downs disabled, max downs, overkill.
- Exact bleed-out ticks (3600), the shorter later downs and the floor.
- Bleed pause while being revived.
- Exact 3 / 5 / 6 s actions (180 / 300 / 360 ticks) and every interrupt (release, move, damage,
  range, host).
- Predicted actions reconcile: the replay matches, and a host interrupt survives it.
- The 0.6 m corpse gate, including vertical sanity. One sample per corpse; the two-sample rule;
  the despawn timers.
- DNA ids are unique and never reused.
- Fabricator: queue order, no duplicates, exact 10 s, late-join progress from the start tick,
  power-loss ejection, `require_power`, the queue cap.
- The whole loop through the pure machines.
- Death sequence:
  - stage 1 reaches the desaturated target within its time;
  - the cutoff reaches 400 Hz;
  - the ring attack is smooth;
  - the cut is exactly one frame, with a single camera-cut flag;
  - the ring is continuous across the cut and its fade is pop-free;
  - the black lasts fade + hold, then the fade-in returns to neutral;
  - the timeout path;
  - the contact, velocity and still-body impact paths;
  - the downed variant and its recovery;
  - `revive` resets to neutral;
  - skip and preview;
  - all values stay finite.
- Synth: silent until targeted, pitch (zero crossings), no pops on start or stop, heartbeat
  pulses, finite output.

**`tools/tests/revive_loop.cfg`.** One machine, about 6 minutes on lavapipe. It runs down →
revive → kill → extract → insert into the dev room fabricator → 10 s reconstruction → respawn in
the chamber with 60 HP, with screenshots and `revive_dump` lines. Run it with `--validation`; it
gives zero validation errors.

**`tools/tests/revive_net.sh`.**

- The host knocks down, revives, kills, extracts and inserts the client (60 ms fake lag).
- The client checks its downed HUD, its death sequence (`death_impact` / `death_cut` lines) and
  the spectator countdown, then that it is back.
- A late joiner connects during the reconstruction and logs the same fabricator progress.
- Environment: `DG_BUILD`, `DG_EXE` (e.g. a wrapper running `deadground` under Xvfb on Linux),
  `SHOTDIR`.

**Rendering.** The reconstruction renders at progress 0 / 0.15 / 0.3 / 0.5 / 0.7 / 1
(`docs/revive_screenshots/reconstruction_*.png`) with `--validation` and zero validation errors.

**Linux.** The game builds and runs on Linux too:

```sh
cmake -S . -B build-linux -DDG_ENABLE_STEAM=OFF -DSDL_WAYLAND=OFF
make -C build-linux deadground test_revive
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ./deadground --test ...
```

lavapipe renders at about 0.5 s per frame, which is enough for these scripts.

## Not done / follow-ups

- **Corpse loot:** the carried inventory is stashed on the body host-side, but not lootable yet.
  That needs a container UI with more than 12 slots.
- **Self-revive:** a hook (rule plus action kind) with no item.
- **Third-person hands:** for remote viewers, the hand-pose cases come from the item swap (syringe
  / vial) and the kneel. The first-person hand IK holds the cases on the interacting player.
- **Glass refraction:** blended Fresnel glass plus distortion particles, not a screen-space
  refraction pipeline.
- **Era presets** for the reconstruction look are not separate yet. Everything is in
  `reconstruction.json` and `recon.glsl`.
- **Chamber camera:** the optional "camera on the chamber for the last seconds" for the dead
  player is not done. The spectator bar shows the countdown instead.
