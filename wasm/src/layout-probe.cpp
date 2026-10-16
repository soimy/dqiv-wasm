// Compile-time guard for the layout assumptions the original DS resources depend
// on. The upstream Win32 build asserts the same sizes in native/GameBoot.cpp;
// this probe makes them checkable on any 32-bit-pointer target without pulling in
// the Windows entry point.
//
// It passes as-is on wasm32 (Emscripten) — no source changes required. That is
// the core result of the porting spike: see docs/porting/01-SPIKE-REPORT.md.
#include "main/data/DataObject.hpp"
#include "main/global/UnkGameApplication.hpp"
#include <cstddef>
#include <cstdio>
extern "C" {
#include <nitro/os/thread.h>
}
#include <algorithm>
#include <span>
#include <vector>
#include "PixelOrder.hpp"

static_assert(sizeof(void*) == 4, "the original game resources contain 32-bit pointers");
static_assert(sizeof(DataObject) == 0x10, "DataObject layout drifted");
static_assert(sizeof(UnkGameApplication) == 0xb4, "UnkGameApplication layout drifted");
static_assert(sizeof(OSThread) == 0xc0, "OSThread layout drifted");

int main() {
    std::printf("sizeof(void*)        = %zu\n", sizeof(void*));
    std::printf("sizeof(DataObject)   = 0x%zx\n", sizeof(DataObject));
    std::printf("sizeof(UnkGameApplication) = 0x%zx\n", sizeof(UnkGameApplication));
    std::printf("sizeof(OSThread)     = 0x%zx\n", sizeof(OSThread));
    // Byte order: a pixel in the game's convention goes to GL in the order this
    // backend accepts and must come back as the value it started as. On wasm the
    // bytes are swizzled first; on desktop they are already in that order. Same
    // invariant on both, which is exactly what the abstraction is for.
    const std::uint32_t sample=0x7f3a5c6e;   // A=0x7f R=0x3a G=0x5c B=0x6e
    std::vector<std::uint32_t> scratch;
    const std::uint32_t sent=*pc::uploadOrder(std::span<const std::uint32_t>(&sample,1),scratch);
    const std::uint32_t back=pc::fromReadback(sent);
    std::printf("byte order         = 0x%08x -> 0x%08x -> 0x%08x  %s\n",sample,sent,back,back==sample?"ok":"MISMATCH");
    if(back!=sample)return 1;
    return 0;
}
