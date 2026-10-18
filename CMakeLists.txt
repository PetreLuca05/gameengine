cmake_minimum_required(VERSION 3.24)
# macOS (docs/PLATFORM_MACOS.md): macOS 11 for both slices; one architecture per build tree
# (cmake/toolchains/macos-*.cmake, the host's by default), tools/macos/build_universal.sh lipos them.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(Deadground VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING "macOS architecture of this build tree (x86_64 or arm64)" FORCE)
  endif()
  list(LENGTH CMAKE_OSX_ARCHITECTURES DG_OSX_ARCH_COUNT)
  if(DG_OSX_ARCH_COUNT GREATER 1)
    message(FATAL_ERROR "One architecture per build tree (Jolt's SIMD flags are per architecture): build x86_64 and arm64 separately, tools/macos/build_universal.sh lipos them")
  endif()
  message(STATUS "Deadground: macOS ${CMAKE_OSX_ARCHITECTURES}, deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

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
# Shipping builds (the macOS .app, a Steam depot): no source-tree paths baked in, so no shader hot
# reload and no source-dir fallbacks (docs/PLATFORM_MACOS.md "Distribution").
option(DG_DISTRIBUTION "Shipping build: no DG_SOURCE_DIR / shader compiler paths" OFF)
# Jolt bit-identical across x86_64 / arm64 / compilers (JPH_CROSS_PLATFORM_DETERMINISTIC: no FMA, a
# few % slower). Vehicles are predicted by the driving client and reconciled, so it only reduces
# corrections in mixed Mac / PC sessions; off by default (docs/PLATFORM_MACOS.md "Determinism").
option(DG_JOLT_CROSS_PLATFORM_DETERMINISTIC "Build Jolt cross-platform deterministic" OFF)

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
set(CROSS_PLATFORM_DETERMINISTIC ${DG_JOLT_CROSS_PLATFORM_DETERMINISTIC} CACHE BOOL "" FORCE)
if(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
  # macOS 11 runs on Ivy Bridge (Mac Pro 2013): AVX + F16C, no AVX2 / BMI / LZCNT / FMA.
  set(USE_AVX2 OFF CACHE BOOL "" FORCE)
  set(USE_LZCNT OFF CACHE BOOL "" FORCE)
  set(USE_TZCNT OFF CACHE BOOL "" FORCE)
  set(USE_FMADD OFF CACHE BOOL "" FORCE)
endif()
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

# ---------------------------------------------------------------- mbedTLS (phone companion server: TLS + certificates)
# Release tarball (generated sources included, no Python / submodules needed). Apache-2.0.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(SDL3 VulkanHeaders joltc enet mbedtls)
# Our code: no FMA contraction anywhere (arm64 clang contracts a * b + c by default, x86_64 does not:
# the deterministic modules would differ between a Mac and a PC; docs/PLATFORM_MACOS.md "Determinism").
# MSVC's /fp:precise does not contract. Applies to every target below (not the fetched libraries).
if(NOT MSVC)
  add_compile_options(-ffp-contract=off)
endif()
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
  third_party/cjson/cJSON.c third_party/ufbx/ufbx.c
  third_party/impl/tp_video.c third_party/impl/tp_mp4.c third_party/qrcodegen/qrcodegen.c)
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
  elseif(APPLE)
    # universal (x86_64 + arm64); the .app carries it in Contents/Frameworks (tools/macos/make_bundle.sh)
    set(STEAM_LIB "${STEAM_SDK_DIR}/redistributable_bin/osx/libsteam_api.dylib")
    set(STEAM_DLL "${STEAM_LIB}")
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
  ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.vert ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.frag ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.mesh
  ${CMAKE_SOURCE_DIR}/shaders/water/*.vert ${CMAKE_SOURCE_DIR}/shaders/water/*.frag ${CMAKE_SOURCE_DIR}/shaders/water/*.comp)
file(GLOB DG_SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.glsl ${CMAKE_SOURCE_DIR}/shaders/post/*.glsl ${CMAKE_SOURCE_DIR}/shaders/sights/*.glsl ${CMAKE_SOURCE_DIR}/shaders/sky/*.glsl ${CMAKE_SOURCE_DIR}/shaders/vgeo/*.glsl ${CMAKE_SOURCE_DIR}/shaders/terrain/*.glsl ${CMAKE_SOURCE_DIR}/shaders/water/*.glsl ${CMAKE_SOURCE_DIR}/third_party/fsr/*.h
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
  if(dirname STREQUAL "water")
    set(sub "water/")
  endif()
  set(out ${DG_SHADER_OUT}/${sub}${name}.spv)
  if(DG_SHADER_COMPILER_KIND STREQUAL "glslc")
    set(cmd ${DG_SHADER_COMPILER} --target-env=vulkan1.3 -O -I ${CMAKE_SOURCE_DIR}/shaders -I ${CMAKE_SOURCE_DIR}/shaders/post -I ${CMAKE_SOURCE_DIR}/shaders/water -I ${CMAKE_SOURCE_DIR}/third_party/fsr -I ${CMAKE_SOURCE_DIR}/third_party/smaa -o ${out} ${src})
  else()
    set(cmd ${DG_SHADER_COMPILER} -V --target-env vulkan1.3 -I${CMAKE_SOURCE_DIR}/shaders -I${CMAKE_SOURCE_DIR}/shaders/post -I${CMAKE_SOURCE_DIR}/shaders/water -I${CMAKE_SOURCE_DIR}/third_party/fsr -I${CMAKE_SOURCE_DIR}/third_party/smaa -o ${out} ${src})
  endif()
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/post
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/sights
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/sky
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/vgeo
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}/water
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
target_link_libraries(deadground PRIVATE SDL3::SDL3-static volk vma dg_thirdparty joltc jolt_shim enet meshopt mbedtls mbedx509 mbedcrypto)
if(NOT DG_DISTRIBUTION)
  target_compile_definitions(deadground PRIVATE
    DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    DG_SHADER_COMPILER="${DG_SHADER_COMPILER}"
    DG_SHADER_COMPILER_KIND="${DG_SHADER_COMPILER_KIND}")
endif()
target_compile_definitions(deadground PRIVATE
  DG_VERSION="${PROJECT_VERSION}"
  # Looks (docs/LOOKS.md): the live tuning tools (look_tune / look_dump) exist in dev builds only.
  $<$<NOT:$<CONFIG:Release>>:DG_LOOK_TUNE=1>)
if(DG_HAVE_STEAM)
  target_compile_definitions(deadground PRIVATE DG_HAVE_STEAM=1)
  target_link_libraries(deadground PRIVATE steam_shim)
  add_custom_command(TARGET deadground POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAM_DLL}" $<TARGET_FILE_DIR:deadground>
    COMMAND ${CMAKE_COMMAND} -E echo 480 > $<TARGET_FILE_DIR:deadground>/steam_appid.txt)
endif()
if(APPLE)
  # SDL3 brings Cocoa / Metal / QuartzCore / IOKit / GameController; miniaudio runtime-links CoreAudio
  # (linked here too, so a stripped dlopen path cannot lose audio). Vulkan is dlopen'd (SDL / volk):
  # @executable_path/../Frameworks finds the bundled libMoltenVK.dylib / libsteam_api.dylib.
  target_link_libraries(deadground PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  set_target_properties(deadground PROPERTIES BUILD_RPATH "@executable_path;@executable_path/../Frameworks" INSTALL_RPATH "@executable_path/../Frameworks")
endif()
if(WIN32)
  target_link_libraries(deadground PRIVATE ws2_32 winmm dbghelp iphlpapi)
  target_compile_definitions(deadground PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
  target_compile_options(deadground PRIVATE /W3 /wd4100 /wd4201 /wd4324 /utf-8)
else()
  target_compile_options(deadground PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_libraries(deadground PRIVATE m pthread)
endif()
# Ballistics must integrate bit-identically on the host and the predicting client: no FMA
# contraction, precise float semantics (docs/BALLISTICS.md "Determinism").
file(GLOB DG_BALLISTICS_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/gameplay/ballistics/*.c)
if(MSVC)
  set_source_files_properties(${DG_BALLISTICS_SOURCES} PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
  set_source_files_properties(${DG_BALLISTICS_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
# Data / assets are mirrored next to the exe on every build (not only when it relinks), so asset-only
# edits (effects, textures) reach the tests and the game. copy_directory_if_different skips unchanged files.
add_custom_target(dg_copy_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/bin/data
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/bin/assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/web ${CMAKE_BINARY_DIR}/bin/web
  COMMENT "Mirroring data/, assets/ and web/")
add_dependencies(deadground dg_copy_data)

# ---------------------------------------------------------------- unit tests (ctest)
# Pure modules only: no SDL / Vulkan / Jolt, so this builds and runs anywhere.
enable_testing()
add_executable(test_recoil tests/test_recoil.c src/gameplay/recoil.c src/core/spring.c src/core/detmath.c)
set_target_properties(test_recoil PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_recoil PRIVATE src)
if(MSVC)
  target_compile_options(test_recoil PRIVATE /W3 /utf-8)
else()
  target_link_libraries(test_recoil PRIVATE m)
endif()
add_test(NAME recoil COMMAND test_recoil)
add_executable(test_spectate tests/test_spectate.c src/net/spectate_core.c src/gameplay/recoil.c src/core/spring.c src/core/detmath.c)
set_target_properties(test_spectate PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_spectate PRIVATE src)
if(MSVC)
  target_compile_options(test_spectate PRIVATE /W3 /utf-8)
else()
  target_link_libraries(test_spectate PRIVATE m)
endif()
add_test(NAME spectate COMMAND test_spectate)
add_executable(test_aim tests/test_aim.c src/gameplay/aim.c src/gameplay/recoil.c src/core/spring.c src/core/detmath.c)
set_target_properties(test_aim PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_aim PRIVATE src)
if(MSVC)
  target_compile_options(test_aim PRIVATE /W3 /utf-8)
else()
  target_link_libraries(test_aim PRIVATE m)
endif()
add_test(NAME aim COMMAND test_aim)
add_executable(test_ballistics tests/test_ballistics.c src/core/detmath.c src/gameplay/ballistics/bal_data.c src/gameplay/ballistics/bal_drag.c
  src/gameplay/ballistics/bal_sim.c src/gameplay/ballistics/bal_surface.c src/gameplay/ballistics/bal_acoustics.c
  src/gameplay/ballistics/bal_tracer.c src/gameplay/ballistics/bal_muzzle.c third_party/cjson/cJSON.c)
set_target_properties(test_ballistics PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_ballistics PRIVATE src third_party/cjson)
target_compile_definitions(test_ballistics PRIVATE DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
  target_compile_options(test_ballistics PRIVATE /W3 /utf-8 /fp:precise)
else()
  target_compile_options(test_ballistics PRIVATE -ffp-contract=off)
  target_link_libraries(test_ballistics PRIVATE m)
endif()
add_test(NAME ballistics COMMAND test_ballistics)
# Deterministic float math (docs/PLATFORM_MACOS.md "Determinism"): accuracy + the cross-platform golden hash.
add_executable(test_detmath tests/test_detmath.c src/core/detmath.c)
set_target_properties(test_detmath PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_detmath PRIVATE src)
if(MSVC)
  target_compile_options(test_detmath PRIVATE /W3 /utf-8 /fp:precise)
else()
  target_link_libraries(test_detmath PRIVATE m)
endif()
add_test(NAME detmath COMMAND test_detmath)

# ---------------------------------------------------------------- animation tools
# Engine-free subset: anim core + FBX import + cache (+ log / platform helpers).
set(DG_ANIMCORE_SOURCES
  src/anim/anim.c src/anim/skeleton.c src/anim/retarget.c src/anim/anim_mesh.c src/anim/controller.c src/anim/procedural.c src/anim/turn.c src/anim/spine.c
  src/anim/weapon_hold.c src/anim/handpose.c src/editor/handpose/hp_discover.c src/editor/handpose/hp_edit.c src/editor/handpose/hp_rig.c src/editor/handpose/hp_xform.c src/anim/damage_additive.c src/anim/foot_ik.c src/anim/swim.c src/anim/anim_net.c src/anim/ragdoll_def.c
  src/anim/anim_library.c src/anim/character_rt.c src/gameplay/hitboxes.c src/gameplay/items.c src/gameplay/attachments.c src/gameplay/recoil.c src/core/spring.c src/core/detmath.c
  src/world/time_of_day.c src/world/weather.c src/world/wind.c src/world/water/water.c src/world/water/water_waves.c src/gameplay/vehicle_def.c src/vfx/vfx_curve.c src/vfx/vfx_library.c src/vfx/vfx_sim.c src/vfx/vfx_flesh.c src/vfx/vfx_atlas.c src/player/holdpoint.c src/player/sway.c src/anim/fire_anim.c src/player/camera_fp_math.c src/gameplay/phone/phone_orient.c
  src/assets/fbx_loader.c src/assets/anim_cache.c src/assets/character.c src/render/skeletal_mesh.c
  src/core/log.c src/core/platform.c)
add_library(dg_animcore STATIC ${DG_ANIMCORE_SOURCES})
# Water swell / queries must be bit-identical on every machine (docs/WATER.md "Determinism").
set(DG_WATER_DETERMINISTIC src/world/water/water.c src/world/water/water_waves.c)
if(MSVC)
  set_source_files_properties(${DG_WATER_DETERMINISTIC} PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
  set_source_files_properties(${DG_WATER_DETERMINISTIC} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
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
add_library(dg_vgeo STATIC src/render/vgeo/vgeo_build.c src/render/vgeo/vgeo_procgen.c src/render/vgeo/vgeo_procgen_building.c src/render/vgeo/vgeo_procgen_skinned.c src/render/vgeo/vgeo_import.c src/render/vgeo/vgeo_impostor.c src/render/vgeo/vgeo_procgen_foliage.c src/render/vgeo/vgeo_tree.c
  src/render/vgeo/foliage_wind.c src/render/vgeo/grass_asset.c)
set_target_properties(dg_vgeo PROPERTIES C_STANDARD 11)
target_link_libraries(dg_vgeo PUBLIC dg_animcore meshopt)
if(MSVC)
  target_compile_options(dg_vgeo PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()

# Virtualized geometry GPU module (culling / Hi-Z) on a VgContext: the game builds it through the src glob,
# the harness tests and tools/bench_vgeo through this library.
add_library(dg_vgeo_gpu STATIC src/render/vgeo/vgeo_ctx.c src/render/vgeo/vgeo_scene.c src/render/vgeo/vgeo_cull.c src/render/vgeo/vgeo_raster.c src/render/vgeo/vgeo_stream.c src/render/vgeo/vgeo_skin.c src/render/vgeo/vgeo_tex.c src/render/vgeo/foliage_support.c)
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

add_executable(img_diff tools/img_diff/img_diff.c)
set_target_properties(img_diff PROPERTIES C_STANDARD 11)
target_link_libraries(img_diff PRIVATE dg_thirdparty)

add_executable(build_vgeo tools/build_vgeo/build_vgeo.c)
set_target_properties(build_vgeo PROPERTIES C_STANDARD 11)
target_link_libraries(build_vgeo PRIVATE dg_vgeo)

# vgeo test scene content (data/levels/vgeo_test.json): generated once into bin/assets/vgeo (never
# committed). Only regenerated when missing: delete the files after a builder format change.
set(DG_VGEO_OUT ${CMAKE_BINARY_DIR}/bin/assets/vgeo)
set(DG_VGEO_CONTENT)
foreach(item "rock_field;field;10000000" "statue;statue;500000" "building;building;2000000")
  list(GET item 0 vg_name)
  list(GET item 1 vg_gen)
  list(GET item 2 vg_tris)
  add_custom_command(OUTPUT ${DG_VGEO_OUT}/${vg_name}.vgeo
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_VGEO_OUT}
    COMMAND $<TARGET_FILE:build_vgeo> --gen ${vg_gen} --tris ${vg_tris} --out ${DG_VGEO_OUT}/${vg_name}.vgeo
    COMMENT "vgeo content ${vg_name}.vgeo" VERBATIM)
  list(APPEND DG_VGEO_CONTENT ${DG_VGEO_OUT}/${vg_name}.vgeo)
endforeach()
# Imported trees (docs/VGEO.md "Tree models"): the assets/treefbx species built (clusters, DAG,
# impostors) into bin/assets/vgeo/trees/<name>.dgtree; the game rebuilds into assets/cache when a
# file is missing or its key is stale.
file(GLOB DG_TREE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/treefbx/*)
add_custom_command(OUTPUT ${DG_VGEO_OUT}/trees/trees.stamp
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_VGEO_OUT}/trees
  COMMAND $<TARGET_FILE:build_vgeo> --trees ${CMAKE_SOURCE_DIR}/assets/treefbx/ ${DG_VGEO_OUT}/trees/
  COMMAND ${CMAKE_COMMAND} -E touch ${DG_VGEO_OUT}/trees/trees.stamp
  DEPENDS ${DG_TREE_SOURCES} ${CMAKE_SOURCE_DIR}/src/render/vgeo/vgeo_tree.c ${CMAKE_SOURCE_DIR}/src/render/vgeo/vgeo_tree.h
  COMMENT "vgeo content: trees" VERBATIM)
list(APPEND DG_VGEO_CONTENT ${DG_VGEO_OUT}/trees/trees.stamp)
add_custom_target(dg_vgeo_content ALL DEPENDS ${DG_VGEO_CONTENT})
add_dependencies(dg_vgeo_content build_vgeo)

# Open-world map generator (docs/ZOMBIE_MAP.md): the pure terrain / road / POI generator and loot
# tables as a library for the preview tool and the unit tests (the game compiles the same sources).
add_library(dg_mapgen STATIC src/world/mapgen.c src/world/mapgen_poi.c src/gameplay/loot.c)
set_target_properties(dg_mapgen PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(dg_mapgen PUBLIC src)
target_link_libraries(dg_mapgen PUBLIC dg_thirdparty)
if(MSVC)
  target_compile_options(dg_mapgen PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_executable(mapgen_preview tools/mapgen_preview/mapgen_preview.c)
set_target_properties(mapgen_preview PROPERTIES C_STANDARD 11)
target_link_libraries(mapgen_preview PRIVATE dg_mapgen)
add_executable(test_mapgen tests/test_mapgen.c)
set_target_properties(test_mapgen PROPERTIES C_STANDARD 11)
target_link_libraries(test_mapgen PRIVATE dg_mapgen)
add_dependencies(test_mapgen dg_copy_data)
add_test(NAME mapgen COMMAND test_mapgen WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Character pipeline (docs/CHARACTERS.md): Skeleton / SkeletalMesh bakes, part splitting, retargeting.
foreach(tool bake_skeleton bake_mesh split_parts retarget)
  add_executable(${tool} tools/${tool}/${tool}.c)
  set_target_properties(${tool} PROPERTIES C_STANDARD 11)
  target_link_libraries(${tool} PRIVATE dg_animcore)
endforeach()

# Hand poses (docs/HANDPOSE.md): unreviewed / auto-placed / orphaned entries, optional stub creation.
add_executable(handpose_report tools/handpose_report/handpose_report.c)
set_target_properties(handpose_report PROPERTIES C_STANDARD 11)
target_link_libraries(handpose_report PRIVATE dg_animcore)
# Schema 1 (per state) -> universal grips + cases, with a report of the dropped differences.
add_executable(handpose_migrate tools/handpose_migrate/handpose_migrate.c)
set_target_properties(handpose_migrate PROPERTIES C_STANDARD 11)
target_link_libraries(handpose_migrate PRIVATE dg_animcore)

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

# Sky-driven shadows (docs/SHADOWS.md): shadows_sky.json, source radius curves, the PCSS kernel's
# penumbra width vs d x tan(r), lit / shade contrast, cutoff hysteresis over a 2 min transition, moon
# handover, cloud drift, tiers, TAA on a softening edge.
add_executable(test_shadow_sky tests/test_shadow_sky.c src/render/shadow_sky.c)
set_target_properties(test_shadow_sky PROPERTIES C_STANDARD 11)
target_link_libraries(test_shadow_sky PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_shadow_sky PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME shadow_sky COMMAND test_shadow_sky WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
add_executable(test_shadow_csm tests/test_shadow_csm.c src/render/shadow_csm.c src/render/shadow_settings.c third_party/cjson/cJSON.c)
set_target_properties(test_shadow_csm PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_shadow_csm PRIVATE src third_party/cjson)
if(MSVC)
  target_compile_options(test_shadow_csm PRIVATE /W3 /wd4201 /utf-8)
else()
  target_link_libraries(test_shadow_csm PRIVATE m)
endif()
add_test(NAME shadow_csm COMMAND test_shadow_csm WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
add_executable(test_shadow_settings tests/test_shadow_settings.c src/render/shadow_settings.c src/render/shadow_csm.c third_party/cjson/cJSON.c)
set_target_properties(test_shadow_settings PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_shadow_settings PRIVATE src third_party/cjson)
if(MSVC)
  target_compile_options(test_shadow_settings PRIVATE /W3 /wd4201 /utf-8)
else()
  target_link_libraries(test_shadow_settings PRIVATE m)
endif()
add_test(NAME shadow_settings COMMAND test_shadow_settings WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Foliage wind (docs/FOLIAGE_WIND.md): the wind field (gust fronts, tile, springs, sources), the Crysis
# vegetation WPO CPU mirror (trunk length, layer linearity, phase decorrelation, bounds), the Tree It
# vertex-colour channel detection over assets/treefbx.
add_executable(test_wind tests/test_wind.c)
set_target_properties(test_wind PROPERTIES C_STANDARD 11)
target_link_libraries(test_wind PRIVATE dg_vgeo)
add_test(NAME wind COMMAND test_wind WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Terrain shading (docs/TERRAIN_SHADING.md): pure modules (parameters, baked maps, the CPU mirror of the
# shader's blending / tiling) and their test: assets/terrain parses, maps vs CPU references, border
# distance / band width / height blend / noise borders / biases, hex bombing autocorrelation + histogram.
add_library(dg_terrain STATIC src/world/terrain/terrain_params.c src/world/terrain/terrain_maps.c src/world/terrain/terrain_blend.c)
set_target_properties(dg_terrain PROPERTIES C_STANDARD 11)
target_link_libraries(dg_terrain PUBLIC dg_animcore)
add_executable(test_terrain tests/test_terrain.c)
set_target_properties(test_terrain PROPERTIES C_STANDARD 11)
target_link_libraries(test_terrain PRIVATE dg_terrain)
add_dependencies(test_terrain dg_copy_data)
add_test(NAME terrain COMMAND test_terrain WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Grass assets (docs/FOLIAGE_GRASS.md): the JSON assets against the target spec, clump meshes (extent,
# cells, root -> tip), the placement rule's clumps per m2, materials per fidelity level.
add_executable(test_grass tests/test_grass.c)
set_target_properties(test_grass PROPERTIES C_STANDARD 11)
target_link_libraries(test_grass PRIVATE dg_vgeo)
add_dependencies(test_grass dg_copy_data)
add_test(NAME grass COMMAND test_grass WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Water (docs/WATER.md): deterministic polynomials, swell spectrum / continuity / determinism, body
# shapes and queries, buoyancy (analytic, Jolt half-submersion / drag / sinking, 200-body budget).
# water_determinism runs the wave fingerprint in two processes and compares them.
add_executable(test_water tests/test_water.c src/physics/physics.c src/physics/buoyancy.c src/core/jobs.c)
set_target_properties(test_water PROPERTIES C_STANDARD 11)
target_link_libraries(test_water PRIVATE dg_animcore joltc jolt_shim)
if(MSVC)
  target_compile_options(test_water PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME water COMMAND test_water WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
set_tests_properties(water PROPERTIES RUN_SERIAL TRUE)   # the 200-body CPU budget needs the cores
add_test(NAME water_determinism COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:test_water> -P ${CMAKE_SOURCE_DIR}/tools/tests/water_determinism.cmake)

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
add_executable(test_fire_anim tests/test_fire_anim.c)
set_target_properties(test_fire_anim PROPERTIES C_STANDARD 11)
target_link_libraries(test_fire_anim PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_fire_anim PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME fire_anim COMMAND test_fire_anim)

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

# Character skinning vs the source FBX (file weights + inverse binds on the shared skeleton's pose):
# legacy baked vertices / palette and the vgeo build of them, per mesh, over strong-bend clips.
add_executable(test_skin_reference tests/test_skin_reference.c)
set_target_properties(test_skin_reference PROPERTIES C_STANDARD 11)
target_link_libraries(test_skin_reference PRIVATE dg_vgeo)
if(MSVC)
  target_compile_options(test_skin_reference PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME skin_reference COMMAND test_skin_reference WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

add_executable(test_anim tests/test_anim.c)
set_target_properties(test_anim PROPERTIES C_STANDARD 11)
target_link_libraries(test_anim PRIVATE dg_animcore)
add_test(NAME anim COMMAND test_anim WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Hand poses (docs/HANDPOSE.md): defaults vs the old curl tables, inheritance, round trip, mirror,
# limits, contact solver, glue with tweaked poses, discovery / auto-registration, editor == gameplay.
add_executable(test_handpose tests/test_handpose.c)
set_target_properties(test_handpose PROPERTIES C_STANDARD 11)
target_link_libraries(test_handpose PRIVATE dg_animcore)
add_test(NAME handpose COMMAND test_handpose WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
add_executable(test_hp_edit tests/test_hp_edit.c)
set_target_properties(test_hp_edit PROPERTIES C_STANDARD 11)
target_link_libraries(test_hp_edit PRIVATE dg_animcore)
add_test(NAME hp_edit COMMAND test_hp_edit)
add_executable(test_handpose_rig tests/test_handpose_rig.c)
set_target_properties(test_handpose_rig PROPERTIES C_STANDARD 11)
target_link_libraries(test_handpose_rig PRIVATE dg_animcore)
add_test(NAME handpose_rig COMMAND test_handpose_rig)

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

# Main menu + lobby (docs/MENU_LOBBY.md): lobby state machine, membership / kick / ready / start,
# chat, version check, wire, maps, cosmetics round trip, profile, seasons + weather bias, screen lint.
add_executable(test_lobby tests/test_lobby.c src/net/lobby.c src/world/maplist.c src/world/season.c src/gameplay/cosmetics.c src/game/profile.c src/ui/gui/gui_layout.c src/core/cvar.c)
set_target_properties(test_lobby PROPERTIES C_STANDARD 11)
target_link_libraries(test_lobby PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_lobby PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_dependencies(test_lobby dg_copy_data)
add_test(NAME lobby COMMAND test_lobby WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
# HUD (docs/UI_WHEELS_HUD.md): radial wheel math + JSON, timed ring, pings, kill stats, compass, the
# console registry categories; the registry lint (descriptions + categories of every registration).
add_executable(test_hud tests/test_hud.c src/ui/widgets/radial_math.c src/ui/widgets/wheel_def.c src/gameplay/ping.c src/core/cmd_registry.c)
set_target_properties(test_hud PROPERTIES C_STANDARD 11)
target_compile_definitions(test_hud PRIVATE DG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_hud PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_hud PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_dependencies(test_hud dg_copy_data)
add_test(NAME hud COMMAND test_hud WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
# Downed / revive / corpse / DNA / fabricator state machines + the death sequence (docs/REVIVE.md).
add_executable(test_revive tests/test_revive.c src/gameplay/revive/revive_core.c src/gameplay/revive/death_seq.c src/audio/death_synth.c third_party/cjson/cJSON.c)
set_target_properties(test_revive PROPERTIES C_STANDARD 11)
target_include_directories(test_revive PRIVATE src third_party/cjson)
if(MSVC)
  target_compile_options(test_revive PRIVATE /W3 /wd4100 /wd4201 /utf-8)
else()
  target_link_libraries(test_revive PRIVATE m)
endif()
add_dependencies(test_revive dg_copy_data)
add_test(NAME revive COMMAND test_revive WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME lint_commands COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/lint_commands.py)
  add_test(NAME aim_sockets COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/aim_sockets.py)
  add_test(NAME lint_looks COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/lint_looks.py)
  # Ground textures (docs/GROUND_REFERENCE.md): the recipes validate, the generated / placeholder sets
  # meet their colour, coverage, gradient, silhouette and seam checks (needs numpy + Pillow).
  add_test(NAME ground_recipes COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/vfxgen/groundgen.py --validate)
  add_test(NAME ground_textures COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/ground_tex_check.py)
endif()
# Post-processing Looks (docs/LOOKS.md): five fixed looks, the analytic grade vs the baked LUT, Realism
# identity, PUBG-style / Cinematic / Vibrant / Gritty character, field isolation, fidelity caps,
# modulators. Built twice: dev (look_tune compiled in) and release (compiled out).
add_executable(test_looks tests/test_looks.c src/render/post/look.c src/render/post/looks.c)
add_executable(test_looks_release tests/test_looks.c src/render/post/look.c src/render/post/looks.c)
target_compile_definitions(test_looks PRIVATE DG_LOOK_TUNE=1)
target_compile_definitions(test_looks_release PRIVATE DG_LOOK_TUNE=0)
foreach(t test_looks test_looks_release)
  set_target_properties(${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_include_directories(${t} PRIVATE src)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W3 /wd4100 /wd4201 /utf-8)
  else()
    target_link_libraries(${t} PRIVATE m)
  endif()
endforeach()
add_test(NAME looks COMMAND test_looks)
add_test(NAME looks_release COMMAND test_looks_release)
# Engine Fidelity (docs/ENGINE_FIDELITY.md): tables, bounds, apply / clamp on the real cvar system, era
# matrix, frame graph validation.
add_executable(test_fidelity tests/test_fidelity.c src/render/fidelity/fidelity.c src/render/graph/fg_check.c src/core/cvar.c)
set_target_properties(test_fidelity PROPERTIES C_STANDARD 11)
target_link_libraries(test_fidelity PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_fidelity PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME fidelity COMMAND test_fidelity WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# LOD ranges and cross-fades (docs/LOD.md): lod.json x tier, character px -> LOD, fades in a slow dolly,
# camera reversal, complementary masks, streaming holds, terrain geomorph without cracks / pops.
add_executable(test_lod tests/test_lod.c src/render/lod_fade.c src/render/fidelity/fidelity.c src/core/cvar.c)
set_target_properties(test_lod PROPERTIES C_STANDARD 11)
target_link_libraries(test_lod PRIVATE dg_animcore)
if(MSVC)
  target_compile_options(test_lod PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME lod COMMAND test_lod WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

# Phone (docs/PHONE.md): QR round trip, pairing auth, HTTP / WebSocket, orientation math, filter +
# reach clamp, JPEG pipeline at 30 fps, photo / video files + sidecars, headless client round trip, TLS.
set(DG_PHONE_CORE_SOURCES
  src/net/http_ws/hws_proto.c src/net/http_ws/hws_auth.c src/net/http_ws/hws_tls.c src/net/http_ws/hws_server.c
  src/gameplay/phone/phone_stream.c src/gameplay/phone/phone_stream_ws.c
  src/gameplay/phone/phone_video.c src/gameplay/phone/phone_capture.c src/gameplay/phone/phone_qr.c src/gameplay/phone/phone_os.c)
file(GLOB DG_QUIRC_SOURCES third_party/quirc/*.c)
add_executable(test_phone tests/test_phone.c ${DG_PHONE_CORE_SOURCES} ${DG_QUIRC_SOURCES} src/core/jobs.c)
set_target_properties(test_phone PROPERTIES C_STANDARD 11)
target_include_directories(test_phone PRIVATE third_party)
target_link_libraries(test_phone PRIVATE dg_animcore mbedtls mbedx509 mbedcrypto)
if(MSVC)
  target_compile_options(test_phone PRIVATE /W3 /wd4100 /wd4201 /utf-8)
  target_link_libraries(test_phone PRIVATE ws2_32 iphlpapi)
endif()
add_test(NAME phone COMMAND test_phone WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)

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

# Virtualized geometry page streaming (cold start, convergence, small pool, eviction audit).
add_executable(test_vgeo_stream tests/test_vgeo_stream.c tests/vgeo_gpu_harness.c)
set_target_properties(test_vgeo_stream PROPERTIES C_STANDARD 11)
target_link_libraries(test_vgeo_stream PRIVATE dg_vgeo_gpu)
add_dependencies(test_vgeo_stream dg_shaders)
if(MSVC)
  target_compile_options(test_vgeo_stream PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME vgeo_stream COMMAND test_vgeo_stream --validation WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
if(EXISTS "${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
  set_tests_properties(vgeo_stream PROPERTIES ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
endif()

# Virtualized geometry skinned meshes (Phase 6): animated bounds, crack-free animated cuts, GPU cut vs
# the CPU reference, HW vs SW raster and the shared skinning helper on the harness.
add_executable(test_vgeo_skinned tests/test_vgeo_skinned.c tests/vgeo_gpu_harness.c)
set_target_properties(test_vgeo_skinned PROPERTIES C_STANDARD 11)
target_link_libraries(test_vgeo_skinned PRIVATE dg_vgeo_gpu)
add_dependencies(test_vgeo_skinned dg_shaders)
if(MSVC)
  target_compile_options(test_vgeo_skinned PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME vgeo_skinned COMMAND test_vgeo_skinned --validation WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
if(EXISTS "${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
  set_tests_properties(vgeo_skinned PROPERTIES ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
endif()

# Virtualized geometry foliage support (Phase 7): masked SW / HW agreement + CPU alpha reference, WPO
# bounds, density hash, cell culling vs brute force, impostor switch, placement streaming, Hi-Z.
add_executable(test_vgeo_foliage tests/test_vgeo_foliage.c tests/vgeo_gpu_harness.c)
set_target_properties(test_vgeo_foliage PROPERTIES C_STANDARD 11)
target_link_libraries(test_vgeo_foliage PRIVATE dg_vgeo_gpu)
add_dependencies(test_vgeo_foliage dg_shaders)
if(MSVC)
  target_compile_options(test_vgeo_foliage PRIVATE /W3 /wd4100 /wd4201 /utf-8)
endif()
add_test(NAME vgeo_foliage COMMAND test_vgeo_foliage --validation WORKING_DIRECTORY $<TARGET_FILE_DIR:deadground>)
if(EXISTS "${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
  set_tests_properties(vgeo_foliage PROPERTIES ENVIRONMENT "VK_ADD_LAYER_PATH=${CMAKE_SOURCE_DIR}/.toolchain/vvl/install/bin")
endif()
