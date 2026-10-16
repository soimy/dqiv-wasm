# Cross-build for wasm32 (Emscripten).
#
# This is deliberately ADDITIVE: the upstream root CMakeLists.txt (Visual Studio
# 2022 / Win32 / MSVC) is left untouched, so the Windows build keeps working and
# merges from upstream stay trivial. The source lists below mirror the upstream
# targets; when upstream adds or moves files, update them here as well. That
# duplication is the first debt to pay off — see docs/porting/03-ROADMAP.md.
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
add_library(dqiv_vorbis STATIC ${DQIV_ROOT}/native/VorbisDecoder.c)
target_include_directories(dqiv_vorbis PRIVATE "${CMAKE_BINARY_DIR}/_deps/stb")

# PC platform: memory, math, video, 3D, display, sound mixing, files, settings.
# Same list as the upstream dqiv_core target.
add_library(dqiv_core STATIC
    ${DQIV_ROOT}/game/src/main/dss/Random.cpp
    ${DQIV_ROOT}/game/src/main/dss/Utf8Iterator.cpp
    ${DQIV_ROOT}/game/src/main/data/DataObject.cpp
    ${DQIV_ROOT}/game/src/main/dss/Pad.cpp
    ${DQIV_ROOT}/game/src/main/global/GlobalDQ4.cpp
    ${DQIV_ROOT}/game/src/main/dss/DssFix.cpp
    ${DQIV_ROOT}/game/src/main/dss/DssVector.cpp
    ${DQIV_ROOT}/native/Entropy.cpp
    ${DQIV_ROOT}/native/FixedMath.cpp
    ${DQIV_ROOT}/native/NitroMath.cpp
    ${DQIV_ROOT}/native/NitroMemory.cpp
    ${DQIV_ROOT}/native/NitroAlgorithms.cpp
    ${DQIV_ROOT}/game/libs/NitroSDK/src/os/os_printf.c
    ${DQIV_ROOT}/native/NitroOs.cpp
    ${DQIV_ROOT}/native/GxGeometry.cpp
    ${DQIV_ROOT}/native/GxTextures.cpp
    ${DQIV_ROOT}/native/GxRasterizer.cpp
    ${DQIV_ROOT}/native/NitroVideo.cpp
    ${DQIV_ROOT}/native/Display.cpp
    ${DQIV_ROOT}/native/HdPictures.cpp
    ${DQIV_ROOT}/game/libs/NitroSDK/src/gx/gx.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/gx/gx_vramcnt.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/gx/gx_bgcnt.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/gx/gx_load2d.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/gx/gx_load3d.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/gx/g2.c
    ${DQIV_ROOT}/native/NitroGeometry.cpp
    ${DQIV_ROOT}/game/libs/NitroSDK/src/gx/g3_util.c
    ${DQIV_ROOT}/native/SoundWave.cpp
    ${DQIV_ROOT}/native/SoundMixer.cpp
    ${DQIV_ROOT}/game/libs/NitroSDK/src/fx/fx_atan.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/fx/fx_trig.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/fx/fx_mtx22.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/fx/fx_mtx33.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/fx/fx_mtx43.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/fx/fx_mtx44.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/fx/fx_vec.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/std/std_string.c
    ${DQIV_ROOT}/native/Assets.cpp
    ${DQIV_ROOT}/native/AssetPack.cpp
    ${DQIV_ROOT}/native/GameInstall.cpp
    ${DQIV_ROOT}/native/Archive.cpp
    ${DQIV_ROOT}/native/Compression.cpp
    ${DQIV_ROOT}/native/GameServices.cpp
    ${DQIV_ROOT}/native/Input.cpp
    ${DQIV_ROOT}/native/Texture.cpp
    ${DQIV_ROOT}/native/DualScreen.cpp
    ${DQIV_ROOT}/native/Settings.cpp
    ${DQIV_ROOT}/native/MobileSound.cpp
    ${DQIV_ROOT}/native/Profile.cpp)
target_include_directories(dqiv_core PUBLIC
    ${DQIV_ROOT}/native ${DQIV_ROOT}/native/compat ${DQIV_ROOT}/game/include
    ${DQIV_ROOT}/game/libs/NitroSDK/include ${DQIV_ROOT}/game/libs/NitroSystem/include)
target_compile_definitions(dqiv_core PUBLIC DQIV_NATIVE=1)
target_link_libraries(dqiv_core PUBLIC dqiv_vorbis)

add_library(dqiv_audio STATIC ${DQIV_ROOT}/native/AudioOutput.cpp)
target_link_libraries(dqiv_audio PUBLIC dqiv_core)

# The original game (decompiled sources), each DS overlay in its own module.
file(GLOB_RECURSE DQIV_GAME_SOURCES CONFIGURE_DEPENDS "${DQIV_ROOT}/game/src/*.cpp")
list(FILTER DQIV_GAME_SOURCES EXCLUDE REGEX "/ModulePadding_[^/]+\\.cpp$")
list(FILTER DQIV_GAME_SOURCES EXCLUDE REGEX "/main/main\\.cpp$")
list(FILTER DQIV_GAME_SOURCES EXCLUDE REGEX "/dss/(Random|Utf8Iterator|Pad|DssFix|DssVector|DssCore|UnkMemory)\\.cpp$")
list(FILTER DQIV_GAME_SOURCES EXCLUDE REGEX "/(DataObject|GlobalDQ4)\\.cpp$")
list(LENGTH DQIV_GAME_SOURCES DQIV_GAME_COUNT)
message(STATUS "decompiled game translation units: ${DQIV_GAME_COUNT}")
add_library(dqiv_game STATIC EXCLUDE_FROM_ALL ${DQIV_GAME_SOURCES})
target_link_libraries(dqiv_game PUBLIC dqiv_core)

add_library(dqiv_platform STATIC
    ${DQIV_ROOT}/native/NitroFilesystem.cpp
    ${DQIV_ROOT}/native/NitroBackup.cpp
    ${DQIV_ROOT}/native/NitroPower.cpp
    ${DQIV_ROOT}/native/NitroOverlays.cpp
    ${DQIV_ROOT}/native/NitroWireless.cpp
    ${DQIV_ROOT}/native/DssRuntime.cpp
    ${DQIV_ROOT}/game/libs/NitroSDK/src/os/os_message.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/wm/wm_gameinfo.c)
target_link_libraries(dqiv_platform PUBLIC dqiv_core)

add_library(dqiv_sound STATIC
    ${DQIV_ROOT}/native/NitroSound.cpp
    ${DQIV_ROOT}/native/SoundVoices.cpp
    ${DQIV_ROOT}/native/SoundSequence.cpp
    ${DQIV_ROOT}/game/libs/NitroSDK/src/snd/snd_main.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/snd/snd_interface.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/snd/snd_command.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/snd/snd_alarm.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/snd/snd_work.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/snd/snd_bank.c
    ${DQIV_ROOT}/game/libs/NitroSDK/src/snd/snd_util.c)
target_link_libraries(dqiv_sound PUBLIC dqiv_core)

file(GLOB_RECURSE DQIV_NNS_SOURCES CONFIGURE_DEPENDS "${DQIV_ROOT}/game/libs/NitroSystem/src/*.c")
add_library(dqiv_nitro_system STATIC EXCLUDE_FROM_ALL ${DQIV_NNS_SOURCES} ${DQIV_ROOT}/native/NitroModel.cpp)
target_link_libraries(dqiv_nitro_system PUBLIC dqiv_core)

# 4 Windows-only platform files, replaced by a small portable implementation.
add_library(dqiv_portable STATIC src/port-stubs.cpp)
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
