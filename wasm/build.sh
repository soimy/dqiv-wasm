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
