cmake_minimum_required(VERSION 3.24)
project(Deadground VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
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
  third_party/cjson/cJSON.c)
target_include_directories(dg_thirdparty PUBLIC
  third_party/stb third_party/cgltf third_party/miniaudio third_party/cjson
  third_party/nuklear third_party/fastnoise third_party)
if(MSVC)
  target_compile_options(dg_thirdparty PRIVATE /W0)
else()
  target_compile_options(dg_thirdparty PRIVATE -w)
  target_link_libraries(dg_thirdparty PUBLIC m dl pthread)
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
  ${CMAKE_SOURCE_DIR}/shaders/*.vert ${CMAKE_SOURCE_DIR}/shaders/*.frag ${CMAKE_SOURCE_DIR}/shaders/*.comp)
file(GLOB DG_SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.glsl)
set(DG_SHADER_OUT ${CMAKE_BINARY_DIR}/bin/shaders)
set(DG_SPV_FILES)
foreach(src ${DG_SHADER_SOURCES})
  get_filename_component(name ${src} NAME)
  set(out ${DG_SHADER_OUT}/${name}.spv)
  if(DG_SHADER_COMPILER_KIND STREQUAL "glslc")
    set(cmd ${DG_SHADER_COMPILER} --target-env=vulkan1.3 -O -I ${CMAKE_SOURCE_DIR}/shaders -o ${out} ${src})
  else()
    set(cmd ${DG_SHADER_COMPILER} -V --target-env vulkan1.3 -I${CMAKE_SOURCE_DIR}/shaders -o ${out} ${src})
  endif()
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DG_SHADER_OUT}
    COMMAND ${cmd}
    DEPENDS ${src} ${DG_SHADER_INCLUDES}
    COMMENT "GLSL ${name}" VERBATIM)
  list(APPEND DG_SPV_FILES ${out})
endforeach()
add_custom_target(dg_shaders DEPENDS ${DG_SPV_FILES})

# ---------------------------------------------------------------- game
file(GLOB_RECURSE DG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.h)
add_executable(deadground ${DG_SOURCES})
add_dependencies(deadground dg_shaders)
target_include_directories(deadground PRIVATE src)
target_link_libraries(deadground PRIVATE SDL3::SDL3-static volk vma dg_thirdparty joltc jolt_shim enet)
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
  target_link_libraries(deadground PRIVATE ws2_32 winmm)
  target_compile_definitions(deadground PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
  target_compile_options(deadground PRIVATE /W3 /wd4100 /wd4201 /wd4324 /utf-8)
else()
  target_compile_options(deadground PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_libraries(deadground PRIVATE m pthread)
endif()
add_custom_command(TARGET deadground POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:deadground>/data
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:deadground>/assets)
