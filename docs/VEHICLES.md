# Vehicles

There are two drivable vehicles: a four-seat car and a two-seat motorcycle. The host simulates each vehicle with Jolt's `VehicleConstraint`, using `WheeledVehicleController` for the car and `MotorcycleController` for the motorcycle. The driving client predicts its own vehicle and reconciles with the host. Everyone else interpolates.

| Part | Where |
|---|---|
| Rules: definitions, fuel curve, tire model, steering clamp and smoothing, quantization | `src/gameplay/vehicle_def.c` / `.h` (pure, unit tested) |
| Jolt wrapper: body, wheels, engine, transmission, differentials, anti-roll, per-wheel grip, motorcycle lean | `src/physics/phys_vehicle.c` (`physics.h` "Vehicles") |
| Game glue (see below) | `src/game/game_vehicles.c` |
| Seated pose: pelvis on the seat, legs IK'd to the pedals | `proc_seated` in `src/anim/procedural.c` |
| Hands on the controls | `g_vehicle_pose` hooks: the characters (`characters.c`) and first-person hands (`hands.c`) |
| One-handed hold | hold-point state `HP_VEHICLE_ONEHAND` (`src/player/holdpoint.c`) |
| Definitions | `assets/vehicles/car.json`, `assets/vehicles/moto.json` |
| Placeholder meshes | `tools/gen_vehicle_mesh.py` writes `assets/vehicles/*.gltf` |
| Tests | `tests/test_vehicles.c`, `tools/tests/vehicles.cfg`, `tools/tests/vehicle_net.sh` |

The game glue covers:
- spawning, seats, enter / exit / eject;
- driving input, fuel, tires, damage, running over;
- replication, prediction and reconciliation;
- rendering, effects, audio, camera, HUD;
- commands and tests.

## Definitions and sockets

Vehicle space has its origin on the ground under the body centre (at rest), with forward −Z, up +Y and right +X. Wheel order for the car is front-left, front-right, rear-left, rear-right; for the motorcycle it is front, rear.

| Key | Contents |
|---|---|
| `body` | Up to four boxes (`center`, `half`), forming the physics compound. |
| `com_offset` | Centre of mass shift (down: stable). |
| `wheels[]` | `pos` (suspension top), `radius`, `width`, `suspension_min` / `max` / `frequency` / `damping`, `max_steer`, `driven`, `max_brake`, `max_handbrake`. |
| `engine` | `max_torque`, `min_rpm`, `max_rpm`, `gears[]`, `reverse_ratio`, `differential_ratio`. |
| `fuel` | `capacity` (l), `idle_lph`, `full_lph`. |
| `damage` | `hp`, `impact_speed`, `impact_scale`. |
| `tires` | `bullet_damage`, `leak_per_hit`, `burst_health`. |
| `seats[]` | `driver`, `can_shoot`, `yaw_limit` (free-look, in degrees). |
| `handling` | `one_hand_steer_limit`, `one_hand_spread_mult`, `passenger_spread_mult`, `exit_ragdoll_speed`. |
| `sockets` | See the list below. |

The sockets are:
- `seat_n`: the hip point.
- `exit_n`: where the player is placed on exit.
- `eye_n`: the seat eye, relative to the hip.
- `steer_pivot`, `steer_axis`, `steer_radius`, `steer_ratio`: the steering wheel or handlebar. The grips `grip_l` / `grip_r` sit on its rim at 9 and 3 o'clock and turn with it.
- `pedal_l`, `pedal_r`.
- `fuel_cap`, `exhaust`, `headlight_l`, `headlight_r`.
- `camera_tp`: the third-person orbit offset.

A missing key keeps the built-in default for that vehicle kind (`vehicle_def_defaults`).

## Driving

The driver's `UserCmd` is the control input:
- **Throttle and steering:** `move_y` is throttle, `move_x` is steering.
- **Handbrake:** `BTN_JUMP`.
- **Brake:** throttle against the direction of travel brakes; once stopped it reverses.

Steering is smoothed for keyboards (`vehicle_steer_smooth`): less lock at speed, and it self-centres faster than it turns in.

Grip is set per wheel, every tick: tire grip × surface grip × `weather_grip()` at the contact point.
- **Surface grip:** dirt 0.78, sand 0.62, gravel 0.8, grass 0.72, water 0.55, metal and tile 0.85, everything else 1.
- **Weather:** wet −15 %, puddles −10 %, snow −45 %.

The motorcycle has two balance modes:
- **Above 3 m/s:** Jolt's lean controller balances it and leans it into turns. Its spring is scaled to this bike's roll inertia; the default gains make the integration unstable at 60 Hz.
- **Below 3 m/s:** a roll torque holds it upright, standing in for the rider's feet or the kickstand.

## Seats, entering and leaving

| Action | Keys |
|---|---|
| Enter the looked-at vehicle | F (the flashlight key works as "use vehicle" while looking at one) or E |
| Exit | F |
| Change seat | 1–4 while seated |
| Third-person camera | V |
| Headlights | H (driver) |
| Horn | Q |

**Entering:** the player goes to the first free seat, the driver's seat first. The player's movement mode becomes `MOVE_SEATED` (`PlayerMove.vehicle` / `seat`). The player follows the seat and the character's capsule is disabled.

**Exiting:** the exit point is the first clear one of: the own door's `exit_n`, the other seats' exits, then over the roof. "Clear" means ground underneath and a free capsule. If none is clear, the player stays in ("No room to get out").

**Ejection:** leaving faster than `exit_ragdoll_speed` (6 m/s car, 5 m/s motorcycle) throws the player out:
- with the vehicle's speed;
- stunned for 1.2 s + 0.06 × speed, at most 3 s, with no control;
- taking (speed − limit) × 4 damage;
- as a ragdoll on every machine (`EV_VEHICLE`), which stands back up when the stun ends.

**Seated pose:** a procedural pose with no clip. The pelvis goes on the seat, the thighs are flexed, and the legs are IK'd to the pedals or pegs. The body faces the vehicle, foot IK and lean are off, and the character animates as if standing still: no run cycle and no camera bob.

**Draw time:** the body is solved at tick rate, then re-attached to the vehicle where it is drawn this frame (`seat_fix`). This stops it trailing a fast car.

**Driver hands:** both hands are IK'd to the steering wheel or handlebar grips and turn with it. The weapon is holstered.

**One-handed shooting:**
- Hold right mouse button with a `one_hand_capable` weapon. That flag is in the weapon JSON and is set on the pistol and the SMG.
- The right hand takes the gun in the `HP_VEHICLE_ONEHAND` hold: the ADS sight line with the arm 8 cm further out and the gun canted 8°.
- In first person only that arm gets the viewmodel treatment (own FOV, hold animation). The left arm stays on the wheel as world geometry.
- Steering is clamped to `one_hand_steer_limit` (car 0.35, motorcycle 0.25).
- Spread is multiplied by `one_hand_spread_mult` (1.6 / 1.8) and by a speed factor (up to ×1.4 at 25 m/s).
- The flag is replicated in the vehicle block, so other players see the pose.

**Passengers:** a seat with `can_shoot` keeps the weapon. Spread is multiplied by `passenger_spread_mult`.

**Own vehicle:** bullets ignore the shooter's own vehicle, and so does the hold point's wall check, so the gun never retracts from the windshield.

## Fuel

- **Burn rate:** `idle + (full − idle) × throttle × (0.3 + 0.7 × rpm fraction)` litres per hour (`vehicle_fuel_rate`). It is multiplied by `vehicle_fuel_scale` (0 = infinite fuel).
- **Empty tank:** at 0 the engine dies (`engine_stall`) and the vehicle coasts.
- **Refuelling:**
  - **Fuel can** (12 l): hold use next to a vehicle.
  - **Fuel pump** (level `"fuel_pumps"`): hold E for 2.5 s to fill the nearest vehicle within 8 m. This is a stub: fuel is free.
- **Replication:** u8 (fraction of the tank).

## Tires

Each tire has `pressure` (0..1), `health` (0..1) and a `leak` rate.

**Damage:**
- **Bullets:** a bullet hitting the wheel cylinder costs health and adds leak. The cylinder is tested analytically against the shot ray, using the vehicle pose rewound to the shooter's view tick on the host.
- **Bursting:** when health drops to `burst_health` the tire bursts (pressure 0). Three hits burst a tire.
- **Spike strips** (level `"vehicle_spikes"`): burst a tire on contact.

**Effects:**
- **Grip:** full down to 70 % pressure, then falling with the square of the pressure. On the rim, grip is 0.25.
- **Rolling radius:** shrinks to 82 %.
- **Pull:** uneven deflation pulls the car towards the flat side (up to ±0.35 steering). A flat on the front-left drifts the car left with no steering input.

**On the rim:** sparks on hard ground.

**Repair:**
- **Tire repair kit:** plugs the leak and inflates to 85 %. It is refused on a destroyed tire.
- **Spare tire:** a new tire. It is a stub without a fitting animation.

**Replication:** pressure and health, u8 each.

## Damage

- **Impacts:** a velocity change above 4 m/s in one tick is an impact. Damage is `(1.6 × Δv − impact_speed) × impact_scale`. Occupants take (Δv − 8) × 4.
- **Bullets:** hit the body (`C_HEALTH`).
- **Wrecked:** at 0 hp the vehicle is wrecked. The engine stops, it smokes, it takes no more damage, and it can still be entered.
- **Running over** zombies, players or dummies:
  - The target must be inside the body box (+0.45 m) and the vehicle moving above 3 m/s.
  - Damage is speed² × 1.2: 43 at 6 m/s, 120 at 10 m/s.
  - Each target has a 0.5 s cooldown, and the vehicle keeps 98.5 % of its speed per hit.
- **Zombies:** they shove a vehicle through their kinematic capsules, and a pack can pin it. The physics stays stable under 50 of them (see Tests).

## Networking

- **Snapshots:** every snapshot carries a vehicle block, about 80 bytes per vehicle: entity, definition, fuel, tires, seat occupancy, steering, rpm, gear, suspension, flags (engine, lights, brake, wrecked, one-hand, handbrake), and the exact pose, velocity and angular velocity. Vehicles are always relevant. Seat state is also in `PlayerMove` (protocol 11).
- **Driving client:** creates its own `PhysVehicle` and simulates it from its own commands every tick, stepping the client's physics world. It records the predicted pose per command.
- **Reconciliation:** when a snapshot acknowledges command N, the host's pose is compared with the prediction for N.
  - **Below 3 m:** 20 % of the position, rotation and velocity error is applied, and the drawn car glides through a decaying correction offset.
  - **Above 3 m:** the car snaps to the host state, extrapolated by the unacknowledged commands.
  - **Measured:** with 80 ms fake lag, the average error was 3 mm, the maximum 31 mm, and there were no snaps (`vehicle_net.sh`).
- **Other vehicles:** interpolated like every entity, with kinematic proxy boxes so players collide with them. Seated remote players are placed on their seat as the vehicle is drawn.
- **Events:** enter, exit, doors, engine start and stall, horn, crashes and bursts are host sound events. The eject ragdoll is `EV_VEHICLE`.
- **Lag compensation:** the host keeps 64 ticks of vehicle poses for wheel hits.

## Presentation

**Models:** body, steering wheel or handlebar, and wheels. Wheels show suspension travel, steering, roll from speed (locked under the handbrake) and the flat-tire radius. Materials named `paint*` take the vehicle colour, and damage darkens it.
- **Glass:** translucent (blended pass).
- **Lights:** headlights glow only with the lights on, brake lights brighten when braking.
- **Headlights:** two spot lights. The two nearest vehicles get them, because the renderer has 4 spot lights in total.

**Effects:** exhaust puffs (rate by rpm), dust on loose ground, skid-mark decals (lateral slip, locked wheels, hard braking on hard ground), rim sparks, smoke when damaged.

**Audio:**
- `engine_loop`: positional, pitched by rpm.
- `tire_skid`: a loop while skidding.
- One-shots: `horn`, `vehicle_crash`, `vehicle_door`, `tire_burst`, `engine_start`, `engine_stall`.
- All are generated by `tools/gen_assets.py` (`gen_vehicle_sounds`). Positional loops come from `audio_loop3d`.

**Camera:**
- **First person:** the head eye (with no bob, since the seated body is still).
- **Third person:** an orbit behind the vehicle with collision, toggled with V or `cl_thirdperson`.
- **Free look:** relative to the vehicle, so the view turns with it. It is limited per seat (135°).

**HUD:**
- **Panel:** speed, gear, rpm bar, fuel and HP bars, a tire diagram coloured by pressure, a seat diagram, warnings (wrecked, no fuel, engine off, lights, handbrake, one-hand) and key hints.
- **World labels:** nearby vehicles show their name, fuel and HP.
- **Prompt:** "Drive / Ride in …".

## Dev room vehicle yard

The yard is far west of the dev room, around (−140, 0, 0). It has:
- a 60 × 40 m asphalt loop;
- a jump ramp in the infield;
- slalom cones on the north straight;
- a spike strip on the east straight;
- a fuel pump;
- two cars (one low on fuel) and a motorcycle.

The weather yard at (−50, 0, 22) also has a placeholder car.

## Commands

| Command | |
|---|---|
| `spawn_vehicle <car\|moto> [distance]` | In front of you (host). |
| `vehicle_enter [seat]`, `vehicle_exit`, `vehicle_seat <1-4>` | Local requests (tests, binds). |
| `vehicle_put <slot> [seat]` | Seat a player in the nearest vehicle (host, network tests). |
| `vehicle_cam [0\|1]`, `vehicle_lights [0\|1]` | |
| `vehicle_fuel [litres\|N%]`, `vehicle_tire <wheel> <pressure> [health]`, `vehicle_repair` | Host. |
| `vehicle_debug 1\|2` | `1` shows the state overlay (speed, rpm, gear, steering, fuel, hp, tires, prediction error). `2` adds wheel, grip and exit markers. |
| `vehicle_test <test> [quit]`, `vehicle_dump` | See Tests. |
| `vehicle_fuel_scale` (cvar) | Fuel burn multiplier. |

## Tests

**`test_vehicles`** (ctest `vehicles`) covers:
- definitions, JSON overrides and the shipped files;
- fuel curve and range;
- tire hits, leak, burst, grip curve, rim, pull direction, repair and spare;
- steering clamp and smoothing;
- impact damage;
- quantization.

**`tools/tests/vehicles.cfg`** runs these `vehicle_test` modes on the host:

| Test | What it checks |
|---|---|
| `enter` / `enter_moto` | Every seat: occupied, eye within 2 mm of the socket, exit through the socket and clear. Ejection at 14 m/s: stunned, thrown with the vehicle's speed, standing again afterwards. |
| `glue` / `glue_moto` | Nine steering angles from lock to lock, settled: palm-to-grip error. Measured mean 2.7 / 1.6 mm (car) and 0.1 mm (motorcycle). |
| `clamp` | One-hand steering clamp and spread, can-shoot rules (pistol yes, rifle no, both hands on the wheel no), holstering, passenger spread. |
| `fuel` | Runs dry, the engine dies, the car coasts; the consumption curve. |
| `tires` | A real ray hits the front-left wheel; three hits → flat; grip; drift to the flat side; repair kit refused on a destroyed tire; spare fitted; repair kit plugs a puncture. |
| `drive` / `drive_moto` | 6 s full throttle (61 km/h car, 57 km/h motorcycle); steering right turns right; upright; braking to a stop; never rolls over. |
| `zombies` | 50 zombies in the way: no invalid transform, stays upright, run-over hits. |

**`tools/tests/vehicle_net.sh`**: a host and a client with 80 ms fake lag. The client drives (prediction) and both dump the vehicle state at the end.
