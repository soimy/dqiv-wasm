// main() for the wasm32 build. It exists to prove two things and nothing else:
//   1. the whole decompiled game + NitroSystem + sound + platform layer links
//      into a single wasm module with no undefined symbols;
//   2. the 32-bit layout assumptions of the original DS resources still hold.
// There is no renderer yet, so this does not start the game — see
// docs/porting/03-ROADMAP.md for what comes next.
#include "main/dss/DssCore.hpp"
#include "main/dss/DssUtils.hpp"
#include "GameServices.hpp"
#include <cstddef>
#include <cstdio>

int main() {
    int weights[4] = {1, 1, 1, 1};
    std::printf("dqiv-wasm: sizeof(void*)=%zu\n", sizeof(void*));
    std::printf("dqiv-wasm: decompiled dss::arrayToIndex([1,1,1,1], value=3, max=4) = %d (expect 3)\n",
                dss::arrayToIndex(weights, 3, 4));
    std::printf("dqiv-wasm: live game heap allocations = %zu\n", pc::liveGameAllocations());
    std::printf("dqiv-wasm: tick counter (unkfunc_0207e7e8) = %d\n", unkfunc_0207e7e8());
    return 0;
}
