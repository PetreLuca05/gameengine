# Chapter 1: "10th Floor"

**Status: built.** Framework: docs/STORY.md. Kit: docs/BUILDING_KIT.md. Single player (`"players": 1`): the host is not reachable on the LAN and refuses joins; co-op comes with the crew-assembly chapters later.

Generated files (edit the generators, not their output):
- `assets/story/chapters/01_tenth_floor.json`: `tools/story/chapter01_gen.py`.
- `data/levels/story_01.json` and the prefabs: `tools/story/build_story01.py`, with the street site in `tools/story/site01.py` and the city in `tools/story/vista_gen.py` (`assets/story/vista/vellmouth.json`).
- Hand-written: `assets/story/scripts/01.story` (texts), `assets/story/vista/sequences.json` (what the city does each day).

Sketches (regenerate with `python tools/story/ch01_sketches.py`): `docs/story/ch01/layout_plan.svg`, `vista_site_plan.svg`, `vista_sofa.svg`, `vista_balcony.svg`.

## 1. Premise

Spring, Monday. You are in flat **10A**, the north-east corner flat on the 10th floor of a 16-storey residential tower in **Vellmouth**, a fictional river city. Only the protagonist is in the game: no family members or other people appear as characters (the family is for later chapters).

1. **A normal day.** You wake on the living-room sofa, facing the TV. Coffee, the phone, the TV, a window: an ordinary morning; the city through the glass is busy and normal.
2. **The emergency broadcast** interrupts the TV: stay indoors, lock your doors, let no one in. Sirens; the traffic stops; a helicopter crosses the north. You lock the front door.
3. **Seven days inside.** Each evening the bed takes you; while you sleep the world gets worse. Every morning you wake beside the bed and look out: more smoke, more darkness at night, wrecks in the street below, more of the dead walking it, and more of them in the hallway outside your door (the door viewer shows them).
4. **The rescue.** On the morning of day 8 the radio says a helicopter will land on the roof of **Orchard House**, the building across the street (north-east). You watch it come in. Then: out of the flat, through the hallway, down ten floors of stairs, across the street and up Orchard House's stairs to its roof.

## 2. Day structure (progression logic)

The story clock drives the time of day (docs/STORY.md "Clock"); each day's timeline beats carry a `"day"` so the clock can restart every morning.

| Day | Morning card | Clock | Weather | The city (vista `day_N`) | Hallway | Street below |
|---|---|---|---|---|---|---|
| 1 | Monday | 07:40 → the broadcast (~10:30) → 21:00 | clear → little_cloudy | Normal; traffic builds, then stalls at the broadcast; a helicopter crosses the north. | empty | traffic |
| 2 | The power is still on | 07:30 → 21:00 | cloudy | Traffic dead, two fires, the birds scatter, the first of them on the roads; abandoned cars on the street. | 1 | cars |
| 3 | The east side went dark | | light_rain | East district dark; St. Aldric burns; a pile-up in the junction, a police car. | 2 | 2 zombies |
| 4 | No power | | cloudy+fog | Halden Tower's district flickers; more fires north; burnt-out cars, an overturned van; the north block's floors burnt out. **The flat's and the building's power go out during this night.** | 4 | 4 |
| 5 | The streets are full of them | | rain | Most of the city dark at night; the bus jack-knifed across the street in front of the tower. | 5 | 6 |
| 6 | Smoke over the river | | overcast+fog | A few generators left; an army truck, sandbags at Orchard House; a military helicopter far off. | 7 | 8 |
| 7 | Hold on | | storm | Darkness; the city burns; rubble from the north block. | 9 | 10 |
| 8 | Rescue | 07:30 → | little_cloudy | The world as on day 7; the rescue helicopter flies in over the river and lands on Orchard House. | 5 left (the rest followed the helicopter's noise) | 12 |

**A day:** the day's objective (`day_N`, "Stay inside…") is on the HUD. From **19:00** the bed is usable ("Get some sleep", marker on the bed; before that it says "Not tired yet"); holding use on the bed sleeps. The clock reaches 19:00 about 4.5 minutes into a day.

**The night** (trigger `night_N`, all while the screen is black):
1. Fade out. The day objective completes.
2. The world changes: the vista's `day_<N+1>` sequence, the corridor's new zombies (`hall_zK`), the street's new zombies (`street_zK`), the weather, the power (night 4).
3. The clock jumps to 07:30, you stand beside the bed (`teleport spawn_bed`, +25 health: sleep heals), **checkpoint `day_<N+1>`** (autosave).
4. Fade in with the day card ("DAY 3" / "The east side went dark", the HUD's end-card style), and on some mornings a line of thought.

All per-day numbers are constants at the top of `tools/story/chapter01_gen.py` (`DAYS`, `HALL`, `STREET`, `WEATHER`, `POWER_OFF_NIGHT`, `DAY_LEN`, `TIRED`).

## 3. Objective graph

```
wake ──► morning (any 2 of: coffee · phone · TV · open a window) ──┐   (or 240 s fallback)
                                                                   ▼
            broadcast (the TV, ~24 s) ──► lock_door ──► day_1 ─┬─ [opt] peek_hall (door viewer)
                                                               └─ [opt] look_out (lean out of a window / over the railing)
day_1 ──19:00──► sleep_1 ──night──► day_2 ──19:00──► sleep_2 ──night──► … day_7 ──► sleep_7 ──night──►
rescue: leave_flat (corridor) ► stairs (ground lobby) ► street (Orchard House lobby) ► roof ► board (helipad) ──► END
```

- Checkpoints: `start`, `day_2` … `day_8` (every morning), `street` (entering the ground lobby on day 8), `end`.
- The bot test (`tests/test_story.c`) plays every branch to the end: 16 seeds, all 9 checkpoints, a save / load at each, deterministic. The in-engine run is `tools/tests/story_bot.cfg` (`story_time 2400`).

## 4. Interactables

| Name | Where | What |
|---|---|---|
| `phone_sofa` | sofa seat | Check phone: the morning's notifications (`msg.phone_day1`). |
| `tv_living` | the TV | Turn on the TV (day 1); the broadcast plays from it. |
| `coffee_maker` | kitchen counter | Make coffee. |
| `radio_kitchen` | kitchen counter | Listen to the radio (battery); the rescue call on day 8 plays from it. |
| `bed_master` | master bed | Hold to sleep, only while a `sleep_*` objective is active (`"when": "sleep_*"`, else "Not tired yet"). |
| `door_lock_10a` | by the front door | Lock the door (only during `lock_door`). The door stays locked until the rescue morning. |
| `peephole_10a` | front door | The door viewer: a fisheye look into the corridor (game_peek.c). |
| windows `win_*` + `lean` spots | casements | Open / Close Window; Lean Out (§6). |
| `lean_over` spots | balcony balustrades | Lean Over the railing. |
| `pickup_bat` | teen room | The bat (melee for the escape). |
| `lift_call` | lift lobby | The lifts are dead. |

## 5. Apartment layout plan (unit 10A)

Plan: `docs/story/ch01/layout_plan.svg`.

**Coordinates are unit-local.** The origin is the north-west interior corner. +x is east, +z is south, y is up, and yaw 0 looks along −Z (north), the engine convention. The floor top is at **y = 30.0**: ground floor 0, floor-to-floor 3.0, 10th floor. The unit sits in the tower's north-east corner.

- **Footprint:** 12 × 12 m minus the 3 × 5 m notch that belongs to 10B. That is **129 m² gross** (about 118 m² net of walls).
- **Ceiling:** 2.7 m.
- **Glass:** the **North (z = 0) and East (x = 12) walls are full-height, full-width glass**, each 8 modules of 1.5 m. A **corner mullion** sits at (12, 0).
- **Plan rule:** every partition that meets the glass lands on a mullion. That puts partitions at x = 4.5, z = 6.0 and z = 9.0.
- **Sliding door:** the north module x 10.5..12.0 (a glass door) opens onto a **1.2 m wraparound balcony** (north x 4.5..13.2, east z −1.2..6.0) with a glass balustrade 1.1 m high. Every balustrade module has a "Lean Over" spot.
- **Windows that open** (`glass_casement_150`, §6): the master bedroom's north module x 1.5..3.0 (`win_master`), the teen room's east module z 7.5..9.0 (`win_teen`), the child room's east module z 10.5..12.0 (`win_child`): the rooms without a balcony in front.

| Room | x | z | m² | Glass | Key props / interactables |
|---|---|---|---|---|---|
| Living | 7.5–12 | 0–6 (open) | 45 (open plan) | N + E (hero corner) | L-sofa (the start: `spawn_sofa`, facing the TV), armchair, TV on a credenza on the south partition, reading lamp, rug, plant. Sliding door to the balcony. |
| Dining | 4.5–7.5 | 0–3 | (in 45) | N | Table for 4, chairs. |
| Kitchen | 4.5–9.6 | 3–6 | (in 45) | | Run on the south partition: fridge (water, energy bars), drawers (knife), sink, dishwasher, hob, cabinets (canned food, water). Coffee maker, radio. |
| Master bedroom | 0–4.5 | 0–4.5 | 20.3 | N (one casement) | The bed (sleep; you wake beside it each morning, `spawn_bed`, looking north at the city), nightstands, dresser. |
| En-suite / walk-in | 0–4.5 | 4.5–7 | 11.3 | | Bathroom, closet. |
| Hall | 4.5–6.0 | 6–12 | 9.0 | | The front door to the corridor (door viewer, the lock), shoe bench, coat rack, coat closet (backpack). |
| Utility | 3.0–4.5 | 7–10.5 | 5.3 | | Washer / dryer, breaker panel (decor), water heater, shelf (tape, batteries, a flare). |
| Study / bath / storage | 6.0–8.4 | 6–12 | 14.5 | | Desk, family bath, storage. |
| Teen room | 8.4–12 | 6–9 | 10.8 | E (one casement) | Bed, desk, trophies, **the bat**, wardrobe. |
| Child room | 8.4–12 | 9–12 | 10.8 | E (one casement) | Loft bed, toy chest (a flare), toys. |

**Outside the unit, the 10th-floor shared corridor** (z 12.3..14.1, x −8.5..12) contains the locked doors of 10B, 10C and 10D, the lift lobby (dead lifts), a riser panel, the east end window and, at the west end, the fire door to the stair core (§7).

Lighting: ceiling downlights per room on the `apt_10a` power group (they go dark for good on night 4), the corridor and the stair core on `building`.

## 6. Windows, leaning out, the door viewer

- **Windows** (docs/BUILDING_KIT.md "Windows"): use opens a casement; the sash swings out of the room. A character-only collision box (`PL_CLIP`) fills the opening: nobody climbs out, but you can lean.
- **Leaning out** (`src/game/game_lean.c`): with a window open, use on it again leans out. The camera moves 0.55 m out past the glass and drops 0.12 m. You can look along the facade and straight down at the street (85° either side, 82° down), which is how you watch the street fill up. Use, moving, jumping or attacking ends it. The balcony railings work the same way ("Lean Over").
- **The door viewer**: the peephole in 10A's door (`peephole_10a`, the door peek of docs/UI_WHEELS_HUD.md). The corridor's zombies spawn right outside the door first (`hall_z1..` from x 5.25 outwards) and shuffle within 1.6 m of their spot (`"wander": 1.6`), so each morning the view is fuller. They do not chase or attack while the door is shut: story zombies stay idle until a `chase` action wakes them (§7).

## 7. Escape route (day 8)

Site plan (x east, z south; the tower is x −12.3..12.15, z 0..24.3). Built by `tools/story/site01.py`:

- **The tower:** the stair core runs from the ground floor to the 11th, one storey per `stair_core_dogleg` (`f0_`…`f11_`). Every fire door is locked except the ground floor's and the 10th's; the chapter unlocks the 10th's on night 7.
  - The ground storey has a lobby (x −8.35..6, z 0.3..17.5): the stair core's fire door is on its west wall, a glass entrance (`lobby_door`) on the north face.
- **Streets:** a plaza north of the tower (z −8..0), the north street (z −22..−8), the east street (x 26..38).
  - The street walls are box buildings: low (12–18 m) so the 10th floor sees over them.
  - Jersey barriers and fencing close the street ends, and a fence closes the alley west of the tower.
- **Orchard House** (x 44..68, z −50..−26), 7 storeys, roof at 21 m:
  - The entrance (`rb_entrance`) is on the south face, across the north street (a zebra crossing).
  - Lobby, then its stair core (`rb0_`…`rb7_`; only the ground and roof doors open), then the roof hut.
  - The roof has a parapet, plant and a helipad at (60, 21, −38). The helicopter lands there (`rescue_heli`, 45 s) and stays.
- **Wrecks** (the damage layer, appearing day by day) narrow the route: by day 5 the bus blocks the direct crossing in front of the tower, so you go round by the junction. Collision-only brushes (`"render": false`) give them substance.
- **Zombies** wake as you come near. A `chase` action hands the story's idle zombies to the horde, which senses within 9 m (`"zombie_sense": 9`), chases and attacks:

| Group | Count | Woken when |
|---|---|---|
| corridor (`hall_`) | 5 | 10A's door opens, or you enter the corridor |
| stairwell (`stair_`, floors 7 / 4 / 2) | 3 | you enter the lower stairwell |
| ground lobby (`lobby_`) and street (`street_`) | 2 + 12 | you enter the ground lobby |
| Orchard House lobby (`rb_`) | 2 | you enter it |

- Dying reloads the last checkpoint: `street` once you reach the ground lobby, else `day_8`.

## 8. Onboarding prompts

Data-driven (`prompts` in the chapter); each shows once, retires when the action is done, and uses the player's bindings:
1. look, move (after waking)
2. interact (the morning)
3. lean: "Open a window, then {interact} to lean out" (after locking the door, until you have)
4. hold: "Hold {interact} to sleep" (the first night)
5. melee, sprint (the rescue)

## 9. Vista composition (locked reference frames)

Site plan: `docs/story/ch01/vista_site_plan.svg`. Bearings are clockwise from north, measured from our tower. The reference photos set the feel:
- A dense mixed-height city.
- A river in the near-middle distance with bridges.
- A dominant glass spire about 2 km out.
- A modern tower cluster on the left horizon.
- Brick and stone mid-rises close by.
- Parks and cranes.

| Element | Bearing | Distance | Height | Layer |
|---|---|---|---|---|
| River Vell (bends NE) | −90 → 88 | 470 m closest (due N) | | near/mid |
| Bridges | −10, 35 | 490 m, 560 m | | near |
| Park | 10 | 320 m | | near |
| Halden Tower (flickering lights) | 82 | 360 m | 95 m, 30 storeys | near (kit geometry, a few visible interiors) |
| Crane A / B | 14 / 61 | 0.9 / 1.4 km | 85 / 95 m | mid |
| St. Aldric Hospital (crash and smoke origin) | 47 | 1.75 km | 45 m | vista |
| **Meridian Needle** (glass spire) | 28 | 2.2 km | 310 m | vista |
| Financial cluster | −2 | 3.6 km | ≤230 m | vista |
| Modern towers E | 70 | 2.6 km | ≤150 m | vista |
| Stadium stub | 118 | 2.7 km | 40 m | vista |

**Locked frames.** The eye is 1.65 m above the floor (y = 31.65), at 16:9 with `cl_fov 60` (vertical; 91.5° horizontal; the engine's minimum). The tests diff these frames against golden captures (docs/STORY.md §Tests).

- **SOFA**: (9.4, 31.65, 3.6), yaw 25°, pitch −4°. Mullions at x 9.0 and 10.5 frame the spire. The river cuts across the lower-left third, the financial cluster sits at the left edge, and the smoke column (after beat 5) is on the right third. Sketch: `vista_sofa.svg`.
- **BALCONY**: (12.9, 31.65, −0.9), yaw 35°, pitch −6°. This matches reference photo 2: river and bridge at the left foreground, spire just left of centre, mid-rise brick blocks filling the lower half, and cranes. Sketch: `vista_balcony.svg`.
- Each frame is captured at 4 states: dawn 06:05, noon, the `cloudy+fog` haze, and night (22:30, window lights). See `tools/tests/vista_frames.cfg`.

Life in the view (docs/STORY.md "Vista"): traffic, birds, helicopters, smoke columns, fires, window lights by time of day and district power, crowds of the dead in the streets, and the **damage layer** (wrecks, scorched floors, rubble) all come from the vista sequences the chapter starts, one `day_N` per morning (§3). The streets the player walks on day 8 are real geometry (§7); their wrecks are drawn by the damage layer and collide.
