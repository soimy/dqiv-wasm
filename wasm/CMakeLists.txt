# Cross-build for wasm32 (Emscripten).
#
# This is deliberately ADDITIVE: the upstream Visual Studio 2022 / Win32 / MSVC
# build is left working, so merges from upstream stay trivial. Source lists are
# shared with it through ../cmake/Sources.cmake, so a new upstream file is
# registered once. What stays here is what genuinely differs: the compiler
# flags, the SDL wiring, and the portable stand-in for the Windows-only files.
#
# What this build does NOT do yet (see docs/porting/03-ROADMAP.md):
#   * no title screen: GameBoot::openResources() needs
#     data/SOUND/sound_data.sdat, so the game stops before it can draw a picture,
#     and that is M5. What is here is the pipeline running on a real GLES 3.0
#     context, plus src/gpu-selfcheck.cpp, which puts known answers through the
#     real shaders and prints what comes back.
#   * no DS overlay section bracketing: cmake/NativeOverlays.cmake injects MSVC
#     #pragma init_seg/data_seg/bss_seg through /FI. The game compiles without it,
#     but the runtime snapshot/restore of overlay modules is not wired up yet.
#   * native/SaveStore.cpp, native/NitroThreads.cpp and native/NitroIdentity.cpp
#     are Windows-only upstream and are replaced by src/port-stubs.cpp
#     (native/DssRuntime.cpp, in the upstream exe target, turned out to be fully
#     portable and is compiled for real here).
#   * TextCanvas has no rasterizer on wasm, so the FPS overlay keeps its
#     rectangle and loses its text. autoQuality is a no-op here for the same kind
#     of reason: the GPU-timing path that drives it is compiled out.
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

# The real game, cross-built for wasm32. It links and it runs: in node it gets
# through the whole decompiled startup and stops where the desktop path would
# need a GL context, which is exactly the point a browser takes over.
# src/early-contexts.cpp goes first on purpose. Within a target clang emits static
# initialisers in source order, and exe objects are linked before library objects,
# so this file's constructors run before the game's own - both measured. It has to
# be first because clang ignores #pragma init_seg, which on MSVC is what delays the
# game's constructors until the overlay registry loads overlay 38.
add_executable(dqiv_wasm_game
    src/early-contexts.cpp
    ${DQIV_ROOT}/native/GameBoot.cpp ${DQIV_ROOT}/native/GameWindow.cpp
    ${DQIV_ROOT}/native/GpuRenderer.cpp ${DQIV_ROOT}/native/GlLoader.cpp
    ${DQIV_ROOT}/native/TextCanvas.cpp ${DQIV_ROOT}/native/FramePresenter.cpp
    ${DQIV_ROOT}/native/NitroSymbols.cpp ${DQIV_ROOT}/native/CrashHandler.cpp
    ${DQIV_ROOT}/native/InputBindings.cpp ${DQIV_ROOT}/native/AudioOutput.cpp
    src/gpu-selfcheck.cpp)
target_link_libraries(dqiv_wasm_game PRIVATE
    dqiv_game dqiv_nitro_system dqiv_sound dqiv_platform dqiv_portable)
# Emscripten turns off exception catching by default, so a throw that a try/catch
# should have caught aborts instead, and the message is lost. GameBoot reports its
# failures through an exception, so catching has to be on for this target.
#
# MAX_WEBGL_VERSION has to be asked for. It defaults to 1, and SDL's emscripten
# driver only raises the context to WebGL 2 when the requested GL major version is
# 3 (which is what GameWindow now asks for), so without this the shaders - every
# one of them #version 300 es - would be compiled against GLES 2.0 and fail.
#
# Memory. Measured: the game ran out during its own initialisation, at an operator
# new, with the heap at 18481152 bytes - that is what Emscripten worked out for
# itself from the static data, and it is not enough. Growth is the practical answer
# for a game that allocates as it goes. The cost is stated: growing copies the whole
# heap, so it is a pause, and the starting figure is raised so there are fewer of
# them rather than one big one at the worst moment.
#
# Measured, both directions, and the result is the opposite of what I first assumed:
# 64 MB is the figure that keeps the browser responsive on a 128 MB ROM (every poll
# answered, 1-4 ms), and raising it to 320 MB is what made the page stop answering
# 1+1 from the moment the file was handed over. So the starting figure stays at the
# measured working value and growth stays on, pause and all.
#
# Asyncify, and the reason is measured: five shapes of the same job all blocked the
# page's own main thread on a 128 MB ROM (extract, ROM as source, source cached so a
# file is a slice, 64 MB heap, 320 MB heap), while a 1 MB file kept it answering every
# poll. So the blocker is putting a source this size through the virtual filesystem,
# and the only lever that lets the main thread yield during it is this. It is
# compatible with the single-threaded cooperative scheduling we chose - no
# SharedArrayBuffer, no COOP/COEP headers. The cost is stated, not hidden: Asyncify
# instruments every function that can pause, so the whole game gets slower, not just
# this part.
target_link_options(dqiv_wasm_game PRIVATE -sDISABLE_EXCEPTION_CATCHING=0 -sMAX_WEBGL_VERSION=2
    -sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1 -sASYNCIFY=1)
