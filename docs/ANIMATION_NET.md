# Animation networking

The host is authoritative for **animation state** (state ids, timing, parameters, death, limb
status) and for **hits**. Poses are never sent. Every peer runs the same deterministic controller
(see ANIMATION.md) from replicated parameters and derives everything cosmetic locally:
* IK, foot IK and spine aim;
* hold glue;
* damage-reaction springs;
* ragdolls.

Code: `src/anim/anim_net.c` (quantization, encoding), `src/net/replication.c` (`NF_ANIMST` field),
`src/net/net.c` (`MSG_ANIM_EVENTS`, asset manifest), `src/gameplay/characters.c` (capture, client
playback, events, lag compensation).

## Asset identity

Animation only lines up if every machine has the same assets. `anim_lib_load` computes a 32-bit
hash per category from the file contents and parsed data:

`skeleton`, `clips`, `controller`, `hold_styles`, `anim_sets`, `hitboxes`, `ragdoll`, `characters`

The client sends all eight hashes in `HELLO`. On any mismatch the host replies `REJECT`, naming
each mismatched category with both hashes:

```
asset mismatch: clips (host 1a2b3c4d, you 99887766), hitboxes (...)
```

Hot reloads (`anim_reload`, `anim_rebake`, `character_reload`, `hitbox_reload`) change the hashes.
With clients connected the host warns that they'll desync until they load the same files. The
protocol version is **7**.

## Replicated state (`AnimNetState`, snapshot field `NF_ANIMST`)

Each character entity carries one `NF_ANIMST` field (bit 11 of the entity field mask) in the normal
delta-compressed snapshots. Inside it, a **u16 sub-field mask** marks which parts changed against
the client's acked baseline, followed by only those values:

| Bit | Field | Bytes | Encoding | Error bound |
|---|---|---|---|---|
| `ANS_STATE` | base + upper layer state ids | 2 | u8 each | exact |
| `ANS_PHASE` | base layer normalized phase | 1 | u8, 1/256 | ≤ 0.002 |
| `ANS_START` | state start tick | 2 | low 16 bits of the host tick | exact (wrap-safe) |
| `ANS_SPEED` | ground speed | 1 | u8, 0.05 m/s steps (0..12.75) | ≤ 0.025 m/s |
| `ANS_MOVE` | local move direction x, y | 2 | i8 each, /127 | ≤ 0.004 |
| `ANS_PITCH` | aim pitch | 2 | i16, rad × 10000 | ≤ 0.00005 rad |
| `ANS_YAW` | aim yaw | 2 | u16 over 2π | ≤ 0.0028° |
| `ANS_BODY` | body yaw (turn-in-place) | 2 | u16 over 2π | ≤ 0.0028° |
| `ANS_FLAGS` | grounded, crouch, prone, sprint, ADS, mantle, hang, dead | 1 | bits | exact |
| `ANS_LIMB` | limb status (`LIMB_LEGS`, `LIMB_ARMS`) | 1 | bits | exact |
| `ANS_WEAPON` | weapon type (hold style / `{weapon}` slots) | 1 | u8 | exact |
| `ANS_MANTLE` | mantle / vault progress | 1 | u8 | 1/255 |
| `ANS_CHAR` | character definition id | 1 | u8 | exact |
| `ANS_DEATH` | death tick + seed (only while dead) | 5 | u32 + u8 | exact |

**Sizes**, including the 2-byte sub-mask:
* walking and aiming (phase, speed, move, pitch, yaw, body): **12 bytes**;
* standing and aiming: about 8 bytes;
* idle and unchanged: nothing (the field isn't sent);
* the first full state of a live character: 21 bytes, or 26 when dead.

The quantizers are round-trip tested in `tests/test_anim.c` against the bounds above.

## Events (`MSG_ANIM_EVENTS`, reliable, ordered)

Discrete things that must happen exactly once travel as compact reliable events rather than state.
A message is `type u8 | count u8 | events…`, and each event is:

```
type u8 | entity u32 | host tick u32 | seed u8                         10 bytes
  + hit / die:      part u8 | direction u16 (octahedral) | strength u8 | damage type u8   +5
  + land / fire / equip: extra u8 (land speed × 8 / weapon)                               +1
```

| Event | Effect on remote characters |
|---|---|
| `AEV_JUMP`, `AEV_LAND` | controller triggers (land carries the vertical speed for `land_hard`) |
| `AEV_FIRE` | upper-layer fire + third-person recoil kick on the held item |
| `AEV_RELOAD`, `AEV_EQUIP`, `AEV_MELEE` | upper-layer triggers |
| `AEV_HIT` | damage reaction on the part (deduplicated against a local prediction) |
| `AEV_DIE` | death state (variant from seed) + local ragdoll with the hit impulse |

The host queues events during the tick (`chars_push_event`, which also applies them to the host's
own view) and flushes them after the snapshot.

**Clients apply events at their tick in render time.** Remote characters render `cl_interp` (100 ms)
behind the host, in interpolated snapshot time. An event that arrives early is held until the render
clock reaches its tick (or applied at once if it's more than 2 s ahead), so a death or hit lines
up with the state stream instead of jumping ahead of it. Events for the local predicted player
apply immediately.

## Client playback

For each remote character the client keeps the last 8 received states (`NetSample`):
* **Interpolation.** Continuous parameters (speed, move direction, aim, body yaw, mantle progress)
  are interpolated at the render tick. Discrete fields come from the older sample.
* **Controller.** Runs the same controller with those inputs. Triggers come from events, and the
  dead flag from the state (or from a die event, until a newer snapshot covers it).
* **Phase nudge.** In the same state, the base phase is pulled toward the host's at 8 % per tick, and
  snapped when the difference exceeds 0.25.
* **Resync.** If the base or upper state ids disagree for more than `anim_resync_ticks` (8), the
  client forces the host's states. It uses the replicated start tick and phase, with a short
  cross-fade, and counts a resync (`char_list`, debug panel; `anim_net_debug 1` logs each one).
* **Death.** It comes from the event, or for late joiners and long-dead characters from the
  replicated death tick and seed, which give a settled pose without simulation.

The local player's controller runs from the **predicted** `PlayerSim` (`chars_local_predict`), so its
own body and shadow don't lag. Its body yaw is simulated deterministically in movement, so prediction
and the host agree.

## Lag-compensated hits

* The host records each character's hitbox set every tick in a 32-tick ring.
* A `UserCmd` carries the shooter's view tick (fixed point, × 256).
* `chars_raycast` rewinds every character to that tick before tracing: an exact copy on whole
  ticks, otherwise an interpolation between neighbouring ticks.
* The client traces its own rendered (interpolated) pose for the predicted reaction and hitmarker.

`tests/test_anim.c` checks that the rewind is exact on whole ticks.

## Two-instance test

```sh
deadground --test --script tools/tests/anim_net_host.cfg &
deadground --test +connect 127.0.0.1 --script tools/tests/anim_net_client.cfg
# or: .toolchain/anet.sh (logs to build/host.log, build/client.log)
```

`+connect` must come before `--script`. The client sets `net_fakelag 150` and `net_fakeloss 5`
after connecting, because those cvars exist only once the transport does.

**Host.** Spawns a wandering zombie runner, a zombie brute and a rifle dummy in front of the
client's spawn and gives the client a rifle. Later it aims at one target and kills it (`char_kill`).

**Client.** Aims in ADS at the chest, head, upper leg, forearm and stomach of what it sees
(`aimat <part>`) and fires.

Both run with `anim_net_debug 1`. Check:
1. **Body parts match.** Each `lagcomp client: predicted hit ent N <part>` line has a matching
   `lagcomp host: hit ent N <part>` line.
2. **Events arrive once.** In `char_list`, `events hit/die` on the client equal the host's counts.
3. **States match.** The host and client `char_list` states agree, and `resyncs` stays 0.

Last run, at 150 ms lag with 5 % loss:
* 5 of 5 hits named the same part on both machines (chest, head, lower_arm_r on a zombie that stepped
  into the line, lower_arm_r, stomach);
* hit counts 4 + 1 and die 1 on both;
* 0 resyncs on every character, including the death.

**Late joiners.** `anim_late_host.cfg` kills a zombie, and `anim_late_client.cfg` connects about
5 s later. The client must list the zombie as `DEAD settled`: the seeded lying pose, no ragdoll
simulation, no die event (it happened before the join), and 0 resyncs, because the first received
state is adopted directly.
