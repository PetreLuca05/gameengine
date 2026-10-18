# macOS (Intel x86_64 + Apple Silicon arm64, Vulkan via MoltenVK)

Status: gap analysis (this commit), then the port. Sections below the analysis are filled in as the
work lands; "Verified" says what has actually been run, and where.

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
