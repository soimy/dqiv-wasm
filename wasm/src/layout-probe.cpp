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

static_assert(sizeof(void*) == 4, "the original game resources contain 32-bit pointers");
static_assert(sizeof(DataObject) == 0x10, "DataObject layout drifted");
static_assert(sizeof(UnkGameApplication) == 0xb4, "UnkGameApplication layout drifted");
static_assert(sizeof(OSThread) == 0xc0, "OSThread layout drifted");

int main() {
    std::printf("sizeof(void*)        = %zu\n", sizeof(void*));
    std::printf("sizeof(DataObject)   = 0x%zx\n", sizeof(DataObject));
    std::printf("sizeof(UnkGameApplication) = 0x%zx\n", sizeof(UnkGameApplication));
    std::printf("sizeof(OSThread)     = 0x%zx\n", sizeof(OSThread));
    return 0;
}
