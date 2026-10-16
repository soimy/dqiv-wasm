# Cross-build for wasm32 (Emscripten).
#
# This is deliberately ADDITIVE: the upstream Visual Studio 2022 / Win32 / MSVC
# build is left working, so merges from upstream stay trivial. Source lists are
# shared with it through ../cmake/Sources.cmake, so a new upstream file is
# registered once. What stays here is what genuinely differs: the compiler
# flags, the SDL wiring, and the portable stand-in for the Windows-only files.
#
# What this build does NOT do yet (see docs/porting/02-FINDINGS.md):
#   * no renderer: native/GameWindow.cpp, GpuRenderer.cpp, GlLoader.cpp,
#     TextCanvas.cpp, FramePresenter.cpp, NitroSymbols.cpp, DssRuntime.cpp and
#     GameBoot.cpp are the platform layer and still assume a desktop GL 3.3
#     window. Only the libraries and a probe executable are built here.
#   * no DS overlay section bracketing: cmake/NativeOverlays.cmake injects MSVC
#     #pragma init_seg/data_seg/bss_seg through /FI. The game compiles without it,
#     but the runtime snapshot/restore of overlay modules is not wired up yet.
#   * native/SaveStore.cpp, native/NitroThreads.cpp and native/NitroIdentity.cpp
#     are Windows-only upstream and are replaced by src/port-stubs.cpp
#     (native/DssRuntime.cpp, in the upstream exe target, turned out to be fully
#     portable and is compiled for real here).
cmake_minimum_required(VERSION 3.24)
project(DQIV_WASM LANGUAGES C CXX)

set(DQIV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
include("${DQIV_ROOT}/cmake/Sources.cmake")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "wasm32 must use 4-byte pointers, got ${CMAKE_SIZEOF_VOID_P}")
endif()
message(STATUS "wasm32: sizeof(void*) == ${CMAKE_SIZEOF_VOID_P}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SDL3 comes from emscripten's port (VIDEO / GAMEPAD / AUDIO / GL all supported).
add_compile_options(-sUSE_SDL=3)
add_link_options(-sUSE_SDL=3)

# The decompiled game is full of intentional narrowing (a menu table's `-1`
# terminator into an `unsigned char` field, 0xa000002b-style DS addresses into
# `int`, ...). MSVC accepts it, Clang rejects it by default: 234 sites across 28
# files. Their values are unchanged by the narrowing (two's complement, matching
# the original ARM code), so the diagnostic is disabled here instead of editing
# hundreds of decompiler-generated lines. See docs/porting/02-FINDINGS.md.
add_compile_options(-Wno-c++11-narrowing)

# Ogg Vorbis decoder (public domain) for the smartphone version's music.
set(DQIV_STB_VORBIS "${CMAKE_BINARY_DIR}/_deps/stb/stb_vorbis.c")
if(NOT EXISTS "${DQIV_STB_VORBIS}")
    file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c
        "${DQIV_STB_VORBIS}"
        EXPECTED_HASH SHA256=4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636)
endif()
dqiv_vorbis_sources(${DQIV_ROOT} DQIV_VORBIS)
add_library(dqiv_vorbis STATIC ${DQIV_VORBIS})
target_include_directories(dqiv_vorbis PRIVATE "${CMAKE_BINARY_DIR}/_deps/stb")

# PC platform: memory, math, video, 3D, display, sound mixing, files, settings.
# Same list as the upstream dqiv_core target, minus the Windows-only SaveStore.
dqiv_core_sources(${DQIV_ROOT} DQIV_CORE)
add_library(dqiv_core STATIC ${DQIV_CORE})
target_include_directories(dqiv_core PUBLIC
    ${DQIV_ROOT}/native ${DQIV_ROOT}/native/compat ${DQIV_ROOT}/game/include
    ${DQIV_ROOT}/game/libs/NitroSDK/include ${DQIV_ROOT}/game/libs/NitroSystem/include)
target_compile_definitions(dqiv_core PUBLIC DQIV_NATIVE=1)
target_link_libraries(dqiv_core PUBLIC dqiv_vorbis)

add_library(dqiv_audio STATIC ${DQIV_ROOT}/native/AudioOutput.cpp)
target_link_libraries(dqiv_audio PUBLIC dqiv_core)

# The original game (decompiled sources), each DS overlay in its own module.
dqiv_game_sources(${DQIV_ROOT} DQIV_GAME_SOURCES)
list(LENGTH DQIV_GAME_SOURCES DQIV_GAME_COUNT)
message(STATUS "decompiled game translation units: ${DQIV_GAME_COUNT}")
add_library(dqiv_game STATIC EXCLUDE_FROM_ALL ${DQIV_GAME_SOURCES})
target_link_libraries(dqiv_game PUBLIC dqiv_core)

dqiv_platform_sources(${DQIV_ROOT} DQIV_PLATFORM)
dqiv_portable_platform_sources(${DQIV_ROOT} DQIV_EXE_PLATFORM)
add_library(dqiv_platform STATIC ${DQIV_PLATFORM} ${DQIV_EXE_PLATFORM})
target_link_libraries(dqiv_platform PUBLIC dqiv_core)

dqiv_sound_sources(${DQIV_ROOT} DQIV_SOUND)
add_library(dqiv_sound STATIC ${DQIV_SOUND})
target_link_libraries(dqiv_sound PUBLIC dqiv_platform)

dqiv_nitro_system_sources(${DQIV_ROOT} DQIV_NNS_SOURCES)
add_library(dqiv_nitro_system STATIC EXCLUDE_FROM_ALL ${DQIV_NNS_SOURCES})
target_link_libraries(dqiv_nitro_system PUBLIC dqiv_core)

# The Windows-only platform files, replaced by a small portable implementation.
dqiv_portable_sources(${DQIV_ROOT} DQIV_PORTABLE)
add_library(dqiv_portable STATIC ${DQIV_PORTABLE})
target_link_libraries(dqiv_portable PUBLIC dqiv_core)

# Proof that the whole decompiled game links into one wasm module: it runs a real
# decompiled function (dss::arrayToIndex) and reports the layout sizes.
add_executable(dqiv_wasm_spike src/spike-main.cpp)
target_link_libraries(dqiv_wasm_spike PRIVATE
    dqiv_game dqiv_nitro_system dqiv_sound dqiv_platform dqiv_portable
    ${DQIV_ROOT}/native/InputBindings.cpp)

# Guard the 32-bit layout assumptions the original DS resources depend on.
add_executable(dqiv_layout_probe src/layout-probe.cpp)
target_link_libraries(dqiv_layout_probe PRIVATE dqiv_core)

# The real game, cross-built for wasm32. This is the first time the platform
# layer is compiled by anything other than MSVC, so it is the gate for all of the
# renderer work. It links; it does not run yet - there is no main loop and no
# assets, and a headless node has no WebGL to ask.
add_executable(dqiv_wasm_game
    ${DQIV_ROOT}/native/GameBoot.cpp ${DQIV_ROOT}/native/GameWindow.cpp
    ${DQIV_ROOT}/native/GpuRenderer.cpp ${DQIV_ROOT}/native/GlLoader.cpp
    ${DQIV_ROOT}/native/TextCanvas.cpp ${DQIV_ROOT}/native/FramePresenter.cpp
    ${DQIV_ROOT}/native/NitroSymbols.cpp ${DQIV_ROOT}/native/CrashHandler.cpp)
target_link_libraries(dqiv_wasm_game PRIVATE
    dqiv_game dqiv_nitro_system dqiv_sound dqiv_platform dqiv_portable)
