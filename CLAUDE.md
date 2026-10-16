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
| Render post / sky / time | RENDERING_POST.md, DAYNIGHT.md | src/render/{post,sky,graph}, game_post.c, game_daynight.c |
| Engine Fidelity (Off .. Modern, bounds every graphics setting) | ENGINE_FIDELITY.md | assets/render/fidelity, src/render/fidelity, src/game/game_fidelity.c, scr_settings.c; tests: test_fidelity, tools/tests/run_fidelity_tests.py |
| VFX / flesh | VFX*.md | src/vfx, src/gameplay/flesh_fx.c, assets/vfx |
| UI | UI.md | src/ui/gui, src/ui/screens, assets/ui |
| Zombies | ZOMBIES.md | src/gameplay/zombies |
| Weather | WEATHER.md | src/world/weather.c (pure), src/game/game_weather.c, shaders/weather.glsl |
| Vehicles | VEHICLES.md | src/physics/phys_vehicle.c, src/gameplay/vehicle_def.c (pure), src/game/game_vehicles.c |
| Light shadows | LIGHT_SHADOWS.md | src/render/vk_light_shadow.c, common.glsl point/spot_light_shadow |
| Virtualized geometry | VGEO.md, VGEO_PLAN.md | src/render/vgeo, src/render/vk_vgeo.c, shaders/vgeo, tools/{build,bench}_vgeo |
| no-nanite classic renderer (this branch: vgeo off) | NO_NANITE.md | src/render/vk_classic.c, shaders/classic_*, tools/tests/perf_compare.cfg |
| Phone + companion page | PHONE.md | src/gameplay/phone, src/net/http_ws, src/render/phone_view.c, web/phone; e2e: tools/tests/phone_e2e.sh |
| Hand poses + editor (`scene handpose`) | HANDPOSE.md | src/anim/handpose.c, src/editor/handpose, src/game/game_scene.c; test: tools/tests/hp_scene.cfg |
| Open-world map (`map zombieland`) | ZOMBIE_MAP.md | src/world/{mapgen,mapgen_poi,openworld,openworld_foliage}.c, data/levels/zombieland.json, data/loot.json; tools/mapgen_preview, tools/tests/zl_*.cfg |

## Dev room (data/levels/devroom.json)
Hall at the origin. Other areas:
- Horde yard: (0, 0, 50).
- Weather yard: (−50, 0, 22).
- Vehicle yard: (−140, 0, 0).
- Sighting lane: east of the hall.
- Flesh range: north.
- Post corner: south-west.

Console: `help [filter]` lists commands, `cvarlist [filter]` lists cvars.
