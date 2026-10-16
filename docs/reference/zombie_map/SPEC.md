# Zombie Survival Map — Implementation Specification

## Purpose

Use this document as the implementation brief for Claude Code. The goal is to create a playable 2 km × 2 km zombie-survival world based on the supplied top-down map reference.

**Important:** This document describes the intended world; it is not a terrain file, 3D model, or ready-made game level. Claude Code should inspect the existing project and implement the map using the project's actual engine and architecture.

## 1. World scale and coordinate system

- Playable world dimensions: **2,000 m × 2,000 m**.
- Suggested world center: `(0, 0, 0)`.
- Suggested horizontal axes: positive X = east, positive Z = north; Y = elevation.
- North is the top of the reference image.
- Keep all playable terrain within the world boundary.
- Treat the positions below as approximate landmark centers, not exact building coordinates. Adjust them to fit the reference and ensure the world remains inside the boundary.

| Location | Approximate center (X, Z) | Identity |
|---|---:|---|
| Mountain Camp | (-700, 650) | Rocky mountains, cabins, winding trails |
| Lake | (-100, 600) | Freshwater, irregular shoreline, dock |
| Factory | (600, 650) | Industrial complex, warehouses, tanks |
| Suburbs | (-550, 100) | Residential streets and detached houses |
| Main City | (0, 100) | Dense urban center |
| Farms | (600, 0) | Crop fields, barns, farm equipment |
| Airfield | (-650, -550) | Runway, taxiways, hangars |
| Forest | (-100, -600) | Dense woodland and trails |
| Port | (600, -650) | Coastal docks, containers, warehouses |
| Quarry | (-700, -850) | Excavation pit and heavy machinery |

## 2. Locations and points of interest

### 2.1 Main City

**Role:** Primary high-risk exploration and looting zone.

**Environment**
- Dense grid of paved roads, larger intersections, side streets, sidewalks, and parking areas.
- Mix of four- to six-story apartment blocks and two- to three-story commercial buildings.
- Shopping street with shops, restaurants, offices, and abandoned vehicles.
- Include a small hospital or clinic, police station, supermarket, and fuel station.
- Add streetlights, traffic signals, faded road markings, broken windows, burned cars, barricades, and debris.

**Gameplay**
- Highest average zombie density.
- Enterable buildings with interiors, staircases, and selected rooftop access.
- Medical loot in the clinic; ammunition and security gear at the police station; food in supermarkets.
- Some rooms should be locked and require keys or tools.
- Narrow streets and interiors should create dangerous close-range encounters.

**Atmosphere:** Claustrophobic urban decay, distant alarms, wind through empty streets, and occasional sounds from inside buildings.

### 2.2 Suburbs

**Role:** Lower-intensity scavenging area and possible starting region.

**Environment**
- Detached houses, driveways, garages, gardens, fences, and backyards.
- Curved residential roads rather than a strict city grid.
- Small parks, playgrounds, mailboxes, and neighborhood parking areas.
- Overgrown hedges, damaged fences, boarded windows, abandoned cars, sheds, and basements.

**Gameplay**
- Low-to-medium zombie density, with occasional groups inside houses.
- Common loot: canned food, water, clothing, basic tools, and household supplies.
- Garages can contain fuel cans, repair kits, and melee weapons.
- Some houses can be used as temporary safehouses.
- Place hidden supplies in basements, sheds, and attics.

**Atmosphere:** Quiet residential decay, wind through trees, creaking structures, and distant animal sounds.

### 2.3 Mountain Camp

**Role:** Remote location with natural defenses and survival equipment.

**Environment**
- Rocky terrain, steep elevation changes, exposed stone, and sparse vegetation.
- Winding dirt roads and switchbacks climbing the mountainside.
- Wooden cabins, tents, storage sheds, and a central campfire.
- Radio antenna or communications tower on a ridge.
- Cliffs and narrow paths that constrain access.

**Gameplay**
- Low zombie density, with occasional infected hikers or camp residents.
- Loot: camping equipment, food, medicine, hunting gear, and ammunition.
- Radio tower can be a quest objective or map-reveal point.
- High elevations should provide useful views of nearby regions.
- Difficult access creates a trade-off between safety and travel time.

**Atmosphere:** Wind, fog, pine trees, distant birds, and creaking cabins.

### 2.4 Lake

**Role:** Natural navigation landmark and possible survival-resource location.

**Environment**
- Irregular freshwater shoreline with rocky, grassy, and sandy sections.
- Small wooden dock and one or two abandoned boats.
- Fishing cabin, storage shed, or lakeside campsite.
- Dense trees around some sections and open views along others.
- Optional streams or drainage channels that fit the terrain.

**Gameplay**
- Water blocks walking unless a bridge, boat, or designated crossing is provided.
- If water collection exists, require purification before drinking.
- Fishing gear and supplies near the dock.
- A boat can be a quest reward or limited-use transport.
- Place occasional loot containers and clues along the shoreline.

**Atmosphere:** Calm water, insects, birds, light fog, and unsettling sounds from the woods.

### 2.5 Factory

**Role:** Dangerous industrial loot zone with environmental hazards.

**Environment**
- Large production halls, metal warehouses, loading bays, and concrete yards.
- Smokestacks, storage tanks, ventilation equipment, and visible pipes.
- Pallets, scrap metal, forklifts, parked trucks, and machinery.
- Fenced perimeter with gates and a security checkpoint.
- Administration building, maintenance workshop, and utility structures.

**Gameplay**
- Medium-to-high zombie density, especially inside buildings.
- Loot: industrial materials, fuel, tools, protective clothing, and mechanical components.
- Generator or electrical control room can power certain locked doors.
- Optional chemical spills, broken machinery, and unstable structures create localized hazards.
- Provide multiple routes for stealth or direct confrontation.

**Atmosphere:** Metallic echoes, machinery creaks, dripping water, and deep interior darkness.

### 2.6 Farms

**Role:** Resource-rich rural region with exposed movement and long sightlines.

**Environment**
- Rectangular crop fields divided by dirt tracks and drainage ditches.
- Farmhouse, barns, sheds, worker cottages, grain silos, and water tanks.
- Tractors, trailers, abandoned farm equipment, fences, hedgerows, and scattered trees.
- Small vegetable gardens and varied field colors and crop heights.

**Gameplay**
- Low-to-medium zombie density in open fields; higher around farm buildings.
- Loot: food, seeds, agricultural tools, fuel, and crafting materials.
- Barns can contain large loot containers or vehicles.
- Tall crops can obscure zombies and players.
- Side quests may involve missing survivors or supply caches.

**Atmosphere:** Wind through crops, creaking machinery, distant crows, and open-field silence.

### 2.7 Airfield

**Role:** High-risk transportation site with open terrain and valuable supplies.

**Environment**
- Long asphalt runway with faded centerlines, threshold markings, and worn edges.
- Taxiways, service roads, hangars, a small terminal, and maintenance buildings.
- Fuel storage, service vehicles, equipment carts, aircraft wreckage, and perimeter fencing.
- A few damaged fence sections and controlled entry gates.

**Gameplay**
- Medium-to-high zombie density near the terminal and hangars.
- Loot: aviation fuel, tools, medical supplies, protective gear, and rare equipment.
- Runway provides long sightlines but little cover.
- Hangars offer enclosed exploration and valuable loot.
- A crashed aircraft or locked maintenance facility can anchor a major quest.

**Atmosphere:** Wind across the runway, loose metal panels, distant alarms, and echoing hangars.

### 2.8 Forest

**Role:** Concealment, exploration, traversal, and natural resources.

**Environment**
- Dense pine and deciduous woodland with variation in tree height and canopy density.
- Narrow dirt trails, clearings, fallen logs, rocks, underbrush, and ground cover.
- Occasional abandoned cabins, hunting stands, and improvised camps.
- Open patches to break up the canopy and help players navigate.

**Gameplay**
- Unpredictable zombie encounters due to limited visibility.
- Optional resources: branches, herbs, mushrooms, and abandoned supply caches.
- Trails connect the quarry, airfield, suburbs, and other regions.
- Clearings can contain rare loot or environmental storytelling.
- Vegetation should conceal movement without making navigation frustrating.

**Atmosphere:** Wind in the canopy, snapping branches, insects, birds, and movement behind trees.

### 2.9 Port

**Role:** Coastal high-value loot destination and potential late-game extraction point.

**Environment**
- Concrete docks and piers extending into the sea.
- Warehouses, cargo containers, loading cranes, fenced storage yards, and harbor office.
- Security checkpoint, fuel depot, and abandoned cargo vessel or smaller ship.
- Coastal roads connecting the port to the city and nearby beaches.

**Gameplay**
- Medium-to-high zombie density in warehouses and container yards.
- Loot: rare supplies, imported goods, fuel, tools, and medical crates.
- Containers create narrow corridors suitable for stealth and ambushes.
- A locked ship or harbor control room can contain a major quest objective.
- A boat or evacuation mission can serve as an endgame objective.

**Atmosphere:** Seagulls, waves against concrete, ship horns, metallic chains, and creaking cranes.

### 2.10 Quarry

**Role:** Rugged industrial area with difficult navigation and crafting materials.

**Environment**
- Large excavated pit with stepped rock terraces.
- Exposed stone, gravel piles, uneven ground, and steep slopes.
- Winding access roads descending into the pit.
- Abandoned excavators, dump trucks, drilling equipment, and maintenance sheds.
- Small processing facility and storage area near the entrance.

**Gameplay**
- Low-to-medium zombie density in the open pit; higher around buildings.
- Loot: scrap metal, mechanical components, tools, and construction materials.
- Height differences create ambush points and alternate routes.
- Steep edges need correct collision and navigation restrictions.
- Optional hidden bunker, storage room, or underground facility for rare loot.

**Atmosphere:** Wind through the pit, falling gravel, distant metallic impacts, and deep echoes.

## 3. Shared world elements

### Road network
- Main paved road connects the city, factory, farms, and port.
- Secondary paved roads connect suburbs and airfield.
- Dirt tracks lead to mountain camp, forest cabins, and quarry.
- Roads should be continuous and navigable, with junctions, shoulders, potholes, abandoned vehicles, and damaged sections.
- Keep major routes readable and leave sufficient clearance for players and vehicles if the game supports vehicles.

### Rivers, bridges, and drainage
- Include a winding river flowing inland toward the coast where compatible with the reference layout.
- Add a few bridges and smaller drainage channels.
- Bridges must be traversable and connect correctly to roads.
- Water should block walking unless a designated crossing or usable boat is provided.
- Avoid placing water across a required route without a crossing.

### Roadside encounters
- Add abandoned cars, overturned trucks, barricades, small checkpoints, wreckage, and occasional supply stashes.
- Use these as visual storytelling and optional loot encounters.
- Avoid blocking all routes through a region.

### Environmental decay
- Use broken windows, peeling paint, rust, cracked asphalt, weeds, fallen signs, debris, and damaged street furniture.
- Vary decay by location instead of making the whole map look identical.
- Keep important landmarks visually readable from a distance.

### Fences and checkpoints
- Use perimeter fencing around the factory, airfield, quarry facilities, and port.
- Add gates, guard booths, damaged sections, and occasional watchtowers.
- Fences need appropriate collision; create openings only where traversal is intended.

### Coastline and map boundary
- Use coastline, cliffs, dense vegetation, impassable water, or other believable boundaries to keep the player inside the 2 km × 2 km world.
- Ensure the port and beaches meet the water cleanly.
- Do not allow players to walk over steep cliffs or escape the playable area.

## 4. Global gameplay rules

### Zombie density
- Highest: Main City and major industrial interiors.
- Medium to high: Factory, Airfield, Port, and selected suburban hotspots.
- Low to medium: Suburbs and Farms.
- Generally lower but less predictable: Forest, Mountain Camp, and Quarry.
- Make density configurable by location rather than hardcoding it into unrelated scripts.

### Loot tiers
- Houses and farms: common food, water, clothing, household tools.
- Clinic: medical items.
- Police/security buildings: ammunition and security equipment.
- Factory and quarry: industrial crafting materials and mechanical components.
- Airfield and port: rare supplies and specialized equipment.
- Use a reusable loot table or loot-container system if one exists in the project.

### Navigation and collision
- Ensure terrain, buildings, fences, cliffs, and water have appropriate collision.
- Keep intended paths traversable.
- Support zombie pathfinding through roads, clearings, and enterable building spaces as appropriate for the current game architecture.
- Do not let zombies navigate through solid walls or inaccessible terrain.

### Lighting and sound
- Use a consistent sun direction and realistic shadows.
- Add atmospheric fog selectively, not so heavily that navigation becomes frustrating.
- Make interiors darker than outdoor areas where appropriate.
- Add biome-specific ambient audio and positional zombie audio.
- Use interior reverberation where supported.

### Performance
- Instance repeated vegetation and props where supported.
- Use level of detail for distant objects.
- Enable occlusion culling for buildings when appropriate.
- Use chunking, streaming, or additive loading if supported by the engine.
- Avoid creating thousands of unique, unbatched objects in a single expensive runtime operation.
- Provide editor controls for regeneration and preserve hand-authored changes where practical.

## 5. Recommended implementation stages

1. **Inspect the project:** Identify the engine, version, existing scene structure, terrain system, input, navigation, zombie AI, and asset pipeline. Do not replace existing systems before understanding them.
2. **Terrain and coastline:** Establish the full 2 km × 2 km terrain, elevation, mountains, lake, river, quarry excavation, and coastal areas.
3. **Roads and navigation:** Build primary roads, secondary roads, trails, and bridges.
4. **POI blockout:** Create all ten locations using simple placeholder geometry at correct scale.
5. **Building interiors:** Make selected houses, warehouses, hangars, and city buildings enterable.
6. **Vegetation and props:** Add forests, fields, vehicles, fences, debris, and environmental storytelling.
7. **Gameplay systems:** Integrate zombie spawning, navigation, loot containers, interaction points, and quests with existing systems.
8. **Polish and optimization:** Add materials, lighting, audio, LODs, collision checks, and performance validation.

## 6. Instructions for Claude Code

1. Inspect the existing project before editing files. Identify the engine and relevant scenes, scripts, prefabs, and assets.
2. Present a short implementation plan tied to the project's actual architecture.
3. Implement in manageable stages. Prefer reusable tools, modular prefabs, and data-driven location definitions over one enormous script.
4. Use placeholders when final assets are unavailable; do not silently assume paid assets or unavailable packages.
5. Keep the world scale accurate. Use the coordinate table as a starting point and adjust positions if needed to preserve the reference layout.
6. Test each stage and report what was implemented, what remains, and any manual editor steps required.
7. Avoid breaking existing gameplay systems. Reuse existing zombie AI, loot, player movement, and interaction systems when available.
8. The result must be a playable world, not merely a flat image or minimap.
9. If the project is Unity, consider an editor-side procedural map-generation tool and modular prefabs so terrain, roads, and POIs can be adjusted without manually placing every object.
10. Do not claim the map is complete until the generated scene has been opened or tested in the project and major issues have been checked.

## 7. Reference image

Attach the generated top-down zombie-survival map image alongside this document when prompting Claude Code. Treat it as a visual reference for relative placement, silhouette, roads, terrain, and atmosphere—not as an exact pixel-to-world blueprint.
