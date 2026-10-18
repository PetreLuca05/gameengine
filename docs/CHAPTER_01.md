# Chapter 1: "10th Floor"

**Status: design, awaiting approval.** Nothing here is built yet. Framework: docs/STORY.md. Kit: docs/BUILDING_KIT.md.
Sketches (regenerate with `python tools/story/ch01_sketches.py`):
- `docs/story/ch01/layout_plan.svg`
- `docs/story/ch01/vista_site_plan.svg`
- `docs/story/ch01/vista_sofa.svg`
- `docs/story/ch01/vista_balcony.svg`

## 1. Premise

Spring, a weekday. You wake at 05:52 in unit **10A**, the north-east corner flat on the 10th floor of a 16-storey residential tower in **Vellmouth**, a fictional river city. The outbreak started overnight. The flat is intact and lived-in, and the city through the glass looks normal. Over about 20 minutes of play it stops looking normal.

The family is the Marlowes:

| Who | Where they are at 05:52 | What they left |
|---|---|---|
| **You** (parent; name and gender are never stated) | asleep on the living-room sofa, the TV on | |
| **Dana** (partner, ER nurse) | **at work**: called in to St. Aldric Hospital at 23:40 | Note on the kitchen counter. Voicemail at 02:14 ("Stay inside. Don't open the door for anyone. I love you."). |
| **Jules** (16, swimmer) | **left early**, at 05:30, for swim practice | Note on the fridge ("Swim 6, back 9, took the last yoghurt, sorry"). A text at 05:41 that never sent. |
| **Mia** (8) | **at school**: the class's overnight "sleepover at the museum" | Permission slip and calendar entry. A drawing taped to her door. |
| **Biscuit** (dog) | **missing** | Dog bowl knocked over. Leash still on its hook. Collar on the balcony. A thread for later chapters. |

The neighbour in **10B** is Ruth Okafor, retired and on her own. She knocks in beat 6.

Tone: domestic realism, with environmental storytelling over exposition. There is no combat until beat 7. The city is the second character: every beat is visible through the glass.

## 2. Beat sheet and scripted progression

The story timeline drives the clock (`time_scale 0`; the chapter sets the time of day every tick from a curve that is keyed to beats). This keeps it deterministic and scrubbable with `story_time`.

Each beat has a time window. The clock eases toward the window's end while the player is inside the beat, and holds there if they dawdle. A fast player is never stalled waiting for the clock.

| # | Beat | Clock | Weather | Through the glass | Teaches |
|---|---|---|---|---|---|
| 1 | **Wake up** on the sofa, facing the TV: the phone buzzes on the seat beside you. | 05:52–06:00 | clear | Pre-dawn blue. City lights on. One distant siren. | look, move |
| 2 | **Check the phone**: no signal, 3 missed calls, Messages (Dana, Jules), Gallery holds family photos. | 06:00–06:10 | clear | Sunrise on the east glass. The first helicopter crosses far north. | phone, interact |
| 3 | **Find the family's notes**: the three notes in any order, plus optional extras (calendar, voicemail, Jules's laptop). | 06:10–06:35 | clear → little_cloudy | Traffic thickens, then **stalls** on the bridge at 35°. Halden Tower (360 m east) **flickers**. | journal, interact |
| 4 | **Gather supplies**: kitchen (knife, food, water), bathroom (first-aid kit), closet (bat in Jules's room, backpack). | 06:20–06:50 (runs parallel to 3) | little_cloudy | Sirens layer in. Birds circle the park. | inventory, heal wheel, melee, throw wheel (a bottle) |
| 5 | **Power cut → breaker**: the flat goes dark. Utility room breaker panel (hold interaction + timed ring). Power returns for about 2 min: the TV emergency broadcast loops ("…remain indoors…"). Then the grid fails for good. | 06:50–07:05 | little_cloudy | **Set-piece:** the helicopter flies the river toward the spire, banks, and goes down behind St. Aldric. Flash, then the boom about 5 s later (1.75 km). Birds scatter. A smoke column rises. Sirens swell. Window lights go out district by district. | hold interact, timed ring |
| 6 | **The knock**: Ruth knocks at 10B's side of the corridor and then at your door. Her voice comes through the door. Peephole (hand-pose case). Choose: **Open** (she comes in, panicked, and leaves to fetch her pills) or **Barricade** (the hall cabinet pushed against the door). Both converge. | 07:05–07:15 | → `smoke_haze` (120 s blend) | The haze thickens. The sun dims to orange. Smoke drifts across the north view. | peephole, push/drag prop |
| 7 | **First zombie**: Ruth screams in the corridor. Either the infected tenant from 10D attacks her (Open), or the bangs on your door stop and the barricade gives (Barricade). One zombie, melee only. | 07:15–07:25 | smoke_haze | Fires in two vista districts. | melee, quick heal |
| 8 | **Leave**: the fire alarm starts, smoke comes from the lift shafts, and the stairwell is the only way out. Pack (optional last-chance items), then go out along the corridor to the **fire door → stairwell**. Descend one flight to the 9½ half-landing. **Chapter end.** Save. | 07:25–07:40 | smoke_haze | Through the corridor's east end window: the city burning. | |

**Proposed chapter end (decision for you):** in the **stairwell, going down**.

On the half-landing you look down the stair eye: torchlight and screams many floors below, a door banging. A rotor thuds overhead, a second helicopter that circles the roof.

Subtitle: *"The roof."* Fade. Chapter 2 starts with the choice of climbing to the roof (the helicopter) or going down (to Mia's museum).

This keeps the stairwell as the exit (a contained, cheap space) and sets up the roof without building it in Chapter 1. The roof door on 16 stays chained in Chapter 1.

**Alternative:** end at the roof (build the roof pieces now, and fit a helicopter-leaves-without-you beat). This costs one more level section and its own vista frame.

## 3. Objective graph

`→` means "unlocks". `[opt]` means optional. `[hid]` means hidden until discovered. `∥` means parallel.

```
wake ──► phone ─┬─► notes (group, 3 of 3) ───────────────────────────────┐
                │     ├ note_dana (kitchen counter)                      │
                │     ├ note_jules (fridge)                              │
                │     ├ note_mia   (calendar + slip, child room)         │
                │     ├ [opt] voicemail_dana   [opt] jules_laptop        │
                │     └ [hid] biscuit_collar (balcony)                   │
                └─► supplies (group, 3 required, ∥ notes) ───────────────┤
                      ├ first_aid (bathroom cabinet) → heal-wheel prompt │
                      ├ melee (knife: kitchen block | bat: teen room)    │
                      ├ water_food (kitchen/fridge, any 2)               │
                      └ [opt] backpack (coat closet) +6 slots            │
                                                                         ▼
                                       power_cut (timer 20 s after both groups, or story_time ≥ 06:50)
                                         └─► breaker (hold 2.5 s, timed ring) ─► broadcast (TV/radio, 120 s)
                                               └─► window_setpiece (auto; triggers when the player is in the LDK or a glass room;
                                                                     else when they next enter one; 60 s fallback)
                                                     └─► knock ─► peephole ─► decide { open | barricade }
                                                           └─► first_zombie (kill or wait at the barricade 90 s → it breaks)
                                                                 └─► leave: corridor → fire_door → stair_landing_9_5 (volume) ─► END + save
```

Soft-lock rules, checked by the bot and the validator in docs/STORY.md:
- Every required objective has at least 2 ways to complete it, or a fallback timer.
- Required items have spare copies: 2 first-aid kits, a knife and a bat.
- The front door cannot be locked from inside before beat 6.

Checkpoints:
- C1 after phone.
- C2 when both groups are done.
- C3 after the breaker.
- C4 after the decision.
- C5 at the end.

## 4. Apartment layout plan (unit 10A)

Plan: `docs/story/ch01/layout_plan.svg`.

**Coordinates are unit-local.** The origin is the north-west interior corner. +x is east, +z is south, y is up, and yaw 0 looks along −Z (north), the engine convention. The floor top is at **y = 30.0**: ground floor 0, floor-to-floor 3.0, 10th floor. The unit sits in the tower's north-east corner.

- **Footprint:** 12 × 12 m minus the 3 × 5 m notch that belongs to 10B. That is **129 m² gross** (about 118 m² net of walls).
- **Ceiling:** 2.7 m.
- **Glass:** the **North (z = 0) and East (x = 12) walls are full-height, full-width glass**, each 8 modules of 1.5 m. A **corner mullion** sits at (12, 0).
- **Plan rule:** every partition that meets the glass lands on a mullion. That puts partitions at x = 4.5, z = 6.0 and z = 9.0.
- **Sliding door:** the north module x 10.5..12.0 opens onto a **1.2 m wraparound balcony** (north x 4.5..13.2, east z −1.2..6.0) with a glass balustrade 1.1 m high.

| Room | x | z | m² | Glass | Key props / interactables |
|---|---|---|---|---|---|
| Living | 7.5–12 | 0–6 (open) | 45 (open plan) | N + E (hero corner) | L-sofa facing the NE corner, chaise along the east glass. TV on a low credenza on the south partition (switchable, broadcasts). Reading chair + lamp at the NW. Rug, plants, toys, dog bed. Sliding door. Blinds on every module. |
| Dining | 4.5–7.5 | 0–3 | (in 45) | N | Table for 4, half-finished breakfast (Jules's bowl, Dana's mug from last night), fruit bowl. |
| Kitchen | 4.5–9.6 | 3–6 | (in 45) | | 5.1 m run on the south partition (tall fridge, oven, hob, sink, dishwasher, 6 drawers, 8 cabinets). Island with 2 stools and 3 drawers. Knife block, kettle, radio, calendar (Mia's trip). Notes on the fridge and the counter. Taps (water). |
| Master bedroom | 0–4.5 | 0–4.5 | 20.3 | N | Bed, 2 nightstands (lamp), dresser (4 drawers), curtains, family photos, Dana's work lanyard (missing). |
| En-suite | 0–2.4 | 4.5–7 | 6.0 | | Shower, WC, vanity + mirror cabinet (first-aid kit #1), towel rail, taps. |
| Walk-in closet | 2.4–4.5 | 4.5–7 | 5.3 | | Walk-through from the hall to the master. Rails, shelves, shoe rack, safe (code is on a note in the study, optional). |
| Hall | 4.5–6.0 | 6–12 | 9.0 | | Entrance (security door, peephole, chain, deadbolt), key bowl, mirror, shoe bench, leash hook, intercom (dead). Hall cabinet (the barricade prop). |
| Coats | 3.0–4.5 | 10.5–12 | 2.3 | | Bi-fold doors: coats, backpack, umbrella, vacuum. |
| Utility | 3.0–4.5 | 7–10.5 | 5.3 | | Stacked washer/dryer, **breaker panel** (beat 5), water heater, mop/bucket, toolbox (screwdriver, tape), torch. |
| Study nook | 6.0–8.4 | 6–7.2 | 2.9 | | Desk, PC (dead), printer, router (blinking amber), shelf, safe-code note. |
| Family bath | 6.0–8.4 | 7.2–9.6 | 5.8 | | Tub + shower, WC, vanity, cabinet (first-aid kit #2, painkillers), laundry basket. |
| Side hall | 6.0–8.4 | 9.6–10.6 | 2.4 | | Child-height hooks, framed school photos. |
| Storage | 6.0–8.4 | 10.6–12 | 3.4 | | Shelving: canned food, water bottles, camping gear (lantern), suitcases. |
| Teen room (Jules) | 8.4–12 | 6–9 | 10.8 | E | Bed, desk + laptop (optional note), swim trophies, **bat**, posters, wardrobe, curtains, gaming chair. |
| Child room (Mia) | 8.4–12 | 9–12 | 10.8 | E | Bunk/loft bed, toy chest, drawings, night light (emissive), school-trip permission slip, plush dog. |

**Outside the unit, the 10th-floor shared corridor** (z 12.3..14.1, x −8.5..12) contains:
- The doors of **10B** (Ruth, knocks, openable later), **10C** and **10D** (source of the zombie in the Open branch).
- The **elevator lobby** (2 lifts, dead: call panel interactable "No power").
- A riser access panel.
- An **east end window**.
- At the west end, the **fire door → stairwell**. The stair core runs x −12..−8.5 as a dog-leg with half-landings, built for floors 9–11 only. Below that is a dressed stair eye with impostor depth.

Lighting:
- Ceiling downlights per room (switch groups), pendants over the island and the dining table, lamps, under-cabinet strips.
- Emissive: TV, router, night light, oven clock (all follow the power state).
- One reflection probe per room (the LDK gets 2). The probes re-bake on power-state change.

## 5. Vista composition (locked reference frames)

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
- **Start**: the chapter opens at `spawn_sofa` (10.3, 30, 3.8), in front of the L-sofa seat, facing the TV (yaw −162.7). The phone (`phone_sofa`) buzzes on the seat.

Life in the view (vista layer, docs/STORY.md §Vista):
- **Traffic** on spline roads: headlight/taillight sprites and low-poly cars. Density follows the clock. Traffic **stalls** after beat 3 and some cars go dark.
- **Birds**: flocks over the park and the river, scattering at the crash.
- **Helicopters**: H1 at beat 2 crossing far north; H2 is the crash at beat 5; H3 is the end hook.
- **Smoke columns**: St. Aldric (beat 5), plus 2 more in beat 7.
- **Window lights**: by time of day and district power state. Districts black out in sequence in beat 5. Halden Tower flickers from beat 3.
- **Audio**: sirens (spatialised at district anchors), city hum, and a wind bed. Opening the sliding door raises the outdoor bus and wind.
- **Weather**: blends over the city, and the sun path crosses the east glass at dawn.

## 6. Props list (placeholder-but-correct, generated, kit-conformant)

Every prop is a kit "furniture" piece (docs/BUILDING_KIT.md §Furniture) with sockets for its interactables.

- **Totals:** about 140 placed props, 62 interactables (doors 14, drawers/cabinets 31, appliances 7, switches 9, blinds/curtains 18 modules in 6 groups, panels 3).
- **Storytelling set:**
  - Breakfast (bowl, spoon, mug, cereal box, juice).
  - Family photos ×9, generated placeholders: the family at the beach, Mia's school photo, Jules with a medal, the dog.
  - Calendar (with the trip marked), school bag (Mia's, left behind: a sleepover kit list), toys ×12 (blocks, plush dog, crayons), swim bag hook (empty), dog bowl + bed + toy, the leash on its hook, the collar on the balcony.
  - Dana's spare scrubs on a chair, a hospital rota on the fridge.
  - Coffee machine (works when powered), radio (scripted broadcast), TV (scripted broadcast + static), laptop (Jules: the optional unsent message draft).
- **Loot tables** (chapter-scoped, data/loot.json tier `ch01_*`) are fixed, not random. The bot test depends on them, and replays must be identical.

## 7. Onboarding prompts

The prompts are data-driven in `assets/story/chapters/01_tenth_floor.json` `prompts`:
- Each shows once and can be dismissed (`Esc`/`B`).
- Each is suppressed if the action has already been done.
- Each respects the key rebinds (the glyph comes from the input map).

Order:
1. look
2. move
3. interact (nightstand phone)
4. phone (hold to raise)
5. journal (after the first note)
6. inventory (first pickup)
7. hotbar
8. heal wheel (first-aid kit)
9. melee (knife/bat equipped)
10. throw wheel (bottle)
11. hold-interact + timed ring (breaker)
12. peephole
13. push prop (barricade)
14. sprint/crouch (corridor)

Phone pairing is offered once and can be skipped.
