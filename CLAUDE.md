# Deadground — agent notes (keep terse; details live in docs/*.md, read only what the task needs)

C11 first-person zombie co-op. SDL3, Vulkan 1.3, Jolt (joltc), ENet, miniaudio, own UI (src/ui/gui).
Windows, no admin; tools are portable, in `.toolchain/` (git-ignored).

## Build / run
- `.toolchain/b.sh`: build `build/` (prints errors + `build exit=N`).
- LNK1168 means the user's game is running. Use `.toolchain/ba.sh` (`build-alt/`) and never kill their process.
- Unit tests: `cd build && ../.toolchain/cmake_x/cmake-4.4.4-windows-x86_64/bin/ctest.exe`.
- Script test: `cd build/bin && ./deadground.exe --test --nosound --log <path> --script <cfg>`.
  - `--log` avoids clobbering the user's log; `--test` skips config load/save.
  - `wait N` counts frames: set `r_maxfps 60`. `screenshot <png>` needs `wait 2` after it.
- Validation: `export VK_ADD_LAYER_PATH="C:\C\Game\.toolchain\vvl\install\bin"` + `--validation`.
- Two machines: host run in background, client with `+connect 127.0.0.1` before `--script`. See `tools/tests/*_net.sh`.
- Assets are copied into `build*/bin` at build time: rebuild after JSON edits.

## Rules
- Work autonomously, verify (tests / screenshots), then commit + build. Branch `main`.
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Never commit `.env` (Higgsfield key), `assets/human/*.fbx` or `assets/human/textures/` (git-ignored). Stage paths selectively.
- Protocol changes (`PlayerSim`, `UserCmd`, `GameEvent`, snapshot layout) bump `DG_PROTOCOL_VERSION` (`src/net/protocol.h`).
- Match the surrounding style: comment density, naming, `DG_*` macros, one `g_engine`.

## Editing gotchas
- Multi-line edits: a Python script written with the Write tool to the scratchpad. It does a `str.replace` with an assert, and opens files with `newline=''` (no CRLF).
- Bash heredocs break on apostrophes inside quoted C.
- clang-format may reflow files on save: re-read before editing again.
- Console commands queued with `cbuf_add` from in-game test code run only after the script's current `wait`. Call functions directly.
- `gd_text*` y is the text top. HUD style lives in `src/ui/hud.c`: caps labels, pill bars, theme colours via `gui_theme()`.

## Map (doc → code)
| Area | Doc | Main code |
|---|---|---|
| Animation / net / damage | ANIMATION*.md, DAMAGE.md | src/anim, src/gameplay/characters.c |
| Skeleton / meshes / modular characters | CHARACTERS.md | src/anim/skeleton.c, src/render/skeletal_mesh.c, src/assets/character.c, tools/bake_* |
| FP camera, hold point | CAMERA_HOLDPOINT.md | src/player/{holdpoint,hands,camera_fp}.c |
| Weapons, recoil, sway | RECOIL.md, ATTACHMENTS.md | src/gameplay/{weapons,recoil,items}.c, data/weapons |
| Fire animation (gun-driven kick, mass model, hands / camera lag, punches; ONE global sway) | FIRE_ANIM.md | src/anim/fire_anim.c (pure), src/game/game_fire.c, src/player/sway.c, src/ui/fire_tuner.c, assets/weapons/{fire_poses,sway_global.json}; tests: test_fire_anim, tools/tests/fire_anim.cfg, sway_check / fire_compare / fire_bench |
| Ballistics (projectiles, drag / wind / ricochet / penetration, crack / report timing, composed muzzle flash, tracers, energy impacts, range `bal_range`) | BALLISTICS.md | src/gameplay/ballistics (bal_sim / bal_surface / bal_acoustics / bal_muzzle / bal_tracer pure; bal_world, bal_fx), assets/ballistics, attachment `muzzle_device` blocks; tests: test_ballistics, tools/ballistics_ref.py, tools/tests/bal_range.cfg |
| Render post / sky / time | RENDERING_POST.md, DAYNIGHT.md | src/render/{post,sky,graph}, game_post.c, game_daynight.c |
| glTF PBR maps (metal-rough, normal, AO, emissive, MASK, doubleSided) | PBR.md | src/assets/assets.c gltf_material, shaders/pbr_maps.glsl, render.c draw_items (set 3), vg_resolve.glsl; test: tools/gen_pbr_test.py, tools/tests/pbr_shelf.cfg |
| Engine Fidelity (Off .. Modern, bounds every graphics setting) | ENGINE_FIDELITY.md | assets/render/fidelity, src/render/fidelity, src/game/game_fidelity.c, scr_settings.c; tests: test_fidelity, tools/tests/run_fidelity_tests.py |
| VFX / flesh | VFX*.md | src/vfx, src/gameplay/flesh_fx.c, assets/vfx |
| UI | UI.md | src/ui/gui, src/ui/screens, assets/ui |
| Zombies | ZOMBIES.md | src/gameplay/zombies |
| Weather | WEATHER.md | src/world/weather.c (pure), src/game/game_weather.c, shaders/weather.glsl |
| Foliage wind (Crysis vegetation WPO, wind field, gust tile, fidelity tiers) | FOLIAGE_WIND.md | src/world/wind.c (pure), src/render/vgeo/foliage_wind.c, shaders/vgeo/wpo.glsl, src/game/game_wind.c, Tree It wind channels in src/render/vgeo/vgeo_tree.c; tests: test_wind, vgeo_foliage, tools/tests/wind.cfg |
| Vehicles | VEHICLES.md | src/physics/phys_vehicle.c, src/gameplay/vehicle_def.c (pure), src/game/game_vehicles.c |
| Light shadows | LIGHT_SHADOWS.md | src/render/vk_light_shadow.c, common.glsl point/spot_light_shadow |
| Sun / moon shadows driven by the sky (softness, contrast, overcast cutoff, PCSS, cloud shadows, moon tile) | SHADOWS.md | src/render/shadow_sky.c (pure), src/game/game_shadow_sky.c, lit_body.glsl sh_tile, assets/render/shadows_sky.json; tests: test_shadow_sky, tools/tests/shadow_ref.py (dev room corner (-50, 0, -11)) |
| Virtualized geometry | VGEO.md, VGEO_PLAN.md | src/render/vgeo, src/render/vk_vgeo.c, shaders/vgeo, tools/{build,bench}_vgeo |
| Classic renderer (`r_vgeo 0`, runtime switch; Settings > Graphics) | CLASSIC_RENDERER.md | src/render/vk_classic.c, shaders/classic_*, `classic_stats`; perf: tools/tests/perf_compare.cfg |
| Phone (equip = pairing, HUD phone panel, phone OS: portal / home / camera / gallery; the model shows no screen) | PHONE.md | src/gameplay/phone (phone_os.c pure), src/net/http_ws, src/render/phone_view.c, web/phone, assets/phone; tests: test_phone, tools/tests/phone_{flow,ws,screen}.py, phone_e2e.sh, phone_net.sh |
| Hand poses: universal grips + cases, rig editor (`scene handpose`, frozen world) | HANDPOSE.md | src/anim/{handpose,weapon_hold}.c, src/editor/handpose (hp_rig, hp_xform, hp_case, hp_editor), src/game/game_freeze.c, assets/handposes/{items,cases,styles}; tools/handpose_migrate; tests: test_handpose(_rig), test_hp_edit, tools/tests/hp_scene.cfg, hp_gameplay.cfg |
| Surface textures (terrain, roads, buildings, grass/wheat; `r_surfaces`) | TEXTURES.md | src/render/surface_tex.c, shaders/surfaces.glsl, assets/textures/{surfaces,foliage}, tools/vfxgen/{texgen,foliage_atlas}.py; tests: tools/tests/tex_*.cfg |
| Ground look target (reference analysis, blade sheet, target spec, reference viewer `ground_ref*`, match report) | GROUND_REFERENCE.md | docs/reference/ground, tools/ground_ref.py, tools/ground_match.py, src/game/game_ground_ref.c; ground texture generation: tools/vfxgen/groundgen.py (`groundgen.bat` with the key), recipes assets/{foliage,terrain}/gen; tests: ground_recipes, ground_textures (tools/tests/ground_tex_check.py) |
| Terrain shading (hex bombing, macro, variants, parallax, scatter, per-pair transitions, `r_terrain_quality`, `terrain_*`, map `terrain_yard`) | TERRAIN_SHADING.md | assets/terrain, src/world/terrain (params / maps / blend pure; terrain_world), src/render/terrain_tex.c, shaders/terrain, src/game/game_terrain.c; tests: test_terrain, tools/tests/terrain_strip.cfg |
| Grass / ground cover assets (JSON clumps on atlas cells, MAT_COVER shading, fades per level, shrubs / ferns / reeds) | FOLIAGE_GRASS.md | assets/foliage/grass, src/render/vgeo/grass_asset.c, shaders/cover.glsl, src/render/ground.c, openworld_foliage.c grass_cover; tests: test_grass |
| Main menu, lobby, seasons, cosmetics, profile (`--menu` in `--test` runs) | MENU_LOBBY.md | src/ui/mainmenu, src/net/{lobby,lobby_net}.c, src/world/{maplist,season}.c, src/gameplay/cosmetics.c, src/game/profile.c, assets/{maps,cosmetics}, assets/world/seasons.json; tests: test_lobby, tools/tests/mm_nav.cfg, lobby_lan.sh |
| Spectating (first-person in-eye, `spectate`) | SPECTATE.md | src/net/spectate{,_core}.c, src/player/spectator.c, game_view_slot (game.c); tests: test_spectate, tools/tests/spec_net.sh |
| Radial wheels (heal / throw / ping / door), timed ring, door peek, throwables, kill counter + feed, compass, pings, HUD settings, pause Commands tab | UI_WHEELS_HUD.md | src/ui/widgets, src/ui/hud, src/ui/pause, src/game/{game_wheels,game_pings,game_peek}.c, src/gameplay/{ping,throwables}.c, assets/ui/wheels, assets/ui/command_categories.json; tests: test_hud, tools/tests/lint_commands.py, tools/tests/hud_{wheels,commands}.cfg |
| Downed / revive, death sequence, corpses + DNA syringe, Fabricator reconstruction (`spawn_teammate`, `down`, `die`, `reconstruction_preview`) | REVIVE.md | src/gameplay/revive (revive_core / death_seq pure), src/game/{game_death,game_revive}.c, shaders/recon.glsl, src/audio/death_synth.c, assets/gameplay/revive_rules.json; tests: test_revive, tools/tests/revive_loop.cfg, revive_net.sh |
| Open-world generator (`map vegetation`, `map wateryard`; the Zombieland map lives on branch `zombieland`) | ZOMBIE_MAP.md | src/world/{mapgen,mapgen_poi,openworld,openworld_foliage}.c, data/levels/{vegetation,wateryard}.json, data/loot.json; tools/mapgen_preview |

## Dev room (data/levels/devroom.json)
Hall at the origin. Other areas:
- Horde yard: (0, 0, 50).
- Weather yard: (−50, 0, 22).
- Vehicle yard: (−140, 0, 0).
- Sighting lane: east of the hall.
- Flesh range: north.
- Post corner: south-west.
- Shadow reference corner: (−50, 0, −11), switches jump to `shadow_preset`s.

Console: `help [filter]` lists commands, `cvarlist [filter]` lists cvars. New commands / cvars need a description and a category (assets/ui/command_categories.json; tools/tests/lint_commands.py).
