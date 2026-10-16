# Spectating (first-person "in-eye" observer)

Status: **design, not implemented yet**. This file holds the data flow, the latency budget and the
determinism audit. The implementation plan at the end lists the code changes.

Dead players (and `spectate` for dev / host) cycle through living players and see exactly what the
target sees: their camera, hold point, ADS, scope PiP, recoil, sway, crosshair spread, HUD, hit
markers, hurt markers, phone and vehicle view.

## CS:GO in-eye vs this design

| | CS:GO in-eye | Deadground |
|---|---|---|
| Target view source | Server snapshot of the target, interpolated (`cl_interp`) | The target's own **forwarded UserCmds**, run through the same prediction path the target runs (`game_sim_player`, `chars_local_predict`, hold point, recoil, sway, spread) |
| "Local" data (ammo, angles, recoil, HUD) | Target's local-player props are also sent to observers | The same: the snapshot private block (`PlayerSim`, ack seq, hold progress, container) and the inventory go to the target's in-eye observers |
| Correction | None needed (it is just a snapshot) | Reconciled against the host's `PlayerSim` exactly like the local player (`game_cl_reconcile`) |
| Delay | L(T→H) + server tick + snapshot interval + L(H→O) + interp ≈ L1 + L2 + 130 ms | L(T→H) + L(H→O) + ≤ 1 tick (forwarded at receive time, no interp) |

## Data flow

```
 target client T                    host H                                  observer client O
 ───────────────                    ──────                                  ─────────────────
 build_cmd (seq S) ──MSG_CMDS──▶ host_handle_cmds
 predict S locally               ├─ game_sv_receive_cmd (queue, as today)
                                 └─ spectate_forward_cmds ──MSG_SPEC_CMDS──▶ spec ring[S] (unreliable,
                                    (same net_update call, flushed          last 4 cmds, like MSG_CMDS)
                                    at once, not on the tick)               │
                                                                            ▼ each O tick:
                                 host_tick: simulate S                      sim target S (first=true:
                                 (authoritative)                            fx, recoil_vis kick, sounds)
                                                                            or extrapolate (last cmd)
                                 net_host_after_tick, per observer:         ▲
                                   private block = TARGET's PlayerSim,      │ reconcile: restore PlayerSim@A,
                                   ack = target's last_processed_seq A, ────┘ replay ring A+1..latest
                                   hold_progress, open_container,
                                   spec block (mode, target, flags,
                                   respawn timer, delay stamp)
                                   relevance centre = target position
                                 MSG_INVENTORY(target's inventory, slot) ──▶ HUD ammo / hotbar
                                 events with target == T (EV_HIT_CONFIRM,──▶ hit markers, hurt markers,
                                   EV_HURT, EV_HINT ...) also go to O        damage flash, hints
```

- **Observer state (host, `src/net/spectate.c`)**: `SpecObserver { u8 mode (SPEC_IN_EYE / SPEC_THIRD /
  SPEC_FREE / SPEC_OFF), target, flags; }` per slot, plus a derived `observers[target]` bitmask (u8,
  `MAX_PLAYERS` ≤ 8). Every "send X to the owner of slot s" site becomes "send to `s` and to each
  in-eye bit in `observers[s]`": the private snapshot block, `send_inventory`, the event filter,
  forwarded commands. One extra recipient per field; the code that builds the data is not
  duplicated.
- **Requests (O → H)**: `RPC_SPECTATE {mode, target | -1 next | -2 prev}`. The host validates (below) and
  answers in the snapshot's spec block. An illegal target is refused and the observer stays on its
  current target.
- **Switching target**: the host sends the target's current state at once, plus its last 32
  commands. That is a full private block in the next snapshot (forced, not waiting for
  `sv_snaprate`) and a `MSG_SPEC_CMDS` burst. The observer resets its replica from that state and
  replays the burst, so the first frame already shows the target's predicted present. The camera is
  inertialized over `spec_blend_time` (150 ms) from the previous view.
- **Host's own player as target**: the host's local commands go into the same forward path from
  `tick()`. `last_processed_seq` is set for the local player too; today only remote players set it.
- **Host as observer (listen server)**: the host already simulates the target authoritatively, so it
  only switches its *view slot*. There is no re-simulation, and the delay is L(T→H).

### Observer client pipeline (`src/player/spectator.c`)

- `G->local_slot` stays the observer's own (dead) player. A new `G->view_slot` is whose eyes are
  rendered (`game_view_player()`).
- Every presentation site listed in the audit switches from `local_slot` to `view_slot`. Command
  building, focus, interaction, inventory UI and RPCs stay on `local_slot`.
- The target slot gets a `PhysCharacter` on the observer (as the local player does in
  `setup_replica`). It is excluded from `repl_interpolate` and `chars_client_tick`, and is driven by
  `chars_local_predict`, i.e. treated as local.
- Per observer tick:
  1. If commands up to `next = sim_seq + 1` are present, restore the confirmed state if the last tick
     was extrapolated, then simulate `next` with `first = true`. Shot fx, the `recoil_vis` kick and
     sounds fire once per command, as on the target. A backlog over 2 is caught up 2 per tick (the
     host's queue rule); a backlog over 16 is jumped.
  2. Otherwise **extrapolate** with the last command: edge buttons cleared, `first = false`, so there
     are no fx and no shots. This runs for up to `spec_extrap_ticks` (default 6 = 100 ms), then the
     target holds.
  3. When the real command arrives, it is rewound and re-simulated. The positional difference goes
     into the view's `error_offset` (the local reconcile smoothing) and is logged as a pop if it is
     over `spec_pop_warn` (5 cm).
- Snapshot private block → `spectator_reconcile(sim, ack)`. This is `game_cl_reconcile` generalized to
  (slot, cmd ring); the local path calls the same function with its own history.
- Observer input drives only spectator controls: next / prev (mouse 1 / 2, LB / RB), 1-8, a mode
  toggle (Space), the player-list click and `Esc` (pause menu). No `UserCmd` field of the observer
  reaches the target.

## Latency budget (one way, target input → observer screen)

| Step | Cost | Note |
|---|---|---|
| T: input → command | 0 | The same tick quantization the target itself sees, so it is not counted |
| T → H transit | L1 | `net_client_send_cmds` flushes at once |
| H: wait for `net_update` | 0 – 1 host frame (avg ½ frame) | Forwarded inside the receive handler and flushed there; it does **not** wait for the host tick or `sv_snaprate` |
| H → O transit | L2 | Unreliable channel, 4× redundancy |
| O: wait for `net_update` + next tick | 0 – 1 tick (avg ½) | Simulated on the first observer tick after arrival; no interpolation buffer |
| O: render | alpha interpolation inside the tick | The same as the local player |
| **Floor** | **L1 + L2** | One hop each way, which no relay design can beat. The only way below it is a direct T→O peer link, which this host-authoritative P2P layer does not have |
| **Expected** | L1 + L2 + ~1 tick (16.7 ms at 60 Hz) | |
| Test bound (`net_fakelag 80` on T and H) | ≤ 2 × 80 + 16.7 ms | `tools/tests/spec_net.sh` |

**`spec delay` readout.** The observer shows `spec delay: X ms`:

- X = (target's one-way estimate, which the host puts in the spec block as RTT/2 in ms) + (observer's
  `app_rtt_ms` / 2) + (backlog ticks × 16.7).
- In the LAN test all processes share one clock (`platform_time`), so a debug field in
  `MSG_SPEC_CMDS` carries the target's send time and the test logs the measured value next to the
  estimate.
- The tooltip text says: "Time from the player's input to your screen. Minimum is one network hop to
  the host plus one hop to you."

## Determinism audit

Every system that changes what the target sees must be a pure function of (commands, replicated
state, seeds, tick), or be marked cosmetic.

- **Status: Pure.** No change.
- **Status: Fix.** The change is listed.
- **Status: Cosmetic.** It may differ by a converging filter or noise. The observer runs it with its
  own state and settings.

| System | Code | State | RNG / clock | Status | Change |
|---|---|---|---|---|---|
| Movement, mantle, hang, noclip | `player/movement.c` `player_move_tick` | `PlayerMove` | SIM_DT | Pure | none (already predicted + reconciled) |
| Aim recoil, recovery, countered | `gameplay/recoil.c` | `RecoilState` in `WeaponState` | pattern RNG from `mag_seed` (`recoil_mag_seed(seq, slot)`) + `shot_index` | Pure | none |
| Bloom / flinch | `weapons.c:144` `weapon_flinch` (host, on damage) | `rc.bloom` | — | Pure via reconcile | only from the replicated `PlayerSim` (host-only input) |
| Spread / cone | `weapons.c:109` `weapon_current_spread`, `chars_limb_spread` | `PlayerSim` + limb status | — | Pure | limb status is replicated (`AnimNetState`) |
| Shot directions | `weapons.c:609` `shot_seed(seq, slot, shots)` | — | seeded | Pure | none |
| Fire timing, burst, fire mode, mag | `weapons.c` `recoil_fire_budget` | `WeaponState` | SIM_DT | Pure | none |
| ADS progress, breath, sway_k | `weapons.c:619` `gun_feel_tick` | `WeaponState` | SIM_DT | Pure | none |
| Gameplay aim sway / ADS bob | `weapon_aim_offset_deg`, `weapon_sway_time(seq)` | — | seq-based | Pure | callers take the seq of the **viewed** slot's last command, not `G->last_cmd` |
| ADS blend (render) | `game.c` `G->ads_prev`, `game_ads_vis` | per local player | — | Fix | per view slot |
| Visual recoil springs | `game.c` `on_shot` / `weapon_feel_tick`, `G->recoil_vis` | Game | kick seeded by the shot seed; stepped SIM_DT | Fix | kick when `ev->slot == view_slot` on the first simulation of a command; step with the view slot's weapon |
| Crosshair spread | `game.c` `G->spread_prev/cur`, `ui/crosshair.c` | Game | — | Fix | view slot; `CH.ads_fade` is cosmetic |
| Anim controller, variants | `anim/controller.c` | `CharacterInst` | `AP_SEED` from replicated anim events | Pure | `chars_local_predict` for the view slot; excluded from `chars_client_tick` |
| Procedural hold / weapon_hold | `anim/weapon_hold.c`, `procedural.c` | `CharacterInst` | SIM_DT springs | Pure | the view slot forced to LOD 0 like the local one |
| Hold point breathing / bob phase | `player/holdpoint.c:500,513` `st->t`, `st->bob_phase` | `HoldPointState` | free-running since character creation | **Fix** | `st->t = seq * SIM_DT`; bob phase from the replicated move phase or seq, so a late observer is in phase |
| Hold point look lag | `holdpoint.c:492` | springs | aim from `player_inputs` | Fix | the view slot uses `player_inputs` (simulated `PlayerSim`), not `inputs_from_net` |
| Item / viewmodel kick on the body | `characters.c:1300` `hands_vm_kick` | `hands` singleton, sampled at render alpha | frame | **Fix** | per-tick post pass samples `recoil_vis` at the tick, keyed on view slot (today it reads a frame-sampled value) |
| Camera (eye smoothing, landing, FOV, roll) | `player/camera.c` `camera_update` | `ViewState` | frame dt | Cosmetic (converging) | observer uses its own `ViewState`; yaw / pitch = target command angles interpolated prev → cur by alpha (not mouse) |
| Camera bob phase | `camera.c:63` | `ViewState.bob_phase` | free-running | Fix if `cam_bob_scale > 0` (default 0) | phase from seq |
| Head stabilizer, camera collision | `camera_fp_math.c`, `camera_fp.c` `CamHeadStab` | Game | frame dt springs | Cosmetic (converging) | reset on target switch; collision from the viewed eye |
| First-person body / camera socket | `characters.c` `chars_fp_camera`, `fp_prepare`, `chars_render_local_fp` | singleton `FP`, `g_fp_aim_off` | — | Fix | pass view slot (already a parameter); head collapse on the viewed character |
| Hands rig (clip fades, interaction reach) | `player/hands.c` `hands_update` | singleton `H` | frame dt blends; reach from local `input_peek_press` + `G->focus` | Fix | reach from the forwarded cmd (`BTN_USE` edge + `use_target`); reset `H` on switch; fades cosmetic |
| Scope PiP, lens, reticle | `game/game_sights.c` | — | — | Pure given ADS + item pose | view slot |
| Hit markers, hurt markers, damage flash | `game_events.c`, `G->hitmarker_*`, `hurt[]` | Game | frame-dt decay | Fix (routing) | host also delivers `EV_HIT_CONFIRM` / `EV_HURT` / `EV_HINT` for the target to its in-eye observers; decay cosmetic |
| Kill feed | `EV_KILL` to all | — | — | Pure | none |
| Phone (landscape, rec, front cam, driven rotation) | `cmd.phone_flags`, `phone_rot` | UserCmd | — | Pure | none |
| Phone zoom | `phone.c:582` `PH.zoom` | local | frame dt, not in the cmd | **Fix** | zoom (u8) into `UserCmd`; observer phone renders the view slot |
| Vehicle seat, driving input | `PlayerMove` seat fields, vehicle block | replicated | — | Pure | none |
| Vehicle FP camera | `game_vehicles.c` `vehicles_view_frame`, `vehicles_camera` | `VG.prev_veh_yaw` | frame | Fix | the slot is a parameter; the forwarded cmd yaw already holds the vehicle yaw (no `view_frame` on top) |
| Vehicle 1st / 3rd person toggle | `VG.cam_tp` | local | — | Fix | bit in `UserCmd` (observer follows the target's choice) |
| Post feedback (low health, death desat, sprint, flash) | `game_post.c:486` `apply_feedback` | `GP` | frame-dt smoothing | Cosmetic | reads the view slot's health / mode |
| Lens droplets | `game_weather.c:388`, `weather.c:347` | own LCG seeded 0xBEEF | frame dt | Cosmetic | marked cosmetic (rain is weather-replicated; drop positions are noise) |
| Film grain, chromatic, vignette, TAA jitter | `render/post` | — | per frame | Cosmetic | none |
| Thunder audio delay | `game_weather.c:433` | — | `platform_time` | Cosmetic | none |
| Bolt-cycle sound / shells | `game.c` `G->bolt_timer` | Game | SIM_DT | Cosmetic | view slot |

There is no wall clock (`platform_time`) in any first-person gameplay path. The *Fix* rows are
(1) "local player" → "view slot" re-keying, and (2) three phase accumulators that run from creation
instead of from the tick: hold point `t`, hold point `bob_phase`, camera `bob_phase`.

## Validation (host)

- `spectate_validate(obs, mode, target)` (pure, unit-tested):
  - A target must be `active`, not `EF_DEAD`, not the observer, and not itself spectating.
  - The observer must be dead (`MOVE_DEAD`) or dev (`sv_cheats` / the host's local player via the
    `spectate` command).
  - `sv_spec_fp_enemies 0` refuses in-eye on another team. There are no teams yet, so `team_of()`
    returns 0 for everyone and the rule is ready for PvP.
  - `sv_spec_allowed 0` turns spectating off.
  - There is no round system yet, so "no spectating pre-round" hooks in as `spectate_round_allows()`,
    which is always true today.
- A target that dies or leaves auto-advances observers to the next legal target. If there is none,
  they go to the free camera.
- The observer never affects the target. Spectator RPCs only change `SpecObserver`. The observer's
  `MSG_CMDS` are still processed for its own dead player (it falls / lies dead) and the button bits
  are ignored.

## Death flow

1. On death, the existing ragdoll head camera (`camera_fp_death`) runs for `spec_death_cam_time`
   (default 3 s).
2. The observer then auto-enters in-eye on the killer (`EV_KILL ent2`) if that is a living player,
   else on the nearest living player. With nobody alive it goes to the free camera (noclip,
   `spec_free_collide` toggles collision).
3. Respawn:
   - `sv_respawn_time` (default 3 s, today's `PLAYER_RESPAWN_TIME`).
   - `sv_respawn_mode`: 0 auto (today), 1 on request (the `Respawn` button once the timer runs out).
   - With auto respawn at 3 s and a 3 s death cam, spectating only appears with mode 1 or longer
     respawn times.
   - The respawn timer is added to the spec block, so the HUD can count down.
4. `sv_join_dead 1`: late joiners join dead and start in spectate (the host flags the player dead
   right after `game_spawn_player`).
5. `spectate <player|next|prev|off> [eye|third|free]`: dev / host, without dying.

## Relevance and bandwidth

- `net_host_after_tick` captures for an in-eye or third-person observer from the target's position
  (`repl_capture`, `repl_rate_limit` viewer). The free camera captures from the camera position, sent
  in `MSG_CMDS` as the observer's `view_tick` sibling field.
- `chunks_update` and the openworld spawner also take observer anchors. The observer's corpse no
  longer anchors anything. Horde AI tiers keep using living players only, so an observer does not
  wake zombies.
- Cost per in-eye observer, at 60 Hz:
  - Forwarded commands: 4 × 32 B × 60 Hz ≈ 7.7 kB/s, the same as the target's own upstream.
  - Snapshot: the target's own snapshot size, since the private block and the relevance set are the
    same.
  - In total, about one extra client.
- `net_snapstats` logs it per observer. No per-target observer cap unless measurements ask for one;
  `sv_spec_max_per_target` is reserved.

## Spectator HUD

- **The target's own HUD**, drawn by the existing `hud_draw` with the view slot: vitals, ammo,
  reload / use rings, hold ring, hotbar, crosshair with their spread, hit markers, hurt markers,
  hints, kill feed and the vehicle HUD.
- **Spectator bar** (new `SCR_SPECTATE` overlay in `menus.c`, layout in `assets/ui`):
  - target name;
  - "SPECTATING — first person" / third person / free;
  - `<` `>` and the mode;
  - `spec delay: X ms` (its tooltip states the floor);
  - respawn countdown or `Respawn`;
  - `Leave`.
- Not in the game yet, so out of scope until they exist: weapon wheels, compass, pings, chat UI,
  teams, rounds. They plug into the same view-slot rule when they are added.

## Protocol (DG_PROTOCOL_VERSION 16)

- `MSG_SPEC_CMDS` (host → observer, unreliable): `{target u8, sent_ms u16, count u8, UserCmd[count]}`.
- The snapshot spec block: `{mode u8, target u8, flags u8, respawn_t u8 (0.1 s), target_owd_ms u16}`.
- `MSG_INVENTORY` gains the slot it describes.
- `UserCmd` gains `phone_zoom u8` and the `BTN2_VEH_TP` bit.
- `RPC_SPECTATE`.

## Tests

| Test | Kind | Checks |
|---|---|---|
| `test_spectate` | unit (pure C, runs anywhere) | target validation matrix; observers mask; command ring: in-order / gaps / duplicates / backlog catch-up / extrapolation limit / rewind on late arrival; spec block encode / decode; coverage: every `PlayerSim` / local-data field round-trips (struct enumerated with offsets, `memcmp` after a poisoned buffer) |
| `test_recoil` (extended) | unit | 600-tick recorded cmd stream through recoil + spread twice (separate state) → bit-identical |
| `spec_determinism.sh` | two processes | target runs a recorded 600-tick stream (`cmd_record` / `cmd_play`); observer in-eye. Both log a per-tick hash of camera transform, bone palette of the viewed character, `RecoilState`, spread, HUD state (`spec_dump`); `tools/tests/spec_compare.py` diffs them by seq |
| `spec_net.sh` | three processes, `net_fakelag 80 net_fakeloss 3` | measured delay ≤ 2 × lag + 1 tick; max pop < `spec_pop_warn`; ADS + scope PiP screenshot pair, mag dump, vehicle driving, phone out, reload / use rings |
| `spec_rules.cfg` | host script | illegal targets refused; relevance centre follows the target (`net_snapstats`); observer buttons do not change the host state |

## Implementation plan

1. View slot plumbing: `G->view_slot`, `game_view_player()`. Re-key every audit *Fix* row; nothing
   changes while `view_slot == local_slot`.
2. Tick-anchored phases: hold point `t` / `bob_phase`, camera `bob_phase`.
3. `src/net/spectate.c/.h`: observer table, validation, forwarding, the snapshot spec block,
   inventory and event routing, relevance anchor. `RPC_SPECTATE`, protocol bump.
4. `src/player/spectator.c/.h`: command ring, simulate / extrapolate / rewind, reconcile
   (`game_cl_reconcile` generalized), switching and inertialization, third-person and free camera,
   controls.
5. Death flow, `sv_respawn_*`, `sv_join_dead`, `spectate` command, settings (`spec_death_cam_time`,
   `spec_show_delay`, `spec_extrap_ticks`).
6. `SCR_SPECTATE` overlay.
7. Tests above, then docs and the CLAUDE.md map row.
