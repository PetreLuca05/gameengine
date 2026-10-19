# Spectating (first-person "in-eye" observer)

Dead players (and `spectate` for dev / host) cycle through living players and see exactly what the
target sees. That covers the target's:

- camera and hold point;
- ADS, scope PiP and phone lens;
- recoil, sway and crosshair spread;
- HUD (ammo, health, hotbar, reload / use / hold rings);
- hit markers, hurt markers and damage flash;
- vehicle view.

The watched player is presented through the same pipeline as the local player: it becomes this
machine's **view slot**. On a client it is re-simulated from its own relayed commands, not drawn
from interpolated snapshots.

Code:

| Part | Files |
|---|---|
| Host | `src/net/spectate.c/.h`: observer table, rules, relay, death flow |
| Pure core | `src/net/spectate_core.c`, tested by `tests/test_spectate.c` |
| Observer client | `src/player/spectator.c/.h` |
| View slot | `game_view_slot` / `game_view_player` / `game_view_cmd` in `src/game/game.c` |
| Spectator bar | `src/ui/hud.c` |

Status:

- **Implemented.**
- `test_spectate` passes.
- The game build and the multi-process tests (`tools/tests/spec_net.sh`) have to be run on the
  Windows toolchain. See "Verification" at the end.

## CS:GO in-eye vs this design

| | CS:GO in-eye | Deadground |
|---|---|---|
| Target view source | Server snapshot of the target, interpolated (`cl_interp`) | The target's own **relayed UserCmds**, run through the same prediction path the target runs (`game_sim_player`, `chars_local_predict`, hold point, recoil, sway, spread) |
| "Local" data (ammo, angles, recoil, HUD) | The target's local-player props are also sent to observers | The same: the snapshot local data (`PlayerSim`, ack seq, hold progress) and the inventory go to the target's observers |
| Correction | None needed (it is just a snapshot) | Reconciled against the host's `PlayerSim` exactly like the local player |
| Delay | L(T→H) + server tick + snapshot interval + L(H→O) + interp ≈ L1 + L2 + 130 ms | L(T→H) + L(H→O) + ≤ 1 tick (relayed at receive time, no interp) |

## Data flow

```
 target client T                    host H                                  observer client O
 ───────────────                    ──────                                  ─────────────────
 build_cmd (seq S) ──MSG_CMDS──▶ host_handle_cmds
 predict S locally   (+send_ms)  ├─ game_sv_receive_cmd (queue, as before)
                                 └─ spectate_host_cmds ───MSG_SPEC_CMDS───▶ SpecCmdRing[S] (unreliable,
                                    (same net_update, flushed at its end)   last 4 commands)
                                                                            │
                                                                            ▼ each O tick (spectator_tick):
                                 host_tick: simulate S                      SpecClock: simulate S (first =
                                 (authoritative)                            true: fx, recoil kick, sounds)
                                                                            or extrapolate (last command)
                                 net_host_after_tick, per client c:         ▲
                                   src = spectate_source(c) (the target)    │ reconcile: restore PlayerSim@A,
                                   local data of src (ack A = its last  ────┘ replay ring A+1..sim_seq
                                   processed seq, PlayerSim, hold)
                                   + spectator block (mode, target, flags,
                                   respawn timer, target one-way delay)
                                   relevance centre = target position
                                 MSG_INVENTORY{slot = src}  ───────────────▶ HUD ammo / hotbar of the target
                                 events with target == T (hit confirm, ───▶ hit / hurt markers, hints
                                   hurt, hint, sound, chat) also go to O
```

### Observer table and requests (host)

- Each slot has a `SpecObserver { mode, target, dev, killer, forced, seq, free_pos }`.
  - `mode` is one of `SPEC_OFF`, `SPEC_IN_EYE`, `SPEC_THIRD`, `SPEC_FREE`.
- Requests reach the host through `RPC_SPECTATE (mode, target | next | prev | auto | keep, dev)`.
  `spectate_request` resolves and validates them. A refusal sends the reason to the player as a hint.

### What a client receives about its spectated player

- **One source per client.** `spectate_source(c)` is the slot whose local data, inventory and
  relevance client `c` receives. It is the target in first or third person, else `c` itself. This
  replaces "the client's own" with one extra recipient per field; no code is duplicated.
- **Events.** `spec_event_delivers` routes events for remote clients and for the host's own
  player:
  - a client gets its own events, plus its target's HUD feedback (hit confirm, hurt, hint, sound,
    chat);
  - events the target predicted itself (`except == target`: its tracers, use animation) are
    skipped, because the observer predicts them too;
  - container and bench windows stay with the target.

### Commands, switching and relevance

- **Relay.** `host_handle_cmds` hands every arriving command to `spectate_host_cmds`. The host
  relays the newest 4 to each observer of that player right away and flushes at the end of
  `net_update`. It does not wait for the next tick or snapshot. The host's own player's commands
  enter the same path from `tick()`.
- **Switching target** (`seq` changes, `forced = 2`):
  1. The next two snapshots go out at once, without waiting for `sv_snaprate`, and carry
     `SPECF_FORCED`.
  2. Each is followed by a relay burst of every command after that snapshot's ack (up to 32).
  3. The observer starts its replica from the snapshot state and replays the burst, so it is at the
     target's present within a tick or two.
  4. The camera blends over `spec_blend_time` (150 ms) from the last rendered view.
- **Relevance.** `spectate_relevance_center` is:
  - the target's position in first or third person;
  - the free-camera position, sent at 4 Hz by `RPC_SPEC_POS`;
  - the observer's own position otherwise.

  `repl_capture` and the horde rate limiter use it. Horde AI tiers keep using living players only,
  so a spectator does not wake zombies.

### Observer client pipeline (`src/player/spectator.c`)

**View slot and its consumers.** `G->local_slot` stays the observer's own (dead) player.
`game_view_slot()` is the watched player in first or third person. Every presentation site uses it:

| Area | What uses the view slot |
|---|---|
| Camera | Eye interpolation, ADS, sway, head socket, death / third-person / free camera |
| Body and items | Flashlight, hands and first-person body, sights, phone lens |
| Overlays | Crosshair, HUD, post feedback |
| Vehicles | Camera and HUD |
| Weapon feel | Recoil camera kick (`on_shot`), crosshair spread, predicted shot effects (`local_view`), the fire animation trigger |

Command building, focus, interaction, inventory UI, RPCs and the own skin stay on `local_slot`.

**Promotion.** The target gets its own `PhysCharacter`, as the local player has. Its kinematic
capsule is removed (`repl_player_body`). An owned-entity hook stops snapshot interpolation from
touching it, and `chars_present_tick` treats it as local.

**Per observer tick (`spectator_tick`):**

1. `spec_clock_step` decides which relayed commands to simulate. A backlog over 2 catches up 2 per
   tick (the host's queue rule); over 16 it jumps.
2. A rewind restores the confirmed state, dropping the extrapolated ticks.
3. Real commands are simulated with `first = true`.
   - When catching up a backlog, only the newest two get effects.
   - The animation steps once per command (`chars_local_predict`).
4. The view angles after a command are the command's angles plus that command's recoil kick, which
   `game_sim_player` reports through `spectator_note_kick`. That is what the target's view holds
   after the same tick. Render interpolates them by `alpha`.
5. Late commands are covered by extrapolation:
   - the last command is repeated for up to `spec_extrap_ticks` (default 6), with its post-kick
     angles and without presses (no shots, jumps or uses), and `first = false`;
   - the target then holds;
   - when the real command arrives, the extrapolated ticks are rewound;
   - the position difference fades out through the view's error offset, the same smoothing as the
     local reconcile (`cam_recon_smooth_time`);
   - corrections above `spec_pop_warn` are counted.

**Snapshot local data (`spectator_cl_local_data`):** restore the host's state at ack, replay the ring
after it with `first = false`, re-extrapolate. This is exactly `game_cl_reconcile`.

**Host as observer (listen server):** the host already simulates the target authoritatively. Only
the view slot changes; there is no replica. The camera follows the commands the host processed.

**Observer input** drives only the spectator controls. `build_cmd` sends the own (dead / dev)
player no movement and no buttons. No field of the observer's `UserCmd` reaches the target.

## Latency budget (one way, target input → observer screen)

| Step | Cost | Note |
|---|---|---|
| T: input → command | 0 | The same tick quantization the target itself sees, so it is not counted |
| T → H transit | L1 | `net_client_send_cmds` flushes at once |
| H: wait for `net_update` | 0 – 1 host frame (avg ½) | Relayed inside the receive handler and flushed at the end of the same `net_update`; it does **not** wait for the host tick or `sv_snaprate` |
| H → O transit | L2 | Unreliable channel, 4× redundancy |
| O: wait for `net_update` + next tick | 0 – 1 tick (avg ½) | Simulated on the first observer tick after arrival; no interpolation buffer |
| O: render | alpha interpolation inside the tick | The same as the local player |
| **Floor** | **L1 + L2** | One hop each way, which no relay design can beat. The only way below it is a direct T→O peer link, which this host-authoritative P2P layer does not have |
| **Expected** | L1 + L2 + ~1 tick (16.7 ms at 60 Hz) | Plus the frame quantization of the two `net_update` waits at the processes' frame rates |
| Test bound (`net_fakelag 80` on every machine) | ≤ 2 × 80 + 16.7 ms | `tools/tests/spec_net.sh` → `spec_compare.py` |

**The `spec delay` readout:**

- The HUD shows X = (the target's one-way estimate, which the host puts in the spec block as its
  ENet RTT / 2) + (the observer's `app_rtt_ms` / 2) + (backlog) + ½ tick.
- In the LAN test all processes share one clock. Commands carry the target's send stamp
  (`MSG_CMDS` `send_ms`, relayed with them), so the observer logs the **measured** delay next to
  the estimate (`spec_stats`, `spec_dump 1`).
- The settings tooltip and the HUD readout both state the floor: one hop to the host plus one hop
  to you.

## Determinism audit

Every system that changes what the target sees must be a pure function of (commands, replicated
state, seeds) or be marked cosmetic. "Changed" lists what this work did.

| System | Code | State | RNG / clock | Status | Changed |
|---|---|---|---|---|---|
| Movement, mantle, hang, noclip | `player/movement.c` `player_move_tick` | `PlayerMove` | SIM_DT | Pure | **Landing stamp** `land_tick` was the machine-local `w->tick` (it differed between owner and observer), now the command seq |
| Aim recoil, recovery, countered | `gameplay/recoil.c` | `RecoilState` in `WeaponState` | pattern RNG from `mag_seed` (`recoil_mag_seed(seq, slot)`) + `shot_index` | Pure | none (`test_spectate`: 600 ticks bit-identical through snapshot + replay + extrapolate / rewind) |
| Bloom / flinch | `weapons.c` `weapon_flinch` (host, on damage) | `rc.bloom` | — | Pure via reconcile | none (a host-only input: the observer gets it with the local data, like the owner) |
| Spread / cone | `weapons.c` `weapon_current_spread`, `chars_limb_spread` | `PlayerSim` + limb status | — | Pure | none (limb status is replicated in `AnimNetState`) |
| Shot directions | `weapons.c` `shot_seed(seq, slot, shots)` | — | seeded | Pure | none |
| Fire timing, burst, fire mode, mag | `weapons.c` `recoil_fire_budget` | `WeaponState` | SIM_DT | Pure | none |
| ADS progress, breath, sway_k | `weapons.c` `gun_feel_tick` | `WeaponState` | SIM_DT | Pure | none |
| Gameplay aim sway / ADS bob | `weapon_aim_offset_deg`, `weapon_sway_time(seq)` | — | seq-based | Pure | Render and crosshair take the seq of the **viewed** player's latest command (`game_view_cmd`) |
| ADS blend (render) | `game.c` `G->ads_prev`, `game_ads_vis` | per view | — | Pure | View slot |
| Visual recoil springs | `game.c` `on_shot` / `weapon_feel_tick`, `G->recoil_vis` | Game | kick seeded by the shot seed; stepped at SIM_DT | Pure | Kicked for the view slot's shots (first simulation of a command), stepped with its weapon; reset on a target switch |
| Recoil view kick | `game_sim_player` `wo.view_kick` | — | — | Pure | Reported per command (`spectator_note_kick`); the observer's camera = command angles + kick |
| Crosshair spread | `G->spread_*`, `ui/crosshair.c` | Game | — | Pure | View slot; `CH.ads_fade` is cosmetic |
| Anim controller, variants | `anim/controller.c` | `CharacterInst` | `AP_SEED` from replicated anim events | Pure | `chars_local_predict` for the watched player (once per command); out of `chars_client_tick` / interpolation while watched |
| Fire / melee animation trigger | `characters.c` `chars_notify_attack` | — | — | Pure | Also for the view slot (was the owner only) |
| Procedural hold / weapon_hold | `anim/weapon_hold.c`, `procedural.c` | `CharacterInst` | SIM_DT springs | Pure | View slot forced to LOD 0 like the local one (`c->local`) |
| Hold point breathing | `player/holdpoint.c` `st->t` | `HoldPointState` | was free-running since character creation | **Fixed** | `t = command seq × SIM_DT` for players simulated on this machine (`ProcInput.hp_clock`) |
| Hold point / camera movement bob | `holdpoint.c` `st->bob_phase`, `camera.c` `bob_phase` | was per-machine accumulators | free-running | **Fixed** | New replicated `PlayerMove.stride` (walk-cycle phase stepped in `player_move_tick`), read by both |
| Hold point look lag | `holdpoint.c` | springs | aim from `player_inputs` | Pure | The view slot uses `player_inputs` (its simulated `PlayerSim`), not `inputs_from_net` |
| Item / viewmodel kick on the body | `characters.c` `hands_vm_kick` | `hands` singleton, sampled at render alpha | frame | Cosmetic (converging) | Follows the view slot's `recoil_vis`; frame-sampled as for the local player |
| Camera (eye smoothing, landing dip, FOV, roll) | `player/camera.c` `camera_update` | `ViewState` | frame dt | Cosmetic (converging) | Reset on a target switch; yaw / pitch = the target's command angles (+kick) interpolated, not the mouse |
| Gliding camera (nominal eye, glide / step springs), camera collision | `camera_fp_math.c`, `camera_fp.c` `CamGlide` / `CamHeadStab` | Game | frame-dt springs | Cosmetic (converging) | Restarted on a switch (and after frames without it, teleports); the nominal eye from the target's interpolated state; collision cast from its capsule |
| Arms layer (own camera / FOV, ADS presentation, equip raise) | `game_arms.c`, `render/arms_layer.c` | `AL` singleton | frame-dt blend | Cosmetic | From the target's first-person pose and the shared `assets/player/arms_layer.json`; off while the camera blends to a new target |
| First-person body / camera socket | `characters.c` `chars_fp_camera`, `chars_render_local_fp` | `FP` singleton | — | Pure | View slot (already a parameter) |
| Hands rig (clip fades, interaction reach) | `player/hands.c` `hands_update` | `H` singleton | frame-dt blends | Pure input / cosmetic fades | Reach from the target's commands (`BTN_USE` edge + `use_target`), look delta from its view change |
| Scope PiP, lens, reticle | `game/game_sights.c` | — | — | Pure given ADS + item pose | Via `game_ads_vis` / the drawn item |
| Hit markers, hurt markers, damage flash | `game_events.c`, `G->hitmarker_*`, `hurt[]` | Game | frame-dt decay | Routing fixed | Host delivers the target's `EV_HIT_CONFIRM` / `EV_HURT` / `EV_HINT` / `EV_SOUND` / `EV_CHAT` to its observers; decay is cosmetic |
| Kill feed | `EV_KILL` to all | — | — | Pure | none |
| Phone (landscape, rec, front camera, driven rotation) | `cmd.phone_flags`, `phone_rot` | UserCmd | — | Pure | Observer's phone renders the view slot's lens |
| Phone zoom | `phone.c` `PH.zoom` | was local only | frame dt | **Fixed** | `UserCmd.phone_zoom` (×32); the smoothing toward it is cosmetic |
| Vehicle seat, driving input | `PlayerMove` seat fields | replicated | — | Pure | none |
| Vehicle first-person / third-person camera choice | `VG.cam_tp` | was local only | — | **Fixed** | `BTN_VEH_CAM` in the command; the camera follows the view slot's vehicle |
| Post feedback (low health, death desaturation, sprint, flash) | `game_post.c` `apply_feedback` | `GP` | frame-dt smoothing | Cosmetic | View slot's health / mode (none on the free camera) |
| Lens droplets | `game_weather.c` `lens_frame`, `weather.c` | own LCG seeded 0xBEEF | frame dt | **Cosmetic** | none: rain amount is replicated weather, drop placement is noise |
| Film grain, chromatic, vignette, TAA jitter | `render/post` | — | per frame | **Cosmetic** | none |
| Thunder audio delay | `game_weather.c` | — | `platform_time` | **Cosmetic** | none |
| Bolt-cycle sound / shells | `game.c` `G->bolt_timer` | Game | SIM_DT | Cosmetic | View slot |

There is no wall clock (`platform_time`) in any first-person gameplay path.

## Validation (host)

`spec_validate` is pure and unit-tested. The rules:

- **Target.** It must be active, not dead, not the observer, and not itself spectating.
- **Observer.**
  - It must be dead, or dev: the host's own player with the `spectate` command, or anyone with
    `sv_cheats`.
  - `sv_spec_allowed 0` turns spectating off.
- **Enemies.**
  - `sv_spec_fp_enemies 0` refuses first person on another team. The request falls back to third
    person (`SPECF_FP_DENIED`).
  - There are no teams yet: everyone is team 0.
- **Rounds.** There is no round system yet. "No spectating pre-round" hooks into
  `SpecRules.round_allows`, which is always true today.
- **Auto-advance** (`spectate_host_tick`). A target that dies, leaves or starts spectating moves its
  observers to the next legal player. With nobody legal they go to the free camera. An observer
  that comes back to life (respawn) stops spectating.
- **Isolation.** The observer never affects the target. Spectator RPCs only change its
  `SpecObserver`. Its own commands still drive its own dead player, and `build_cmd` strips them.

## Death flow

1. **On death** (`apply_damage` → `spectate_on_death`): the killer is remembered and the respawn
   timer comes from `sv_respawn_time` (default 3 s, the old `PLAYER_RESPAWN_TIME`).
2. **Death camera.** The existing ragdoll-head camera (`camera_fp_death`) runs for
   `spec_death_cam_time` (default 3 s). Fire skips it.
3. **Auto spectate.** The client then requests first person on the killer
   (`SPEC_REQ_AUTO`: the killer if it is a living player, else the nearest living player). With
   nobody alive it gets the free camera. This only happens when the respawn is more than 1.5 s
   away, or manual. With the default 3 s respawn there is no spectating unless the respawn is
   slower or `sv_respawn_mode` is 1.
4. **Respawn rules.**
   - `sv_respawn_mode 0` respawns automatically after `sv_respawn_time` (the old behaviour).
   - `sv_respawn_mode 1` respawns on request: the `Respawn` key (reload) or the `respawn` command
     once the timer has run out.
   - The spectator block carries the timer, and the HUD counts it down.
5. **Late joiners.** `sv_join_dead 1` makes them join dead and hidden while anyone is alive, and
   they start in first person on someone.
6. **Dev.** `spectate <player|next|prev|auto|free|off> [eye|third|free]` works without dying (host /
   `sv_cheats`). `spectate` with no argument prints the state and the delay.

## Controls (while spectating)

| Key | Action |
|---|---|
| Fire / next item (RB) | next player |
| Aim / previous item (LB) | previous player |
| Use | first person → third person → free camera |
| 1–8 | the n-th living player |
| Move, jump / crouch, sprint | free camera (noclip; `spec_free_collide` slides along walls) |
| Mouse | orbit (third person) / look (free camera) |
| Reload | Respawn, when allowed |
| Esc | pause menu |

## Relevance and bandwidth

- **Snapshots.** In first or third person a snapshot is the target's own: the private block and the
  relevance set are the same.
- **Relay.** It costs about 4 × (36 B command + 4 B stamp) × 60 Hz ≈ 9.6 kB/s, the same as the target's own upstream.
- **Inventory.** It is sent when the target's inventory changes.
- **In total**, one observer costs about one extra client.
- **Measurement.** `net_snapstats` prints per client the snapshot + relay bytes, the spectator relay
  rate and the source slot (relevance follows the target).
- **No cap.** There is no per-target observer cap; at 8 players at most 7 observers share one
  target.

## Spectator HUD

- **The target's own HUD** comes from the existing `hud_draw` with the view slot:
  - vitals and ammo;
  - reload / use rings and the hold ring;
  - hotbar (their equipped slot);
  - crosshair with their spread, sway and focus;
  - hit markers, hurt markers, hints and kill feed;
  - the vehicle HUD.
- **Spectator bar** (top centre):
  - "Spectating - first person / third person / free camera";
  - the target's name with `<` / `>`;
  - the controls;
  - `Spec delay X ms | minimum = one hop to the host + one hop to you`.
- **Bottom centre:** the respawn countdown or the `Respawn` key.
- **During the death camera:** "Spectating in N s, Fire to skip".
- **Settings → Character → Spectating:** death camera time, delay readout (its tooltip states the
  floor), switch blend, free-camera collision.
- **Not in the game yet:**
  - weapon wheels, compass, pings, chat UI, a player list to click, teams and rounds;
  - an X-ray / overhead view (the free camera covers overhead).

  When they are added they draw from the view slot in the same way.

## Protocol (DG_PROTOCOL_VERSION 16)

- **`MSG_CMDS`** gains `send_ms` (u32, the sender's clock).
- **`MSG_SPEC_CMDS`** (host → observer, unreliable): `{target u8, count u8, (UserCmd, send_ms u32)[count]}`.
- **Snapshot:**
  - the local data (`spec_write_local`: ack, `PlayerSim`, hold progress, open container) of the own
    or spectated player;
  - then the spectator block (`spec_write_block`: data slot, mode, target, flags, respawn 0.1 s,
    killer, target one-way delay, switch seq).
- **`MSG_INVENTORY`** gains the slot it describes.
- **`UserCmd`** gains `phone_zoom` (u8, in the old padding) and `BTN_VEH_CAM`.
- **`PlayerMove`** gains `stride`.
- **RPCs:** `RPC_SPECTATE`, `RPC_RESPAWN`, `RPC_SPEC_POS`.

## Settings (cvars)

| Cvar | Default | |
|---|---|---|
| `spec_death_cam_time` | 3 | s on the own ragdoll before auto spectating |
| `spec_show_delay` | 1 | delay readout |
| `spec_extrap_ticks` | 6 | extrapolation budget for late commands |
| `spec_blend_time` | 0.15 | camera blend on a switch (s) |
| `spec_pop_warn` | 0.05 | corrections above this are counted / logged (m) |
| `spec_free_speed`, `spec_free_collide` | 8, 1 | free camera |
| `spec_dump` | 0 | per-tick state hash + `spec_stats` (tests) |
| `sv_spec_allowed`, `sv_spec_fp_enemies` | 1, 1 | host rules |
| `sv_respawn_time`, `sv_respawn_mode`, `sv_join_dead` | 3, 0, 0 | respawn rules |

## Tests

| Test | Kind | Checks |
|---|---|---|
| `test_spectate` (ctest `spectate`) | unit, runs anywhere | target rules matrix; event routing; command ring; observer clock under 80 + 80 ms lag + 3 % loss per hop (delay ≤ both hops + 1 tick, extrapolation bounded) and jitter / outages (hold, catch-up); the rewind contract (confirmed state equals the target's at every seq through extrapolation, rewinds and reconciles); every local-data field reaches the observer (field table + coverage of the struct, poisoned decode); spec block / relay round trip; 600-tick recoil + view + spread bit-identical between target and an observer started from a snapshot with extrapolate / rewind / reconcile |
| `tools/tests/spec_net.sh` | three processes, `net_fakelag 80 net_fakeloss 3` | host kills the observer → death camera → auto spectate → `spectate 1`; target does ADS, a magazine dump, reload and use rings, the sniper scope (PiP), the phone camera, walking, driving. `spec_compare.py`: per-seq comparison of the target's `spec_dump local` and the observer's `spec_dump observer` (state hashes, eye, spread, ammo, health, timers; the bone pose is reported only), ≥ 600 consecutive identical ticks, measured delay ≤ 2 × lag + 4 frames + 1 tick (per hop the conditioner releases on the sender's next frame and the receiver polls once a frame), largest correction ≤ `spec_pop_warn` (host teleports > 10 m and a seated target are not counted). Send stamps use `platform_wall_ms` (one time base for every process on the machine). `DG_PORT=27115` runs it on another LAN port. Screenshot pairs `spec_t_*.png` / `spec_o_*.png` |
| `tools/tests/spec_rules.cfg` | host script | illegal targets refused with a reason (self, missing slot), dev spectating, free camera, `sv_spec_allowed 0` |

## Verification and known gaps

**Verification in the implementing session (a Linux container without the Windows toolchain):**

- `test_spectate` was built and run: all checks pass.
- Every game source file was type-checked (`gcc -fsyntax-only` against the SDL3 / Vulkan / ENet /
  joltc headers) with no new warnings.
- The game itself, `spec_net.sh` and the screenshots were **not** run there. They need
  `.toolchain/b.sh` + `tools/tests/spec_net.sh` on the Windows machine.

**Known gaps:**

- **The vehicle under a spectated driver** is not re-simulated. The owner predicts its own car
  (`vehicles_client_tick`); the observer shows the car from snapshots at the interpolation delay,
  and the seated replica follows that car. To bring it to the relay delay, the observer would run
  `vehicles_client_tick` for the target's commands.
- **The host as observer** shows the target at the host's command processing rate (0–2 commands per
  host tick under jitter). Remote observers smooth this with their clock.
- **Bone pose hashes** compare the post-pass pose. The first-person solve uses the frame camera, so
  pose equality depends on equal frame timing. The state hashes (`sim`, `rc`, `move`, `wpn`) are
  the strict check; `spec_compare.py` reports the pose without requiring it.
- **Host-side inputs** (`setpos`, `vehicle_put`, damage) reach the target's prediction one round trip
  late: around them the target's `spec_dump local` (its prediction) and the observer's confirmed
  replica differ for a few ticks by design.
- **Extrapolation corrections:** the clock rides the earliest arrival, so under real frame jitter
  the observer extrapolates 1-6 ticks much of the time. An input change in that window (the walk
  start / jump in `spec_net.sh`) is corrected by 0.1-0.4 m: `spec_net.sh` fails the 0.05 m
  correction check on some runs on a loaded machine. A small adaptive jitter buffer would trade
  ~2-4 ticks of delay for it.
- **Observer health of a re-spectated target:** after switching away and back to the same target
  (the cycle step of `spec_net.sh`), the observer's copy of the target's health read 0 / 0 on many
  ticks (`hp` in `spec_compare.py`) while seated; not yet traced (snapshot baseline vs the replica).
