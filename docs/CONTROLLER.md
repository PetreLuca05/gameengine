# Player controller: ledge grab and climb

The first-person controller (`src/player/movement.c`) moves the player on a Jolt CharacterVirtual:
walk / sprint / crouch, air control, coyote time, jump buffering, stairs, water
(docs/WATER.md "Swimming"). Ledges are handled by **one detector** and two rules on top of it.
- **Falling past a ledge in reach grabs it.** The player hangs and sees two prompts.
- **Running at a ledge and jumping again in reach climbs it** in one go, with no hang and no prompt.

| Area | Code | Data |
|---|---|---|
| Ledge detector, traversal (mantle / vault / pull-up), hang, shimmy | `src/player/movement.c` (`detect_ledge`, `player_move_tick`) | `pl_*` cvars |
| The rules (pure) | `src/player/ledge_rules.c/.h` (`ledge_decide`, `hang_decide`, `ledge_regrab_blocked`) | |
| Two-handed hook, hang prompts, `ledge_report` | `src/game/game_ledge.c` | action map (`src/input`) |
| Prompts on the HUD | `src/ui/hud.c` (`draw_hang_prompts`) | |
| Body / hands while hanging | `src/anim` (`AP_HANG`, `player_hang_grips`) | |
| Tests | `tests/test_ledge.c`, `tools/tests/ledge.cfg` + `ledge_check.py`, `hang_net.sh` | dev room ledge yard |

## The detector

`detect_ledge(feet, yaw, allow_vault, airborne)` runs these casts:
1. Forward sphere casts at several heights (reach `PLAYER_RADIUS + pl_mantle_reach`) find the wall.
2. A downward cast just past the face finds a walkable top.
3. A head-room cast above the player.
4. Optionally, the vault landing.
5. A capsule on top, standing, else crouched.

The ledge is classed by its height above the feet:

| Kind | Height | Use |
|---|---|---|
| `vault` | ≤ `pl_vault_max`, thin, free landing beyond | vault over |
| `mantle` | `pl_mantle_min` .. `pl_mantle_max` (ground) / `pl_air_mantle_max` (air) | mantle onto it |
| `hang` | above that, up to `pl_hang_reach`, with a free hang capsule under the edge | grab (fall case) / pull-up (auto-climb) |
| none | out of reach, no top, no clearance (head room or the capsule on top) | nothing: no climb into geometry |

## State machine

```
                 jump press on the ground with a ledge in reach            (unchanged)
   WALK ───────────────────────────────────────────────────────────────►  VAULT / MANTLE ──► WALK
    │ jump (climb_intent = forward held)  /  walk off an edge (no intent)
    ▼
  AIRBORNE
    ├─ climb_intent && forward held && jump pressed again in reach (the jump buffer counts;
    │  held from the take-off with pl_climb_hold_to_jump) && facing within pl_climb_facing_cone
    │        ──► AUTO-CLIMB: VAULT (low) | MANTLE (mid) | PULL-UP (high): no HANG state, no prompt
    ├─ falling (vel.y < 0) && crouch not held && a hang ledge in reach && facing within the cone
    │  && not the ledge in regrab cooldown && pl_hang_two_handed allows the held item
    │        ──► HANG  (prompts: [Space] Climb  [Ctrl/S] Let go)
    └─ otherwise ──► normal fall / landing ──► WALK

  HANG ── Space (a fresh press, hang_time > 0.15 s) + clearance ──► MANTLE (pull-up, 0.75 s) ──► WALK
       ── Ctrl pressed / S held ──► AIRBORNE with the current velocity; this ledge in regrab cooldown
       ── pl_hang_stamina > 0 and that many seconds hung ──► the same as letting go
       ── A / D (pl_hang_shimmy_speed > 0) ──► slides along the edge while the detector still finds a hang there

  SWIM ── forward + jump at an edge ──► the swim climb-out (mantle / hang from the surface, unchanged)
```

- **The decision** (`ledge_decide`): auto-climb wins when its inputs are there. A jump that started
  towards the ledge and is still moving towards it climbs, whatever the height. Otherwise the fall
  case grabs. A mantle-height ledge passed while falling is landed on, not grabbed.
- **The second press.** The take-off press is used by the jump, so only a new press counts: within
  `pl_jump_buffer` before or while the ledge is in reach. With `pl_climb_hold_to_jump 1`, holding the
  jump from the take-off counts too.
- **Release forward or skip the second jump** and the fall case applies: the player grabs the ledge
  on the way down and the prompts show.
- **Rising past a ledge** grabs nothing; the grab waits for the fall.
- **Crouch held while falling** is a deliberate drop: no grab (and with the crouched capsule, no
  traversal at all).
- **Regrab cooldown.** The cooldown (`pl_regrab_cooldown`) covers only the ledge just climbed from or
  let go of (within 1 m along it, 0.5 m in height), not every ledge.
- **Two-handed items.** `pl_hang_two_handed 0` forbids the grab with a two-handed item out. The
  default allows it: the long gun is slung while hanging, and a sidearm can be fired one-handed
  (game.c).
- **Clearance.** No auto-climb into geometry. A ledge without head room or without room for the
  capsule on top is not a ledge (the detector returns none): no climb and no grab. The pull-up from
  a hang re-runs the detector from the hang position for a fresh clearance check.
- **Let go** keeps the current velocity: zero while still, the shimmy's along the ledge.

## Rules (cvars, `CVAR_RULE`: host rules replicated to clients)

| Cvar | Default | |
|---|---|---|
| `pl_climb_facing_cone` | 45° | the view's angle from the wall's inward normal for a grab or an auto-climb |
| `pl_climb_hold_to_jump` | 0 | a held jump counts as the second press |
| `pl_regrab_cooldown` | 0.5 s | no regrab of the ledge just left |
| `pl_hang_stamina` | 0 | seconds the grip holds; 0 = no drain (the Chapter 1 rule) |
| `pl_hang_two_handed` | 1 | grab with a two-handed item out |
| `pl_hang_shimmy_speed` | 0.8 m/s | sideways along the ledge; 0 = no shimmy |
| `pl_mantle_*`, `pl_air_mantle_max`, `pl_vault_*`, `pl_hang_reach`, `pl_hang_yaw_limit` | | the detector's heights / reach, speeds, the view limit while hanging (unchanged) |

`pl_debug_mantle 1` draws the detector's casts.

## Prompts

Local UI only (`ledge_hang_prompts`): the local player hanging, not a spectated one. The key names
come from the action map: the first keyboard binding of Jump, and of Crouch / Move Back. With a
gamepad connected, the pad bindings are shown instead (A / B by default). They are drawn under the
crosshair in the interaction prompt's style. The HUD logs `ledge: prompts shown: ...` / `ledge:
prompts hidden` when they appear and go.

## Network

Hang, mantle, vault and pull-up are predicted controller states, as before. The new `PlayerMove`
fields (`climb_intent`, `regrab_point`, `hang_used`) are part of the predicted state and are
reconciled with it (protocol 27). Spectators get them in the local data block. The prompt is local UI
and never replicated.

## Tests

- `test_ledge` (ctest `ledge`, pure):
  - the fall grab, and no grab when rising, crouch-dropping, facing away or forbidden by the
    two-handed rule;
  - the auto-climb's vault / mantle / pull-up, and the second press against a press still held
    (with and without `pl_climb_hold_to_jump`);
  - forward released or no take-off intent: grab;
  - the regrab cooldown per ledge;
  - the hang's climb / let go / stamina.
- `tools/tests/ledge.cfg` + `ledge_check.py` (game, dev room ledge yard at x 50..66, z 10, three
  2.6 m walls):
  - fall past a ledge: hang + both prompts;
  - let go: drops to the floor;
  - Space: on top;
  - run + jump + forward + second jump: on top with no hang and no prompt;
  - the same without the second jump: hang + prompts;
  - regrab blocked with the cooldown, immediate without it;
  - `pl_hang_stamina 1`: drops after a second;
  - a slab over the top: no climb.

  Run it with `deadground --test --nosound --log ledge.log --script tools/tests/ledge.cfg`, then
  `python3 tools/tests/ledge_check.py ledge.log`.
- `tools/tests/hang_net.sh` (LAN): a client watches the host hang, shoot a sidearm and climb.
