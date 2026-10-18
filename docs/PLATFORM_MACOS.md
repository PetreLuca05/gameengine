# macOS (Intel x86_64 + Apple Silicon arm64, Vulkan via MoltenVK)

Status: ported.

> **Engine change during the port.** `main` became classic-only (commit `9fce3a1`, "Remove the
> virtualized geometry renderer"). The nanite / vgeo GPU path now lives on branch `nanite`.
> * Everything vgeo-specific below (sections 1.4, 4, 5 and 9: the 32-bit two-phase visibility
>   writes, vertex pulling instead of mesh shaders, the vgeo harness on MoltenVK) is on
>   **`claude/nanite-macos`**. That branch is `nanite` plus this whole port, verified green on all
>   four CI platforms, including the vgeo raster tests on the macOS runner's Apple GPU through MoltenVK.
> * On `main`, the classic instanced renderer draws everything. It needs none of the vgeo features,
>   so every Mac GPU family, Intel included, runs the same renderer.
* Section 1 is the gap analysis written before any change.
* Sections 2-8 describe what was built.
* Section 9 says what has actually been run, and where.
* Section 10 lists what remains for hardware.

## 1. Gap analysis

Sources: an audit of `CMakeLists.txt`, `src/render` (device, swapchain, frame graph, vgeo, classic,
post), every shader under `shaders/`, `src/core`, `src/net`, `src/audio`, `third_party/*_shim`, and
MoltenVK's own feature reporting (`MoltenVK/GPUObjects/MVKDevice.mm`, main branch of 2026-10-13,
MoltenVK 1.4.x), plus SPIRV-Cross run over all 141 compiled shaders
(`tools/macos/msl_check.py`).

GPU families used below:
* **Apple7**: M1 family.
* **Apple8**: M2.
* **Apple9**: M3 / M4 and later.
* **Mac2-Intel**: Intel UHD / Iris iGPUs on Intel Macs.
* **Mac2-AMD**: Radeon Pro / RX dGPUs on Intel Macs.

All are reached through Metal 3 (macOS 13+) or Metal 2.x (macOS 11-12).

### 1.1 Vulkan instance / device

| Item | Engine today | MoltenVK | Gap / action |
|---|---|---|---|
| Loader | `volkInitialize()` (dlopen `libvulkan.dylib`); SDL loads its own library for `SDL_Vulkan_CreateSurface` | SDL3 on macOS prefers `@executable_path/../Frameworks/libMoltenVK.dylib`, then `libvulkan.1.dylib` | **Blocker.** volk and SDL can end up with different libraries, giving an instance from one ICD and a surface call into another. Drive volk from `SDL_Vulkan_GetVkGetInstanceProcAddr()`, so both use one library (every platform). |
| `VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` | not requested | required through the Khronos loader (≥ 1.3.216), else `vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER` | **Blocker.** Enable it when the loader lists it. Direct MoltenVK (the bundle) does not list it, which is fine. |
| `VK_KHR_portability_subset` | not enabled | always advertised | **Blocker** (spec: must be enabled). Enable it and log the subset features. The engine uses none of the restricted ones; see 1.3. |
| apiVersion | devices `< 1.3` skipped | 1.3 since MoltenVK 1.3.0 (May 2025) | Require the MoltenVK of Vulkan SDK ≥ 1.4.309. Log a clear message when only a 1.2 MoltenVK is found. |
| dynamicRendering, synchronization2, demote | required (1.3 core) | supported | none |
| `VK_KHR_push_descriptor` | required by the post stack | supported | none |
| `VK_EXT_swapchain_colorspace` | optional (HDR10) | supported. HDR10_ST2084 maps to `CAMetalLayer` PQ with EDR. | HDR10 path works as-is on EDR displays. Extended-sRGB (scRGB) is not used, so no gap. |
| `VK_EXT_mesh_shader` | optional (vgeo HW raster) | **not exposed** | Vertex-pulling path is already automatic (`vgeo_raster.c`). Add a cvar to force it elsewhere for testing (`r_vgeo_mesh`). |
| `VK_EXT_dynamic_rendering_unused_attachments` | optional | not exposed | Existing fallback: velocity always written. |
| `VK_EXT_device_fault` | optional | not exposed | none |
| Present modes | FIFO; IMMEDIATE, then MAILBOX with vsync off | FIFO, IMMEDIATE (CAMetalLayer `displaySyncEnabled = NO`), MAILBOX not exposed | none (IMMEDIATE preferred already) |
| Swapchain extent | `currentExtent`, else `SDL_GetWindowSizeInPixels` | `currentExtent` = drawable size in pixels (Retina) | none. The window has `SDL_WINDOW_HIGH_PIXEL_DENSITY`. |
| Pipeline cache | none (every `vkCreate*Pipelines` passes `VK_NULL_HANDLE`) | the cache stores the SPIRV-Cross output (MSL) | **Startup cost.** On MoltenVK every pipeline re-runs SPIRV-Cross plus the Metal front end on each launch. Add a persistent `VkPipelineCache` in the pref dir (helps every platform). |
| Shader hot reload | on by default; `system()` with a compiler path baked at build time | — | Off when the source dir / compiler is absent (bundle). It already checks the path; make it explicit for bundles. |
| Timestamps | `timestampPeriod` used everywhere | Apple GPUs sample only at encoder (stage) boundaries; MoltenVK reports a period of 1 ns, low-pass filtered | Pass-level numbers stay meaningful. Writes inside one render pass collapse, so the vgeo HW / SW split shows 0 / total. Document it; the profiler is unchanged. |

### 1.2 Shader features (SPIRV-Cross → MSL)

`tools/macos/msl_check.py` on the Linux build: **134 / 141 translate**. The 7 failures are exactly
the visibility-buffer writers (`vg_raster_hw*.frag`, `vg_raster_mesh*.frag`, `vg_raster_sw*.comp`):
"MSL currently does not support 64-bit atomics".

| Feature | Use | MoltenVK / Metal | Action |
|---|---|---|---|
| 64-bit buffer atomicMax (vgeo visibility buffer) | 5 writers, fragment and compute | **`shaderBufferInt64Atomics = false` on every device** (hard-coded in MVKDevice.mm). Metal 3.1 has 64-bit `atomic_max` on Apple8+ for textures only, which MoltenVK does not map. | **Blocker for vgeo.** vgeo turns itself off today, so the Mac would always get the classic renderer. Add a **split depth / payload fallback** with two 32-bit atomic passes (1.4), selected per device. Bit-identical to the 64-bit result. |
| `shaderInt64` (SW raster edge functions, vis buffer loads) | vgeo | true on all Metal 2.3+ devices | none |
| Bindless `sampler2D vg_tex[1152]`, `nonuniformEXT` | vgeo, all stages | Needs Metal argument buffers. Tier 2 (Apple7+, Mac2-AMD on macOS 13+) gives up to ~500k textures and `maxArgumentBufferSamplerCount` *unique* samplers (vgeo uses about 4). Tier 1 (Mac2-Intel) has `arrayOfSamplers`, but the MoltenVK limits are 16 samplers / 128 textures per stage. | Gate vgeo on `shaderSampledImageArrayNonUniformIndexing`, and on per-stage update-after-bind sampled-image limits ≥ 1152. Mac2-Intel falls back to classic. |
| Subgroups | `subgroupBroadcastFirst` only (vg_raster_sw) | supported (simdgroup). Sizes: Apple 32, AMD 32/64, Intel 8-32. | No size assumptions anywhere (audited). none |
| Shared memory | largest `env_sh.comp` ≈ 9.5 KB; everything else ≤ 4.5 KB | 32 KB | none |
| Mesh shaders | `vg_raster_hw*.mesh` | not exposed | vertex pulling (exists) |
| Geometry / tessellation / transform feedback / `gl_PointSize` / `interpolateAt*` / `gl_SampleID` / input attachments | none | — | none (audited) |
| Formatless storage writes (5 post shaders) | `shaderStorageImageWriteWithoutFormat` | true | none |
| Writable SSBOs in vertex stage | vgeo | `vertexPipelineStoresAndAtomics` true | none |
| `scalar` block layout | vgeo | true | none |
| `precise` (water) | Gerstner waves | SPIRV-Cross emits `precise` → fast-math off for those expressions | Verify on device with the water determinism test. |
| `invariant gl_Position` | 4 shaders | supported (`[[invariant]]`, MSL 2.1+) | none |

### 1.3 Portability subset (`VkPhysicalDevicePortabilitySubsetFeaturesKHR`)

| Subset feature | Engine use |
|---|---|
| triangleFans | none (TRIANGLE_LIST everywhere, LINE_LIST for debug lines) |
| pointPolygons | none (FILL, LINE for wireframe debug only) |
| constantAlphaColorBlendFactors | none |
| imageViewFormatSwizzle | none (identity swizzles) |
| imageViewFormatReinterpretation | none |
| events | none |
| samplerMipLodBias | none (0) |
| separateStencilMaskRef / mutableComparisonSamplers | no stencil, no comparison samplers |
| vertexAttributeAccessBeyondStride | none: vgeo pulls vertices from SSBOs, the classic path uses tight strides |
| multisampleArrayImage / imageView2DOn3DImage | none |
| shaderSampleRateInterpolationFunctions | none |
| tessellation* | none |
| `wideLines` | not enabled (lineWidth always 1) |

These are logged at device creation, so a regression shows up in the log.

### 1.4 vgeo on Metal: visibility buffer without 64-bit atomics

Today each pixel holds a u64: `(floatBits(1 - z) << 32) | payload`, and `atomicMax` is the depth test.

The fallback keeps exactly that memory layout: the same 8 bytes per pixel, viewed as two u32 words
(payload, depth key). Each raster pass then runs twice:
1. **depth**: `atomicMax(word[2i+1], depthkey)` from every HW / SW rasteriser.
2. **payload**: the same rasterisers again; where `depthkey == word[2i+1]`, `atomicMax(word[2i], payload)`.

The result per pixel is max-depth first, then max payload among equal depths. That is the u64
`atomicMax` result exactly, so the resolve, Hi-Z, debug views and every reader stay untouched, and the
output is bit-identical. The cost is a second raster pass (vertex / triangle setup twice, about +40-60%
raster time). Selection: `r_vgeo_vis32` 0 auto (fallback only without `shaderBufferInt64Atomics`),
1 always (testing on any GPU).

### 1.5 Textures / formats

| Item | Engine | Metal | Action |
|---|---|---|---|
| BC1-7 | No BC content is shipped today. KTX2 passes its `vkFormat` straight through, with no support check. | **Supported on all Apple Silicon Macs** (`supportsBCTextureCompression`, Apple7+ on macOS 11+) and on all Intel Macs. ASTC is supported on Apple7+ and not on Intel/AMD. | The brief's premise ("BC unsupported on Apple Silicon") applies to iOS, not macOS. No ASTC transcode is needed for the Mac. Add a format-support check to the KTX2 loader with a clear error, so a future BC or ASTC asset cannot crash an unsupported device. |
| Depth | D32_SFLOAT only, no stencil | supported (D24S8 is not, and is unused) | none |
| Storage formats | rgba16f, r32f, rg16f, r16f, r8, rgba8 | all readable / writable on Apple7+ and Mac2 (r8 write needs Apple / Mac2) | none |
| Swapchain | B8G8R8A8 / R8G8B8A8 UNORM, A2B10G10R10 for HDR10 | supported | none |

### 1.6 TBDR (Apple GPUs)

The frame graph places barriers but not load / store ops; each pass picks its own.
* There are 26 STORE_OP_STORE, a few of which could be DONT_CARE.
* No `TRANSIENT_ATTACHMENT` usage.
* No lazily allocated memory.

Transients are aliased by lifetime and always get `SAMPLED | TRANSFER` usage, so none of them can be
memoryless as written. The candidates are attachments written and never read after their pass.
Action: audit the store ops of the main passes (G-buffer depth, the viewmodel, UI), and use
DONT_CARE where nothing reads the result. Memoryless is left for when the graph knows an
attachment-only lifetime (documented as follow-up).

### 1.7 Platform code

| Area | Today | Gap / action |
|---|---|---|
| Threads, timers, paths, file info | All SDL3 (`SDL_GetPerformanceCounter` = `mach_absolute_time` on macOS, `SDL_GetPrefPath`, `SDL_CreateThread`). No `/proc`, epoll, affinity or Linux-only API. | Worker threads get a QoS class (`pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)`); macOS has no affinity API. |
| SIMD | **None hand-written** in `src/` or the shims. Jolt picks NEON on arm64, SSE4.2 on x86_64. minih264 picks NEON. | none. Jolt's x86 options (AVX2 / LZCNT / F16C …) must be off for the x86_64 slice targeting macOS 11 hardware (see 1.8). |
| Crash handling | Windows only (DbgHelp + watchdog). POSIX is a no-op. | Add a POSIX `sigaction` handler plus `backtrace()` into the log (macOS and Linux). macOS also writes its own `.ips` crash report. |
| Writable dirs | log / config / saves / screenshots go to `SDL_GetPrefPath` = `~/Library/Application Support/Deadground/Deadground/`. Several dev paths write under the base dir: anim cache, HDR screenshots, icon bake, handpose editor, shader hot reload. | Inside a signed `.app`, `SDL_GetBasePath` is `Contents/Resources/` and read-only. Redirect the anim cache and HDR screenshots to the pref dir when the base dir is not writable. The dev-only editors stay as they are (they run from a build tree). |
| Sockets | phone HTTP/WS server: POSIX path exists, but `send(…, MSG_NOSIGNAL)` | Darwin's SDK has no `MSG_NOSIGNAL`. Use `SO_NOSIGPIPE` on the socket plus `signal(SIGPIPE, SIG_IGN)`. |
| LAN discovery | ENet broadcast to 255.255.255.255 | macOS 15+ local-network privacy prompt. Denial makes `sendto` fail with `EHOSTUNREACH`/`EPERM`. Log once with the reason. `NSLocalNetworkUsageDescription` goes in Info.plist. |
| Audio | miniaudio, backend auto → CoreAudio | Static link needs `-framework CoreAudio AudioToolbox CoreFoundation` (miniaudio runtime-links by default, so this is a safety net only). |
| Steam | CMake assumes non-Windows = Linux (`linux64/libsteam_api.so`) | Use `redistributable_bin/osx/libsteam_api.dylib` (universal), bundled in `Contents/Frameworks`, rpath `@executable_path/../Frameworks`, `steam_appid.txt` next to the executable. `+connect_lobby` already comes through argv. |
| App Nap | — | `NSAppSleepDisabled` in Info.plist, plus `NSProcessInfo beginActivity` is not needed for a foreground game. |
| Retina | `SDL_WINDOW_HIGH_PIXEL_DENSITY`; render size from `SDL_GetWindowSizeInPixels`; UI scales mouse by pixel density | none (audited). `r_scale` stays relative to the backing pixels. |
| Input | SDL3 (GameController via SDL's MFi / GCController backend), relative mouse | none |
| Process spawning | `system()` only for dev shader recompile | gated (1.1) |

### 1.8 Build

* No `APPLE` branch in CMake. Missing:
  * frameworks;
  * bundle;
  * rpath;
  * MoltenVK copy;
  * Info.plist;
  * the Steam dylib path;
  * the deployment target.
* Jolt: joltc builds Jolt with its own CMake. For `CMAKE_OSX_ARCHITECTURES` with two values, Jolt's x86 options are not compatible with one universal compile. Each arch is configured and built separately, then `lipo`'d (`tools/macos/build_universal.sh`). One build tree per arch is also the fast single-arch dev path.
* Minimum OS: **macOS 11 (x86_64) and macOS 11 (arm64)**.
  * MoltenVK needs 10.15+ and SDL3 needs 10.13+, so the floor comes from the GPU side.
  * Metal argument buffers tier 2 and the 1.3 MoltenVK features work on 11, and every Apple Silicon Mac shipped with 11+.
  * Picking 12 for arm64 buys nothing the engine uses, so both slices share 11.
  * Apple7 (M1) is the minimum Apple GPU.
* Determinism:
  * Ballistics and water are built with `-ffp-contract=off`, but the rest of the deterministic set is not: animation core, recoil, spring, fire anim, aim.
  * arm64 clang fuses `a*b+c` by default, so it would diverge from x86_64 / MSVC.
  * Fix: `-ffp-contract=off` for every target on clang / gcc.
  * `sinf` / `cosf` / `expf` differ between libms (Apple libm, glibc, MSVC CRT). See the determinism section for the audit and the cross-platform golden test.
* Shaders: `glslc` / `glslangValidator` from Homebrew or the Vulkan SDK, unchanged. MSL is produced at runtime by MoltenVK. `msl_check.py` is the CI gate. With `--metal` on macOS it compiles each MSL file to AIR, the step MoltenVK otherwise only hits at pipeline creation.

### 1.9 What cannot be verified from this repository's CI

The following are listed in the manual checklist (section 6):
* GitHub's macOS runners are VMs. arm64 runners expose a paravirtual Metal device; Intel runners expose none.
* Real-GPU image diffs, perf numbers per family, the Steam overlay, an iPhone pairing, EDR output and the LAN Mac↔PC session all need hardware.

## 2. Setup and build

Requirements:
* macOS 13+ to build. The game runs on macOS 11+.
* Xcode command line tools: Apple clang, `lipo`, `codesign`, `xcrun metal`.
* CMake 3.24+ and Ninja.
* A GLSL compiler (`glslc` or `glslang`).
* MoltenVK. Either the LunarG Vulkan SDK for macOS (universal MoltenVK, loader, validation layers), or Homebrew:

```sh
brew install cmake ninja glslang spirv-cross molten-vk vulkan-loader   # host-arch MoltenVK
```

Single-architecture developer build (the fast path; the host architecture by default):

```sh
cmake -S . -B build-mac-arm64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-arm64.cmake
cmake --build build-mac-arm64
cd build-mac-arm64/bin && ./deadground                  # SDL finds MoltenVK / the loader itself
ctest --test-dir build-mac-arm64                         # headless unit tests
```

Where Vulkan comes from:
* SDL loads `@executable_path/../Frameworks/libMoltenVK.dylib`, then `libvulkan.1.dylib`, then `libMoltenVK.dylib`.
* volk is driven from the same library (`vkd_create_device`).
* To use the Khronos loader in a build tree (validation layers), point SDL at it:

```sh
export SDL_VULKAN_LIBRARY=$(brew --prefix)/lib/libvulkan.1.dylib
export VK_ICD_FILENAMES=$(brew --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json
./deadground --validation
```

On the nanite branch, the window-less vgeo test harness (`test_vgeo_*`, `bench_vgeo`) uses volk's own loader search. Set
`DYLD_FALLBACK_LIBRARY_PATH=$(brew --prefix)/lib` and `VK_ICD_FILENAMES` as above.

Universal build (on Apple Silicon; the x86_64 tree runs its build-time vgeo tools under Rosetta):

```sh
tools/macos/build_universal.sh Release -DDG_DISTRIBUTION=ON    # build-mac-{arm64,x86_64} -> build-mac-universal/bin
ARCHS=arm64 tools/macos/build_universal.sh RelWithDebInfo       # one slice through the same script
```

Notes:
* One architecture per build tree is enforced. Jolt's SIMD flags are chosen per architecture:
  * NEON on arm64.
  * SSE4.2 + AVX + F16C on x86_64, with AVX2 / BMI / LZCNT / FMA off because macOS 11 still runs on Ivy Bridge (Mac Pro 2013).
* `DG_DISTRIBUTION=ON` leaves out the source-tree paths, so there is no shader hot reload and no `DG_SOURCE_DIR` fallback in a shipped build.

Tools: the Python tools run with the system / Homebrew `python3`. `.env` (Higgsfield key) is read from
the repository root as on Windows. The `.bat` launchers have shell equivalents where they matter
(`tools/macos/*.sh`). `.toolchain/` is the Windows portable toolchain and is not used on macOS.

## 3. App bundle, signing, notarization

```sh
tools/macos/make_bundle.sh build-mac-universal/bin dist --moltenvk $VULKAN_SDK/macOS/lib/libMoltenVK.dylib \
    [--bundle-id com.studio.deadground] [--version 0.1.0] [--build 42] [--steam-appid 480]
tools/macos/sign.sh dist/Deadground.app                         # ad-hoc (dev / CI)
DG_SIGN_IDENTITY="Developer ID Application: Studio (TEAMID)" tools/macos/sign.sh dist/Deadground.app
DG_NOTARY_PROFILE=deadground-notary tools/macos/notarize.sh dist/Deadground.app
```

| Path | Content |
|---|---|
| `Contents/MacOS/deadground` | universal executable; rpath `@executable_path/../Frameworks` |
| `Contents/Resources/{shaders,assets,data,web}` | `SDL_GetBasePath()` inside a bundle |
| `Contents/Resources/Deadground.icns` | from `tools/macos/AppIcon.png`, a placeholder (`gen_icon.py`) |
| `Contents/Frameworks/libMoltenVK.dylib` | universal (Vulkan SDK / MoltenVK release); SDL loads it first |
| `Contents/Frameworks/libsteam_api.dylib` | Steam builds. The executable's `@loader_path` reference is rewritten to `@rpath`. |
| `Contents/MacOS/steam_appid.txt` | only with `--steam-appid`, for runs outside Steam |
| `Contents/Info.plist` | from `tools/macos/Info.plist.in` |

Info.plist keys:
* `LSMinimumSystemVersion` 11.0.
* `NSHighResolutionCapable`.
* `LSAppNapIsDisabled`: a lobby host in the background keeps ticking.
* `LSSupportsGameMode`.
* `NSLocalNetworkUsageDescription`: the macOS 15+ prompt for LAN discovery and phone pairing.
* `LSArchitecturePriority` arm64 first.

Entitlements (hardened runtime):
* `cs.disable-library-validation` and `cs.allow-dyld-environment-variables`, both for the Steam overlay. It is injected with `DYLD_INSERT_LIBRARIES` and signed by Valve.
* The game is not sandboxed (Steam titles are not), so network access needs no entitlement.

Writable data in a bundle: `Contents/Resources` is read-only once signed.
* Logs, config, saves and screenshots already live in `~/Library/Application Support/Deadground/Deadground/`.
* Caches (anim, vgeo builds, trees) and HDR screenshots move there too (`platform_writable_dir`).
* So does `pipeline_cache.bin`.

Steam: `+connect_lobby <id>` arrives through `argv` as on Windows; nothing macOS-specific is needed.

## 4. Feature matrix per GPU family

| Feature | Apple7 (M1) | Apple8 (M2) | Apple9+ (M3 / M4) | Mac2 AMD (Radeon Pro) | Mac2 Intel (UHD / Iris) |
|---|---|---|---|---|---|
| Vulkan 1.3 via MoltenVK 1.3+ | yes | yes | yes | yes | yes |
| Renderer on `main` | classic instanced | same | same | same | same |
| vgeo / nanite (branch `claude/nanite-macos`) | yes, 32-bit two-phase visibility writes | same | same | same | **no**: tier-1 argument buffers, the classic renderer (`r_vgeo 0`) |
| vgeo HW raster (nanite branch) | vertex pulling (no `VK_EXT_mesh_shader`) | same | same | same | — |
| 64-bit buffer atomics | not exposed by MoltenVK | not exposed | not exposed | not exposed | not exposed |
| Bindless 1152 textures / stage (nanite branch) | argument buffers tier 2 | tier 2 | tier 2 | tier 2 (macOS 13+) | tier 1: vgeo off |
| BC1-7 textures | yes | yes | yes | yes | yes |
| ASTC | yes | yes | yes | no | no |
| Push descriptors, dynamic rendering, sync2 | yes | yes | yes | yes | yes |
| Subgroup size | 32 | 32 | 32 | 32 / 64 | 8-32 |
| HDR output (HDR10 PQ, EDR) | yes on XDR / EDR displays | yes | yes | external HDR displays | external HDR displays |
| Memoryless transients | available (MoltenVK lazily allocated) | yes | yes | no | no |
| Fidelity auto-detect cap | High (Pro / Max / Ultra: Modern) | High / Modern | High / Modern | High | Medium |

## 5. Known differences from Windows / Linux

* **vgeo raster cost** (nanite branch): every pass rasterises twice (depth, then payload). The extra cost is in the
  HW / SW raster passes only. On llvmpipe the two phases each take roughly the one-phase time.
  Expect +40-100 % raster time on Apple GPUs; the culling, resolve and shading passes are unchanged.
  Image: identical (sections 1.4 and 9).
* **Timestamps**: Apple GPUs sample at encoder boundaries. Pass-level GPU times (frame graph, vgeo
  pass totals) are meaningful. Splits inside one render pass (the vgeo HW / SW split, `vgeo_stats`)
  can read 0 / total. The fidelity auto-detect uses whole-frame times.
* **Present**: no MAILBOX. `r_vsync 0` is IMMEDIATE (`CAMetalLayer.displaySyncEnabled = NO`).
* **Math**: MoltenVK fast math is off (`MVK_CONFIG_FAST_MATH_ENABLED=0`, `platform_preinit`), so
  the shaders' NaN guards (TAA, exposure) behave as on the other drivers. Override it in the
  environment to measure fast math.
* **Mesh shaders** (nanite branch): none (MoltenVK). `--vgeo-no-mesh` / `DG_VGEO_NO_MESH=1` reproduces the Mac HW
  raster path on any GPU, and `--vgeo-vis32` / `DG_VGEO_VIS32=1` the visibility writes.
* **Crash handling**: a signal handler logs a backtrace, then macOS writes its own `.ips` report
  (`~/Library/Logs/DiagnosticReports`). There is no hang watchdog yet (the Windows `DG_WATCHDOG`
  uses thread suspension).
* **Threads**: no affinity on macOS. Job workers run at `QOS_CLASS_USER_INTERACTIVE` (performance
  cores on Apple Silicon).
* **LAN**: discovery sends the limited broadcast plus each interface's subnet broadcast
  (`lan_ifaddr.c`). A refused broadcast (Local Network permission denied, firewall) is logged once,
  and same-machine games are still listed.
* **Audio**: miniaudio's CoreAudio backend (runtime-linked, the frameworks are linked as well);
  default buffer sizes. HRTF is the engine's own and platform-independent.

## 6. Determinism (Mac <-> PC)

Contract:
* Host and predicting client run these on the same inputs and must agree **bit for bit** on any platform pair:
  * ballistics (`gameplay/ballistics`: sim, surfaces, data fit);
  * recoil;
  * sway;
  * fire animation;
  * aim.
* Every peer also generates the open-world map (terrain raster and POI build) from its seed.
* Everything else replicated is either integer-tick logic (the animation controller) or checked with
  tolerances: the muzzle claim within `wpn_muzzle_tolerance` 0.3 m, and vehicles reconciled to the host.

What broke it, and the fixes:
1. **FMA contraction.**
   * arm64 clang fuses `a * b + c` by default; x86_64 without `-mfma` does not.
   * Fix: `-ffp-contract=off` for every engine target on clang / gcc. MSVC `/fp:precise` does not contract.
2. **libm.**
   * `sinf` / `expf` / `powf` / `tanf` / ... differ in the last bits between Apple libm, glibc and the MSVC CRT.
   * Fix: the contract modules and the map generator call `src/core/detmath`.
     * It evaluates in double with IEEE-exact operations and fixed polynomials, then rounds once.
     * The result is 0 ulp from correctly rounded over the test sweeps.
   * Translation units that `#define DG_DETMATH` before their includes get the `core/dgmath.h`
     helpers (`exp_decay`, `dir_from_angles`, `quat_axis_angle`, ...) on detmath too.
   * Everything else keeps the fast libm.
3. **Unspecified evaluation order.**
   * C leaves the order of function arguments and of `+` / `|` operands open: clang goes left to right, gcc / MSVC usually right to left.
   * RNG draws inside one argument list therefore drew in a different order on a Mac:
     * the transonic random walk (`bal_sim.c`);
     * the Irwin-Hall sums (`bal_sim.c`, `recoil.c`);
     * 18 statements of the POI generator. Zombieland built 38,052 primitives with gcc and 38,040 with clang.
   * Fix: one draw per statement, in the gcc / MSVC order, so existing layouts are kept.
   * A scan of `src/` finds no other case outside cosmetic local particles (`gameplay/effects.c`).

Tests: golden hashes from Linux x86_64 gcc. CI checks them with MSVC (Windows), arm64 clang and x86_64 clang (macOS).

| Test | What is hashed |
|---|---|
| `test_detmath` | 20k inputs through every kernel |
| `test_ballistics` | every cartridge x both drag models, wind, transonic walk, zero solve |
| `test_recoil` | pattern steps, shots, recovery ticks, spread |
| `test_mapgen` | vegetation raster; the POI hash is printed for any map (Zombieland checked locally: gcc == clang) |

Locally, gcc and clang trees produce identical hashes and pass every test.

Jolt is not bit-identical across architectures by default: NEON vs SSE, and FMA. Vehicles are predicted by the driver and
reconciled, so this costs corrections, not desyncs.
* `-DDG_JOLT_CROSS_PLATFORM_DETERMINISTIC=ON`, set on every platform of a session, turns on `JPH_CROSS_PLATFORM_DETERMINISTIC`. Its cost is a few % of physics time.
* The macOS x86_64 slice builds Jolt without FMA in either case.

Manual Mac <-> PC session: `tools/tests/bal_net.sh` with the host on one machine and the client
`+connect <ip>` on the other. `bal_net_compare.py` checks predicted vs authoritative hits within 0.25 m. With the
fixes above the two logs' `predict` / `host` positions should match exactly. Compare the printed
coordinates, not only the verdict.

## 7. Shaders on Metal

* The same SPIR-V on every platform. MoltenVK translates it to MSL with SPIRV-Cross at pipeline creation.
* `tools/macos/msl_check.py` translates every shader the way MoltenVK does (MSL 3.0, argument buffers).
  * With `--metal` (macOS) it also compiles each MSL file to AIR with `xcrun metal`.
  * It skips the 7 shaders MoltenVK never loads: the 64-bit atomic visibility writers and their mesh pairs.
  * CI gates on it (Linux: SPIRV-Cross; macOS: SPIRV-Cross + Metal).
* Startup cost:
  * The persistent `VkPipelineCache` (`<pref>/pipeline_cache.bin`) keeps MoltenVK's translated MSL between launches.
  * Metal's system shader cache keeps the compiled MSL.
  * `MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1` compiles on every core.
  * A pipeline cache is tied to the GPU and driver (vendor / device / UUID), so it is built on the first launch rather than shipped.

## 8. TBDR (Apple GPUs)

* Load ops: every pass already clears or loads explicitly. The first write of a transient starts
  from `UNDEFINED` (the frame graph), which MoltenVK maps to `MTLLoadActionDontCare`.
* Store ops: `fg_store_op(r)` gives `DONT_CARE` for a transient attachment that no later pass of
  the frame uses. The tile is then never written back. It is used by the scope picture-in-picture
  depth (its last pass) and the phone-camera scene depth. Imports (the main HDR / depth / G-buffer)
  and histories are always stored.
* Memoryless: the main targets are all sampled later (SSAO / SSR / TAA / sights), so none can be
  memoryless as the graph is today. The candidates are attachment-only transients. The graph would
  have to allocate them with `TRANSIENT_ATTACHMENT` usage and lazily allocated memory instead of
  pooled sampled images: a follow-up when profiling on Apple hardware shows the bandwidth.
* Post stack: compute passes at render resolution with no mid-pass readbacks; the frame graph
  orders them, so there are no CPU stalls.

## 9. Verified (and where)

GitHub Actions `platforms.yml`. Branch `claude/macos-port` (now `claude/nanite-macos` for the vgeo rows) was all green in run 11 (commit `928133e`) and run 12, all four jobs. After the merge into the classic-only `main`, the vgeo rows no longer apply there. The macOS arm64 job runs the game itself on MoltenVK instead: 120 frames of the dev room, with a screenshot artifact (`moltenvk-devroom`).

| What | Linux x86_64 (gcc) | Windows x64 (MSVC) | macOS arm64 (Apple clang, macos-15) | macOS x86_64 (Apple clang, macos-15-intel) |
|---|---|---|---|---|
| Full build (game, tools, tests) | yes | yes | yes | yes |
| Headless unit suite (`phone` fails on `main` already and runs non-gating, `tools/ci/known_failures.sh`) | pass | pass | pass | pass |
| `test_detmath` golden | = | = | = | = |
| `test_ballistics` trajectory golden | = | = | = | = |
| `test_recoil` golden | = | = | = | = |
| `test_mapgen` vegetation raster golden | = | = | = | = |
| vgeo GPU harness (`vgeo_raster`, `_stream`, `_skinned`, `_foliage`; nanite branch) | llvmpipe, 64-bit **and** forced 32-bit writes | no GPU on the runner (skipped) | **MoltenVK on the runner's Apple paravirtual GPU**: 32-bit writes, vertex pulling; HW / SW depth within 1e-5 of the CPU reference | no GPU (skipped) |
| SPIR-V -> MSL (SPIRV-Cross) | 139 / 139 (7 never loaded on Metal) | — | 139 / 139, then `xcrun metal` AIR compile | — |
| Universal build + Deadground.app (ad-hoc signed) | — | — | `deadground` fat x86_64 + arm64 (x86_64 slice built under Rosetta); universal MoltenVK in `Contents/Frameworks`; `codesign --verify --strict` passes; uploaded as the `Deadground-macos-universal` artifact | — |

Local checks in this repository's container (Linux, llvmpipe, Xvfb):
* The game runs (`--test`, dev room).
* Dev-room image, 64-bit vs 32-bit visibility writes: 0 of 1.44 M pixels over threshold.
* Scope picture-in-picture with the store-op change, under the validation layer: 0 messages, identical image.
* gcc and clang trees: all tests and goldens equal.
* `msl_check`: 0 failures.

Bugs only the Mac / clang / MSVC CI could find, each fixed:
* minih264's Apple arm64 path called a non-existent intrinsic (`vtbl2q_u8`).
* `ffx_a.h` relied on implicit `abs` (clang errors).
* A shader function named `bias` collided with `metal::bias` after SPIRV-Cross, so the SW raster pipelines failed to create on Metal.
* `ru_maxrss` is in bytes on macOS.
* Jolt's Visual Studio project has no RelWithDebInfo configuration (CI builds Release with that generator).
* The portable math made the ballistic step 50 % slower; the bit-exact `floor` / `ldexp` / `frexp` brought it back to +25 % (0.25 us per step).

## 10. Not verified here: needs Mac hardware (manual checklist)

Nothing below can run on GitHub's VMs: no real GPU, no display, no Steam client, no iPhone.

- [ ] **Apple Silicon (M1 / M2 / M3+)**:
  * `--validation` run through the loader (`SDL_VULKAN_LIBRARY`): zero errors in the dev room, horde yard and foliage yard.
  * Record the fidelity auto-detect result and frame times per family (classic renderer).
  * On `claude/nanite-macos`, also run `vgeo_bench` to record the two-phase raster cost.
  * Image-diff the perf-audit scenes against a Windows reference with `tools/img_diff`. Expected: identical but for TAA noise.
- [ ] **Intel Mac (UHD 630 / Iris)**:
  * On the nanite branch, vgeo must log "disabled" (tier-1 argument buffers) and draw through the classic renderer.
  * Fidelity Low / Medium hold 60 fps at 1080p.
  * AMD dGPU Mac (nanite branch): vgeo on.
- [ ] Retina:
  * windowed and fullscreen at 2x;
  * `r_scale` 50-100 %;
  * UI hit-testing at the corners;
  * moving the window between a Retina and a 1x display.
- [ ] Trackpad (two-finger scroll = wheel, click / right click), Magic Mouse, an Xbox / PS5 controller through SDL's GameController backend.
- [ ] Steam:
  * overlay (Shift-Tab) inside the signed / notarized bundle;
  * an invite accepted from a friend (`+connect_lobby` launch);
  * `libsteam_api.dylib` found from `Contents/Frameworks`.
- [ ] Phone pairing from an iPhone on the same Wi-Fi. The first launch asks for Local Network: test "Don't Allow" too. Expect the log line, same-machine games listed, and the game still running.
- [ ] HDR: `r_hdr_output 1` on an XDR / EDR display (HDR10 PQ through MoltenVK). Compare to SDR.
- [ ] Mac <-> PC LAN session with `tools/tests/bal_net.sh` split across the machines (host on each side once): predicted vs host hits identical.
- [ ] Notarization with a Developer ID: `tools/macos/sign.sh` + `notarize.sh`, then Gatekeeper on a clean Mac (`spctl` accepts, first launch without a warning).
- [ ] App Nap: background the game during a lobby wait. Ticks continue (`LSAppNapIsDisabled`).
- [ ] Crash report: `kill -SEGV <pid>` logs a backtrace, and macOS writes its `.ips`.
