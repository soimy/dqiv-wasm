#!/usr/bin/env bash
# Builds the wasm32 port scaffold with Emscripten.
#
# Prerequisite: an activated emsdk, e.g.
#     git clone https://github.com/emscripten-core/emsdk && cd emsdk && ./emsdk install latest && ./emsdk activate latest
#     source ./emsdk_env.sh
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "error: emcmake not found. Activate emsdk first:" >&2
    echo "    source /path/to/emsdk/emsdk_env.sh" >&2
    exit 1
fi

BUILD_TYPE="${BUILD_TYPE:-Debug}"
JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )"

emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build build -j"$JOBS"

echo
echo "built:  wasm/build/dqiv_wasm_spike.wasm"
echo "run:    node wasm/build/dqiv_wasm_spike.js"
echo "probe:  node wasm/build/dqiv_layout_probe.js"
echo
echo "game:   wasm/build/dqiv_wasm_game.wasm + .js, opened through wasm/index.html"
echo "        serve the build directory over http (the .wasm is fetched, not embedded):"
echo "        cd wasm/build && python3 -m http.server"
echo "        node cannot run it - there is no WebGL in node, so it stops at the"
echo "        GL context, which is the point a browser takes over."
