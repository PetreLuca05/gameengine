# Weapon attachments and sights

Optics (red dot, 4x, 8x), muzzle devices, underbarrel grips, magazines and stocks. Each weapon
instance carries its own attachments. The ADS pose is exact by construction for every weapon and
optic. Scopes render a real picture-in-picture.

Code:

| File | What it does |
|---|---|
| `src/gameplay/attachments.c/.h` | Definitions, placement, effective weapon, FOV rules |
| `src/game/game_sights.c` | Per frame: FOV, sensitivity, renderer hand-off, `ads_debug`, tests |
| `src/render/post/post_sights.c` | Sight passes |
| `src/render/render.c` | Scope picture-in-picture (PiP) scene pass |
| `shaders/sights/*.frag` | Sight shaders |
| `src/ui/menus.c` | Weapon bench UI |

## Data

### Attachment files: `assets/attachments/*.json`

Ids are 1-based `u8` indices, assigned in file-name order. Every machine loads the same files, so
the ids agree everywhere; they are what gets replicated.

Attachment space uses the item convention: -Z points forward (along the barrel), +Y points up.

| Key | Meaning |
|---|---|
| `id`, `name` | Identifier and display name |
| `slot` | `optic`, `muzzle`, `underbarrel`, `magazine` or `stock` |
| `sight` | Optics only: `reddot` or `scope` |
| `mount` | `{pos, rot}`: the frame placed on the weapon's slot socket. Usually the bottom centre (optics), the rear face (muzzle devices) or the top (grips, magazines). |
| `sight_socket` | The ADS point on the optic axis: the rear of the red dot window, or the scope ocular |
| `eye_relief` | Metres from the eye to `sight_socket`. `"weapon"` keeps the iron-sight stock weld (red dots). |
| `zoom` | Magnification |
| `ads_fov` | Explicit sight FOV (degrees against a 75° view). Scopes: sets the lens magnification `tan(37.5°)/tan(ads_fov/2)`; red dots: the hands / weapon zoom. The default is `2·atan(tan(base/2)/zoom)`. |
| `main_zoom` | Scopes: mild zoom of the hands / weapon projection around the scope (default 1.15); the world is not zoomed |
| `rail_offset` | Metres along the rail; + moves towards the stock. Clamped to the slot's `rail`. Scopes default to `"auto"` (see Eye relief). |
| `reticle` | See the next table |
| `lens` | See the table after it |
| `ads_dof_near_blur` | Near-field (weapon) blur while aimed, in CoC pixels |
| `modifiers` | Multipliers. Default 1, see below. |
| `muzzle_length`, `muzzle_device` | Muzzle devices: the muzzle moves to the device's front face. `muzzle_device` is a block, `{ "kind": "hider|comp|suppressor", "flash": {...}, "ports", "report_db", "velocity_mul", "poi_moa", "pop_cold_s", ... }`. Its fields are the flash component multipliers, the report level, velocity / point of impact, and the suppressor pop / heat ([BALLISTICS.md](BALLISTICS.md) "Muzzle devices"). A plain string picks only the VFX device. |
| `grip_l` | Underbarrel grips: the left-hand socket, as `{pos, fingers, back}` (palm frame) |
| `model` | Box parts `{offset, half, color}`, up to 10 |

`reticle` keys:

| Key | Meaning |
|---|---|
| `shape` | `dot`, `circle_dot`, `duplex` or `mildot` |
| `moa` | Dot diameter, or scope line thickness, in MOA |
| `outer_moa` | Circle or duplex post gap |
| `color`, `brightness` | Reticle colour and brightness |
| `ffp` | First focal plane |
| `illuminated` | Illuminated reticle |
| `parallax_m` | Red dot: the parallax-free distance |

`lens` keys:

| Key | Meaning |
|---|---|
| `pos`, `radius` | The window, or the scope ocular |
| `objective`, `objective_radius` | The scope objective |
| `eye_box`, `eye_box_depth` | Scope eye-box tolerances, lateral and along the axis, in metres |
| `tint`, `dust`, `fresnel` | Glass |
| `chromatic` | Rim chromatic aberration in pixels |

`modifiers` keys: `ads_time`, `sway_scale`, `recoil_vertical`, `recoil_horizontal`,
`camera_kick`, `viewmodel_kick`, `spread_hip`, `spread_ads`, `damage`, `range`, `mag_size`,
`reload_time`.

Shipped attachments:

| Id | Slot | Effect |
|---|---|---|
| `red_dot` | optic | 1.4x, 2 MOA dot, parallax-free at 50 m |
| `scope_4x` | optic | Duplex reticle, SFP, picture-in-picture |
| `scope_8x` | optic | Mil-dot reticle, illuminated centre, tight eye box |
| `suppressor` | muzzle | 0.92 recoil, 0.95 damage, 0.9 range, suppressor flash |
| `compensator` | muzzle | 0.86 vertical recoil, 0.78 horizontal recoil |
| `vertical_grip` | underbarrel | Left hand moves onto the grip; 0.85 recoil, 0.9 sway |
| `extended_mag` | magazine | 1.5x capacity, slower reload |
| `heavy_stock` | stock | 0.88 recoil, 0.8 camera kick, 0.8 sway, slower ADS |

### Weapon slots: `data/weapons/*.json`, `"attachment_slots"`

```json
"attachment_slots": {
  "optic":  { "pos": [0, 0.085, -0.02], "rail": [-0.06, 0.04], "allow": ["red_dot", "scope_4x", "scope_8x"], "hide_parts": [4] },
  "muzzle": { "allow": ["suppressor", "compensator"] },
  "underbarrel": { "pos": [0, 0.015, -0.3], "allow": ["vertical_grip"] },
  "magazine": { "pos": [0, 0.03, -0.08], "allow": ["extended_mag"], "hide_parts": [3] },
  "stock": { "pos": [0, 0.035, 0.08], "allow": ["heavy_stock"], "hide_parts": [2] }
}
```

- `pos` and `rot` give the socket in item space. A muzzle slot without `pos` sits on the barrel tip.
- `rail` gives the movement range along +Z.
- `allow` lists the accepted attachments; empty means any attachment of that slot type.
- `hide_parts` lists indices into `model` that are hidden while the slot is filled: iron sight
  blocks, built-in scopes, the stock or magazine being replaced.
- `default` gives the attachment a newly spawned weapon comes with. The DMR spawns with
  `scope_4x` and the sniper with `scope_8x`.

The iron sight stays the default sight. It is `hold.sockets.sight`, or the one derived from the
model, used whenever no optic is mounted.

## Placement and exact ADS

Attachment space maps to item space as:

    place = slot socket x T(0, 0, rail position) x inverse(mount)

The mounted optic's sight point becomes the item's ADS sight socket:

    sight = place x sight_socket

The hold point puts the item at:

    item (camera space) = T(0, 0, -eye_relief) x inverse(sight)

This is `holdpoint_ads_item`. The sight lands exactly on the view axis at the eye relief, with the
barrel along the view, for every weapon and every optic.

### Eye relief

- Red dots (`"eye_relief": "weapon"`) keep the iron sights' stock weld: the grip stays where the
  iron sights put it. `eye_relief_add = iron_sight.z - sight.z`.
- Scopes have a fixed eye relief (0.09 m for the 4x, 0.085 m for the 8x). With `rail_offset`
  `"auto"`, the scope slides on the rail the way a real scope is mounted. The cheek weld (the eye
  to grip distance of the iron sights) and the eye relief together fix where the ocular must be:

      rail = (weld - eye_relief) - sight_z(rail 0)
      weld = iron eye relief + iron_sight.z

  The grip therefore stays within arm's reach and the hold never has to pull the gun.

### Support hand in ADS

If the support arm can't reach `grip_l`, it is never stretched. Before, the gun slid back along
the barrel to compensate, which changed the eye relief on long guns (by up to 52 mm on the LMG).

In ADS the item now never moves. Instead the left hand slides back along the handguard:
`ProcState.grip_l_slide`, up to 8 cm in front of the grip, in both the tick solve and the
per-frame re-solve. The barrel pull is used only if the slide isn't enough.

## Per-instance attachments and the network

- `InvSlot.att[ATT_SLOTS]` holds the ids per slot. The inventory is replicated to its owner
  whole. Pickups (`Pickup.att`), drops and containers keep the attachments.
- `World.attach[entity][slot]` holds the ids of the item an entity holds: a player's equipped
  weapon, or a pickup. It is replicated in `NetEnt.attach` under `NF_ATTACH`.
- `NF_ATTACH` uses a per-slot delta: a 5-bit mask against the baseline, then only the changed
  ids. A new entity with nothing attached skips the field entirely.
- **Host authority.** The `attach` command, the bench and `RPC_ATTACH` all go through
  `game_sv_attach`. It validates the weapon's slot and the `allow` list. If the capacity shrinks,
  for example when an extended magazine is removed, the extra rounds go back to the reserve.
- **Modifiers.** `attach_weapon_effective(db, base, att, out)` is a pure function. The host
  applies it in the weapon tick, and the predicting client gets identical values from the same
  replicated inventory. `weapon_equipped_def()` returns the effective definition, so spread,
  sway, the crosshair and recoil all see the modifiers.
- **Remote players** draw the mounted parts from `World.attach`, through
  `hands_draw_held_att` and `CharacterInst.item_att` / `item_sock`. Their hold uses the effective
  sockets: the optic's sight, the left grip and the muzzle.

## FOV and sensitivity

- **The world camera FOV never changes with ADS.** It is the host's `sv_fov` (vertical degrees,
  default 75 = 107° horizontal at 16:9), the same for every player whatever their settings: no FOV
  advantage. Screens up to `sv_fov_max_aspect` (16:9) see more to the sides (Hor+); wider screens
  keep the 16:9 horizontal FOV. `cl_fov` is still accepted from old configs and ignored. Sprint
  widening (`cl_sprint_fov`) and the fire FOV punch still apply.
- **ADS zooms only the hands and the weapon.** Their projection narrows by `attach_ads_vm_zoom`
  (a tangent ratio) along `smoothstep(ads_t)`, exactly the aimed value the moment `ads_t`
  reaches 1. Iron sights use the weapon's `ads_fov`, a red dot its own `ads_fov` / `zoom` on top,
  a scope `main_zoom`; each as a FOV against a 75° view, so the gun and sight look as they did
  when this zoomed the whole view. The hip value is `RenderCamera.vm_fov_deg` (the hip FOV) or
  `cam_weapon_fov`. The sight is on the view axis, so it projects to the exact screen centre
  under either projection.
- **Scopes magnify only inside the lens.** The picture-in-picture image carries the whole
  magnification (`attach_sight_zoom`: `zoom`, or `ads_fov` against 75°) relative to the base
  FOV; the world around the scope stays at the base FOV. A second-focal-plane reticle scales with
  the weapon projection it is drawn in (`post_sights.c`). There is no full-screen scope mode.

### Sensitivity

Sensitivity while aiming is `cl_ads_sensitivity` times the sight FOV factor. The sight FOV is the
base FOV at the optic's real magnification: irons and red dots are 1x (factor 1, the world is not
zoomed), a 4x scope `2·atan(tan(base/2)/4)`.
`ads_sens_mode` sets the factor:

| Mode | Factor |
|---|---|
| `monitor_distance` (default) | `atan(c·tan(sight/2)) / atan(c·tan(hip/2))`. `c` = `ads_sens_coef` (0 gives the zoom ratio; 1 matches at the screen edge). |
| `fov_ratio` | `c · sight / hip` |
| `off` | 1 |

These are in Settings, on the Controls tab.

## Rendering

### Pass order

The scope PiP pass and its sky run after the viewmodel. The sight passes run after the depth of
field and before bloom. As a result:

- the reticle is sharp (excluded from DoF);
- the emissive dot blooms;
- TAA never smears the reticle.

### The lens model

The lens is analytic, one fullscreen triangle per pass, scissored to the lens:

1. The pixel's ray under the **viewmodel** projection is intersected with the lens disc in view
   space. The lens is part of the weapon, so it uses the projection the weapon is drawn with.
2. Viewmodel depth occludes it. `vm_depth` is now sampleable.
3. Whatever the lens shows uses the pixel's ray under the **main** projection, because it shows
   the world.

### Shaders

| Shader | What it draws |
|---|---|
| `glass.frag` | Multiply blend: the window's coated-glass tint, dust specks dimming it |
| `reddot.frag` | Additive: a collimated MOA dot, the fresnel sky reflection, lit dust |
| `scope.frag` | Premultiplied over: the PiP image, or the full-screen vignette |

**The red dot** is aimed at the convergence point (lens + axis × `parallax_m`). Any eye position
sees it on the same target point, which makes it parallax-free at that distance. It is sized in
MOA, drawn at least about 2.3 px wide with its energy spread, and emissive in exposed units. Its
brightness is `sight_brightness`.

**The scope** maps each main-view direction at tangent `t` from the optic axis to `t / k` in the
scope image, where `k = zoom · tan(main/2) / tan(base/2)`. Effects:

- radial chromatic aberration;
- a soft inner rim;
- the eye box: off the exit pupil the visible circle slides and a scope-shadow crescent closes
  in, and a wrong relief shrinks it;
- the reticle: FFP (fixed against the target) or SFP (fixed in the eyepiece);
- an optional illuminated centre;
- dark coated glass at the hip.

**The PiP pass** (`exec_scope_pip`) draws the opaque static scene and the characters, lit
forward, into a square target. The target size is `r_scope_pip_scale` × the render height,
128..1024 px. It uses:

- its own frame UBO and descriptor set, with the shadow map and skinning buffers shared;
- the camera at the objective lens, at the FOV the ocular can show;
- a near plane at 0.3 m, so the player's own body and weapon never appear;
- the same procedural sky, `post_scope_sky`.

### Depth of field

ADS DoF switches on exactly while the aim is complete (`ads_dof`):

- a fixed far focus (`ads_dof_focus`, 80 m) at `ads_dof_fstop`;
- viewmodel pixels get the optic's `ads_dof_near_blur` (irons: `ads_dof_near_blur`);
- the user's DoF mode applies again the first frame the aim is incomplete.

## Dev room

- **Weapon benches**: one at the loadout area and one at the sighting lane. Use one (or run
  `bench`) to fit attachments to the equipped weapon. The bench shows the effective stats against
  the bare weapon.
- **Sighting-in lane** (east side): fire from (32, 0, -8) facing +X. Targets stand at
  10 / 25 / 50 / 100 / 200 m. Each has a 1 MOA grid (`moa_target` brush, `MAT_GRID_MOA`): bold
  every 5 MOA, red every 10 MOA. The centre is snapped onto a 10 MOA crossing, with a 2 MOA aim
  square. The targets are staggered so the far ones stay visible.

## Commands and cvars

| | |
|---|---|
| `attach [slot] [id/none]` | Equipped weapon; no arguments lists the slots and options |
| `bench` | Toggle the bench UI |
| `ads_debug 1` | The sight socket projected against the exact screen centre (px, mm off axis, degrees, eye relief), world / hands FOV, sight zoom, PiP FOV, sensitivity, DoF mode |
| `sight_debug 1..6` | 1 lens mask, 2 raw PiP, 3 eye box, 4 reticle only, 5 glass only, 6 lens hit / occlusion |
| `sv_fov`, `sv_fov_max_aspect` | World camera vertical FOV for every player (host, replicated; 60..100, default 75) and the widest Hor+ aspect (16:9; 0 = unlimited) |
| `r_scope_pip_scale` | PiP resolution (fraction of the render height) |
| `sight_brightness` | Red dot / illuminated reticle brightness |
| `ads_sens_mode`, `ads_sens_coef` | Sensitivity scaling |
| `ads_dof`, `ads_dof_focus`, `ads_dof_fstop`, `ads_dof_near_blur` | ADS depth of field |

## Tests

| Command / script | Checks |
|---|---|
| `sight_test align all quit` (`tools/tests/sight_align.cfg`) | Every weapon × (iron + every allowed optic), 26 combinations. Pitch -89..89 at 1° per frame, then a 360° yaw sweep. Sight off axis ≤ 0.5 mm, barrel angle ≤ 0.1°, eye relief constant within 0.5 mm. |
| `sight_test sweep` | The same pitch / yaw sweep for the weapon as equipped |
| `sight_test aspect` | The sight socket through 16:9, 21:9 and 4:3 projections: ≤ 0.5 px from the centre |
| `sight_test parallax` | Red dot: an eye 2 cm off axis still sees the dot on the 50 m target within 1 MOA |
| `sight_test fov` … `sight_test_report` | The world FOV never changes while aiming; the frame ADS completes already has the aimed hands / weapon FOV and the ADS DoF; the first incomplete frame has neither |
| `sight_test net` | `NF_ATTACH` through the real snapshot encoder: full, 2-slot delta, unchanged (no field) |
| `.toolchain/snet.sh` | Two instances over LAN, each mounting optics through `RPC_ATTACH`. `sight_test dump` shows matching ids on both machines, and screenshots show the other player's optic. |
| `tools/tests/sights.cfg`, `sight_lane.cfg`, `sight_misc.cfg` | Screenshots and the tests above |

The tests above all passed:

- 26 / 26 combinations aligned: worst 0.157 mm off axis, eye-relief spread 0.172 mm;
- aspect: ≤ 0.284 px;
- parallax: 0 MOA;
- FOV / DoF: 8 / 8 switches;
- network: ids match on host and client.
