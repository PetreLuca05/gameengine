# Characters: one Skeleton, many SkeletalMeshes

Deadground characters follow Unreal's `USkeleton` / `USkeletalMesh` split (Unity: `Avatar` + the
`SkinnedMeshRenderer` bones array):

- **One Skeleton asset** (`human`, the `assets/human/Dummy.fbx` rig). Everything that animates or
  simulates a body is authored against it.
- **Any number of SkeletalMesh assets** (full bodies or modular parts). They bind to the Skeleton
  **by bone name** and never carry animation of their own.

```
                   Skeleton asset "human"  (src/anim/skeleton.h)
   bones · reference pose · sockets · virtual bones · groups/masks · LOD bone sets · base pose
                                      |
     clips · anim sets · controller · ragdoll · hitboxes · foot/hand IK · hold styles
                         (AnimLibrary, src/anim/anim_library.h)
                                      |
        +---------------+-------------+-------------+-----------------+
   SkeletalMesh    SkeletalMesh   SkeletalMesh   SkeletalMesh     SkeletalMesh ...
   Dummy (body)    Ch33 (skin)    head part      torso part       hair (mesh-only bones)
   (src/render/skeletal_mesh.h: own bone list -> Skeleton indices, own LODs / materials / sockets)

  Runtime, per character:
    CharRuntime (controller, IK, procedural) --> ONE pose --> SkeletonInstance (world pose + palette)
                                                                  |  gather (per part)
                                     SkinnedMeshInstance  SkinnedMeshInstance  SkinnedMeshInstance
                                          (head)              (torso)             (legs) ...
```

## What lives where

| Data | File | Code |
|---|---|---|
| Skeleton metadata: sockets, virtual bones, groups, base pose, rig source | `assets/skeletons/human.json` | `src/anim/skeleton.c` |
| Baked Skeleton (optional, ships without the FBX) | `assets/skeletons/human.skel` (`tools/bake_skeleton`) | `skel_asset_load` |
| Clips, anim sets, controller, hold points | `assets/human/*.fbx`, `clips.json`, `anim_sets/`, `controller.json`, `holdpoint.json` | `src/anim/anim_library.c` |
| Ragdoll profile, hitbox / body-part map | `assets/human/ragdoll.json`, `hitboxes.json` | `src/anim/ragdoll_def.c`, `src/gameplay/hitboxes.c` |
| Meshes | any `.fbx` (baked through `assets/cache/`) or `.skm` (`tools/bake_mesh`) | `src/render/skeletal_mesh.c`, `src/assets/fbx_loader.c` |
| Per-mesh aliases and mesh-local sockets | `<Model>.json` next to the mesh (a "sidecar") | `anim_library.c: load_mesh_file` |
| Characters: parts, tints, anim set, damage, ragdoll scale | `assets/characters/*.json` | `src/assets/character.c` |
| Skins (player outfits) | `assets/human/skins.json` + anything dropped into `assets/skins/` | `anim_library.c: load_skins` |
| GPU parts, drawing, part selection, `cl_skin` | | `src/gameplay/characters.c` |
| Instanced batching | | `skin_batch_build` (`skeletal_mesh.c`), `render.c` |

Animation, IK, the ragdoll, hitboxes, sockets, the hold point and hand glue only ever see the
Skeleton (`lib->skel`) and the character's pose. None of them knows what meshes a character wears.

## The Skeleton asset

`SkeletonAsset` (`src/anim/skeleton.h`) is the `Skeleton` (bones, names, reference/bind pose,
name → index, roles, mirror table) plus:

- **Sockets.** Named transforms on bones: `camera`, `hand_r_grip`, `hand_l_grip`, `back`, `hip_r`,
  `hip_l`, `seat`, `head_top`.
  - `"space": "bind"` (the default): the offset is in model space at the bind pose.
  - `"space": "bone"`: the offset is in the bone's local frame.
  - Characters inherit every skeleton socket. A character's own `"sockets"` / `"eye_offset"`
    override them by name.
- **Virtual bones** (Unreal-style). `{ "source": A, "target": B }` is a bone parented to A that
  follows B. It is never added to the hierarchy, clips or the wire.
  - Example: `VB hand_l_in_hand_r` is the left hand in the right hand's space, the natural target
    for two-handed glue.
  - API: `skel_vbone_xform` / `skel_vbone_local` / `skel_inst_vbone_world`.
- **Bone groups / masks.** Built in: `all`, `upper_body`, `lower_body`, `spine`, `head`, `arms`,
  `arm_l`, `arm_r`, `hands`, `fingers`, `legs`, `feet`, `hair`, `face`.
  - The JSON adds more, composed of `chains`, `bones`, `groups`, `exclude_chains` and `exclude`.
  - Used by part regions, hide masks and `skel_asset_group_mask` (a `BoneMask` for layered blends).
- **LOD bone sets.** LOD0–1 use every bone. LOD2+ use no fingers and no end bones; mesh joints
  outside a LOD's set are folded onto their kept ancestor when the mesh is baked.
- **Retarget base pose.** The reference other rigs are mapped onto. It equals the bind pose unless
  `"base_pose": { "LeftArm": { "rotation": [x, y, z] } }` says otherwise.
- **Hashes.**
  - `skel.hash` (names + hierarchy + bind) is the identity meshes are baked against.
  - `skel_asset_hash` (bones + metadata) goes into the network manifest.

`SkeletonInstance` exists **once per character**, whatever its part count. It holds the drawn pose
(bone world transforms) and the shared palette (`world × inverse bind`). `palette_builds` counts
palettes: one per drawn pose, not one per part.

## SkeletalMesh assets

A `SkeletalMesh` is skinned geometry (`SkinMeshData`: LODs, materials) plus **its own bone list**.
Each entry is either:

- a Skeleton bone (`skel ≥ 0`), or
- a **mesh-only leaf bone** (`skel = -1`): hair, cloth, straps, Mixamo end bones. It rides on its
  `parent` entry with a fixed `local` bind transform.

Vertex `joints[]` index that list, so a head part skins with 2 bones instead of 65.

- Bones without weights are compacted away at bake.
- Per-bone `bind_delta` records how the file's bind differs from the Skeleton's reference pose
  (file bind = `bind_delta * bind_model`).
- Mesh-local `sockets` resolve against the mesh's own bones, e.g. a backpack attach on one specific
  jacket (`skm_socket_world`).

**Palette gather.** `skm_gather_palette(mesh, skeleton_instance, leaf_local, out)` copies one matrix
per skeleton bone from the shared palette and composes one transform per mesh-only bone. Bones with
`has_fix` (meshes kept in their file bind, see Proportions) multiply by `bind_fix` = `inverse(bind_delta)`.
`leaf_local` is the hook for a future secondary-motion layer: it overrides mesh-only locals, and
NULL means rigid.

### Compatibility rules (Unreal's)

| Case | Result |
|---|---|
| Every skinned bone exists in the Skeleton by name, same parent chain | compatible |
| Skeleton bones the mesh lacks (a bald head has no hair bones) | fine, they're simply not skinned |
| Extra bones in the mesh | mesh-only leaf bones riding on their parent; they never enter the Skeleton |
| Renamed bones | rejected; fix with an alias table |
| Another hierarchy (a bone re-parented, an inserted bone with skeleton children) | rejected with the offending bones listed |
| Other proportions | allowed |

Proportions:

- Every influence moves a vertex rigidly from the file's bind of its bone to that bone on the Skeleton
  (`world * inverse(file bind)`, Unreal's per-mesh inverse binds), so a mesh keeps its shape on the shared
  skeleton.
- Two ways to get there, picked per mesh at bake (`fbx_load_skeletal_mesh`):
  - **Baked:** the per-bone retarget is blended into the vertices and the palette is the plain skeleton
    one. Exact only while a vertex's influences retarget alike. The bake bounds the error over every pose
    (weighted distance between the blend and each influence's own retarget, `retarget_blend_err_m`).
  - **Per bone** (bound > 0.1 mm): the vertices stay in the file's bind space and the palette retargets
    per bone (`SkmBone.bind_fix`). The Dummy stays baked (bound 0.00 mm, gather is a plain copy); the
    Mixamo skins (knees 57-112 deg, shoulders ~18 deg off the Dummy bind) go per bone: baking it was off by
    up to 65-97 mm in strong bends (mean 1.5-4.3 mm), now 0.01 mm against the file's own skin.
- Weights are quantised to u8 summing to exactly 255 (`skin_quantize_weights`): the shaders never
  renormalise, so 256 scaled a vertex about the world origin (1.66 m off at 300 m).
- Bone-length differences above the tolerance are reported as warnings.
- IK, the ragdoll and hitboxes read bone lengths from the Skeleton and scale by the character's
  `scale` / `ragdoll_scale`.

Name matching ignores namespaces (`mixamorig7:Hips` is `mixamorig1:Hips`). The check is
`rig_validate` (`src/assets/character.h`), run on every FBX before it is baked.

### Aliases

An alias table maps file bone names to Skeleton names. Put it in the model's sidecar, a same-name
`.json` next to the FBX:

```json
{ "aliases": { "mixamorig7:HeadTop_End": "mixamorig1:Head_end", "upperarm_l": "mixamorig1:LeftArm" } }
```

- `tools/bake_mesh` prints the suggested aliases for every renamed bone it recognizes, and
  `--write-sidecar` writes them out.
- The Mixamo skins in `assets/human/` have sidecars mapping their end bones (`Thumb4`,
  `HeadTop_End`, `Toe_End`) onto the canonical `*_end` bones. With those, the rigs validate as
  identical.

## Characters and modular parts

```json
{
  "id": "zombie_placeholder", "character_id": 2, "kind": "zombie",
  "materials": { "Alpha_Body_MAT": [0.42, 0.52, 0.36] },
  "parts": {
    "head":  [ { "id": "head_a", "mesh": "assets/human/Dummy.fbx", "region": "part_head", "weight": 2 },
               { "id": "head_big", "mesh": "assets/human/Dummy.fbx", "region": "part_head", "scale": [1.12, 1.1, 1.12] } ],
    "torso": { "mesh": "assets/meshes/characters/jacket.skm", "hide": ["torso"] },
    "legs":  "assets/meshes/characters/legs_a.skm",
    "gear_helmet": { "mesh": "assets/meshes/characters/helmet.skm", "weight": 0.3 }
  },
  "anim_set": "zombie", "part_mult": { "head": 4.0 }, "ragdoll_scale": 1.0, "scale": 1.0
}
```

**Slots.** `body`, `head`, `torso`, `legs`, `hands`, `hair`, `gear_0`..`gear_3`.

- `gear_<name>` takes the next free gear slot.
- Slot order is the wire order.
- `"mesh": path` alone (the old format) is one `body` part, so every existing character file still
  works.

**Options.** Each slot lists one or more options.

- A string is just a mesh path. An object can have:
  - `id`, `mesh`, `weight` (random variety; 0 = only by choice);
  - `default: true`;
  - `materials` (tints by material name, over the character's);
  - `hide` (bone groups whose triangles the character's **other** parts drop, e.g. the body under
    a jacket);
  - `region`, `inflate`, `scale` (see placeholders below).

**Selection.** The selection is one byte per slot (0 = empty, k = option k−1).

- The host picks it at spawn (`character_pick_parts`, seeded by the entity): defaults win,
  otherwise a weighted random pick.
- Players get the default player skin, then whatever their `cl_skin` asks for.

**Swapping.** `character_set_part <slot> <id|none> [entity]` (host), `char_inspect_swap`, or
`chars_set_part`.

- A swap only changes the mesh list. The pose, controller and ragdoll are untouched, so swapping
  mid-animation is seamless.
- Use it for outfits, zombie variety, or armor coming off after body-part damage.

**Rendering.**

- Each worn part becomes a **GPU part**: LOD meshes with tints baked into vertex colours (or a skin
  atlas), minus hidden triangles, plus the first-person arm/no-arm split.
- GPU parts are keyed by (mesh, tints, texture, hidden bones) via `character_part_key`, and shared
  by every character wearing them the same way.
- Instanced batches are per (texture, GPU part LOD): 200 placeholder zombies give 800 part draws in
  40 batches (10 GPU parts × 4 LODs).
- **Palettes in place** (docs/perf/FINDINGS.md PF-0020). `draw_part` asks `render_skin_palette` for room
  in this frame's bone buffer and `skm_gather_palette` writes there directly: a mapped ring of
  3 × `MAX_SKIN_BONES` matrices shared by the frame slots (device-local BAR memory where the driver
  offers it; `render: skinning palette ring` in the log). Frame N writes region N mod 3; its
  previous-palette (velocity) lookups read region N - 1 where last frame left them. The request is
  keyed by (skeleton instance, pose version, the GPU part's palette class): parts whose bone tables
  are equal (`skm_palette_equal`, decided once per GPU part) and every LOD / shadow draw of the same pose
  get one copy (the PF-0007 sharing without a compare: the ring is write-only memory). No CPU bone
  array, upload copy or previous-palette copy remain. `perf_fix_disable PF-0020` (and `r_cpu_skinning`)
  goes back to the per-slot buffer and the copies.
- **Draw poses on the jobs** (PF-0021). `chars_render` takes every drawn character's pose source on the
  main thread (interpolation instant, seat fix, ragdoll poses: they read the physics world), interpolates
  the poses and builds the skeleton palettes with `jobs_parallel_for`, then issues every draw on the main
  thread in entity order (each part reserving its palette) and gathers the reserved palettes on the jobs.
  Poses, palettes, offsets and draw order are those of the serial loop (`perf_fix_disable PF-0021`).
- **Shared poses** (PF-0034, docs/ANIMATION.md "LOD"). A far character in a plain base-layer state draws
  its phase bucket's cached model-space palette (`SkinnedDraw.rooted`: the instance's `root` /
  `prev_root` rows place it, `SKIN_ROOTED` in `info.z`; `skin.vert` / `skin_shadow.vert` apply them).

**Seams.** Parts cut from one mesh share their seam vertices exactly. Authored parts should share
edge vertices at the seams; `hide` covers overlaps.

### Placeholder modular set (no art needed)

`zombie_placeholder` cuts `Dummy.fbx` into parts at load time:

- `"region": "part_head" | "part_torso" | "part_hands" | "part_legs"` are skeleton groups that tile
  the body. Each triangle belongs to exactly one owner bone, so the parts add up to the full mesh
  with no holes and no doubled triangles.
- Variety comes from tints plus `inflate` (metres along the normals) and `scale` (about each
  vertex's bone).

`tools/split_parts` does the same cut offline and writes `.skm` parts.

## How to…

**Add a skin (drag and drop).** Drop `MyHero.fbx` (or `.skm`) into `assets/skins/` and restart.

- It becomes skin `myhero`: selectable in Pause > Skin / `cl_skin myhero`, and seen by every player.
- Optional `MyHero.json` next to it: `name`, `atlas` + `tiles` (texture atlas, as in `skins.json`),
  `aliases`, `sockets`.
- An incompatible rig is reported in the log with suggested aliases.

**Add a character model.** Bake or validate it (`bake_mesh path/Model.fbx`), then add one JSON line:
`"mesh": "path/Model.fbx"` in a new `assets/characters/<id>.json`, or an option in an existing
character's `parts`.

**Make a modular set.**

1. Author the parts on the Dummy rig: one mesh per part, sharing seam vertices, the same skeleton
   names, any subset of bones, extra leaf bones allowed.
2. `bake_mesh --out assets/meshes/characters parts/*.fbx` (or reference the FBX files directly;
   they bake through the cache).
3. List them per slot in the character JSON with weights. Use `hide` where a part covers another.
4. Check the result in-game with `char_inspect 1`.

**Use a rig with other bone names (store asset).**

1. Run `retarget Model.fbx --write-sidecar [--skm out.skm]`. It maps the humanoid bones (Mixamo, UE
   mannequin `pelvis`/`spine_01`/`upperarm_l`/`thumb_01_l`, Unity `LeftUpperArm`, Bip) and writes
   the alias sidecar.
2. The model then loads like any other.
3. Base-pose correction per bone and `retarget_pose` are implemented. `retarget_clip` (batch clip
   import from foreign rigs) is a stub.

## Tools

| Tool | Does |
|---|---|
| `bake_skeleton [--id human] [--out file] [--list]` | Bakes `assets/skeletons/<id>.skel`, checks the bake is deterministic, lists bones / sockets / virtual bones / groups / LOD sets |
| `bake_mesh [--skeleton human] [--out dir] [--aliases f] [--report-only] [--write-sidecar] <fbx/dir>…` | Compatibility report (missing / extra / renamed bones, hierarchy, proportions, bind deltas, suggested aliases) + `.skm`. Supersedes `validate_rig` |
| `split_parts [--regions a,b,c] [--out dir] <fbx/skm>` | Cuts a full-body mesh into part `.skm` files by bone groups and reports whether they tile it exactly |
| `retarget [--write-sidecar] [--skm out] <fbx>` | Humanoid name mapping + base-pose correction report, alias sidecar, compatible `.skm` |
| `validate_rig` | The old rig check (kept for scripts) |

**Baked files.** `.skel` / `.skm` are optional ship formats.

- During development everything bakes from the FBX through `assets/cache/` (keyed by file hash +
  skeleton hash + aliases + `ANIM_BAKE_VERSION`), so editing a model re-bakes it automatically.
- A `.skm` baked against another skeleton hash is refused at load ("re-run tools/bake_mesh").

## In game

| Command / cvar | |
|---|---|
| `char_inspect 1` | Inspector for the character under the crosshair (or `anim_target`). Panel: parts per slot (option, bone subset, mesh-only bones, triangles, shared GPU part), sockets, virtual bones. World overlay: sockets (yellow), virtual bones (cyan), mesh-only bones (magenta), the selected part's bone subset (green) |
| `char_inspect_slot <slot\|all>` | Select the part whose bone subset is listed / drawn |
| `char_inspect_swap <slot> [next\|prev\|none\|id]` | Swap a part (host; clients only their own) |
| `character_parts [entity]`, `character_set_part <slot> <id\|none> [entity]` | List / set parts |
| `cl_skin <id>` | Your outfit (sent to the host, replicated) |
| `char_preview_show [0\|1\|2\|3] [w h] [yaw_deg] [held item]` | The inventory character preview over the screen (see "Character preview"); 2 renders it without showing it, 3 holds its pose still; reloads `assets/ui/char_preview.json` |
| `char_worn <head\|-> [chest\|-] [back\|-] [slot]` | Worn gear item ids on a player (default your own, `-` / `none` empties a slot), kept every frame; logs each fit (bone, offset) |
| `char_preview_check` | Next frame: the live player's CharacterInst, poses and the render camera before / after the preview call must be bit-identical (PASS / FAIL) |

## Character preview

`chars_preview(w, player_slot, &in)` (characters.h; the inventory screen calls it every frame it shows the
player, docs/INVENTORY.md) renders a second, client-only instance of that player's character into its own
RGBA texture and returns it (0 until ready; nothing runs on frames without a call).

- **The instance** (`src/gameplay/char_preview.c`): a CharacterInst outside `w->chars` (never simulated,
  replicated, hit-tested or drawn by the world passes) with the shown player's definition and parts (only
  read from the live instance; the own player's `cl_skin` / `cl_parts` apply at once) plus `in.worn`. Its own
  CharRuntime plays the controller's idle clip (no foot IK / turn / lean / damage / spine; missing clips fall
  back as the controller does). `in.held_item` goes through the hold system (`proc_hold`, the low ready by
  default: `"held": { "pose": "aim" }` aims it) and is drawn with its attachments. The idle stance's own
  turn is undone once (shoulders and some of the hips square to the camera); `in.yaw` turns it further.
  It stands at the world origin facing -Z; the live player's CharacterInst, pose, item, third-person flag
  and the main camera are never written (`char_preview_check`).
- **The pass** (`src/render/char_preview_view.c`, render.c `render_char_preview_pass`): skinned draws with
  `SkinnedDraw.view = RENDER_VIEW_CHAR_PREVIEW` form skinned pass 4 and are in no other pass; the worn gear and
  the held item are captured into the preview's own static list (`render_char_preview_capture_begin/end`).
  Forward lit (PIPE_SKIN / PIPE_LIT) at `supersample` x the size into a transient RGBA16F + depth, with its own
  frame UBO (the 5th frame set) carrying the studio rig in place of the world's light: key = the sun slot,
  fill = the moon slot (both unshadowed), two rim point lights, hemispheric ambient; no shadows, sky
  environment / SH, cloud shadow, fog, weather, interiors, city windows, spot lights or screen-space inputs.
  The same image at noon, at midnight and in a storm.
- **The resolve** (`shaders/post/char_preview.comp`): fixed exposure, the curve (PBR Neutral / ACES),
  saturation; per output pixel the covered samples (depth < 1) give alpha = covered / s^2 and colour = their
  mean: straight alpha, sRGB-encoded in an RGBA8 target, which is what `gd_image` (shaders/ui.frag, src-alpha
  blending of display-encoded values) expects; empty pixels carry their covered neighbours' colour at alpha 0
  (no dark halo under bilinear sampling). The UI pass of the same frame samples it.
- **Framing** (`char_preview_rig.c`, pure, `tests/test_char_preview.c`): a level camera at chest height. A
  level perspective camera maps heights at the character's plane linearly onto the image, so from the first
  idle pose's skeleton the head top (head-top bone + `crown_m` x scale) lands `head_top` of the image height
  from the top and the bottom edge cuts the legs `bottom_t` of the way from the hip joints to the knees.
  The same camera for every skin; it scales with the character.
- **The look** (`assets/ui/char_preview.json`, reloaded by `char_preview_show`): `framing` (head_top 0.21,
  crown_m 0.05, bottom_t 0.7, fov_deg 28, turn_deg, square), `lights` in camera space (x right, y up, z towards
  the camera: key top-front-left, a cool fill, rim lights behind the shoulders, ambient), `output` (exposure
  0.85, tonemap neutral, saturation 0.85, supersample 2), `held.pose`. Matched to the inventory reference shot:
  dark, low contrast, a soft key, a subtle rim on the silhouette over the blurred backdrop.
- **Cost**: one character at LOD0 into ~900 x 1000 x 2^2 samples plus a 9-tap resolve, only on frames that
  call it.

## Worn gear

`chars_set_worn(w, player_slot, worn[CWORN_COUNT])` (the game calls it every frame: the host from the
inventory, clients from the replicated state) sets the item defs a player wears: `CWORN_HEAD` (helmet),
`CWORN_CHEST` (plate carrier), `CWORN_BACK` (backpack). Each is its detailed mesh (`ItemDef.model`, `.dgim`)
drawn rigidly on a bone of the pose being drawn: in the world for every player (third person, the local
player's shadow and phone cameras: the owner's copy is `MAT_SHADOW_ONLY` + `DrawItem.self`, which phone
cameras draw like the body's `MAT_SELF`), with the first-person body (chest and back only, unshadowed: the
helmet would fill the view) and in the character preview (`CharPreviewIn.worn`).

Placement is in the bind space of the character's body mesh: a per-bone retargeted skin keeps its file bind
(`SkmBone.bind_delta`), so the gear follows the skin's own head and torso, not only the Skeleton's.

- Default fit (no `"worn"` block): an anchor of the item mesh's rest bounds goes to a target measured once
  per body mesh from its LOD0 vertices (by the bone each mostly follows); without a body mesh, the
  Skeleton's bind pose.

| Slot | Bone | Item anchor (item space: helmets face -Z, packs carry their straps on -Z) | Target on the body |
|---|---|---|---|
| head | Head | the crown: bounds top, centre | the skull's top + 20 mm (shell + pads), over the skull's centre |
| chest | Spine2 | the top centre (the shoulder straps) | the shoulders' top (clavicle / Spine2 vertices) + 10 mm, the torso's depth centre (spine + clavicle vertices) 8 mm forward; scaled up (<= 1.25) to the torso's depth + 45 mm so the plates stay outside a deep chest |
| back | Spine2 | the top edge on the body side (bounds min z + 45 mm of straps) | 30 mm below the shoulders' top, on the back's surface (largest z of the spine / clavicle vertices) |

- An item JSON block `"worn": { "bone": "Spine1", "pos": [x, y, z], "rot": [x, y, z], "scale": 1 }` overrides:
  the bone (name, namespace-free), the item origin relative to that bone in the body's model axes at bind
  (m: x right, y up, z back), the rotation (deg, applied y x z); `rot` alone turns the item about its
  fitted anchor.
- `char_worn` logs each fit and the body's measures. None of `helmet`, `plate_carrier`, `backpack`,
  `backpack_hiking`, `sling_bag` needs a `"worn"` block. On the default player skin (Ch32): helmet on Head,
  origin (+0.119 +0.079 -0.040) m from the joint (the helmet mesh is centred 0.119 m off its grip origin);
  plate carrier on Spine2 (0 +0.208 +0.096), scale 1.05; hiking pack on Spine2 (0 +0.135 +0.199). The Dummy's
  box torso still swallows a corner of the front plate in the idle's hunch.

## PBR maps (textured skins)

What the Mixamo skins in `assets/human` contain (`build/bin/fbx_textures` lists every material texture
connection; all maps are embedded PNGs, FBX "Video" content):

| Model | Material | DiffuseColor | NormalMap / Bump | ShininessExponent | SpecularColor |
|---|---|---|---|---|---|
| Ch32 (hazmat) | Ch32_body | 1001_Diffuse 4096² | NormalMap 1001_Normal | 1001_Glossiness (L) | 1001_Specular |
| Ch33 (officer), Ch38 (athlete) | ChNN_body | 1001_Diffuse 4096² | NormalMap 1001_Normal | 1001_Glossiness | 1001_Specular |
| | ChNN_hair | 1002_Diffuse 2048² RGBA (also TransparentColor) | Bump = 1002_Normal (a normal map) | none | none |
| Ch42 (casual) | Ch42_Body | 1001_Diffuse | NormalMap 1001_Normal | 1001_Glossiness | 1001_Specular |
| | Ch42_Hair | 1002_Diffuse RGBA | Bump = 1001_Normal: the body's map (export error, ignored) | none | none |

No AO, emissive or metallic maps; the Dummy has none at all (the zombies and placeholder characters stay
vertex-coloured). The specular maps are a constant sRGB 56 (linear F0 0.04, dielectric) except a few
buckles / zips / rings (hazmat up to sRGB 141).

**Building the atlases.**

1. `build/bin/fbx_textures --out assets/human/textures assets/human/Ch*_nonPBR.fbx` extracts the PNGs and
   writes `<model>.maps.json` (material -> FBX property -> file).
2. `python tools/gen_skin_atlas.py` writes, per skin, tiled like the albedo (1024² per material, `--tile`):
   - `<atlas>.png` albedo (sRGB);
   - `<atlas>_normal.png` (linear): NormalMap, else Bump when that image is a normal map; a map of another
     UDIM tile than the material's diffuse is dropped (flat);
   - `<atlas>_orm.png` (linear): r AO (255: none), g roughness = 1 - glossiness, b metallic from the
     specular colour (KHR_materials_pbrSpecularGlossiness conversion: F0 0.04 -> metallic 0 and the albedo
     unchanged; only metallic texels get the converted albedo);
   - `<atlas>_emissive.png` (sRGB) when a material has an EmissiveColor map.
   It registers them in `data/assets.json` (`"srgb": false` for the linear ones). A tile in `skins.json`
   may name its maps itself (`normal`, `gloss`, `roughness`, `specular`, `metallic`, `occlusion`, `emissive`).
   The generated PNGs are model content (git-ignored, like the extracted textures).

**Checks the tool makes (it prints them).**

- Normal map convention by integrability: a normal map is the gradient of a height field, so its curl
  vanishes only for the right green direction. Counted over 32 x 32 blocks: the body maps are OpenGL (green
  up, e.g. Ch32 802 / 117 blocks), the Ch33 / Ch38 hair maps DirectX (167 / 126, 126 / 40) and are flipped.
  The engine convention is glTF / OpenGL with the FBX v flipped at load (image up = -v, `pbr_normal`).
- Glossiness, not roughness: the ShininessExponent map is bright on skin / hard hat and dark on cloth;
  roughness means 0.62 .. 0.83.
- sRGB vs linear: albedo / emissive sRGB, normal / ORM UNORM.

**At run time** `gpu_part_for` looks the atlases up by name (`<atlas>_normal`, `_orm`, `_emissive`); a
skin without them draws as before. `draw_part` sets the maps with factors 1 (wet characters still gloss:
roughness factor 0.37 when soaked). The skinned shaders sample them (docs/PBR.md "Skinned characters").
`r_char_maps 0` turns them off (A/B), `r_char_normal_scale` scales the normal maps.

Cost (RTX 3060 Ti, 1600x900, five textured characters near the camera, `r_char_maps` 1 vs 0 alternated):
GPU 4.32 vs 4.27 ms with vgeo, 3.18 vs 3.53 ms classic, both within the run-to-run noise. Memory: one
normal + one ORM RGBA8 atlas per skin (2048 x 1024 with mips: 11 MB each for two-material skins).

## Networking

- The character id and the **part selection** travel in the replicated anim state
  (`ANS_PARTS`, protocol 13): a u16 mask of non-empty slots plus one byte per such slot, sent only
  when it changes. A zombie with four parts costs 6 bytes once.
- Clients build identical characters from it. Pose replication stays skeleton-level and is
  unchanged.
- A client's `cl_skin` goes to the host as `RPC_SET_PART` (body slot). The host validates it, and
  everyone sees the outfit.
- The skin list and character files are part of the asset manifest, so mismatched installs are
  refused at join.

## Tests

`tests/test_char_preview.c` (ctest `char_preview`): the preview framing (head top and image bottom for any
character scale, the chest-height camera, the lens), the JSON over the defaults, the shipped look parses.
`tools/tests/char_preview.cfg` + `char_preview_check.py`: the first-person view unchanged while the preview
renders, the preview identical at noon / midnight / in a storm, the live player untouched
(`char_preview_check`), gear in the preview (front / side / back), a held rifle, gear on a teammate and on the
own body in third person.

`tests/test_characters.c` (ctest `characters`):

- deterministic skeleton bake and round trip;
- subset palette gather equals the full evaluation;
- equal bone tables gather equal palettes (`skm_palette_equal`: what the parts of a pose share);
- a part skins exactly like the same vertices on the full body;
- mesh-only leaves follow their parent, and the leaf override only moves the leaf;
- aliases resolve renamed bones;
- UE / Unity / Mixamo name mapping;
- base-pose correction is identity on an identical rig;
- a re-parented bone is rejected with a clear message;
- `.skm` round trip is byte-identical;
- the placeholder regions tile the body;
- several parts mean one evaluation and one palette per tick;
- a mid-animation swap leaves the pose bit-identical;
- hide masks work;
- 200 modular zombies batch within parts × LODs;
- part selection replication is small and deterministic;
- mesh-local and skeleton sockets.

`tests/test_skin_reference.c` (ctest `skin_reference`) re-reads every character FBX with ufbx and skins it
on the CPU with the file's own weights (all influences) and inverse binds on 36 strong-bend poses, then
compares per vertex: the legacy data (baked vertices x `skm_gather_palette`), the same with the file's
binds (retarget only: < 0.5 mm), the vgeo build of the vertices (weld + skin per position: < 2.5 mm off the
legacy skin), weight sums, and a pose 300 m from the origin.

The hand glue, ragdoll and hitbox suites (`anim`, `ragdoll`, `flesh`) cover the rest and are
unchanged by this refactor.
