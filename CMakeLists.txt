cmake_minimum_required(VERSION 3.24)
project(Deadground VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# C11 is enforced on our own target only (see below); third-party C code keeps its defaults
# (e.g. ENet needs POSIX declarations that strict -std=c11 hides on Linux).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Jolt/joltc default to the static CRT; use it everywhere so all objects agree.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin)

set(STEAM_SDK_DIR "$ENV{STEAM_SDK_DIR}" CACHE PATH "Path to the Steamworks SDK 'sdk' folder (optional)")
option(DG_ENABLE_STEAM "Build the Steam transport (requires STEAM_SDK_DIR)" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---------------------------------------------------------------- SDL3
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.18
  GIT_SHALLOW TRUE)

# ---------------------------------------------------------------- Vulkan headers
FetchContent_Declare(VulkanHeaders
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG vulkan-sdk-1.4.363.0
  GIT_SHALLOW TRUE)

# ---------------------------------------------------------------- Jolt via joltc
set(JPH_SAMPLES OFF CACHE BOOL "" FORCE)
set(JPH_INSTALL OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(joltc
  GIT_REPOSITORY https://github.com/amerkoleci/joltc.git
  GIT_TAG 886e088675bae3a086f8318c7803f8ee962c2f2c)

# ---------------------------------------------------------------- ENet (LAN transport)
FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG v1.3.18
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(SDL3 VulkanHeaders joltc enet)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)

# ---------------------------------------------------------------- vendored single-file libs
add_library(volk STATIC third_party/volk/volk.c)
target_include_directories(volk PUBLIC third_party/volk)
target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)

add_library(vma STATIC third_party/vma/vma_impl.cpp)
target_include_directories(vma PUBLIC third_party/vma)
target_link_libraries(vma PUBLIC volk)

add_library(dg_thirdparty STATIC
  third_party/impl/tp_stb.c third_party/impl/tp_cgltf.c third_party/impl/tp_miniaudio.c
  third_party/impl/tp_nuklear.c third_party/impl/tp_fastnoise.c
  third_party/cjson/cJSON.c third_party/ufbx/ufbx.c)
set_target_properties(dg_thirdparty PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_include_directories(dg_thirdparty PUBLIC
  third_party/stb third_party/cgltf third_party/miniaudio third_party/cjson
  third_party/nuklear third_party/fastnoise third_party/ufbx third_party/meshoptimizer third_party)
if(MSVC)
  target_compile_options(dg_thirdparty PRIVATE /W0)
else()
  target_compile_options(dg_thirdparty PRIVATE -w)
  target_link_libraries(dg_thirdparty PUBLIC m dl pthread)
endif()

# meshoptimizer v0.22: simplifier (character LODs, vgeo DAG) + clusterizer (vgeo clusters).
add_library(meshopt STATIC third_party/meshoptimizer/simplifier.cpp third_party/meshoptimizer/allocator.cpp third_party/meshoptimizer/clusterizer.cpp)
target_include_directories(meshopt PUBLIC third_party/meshoptimizer)
if(MSVC)
  target_compile_options(meshopt PRIVATE /W0)
else()
  target_compile_options(meshopt PRIVATE -w)
endif()

# Jolt shim: small extern "C" helpers for anything joltc lacks.
add_library(jolt_shim STATIC third_party/jolt_shim/jolt_shim.cpp)
target_include_directories(jolt_shim PUBLIC third_party/jolt_shim)
target_link_libraries(jolt_shim PUBLIC joltc PRIVATE Jolt)

# ---------------------------------------------------------------- Steam (optional)
set(DG_HAVE_STEAM OFF)
if(DG_ENABLE_STEAM AND STEAM_SDK_DIR AND EXISTS "${STEAM_SDK_DIR}/public/steam/steam_api.h")
  set(DG_HAVE_STEAM ON)
  if(WIN32)
    set(STEAM_LIB "${STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
    set(STEAM_DLL "${STEAM_SDK_DIR}/redistributable_bin/win64/steam_api64.dll")
  else()
    set(STEAM_LIB "${STEAM_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so")
    set(STEAM_DLL "${STEAM_LIB}")
  endif()
  add_library(steam_shim STATIC third_party/steam_shim/steam_shim.cpp)
  target_include_directories(steam_shim PUBLIC third_party/steam_shim PRIVATE "${STEAM_SDK_DIR}/public")
  target_link_libraries(steam_shim PUBLIC "${STEAM_LIB}")
  message(STATUS "Deadground: Steam transport ENABLED (${STEAM_SDK_DIR})")
else()
  message(STATUS "Deadground: Steam transport disabled (LAN-only build)")
endif()

# ---------------------------------------------------------------- shaders
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
find_program(GLSLANG NAMES glslang glslangValidator HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
if(GLSLC)
  set(DG_SHADER_COMPILER "${GLSLC}")
  set(DG_SHADER_COMPILER_KIND "glslc")
elseif(GLSLANG)
  set(DG_SHADER_COMPILER "${GLSLANG}")
  set(DG_SHADER_COMPILER_KIND "glslang")
else()
  message(FATAL_ERROR "No GLSL compiler found (need glslc or glslang/glslangValidator on PATH or VULKAN_SDK)")
endif()
message(STATUS "Deadground: shader compiler ${DG_SHADER_COMPILER}")

file(GLOB DG_SHADER_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/shaders/*.vert ${CMAKE_SOURCE_DIR}/shaders/*.frag ${CMAKE_SOURCE_DIR}/shaders/*.comp
  ${CMAKE_SOURCE_DIR}/shaders/post/*.vert ${CMAKE_SOURCE_DIR}/shaders/post/*.frag ${CMAKE_SOURCE_DIR}/shaders/post/*.comp
  ${CMAKE_SOURCE_DIR}/shaders/sights/*.frag ${CMAKE_SOURCE_DIR}/shaders/sights/*.comp
  ${CMAKE_SOURCE_DIR}/shaders/sky/*.comp ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.comp
  ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.vert ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.frag ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.mesh)
file(GLOB DG_SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.glsl ${CMAKE_SOURCE_DIR}/shaders/post/*.glsl ${CMAKE_SOURCE_DIR}/shaders/sights/*.glsl ${CMAKE_SOURCE_DIR}/shaders/sky/*.glsl ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.glsl ${CMAKE_SOURCE_DIR}/third_party/fsr/*.h
  ${CMAKE_SOURCE_DIR}/shaders/post/*.h)
set(DG_SHADER_OUT ${CMAKE_BINARY_DIR}/bin/shaders)
set(DG_SPV_FILES)
foreach(src ${DG_SHADER_SOURCES})
  get_filename_component(name ${src} NAME)
  get_filename_component(dir ${src} DIRECTORY)
  get_filename_component(dirname ${dir} NAME)
  set(sub "")
  if(dirname STREQUAL "post")
    set(sub "post/")
  endif()
  if(dirname STREQUAL "sights")
    set(sub "sights/")
  endif()
  if(dirname STREQUAL "sky")
    set(sub "sky/")
  endif()
  if(dirname STREQUAL "vgeo")
    set(sub "vgeo/")
  endif()
  set(out ${DG_SHADER_OUT}/${sub}${name}.spv)
  if(DG_SHADER_COMPILER_KIND STREQUAL "glslc")
    set(cmd ${DG_SHADER_COMPILER} --target-env=vulkan1.3 -O -I ${CMAKE_SOURCE_DIR}/shaders -I ${CMAKE_SOURCE_DIR}/shaders/post -I ${CMAKE_SOURCE_DIR}/third_party/fsr -I ${CMAKE_SOURCE_DIR}/third_party/smaa -o ${out} ${src})
  else()
    set(cmd ${DG_SHADER_COMPILER} -V --target-env vulkan1.3 -I${CMAKE_SOURCE_DIR}/shaders -I${CMAKE_SOURCE_DIR}/shaders/post -I${CMAKE_SOURCE_DIR}/third_party/fsr -I${CMAKE_SOURCE_DIR}/third_party/smaa -o ${out} ${src})
  endif()
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/post
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/sights
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/sky
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/vgeo
    COMMAND ${cmd}
    DEPENDS ${src} ${DG_SHADER_INCLUDES}
    COMMENT "GLSL ${name}" VERBATIM)
  list(APPEND DG_SPV_FILES ${out})
endforeach()
add_custom_target(dg_shaders DEPENDS ${DG_SPV_FILES})

# ---------------------------------------------------------------- game
file(GLOB_RECURSE DG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.h)
add_executable(deadground ${DG_SOURCES})
set_target_properties(deadground PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_dependencies(deadground dg_shaders)
target_include_directories(deadground PRIVATE src third_party/smaa third_party/fsr)
target_link_libraries(deadground PRIVATE SDL3::SDL3-static volk vma dg_thirdparty joltc jolt_shim enet meshopt)
target_compile_definitions(deadground PRIVATE
  DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  DG_SHADER_COMPILER="${DG_SHADER_COMPILER}"
  DG_SHADER_COMPILER_KIND="${DG_SHADER_COMPILER_KIND}"
  DG_VERSION="${PROJECT_VERSION}")
if(DG_HAVE_STEAM)
  target_compile_definitions(deadground PRIVATE DG_HAVE_STEAM=1)
  target_link_libraries(deadground PRIVATE steam_shim)
  add_custom_command(TARGET deadground POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAM_DLL}" $<TARGET_FILE_DIR:deadground>
    COMMAND ${CMAKE_COMMAND} -E echo 480 > $<TARGET_FILE_DIR:deadground>/steam_appid.txt)
endif()
if(WIN32)
  target_link_libraries(deadground PRIVATE ws2_32 winmm dbghelp)
  target_compile_definitions(deadground PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
  target_compile_options(deadground PRIVATE /W3 /wd4100 /wd4201 /wd4324 /utf-8)
else()
  target_compile_options(deadground PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_libraries(deadground PRIVATE m pthread)
endif()
# Data / assets are mirrored next to the exe on every build (not only when it relinks), so asset-only
# edits (effects, textures) reach the tests and the game. copy_directory_if_different skips unchanged files.
add_custom_target(dg_copy_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/bin/data
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/bin/assets
  COMMENT "Mirroring data/ and assets/")
add_dependencies(deadground dg_copy_data)

# ---------------------------------------------------------------- unit tests (ctest)
# Pure modules only: no SDL / Vulkan / Jolt, so this builds and runs anywhere.
enable_testing()
add_executable(test_recoil tests/test_recoil.c src/gameplay/recoil.c src/core/spring.c)
set_target_properties(test_recoil PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_recoil PRIVATE src)
if(MSVC)
  target_compile_options(test_recoil PRIVATE /W3 /utf-8)
else()
  target_link_libraries(test_recoil PRIVATE m)
endif()
add_test(NAME recoil COMMAND test_recoil)

# ---------------------------------------------------------------- animation tools
# Engine-free subset: anim core + FBX import + cache (+ log / platform helpers).
set(DG_ANIMCORE_SOURCES
  src/anim/anim.c src/anim/skeleton.c src/anim/retarget.c src/anim/anim_mesh.c src/anim/controller.c src/anim/procedural.c src/anim/turn.c src/anim/spine.c
  src/anim/weapon_hold.c src/anim/damage_additive.c src/anim/foot_ik.c src/anim/anim_net.c src/anim/ragdoll_def.c
  src/anim/anim_library.c src/anim/character_rt.c src/gameplay/hitboxes.c src/gameplay/items.c src/gameplay/attachments.c src/gameplay/recoil.c src/core/spring.c
  src/world/time_of_day.c src/world/weather.c src/gameplay/vehicle_def.c src/vfx/vfx_curve.c src/vfx/vfx_library.c src/vfx/vfx_sim.c src/vfx/vfx_flesh.c src/vfx/vfx_atlas.c src/player/holdpoint.c src/player/camera_fp_math.c
  src/assets/fbx_loader.c src/assets/anim_cache.c src/assets/character.c src/render/skeletal_mesh.c
  src/core/log.c src/core/platform.c)
add_library(dg_animcore STATIC ${DG_ANIMCORE_SOURCES})
set_target_properties(dg_animcore PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(dg_animcore PUBLIC src)
target_link_libraries(dg_animcore PUBLIC dg_thirdparty meshopt SDL3::SDL3-static)
if(MSVC)
  target_compile_options(dg_animcore PRIVATE /W3 /wd4100 /wd4201 /utf-8)
  target_link_libraries(dg_animcore PUBLIC winmm)
else()
  target_link_libraries(dg_animcore PUBLIC m pthread)
endif()

add_executable(bake_anim tools/bake_anim/bake_anim.c)
set_target_properties(bake_anim PROPERTIES C_STANDARD 11)
target_link_libraries(bake_anim PRIVATE dg_animcore)

add_executable(fbx_textures tools/fbx_textures/fbx_textures.c)
set_target_properties(fbx_textures PROPERTIES C_STANDARD 11)
target_link_libraries(fbx_textures PRIVATE dg_animcore)

# Virtualized geometry builder (pure C): the game compiles the same sources through the src glob.
add_library(dg_vgeo STATIC src/render/vgeo/vgeo_build.c src/render/vgeo/vgeo_procgen.c src/render/vgeo/vgeo_import.c)
set_target_properties(dg_vgeo PROPERTIES C_STANDARD 11)
target_link_libraries(dg_vgeo PUBLIC dg_animcore meshopt)
if(MSVC)
  target_compile_options(dg_vgeo PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()

# Virtualized geometry GPU module (culling / Hi-Z) on a VgContext: the game builds it through the src glob,
# the harness tests and tools/bench_vgeo through this library.
add_library(dg_vgeo_gpu STATIC src/render/vgeo/vgeo_ctx.c src/render/vgeo/vgeo_scene.c src/render/vgeo/vgeo_cull.c src/render/vgeo/vgeo_raster.c)
set_target_properties(dg_vgeo_gpu PROPERTIES C_STANDARD 11)
target_link_libraries(dg_vgeo_gpu PUBLIC dg_vgeo volk vma)
if(MSVC)
  target_compile_options(dg_vgeo_gpu PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()

add_executable(bench_vgeo tools/bench_vgeo/bench_vgeo.c tests/vgeo_gpu_harness.c)
set_target_properties(bench_vgeo PROPERTIES C_STANDARD 11)
target_include_directories(bench_vgeo PRIVATE tests)
target_link_libraries(bench_vgeo PRIVATE dg_vgeo_gpu)
add_dependencies(bench_vgeo dg_shaders)

add_executable(build_vgeo tools/build_vgeo/build_vgeo.c)
set_target_properties(build_vgeo PROPERTIES C_STANDARD 11)
target_link_libraries(build_vgeo PRIVATE dg_vgeo)

# Character pipeline (docs/CHARACTERS.md): Skeleton / SkeletalMesh bakes, part splitting, retargeting.
foreach(tool bake_skeleton bake_mesh split_parts retarget)
  add_executable(${tool} tools/${tool}/${tool}.c)
  set_target_properties(${tool} PROPERTIES C_STANDARD 11)
  target_link_libraries(${tool} PRIVATE dg_animcore)
endforeach()

add_executable(validate_rig tools/validate_rig/validate_rig.c)
set_target_properties(validate_rig PROPERTIES C_STANDARD 11)
target_link_libraries(validate_rig PRIVATE dg_animcore)

# VFX core (pure): curves, library, simulation, atlas packing.
add_executable(test_vfx tests/test_vfx.c)
set_target_properties(test_vfx PROPERTIES C_STANDARD 11)
target_link_libraries(test_vfx PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_vfx PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME vfx COMMAND test_vfx WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Day / night core: sun / moon positions, time replication, curves, events, light schedules.
add_executable(test_daynight tests/test_daynight.c)
set_target_properties(test_daynight PROPERTIES C_STANDARD 11)
target_link_libraries(test_daynight PRIVATE dg_animcore)
add_test(NAME daynight COMMAND test_daynight WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Weather: blending determinism, late join pack, surface rates, wet material, lightning, scheduler, lens drops.
add_executable(test_weather tests/test_weather.c)
set_target_properties(test_weather PROPERTIES C_STANDARD 11)
target_link_libraries(test_weather PRIVATE dg_animcore)
add_test(NAME weather COMMAND test_weather WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Vehicles: definitions, fuel curve, tire model, steering clamp / smoothing, quantization.
add_executable(test_vehicles tests/test_vehicles.c)
set_target_properties(test_vehicles PROPERTIES C_STANDARD 11)
target_link_libraries(test_vehicles PRIVATE dg_animcore)
add_test(NAME vehicles COMMAND test_vehicles WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Flesh impacts: exit rules / thickness, splatter placement, skinned decal budgets, gore off.
add_executable(test_flesh tests/test_flesh.c)
set_target_properties(test_flesh PROPERTIES C_STANDARD 11)
target_link_libraries(test_flesh PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_flesh PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME flesh COMMAND test_flesh WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Horde core: grid vs brute force, steering bit-identical across thread counts, tier hysteresis.
add_executable(test_horde tests/test_horde.c src/gameplay/zombies/horde_grid.c src/gameplay/zombies/horde_steer.c src/core/jobs.c)
set_target_properties(test_horde PROPERTIES C_STANDARD 11)
target_link_libraries(test_horde PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_horde PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME horde COMMAND test_horde WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Hip vs ADS recoil: hip ratios, ADS kick anchored on the sight for every weapon x optic.
add_executable(test_recoil_ads tests/test_recoil_ads.c)
set_target_properties(test_recoil_ads PROPERTIES C_STANDARD 11)
target_link_libraries(test_recoil_ads PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_recoil_ads PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME recoil_ads COMMAND test_recoil_ads WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

add_executable(test_characters tests/test_characters.c)
set_target_properties(test_characters PROPERTIES C_STANDARD 11)
target_link_libraries(test_characters PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_characters PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME characters COMMAND test_characters WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

add_executable(test_anim tests/test_anim.c)
set_target_properties(test_anim PROPERTIES C_STANDARD 11)
target_link_libraries(test_anim PRIVATE dg_animcore)
add_test(NAME anim COMMAND test_anim WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Ragdolls with real Jolt physics: joint limits over 10 s drops, rest / freeze.
add_executable(test_ragdoll tests/test_ragdoll.c src/physics/physics.c src/physics/ragdoll.c)
set_target_properties(test_ragdoll PROPERTIES C_STANDARD 11)
target_link_libraries(test_ragdoll PRIVATE dg_animcore joltc jolt_shim)
if(MSVC)
  target_compile_options(test_ragdoll PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME ragdoll COMMAND test_ragdoll WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Game UI: layout engine, inventory drag and drop / intents / reconcile, attachments, icons.
add_executable(test_ui tests/test_ui.c src/ui/gui/gui_layout.c src/ui/inv_dragdrop.c src/gameplay/inv_ops.c src/gameplay/inventory.c)
set_target_properties(test_ui PROPERTIES C_STANDARD 11)
target_link_libraries(test_ui PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_ui PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME ui COMMAND test_ui WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

add_executable(anim_probe tools/anim_probe/anim_probe.c)
set_target_properties(anim_probe PROPERTIES C_STANDARD 11)
target_link_libraries(anim_probe PRIVATE dg_animcore)

# Virtualized geometry builder (docs/VGEO.md): structure, crack-free cuts, determinism, gates.
add_executable(test_vgeo_build tests/test_vgeo_build.c)
set_target_properties(test_vgeo_build PROPERTIES C_STANDARD 11)
target_link_libraries(test_vgeo_build PRIVATE dg_vgeo)
if(MSVC)
  target_compile_options(test_vgeo_build PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME vgeo_build COMMAND test_vgeo_build WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
add_test(NAME vgeo_build_long COMMAND test_vgeo_build --long WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
set_tests_properties(vgeo_build_long PROPERTIES LABELS long TIMEOUT 600)

# Virtualized geometry GPU culling on the window-less harness (needs a Vulkan 1.3 device; validation
# when the portable layer is present).
add_executable(test_vgeo_gpu tests/test_vgeo_gpu.c tests/vgeo_gpu_harness.c)
set_target_properties(test_vgeo_gpu PROPERTIES C_STANDARD 11)
target_link_libraries(test_vgeo_gpu PRIVATE dg_vgeo_gpu)
add_dependencies(test_vgeo_gpu dg_shaders)
if(MSVC)
  target_compile_options(test_vgeo_gpu PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME vgeo_gpu COMMAND test_vgeo_gpu --validation WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
if(EXISTS "${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
  set_tests_properties(vgeo_gpu PROPERTIES ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
endif()

# Virtualized geometry rasterisers (HW mesh / vertex pulling vs SW compute) on the harness.
add_executable(test_vgeo_raster tests/test_vgeo_raster.c tests/vgeo_gpu_harness.c)
set_target_properties(test_vgeo_raster PROPERTIES C_STANDARD 11)
target_link_libraries(test_vgeo_raster PRIVATE dg_vgeo_gpu)
add_dependencies(test_vgeo_raster dg_shaders)
if(MSVC)
  target_compile_options(test_vgeo_raster PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME vgeo_raster COMMAND test_vgeo_raster --validation WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
if(EXISTS "${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
  set_tests_properties(vgeo_raster PROPERTIES ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
endif()
