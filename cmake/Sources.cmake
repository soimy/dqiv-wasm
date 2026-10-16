# The single place where the port's targets register their files.
#
# Included by BOTH the upstream Win32/MSVC build (root CMakeLists.txt) and the
# wasm32 cross-build (wasm/CMakeLists.txt), so a new upstream file is registered
# once instead of twice. Every Windows-only file sits behind if(WIN32); a
# non-Windows build gets the portable stand-in from dqiv_portable_sources()
# instead. This closes the duplication that docs/porting/03-ROADMAP.md calls M2.
#
# Each function takes the repository root as its first argument and returns the
# list through its second argument (caller scope).
#
# Two consequences worth stating plainly:
#   * Paths are absolute here, so the generated Visual Studio project lists its
#     sources with absolute paths. The Windows build *product* is unchanged.
#   * Source order inside each target is preserved exactly, including the
#     position of the Windows-only files, so link order on Windows stays as it
#     was before this file existed.

# Public-domain Ogg Vorbis decoder, used for the smartphone version's music.
function(dqiv_vorbis_sources root out)
    set(${out} "${root}/native/VorbisDecoder.c" PARENT_SCOPE)
endfunction()

# PC platform: memory, math, video, 3D, display, sound mixing, files, settings.
function(dqiv_core_sources root out)
    set(result
        "${root}/game/src/main/dss/Random.cpp"
        "${root}/game/src/main/dss/Utf8Iterator.cpp"
        "${root}/game/src/main/data/DataObject.cpp"
        "${root}/game/src/main/dss/Pad.cpp"
        "${root}/game/src/main/global/GlobalDQ4.cpp"
        "${root}/game/src/main/dss/DssFix.cpp"
        "${root}/game/src/main/dss/DssVector.cpp"
        "${root}/native/Entropy.cpp"
        "${root}/native/FixedMath.cpp"
        "${root}/native/NitroMath.cpp"
        "${root}/native/NitroMemory.cpp"
        "${root}/native/NitroAlgorithms.cpp"
        "${root}/game/libs/NitroSDK/src/os/os_printf.c"
        "${root}/native/NitroOs.cpp"
        "${root}/native/GxGeometry.cpp"
        "${root}/native/GxTextures.cpp"
        "${root}/native/GxRasterizer.cpp"
        "${root}/native/NitroVideo.cpp"
        "${root}/native/Display.cpp"
        "${root}/native/HdPictures.cpp"
        "${root}/game/libs/NitroSDK/src/gx/gx.c"
        "${root}/game/libs/NitroSDK/src/gx/gx_vramcnt.c"
        "${root}/game/libs/NitroSDK/src/gx/gx_bgcnt.c"
        "${root}/game/libs/NitroSDK/src/gx/gx_load2d.c"
        "${root}/game/libs/NitroSDK/src/gx/gx_load3d.c"
        "${root}/game/libs/NitroSDK/src/gx/g2.c"
        "${root}/native/NitroGeometry.cpp"
        "${root}/game/libs/NitroSDK/src/gx/g3_util.c")
    if(WIN32)
        list(APPEND result "${root}/native/SaveStore.cpp")
    endif()
    list(APPEND result
        "${root}/native/SoundWave.cpp"
        "${root}/native/SoundMixer.cpp"
        "${root}/game/libs/NitroSDK/src/fx/fx_atan.c"
        "${root}/game/libs/NitroSDK/src/fx/fx_trig.c"
        "${root}/game/libs/NitroSDK/src/fx/fx_mtx22.c"
        "${root}/game/libs/NitroSDK/src/fx/fx_mtx33.c"
        "${root}/game/libs/NitroSDK/src/fx/fx_mtx43.c"
        "${root}/game/libs/NitroSDK/src/fx/fx_mtx44.c"
        "${root}/game/libs/NitroSDK/src/fx/fx_vec.c"
        "${root}/game/libs/NitroSDK/src/std/std_string.c"
        "${root}/native/Assets.cpp"
        "${root}/native/AssetPack.cpp"
        "${root}/native/GameInstall.cpp"
        "${root}/native/Archive.cpp"
        "${root}/native/Compression.cpp"
        "${root}/native/GameServices.cpp"
        "${root}/native/Input.cpp"
        "${root}/native/Texture.cpp"
        "${root}/native/DualScreen.cpp"
        "${root}/native/Settings.cpp"
        "${root}/native/MobileSound.cpp"
        "${root}/native/Profile.cpp"
    )
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The original game (decompiled sources), each DS overlay in its own module.
# The four exclusions are files that belong to dqiv_core, plus the per-overlay
# padding the generator emits, plus main.cpp.
function(dqiv_game_sources root out)
    file(GLOB_RECURSE result CONFIGURE_DEPENDS "${root}/game/src/*.cpp")
    list(FILTER result EXCLUDE REGEX "/ModulePadding_[^/]+\\.cpp$")
    list(FILTER result EXCLUDE REGEX "/main/main\\.cpp$")
    list(FILTER result EXCLUDE REGEX "/dss/(Random|Utf8Iterator|Pad|DssFix|DssVector|DssCore|UnkMemory)\\.cpp$")
    list(FILTER result EXCLUDE REGEX "/(DataObject|GlobalDQ4)\\.cpp$")
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# DS power/wireless/overlay/file APIs.
function(dqiv_platform_sources root out)
    set(result
        "${root}/native/NitroFilesystem.cpp"
        "${root}/native/NitroBackup.cpp"
        "${root}/native/NitroPower.cpp")
    if(WIN32)
        list(APPEND result "${root}/native/NitroThreads.cpp")
    endif()
    list(APPEND result "${root}/native/NitroOverlays.cpp")
    if(WIN32)
        list(APPEND result "${root}/native/NitroIdentity.cpp")
    endif()
    list(APPEND result
        "${root}/native/NitroWireless.cpp"
        "${root}/game/libs/NitroSDK/src/os/os_message.c"
        "${root}/game/libs/NitroSDK/src/wm/wm_gameinfo.c")
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The NitroSDK sound driver.
function(dqiv_sound_sources root out)
    set(${out}
        "${root}/native/NitroSound.cpp"
        "${root}/native/SoundVoices.cpp"
        "${root}/native/SoundSequence.cpp"
        "${root}/game/libs/NitroSDK/src/snd/snd_main.c"
        "${root}/game/libs/NitroSDK/src/snd/snd_interface.c"
        "${root}/game/libs/NitroSDK/src/snd/snd_command.c"
        "${root}/game/libs/NitroSDK/src/snd/snd_alarm.c"
        "${root}/game/libs/NitroSDK/src/snd/snd_work.c"
        "${root}/game/libs/NitroSDK/src/snd/snd_bank.c"
        "${root}/game/libs/NitroSDK/src/snd/snd_util.c"
        PARENT_SCOPE)
endfunction()

# The NitroSystem (NNS) library, plus the model code that lives in this target.
function(dqiv_nitro_system_sources root out)
    file(GLOB_RECURSE result CONFIGURE_DEPENDS "${root}/game/libs/NitroSystem/src/*.c")
    list(APPEND result "${root}/native/NitroModel.cpp")
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The stand-in for the three Windows-only platform files above. Empty on Windows
# (where the originals are already in the lists), the portable implementation
# elsewhere. Only the wasm build creates a target from it.
function(dqiv_portable_sources root out)
    if(WIN32)
        set(result "")
    else()
        set(result "${root}/wasm/src/port-stubs.cpp")
    endif()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# Platform-layer files that upstream keeps in the executable target but which are
# fully portable, so the wasm build compiles them for real. Verified in
# docs/porting/02-FINDINGS.md section 2.
function(dqiv_portable_platform_sources root out)
    set(${out} "${root}/native/DssRuntime.cpp" PARENT_SCOPE)
endfunction()
