# Zombieland: the 2 km open-world map

`map zombieland` loads a 2 km × 2 km zombie-survival world built from
[docs/reference/zombie_map/SPEC.md](reference/zombie_map/SPEC.md). Nothing is hand-modelled. A data file
describes the world (`data/levels/zombieland.json`: terrain regions, sea, river, roads, ten points of
interest). The generator turns it into terrain, roads, bridges, buildings, props, trees, loot and spawns
at load time. This takes about 1 s, plus about 1.6 s for the procedural foliage meshes and impostors on
the first map load of a session.

| Piece | Code |
|---|---|
| Terrain, water, roads, bridges, raster (pure) | `src/world/mapgen.c/.h` |
| POI planner + building / prop kit (pure) | `src/world/mapgen_poi.c/.h` |
| Loot tables (pure) | `src/gameplay/loot.c/.h`, `data/loot.json` |
| Game glue: collision, meshes, water, entities, rules | `src/world/openworld.c/.h`, `openworld_internal.h` |
| Trees, grass, crops | `src/world/openworld_foliage.c`, foliage kinds in `src/render/vgeo/vgeo_procgen_foliage.c` |
| Streaming (custom bounded provider, far tiles) | `src/world/chunks.c` (`chunks_create_custom`) |
| Zombie hooks | `src/gameplay/zombies/horde.c` (`horde_spawn_at`, `horde_set_water`, `horde_set_sense`) |
| Tools / tests | `tools/mapgen_preview`, `tests/test_mapgen.c` (ctest `mapgen`), `tools/tests/zl_*.cfg` |

## Vegetation showcase

`map vegetation` (`data/levels/vegetation.json`) is a 500 m valley that uses the same generator to show
every foliage kind: a pine forest (NW), a broadleaf forest (NE), a dead forest (W), a mixed forest (E),
a meadow of grass tufts and bushes in the middle, a wheat field (south) and a pond. No zombies, no POIs.

## Coordinates

The map uses the engine compass from `time_of_day.h`:
- X is east and Y is up.
- Z is south, so north is −Z.
- Yaw 0 faces north, 180 south, −90 east and 90 west.

The spec table gives positions as (X east, Z north). Such a position maps to engine `(x, −z)`. The sun
rises in the east, so the in-game compass, the sun and the reference layout agree.

## Layout

The world is centred at the origin. A raster covers ±1024 m and the playable area is ±1000 m.

| Location | Spec (X, Z north) | Engine (x, z) | What is there |
|---|---|---|---|
| Main City | (0, 100) | (0, −100) | 6 × 5 street grid (76 × 70 m blocks, a 13 m avenue), perimeter blocks of 2–6 storey apartments / offices / shops (avenue storefronts with awnings), clinic, police station (locked armory, `key_red`), supermarket, fuel station (pumps, a drivable car), parking lot, park, street lamps, traffic signals, crosswalks, wrecks, barricades. |
| Suburbs | (−550, 100) | (−550, −100) | 4 curved streets and 3 lanes, about 100 detached houses (garages, porches, chimneys, mailboxes, back-yard fences, sheds, driveways), a playground, street lamps, wrecks. **Player spawns.** |
| Mountain Camp | (−700, 650) | (−700, −640) | A dirt pad on the mountainside with 6 cabins, tents, a campfire (light) and sheds. A switchback dirt road leads up from the suburbs. A 34 m radio tower stands on the ridge at (−765, −735) with a radio hut. |
| Lake | (−100, 600) | (−100, −600) | 175 × 112 m lake (water level 12.5 m, 7 m deep), a dock, a moored and a beached boat, a fishing cabin, a shed, a campsite. The river leaves it to the south-east. |
| Factory | (600, 650) | (600, −650) | A fenced concrete yard with a checkpoint at the west gate, 2 production halls (mezzanines, stairs), 2 warehouses, a 3-storey admin block (roof access), a workshop, tanks, 2 smokestacks, a pipe rack, containers, yard clutter (pallets, crates, forklifts, trucks, scrap). |
| Farms | (600, 0) | (600, 0) | 95 × 70 m crop parcels (wheat, green crop, ploughed; wheat foliage on the fields) between dirt tracks; 3 farmsteads (farmhouse, barn with a car or a tractor, silos, shed, water tower, hay, fences, a vegetable plot). |
| Airfield | (−650, −550) | (−650, 500) | A 580 × 40 m runway with markings, a taxiway and links, an apron, 3 hangars (a small plane, a car), a terminal (roof access), a control tower, a fuel depot, a locked maintenance store (`key_blue`), a plane wreck, a perimeter fence with a gate and a checkpoint. |
| Forest | (−100, −600) | (−100, 600) | Dense mixed woodland (about 3 trees / 100 m², clearings, underbrush), 3 cabins, 5 hunting stands, 2 improvised camps, fallen logs, boulders, supply caches, trails to the city, airfield, suburbs and quarry. |
| Port | (600, −650) | (620, 700) | A concrete quay at 3 m above the sea with a quay wall, 3 piers (bollards, lamps), 2 gantry and 2 jib cranes, 2 container yards (stacks up to 3, some open with loot), 3 warehouses, a harbor office (locked control room, `key_blue`), a fuel depot, a moored cargo ship (gangway, enterable bridge with a locked room, deck containers), a boat, a land-side fence with a gate. |
| Quarry | (−700, −850) | (−700, 822) | A 6-terrace pit, 30 m deep, with a spiral ramp. Excavator, dump trucks, a drill rig and gravel piles. A hidden bunker at the bottom. A fenced processing facility at the rim: plant hall with a conveyor and hopper, workshop, shed. |

**Roads.** All roads are `MGR_*` classes, profiled and flattened into the terrain:
- Main roads link the city to the farms (Route 1), the city to the factory (Route 2), the farms to the port
  (Route 3) and the farms to the factory (Route 4).
- Secondary roads go to the suburbs and the airfield, plus a coast road from the city to the port.
- Dirt tracks lead to the camp (switchbacks), the radio ridge, the lake, the quarry, the field tracks and
  the factory back road.
- Trails run through the forest.

**River and edges.**
- The river runs from the lake to the sea west of the port. The three crossings get bridges: Route 1,
  Route 2 and the coast road.
- The map edge is mountains in the west and north, hills in the north-east, and sea to the south and
  south-east. Invisible walls at ±1000 m back them up.

`mapgen_preview` draws the generated map top-down:

```
build/bin/mapgen_preview.exe data/levels/zombieland.json out.png 2   # 2 m per pixel, north up
```

## Data format (`data/levels/<map>.json`)

Any level with a `"world"` block is an open-world level. The `level_load` → `openworld_load` chain
handles it, and `env`, `weather` and `name` work as in other levels.

```jsonc
"world": {
  "seed": 1337, "half_size": 1000, "raster_half": 1024,       // playable / generated half extents (m)
  "terrain": { "base": 14, "hill_amp": 6, "hill_freq": 0.0022 }, // noise hills around the base height
  "sea": { "floor": -16, "beach": 55, "noise": 16, "polygon": [[x, z], ...] },   // closed polygon of sea
  "features": [ ... ],      // applied in kind order: relief, sea, lakes, pits, pads; paint / forest anywhere
  "rivers": [ { "points": [[x, z], ...], "width": 14, "depth": 2.6, "bank": 9, "start_level": 12.5, "end_level": 0 } ],
  "roads":  [ { "class": "main", "name": "...", "points": [[x, z], ...], "width": 9, "smooth": true, "loop": false } ],
  "zombies": { "wild_density": 0.5, "wild_max": 4, "cap": 120, "despawn": 190, "spawn_min": 40, "spawn_max": 95, "sense": 38 }
},
"pois": [ { "id": "city", "name": "Main City", "kind": "city", "pos": [x, z], "size": [w, d], "yaw": 0,
            "loot": "household", "ambience": "urban", "zombies": { "density": 6, "max": 26, "indoor": 0.45 }, ...kind fields } ]
```

**Features.** Kind-specific fields:

| type | fields | effect |
|---|---|---|
| `mountain` | `pos`, `radius` (r or [rx, rz]), `height`, `ridge` 0..1 | smooth bump with ridged-noise relief |
| `hill` | `pos`, `radius`, `height` (may be negative: a basin) | smooth bump |
| `rim` | `edges` ("WNES"), `height`, `width` (inwards from the edge), `range` [min, max] along the edge | boundary mountains |
| `lake` | `pos`, `radius`, `yaw`, `level`, `depth`, `shore_noise` | basin + shore, water surface at `level` |
| `pit` | `pos`, `radius`, `yaw`, `depth`, `terraces` | stepped excavation (rock risers, gravel benches) |
| `pad` | `pos`, `size` [w, d], `yaw`, `level` (number or omitted = mean of the terrain), `blend`, `material` | flat building ground |
| `paint` | `pos`, `size`, `yaw`, `material` | surface material only |
| `forest` | `pos`, `radius`, `density` (trees / 100 m²), `pine` (fraction of the living trees), `dead` (fraction, default 0.035) | tree scatter + forest floor |

**Materials:** `grass`, `meadow`, `forest`, `dirt`, `rock`, `sand`, `gravel`, `asphalt`, `dirt_road`,
`concrete`, `field`, `crop`, `plowed`, `mud`. Slopes over 0.85 become rock and slopes over 0.55 become
dirt.

**Road classes.** Each class sets the default width, the shoulder blend, the maximum grade and the profile
smoothing:

| class | width | shoulder | max grade | smoothing |
|---|---|---|---|---|
| `main` | 9 m | 10 m | 8 % | ±24 m |
| `secondary` | 7.5 m | 9 m | 10 % | ±18 m |
| `street` | 8 m | 4 m | 12 % | ±8 m (none when on a pad) |
| `dirt` | 4.5 m | 6 m | 16 % | ±10 m |
| `trail` | 2.5 m | 3 m | 30 % | ±4 m |
| `runway` | 40 m | 25 m | 1 % | ±60 m |

How profiles are built:
- The profile is the pre-road terrain sampled every 2 m, averaged, then grade-limited.
- A road's ends snap to an earlier road of a higher class (junctions) and blend into it over 30 m.
- Where a road crosses a river, a deck goes 2.6 m above the water and ramps of 24 m lead up to it. The
  river carve runs under the deck.

**POI kinds and their fields:**

| kind | fields |
|---|---|
| `city` | `block_x`, `block_z`, `special_blocks` {clinic, police, supermarket, fuel, parking, park: [bx, bz]} |
| `suburbs` | `streets` |
| `factory` | `gate` |
| `farms` | `farmsteads` [[x, z], ...], `parcel_x`, `parcel_z` |
| `airfield` | `runway_a`, `runway_b`, `apron_side` (±1: left / right of a→b), `gate` |
| `port` | `level` (quay height), `gate`, `yaw` (the sea on local −Z) |
| `quarry` | `pit`, `pit_radius`, `facility`, `ramp_angle`, `gate` |
| `lake` | `dock`, `dock_yaw` (local +Z into the water), `campsite` |
| `camp` | `tower`, `pad_blend`, `yaw` |
| `forest` | `cabins` [[x, z], ...] |
| `region` | none: a density / loot region without generated content |

## Pipeline

All generation is deterministic for a description. Client and host build the same world.

1. **Parse** (`mapgen_parse`) the world block.
2. **Plan** (`mappoi_plan`). This adds streets, pads, painted surfaces and forests to the generator, and
   building lots plus keep-outs (vegetation-free footprints) to the POI list.
3. **Finalize** (`mapgen_finalize`). This computes:
   - spatial grids: features, roads, rivers, keep-outs;
   - pad auto levels;
   - river water profiles;
   - road samples, profiles, junctions and bridges.
4. **Raster.** A 1 m height + material raster (2049², about 21 MB), in parallel rows on the job system, then
   a slope pass.
5. **Build** (`mappoi_build`). Lots become primitives and entity requests:
   - primitives are boxes, tapered cylinders and wedges, each with a colour, surface and flags;
   - entity requests are containers, pickups, doors, lights, vehicles, pumps, zombie spots and spawns.

   Markings, bridges, city / suburb street furniture and roadside encounters are built here too.
6. **Glue** (`openworld_load`):
   - **Collision:** 32 × 32 static heightfield tiles (64 m, 1 m samples) and 4 invisible edge walls.
   - **Geometry:** primitives batched into one mesh per 64 m tile and material variant (plain, gloss,
     metal, emissive, glass, unshadowed; detail variants culled beyond 260 m). Road ribbons go on top.
     Collidable primitives become one triangle-mesh body per tile and surface (impact effects / sounds
     keep their material).
   - **Water:** a sea plane, lake discs and river ribbons (translucent, unshadowed).
   - **Entities** (host): containers rolled from `data/loot.json`, doors (locked ones with their key),
     lamps (dusk-to-dawn schedule), pickups. `game_vehicles.c` spawns the vehicles and pumps after its
     library loads.
   - **Foliage:** trees and underbrush for the whole map. Trunk capsules become one static mesh per tile.
   - **Terrain streaming:** `chunks_create_custom` with the raster as provider. Visual chunks (32 m, 4
     LODs) stream in whole 128 m far tiles around the players. Far tiles (4 m / 16 m) draw wherever their
     chunks are not all loaded. Grass and crops come with the chunks.

## Gameplay

**Zombies.** `openworld_tick` runs on the host. Every second it does the following for each player:
- It looks up the POI region around the player. Outside every POI the `wild_*` values apply.
- It aims for `min(max, density × 3.8)` zombies within 110 m.
- It spawns up to 3 per second. A share `indoor` of them goes to the POI's zombie spots (floors of
  enterable buildings, containers, hangars) through `horde_spawn_at`. The rest go on a ring 40–95 m
  around the player through `horde_spawn`, never in water.
- It culls zombies more than 190 m from every player.

The total is capped at 120. Densities follow the spec: city 6.0, factory 4.5, airfield / port 4.0,
suburbs 2.0, farms 1.5, forest / camp / quarry 1.2, lake 0.8, wilderness 0.5.

The horde normally chases the nearest player at any distance. On this map a **sense radius** (38 m)
keeps zombies idle until one of these happens:
- a player comes within the radius;
- a player fires within 3 × the radius;
- the zombie is hurt.

Idle zombies wander: a new point within 8 m every 6 s, and a third of the time they just stand. A chasing
zombie gives up beyond 2.5 × the radius. Deep water (more than 1 m) stops zombies like a ledge.

Outdoor spawns skip building footprints, so a zombie never lands on a roof.

**Loot.** `data/loot.json` defines the tiers, as weighted items with count ranges, item rolls and an empty
chance:

| Tier | Where |
|---|---|
| household | houses, shops |
| garage | garages, sheds, fuel station |
| food | supermarket, shops |
| office | offices, admin |
| medical | clinic |
| police | police, checkpoints |
| armory | locked police room |
| industrial | factory, quarry, yards |
| rare | airfield, port |
| military | locked stores |
| farm | farms |
| camping | camp, forest |
| fishing | lake |

New items: painkillers, canned food, water bottle, cloth, duct tape, mechanical parts, seeds and fishing
kit. Food and water heal a little; there is no hunger system. Keys: the red key (police containers) opens
the police armory. The blue key (rare containers) opens the harbor control room, the ship's bridge room
and the airfield store.

**Water and the edge.** A player whose feet are more than 1.3 m below a water surface (sea, lake, river) is
put back on the last dry ground with the hint "Too deep - you can't swim". Bridges and the dock cross the
water. The ±1000 m walls are a backstop behind the mountains and the sea.

**Spawns.** 7 spawn points lie on the suburb streets.

**Vehicles.** Five drivable cars: the fuel station, two barns, a hangar and the city. There are also fuel
pumps. Everything else on the roads is static wreck geometry.

## Commands

| | |
|---|---|
| `map zombieland` | load the map |
| `poi_list` | the POIs with their density, loot tier and enterable buildings |
| `poi_goto <id> [height]` | teleport to a POI centre |
| `ow_where` | ground, water level, material and POI at the player |
| `ow_stats` | zombies (chasing / spawned / culled), chunks, draws, bodies, foliage, frame time |
| `ow_zombie_list` | the zombies with positions and distances (debug) |
| `ow_zombies 0/1` | the POI zombie spawner (cvar) |

## Tests

**`ctest -R mapgen`** (`tests/test_mapgen.c`, 3700+ checks) covers:
- determinism;
- roads on their profile and within the grade;
- bridges over the river with decks above the water;
- the river flowing downhill;
- sea, lake, river and dry-land water levels;
- mountain and sea edges;
- the 10 POIs within 80 m of the spec positions, each with ≥ 2 enterable buildings;
- the density order of the spec;
- no building on a carriageway;
- no trees on roads or inside footprints;
- spawns in the suburbs on dry land;
- containers, zombie spots, locked doors and vehicles present;
- loot items exist, every container tier exists, rolls are deterministic.

The script tests run from `build/bin`:
`deadground.exe --test --nosound --log <log> --script ../../tools/tests/<cfg>`.
- `zl_tour.cfg`, generated by `gen_zl_tour.py`: 19 screenshots, as overview, aerial and ground shots of
  every location and a bridge. It uses `cl_camera_socket 0` because `setpos` while flying does not move
  the head-socket camera.
- `zl_gameplay.cfg`: the clinic outside and inside, 15 s in the city with the spawner on (`ow_stats`,
  `horde_stats`), walking into the lake, the west edge, the police station.
- `zl_perf.cfg`: `post_bench` uncapped at:
  - street level in the city and the suburbs;
  - in the forest;
  - over the city, from the overview and from the mountain;
  - in the city with the spawner on.
- `zl_perf_night.cfg`: the city and the fuel station at night (lamps).
- `zl_zombies.cfg`: lets the spawner fill the city, looks down from 30 m, fires a shot (noise aggro), and
  spawns a ring of zombies on a suburb street.
- `zl_net.sh`: a host and a client on zombieland. The client builds the same world, spawns in the suburbs
  and sees the host's zombies. The port is `ZL_PORT` (27031) and the build dir is `DG_BUILD`.

Set `DG_WATCHDOG=<seconds>` for scripted runs. If no frame finishes for that long, the main thread's stack
goes to the log (`src/core/crash.c`).

## Performance (RTX 3060 Ti, 1600 × 900, `zl_perf.cfg` / `zl_perf_night.cfg`)

| Spot | Frame-graph GPU passes | Frame (smoothed) |
|---|---|---|
| city street, suburbs, forest, over the city, overview, mountain | 2.8–3.3 ms | 4.3–4.5 ms (about 230 fps) |
| city with the spawner on (12 zombies) | 2.8 ms | 4.3 ms |
| city at night | 2.9 ms | 6.1 ms |
| fuel station at night (its canopy lamp casts shadows) | 3.2 ms | 9.4 ms |

The frame-graph figures include about 0.2–0.5 ms of vgeo foliage raster. The whole uncapped day script
averaged 6.5 ms per frame, loads included.

What the map holds:
- **Static geometry:** about 500k triangles in 1359 draws (64 m tiles × material). The renderer
  frustum-culls them per tile and detail tiles are distance-culled. Small tiles also keep the sun-cascade
  and local-light caster lists short.
- **Collision:** 2036 static bodies.
- **Trees and bushes:** 16.6k. They LOD, fade (850–1100 m) and switch to impostors on the vgeo path.
- **Grass and crops:** on the streamed chunks, fading at 65 / 95 m.
- **Terrain:** about 160 visual chunks and 256 far tiles.

**Lamps.** Most lamps cast no local shadow: street, pier, bridge and checkpoint lamps carry
`LIGHT_GROUP_NO_SHADOW` (`world.h`, replicated with the light group). Campfires and the fuel canopy do
cast. With every lamp shadowed, the night city took 36 ms a frame.

**Load.** The generator, raster and uploads take about 1 s. Building the 6 procedural foliage kinds
takes about 1.6 s: cluster builds and impostor bakes run in parallel, and the tree kinds use 512 px
atlases. A client therefore joins well inside the 4 s ENet timeout.

## What is placeholder

- **Art.** Buildings, props and vehicles are the primitive kit: flat-coloured boxes, cylinders and
  wedges. Windows are panes or open holes. There are no textures and no decals beyond road markings.
- **Interiors.** They are simple: walls with door / window openings, one partition per storey, straight
  stair flights in a stairwell and roof hatches. There is no furniture apart from shelves, racks,
  containers and vehicles.
- **Zombies.** They have no navmesh (the horde steers in straight lines with wall avoidance) and no
  audio-driven aggro: noise does not attract them yet.
- **Water.** No swimming and no usable boats. The water surfaces are flat translucent planes: no waves,
  reflections or underwater effects.
- **Quests.** Not implemented: the radio tower, the locked ship, the harbor control room and the bunker
  are just places with loot. The `ambience` fields are informative; there are no per-region ambient
  sounds yet.
- **Unused assets.** The tree assets in `assets/trees` (TreeIt `.tre`, CC BY) are not used: the format is
  not loadable. The trees are procedural vgeo foliage instead.
- **Static wrecks.** Abandoned cars on roads cannot be driven, searched or moved.
- **Indoor light.** Interiors get the sky ambient through the open floor plan (no indoor occlusion beyond
  GTAO), so floors read bluish. They are not much darker than outside, except at night.
