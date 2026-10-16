# Deadground architecture

This covers milestone 1: the engine foundation, the DevRoom, and every core system.
Game code is C11. C++ appears only inside third-party libraries that expose C APIs
(Jolt/joltc, VMA) and in two isolated shims (`third_party/jolt_shim`, `third_party/steam_shim`).

## Ground rules

* **One global:** `Engine g_engine` (`src/core/engine.h`). Every subsystem's state is a struct owned
  through a pointer on it: `g_engine.render`, `.input`, `.net`, `.game`, `.log`, `.cvars`, `.jobs`, ...
  Modules keep no other mutable globals. The only file-scope statics are immutable tables, such as
  joltc's callback tables, which joltc keeps by pointer.
* **Explicit memory:** `Arena` (`core/arena.h`) gives three lifetimes: `perm_arena` (process),
  `level_arena` (reset on map change; mesh collision data lives here) and `frame_arena` (reset every
  frame; render sort keys and similar scratch). Large per-world tables are allocated once, and
  everything else is malloc/free with clear ownership.
* **Fixed timestep:** the simulation runs at 60 Hz (`SIM_DT`). Rendering runs at display rate and
  interpolates between the previous and current tick.
* **Data driven:** items (`data/items.json`), weapons (`data/weapons/*.json`, one file per gun, loaded sorted by file name so indices match on every machine), the level
  (`data/levels/devroom.json`), hand poses (`data/hand_poses.json`) and the asset manifest
  (`data/assets.json`). Adding a weapon or item needs no code changes.
* **Plain structs + function pointers** for every extension point: `NetTransport`, `ChunkProvider`,
  `WorldGen` and `InteractDef`.

## Module map

```
src/
  app/        main.c (boot, frame loop, command line), video.c (window mode / resolution / FPS cap)
  core/       arena, log, cvar (cvars + commands + command buffer + config), jobs (thread pool),
              platform (files, paths, time), dgmath (vec/mat/quat, ray-capsule), crash (stack traces)
  render/     Vulkan 1.3 backend: vk_device (instance/device/swapchain/targets), vk_resources
              (VMA buffers, staged uploads, textures + mips, deferred deletion), vk_pipelines
              (pipelines, shader hot reload), render.c (frame recording), mesh_gen (procedural meshes),
              debug_draw (timed lines/shapes/text)
  physics/    Jolt wrapper: bodies, layers, ray/shape casts, overlaps, CharacterVirtual, constraints,
              debug draw; ragdoll (cosmetic Jolt ragdolls on the shared skeleton)
  anim/       shared-skeleton animation: anim (skeleton, clips, pose math, IK), anim_mesh (skinned
              meshes + LODs), controller (layers / state machines / blend trees), procedural
              (turn, spine, weapon hold + hand glue, damage additive, foot IK), anim_net
              (quantized state + events), ragdoll_def, anim_library (all assets + hashes),
              character_rt (per-character runtime)
  input/      action map (keyboard/mouse/gamepad), rebinding + capture, +action script commands
  player/     movement (controller + mantle/vault/hang), camera (bob, FOV, stair smoothing),
              hands (arms + hands rig, pose clips, two-bone IK)
  gameplay/   items + inventory, health/damage, weapons (hitscan/projectile/melee), entities
              (doors, gates, levers, switches, lights, containers, pickups, props, dummies),
              characters (skinned characters in the world: sim, client playback, LOD, ragdolls,
              zombies), hitboxes (per-part hitboxes, lag-comp ring, damage multipliers),
              events (gameplay -> net/presentation), effects (decals, sparks, tracers, damage numbers)
  world/      world (entity table + components + players), level (JSON level builder),
              chunks (streaming, terrain LOD, heightfields, WorldGen)
  net/        net_transport.h + transport_lan (ENet) + transport_steam (shim) + conditioner (fake
              lag/loss), protocol, bitbuf, replication (snapshots, deltas, interpolation), net.c (session)
  ui/         ui (Nuklear + HUD batcher), font (baked atlas, world text), console, hud, menus,
              recoil_debug, anim_debug (controller panel + animation test stand)
  audio/      miniaudio engine, sound groups, 3D one-shots, music loop
  assets/     glTF (cgltf), PNG/JPG (stb_image), KTX2, asset manifest, FBX (ufbx) + bake cache,
              character definitions + rig validation
  game/       game.c (session/tick orchestration), game_events.c (client events, RPCs),
              game_render.c (presentation), usercmd.h
shaders/      lit, shadow, lines, text, post (ACES + FXAA + gamma), ui, skin / skin_shadow (GPU skinning)
              (GLSL -> SPIR-V at build time)
tools/        gen_assets.py (placeholder content), bake_anim (FBX -> cache), validate_rig,
              tests/*.cfg (scripted automation)
tests/        test_recoil, test_anim, test_ragdoll (ctest)
```

## Frame loop (`app/main.c`)

```
for each frame:
    input_begin_frame / ui_input_begin
    poll SDL events  -> console (~), menus (Esc), Nuklear, input action state
    input_end_frame  -> look delta (sensitivity/ADS/invert), move axes, rising-edge counters
    cbuf_execute     -> console / script commands (supports "wait N")
    net_update       -> transport poll, discovery, message dispatch
    game_frame(dt):
        apply mouse look to the client-owned view angles (every frame, not per tick)
        hotbar input
        accumulator += dt; while (acc >= 1/60): tick()
            build UserCmd from input (edges consumed per tick, so short taps still register)
            HOST:   host_tick -> every player: movement, weapons, interaction (validated)
                    -> entities, projectiles, chunks, Jolt step, lag-comp history
                    -> dispatch events (local player directly, remote players via net)
                    -> net_host_after_tick (snapshots at sv_snaprate, inventories, server cvars)
            CLIENT: store cmd in history, send last 4 cmds (redundancy), predict the local player
        alpha = acc / dt_tick
        client: interpolate remote entities at (server time - cl_interp), sync kinematic proxies
        interaction focus raycast, HUD timers, effects
    jobs_pump_completions   -> finished chunk generation jobs -> main-thread finalize
    render_begin_frame -> game_render -> ui_render -> render_end_frame
    optional FPS cap
```

### Rendering

`render_end_frame` records one command buffer per frame. It uses two frames in flight and a
three-image swapchain:

1. **Pending uploads.** Staging copies for meshes and textures, plus mip generation by blitting.
   Staging buffers are freed through the frame slot's deferred-deletion list.
2. **Shadow pass.** One directional sun, an ortho frustum snapped to texels, quality from `r_shadow_quality`.
3. **HDR scene pass** at `r_render_scale`. In order:
   * opaque draws, sorted by texture and mesh with frustum culling;
   * blended draws;
   * depth-tested debug lines;
   * world text (labels, debug text);
   * a depth clear, then the **viewmodel** (hands, arms, held weapon);
   * overlay lines.
4. **Swapchain pass.** Post-processing (ACES tonemap, FXAA, brightness/gamma, vignette), then UI:
   the HUD batch, then Nuklear draw commands with per-command scissor.

Game code never touches Vulkan. It submits `DrawItem`s (mesh + material + matrix), point lights,
lines, text quads and UI vertices during the frame.

## Simulation model

### World and entities (`world/world.h`)

* `EntityId` = 20-bit generation + 12-bit index. The **host allocates** IDs, and clients spawn replicas
  at exactly the same index and generation (`ent_spawn_at`), so IDs are stable network handles.
  Indices ≥ `CLIENT_ENTITY_BASE` are reserved for client-only entities.
* Component data lives in fixed arrays indexed by entity index, with a `comps` bitmask per entity:
  transform (+ previous transform for interpolation), visual, physics body, health, interactable,
  door, lever, light, container, pickup, projectile and hitbox.
* `Player[MAX_PLAYERS]` holds per-slot state: `PlayerSim` (predicted), the Jolt `CharacterVirtual`,
  inventory, the host's command queue, hold-interaction state and the open container.
* Static level geometry isn't made of entities. It's built identically on every machine from the level
  file, as merged vertex-colored meshes and static Jolt bodies.

### Player simulation (`player/movement.c`, `gameplay/weapons.c`)

`game_sim_player(world, slot, cmd, dt, first)` is the only place a player advances. Both the host and
client-side prediction run it:

* `player_move_tick`: CharacterVirtual `ExtendedUpdate` (stairs + stick-to-floor), Quake-style
  acceleration and friction, air control, coyote time, jump buffering, crouch with head-room check, and
  steep-slope cancel. **Ledge traversal** uses a forward sphere probe at stacked heights, then a
  downward probe past the wall face, a head-room cast and a capsule clearance test on top. The result is:
  * **mantle**: a scripted two-phase path;
  * **vault**: thin obstacle, sprinting, free landing beyond;
  * **hang**: airborne and above mantle height. You then pull up or drop.

  It also writes world-space **hand anchors** for the IK system.
* `weapons_tick`: equip, consumables, melee sweep during active frames, guns (fire modes, sub-tick
  RPM, spread cone, three-layer recoil, ADS blend, sway / hold breath, magazine, reload), projectiles.
  Clients predict the **magazine** and the **aim recoil state** (`RecoilState` in `WeaponState`), and only
  the host edits inventories. Recoil math is the pure module `gameplay/recoil.c` (unit tested). See
  [docs/RECOIL.md](docs/RECOIL.md).
* The host simulates a remote player **once per received command** and never on a guess. A starved
  queue waits and a backed-up one catches up two commands per tick, so fire timing, recoil and ammo
  replay identically on the client.
* `first == false` during reconciliation replays, so effects, sounds and recoil happen once.

### Interactions

`InteractDef` has `on_focus` (prompt, runs on the client), `can_use`, `on_use`, `on_hold` (both run on
the host), and `hand_target` (where each IK hand should reach, in world space). Clients send the focused
entity in their `UserCmd`. The host validates existence, the enabled flag, distance (with latency slack)
and line of sight before calling the callbacks. Hold interactions accumulate `hold_progress` on the host,
which reaches the owning client through the private snapshot block.

### Events

Authoritative code never talks to the network or presentation directly. It pushes `GameEvent`s:
hint, sound, damage number, hit confirm, hurt direction, impact, tracer, muzzle, kill, container
contents, respawn. After each tick the host delivers events locally to its own player and sends them
reliably to remote players, honoring `except` so the shooter doesn't get their own predicted tracer
twice.

## Networking (`net/`)

```
       game.c / net.c (session: roles, handshake, snapshots, events, RPCs, stats)
                          |
                 NetTransport (net_transport.h)
                 /                  |                  \
     transport_lan.c (ENet)   transport_steam.c   transport_conditioner.c
     UDP, reliable + unrel.   -> steam_shim.cpp    wraps either: net_fakelag,
     channels, fragmentation     (C++ Steamworks)  net_fakejitter, net_fakeloss
```

* **Topology:** host-authoritative peer-to-peer. One player hosts; single player is simply a host
  with no remote peers. The host's local player runs the authoritative simulation directly, with no
  serialization loopback.
* **Handshake:** HELLO (protocol version, name) → WELCOME (slot, map, seed, tick), then server cvars
  (`CVAR_SERVER`, so movement tuning matches for prediction) and the inventory.
* **Client → host:** an unreliable `CMDS` packet every tick carrying the last 4 `UserCmd`s
  (redundancy against loss), the last received snapshot tick (ack), and the client's render tick
  (for lag compensation). The host queues commands per player and drops duplicates. When input is
  missing it repeats the last command without edge buttons, and it trims the queue to bound latency.
* **Host → client snapshots** at `sv_snaprate` (default 30 Hz), unreliable:
  * header: tick, baseline tick, last processed command seq;
  * private `PlayerSim` for reconciliation, plus hold progress and the open container;
  * entity deltas, then removals.

  Entities are captured per client, within `sv_relevance` of the player (or the chunk-stream radius in
  streamed worlds). Lights, gates and players are always relevant. Each entity is delta-encoded
  field by field (`NF_*` masks) against the client's **last acknowledged** snapshot, kept in a
  32-entry per-client history. New entities send all fields, missing ones are listed as removals,
  rotations use smallest-three packing, and there's no baseline until the client acks one. ENet
  fragments the large first snapshot.
* **Client prediction:** each tick the local player is simulated immediately. When a snapshot arrives,
  the client restores the host's `PlayerSim` for the acked seq and replays newer commands from its
  history. Any visual error is folded into a decaying `error_offset`, so corrections are smooth, and
  `prediction_error` is shown in the stats overlay.
* **Interpolation:** remote entities are buffered (16 samples) and rendered at
  `server_time - cl_interp` (100 ms default). Server time is estimated from snapshot ticks; the
  estimate jumps forward on early packets and drifts back slowly. Remote players and props get
  kinematic Jolt proxies on the client, so the predicted local player collides with them.
* **Lag compensation:** the host records hitbox positions for 64 ticks. Hitscan and melee traces
  rewind hitboxes to the client's view tick. World geometry is traced live through Jolt.
* **Reliable channel:** events, inventories, RPCs (inventory move/drop, container take/put/close),
  server cvars.
* **LAN discovery:** the host answers `DGQ1` broadcasts on UDP 27016 with name, map, players and port.
* **Steam:** listen with `CreateListenSocketP2P`, connect with `ConnectP2P` over Valve's relay. The host
  creates a friends-only lobby; joining a lobby connects to its owner. Rich presence sets
  `connect = "+connect_lobby <id>"`. `GameLobbyJoinRequested` (friends list) and
  `GameRichPresenceJoinRequested` turn into `connect_lobby` commands.

## Hands and arms IK (`player/hands.c`)

The placeholder visuals are a cube per hand and two boxes per arm. The pipeline is built for real hand
models and authored poses:

1. **Base layer:** keyframed clips from `data/hand_poses.json`. Each key has a camera-space position,
   rotation and grip type per hand; clips crossfade over 150 ms. Timed clips are sampled by action
   progress: `melee_swing` by swing time, `reload_*` by reload time, `consumable_use`, `mantle`,
   `vault`. For weapon clips the right hand **is** the weapon grip frame: the weapon model attaches to it,
   and the left hand IK-attaches to the weapon's `support` point.
2. **Procedural layer:** walk bob, look sway on springs, velocity lag, landing dip, breathing and recoil.
3. **World-space overrides,** blended per hand by weight:
   * ledge anchors written by the movement code (hang, mantle, and vault with one hand planted);
   * interaction reach driven by each `InteractDef.hand_target`, re-evaluated every frame so the hand
     follows the door knob as the door swings and the lever handle as it rotates. A container uses
     both hands on the lid's front edge, a switch uses a pointing finger, a pickup the item.
4. **Solve:** analytic two-bone IK with a down-and-out elbow pole vector. Out-of-reach targets clamp at
   full extension, and `hands_debug 1` draws the chains.

**To swap in real models:** replace `draw_arm()` and `draw_hand()` in `hands.c` with skinned meshes
driven by the solved shoulder/elbow/wrist chain and `grip`, and author more clips in the JSON
(`hands_reload` hot-reloads it). Remote players use the same solver (`hands_render_remote`), driven by
their replicated weapon and state.

## Characters and animation (`anim/`, `gameplay/characters.c`)

Players, zombies and dummies are skinned characters on one canonical skeleton
(`assets/human/Dummy.fbx`):

* **Host.** Runs a sim-only controller for every character each tick. That produces per-part
  hitboxes (the ragdoll bodies) recorded for lag compensation, and a quantized `AnimNetState`
  replicated in snapshots (`NF_ANIMST`). Discrete moments (jump, land, fire, reload, hit, die, …)
  are compact reliable `MSG_ANIM_EVENTS`.
* **Clients.** Run the same deterministic controller from the replicated parameters, snap to the
  host's states on divergence, and add every cosmetic layer locally: spine aim, weapon hold with
  hand glue, damage reactions, foot IK, ragdolls.
* **Render side.** A LOD system (projected pixels, hysteresis, update intervals, per-LOD systems)
  feeds GPU-skinned instanced draws.

See [docs/ANIMATION.md](docs/ANIMATION.md), [docs/DAMAGE.md](docs/DAMAGE.md) and
[docs/ANIMATION_NET.md](docs/ANIMATION_NET.md).

The first-person hands (`player/hands.c`, below) are still a separate rig. The full-body local
character is drawn shadow-only.

## World streaming (`world/chunks.c`)

* The world is a grid of 32 m chunks with 33×33 height samples (shared edges).
* `ChunkProvider { generate(user, coord, out), height_at(user, x, z) }` is the content plug-in point.
  `generate` runs on the job system. The main thread finalizes a budgeted number of chunks per frame:
  four LOD meshes (stride 1/2/4/8, with skirts to hide cracks) and a Jolt `HeightFieldShape`.
* Chunks load nearest-first around **every player** within `world_chunk_radius`. They unload after
  2 s without being needed, which gives hysteresis at the border. Network relevance uses the same
  radius.
* `WorldGen` is a seeded interface with `height`, `biome` and `place_structures`. The demo implements
  height (FastNoiseLite fBm plus ridged noise, with a flattened spawn clearing). Biome is height-based,
  and structure placement returns deterministic candidate points without spawning anything yet.
* Clients generate terrain locally from the replicated seed, so terrain isn't sent over the network.

## Where future systems plug in

| System | Hook |
|---|---|
| **Zombie AI** | `ET_ZOMBIE` exists: a skinned character with a kinematic capsule and a test wander / attack behaviour (`zombie_ai` in `gameplay/characters.c`, host only). Real AI replaces `zombie_ai`. It should drive `CharInputs` (speed, move direction, aim, triggers) and move the capsule; animation, hitboxes, replication and ragdolls already follow. |
| **Spawning / director** | A host-side system that listens to `chunks_update` loads/unloads and to WorldGen structure points. |
| **Real terrain / biomes / structures** | Implement a `ChunkProvider` or extend `WorldGen` (biome + `place_structures`). Spawn structure entities in the finalize step on the host (relevance handles replication), or build them as chunk-local static geometry on every machine for deterministic content. |
| **Hand / character models** | Character models: a new FBX on the canonical rig + `assets/characters/<id>.json` (check with `tools/validate_rig`). First-person hands: `hands.c` rendering (see above). |
| **More weapons / items** | JSON only. New behavior goes in `weapons_tick` branches keyed by `WeaponKind`. |
| **Persistence / save** | `World` + `Inventory` are plain data. Serialize entity component arrays the same way replication captures them (`repl_capture`). |
| **Dedicated server** | The host code path doesn't depend on rendering. Add a headless main that skips `render_*`/`ui_*` and sets `g_engine.headless`. |

## Debugging aids

* The Vulkan validation layer is opt-in with `--validation`.
* The crash handler logs a symbolized stack trace to `deadground.log` (Windows/DbgHelp).
* `--test --script` automation with `screenshot`, `getpos`, `inv`, `status`, `lookat`, and `+action`/`-action`
  input drives the game headlessly. That's how everything here was verified.
