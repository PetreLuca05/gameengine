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

`tests/test_characters.c` (ctest `characters`):

- deterministic skeleton bake and round trip;
- subset palette gather equals the full evaluation;
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
