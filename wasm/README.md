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
```

Verified with Emscripten 4.0.13 on macOS (Apple Silicon): 677 translation units,
0 errors, 8.5 MB `.wasm` (Debug). Rebuilt from scratch on 2026-10-16 with the same
result; `.github/workflows/ci.yml` runs this build on `ubuntu-latest` and asserts the
spike prints `= 3` plus the four layout assertions below.

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

* **No renderer.** `GameWindow.cpp`, `GpuRenderer.cpp`, `GlLoader.cpp`,
  `TextCanvas.cpp`, `FramePresenter.cpp`, `NitroSymbols.cpp`, `DssRuntime.cpp`
  and `GameBoot.cpp` still assume a desktop OpenGL 3.3 window. Replacing them with
  a WebGL2/GLES3 backend is the next milestone; the gap list is in
  [`docs/porting/02-FINDINGS.md`](../docs/porting/02-FINDINGS.md).
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
