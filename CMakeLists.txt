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

# meshoptimizer (simplifier only): generated character LODs.
add_library(meshopt STATIC third_party/meshoptimizer/simplifier.cpp third_party/meshoptimizer/allocator.cpp)
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
  ${CMAKE_SOURCE_DIR}/shaders/sky/*.comp)
file(GLOB DG_SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.glsl ${CMAKE_SOURCE_DIR}/shaders/post/*.glsl ${CMAKE_SOURCE_DIR}/shaders/sights/*.glsl ${CMAKE_SOURCE_DIR}/shaders/sky/*.glsl ${CMAKE_SOURCE_DIR}/third_party/fsr/*.h
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
  src/anim/anim.c src/anim/anim_mesh.c src/anim/controller.c src/anim/procedural.c src/anim/turn.c src/anim/spine.c
  src/anim/weapon_hold.c src/anim/damage_additive.c src/anim/foot_ik.c src/anim/anim_net.c src/anim/ragdoll_def.c
  src/anim/anim_library.c src/anim/character_rt.c src/gameplay/hitboxes.c src/gameplay/items.c src/gameplay/attachments.c src/gameplay/recoil.c src/core/spring.c
  src/world/time_of_day.c src/vfx/vfx_curve.c src/vfx/vfx_library.c src/vfx/vfx_sim.c src/vfx/vfx_atlas.c src/player/holdpoint.c src/player/camera_fp_math.c
  src/assets/fbx_loader.c src/assets/anim_cache.c src/assets/character.c
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
