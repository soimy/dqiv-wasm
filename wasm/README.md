# `wasm/` — Emscripten cross-build scaffold

This directory cross-builds the decompiled game for **wasm32** without touching the
upstream MSVC/Win32 build. It is the starting point of the port described in
[`docs/porting/`](../docs/porting/).

## Build

```bash
source /path/to/emsdk/emsdk_env.sh     # once per shell
./wasm/build.sh                        # or: BUILD_TYPE=Release ./wasm/build.sh
node wasm/build/dqiv_wasm_spike.js
node wasm/build/dqiv_layout_probe.js
node wasm/build/dqiv_wasm_game.js      # stops at the GL context: node has no WebGL
```

To see a picture, serve the build directory over http — the `.wasm` is fetched, not
embedded, so `file://` will not work:

```bash
cp wasm/index.html wasm/build/ && cd wasm/build && python3 -m http.server
```

Then open `http://localhost:8000/index.html`. This is the step that needs a person:
there is no browser automation in this project. The self-check prints two lines
(byte order, clip) to the console and mirrors them onto the bottom of the page.

Verified with Emscripten 4.0.13 on macOS (Apple Silicon): 0 errors. Debug `.wasm`:
spike 8.6 MB, game 23.4 MB, probe 0.4 MB. `.github/workflows/ci.yml` runs this build
on `ubuntu-latest` and asserts the spike prints `= 3`, the four layout assertions, the
byte-order round trip, all 8 shaders against both GLSL profiles (`check-shaders.py`),
and that the game gets through the whole decompiled startup and stops at the GL
boundary.

## What this builds

| Target | Contents |
| --- | --- |
| `dqiv_core` | the portable PC core: DS video/GPU emulation, file system, settings, sound mixing |
| `dqiv_game` | the decompiled game itself, 553 translation units |
| `dqiv_nitro_system` | the NitroSystem (NNS) library |
| `dqiv_sound` | the NitroSDK sound driver |
| `dqiv_platform` | DS power/wireless/overlay/file APIs |
| `dqiv_portable` | portable stand-ins for the three Windows-only platform files |
| `dqiv_wasm_spike` | links everything into one module and runs a real decompiled function |
| `dqiv_layout_probe` | compile-time guard for the 32-bit layout assumptions |
| `dqiv_wasm_game` | the real game: the platform layer compiled for wasm32 for the first time, plus `src/gpu-selfcheck.cpp` |

## Why wasm32 works without touching the game

The original DS resources contain **32-bit pointers**, and a pointer field in a
guest structure is 4 bytes, so the whole game assumes a 32-bit pointer value
space. wasm32's pointers are 32-bit offsets into a 4 GB linear memory, which
reproduces exactly that property — including the "an integer may hold either a
pointer or a message ID" idiom in `GameServices::isGameTextPointer()`.

That is why `sizeof(void*) == 4`, `sizeof(DataObject) == 0x10` and
`sizeof(UnkGameApplication) == 0xb4` hold here unmodified, and why a *native*
64-bit target (macOS arm64, iOS) does not get this for free.

## What is deliberately missing

* **No title screen.** The renderer is compiled, linked and running on a real GLES
  3.0 context, and `src/gpu-selfcheck.cpp` puts known answers through the real
  shaders. What is missing is the thing the game needs first:
  `GameBoot::openResources()` throws without `data/SOUND/sound_data.sdat`, so it
  stops before it can draw a picture. That is M5.
* **Three honest minimum implementations, not implementations.** `TextCanvas` has no
  rasterizer on wasm, so the FPS overlay keeps its rectangle and loses its text;
  `settings.autoQuality` is a no-op because the GPU-timing path that drives it is
  compiled out; `divideLikeTheDs` (DS software-division semantics — `x/0` gives `x`,
  `x%0` gives `0`) is desktop-only, because wasm traps on integer division by zero
  and the instance is already gone by the time it could be handled.
* **13 linker signature mismatches, 7 of them already in the spike.** All decompiler
  signature guesses in the decompiled C sources — MSVC has the same ones silently,
  wasm only says so out loud. wasm-ld inserts thunks so it links, but a mismatched
  return type or arity is a real correctness risk and needs looking at one by one.
* **No overlay section bracketing.** Upstream generates it with MSVC-only
  `#pragma init_seg/data_seg/bss_seg` (see `cmake/NativeOverlays.cmake`). The game
  compiles and links without it, but per-overlay state snapshot/restore is not
  wired up yet.
* **Asset extraction is not wired up.** `native/PackMain.cpp` is portable, so the
  plan is to run it in the browser on the user's own ROM — no game data is ever
  shipped.

The source lists are **not** duplicated any more: `../cmake/Sources.cmake` is the
single place where every target registers its files, and both the root
`CMakeLists.txt` and this one reference it. What stays here is what genuinely
differs — the compiler flags, the SDL wiring, and the portable stand-in for the
three Windows-only platform files.
